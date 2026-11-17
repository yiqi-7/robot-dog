#pragma once
// ===========================================================================
// mqtt_bridge_node.hpp —— MQTT 桥接节点类声明
//
// 按功能维度拆分实现（每个 .cpp 一个关注点，扩展时只动相关文件）：
//   node_core.cpp       构造/析构：参数、话题、定时器、Nav2 动作客户端装配
//   node_mqtt.cpp       MQTT 传输：连接/回调/遗嘱/site map
//   node_telemetry.cpp  遥测与告警：1Hz 状态上行、电量温度告警、事件冷却
//   node_tasks.cpp      任务执行：assign/取消/Nav2/状态回写/初始位姿
//   node_voice.cpp      语音：§3.1 音频上行 + §3.2 piper 播报
//   main.cpp            入口
// ===========================================================================
// ============================================================================
// mqtt_bridge.cpp —— 机器狗 ↔ 云端看板 MQTT 桥接节点（C++）
//
// 本节点承担三件事（对应课程第三章 3.2~3.4）：
//   ① 遥测上行：ROS 数据 → MQTT state 主题
//   ② 指令下行：MQTT cmd 主题 → 机器人动作（急停 / 复位 / 前往目标点）
//   ③ 站点地图：向看板发布 {P}/site/map，让看板厂区图与仿真世界一致
//
// ---------------------------------------------------------------------------
// 坐标系说明（重要）
// ---------------------------------------------------------------------------
// 仿真世界是 20×14 m 的小厂区（墙在 x=±10, y=±7），而看板厂区图按 600×380 m
// 的米制坐标渲染（1 px = 0.5 m）。若直接上报仿真坐标，机器人在看板上只是角落里
// 一个点。因此本节点做一次【线性缩放 + 平移】，把仿真世界铺满看板地图：
//
//     dash_x = (sim_x + 10) * S + OFFSET_X        S = 20 (1 仿真米 = 20 看板米)
//     dash_y = (sim_y +  7) * S + OFFSET_Y        仿真世界 → 看板 x∈[100,500], y∈[50,330]
//
// 看板下发目标点时按同一公式【反算】回仿真坐标，再导航过去。
// 缩放系数与偏移都是参数，改参数即可适配别的世界尺寸。
//
// ---------------------------------------------------------------------------
// 数据流
// ---------------------------------------------------------------------------
//   上行  /odom + /joint_states → {P}/robots/{id}/state   (1 Hz, QoS1, retained)
//   下发  {P}/robots/{id}/cmd   → estop / resume / assign / cancel
//   地图  {P}/site/map          → 一次性 retained 发布（连接成功后）
//   任务  {P}/fleet/tasks       → 订阅看板的全量任务快照；任务结束【回写】状态
//   告警  {P}/event             → 电量 / 温度 / 急停 / 导航失败等事件上报
//
// ---------------------------------------------------------------------------
// 心跳与掉线（3.3）
// ---------------------------------------------------------------------------
//   连接时注册遗嘱消息（LWT）：非正常断开（断电/断网/被 kill -9）时，
//   Broker 会代本节点发布 status=offline，看板秒级感知；正常 disconnect 不触发。
//
// ---------------------------------------------------------------------------
// 线程模型
// ---------------------------------------------------------------------------
//   libmosquitto 自己的网络线程负责收发，ROS executor 线程负责定时与订阅。
//   两者共享状态必须用 data_mutex_ 保护。
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <mosquitto.h>
#include <nlohmann/json.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <thread>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

using json = nlohmann::json;
using namespace std::chrono_literals;

// Nav2 的导航动作：派单统一走它，由 Nav2 负责全局/局部规划与避障。
// 【历史教训】早期版本自己算差角直接发 cmd_vel（纯"朝目标直走"），
// 没有规划与避障，机器人在配电室墙角一头顶住墙、关节速度打满限幅值，
// 表现为"卡住不动"。改用 Nav2 后由代价地图绕开障碍。
using NavigateToPose = nav2_msgs::action::NavigateToPose;
using NavGoalHandle = rclcpp_action::ClientGoalHandle<NavigateToPose>;

namespace {

// 角度归一化到 [-pi, pi]，避免偏航累计超过一圈后跳变
double wrapPi(double a)
{
  while (a > M_PI) a -= 2.0 * M_PI;
  while (a < -M_PI) a += 2.0 * M_PI;
  return a;
}

// 四元数 → 偏航角
double yawFromQuat(double x, double y, double z, double w)
{
  return std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
}

// Base64 编码（VOICE-INTERFACE.md §3.1 的 data 字段）。
// 只有这一处用途，手写省一个依赖；每 3 字节 → 4 字符，末尾补 '='。
std::string base64Encode(const uint8_t *data, size_t len)
{
  static const char kTab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t v = (b0 << 16) | (b1 << 8) | b2;
    out.push_back(kTab[(v >> 18) & 0x3F]);
    out.push_back(kTab[(v >> 12) & 0x3F]);
    out.push_back((i + 1 < len) ? kTab[(v >> 6) & 0x3F] : '=');
    out.push_back((i + 2 < len) ? kTab[v & 0x3F] : '=');
  }
  return out;
}

// 当前墙上时钟的毫秒时间戳（Unix epoch ms）。
// 【不能】用 rclcpp 的 now()：本节点受 use_sim_time 控制，Gazebo 仿真时钟的
// nanoseconds() 不是 Unix epoch，直接当毫秒用会得到 217835 这种荒唐值，
// 看板按 updatedAt/ts 排序、显示"多久之前"就全乱了。
// 所有 MQTT 报文里的时间戳统一用系统时钟，与看板 Date.now() 语义一致。
long long wallClockMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// 站点定义（坐标一律用【仿真世界米制】，发布时统一做缩放变换）
// 名称与看板内置地图保持一致，这样语音规则解析、任务下拉框都能直接匹配。
// ---------------------------------------------------------------------------
struct SiteZone {
  const char *id;
  const char *name;
  double x, y, w, h;   // 仿真世界中的矩形（左上角 + 宽高）
};

struct SitePoint {
  const char *id;
  const char *name;
  double x, y;              // 仿真世界坐标
  const char *zone;         // 所属区域 id
  const char *kind;         // "waypoint" / "charger" / nullptr
};

// 区域：与 worlds/factory.sdf 里的六个物理区域逐一对应（坐标 = 仿真世界米制）
//   车间A  西-南   x[-9.6,-1.3] y[-6.9,-0.6]
//   车间B  西-北   x[-9.6,-1.3] y[ 0.6, 6.9]
//   仓库   东-南   x[ 1.3, 9.6] y[-6.9,-0.6]
//   充电区 东北偏西 x[ 1.3, 4.6] y[ 0.6, 6.9]
//   配电室 东北下   x[ 4.6, 9.6] y[ 0.6, 3.4]
//   中控室 东北上   x[ 4.6, 9.6] y[ 3.6, 6.9]
const std::vector<SiteZone> kZones = {
  {"workshop-a", "车间A",  -9.6, -6.9, 8.3, 6.3},
  {"workshop-b", "车间B",  -9.6,  0.6, 8.3, 6.3},
  {"warehouse",  "仓库",    1.3, -6.9, 8.3, 6.3},
  {"charging",   "充电区",  1.3,  0.6, 3.3, 6.3},
  {"power",      "配电室",  4.6,  0.6, 5.0, 2.8},
  {"control",    "中控室",  4.6,  3.6, 5.0, 3.3},
};

// 点位：全部选在可通行地面（走廊或区域内空地），避开货架/变压器/操作台/充电桩
const std::vector<SitePoint> kPoints = {
  {"warehouse-racks", "仓库-货架区",   5.0, -3.5, "warehouse",  nullptr},
  {"warehouse-dock",  "仓库-月台",     8.5, -1.5, "warehouse",  nullptr},
  {"a-line-1",        "车间A-1号产线", -7.0, -3.7, "workshop-a", nullptr},
  {"a-line-2",        "车间A-2号产线", -3.5, -3.7, "workshop-a", nullptr},
  {"a-west-gate",     "车间A-西门",    -8.47, -1.16, "workshop-a", nullptr},
  {"b-assembly",      "车间B-装配台",  -7.5,  3.8, "workshop-b", nullptr},
  {"b-east-gate",     "车间B-东门",    -2.5,  3.0, "workshop-b", nullptr},
  {"power-room",      "配电室",         7.1,  2.6, "power",      nullptr},
  {"boiler",          "锅炉房",         5.17, 5.35, "control",    nullptr},
  {"charger-1",       "充电区-1号桩",   1.9,  4.9, "charging",   "charger"},
  {"charger-2",       "充电区-2号桩",   3.2,  4.9, "charging",   "charger"},
  {"control-room",    "中控室",         7.44, 5.87, "control",    nullptr},
  {"north-gate",      "厂区北门",       0.0,  6.4, nullptr,      nullptr},
  {"ring-road",       "厂区环路",       0.0, -6.3, nullptr,      nullptr},
};

}  // namespace

class MqttBridge : public rclcpp::Node
{
public:

  MqttBridge();
  ~MqttBridge() override;

  // ---------------- 坐标换算（看板 ↔ 仿真世界，公式见文件头） ----------------
  // =========================================================================
  // 坐标变换：仿真世界米制 ⇄ 看板地图米制
  // =========================================================================
  double simToDashX(double sim_x) const
  {
    return (sim_x + site_world_w_ * 0.5) * site_scale_ + site_offset_x_;
  }

  // 注意 y 要翻转：仿真世界 y 向上增长（北为正），看板地图 y 向下增长（屏幕坐标）。
  // 所以 仿真北(+y) → 看板靠上(小 y)，这样看板厂区图与 Gazebo 看到的方向一致。
  double simToDashY(double sim_y) const
  {
    return (site_world_h_ * 0.5 - sim_y) * site_scale_ + site_offset_y_;
  }

  // 反算：看板目标点 → 仿真坐标（导航用）
  double dashToSimX(double dash_x) const
  {
    return (dash_x - site_offset_x_) / site_scale_ - site_world_w_ * 0.5;
  }

  double dashToSimY(double dash_y) const
  {
    return site_world_h_ * 0.5 - (dash_y - site_offset_y_) / site_scale_;
  }

  // ---------------- MQTT 传输：连接 / 回调 / 遗嘱消息 / site map ----------------
  void initMqtt();
  static void onConnectCb(struct mosquitto * /*mosq*/, void *obj, int rc);
  static void onDisconnectCb(struct mosquitto * /*mosq*/, void *obj, int rc);
  void publishSiteMap();
  static void onMessageCb(struct mosquitto * /*mosq*/, void *obj,
                          const struct mosquitto_message *msg);

  // ---------------- 遥测与告警：1Hz 状态上行 / 电量温度告警 / 事件冷却 ----------------
  void publishEvent(const char * level, const std::string &message);
  bool eventAllowed(const std::string &key, double cooldown_s);
  void checkAlarms();
  void onPublishTimer();

  // ---------------- 任务执行：assign / Nav2 动作 / 状态回写 / 初始位姿 / 巡逻暂停 ----------------
  void startNavigation(const std::string &target_id, const std::string &title, int task_id,
                       const json &task_obj = json());
  void sendNavGoal(double x, double y, const std::string & name);
  enum class Outcome { Success, Canceled, Failed };

  // 结束当前任务：清状态、停车、恢复巡逻、回写任务状态
  void finishTask(const std::string & detail, Outcome outcome = Outcome::Canceled);
  void cancelNavigation(const char * reason);
  void writeBackTask(int task_id, const char *new_status);
  void onFleetTasks(const std::string &text);
  void onNavTimer();
  void publishInitialPose();
  void pausePatrol(bool paused);

  // ---------------- 语音：§3.1 音频上行 / §3.2 piper 播报队列 ----------------
  void onAudioPcm(const std::vector<uint8_t> &pcm);
  void onTts(const std::string &text);
  void enqueueTts(const std::string & text);
  void ttsWorker();

  // ---------------- 成员变量 ----------------

  // ---------------- 成员变量 ----------------
  std::string broker_host_, robot_id_, robot_name_, robot_model_;
  std::string topic_prefix_, username_, password_;
  std::string state_topic_, cmd_topic_, map_topic_;
  std::string fleet_tasks_topic_, event_topic_;     // 3.5 任务回写 / 告警上行
  int broker_port_ = 1883, keepalive_ = 45;
  double publish_hz_ = 1.0, battery_ = 100.0;

  // 3.5 告警参数与 alarmId 分配器
  long long alarm_base_id_ = 1000000, alarm_seq_ = 0;
  double battery_warn_ = 20.0, battery_crit_ = 10.0;
  double temp_warn_ = 75.0, temp_crit_ = 85.0;
  bool battery_warn_reported_ = false, battery_crit_reported_ = false;

  // 3.5 任务列表：看板发来的全量快照 + assign 缓存的完整任务对象
  json fleet_tasks_;                    // 最新一份全量数组
  json cached_task_;                    // assign 报文里的 task 对象
  std::map<int, std::pair<std::string, long long>> task_terminal_;  // id → (终态, 时间)
  int active_task_id_ = 0;              // 正在执行的任务 id（回写用）
  std::map<std::string, rclcpp::Time> event_last_;  // 告警冷却表

  double site_scale_ = 20.0, site_offset_x_ = 100.0, site_offset_y_ = 50.0;
  double site_world_w_ = 20.0, site_world_h_ = 14.0;
  bool publish_map_ = true;

  double nav_speed_ = 0.25, nav_tolerance_ = 0.30, nav_timeout_s_ = 120.0;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_patrol_;
  std::string patrol_status_;

  struct mosquitto *mosq_ = nullptr;

  // 语音（§3.1 上行 / §3.2 下行）
  bool voice_enabled_ = true;
  std::string audio_ros_topic_, audio_topic_, tts_topic_;
  int audio_sample_rate_ = 16000, audio_channels_ = 1;
  rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr sub_audio_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_audio_info_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_tts_text_;
  std::string piper_bin_, piper_model_;
  std::thread tts_thread_;
  std::mutex tts_mutex_;
  std::condition_variable tts_cv_;
  std::deque<std::string> tts_queue_;

  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr sub_odom_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr sub_joint_;
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr pub_cmd_vel_;
  rclcpp::Publisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr pub_initial_pose_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr pub_patrol_enable_;
  rclcpp_action::Client<NavigateToPose>::SharedPtr nav_client_;
  rclcpp::TimerBase::SharedPtr timer_state_, timer_nav_;
  rclcpp::Time start_time_, nav_start_time_;

  // 共享状态（受 data_mutex_ 保护）
  std::mutex data_mutex_;
  double pos_x_ = 0.0, pos_y_ = 0.0, heading_ = 0.0, speed_ = 0.0;
  double joint_load_ = 0.0, motor_temp_ = 38.0;
  int signal_ = -55;
  std::string status_ = "idle", task_;
  bool mqtt_connected_ = false, odom_ok_ = false;

  // 导航状态
  bool navigating_ = false;
  double nav_target_x_ = 0.0, nav_target_y_ = 0.0;
  std::string nav_target_name_;
  std::shared_ptr<NavGoalHandle> nav_goal_;   // 当前 Nav2 目标句柄（用于取消）
  bool initial_pose_sent_ = false;            // 初始位姿是否已对齐

  // 定时器用的快照（避免持锁跨线程调用日志宏）
  bool connected_snapshot_ = false, odom_snapshot_ = false;
};

