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
  MqttBridge() : rclcpp::Node("mqtt_bridge")
  {
    // ---------------- 参数 ----------------
    // 默认走本机：看板 server.js 与 mosquitto broker 都跑在同一台机器上，
    // 两者必须连同一个 broker 才能互通。这里若写成 192.168.137.1（主机 ICS
    // 网卡地址）而看板连 127.0.0.1，就会出现"看板派单了但机器人收不到"的
    // 假故障，排查成本极高，故默认值统一为 127.0.0.1。
    broker_host_   = declare_parameter("broker_host", "127.0.0.1");
    broker_port_   = declare_parameter("broker_port", 1883);
    robot_id_      = declare_parameter("robot_id", "robot-01");
    robot_name_    = declare_parameter("robot_name", "仿真狗");
    robot_model_   = declare_parameter("robot_model", "quadruped-sim");
    topic_prefix_  = declare_parameter("topic_prefix", "qbot/v1");
    publish_hz_    = declare_parameter("publish_hz", 1.0);
    keepalive_     = declare_parameter("keepalive", 45);
    username_      = declare_parameter("username", "");
    password_      = declare_parameter("password", "");
    battery_       = declare_parameter("battery_init", 100.0);

    // 坐标缩放：仿真世界 → 看板地图
    site_scale_    = declare_parameter("site_scale", 20.0);
    site_offset_x_ = declare_parameter("site_offset_x", 100.0);
    site_offset_y_ = declare_parameter("site_offset_y", 50.0);
    site_world_w_  = declare_parameter("site_world_width", 20.0);    // 仿真世界宽 m
    site_world_h_  = declare_parameter("site_world_height", 14.0);   // 仿真世界高 m
    publish_map_   = declare_parameter("publish_site_map", true);
    // 语音链路（VOICE-INTERFACE.md §3.1/§3.2）。麦克风节点 → ROS → 这里上行，
    // 不让采集节点单独连 MQTT：机器人端只维持一条连接，遗嘱/重连/背压都在这管。
    voice_enabled_     = declare_parameter("voice_enabled", true);
    audio_ros_topic_   = declare_parameter("audio_topic", "voice/audio_pcm");
    audio_sample_rate_ = declare_parameter("audio_sample_rate", 16000);
    audio_channels_    = declare_parameter("audio_channels", 1);
    // TTS 本地播报：piper（模型 course 提供 zh_CN-huayan-medium）
    piper_bin_   = declare_parameter("piper_bin", "/home/wby/venvs/piper/bin/piper");
    piper_model_ = declare_parameter("piper_model",
      "/run/media/wby/shuju/Document/robot-dog/quadruped-ops/models/piper/zh_CN-huayan-medium.onnx");

    // 前往目标点的行驶参数
    nav_speed_     = declare_parameter("nav_speed", 0.25);   // m/s
    nav_tolerance_ = declare_parameter("nav_tolerance", 0.30);// m，到达判定
    nav_timeout_s_ = declare_parameter("nav_timeout", 120.0); // s

    // ---- 3.5 告警阈值与 alarmId 分配 ----
    // alarmId 约定"上报方分配、全局递增"。看板 mock 模式的种子告警用 1000 附近
    // 的小号，这里从 100 万起，避免两边撞号。
    alarm_base_id_ = declare_parameter("alarm_base_id", 1000000);
    battery_warn_  = declare_parameter("battery_warn", 20.0);      // % 低于即预警
    battery_crit_  = declare_parameter("battery_crit", 10.0);      // % 低于即严重
    temp_warn_     = declare_parameter("motor_temp_warn", 75.0);   // °C
    temp_crit_     = declare_parameter("motor_temp_crit", 85.0);   // °C

    state_topic_ = topic_prefix_ + "/robots/" + robot_id_ + "/state";
    cmd_topic_   = topic_prefix_ + "/robots/" + robot_id_ + "/cmd";
    audio_topic_ = topic_prefix_ + "/robots/" + robot_id_ + "/audio";
    tts_topic_   = topic_prefix_ + "/robots/" + robot_id_ + "/tts";
    map_topic_   = topic_prefix_ + "/site/map";
    fleet_tasks_topic_ = topic_prefix_ + "/fleet/tasks";
    event_topic_       = topic_prefix_ + "/event";

    start_time_ = now();

    // ---------------- ROS 订阅 ----------------
    sub_odom_ = create_subscription<nav_msgs::msg::Odometry>(
      "odom", rclcpp::SensorDataQoS(),
      [this](nav_msgs::msg::Odometry::SharedPtr msg) {
        std::lock_guard<std::mutex> lk(data_mutex_);
        pos_x_ = msg->pose.pose.position.x;
        pos_y_ = msg->pose.pose.position.y;
        heading_ = yawFromQuat(
          msg->pose.pose.orientation.x, msg->pose.pose.orientation.y,
          msg->pose.pose.orientation.z, msg->pose.pose.orientation.w);
        speed_ = std::hypot(msg->twist.twist.linear.x, msg->twist.twist.linear.y);
        odom_ok_ = true;
      });

    sub_joint_ = create_subscription<sensor_msgs::msg::JointState>(
      "joint_states", rclcpp::SensorDataQoS(),
      [this](sensor_msgs::msg::JointState::SharedPtr msg) {
        std::lock_guard<std::mutex> lk(data_mutex_);
        // 关节负载归一化到 0~1，供温度模型使用。
        // 【实测】CHAMP 步态正常行走时，12 个关节里有 4 个髋/膝轴会跑到
        // ±12 rad/s 的限幅值，平均绝对角速度约 5~6 rad/s —— 这是步态控制的
        // 正常表现，不是卡死。所以不能拿原始值直接乘以系数（会算出 130°C+），
        // 这里除以标称值 6.0 归一化，再截断到 1.0。
        double sum = 0.0;
        size_t n = 0;
        for (double v : msg->velocity) { sum += std::abs(v); ++n; }
        const double avg = n ? sum / static_cast<double>(n) : 0.0;
        joint_load_ = std::min(avg / 6.0, 1.0);
      });

    // 巡逻节点进度：/patrol/status 的 JSON 直接透传到看板 currentTask，
    // 让前端能看到「巡逻到第几个航点/重试中/已跳过」
    sub_patrol_ = create_subscription<std_msgs::msg::String>(
      "patrol/status", rclcpp::SensorDataQoS(),
      [this](std_msgs::msg::String::SharedPtr msg) {
        std::lock_guard<std::mutex> lk(data_mutex_);
        patrol_status_ = msg->data;
        task_ = "巡逻: " + patrol_status_;
      });

    // ---------------- 语音上行（§3.1） ----------------
    // voice_source 把"一句话"发成 16 kHz 单声道 PCM 片段，这里转成 §3.1 的
    // JSON（base64）上行。格式信息走 latch 话题，采样率/声道以采集端为准，
    // 避免桥这边参数写死和采集端不一致（曾经 16k/8k 错位导致 ASR 全是噪声）。
    pub_tts_text_ = create_publisher<std_msgs::msg::String>("voice/tts_text", 10);
    if (voice_enabled_) {
      sub_audio_ = create_subscription<std_msgs::msg::UInt8MultiArray>(
        audio_ros_topic_, rclcpp::QoS(10),
        [this](std_msgs::msg::UInt8MultiArray::SharedPtr msg) { onAudioPcm(msg->data); });

      sub_audio_info_ = create_subscription<std_msgs::msg::String>(
        "voice/audio_info", rclcpp::QoS(rclcpp::KeepLast(1)).transient_local().reliable(),
        [this](std_msgs::msg::String::SharedPtr msg) {
          try {
            const json j = json::parse(msg->data);
            audio_sample_rate_ = j.value("sampleRate", audio_sample_rate_);
            audio_channels_    = j.value("channels", audio_channels_);
            RCLCPP_INFO(get_logger(), "语音格式：%d Hz / %d 声道",
              audio_sample_rate_, audio_channels_);
          } catch (const std::exception &) { /* 格式信息损坏就沿用默认值 */ }
        });
      RCLCPP_INFO(get_logger(), "语音上行已启用：ROS %s → MQTT %s",
        audio_ros_topic_.c_str(), audio_topic_.c_str());
      tts_thread_ = std::thread([this] { ttsWorker(); });
    }

    pub_cmd_vel_ = create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);

    // 派单导航走 Nav2 动作接口（规划 + 避障 + 可取消）
    nav_client_ = rclcpp_action::create_client<NavigateToPose>(this, "navigate_to_pose");

    // 初始位姿修正：AMCL 的 initial_pose 参数是固定值，但机器人每次停下位置
    // 都不同（上一轮任务结束在哪、世界是否复用），固定值会让粒子滤波收敛到
    // 错误位置（实测偏 2 m，代价图与真实环境错位 → 规划贴墙顶死）。
    // 因此启动后拿第一条 /odom 的真实位姿，主动发一次 /initialpose 对齐。
    pub_initial_pose_ = create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
      "initialpose", 10);

    // 派单与巡逻互斥：派单时暂停巡逻，任务结束后恢复
    pub_patrol_enable_ = create_publisher<std_msgs::msg::Bool>("/patrol/enable", 10);

    // ---------------- MQTT ----------------
    initMqtt();

    // ---------------- 定时器 ----------------
    // ① 1 Hz 状态上报
    const auto state_period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / std::max(0.1, publish_hz_)));
    timer_state_ = create_wall_timer(state_period, [this]() { onPublishTimer(); });

    // ② 20 Hz 前往目标点的行驶控制（仅在有目标时工作）
    timer_nav_ = create_wall_timer(50ms, [this]() { onNavTimer(); });

    RCLCPP_INFO(get_logger(),
      "MQTT 桥已启动\n"
      "  Broker     : %s:%d\n"
      "  机器人     : %s (%s)\n"
      "  状态主题   : %s @ %.1f Hz\n"
      "  指令主题   : %s\n"
      "  任务主题   : %s（订阅 + 回写）\n"
      "  告警主题   : %s\n"
      "  地图主题   : %s（缩放 %.1f×，世界 %.0f×%.0f m）\n"
       "  语音上行   : %s → %s\n"
       "  遗嘱消息   : 已注册",
      broker_host_.c_str(), broker_port_,
      robot_id_.c_str(), robot_name_.c_str(),
      state_topic_.c_str(), publish_hz_, cmd_topic_.c_str(),
      fleet_tasks_topic_.c_str(), event_topic_.c_str(),
      map_topic_.c_str(), site_scale_, site_world_w_, site_world_h_,
      voice_enabled_ ? audio_ros_topic_.c_str() : "未启用", audio_topic_.c_str());
  }

  ~MqttBridge() override
  {
    if (tts_thread_.joinable()) {
      tts_cv_.notify_all();
      tts_thread_.join();
    }
    if (mosq_) {
      mosquitto_disconnect(mosq_);        // 正常断开 → 不触发遗嘱
      mosquitto_loop_stop(mosq_, false);
      mosquitto_destroy(mosq_);
      mosq_ = nullptr;
    }
    mosquitto_lib_cleanup();
  }

private:
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

  // =========================================================================
  // MQTT 初始化与回调
  // =========================================================================
  void initMqtt()
  {
    mosquitto_lib_init();
    mosq_ = mosquitto_new(robot_id_.c_str(), true, this);
    if (!mosq_) {
      RCLCPP_ERROR(get_logger(), "mosquitto_new 失败");
      return;
    }

    // 遗嘱消息：非正常断开时由 Broker 代发（3.3 心跳机制核心）
    json will = {
      {"robotId", robot_id_}, {"name", robot_name_}, {"model", robot_model_},
      {"status", "offline"}, {"ts", 0},
    };
    const std::string will_str = will.dump();
    mosquitto_will_set(mosq_, state_topic_.c_str(),
      static_cast<int>(will_str.size()), will_str.c_str(), 1, true);

    if (!username_.empty())
      mosquitto_username_pw_set(mosq_, username_.c_str(), password_.c_str());

    mosquitto_connect_callback_set(mosq_, &MqttBridge::onConnectCb);
    mosquitto_message_callback_set(mosq_, &MqttBridge::onMessageCb);
    mosquitto_disconnect_callback_set(mosq_, &MqttBridge::onDisconnectCb);

    int rc = mosquitto_connect_async(mosq_, broker_host_.c_str(), broker_port_, keepalive_);
    if (rc != MOSQ_ERR_SUCCESS) {
      RCLCPP_ERROR(get_logger(), "connect_async 失败: %s", mosquitto_strerror(rc));
      return;
    }
    mosquitto_loop_start(mosq_);   // 独立网络线程，内部自动重连
  }

  static void onConnectCb(struct mosquitto * /*mosq*/, void *obj, int rc)
  {
    auto *self = static_cast<MqttBridge *>(obj);
    if (rc != 0) {
      RCLCPP_WARN(self->get_logger(), "MQTT 连接被拒绝: %s", mosquitto_connack_string(rc));
      return;
    }
    {
      std::lock_guard<std::mutex> lk(self->data_mutex_);
      self->mqtt_connected_ = true;
    }
    RCLCPP_INFO(self->get_logger(), "MQTT 已连接 %s:%d",
      self->broker_host_.c_str(), self->broker_port_);

    int rc_cmd = mosquitto_subscribe(self->mosq_, nullptr, self->cmd_topic_.c_str(), 1);
    if (rc_cmd != MOSQ_ERR_SUCCESS) {
      RCLCPP_ERROR(self->get_logger(), "订阅 cmd 失败: %s", mosquitto_strerror(rc_cmd));
    }

    // 订阅看板的任务列表快照（retained）。任务结束后要在这份全量数组上改
    // status 再回写，所以必须先拿到看板的版本，否则回写会丢字段。
    int rc_task = mosquitto_subscribe(self->mosq_, nullptr, self->fleet_tasks_topic_.c_str(), 1);
    if (rc_task != MOSQ_ERR_SUCCESS) {
      RCLCPP_ERROR(self->get_logger(), "订阅 fleet/tasks 失败: %s", mosquitto_strerror(rc_task));
    } else {
      RCLCPP_INFO(self->get_logger(), "已订阅 %s（等待 retained 快照）",
        self->fleet_tasks_topic_.c_str());
    }

    // 语音播报（§3.2）：网关把要念给机器人听的话发到 tts 主题
    if (self->voice_enabled_) {
      int rc_tts = mosquitto_subscribe(self->mosq_, nullptr, self->tts_topic_.c_str(), 1);
      if (rc_tts == MOSQ_ERR_SUCCESS) {
        RCLCPP_INFO(self->get_logger(), "已订阅语音播报 %s", self->tts_topic_.c_str());
      }
    }

    // 连接成功后把站点地图发布一次（retained），看板据此渲染厂区图
    if (self->publish_map_) self->publishSiteMap();
  }

  static void onDisconnectCb(struct mosquitto * /*mosq*/, void *obj, int rc)
  {
    auto *self = static_cast<MqttBridge *>(obj);
    {
      std::lock_guard<std::mutex> lk(self->data_mutex_);
      self->mqtt_connected_ = false;
    }
    if (rc == 0) RCLCPP_INFO(self->get_logger(), "MQTT 正常断开");
    else RCLCPP_WARN(self->get_logger(), "MQTT 意外断开 (rc=%d)，自动重连中", rc);
  }

  // =========================================================================
  // 站点地图发布
  // =========================================================================
  void publishSiteMap()
  {
    json zones = json::array();
    for (const auto &z : kZones) {
      // 矩形用"左上角 + 宽高"表达。因为看板 y 向下、仿真 y 向上，
      // 矩形顶边对应仿真 y 的【大】值（z.y + z.h），所以顶边取 simToDashY(z.y + z.h)。
      zones.push_back({
        {"id",     z.id},
        {"name",   z.name},
        {"x",      simToDashX(z.x)},
        {"y",      simToDashY(z.y + z.h)},
        {"width",  z.w * site_scale_},
        {"height", z.h * site_scale_},
      });
    }

    json points = json::array();
    for (const auto &p : kPoints) {
      json item = {
        {"id", p.id}, {"name", p.name},
        {"x", std::round(simToDashX(p.x) * 10.0) / 10.0},
        {"y", std::round(simToDashY(p.y) * 10.0) / 10.0},
      };
      if (p.zone) item["zone"] = p.zone;
      if (p.kind) item["kind"] = p.kind;
      points.push_back(item);
    }

    json site = {
      {"version", 1},
      {"width",  600.0},
      {"height", 380.0},
      {"zones",  zones},
      {"points", points},
      {"updatedAt", now().nanoseconds() / 1000000},
    };
    const std::string payload = site.dump();
    mosquitto_publish(mosq_, nullptr, map_topic_.c_str(),
      static_cast<int>(payload.size()), payload.c_str(), 1, true);
    RCLCPP_INFO(get_logger(), "站点地图已发布：%zu 个区域, %zu 个点位",
      kZones.size(), kPoints.size());
  }

  // =========================================================================
  // 下行指令处理（运行在 MQTT 网络线程）
  // =========================================================================
  static void onMessageCb(struct mosquitto * /*mosq*/, void *obj,
                          const struct mosquitto_message *msg)
  {
    if (!msg || !msg->payload || msg->payloadlen <= 0) return;
    auto *self = static_cast<MqttBridge *>(obj);
    const std::string text(static_cast<const char *>(msg->payload), msg->payloadlen);

    // ---- 任务列表快照（3.5 回写的底稿）----
    // 看板每次增删改任务都会 retained 发布全量数组，这里存一份最新版本。
    if (msg->topic && self->fleet_tasks_topic_ == msg->topic) {
      self->onFleetTasks(text);
      return;
    }

    // ---- 语音播报（§3.2）：念给机器人的话，转给 ROS 侧 ----
    if (msg->topic && self->tts_topic_ == msg->topic) {
      self->onTts(text);
      return;
    }

    json j;
    try {
      j = json::parse(text);
    } catch (const std::exception &e) {
      RCLCPP_WARN(self->get_logger(), "指令解析失败: %s", e.what());
      return;
    }

    const std::string action = j.value("action", "");
    RCLCPP_INFO(self->get_logger(), "收到看板指令: %s", text.c_str());

    // ---- 急停 ----
    if (action == "estop") {
      geometry_msgs::msg::Twist stop;
      self->pub_cmd_vel_->publish(stop);
      // 急停必须同时撤销导航目标，否则 Nav2 会立刻重新下发速度指令
      self->cancelNavigation("急停");
      {
        std::lock_guard<std::mutex> lk(self->data_mutex_);
        self->status_ = "fault";
        self->task_.clear();
      }
      RCLCPP_WARN(self->get_logger(), "执行紧急停止 (ESTOP)");
      self->publishEvent("critical", "收到看板紧急停止指令，已撤销导航目标并停车");
      return;
    }

    // ---- 取消任务（看板"取消"按钮）----
    // 早期版本没有这个分支，看板点取消只会得到"未知指令"，导航目标继续
    // 执行导致机器人顶着墙一直推。必须有。
    if (action == "cancel") {
      self->cancelNavigation("看板取消");
      return;
    }

    // ---- 复位 ----
    if (action == "resume") {
      {
        std::lock_guard<std::mutex> lk(self->data_mutex_);
        self->status_ = "idle";
        self->navigating_ = false;
      }
      self->pausePatrol(false);
      RCLCPP_INFO(self->get_logger(), "故障已复位");
      return;
    }

    // ---- 任务下发 ----
    // 两种报文形态都要兼容：
    //   看板/语音网关： {"action":"assign","task":{"id":3,"target":"charger-1","title":"..."}}
    //   简化手动测试： {"action":"assign","target":"charger-1"}
    if (action == "assign") {
      std::string target_id, title;
      int task_id = 0;
      json task_obj;      // 完整任务对象：回写时要原样带回全部字段
      if (j.contains("task") && j["task"].is_object()) {
        const auto &t = j["task"];
        target_id = t.value("target", "");
        title     = t.value("title", "");
        task_id   = t.value("id", 0);
        task_obj  = t;
      } else {
        target_id = j.value("target", "");
        title     = j.value("title", "");
        task_id   = j.value("taskId", 0);
      }
      self->startNavigation(target_id, title, task_id, task_obj);
      return;
    }

    RCLCPP_WARN(self->get_logger(), "未知指令 action='%s'", action.c_str());
  }

  // =========================================================================
  // 开始前往目标点
  // =========================================================================
  void startNavigation(const std::string &target_id, const std::string &title, int task_id,
                       const json &task_obj = json())
  {
    if (target_id.empty()) {
      RCLCPP_WARN(get_logger(), "任务未指定目标点，忽略");
      return;
    }
    // 在点位表里查找（先按 id，再按名称模糊匹配）
    const SitePoint *found = nullptr;
    for (const auto &p : kPoints) {
      if (target_id == p.id) { found = &p; break; }
    }
    if (!found) {
      for (const auto &p : kPoints) {
        if (target_id.find(p.name) != std::string::npos ||
            std::string(p.name).find(target_id) != std::string::npos) {
          found = &p; break;
        }
      }
    }
    if (!found) {
      RCLCPP_WARN(get_logger(), "未知目标点 '%s'，可用点位见启动日志", target_id.c_str());
      return;
    }

    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      nav_target_x_ = found->x;      // 仿真世界坐标
      nav_target_y_ = found->y;
      nav_target_name_ = found->name;
      navigating_ = true;
      status_ = "working";
      task_ = title.empty() ? std::string("前往 ") + found->name : title;
      nav_start_time_ = now();
      active_task_id_ = task_id;
      if (task_obj.is_object()) cached_task_ = task_obj;   // 回写底稿
    }
    // 派单接管控制权：先让巡逻停手，否则两个节点同时发导航目标互相打断
    pausePatrol(true);
    RCLCPP_INFO(get_logger(), "开始前往 [%s] %s  目标=(%.2f, %.2f) 任务#%d",
      found->id, found->name, found->x, found->y, task_id);

    sendNavGoal(found->x, found->y, found->name);
  }

  // =========================================================================
  // 下发 Nav2 导航目标（map 坐标系）
  // =========================================================================
  void sendNavGoal(double x, double y, const std::string & name)
  {
    if (!nav_client_->wait_for_action_server(2s)) {
      RCLCPP_WARN(get_logger(), "Nav2 action server 未就绪，目标暂未下发（稍后重试）");
      // 保留 navigating_=true，由 onNavTimer 的超时逻辑兜底重试/放弃
      return;
    }

    NavigateToPose::Goal goal;
    goal.pose.header.frame_id = "map";
    goal.pose.header.stamp = now();
    goal.pose.pose.position.x = x;
    goal.pose.pose.position.y = y;
    goal.pose.pose.position.z = 0.0;
    goal.pose.pose.orientation.w = 1.0;   // 不约束最终朝向，到位即可

    auto options = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();
    options.goal_response_callback =
      [this, name](const NavGoalHandle::SharedPtr & handle) {
        if (!handle) {
          RCLCPP_WARN(get_logger(), "Nav2 拒绝了目标 %s（可能不在代价地图可通行区）", name.c_str());
          finishTask("（目标被拒绝）", Outcome::Failed);
          return;
        }
        std::lock_guard<std::mutex> lk(data_mutex_);
        nav_goal_ = handle;
        RCLCPP_INFO(get_logger(), "Nav2 已接受目标 %s", name.c_str());
      };
    options.result_callback =
      [this, name](const NavGoalHandle::WrappedResult & result) {
        if (result.code == rclcpp_action::ResultCode::SUCCEEDED) {
          RCLCPP_INFO(get_logger(), "任务完成：已到达 %s", name.c_str());
          finishTask(name, Outcome::Success);
        } else if (result.code == rclcpp_action::ResultCode::CANCELED) {
          RCLCPP_INFO(get_logger(), "导航已取消（%s）", name.c_str());
          finishTask("已取消", Outcome::Canceled);
        } else {
          RCLCPP_WARN(get_logger(), "导航失败（%s），Nav2 将自行尝试恢复行为", name.c_str());
          char buf[192];
          std::snprintf(buf, sizeof(buf),
            "导航失败（%s）：Nav2 未能到达目标点，本次任务放弃", name.c_str());
          publishEvent("warning", buf);
          finishTask("（导航失败）", Outcome::Failed);
        }
      };
    nav_client_->async_send_goal(goal, options);
  }

  // 任务结局：决定回写 fleet/tasks 时把 status 改成什么
  enum class Outcome { Success, Canceled, Failed };

  // 结束当前任务：清状态、停车、恢复巡逻、回写任务状态
  void finishTask(const std::string & detail, Outcome outcome = Outcome::Canceled)
  {
    int tid = 0;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      navigating_ = false;
      status_ = "idle";
      task_.clear();
      nav_goal_.reset();
      tid = active_task_id_;
      active_task_id_ = 0;
    }
    pub_cmd_vel_->publish(geometry_msgs::msg::Twist());   // 确保停车
    pausePatrol(false);

    // 3.5 任务回写：成功 → completed；被取消/急停 → revoked；
    // 失败则保持 published，让调度方可以重新派单（规范里只有这四种状态）。
    if (outcome == Outcome::Success) {
      writeBackTask(tid, "completed");
    } else if (outcome == Outcome::Canceled) {
      writeBackTask(tid, "revoked");
    }
    (void)detail;
  }

  // 取消当前导航目标（看板"取消"按钮 / 急停都走这里）
  void cancelNavigation(const char * reason)
  {
    std::shared_ptr<NavGoalHandle> handle;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      handle = nav_goal_;
    }
    if (handle) {
      nav_client_->async_cancel_goal(handle);
      RCLCPP_WARN(get_logger(), "取消导航目标（%s）", reason);
      char buf[160];
      std::snprintf(buf, sizeof(buf), "任务被取消：%s", reason);
      publishEvent("info", buf);
    } else {
      RCLCPP_INFO(get_logger(), "当前无导航目标可取消（%s）", reason);
    }
    finishTask("已取消", Outcome::Canceled);
  }


  // =========================================================================
  // 3.5 告警上报 {P}/event
  //
  // 报文（见 MQTT-INTERFACE.md 5.2 节）：
  //   { "alarmId": 1042, "ts": 1789008000000, "level": "critical",
  //     "source": "仿真狗", "message": "..." }
  // level 取 info / warning / critical；alarmId 由上报方分配且全局递增。
  // 没有这条上行通道时，看板的告警中心在真实链路下永远是空的
  // （只有 mock 模式的设备模拟器会自己造告警）。
  // =========================================================================
  void publishEvent(const char * level, const std::string &message)
  {
    if (!mqtt_connected_) return;   // 断线时不上报，避免重连后灌一堆旧告警

    json e = {
      {"alarmId", alarm_base_id_ + (++alarm_seq_)},
      {"ts",      wallClockMs()},
      {"level",   level},
      {"source",  robot_name_},
      {"message", message},
    };
    const std::string payload = e.dump();
    int rc = mosquitto_publish(mosq_, nullptr, event_topic_.c_str(),
      static_cast<int>(payload.size()), payload.c_str(), 1, false);
    if (rc == MOSQ_ERR_SUCCESS) {
      RCLCPP_WARN(get_logger(), "已上报告警[%s] %s", level, message.c_str());
    } else {
      RCLCPP_WARN(get_logger(), "告警上报失败: %s", mosquitto_strerror(rc));
    }
  }

  // =========================================================================
  // 语音上行：PCM 片段 → {P}/robots/{id}/audio
  // 报文格式见 VOICE-INTERFACE.md §3.1（QoS1，非 retained）
  // =========================================================================
  void onAudioPcm(const std::vector<uint8_t> &pcm)
  {
    if (!mqtt_connected_ || pcm.empty()) return;
    const size_t bytes = pcm.size() - (pcm.size() % 2);   // 16bit 样本必须整对齐
    if (bytes == 0) return;

    const double dur = static_cast<double>(bytes) /
                       (2.0 * audio_channels_ * audio_sample_rate_);
    json j = {
      {"robotId",    robot_id_},
      {"ts",         wallClockMs()},
      {"format",     "pcm_s16le"},
      {"sampleRate", audio_sample_rate_},
      {"channels",   audio_channels_},
      {"data",       base64Encode(pcm.data(), bytes)},
    };
    const std::string payload = j.dump();
    const int rc = mosquitto_publish(mosq_, nullptr, audio_topic_.c_str(),
      static_cast<int>(payload.size()), payload.c_str(), 1, false);
    if (rc == MOSQ_ERR_SUCCESS) {
      RCLCPP_INFO(get_logger(), "语音已上行：%.2f s（%zu KB PCM → %zu KB base64）",
        dur, bytes / 1024, payload.size() / 1024);
    } else {
      RCLCPP_WARN(get_logger(), "语音上行失败：%s", mosquitto_strerror(rc));
    }
  }

  // =========================================================================
  // 语音播报下行（§3.2）：仿真里没有喇叭，转成 ROS 话题让上层/TTS 节点处理
  // =========================================================================
  void onTts(const std::string &text)
  {
    std::string say = text;
    std::string cmd_id;
    try {
      const json j = json::parse(text);
      say = j.value("text", "");
      cmd_id = j.value("cmdId", "");
    } catch (const std::exception &) { /* 允许纯文本报文 */ }
    if (say.empty()) return;

    RCLCPP_INFO(get_logger(), "语音播报%s：%s",
      cmd_id.empty() ? "" : (" [" + cmd_id + "]").c_str(), say.c_str());
    std_msgs::msg::String m;
    m.data = say;
    pub_tts_text_->publish(m);
    enqueueTts(say);
  }

  // =========================================================================
  // TTS 本地播报（课程标准）：piper 合成 zh_CN-huayan-medium → aplay 播放。
  // 队列化：网关可能连发多条（"收到"+"已执行"），串行播不互相打断。
  // =========================================================================
  void enqueueTts(const std::string & text)
  {
    if (text.empty()) return;
    {
      std::lock_guard<std::mutex> lk(tts_mutex_);
      tts_queue_.push_back(text);
    }
    tts_cv_.notify_one();
  }

  void ttsWorker()
  {
    while (rclcpp::ok()) {
      std::string text;
      {
        std::unique_lock<std::mutex> lk(tts_mutex_);
        tts_cv_.wait(lk, [this] { return !tts_queue_.empty() || !rclcpp::ok(); });
        if (tts_queue_.empty()) continue;
        text = tts_queue_.front();
        tts_queue_.pop_front();
      }
      RCLCPP_INFO(get_logger(), "TTS 播报: %s", text.c_str());
      const std::string inFile = "/tmp/qbot_tts_in.txt";
      const std::string outFile = "/tmp/qbot_tts.wav";
      {
        std::ofstream f(inFile);
        f << text << "\n";
      }
      const std::string synth = piper_bin_ + " -m " + piper_model_ +
        " -f " + outFile + " < " + inFile + " 2>/dev/null";
      if (std::system(synth.c_str()) == 0) {
        std::system(("aplay -q " + outFile + " 2>/dev/null").c_str());
      } else {
        // 没装 piper / 没有音频输出设备时只留日志，不影响链路
        RCLCPP_WARN(get_logger(), "TTS 合成/播放失败（piper 不可用或无声卡输出）");
      }
    }
  }

  // 同类告警冷却：同一个 key 在 cooldown_s 内只报一次，避免每秒刷屏
  bool eventAllowed(const std::string &key, double cooldown_s)
  {
    const auto t = now();
    std::lock_guard<std::mutex> lk(data_mutex_);
    auto it = event_last_.find(key);
    if (it != event_last_.end() && (t - it->second).seconds() < cooldown_s) return false;
    event_last_[key] = t;
    return true;
  }

  // =========================================================================
  // 3.5 任务状态回写 {P}/fleet/tasks
  //
  // 看板是任务列表的主要写入方，每次增删改都 retained 发布【全量数组】；
  // 机器人执行结束后要在同一主题上把该任务 status 改成 completed / revoked
  // 并重新全量发布（retained）。否则看板的任务列表永远停在"已发布"，
  // 用户根本看不出任务到底做完了没有。
  // =========================================================================
  void writeBackTask(int task_id, const char *new_status)
  {
    if (task_id <= 0) return;      // 无 id（如手动测试报文）无可回写

    std::string payload;
    bool ready = false;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      const long long ts = wallClockMs();

      // 记下终态：看板之后若重发旧列表，还要把这个终态盖回去
      task_terminal_[task_id] = std::make_pair(std::string(new_status), ts);

      if (!fleet_tasks_.is_array()) {
        // 还没收到看板的快照：用 assign 里缓存的任务对象自己拼一个数组
        if (cached_task_.is_object()) {
          json t = cached_task_;
          t["id"] = task_id;
          t["status"] = new_status;
          t["updatedAt"] = ts;
          fleet_tasks_ = json::array({t});
          ready = true;
        }
      } else {
        bool hit = false;
        for (auto &t : fleet_tasks_) {
          if (t.value("id", 0) == task_id) {
            t["status"] = new_status;
            t["updatedAt"] = ts;
            hit = true;
            break;
          }
        }
        // 快照里没有这一条（看板可能还没同步到）：补一条进去
        if (!hit && cached_task_.is_object()) {
          json t = cached_task_;
          t["id"] = task_id;
          t["status"] = new_status;
          t["updatedAt"] = ts;
          fleet_tasks_.push_back(t);
        }
        ready = true;
      }
      if (ready) payload = fleet_tasks_.dump();
    }

    if (!ready) {
      RCLCPP_WARN(get_logger(), "任务#%d 状态回写跳过：既无列表快照也无任务对象", task_id);
      return;
    }

    int rc = mosquitto_publish(mosq_, nullptr, fleet_tasks_topic_.c_str(),
      static_cast<int>(payload.size()), payload.c_str(), 1, /*retain=*/true);
    if (rc == MOSQ_ERR_SUCCESS) {
      RCLCPP_INFO(get_logger(), "任务#%d 已回写 %s（fleet/tasks 全量 %zu 字节）",
        task_id, new_status, payload.size());
    } else {
      RCLCPP_WARN(get_logger(), "任务#%d 回写失败: %s",
        task_id, mosquitto_strerror(rc));
    }
  }

  // 收到看板的任务列表全量快照：存下来，并把本机已产生的终态盖回去
  void onFleetTasks(const std::string &text)
  {
    json arr;
    try {
      arr = json::parse(text);
    } catch (const std::exception &e) {
      RCLCPP_WARN(get_logger(), "任务列表解析失败: %s", e.what());
      return;
    }
    if (!arr.is_array()) return;

    bool patched = false;
    std::string payload;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      for (auto &t : arr) {
        auto it = task_terminal_.find(t.value("id", 0));
        if (it != task_terminal_.end() && t.value("status", "") != it->second.first) {
          t["status"] = it->second.first;
          t["updatedAt"] = it->second.second;
          patched = true;
        }
      }
      fleet_tasks_ = arr;
      if (patched) payload = fleet_tasks_.dump();
    }
    RCLCPP_INFO(get_logger(), "已同步看板任务列表：%zu 条%s",
      arr.size(), patched ? "（已补回本机终态）" : "");

    // 只有真的补过终态才回发一次，避免和看板来回刷。
    // 看板收到后只 setTasks 不会回发，所以不存在无限循环。
    if (patched && mqtt_connected_) {
      mosquitto_publish(mosq_, nullptr, fleet_tasks_topic_.c_str(),
        static_cast<int>(payload.size()), payload.c_str(), 1, true);
    }
  }

  // =========================================================================
  // 3.5 阈值告警检查（挂在 1 Hz 状态定时器上）
  // =========================================================================
  void checkAlarms()
  {
    if (!mqtt_connected_) return;

    double bat = 0.0, temp = 0.0;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      bat = battery_;
      temp = motor_temp_;
    }

    char buf[192];

    // ---- 电量：越阈只报一次（不会因为持续低电而每秒刷一条）----
    if (bat <= battery_crit_ && !battery_crit_reported_) {
      battery_crit_reported_ = true;
      battery_warn_reported_ = true;
      std::snprintf(buf, sizeof(buf),
        "电池电量 %.1f%% 低于严重阈值 %.0f%%，请立即返回充电桩", bat, battery_crit_);
      publishEvent("critical", buf);
    } else if (bat <= battery_warn_ && !battery_warn_reported_) {
      battery_warn_reported_ = true;
      std::snprintf(buf, sizeof(buf),
        "电池电量 %.1f%% 低于预警阈值 %.0f%%，建议尽快安排充电", bat, battery_warn_);
      publishEvent("warning", buf);
    }

    // ---- 电机温度：带 60 s 冷却，高温持续时不会刷屏 ----
    if (temp >= temp_crit_) {
      if (eventAllowed("temp_crit", 60.0)) {
        std::snprintf(buf, sizeof(buf),
          "电机温度 %.1f°C 超过临界阈值 %.0f°C，立即停机检查", temp, temp_crit_);
        publishEvent("critical", buf);
      }
    } else if (temp >= temp_warn_) {
      if (eventAllowed("temp_warn", 60.0)) {
        std::snprintf(buf, sizeof(buf),
          "电机温度 %.1f°C 超过预警阈值 %.0f°C，注意散热", temp, temp_warn_);
        publishEvent("warning", buf);
      }
    }
  }


  // =========================================================================
  // 巡视定时器（20 Hz）：初始位姿对齐 + 任务超时兜底
  //
  // 注意：这里【不再】直接发 cmd_vel 驱动轮子。行驶由 Nav2 的
  // controller_server 负责（它会看局部代价地图绕障），本函数只做状态维护。
  // =========================================================================
  void onNavTimer()
  {
    // ① 启动后补发一次 AMCL 初始位姿（见构造函数注释）
    if (!initial_pose_sent_ && odom_ok_) {
      if ((now() - start_time_).seconds() > 8.0) {
        publishInitialPose();
        initial_pose_sent_ = true;
      }
      return;
    }

    // ② 任务超时兜底：Nav2 长时间无结果时主动取消，避免"永远 working"
    bool timeout = false;
    std::string name;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      if (!navigating_) return;
      if ((now() - nav_start_time_).seconds() > nav_timeout_s_) {
        timeout = true;
        name = nav_target_name_;
      }
    }
    if (timeout) {
      RCLCPP_WARN(get_logger(), "任务超时（%.0f s）自动取消：%s",
        nav_timeout_s_, name.c_str());
      char buf[192];
      std::snprintf(buf, sizeof(buf), "导航超时（%.0f s）：未能到达 %s，任务已撤销",
        nav_timeout_s_, name.c_str());
      publishEvent("warning", buf);
      cancelNavigation("超时");
    }
  }

  // 用当前里程计位姿对齐 AMCL，消除初始定位偏差
  void publishInitialPose()
  {
    geometry_msgs::msg::PoseWithCovarianceStamped msg;
    msg.header.frame_id = "map";
    msg.header.stamp = now();
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      msg.pose.pose.position.x = pos_x_;
      msg.pose.pose.position.y = pos_y_;
      msg.pose.pose.position.z = 0.0;
      // 朝向：由 yaw 还原四元数（只用 z/w 分量）
      msg.pose.pose.orientation.z = std::sin(heading_ * 0.5);
      msg.pose.pose.orientation.w = std::cos(heading_ * 0.5);
    }
    // 协方差：位置 0.25 m²、朝向 0.07 rad²，给粒子滤波合理的初始散布
    msg.pose.covariance = {0.25, 0, 0, 0, 0, 0,
                           0, 0.25, 0, 0, 0, 0,
                           0, 0, 0.06, 0, 0, 0,
                           0, 0, 0, 0.02, 0, 0,
                           0, 0, 0, 0, 0.02, 0,
                           0, 0, 0, 0, 0, 0.07};
    pub_initial_pose_->publish(msg);
    RCLCPP_INFO(get_logger(), "已按里程计对齐 AMCL 初始位姿 (%.2f, %.2f)",
      msg.pose.pose.position.x, msg.pose.pose.position.y);
  }

  // 巡逻开关：派单期间暂停巡逻，避免两个节点抢同一个导航通道
  void pausePatrol(bool paused)
  {
    std_msgs::msg::Bool m;
    m.data = !paused;
    pub_patrol_enable_->publish(m);
  }


  // =========================================================================
  // 状态上报（1 Hz）
  // =========================================================================
  void onPublishTimer()
  {
    json state;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);

      // 电量：行走耗电快、待机慢（真实机器人应从 BMS 读取）
      const bool moving = speed_ > 0.02;
      const double drain_moving = 0.05;   // %/s
      const double drain_idle   = 0.005;  // %/s
      battery_ = std::max(0.0, battery_ - (moving ? drain_moving : drain_idle) / publish_hz_);

      // 状态机：fault > 有任务 > 运动中 > 待机
      if (status_ != "fault") {
        if (!task_.empty() || navigating_) status_ = "working";
        else                                status_ = moving ? "working" : "idle";
      }

      // 电机温度：基础 38°C + 步态负载（归一化 0~1）× 22°C + 行走附加 6°C。
      // 典型值：静止停靠 38°C、正常巡逻 55~60°C、重载/受阻顶墙 66°C。
      // 与"步态电机正常工作 40~70°C、超过 75°C 需预警"的经验区间对齐，
      // 既不会动不动就报临界，也不会在真过热时漏报。
      const double target_temp = 38.0 + joint_load_ * 22.0 + (moving ? 6.0 : 0.0);
      motor_temp_ += (target_temp - motor_temp_) * 0.1;

      state = {
        {"robotId",   robot_id_},
        {"name",      robot_name_},
        {"model",     robot_model_},
        {"ts",        now().nanoseconds() / 1000000},
        {"status",    status_},
        {"battery",   std::round(battery_ * 10.0) / 10.0},
        // 位置：仿真坐标 → 看板地图坐标
        {"x",         std::round(simToDashX(pos_x_) * 10.0) / 10.0},
        {"y",         std::round(simToDashY(pos_y_) * 10.0) / 10.0},
        {"speed",     std::round(speed_ * 100.0) / 100.0},
        // 朝向同样要取反：看板 y 轴向下，与仿真相反，否则箭头方向与地图运动方向不一致
        {"heading",   std::round(wrapPi(-heading_) * 1000.0) / 1000.0},
        {"motorTemp", std::round(motor_temp_ * 10.0) / 10.0},
        {"signal",    signal_},
        {"uptimeH",   std::round((now() - start_time_).seconds() / 36.0) / 100.0},
        {"currentTask", task_.empty() ? json(nullptr) : json(task_)},
        {"cam",       0},
      };
      connected_snapshot_ = mqtt_connected_;
      odom_snapshot_ = odom_ok_;
    }

    if (!connected_snapshot_) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "MQTT 未连接，本帧丢弃");
      return;
    }
    if (!odom_snapshot_) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "尚未收到 /odom");
    }

    const std::string payload = state.dump();
    int rc = mosquitto_publish(mosq_, nullptr, state_topic_.c_str(),
      static_cast<int>(payload.size()), payload.c_str(), 1, true);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
        "publish 失败: %s", mosquitto_strerror(rc));
    }

    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 10000,
      "上报: %s 电量 %.1f%% 仿真位姿 (%.2f,%.2f) 速度 %.2f%s",
      status_.c_str(), battery_, pos_x_, pos_y_, speed_,
      navigating_ ? " [前往目标中]" : "");

    // 3.5 阈值告警：复用 1 Hz 状态节拍检查电量与电机温度
    checkAlarms();
  }

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

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<MqttBridge>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
