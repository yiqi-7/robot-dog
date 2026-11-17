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
//   下发  {P}/robots/{id}/cmd   → estop / resume / assign
//   地图  {P}/site/map          → 一次性 retained 发布（连接成功后）
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
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <mosquitto.h>
#include <nlohmann/json.hpp>

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
    broker_host_   = declare_parameter("broker_host", "192.168.137.1");
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

    // 前往目标点的行驶参数
    nav_speed_     = declare_parameter("nav_speed", 0.25);   // m/s
    nav_tolerance_ = declare_parameter("nav_tolerance", 0.30);// m，到达判定
    nav_timeout_s_ = declare_parameter("nav_timeout", 120.0); // s

    state_topic_ = topic_prefix_ + "/robots/" + robot_id_ + "/state";
    cmd_topic_   = topic_prefix_ + "/robots/" + robot_id_ + "/cmd";
    map_topic_   = topic_prefix_ + "/site/map";

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
        double load = 0.0;
        for (double v : msg->velocity) load += std::abs(v);
        joint_load_ = load;
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
      "  地图主题   : %s（缩放 %.1f×，世界 %.0f×%.0f m）\n"
      "  遗嘱消息   : 已注册",
      broker_host_.c_str(), broker_port_,
      robot_id_.c_str(), robot_name_.c_str(),
      state_topic_.c_str(), publish_hz_, cmd_topic_.c_str(),
      map_topic_.c_str(), site_scale_, site_world_w_, site_world_h_);
  }

  ~MqttBridge() override
  {
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

    mosquitto_subscribe(self->mosq_, nullptr, self->cmd_topic_.c_str(), 1);

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
      if (j.contains("task") && j["task"].is_object()) {
        const auto &t = j["task"];
        target_id = t.value("target", "");
        title     = t.value("title", "");
        task_id   = t.value("id", 0);
      } else {
        target_id = j.value("target", "");
        title     = j.value("title", "");
        task_id   = j.value("taskId", 0);
      }
      self->startNavigation(target_id, title, task_id);
      return;
    }

    RCLCPP_WARN(self->get_logger(), "未知指令 action='%s'", action.c_str());
  }

  // =========================================================================
  // 开始前往目标点
  // =========================================================================
  void startNavigation(const std::string &target_id, const std::string &title, int task_id)
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
          finishTask("（目标被拒绝）");
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
          finishTask(name);
        } else if (result.code == rclcpp_action::ResultCode::CANCELED) {
          RCLCPP_INFO(get_logger(), "导航已取消（%s）", name.c_str());
          finishTask("已取消");
        } else {
          RCLCPP_WARN(get_logger(), "导航失败（%s），Nav2 将自行尝试恢复行为", name.c_str());
          finishTask("（导航失败）");
        }
      };
    nav_client_->async_send_goal(goal, options);
  }

  // 结束当前任务：清状态、停车、恢复巡逻
  void finishTask(const std::string & detail)
  {
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      navigating_ = false;
      status_ = "idle";
      task_.clear();
      nav_goal_.reset();
    }
    pub_cmd_vel_->publish(geometry_msgs::msg::Twist());   // 确保停车
    pausePatrol(false);
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
    } else {
      RCLCPP_INFO(get_logger(), "当前无导航目标可取消（%s）", reason);
    }
    finishTask("已取消");
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

      // 电机温度：基础 38°C + 关节负载，一阶惯性避免跳变
      const double target_temp = 38.0 + joint_load_ * 1.5 + (moving ? 8.0 : 0.0);
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
  }

  // ---------------- 成员变量 ----------------
  std::string broker_host_, robot_id_, robot_name_, robot_model_;
  std::string topic_prefix_, username_, password_;
  std::string state_topic_, cmd_topic_, map_topic_;
  int broker_port_ = 1883, keepalive_ = 45;
  double publish_hz_ = 1.0, battery_ = 100.0;

  double site_scale_ = 20.0, site_offset_x_ = 100.0, site_offset_y_ = 50.0;
  double site_world_w_ = 20.0, site_world_h_ = 14.0;
  bool publish_map_ = true;

  double nav_speed_ = 0.25, nav_tolerance_ = 0.30, nav_timeout_s_ = 120.0;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_patrol_;
  std::string patrol_status_;

  struct mosquitto *mosq_ = nullptr;

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
