// ============================================================================
// mqtt_bridge.cpp —— 机器狗 ↔ 云端看板 MQTT 桥接节点（C++）
//
// 作用
// ----
// 把仿真机器狗的 ROS 2 数据上报到 MQTT 看板，并接收看板下发的指令。
// 这是"第三章 3.2 机器人与云端通信框架"的核心实现。
//
// 数据流
// ------
//   上行（机器人 → 看板）
//     /odom            → 位置 x,y、速度、偏航 heading
//     /joint_states    → 关节负载 → 估电机温度
//     /imu             → 俯仰/横滚（预留，看板暂不显示）
//     定时器 1 Hz      → 组装 JSON → 发布 {P}/robots/{id}/state（QoS1 + retained）
//
//   下行（看板 → 机器人）
//     {P}/robots/{id}/cmd → 解析 action：
//       estop  → 立即发布零速 /cmd_vel，状态置 fault
//       resume → 清除 fault，恢复 idle
//       assign → 记录目标任务（5.2 章节再接入 Nav2）
//
// 连接机制（3.3 心跳基础）
// ------------------------
//   1. 遗嘱消息（LWT）：连接时向 Broker 注册"我断了就替我发这条"
//      → 机器狗断电/断网时，Broker 立刻代发 status=offline，看板秒级感知
//   2. keepalive：45 秒（为将来经 Cloudflare WebSocket 留余量，CF 空闲超时 100s）
//   3. 自动重连：mosquitto_loop_start 内部重连 + on_disconnect 打日志
//
// 线程安全
// --------
//   MQTT 回调运行在 libmosquitto 自己的网络线程，ROS 定时器运行在 executor 线程，
//   两者共享状态（status_、task_、mqtt_connected_ 等）必须用 mutex 保护。
//
// 参数
// ----
//   broker_host  MQTT Broker 地址（默认 192.168.137.1，即 Windows 主机）
//   broker_port  MQTT 端口（默认 1883）
//   robot_id     机器人编号（默认 robot-01）
//   robot_name   机器人显示名（默认 仿真狗）
//   robot_model  型号字符串
//   topic_prefix 主题前缀（默认 qbot/v1，必须与看板一致）
//   publish_hz   上报频率（默认 1.0 Hz）
//   keepalive    MQTT 心跳秒数（默认 45）
//   username     可选，Broker 开启认证时填
//   password     可选
//   battery_init 初始电量（默认 100.0 %）
// ============================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>

#include <mosquitto.h>
#include <nlohmann/json.hpp>

#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

using json = nlohmann::json;
using namespace std::chrono_literals;

namespace {

// 把角度归一化到 [-pi, pi]，避免偏航角累计超过一圈后跳变
double wrapPi(double a)
{
  while (a > M_PI) a -= 2.0 * M_PI;
  while (a < -M_PI) a += 2.0 * M_PI;
  return a;
}

// 四元数 → 偏航角（只取绕 Z 的分量，本项目机器狗始终近似水平）
double yawFromQuat(double x, double y, double z, double w)
{
  return std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
}

}  // namespace

class MqttBridge : public rclcpp::Node
{
public:
  MqttBridge() : rclcpp::Node("mqtt_bridge")
  {
    // ---------------- 参数声明 ----------------
    broker_host_  = declare_parameter("broker_host", "192.168.137.1");
    broker_port_  = declare_parameter("broker_port", 1883);
    robot_id_     = declare_parameter("robot_id", "robot-01");
    robot_name_   = declare_parameter("robot_name", "仿真狗");
    robot_model_  = declare_parameter("robot_model", "quadruped-sim");
    topic_prefix_ = declare_parameter("topic_prefix", "qbot/v1");
    publish_hz_   = declare_parameter("publish_hz", 1.0);
    keepalive_    = declare_parameter("keepalive", 45);
    username_     = declare_parameter("username", "");
    password_     = declare_parameter("password", "");
    battery_      = declare_parameter("battery_init", 100.0);

    // 主题拼接：必须与看板 src/mqtt/topics.ts 逐字一致
    state_topic_ = topic_prefix_ + "/robots/" + robot_id_ + "/state";
    cmd_topic_   = topic_prefix_ + "/robots/" + robot_id_ + "/cmd";

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
        // 用关节角速度绝对值粗略代表负载：转得越快，电机越热
        double load = 0.0;
        for (double v : msg->velocity) load += std::abs(v);
        joint_load_ = load;
      });

    // ---------------- ROS 发布（接收看板指令后控制机器狗） ----------------
    pub_cmd_vel_ = create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);

    // ---------------- MQTT 初始化 ----------------
    initMqtt();

    // ---------------- 定时上报 ----------------
    auto period = std::chrono::duration<double>(1.0 / std::max(0.1, publish_hz_));
    timer_ = create_wall_timer(
      std::chrono::duration_cast<std::chrono::nanoseconds>(period),
      [this]() { onPublishTimer(); });

    RCLCPP_INFO(get_logger(),
      "MQTT 桥已启动\n"
      "  Broker      : %s:%d\n"
      "  机器人      : %s (%s)\n"
      "  上报主题    : %s  @ %.1f Hz\n"
      "  指令主题    : %s\n"
      "  遗嘱消息    : 已注册（掉线自动上报 offline）",
      broker_host_.c_str(), broker_port_,
      robot_id_.c_str(), robot_name_.c_str(),
      state_topic_.c_str(), publish_hz_, cmd_topic_.c_str());
  }

  ~MqttBridge() override
  {
    if (mosq_) {
      mosquitto_disconnect(mosq_);   // 主动断开 → 不发遗嘱（正常关机）
      mosquitto_loop_stop(mosq_, false);
      mosquitto_destroy(mosq_);
      mosq_ = nullptr;
    }
    mosquitto_lib_cleanup();
  }

private:
  // ------------------------------------------------------------------
  // MQTT 初始化
  // ------------------------------------------------------------------
  void initMqtt()
  {
    mosquitto_lib_init();

    // client_id 用 robot_id，Broker 靠它识别会话；clean_session=true 表示不保留离线队列
    mosq_ = mosquitto_new(robot_id_.c_str(), true, this);
    if (!mosq_) {
      RCLCPP_ERROR(get_logger(), "mosquitto_new 失败（内存不足？）");
      return;
    }

    // ---- 遗嘱消息 LWT：这是 3.3 心跳机制的关键 ----
    // 只有在【非正常断开】（断电/断网/进程被杀）时，Broker 才会代发这条消息。
    // retain=true 让看板后连也能立刻看到"离线"状态。
    json will_payload = {
      {"robotId", robot_id_},
      {"name",    robot_name_},
      {"model",   robot_model_},
      {"status",  "offline"},
      {"ts",      0},
    };
    std::string will_str = will_payload.dump();
    mosquitto_will_set(mosq_, state_topic_.c_str(),
      static_cast<int>(will_str.size()), will_str.c_str(), 1, true);

    // ---- 可选认证 ----
    if (!username_.empty()) {
      mosquitto_username_pw_set(mosq_, username_.c_str(), password_.c_str());
    }

    // ---- 回调注册 ----
    mosquitto_connect_callback_set(mosq_, &MqttBridge::onConnectCb);
    mosquitto_message_callback_set(mosq_, &MqttBridge::onMessageCb);
    mosquitto_disconnect_callback_set(mosq_, &MqttBridge::onDisconnectCb);

    // ---- 异步连接 + 启动网络线程 ----
    int rc = mosquitto_connect_async(mosq_, broker_host_.c_str(), broker_port_, keepalive_);
    if (rc != MOSQ_ERR_SUCCESS) {
      RCLCPP_ERROR(get_logger(), "connect_async 失败: %s", mosquitto_strerror(rc));
      return;
    }
    mosquitto_loop_start(mosq_);   // 独立线程收发，内部自动重连
  }

  // ---------------- MQTT 回调（运行在网络线程） ----------------
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

    // 连接成功后订阅指令主题（QoS1）
    mosquitto_subscribe(self->mosq_, nullptr, self->cmd_topic_.c_str(), 1);
  }

  static void onDisconnectCb(struct mosquitto * /*mosq*/, void *obj, int rc)
  {
    auto *self = static_cast<MqttBridge *>(obj);
    {
      std::lock_guard<std::mutex> lk(self->data_mutex_);
      self->mqtt_connected_ = false;
    }
    if (rc == 0) {
      RCLCPP_INFO(self->get_logger(), "MQTT 正常断开");
    } else {
      RCLCPP_WARN(self->get_logger(), "MQTT 意外断开（rc=%d），libmosquitto 将自动重连", rc);
    }
  }

  static void onMessageCb(struct mosquitto * /*mosq*/, void *obj,
                          const struct mosquitto_message *msg)
  {
    if (!msg || !msg->payload || msg->payloadlen <= 0) return;
    auto *self = static_cast<MqttBridge *>(obj);
    std::string text(static_cast<const char *>(msg->payload), msg->payloadlen);

    json j;
    try {
      j = json::parse(text);
    } catch (const std::exception &e) {
      RCLCPP_WARN(self->get_logger(), "指令 JSON 解析失败: %s | 原文: %s", e.what(), text.c_str());
      return;
    }

    const std::string action = j.value("action", "");
    RCLCPP_INFO(self->get_logger(), "收到看板指令: %s", text.c_str());

    // ---- 急停：立即零速 + 状态置 fault ----
    if (action == "estop") {
      geometry_msgs::msg::Twist stop;
      self->pub_cmd_vel_->publish(stop);
      std::lock_guard<std::mutex> lk(self->data_mutex_);
      self->status_ = "fault";
      self->task_.clear();
      RCLCPP_WARN(self->get_logger(), "执行紧急停止 (ESTOP)");
      return;
    }

    // ---- 复位：清除故障状态 ----
    if (action == "resume") {
      std::lock_guard<std::mutex> lk(self->data_mutex_);
      self->status_ = "idle";
      RCLCPP_INFO(self->get_logger(), "故障已复位");
      return;
    }

    // ---- 任务下发：本章只记录，5.2 再接 Nav2 ----
    if (action == "assign") {
      std::lock_guard<std::mutex> lk(self->data_mutex_);
      self->status_ = "working";
      self->task_ = j.value("target", std::string("未指定目标"));
      RCLCPP_INFO(self->get_logger(), "接收任务: %s → %s",
        j.value("taskId", std::string("?")).c_str(), self->task_.c_str());
      return;
    }

    RCLCPP_WARN(self->get_logger(), "未知指令 action='%s'，已忽略", action.c_str());
  }

  // ------------------------------------------------------------------
  // 定时上报状态（3.4 状态与电量管理）
  // ------------------------------------------------------------------
  void onPublishTimer()
  {
    json state;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);

      // ---- 电量模拟：行走耗电快，待机耗电慢 ----
      // 真实机器人从 BMS 读取；仿真里按运动状态线性估算
      const double drain_moving = 0.05;   // %/s，约 33 分钟耗尽
      const double drain_idle   = 0.005;  // %/s，约 5.5 小时耗尽
      const bool moving = speed_ > 0.02;
      battery_ = std::max(0.0, battery_ - (moving ? drain_moving : drain_idle) / publish_hz_);

      // ---- 状态机：优先级 fault > 有任务 > 运动中 > 待机 ----
      // 注意：不能在"有任务但还没开始走"时被判成 idle，否则看板上任务刚派下去
      // 状态就闪回待机，操作员会以为任务丢了
      if (status_ != "fault") {
        if (!task_.empty()) {
          status_ = "working";
        } else {
          status_ = moving ? "working" : "idle";
        }
      }

      // ---- 电机温度：基础 38°C + 关节负载，行走时升高 ----
      const double target_temp = 38.0 + joint_load_ * 1.5 + (moving ? 8.0 : 0.0);
      motor_temp_ += (target_temp - motor_temp_) * 0.1;   // 一阶惯性，避免跳变

      state = {
        {"robotId",     robot_id_},
        {"name",        robot_name_},
        {"model",       robot_model_},
        {"ts",          now().nanoseconds() / 1000000},   // epoch 毫秒
        {"status",      status_},
        {"battery",     std::round(battery_ * 10.0) / 10.0},
        {"x",           std::round(pos_x_ * 100.0) / 100.0},
        {"y",           std::round(pos_y_ * 100.0) / 100.0},
        {"speed",       std::round(speed_ * 100.0) / 100.0},
        {"heading",     std::round(wrapPi(heading_) * 1000.0) / 1000.0},
        {"motorTemp",   std::round(motor_temp_ * 10.0) / 10.0},
        {"signal",      signal_},
        {"uptimeH",     std::round((now() - start_time_).seconds() / 36.0) / 100.0},
        {"currentTask", task_.empty() ? json(nullptr) : json(task_)},
        {"cam",         0},
      };
      connected_snapshot_ = mqtt_connected_;
      odom_snapshot_ = odom_ok_;
    }

    if (!connected_snapshot_) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
        "MQTT 未连接，本帧丢弃（等待自动重连）");
      return;
    }
    if (!odom_snapshot_) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "尚未收到 /odom，上报数据可能为空");
    }

    const std::string payload = state.dump();
    // QoS1 保证至少到达一次；retain 让后连的看板立刻拿到最新状态
    int rc = mosquitto_publish(mosq_, nullptr, state_topic_.c_str(),
      static_cast<int>(payload.size()), payload.c_str(), 1, true);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
        "publish 失败: %s", mosquitto_strerror(rc));
    }

    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 10000,
      "上报: status=%s battery=%.1f%% pos=(%.2f,%.2f) speed=%.2f",
      status_.c_str(), battery_, pos_x_, pos_y_, speed_);
  }

  // ---------------- 成员变量 ----------------
  // 参数
  std::string broker_host_, robot_id_, robot_name_, robot_model_;
  std::string topic_prefix_, username_, password_, state_topic_, cmd_topic_;
  int broker_port_ = 1883;
  double publish_hz_ = 1.0, battery_ = 100.0;
  int keepalive_ = 45;

  // MQTT
  struct mosquitto *mosq_ = nullptr;

  // ROS
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr sub_odom_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr sub_joint_;
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr pub_cmd_vel_;
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::Time start_time_;

  // 共享状态（受 data_mutex_ 保护）
  std::mutex data_mutex_;
  double pos_x_ = 0.0, pos_y_ = 0.0, heading_ = 0.0, speed_ = 0.0;
  double joint_load_ = 0.0, motor_temp_ = 38.0;
  int signal_ = -55;
  std::string status_ = "idle", task_;
  bool mqtt_connected_ = false, odom_ok_ = false;
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
