// ===========================================================================
// node_core.cpp —— 构造 / 析构：参数声明、话题与定时器装配、Nav2 动作客户端
// 从原 mqtt_bridge.cpp 拆出，行为不变；类声明见 mqtt_bridge_node.hpp
// ===========================================================================

#include "quadruped_mqtt_bridge/mqtt_bridge_node.hpp"

MqttBridge::MqttBridge() : rclcpp::Node("mqtt_bridge") 
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

  MqttBridge::~MqttBridge() 
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
