// ===========================================================================
// node_core.cpp —— mqtt_bridge 构造 / 析构：参数声明、话题与定时器装配
//
// 拆分后本文件只剩"遥测节点"的装配：/odom、/joint_states、task_runner/state
// 三个订阅 + 1 Hz 状态定时器。Nav2 动作客户端、cmd_vel、语音全部搬走了。
// 类声明见 mqtt_bridge_node.hpp
// ===========================================================================

#include "quadruped_mqtt_bridge/mqtt_bridge_node.hpp"

MqttBridgeNode::MqttBridgeNode() : rclcpp::Node("mqtt_bridge")
{
  // ---------------- 参数 ----------------
  // 默认走本机：看板 server.js 与 mosquitto broker 都跑在同一台机器上，
  // 两者必须连同一个 broker 才能互通。这里若写成 192.168.137.1（主机 ICS
  // 网卡地址）而看板连 127.0.0.1，就会出现"看板派单了但机器人收不到"的
  // 假故障，排查成本极高，故默认值统一为 127.0.0.1。
  // 【参数名一律不变】三个节点各连各的 broker 连接，这组参数三边同名同义。
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

  // 坐标缩放：仿真世界 → 看板地图（只看板契约，归本节点）
  site_scale_    = declare_parameter("site_scale", 20.0);
  site_offset_x_ = declare_parameter("site_offset_x", 100.0);
  site_offset_y_ = declare_parameter("site_offset_y", 50.0);
  site_world_w_  = declare_parameter("site_world_width", 20.0);    // 仿真世界宽 m
  site_world_h_  = declare_parameter("site_world_height", 14.0);   // 仿真世界高 m
  publish_map_   = declare_parameter("publish_site_map", true);

  // ---- 3.5 告警阈值与 alarmId 分配 ----
  // alarmId 约定"上报方分配、全局递增"。看板 mock 模式的种子告警用 1000 附近
  // 的小号，这里从 100 万起，避免两边撞号。task_runner 用同一基址 + 10 万段的
  // 偏移，两个节点的 alarmId 不会撞（见 task_runner_node.cpp）。
  alarm_base_id_ = declare_parameter("alarm_base_id", 1000000);
  battery_warn_  = declare_parameter("battery_warn", 20.0);      // % 低于即预警
  battery_crit_  = declare_parameter("battery_crit", 10.0);      // % 低于即严重
  temp_warn_     = declare_parameter("motor_temp_warn", 75.0);   // °C
  temp_crit_     = declare_parameter("motor_temp_crit", 85.0);   // °C

  // MQTT 主题：本节点【只写】这三个（state / event / site/map），无订阅
  state_topic_ = topic_prefix_ + "/robots/" + robot_id_ + "/state";
  map_topic_   = topic_prefix_ + "/site/map";
  event_topic_ = topic_prefix_ + "/event";

  start_time_ = now();

  // ---------------- ROS 订阅 ----------------
  sub_odom_ = create_subscription<nav_msgs::msg::Odometry>(
    "odom", rclcpp::SensorDataQoS(),
    [this](nav_msgs::msg::Odometry::SharedPtr msg) {
      std::lock_guard<std::mutex> lk(data_mutex_);
      pos_x_ = msg->pose.pose.position.x;
      pos_y_ = msg->pose.pose.position.y;
      heading_ = qmb::yawFromQuat(
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

  // 任务状态：拆节点后任务归属 task_runner，但 state 报文的 status/currentTask
  // 必须照样反映"正在执行什么"。这是拆分后唯一的跨节点内部话题（不是 MQTT 契约）。
  // 用 transient_local（latched）：mqtt_bridge 比 task_runner 晚起也能拿到上一帧。
  sub_task_state_ = create_subscription<std_msgs::msg::String>(
    "task_runner/state", rclcpp::QoS(rclcpp::KeepLast(1)).transient_local().reliable(),
    [this](std_msgs::msg::String::SharedPtr msg) {
      try {
        const json j = json::parse(msg->data);
        std::lock_guard<std::mutex> lk(data_mutex_);
        remote_status_ = j.value("status", "idle");
        task_          = j.value("task", "");
        navigating_    = j.value("navigating", false);
      } catch (const std::exception &) { /* 报文损坏就沿用上一帧 */ }
    });

  // ---------------- MQTT ----------------
  initMqtt();

  // ---------------- 定时器：1 Hz 状态上报 ----------------
  const auto state_period = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::duration<double>(1.0 / std::max(0.1, publish_hz_)));
  timer_state_ = create_wall_timer(state_period, [this]() { onPublishTimer(); });

  RCLCPP_INFO(get_logger(),
    "MQTT 遥测节点已启动（只写不读）\n"
    "  Broker     : %s:%d\n"
    "  机器人     : %s (%s)\n"
    "  状态主题   : %s @ %.1f Hz\n"
    "  告警主题   : %s\n"
    "  地图主题   : %s（缩放 %.1f×，世界 %.0f×%.0f m）\n"
    "  任务来源   : task_runner/state（ROS 内部话题）\n"
    "  遗嘱消息   : 已注册（本节点独占）",
    broker_host_.c_str(), broker_port_,
    robot_id_.c_str(), robot_name_.c_str(),
    state_topic_.c_str(), publish_hz_, event_topic_.c_str(),
    map_topic_.c_str(), site_scale_, site_world_w_, site_world_h_);
}

MqttBridgeNode::~MqttBridgeNode()
{
  if (mosq_) {
    mosquitto_disconnect(mosq_);        // 正常断开 → 不触发遗嘱
    mosquitto_loop_stop(mosq_, false);
    mosquitto_destroy(mosq_);
    mosq_ = nullptr;
  }
  mosquitto_lib_cleanup();
}
