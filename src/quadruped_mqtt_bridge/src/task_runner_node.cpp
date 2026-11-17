// ===========================================================================
// task_runner_node.cpp —— 构造/析构、MQTT 连接、下行指令分发、事件上报
//
// 拆分前这些代码混在 mqtt_bridge 的 node_core.cpp / node_mqtt.cpp 里，
// 与遥测、语音共用一个类。现在 task_runner 独立连 broker（客户端 ID 带
// "-task" 后缀，避免与 mqtt_bridge 撞号被 Broker 互踢），只订阅 cmd 与
// fleet/tasks，只发布 fleet/tasks（回写）与 event。
// ===========================================================================

#include "quadruped_mqtt_bridge/task_runner_node.hpp"

TaskRunnerNode::TaskRunnerNode() : rclcpp::Node("task_runner")
{
  // ---------------- 参数（名字与默认值一律不变） ----------------
  broker_host_   = declare_parameter("broker_host", "127.0.0.1");
  broker_port_   = declare_parameter("broker_port", 1883);
  robot_id_      = declare_parameter("robot_id", "robot-01");
  robot_name_    = declare_parameter("robot_name", "仿真狗");
  topic_prefix_  = declare_parameter("topic_prefix", "qbot/v1");
  keepalive_     = declare_parameter("keepalive", 45);
  username_      = declare_parameter("username", "");
  password_      = declare_parameter("password", "");

  // 告警号段（与 mqtt_bridge 共用基址，偏移见头文件说明）
  alarm_base_id_ = declare_parameter("alarm_base_id", 1000000);

  // 前往目标点的行驶参数
  nav_speed_     = declare_parameter("nav_speed", 0.25);    // m/s
  nav_tolerance_ = declare_parameter("nav_tolerance", 0.30); // m，到达判定
  nav_timeout_s_ = declare_parameter("nav_timeout", 120.0);  // s

  cmd_topic_         = topic_prefix_ + "/robots/" + robot_id_ + "/cmd";
  fleet_tasks_topic_ = topic_prefix_ + "/fleet/tasks";
  event_topic_       = topic_prefix_ + "/event";

  start_time_ = now();

  // ---------------- ROS 订阅：位姿（超时/初始位姿）+ 巡逻进度 ----------------
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

  // 巡逻节点进度：/patrol/status 的 JSON 透传到看板 currentTask，
  // 让前端能看到「巡逻到第几个航点/重试中/已跳过」
  sub_patrol_ = create_subscription<std_msgs::msg::String>(
    "patrol/status", rclcpp::SensorDataQoS(),
    [this](std_msgs::msg::String::SharedPtr msg) {
      {
        std::lock_guard<std::mutex> lk(data_mutex_);
        patrol_status_ = msg->data;
        // 派单期间不要用巡逻进度覆盖任务标题（派单时巡逻已暂停）
        if (!navigating_) task_ = "巡逻: " + patrol_status_;
      }
      publishTaskState();
    });

  // ---------------- ROS 发布 ----------------
  pub_cmd_vel_ = create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);

  // 初始位姿修正：AMCL 的 initial_pose 参数是固定值，但机器人每次停下位置
  // 都不同（上一轮任务结束在哪、世界是否复用），固定值会让粒子滤波收敛到
  // 错误位置（实测偏 2 m，代价图与真实环境错位 → 规划贴墙顶死）。
  // 因此启动后拿第一条 /odom 的真实位姿，主动发一次 /initialpose 对齐。
  pub_initial_pose_ = create_publisher<geometry_msgs::msg::PoseWithCovarianceStamped>(
    "initialpose", 10);

  // 派单与巡逻互斥：派单时暂停巡逻，任务结束后恢复
  pub_patrol_enable_ = create_publisher<std_msgs::msg::Bool>("/patrol/enable", 10);

  // 任务状态 → mqtt_bridge（state 报文的 status/currentTask 来源）。
  // latched：mqtt_bridge 晚起也能拿到上一帧，不会一开机就显示 idle。
  pub_task_state_ = create_publisher<std_msgs::msg::String>(
    "task_runner/state", rclcpp::QoS(rclcpp::KeepLast(1)).transient_local().reliable());

  // 派单导航走 Nav2 动作接口（规划 + 避障 + 可取消）
  nav_client_ = rclcpp_action::create_client<NavigateToPose>(this, "navigate_to_pose");

  // ---------------- MQTT ----------------
  initMqtt();

  // ---------------- 定时器：20 Hz 任务维护（初始位姿 / 超时兜底） ----------------
  timer_nav_ = create_wall_timer(50ms, [this]() { onNavTimer(); });

  // 开机先发一帧 idle，让 mqtt_bridge 立刻有任务状态可用
  publishTaskState();

  RCLCPP_INFO(get_logger(),
    "MQTT 任务节点已启动\n"
    "  Broker     : %s:%d\n"
    "  机器人     : %s (%s)\n"
    "  指令主题   : %s（estop/resume/assign/cancel）\n"
    "  任务主题   : %s（订阅 + 回写）\n"
    "  告警主题   : %s（多发布者之一）\n"
    "  导航参数   : 速度 %.2f m/s 容差 %.2f m 超时 %.0f s\n"
    "  遗嘱消息   : 不注册（由 mqtt_bridge 独占）",
    broker_host_.c_str(), broker_port_,
    robot_id_.c_str(), robot_name_.c_str(),
    cmd_topic_.c_str(), fleet_tasks_topic_.c_str(), event_topic_.c_str(),
    nav_speed_, nav_tolerance_, nav_timeout_s_);
}

TaskRunnerNode::~TaskRunnerNode()
{
  if (mosq_) {
    mosquitto_disconnect(mosq_);        // 正常断开（本节点无遗嘱）
    mosquitto_loop_stop(mosq_, false);
    mosquitto_destroy(mosq_);
    mosq_ = nullptr;
  }
  mosquitto_lib_cleanup();
}


// =========================================================================
// MQTT 初始化与回调
// =========================================================================
void TaskRunnerNode::initMqtt()
{
  mosquitto_lib_init();
  // 客户端 ID 必须与 mqtt_bridge 不同：同 ID 的两个连接会被 Broker 互相顶掉，
  // 表现为"遥测时有时无、指令偶发收不到"这种极难排查的假故障。
  const std::string client_id = robot_id_ + "-task";
  mosq_ = mosquitto_new(client_id.c_str(), true, this);
  if (!mosq_) {
    RCLCPP_ERROR(get_logger(), "mosquitto_new 失败");
    return;
  }

  // 本节点【不注册遗嘱消息】：LWT 只归 mqtt_bridge，否则任一副节点重启都会
  // 让看板把整台机器人误判成 offline。

  if (!username_.empty())
    mosquitto_username_pw_set(mosq_, username_.c_str(), password_.c_str());

  mosquitto_connect_callback_set(mosq_, &TaskRunnerNode::onConnectCb);
  mosquitto_message_callback_set(mosq_, &TaskRunnerNode::onMessageCb);
  mosquitto_disconnect_callback_set(mosq_, &TaskRunnerNode::onDisconnectCb);

  int rc = mosquitto_connect_async(mosq_, broker_host_.c_str(), broker_port_, keepalive_);
  if (rc != MOSQ_ERR_SUCCESS) {
    RCLCPP_ERROR(get_logger(), "connect_async 失败: %s", mosquitto_strerror(rc));
    return;
  }
  mosquitto_loop_start(mosq_);   // 独立网络线程，内部自动重连
}

void TaskRunnerNode::onConnectCb(struct mosquitto * /*mosq*/, void *obj, int rc)
{
  auto *self = static_cast<TaskRunnerNode *>(obj);
  if (rc != 0) {
    RCLCPP_WARN(self->get_logger(), "MQTT 连接被拒绝: %s", mosquitto_connack_string(rc));
    return;
  }
  {
    std::lock_guard<std::mutex> lk(self->data_mutex_);
    self->mqtt_connected_ = true;
  }
  RCLCPP_INFO(self->get_logger(), "MQTT 已连接 %s:%d（任务节点）",
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

  // 重连后补一帧任务状态，避免 mqtt_bridge 一直显示重连前的旧状态
  self->publishTaskState();
}

void TaskRunnerNode::onDisconnectCb(struct mosquitto * /*mosq*/, void *obj, int rc)
{
  auto *self = static_cast<TaskRunnerNode *>(obj);
  {
    std::lock_guard<std::mutex> lk(self->data_mutex_);
    self->mqtt_connected_ = false;
  }
  if (rc == 0) RCLCPP_INFO(self->get_logger(), "MQTT 正常断开");
  else RCLCPP_WARN(self->get_logger(), "MQTT 意外断开 (rc=%d)，自动重连中", rc);
}


// =========================================================================
// 下行报文入口（运行在 MQTT 网络线程）：只认自己的两个主题
// =========================================================================
void TaskRunnerNode::onMessageCb(struct mosquitto * /*mosq*/, void *obj,
                                 const struct mosquitto_message *msg)
{
  if (!msg || !msg->payload || msg->payloadlen <= 0) return;
  auto *self = static_cast<TaskRunnerNode *>(obj);
  const std::string text(static_cast<const char *>(msg->payload), msg->payloadlen);
  const std::string topic = msg->topic ? msg->topic : "";

  // ---- 任务列表快照（3.5 回写的底稿）----
  // 看板每次增删改任务都会 retained 发布全量数组，这里存一份最新版本。
  if (topic == self->fleet_tasks_topic_) {
    self->onFleetTasks(text);
    return;
  }

  // ---- 指令下发 ----
  if (topic == self->cmd_topic_) {
    self->handleCmd(text);
    return;
  }

  // 拆分后每个节点只订阅自己的主题；真收到别的主题只可能是通配符笔误，
  // 记一条 DEBUG 即可（不再当成"未知指令"告警刷日志）。
  RCLCPP_DEBUG(self->get_logger(), "task_runner 忽略未知主题 %s", topic.c_str());
}


// =========================================================================
// 指令分发：estop / resume / cancel / assign
// =========================================================================
void TaskRunnerNode::handleCmd(const std::string &text)
{
  json j;
  try {
    j = json::parse(text);
  } catch (const std::exception &e) {
    RCLCPP_WARN(get_logger(), "指令解析失败: %s", e.what());
    return;
  }

  const std::string action = j.value("action", "");
  RCLCPP_INFO(get_logger(), "收到看板指令: %s", text.c_str());

  // ---- 急停 ----
  if (action == "estop") {
    // 先刹车再撤目标：顺序反了的话 Nav2 会在撤销动画里再发一帧速度
    pub_cmd_vel_->publish(geometry_msgs::msg::Twist());
    // 急停必须同时撤销导航目标，否则 Nav2 会立刻重新下发速度指令
    cancelNavigation("急停");
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      status_ = "fault";
      task_.clear();
    }
    publishTaskState();     // 让 state 报文立刻变 fault
    RCLCPP_WARN(get_logger(), "执行紧急停止 (ESTOP)：已向 /cmd_vel 发布零速并撤销导航目标");
    publishEvent("critical", "收到看板紧急停止指令，已撤销导航目标并停车");
    return;
  }

  // ---- 取消任务（看板"取消"按钮）----
  // 早期版本没有这个分支，看板点取消只会得到"未知指令"，导航目标继续
  // 执行导致机器人顶着墙一直推。必须有。
  if (action == "cancel") {
    cancelNavigation("看板取消");
    return;
  }

  // ---- 复位 ----
  if (action == "resume") {
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      status_ = "idle";
      navigating_ = false;
      task_.clear();
    }
    publishTaskState();
    pausePatrol(false);
    RCLCPP_INFO(get_logger(), "故障已复位（status=idle，巡逻已恢复）");
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
    startNavigation(target_id, title, task_id, task_obj);
    return;
  }

  RCLCPP_WARN(get_logger(), "未知指令 action='%s'", action.c_str());
}


// =========================================================================
// 事件上报 {P}/event（多发布者之一：急停 / 导航失败 / 任务完成）
// =========================================================================
void TaskRunnerNode::publishEvent(const char * level, const std::string &message)
{
  {
    std::lock_guard<std::mutex> lk(data_mutex_);
    if (!mqtt_connected_) return;   // 断线时不上报，避免重连后灌一堆旧告警
  }

  json e = {
    // 与 mqtt_bridge 共用基址但偏移 10 万，两个节点的 alarmId 不会撞号
    {"alarmId", alarm_base_id_ + 100000 + (++alarm_seq_)},
    {"ts",      qmb::wallClockMs()},
    {"level",   level},
    {"source",  robot_name_},
    {"message", message},
  };
  const std::string payload = e.dump();
  int rc = qmb::publishText(mosq_, event_topic_, payload, 1, false);
  if (rc == MOSQ_ERR_SUCCESS) {
    RCLCPP_WARN(get_logger(), "已上报告警[%s] %s", level, message.c_str());
  } else {
    RCLCPP_WARN(get_logger(), "告警上报失败: %s", mosquitto_strerror(rc));
  }
}


// =========================================================================
// 任务状态广播：mqtt_bridge 用它填 state 报文的 status / currentTask
// =========================================================================
void TaskRunnerNode::publishTaskState()
{
  json j;
  {
    std::lock_guard<std::mutex> lk(data_mutex_);
    const bool busy = !task_.empty() || navigating_;
    j = {
      // fault 优先级最高，其余由"有没有任务/在不在动"推出来
      {"status",     status_ == "fault" ? "fault" : (busy ? "working" : "idle")},
      {"task",       task_},
      {"navigating", navigating_},
    };
  }
  std_msgs::msg::String m;
  m.data = j.dump();
  pub_task_state_->publish(m);
}
