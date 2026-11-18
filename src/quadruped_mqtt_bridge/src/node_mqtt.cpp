#include <chrono>
// ===========================================================================
// node_mqtt.cpp —— mqtt_bridge 的 MQTT 传输层：连接 / 回调 / 遗嘱消息 / site map
//
// 拆分后本节点【没有任何 MQTT 订阅】：cmd 归 task_runner、tts 归 voice_link。
// 只有 mqtt_bridge 注册遗嘱消息（LWT = state 主题 status=offline）——
// 三个连接都带遗嘱的话，任何一个断开都会让看板误判整机掉线。
// ===========================================================================

#include "quadruped_mqtt_bridge/mqtt_bridge_node.hpp"

// =========================================================================
// MQTT 初始化与回调
// =========================================================================
void MqttBridgeNode::initMqtt()
{
  mosquitto_lib_init();
  // 客户端 ID 仍用 robot_id：看板/运维按机器人身份看连接，语义不变。
  // （task_runner / voice_link 各自带后缀，见各自 initMqtt 的注释）
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

  mosquitto_connect_callback_set(mosq_, &MqttBridgeNode::onConnectCb);
  mosquitto_disconnect_callback_set(mosq_, &MqttBridgeNode::onDisconnectCb);

  // 重连退避：2s 起、指数增长、封顶 10s（官方 API，不干扰库状态机）
  mosquitto_reconnect_delay_set(mosq_, 2, 10, true);
  int rc = mosquitto_connect_async(mosq_, broker_host_.c_str(), broker_port_, keepalive_);
  if (rc != MOSQ_ERR_SUCCESS) {
    RCLCPP_ERROR(get_logger(), "connect_async 失败: %s", mosquitto_strerror(rc));
    return;
  }
  mosquitto_loop_start(mosq_);   // 独立网络线程，内部自动重连
  // 重连策略交给 libmosquitto：mosquitto_reconnect_delay_set()
  // （退避 2s 起、封顶 10s）。手写看门狗会干扰库的连接状态机：
  // 实测 disconnect+reconnect 连发会让服务器判协议错误踢连接，
  // 形成 3s 抖动循环；只 reconnect 又会受内核 SYN 退避影响。
}

void MqttBridgeNode::onConnectCb(struct mosquitto * /*mosq*/, void *obj, int rc)
{
  auto *self = static_cast<MqttBridgeNode *>(obj);
  if (rc != 0) {
    RCLCPP_WARN(self->get_logger(), "MQTT 连接被拒绝: %s", mosquitto_connack_string(rc));
    return;
  }
  {
    std::lock_guard<std::mutex> lk(self->data_mutex_);
    self->mqtt_connected_ = true;
  }
  RCLCPP_INFO(self->get_logger(), "MQTT 已连接 %s:%d（遥测节点，无订阅）",
    self->broker_host_.c_str(), self->broker_port_);

  // 连接成功后把站点地图发布一次（retained），看板据此渲染厂区图。
  // 上线即可见，不需要等第一次状态上报。
  if (self->publish_map_) self->publishSiteMap();
}

void MqttBridgeNode::onDisconnectCb(struct mosquitto * /*mosq*/, void *obj, int rc)
{
  auto *self = static_cast<MqttBridgeNode *>(obj);
  {
    std::lock_guard<std::mutex> lk(self->data_mutex_);
    self->mqtt_connected_ = false;
  }
  if (rc == 0) RCLCPP_INFO(self->get_logger(), "MQTT 正常断开");
  else RCLCPP_WARN(self->get_logger(), "MQTT 意外断开 (rc=%d)，自动重连中", rc);
}


// =========================================================================
// 站点地图发布（retained，QoS1）
// =========================================================================
void MqttBridgeNode::publishSiteMap()
{
  json zones = json::array();
  for (const auto &z : qmb::kZones) {
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
  for (const auto &p : qmb::kPoints) {
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
    qmb::kZones.size(), qmb::kPoints.size());
}
