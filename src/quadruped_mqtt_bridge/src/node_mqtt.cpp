// ===========================================================================
// node_mqtt.cpp —— MQTT 传输：mosquitto 连接 / 回调 / 遗嘱消息 / site map 发布
// 从原 mqtt_bridge.cpp 拆出，行为不变；类声明见 mqtt_bridge_node.hpp
// ===========================================================================

#include "quadruped_mqtt_bridge/mqtt_bridge_node.hpp"


  // =========================================================================
  // MQTT 初始化与回调
  // =========================================================================
  void MqttBridge::initMqtt() 
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

  void MqttBridge::onConnectCb(struct mosquitto * /*mosq*/, void *obj, int rc) 
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

  void MqttBridge::onDisconnectCb(struct mosquitto * /*mosq*/, void *obj, int rc) 
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
  void MqttBridge::publishSiteMap() 
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
  void MqttBridge::onMessageCb(struct mosquitto * /*mosq*/, void *obj, const struct mosquitto_message *msg) 
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
