// ===========================================================================
// node_telemetry.cpp —— mqtt_bridge 的遥测与告警：1Hz 状态上行、电量/温度告警、
// 事件冷却。从原单进程节点搬过来，行为与报文字段一字未改；
// 唯一变化：status/currentTask 里的"任务信息"来自 task_runner/state（见 node_core.cpp）
// ===========================================================================

#include "quadruped_mqtt_bridge/mqtt_bridge_node.hpp"


  // =========================================================================
  // 3.5 告警上报 {P}/event
  //
  // 报文（见 MQTT-INTERFACE.md 5.2 节）：
  //   { "alarmId": 1042, "ts": 1789008000000, "level": "critical",
  //     "source": "仿真狗", "message": "..." }
  // level 取 info / warning / critical；alarmId 由上报方分配且全局递增。
  // 没有这条上行通道时，看板的告警中心在真实链路下永远是空的
  // （只有 mock 模式的设备模拟器会自己造告警）。
  //
  // 【多发布者约定】event 主题允许 mqtt_bridge（电量/温度）与 task_runner
  // （急停/导航失败/任务完成）共同发布；两边用同一基址的不同号段，不会撞 alarmId。
  // =========================================================================
  void MqttBridgeNode::publishEvent(const char * level, const std::string &message)
  {
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      if (!mqtt_connected_) return;   // 断线时不上报，避免重连后灌一堆旧告警
    }

    json e = {
      {"alarmId", alarm_base_id_ + (++alarm_seq_)},
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


  // 同类告警冷却：同一个 key 在 cooldown_s 内只报一次，避免每秒刷屏
  bool MqttBridgeNode::eventAllowed(const std::string &key, double cooldown_s)
  {
    const auto t = now();
    std::lock_guard<std::mutex> lk(data_mutex_);
    auto it = event_last_.find(key);
    if (it != event_last_.end() && (t - it->second).seconds() < cooldown_s) return false;
    event_last_[key] = t;
    return true;
  }


  // =========================================================================
  // 3.5 阈值告警检查（挂在 1 Hz 状态定时器上）
  // =========================================================================
  void MqttBridgeNode::checkAlarms()
  {
    {
      std::lock_guard<std::mutex> lk(data_mutex_);
      if (!mqtt_connected_) return;
    }

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
  // 状态上报（1 Hz）
  // =========================================================================
  void MqttBridgeNode::onPublishTimer()
  {
    // 执行端心跳丢失 → 立刻告警（机器人可能仍在执行，但已无人监管）
    if (heartbeat_seen_ && (now() - last_heartbeat_).seconds() > 4.0 &&
      eventAllowed("heartbeat_lost", 60.0)) {
      publishEvent("critical", "任务执行端（task_runner）心跳丢失超 4s，机器人执行已失去监管");
    }
    json state;
    // 日志快照（避免在锁外读共享成员，同时保留"不持锁打日志"的原意）
    std::string log_status;
    double log_bat = 0.0, log_x = 0.0, log_y = 0.0, log_speed = 0.0;
    bool log_nav = false;
    {
      std::lock_guard<std::mutex> lk(data_mutex_);

      // 电量：行走耗电快、待机慢（真实机器人应从 BMS 读取）
      const bool moving = speed_ > 0.02;
      const double drain_moving = 0.05;   // %/s
      const double drain_idle   = 0.005;  // %/s
      battery_ = std::max(0.0, battery_ - (moving ? drain_moving : drain_idle) / publish_hz_);

      // 状态机：fault > 有任务 > 运动中 > 待机
      // fault 与 task_/navigating_ 现在都由 task_runner 经 task_runner/state 告知，
      // 判定规则与拆分前完全一致。
      if (remote_status_ == "fault") status_ = "fault";
      else if (!task_.empty() || navigating_) status_ = "working";
      else                                status_ = moving ? "working" : "idle";

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
        {"heading",   std::round(qmb::wrapPi(-heading_) * 1000.0) / 1000.0},
        {"motorTemp", std::round(motor_temp_ * 10.0) / 10.0},
        {"signal",    signal_},
        {"uptimeH",   std::round((now() - start_time_).seconds() / 36.0) / 100.0},
        {"currentTask", task_.empty() ? json(nullptr) : json(task_)},
        {"cam",       0},
      };
      connected_snapshot_ = mqtt_connected_;
      odom_snapshot_ = odom_ok_;

      log_status = status_;
      log_bat = battery_; log_x = pos_x_; log_y = pos_y_;
      log_speed = speed_; log_nav = navigating_;
    }

    if (!connected_snapshot_) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "MQTT 未连接，本帧丢弃");
      return;
    }
    if (!odom_snapshot_) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000, "尚未收到 /odom");
    }

    const std::string payload = state.dump();
    int rc = qmb::publishText(mosq_, state_topic_, payload, 1, /*retain=*/true);
    if (rc != MOSQ_ERR_SUCCESS && rc != MOSQ_ERR_NO_CONN) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
        "publish 失败: %s", mosquitto_strerror(rc));
    }

    RCLCPP_INFO_THROTTLE(get_logger(), *get_clock(), 10000,
      "上报: %s 电量 %.1f%% 仿真位姿 (%.2f,%.2f) 速度 %.2f%s",
      log_status.c_str(), log_bat, log_x, log_y, log_speed,
      log_nav ? " [前往目标中]" : "");

    // 3.5 阈值告警：复用 1 Hz 状态节拍检查电量与电机温度
    checkAlarms();
  }
