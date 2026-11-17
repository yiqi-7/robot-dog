#pragma once
// ===========================================================================
// mqtt_bridge_node.hpp —— 节点①mqtt_bridge（MqttBridgeNode）类声明
//
// 【单一职责】本节点只管"把机器人现在的样子告诉看板"，不做任何业务决策：
//   · 连接管理：唯一带遗嘱消息（LWT = state 主题 status=offline）的节点，
//     机器人掉线/断电时看板秒级感知，这条语义必须由一个"长期活着"的
//     遥测节点独占，否则三个连接会互相顶掉遗嘱。
//   · 1 Hz 状态上行：onPublishTimer()
//   · 阈值告警：checkAlarms()（电量/电机温度）+ eventAllowed() 同类冷却
//   · 站点地图：publishSiteMap()，连接成功后 retained 发一次
//
// 【MQTT 只写不读】state、event、site/map —— 本节点没有任何 MQTT 订阅，
//   所有下行指令都由 task_runner 处理，语音由 voice_link 处理。
//   （event 主题允许多个节点共同发布，见 README 的"多发布者"约定。）
//
// 【ROS 侧】订阅 /odom、/joint_states 取本体状态；另订阅 task_runner/state
//   拿"当前在执行什么任务"（拆节点后唯一新增的内部话题：state 报文里的
//   status/currentTask 依赖任务信息，但任务归属 task_runner，只能跨节点传）。
//
// 实现文件：
//   node_core.cpp       构造/析构：参数、话题、定时器装配
//   node_mqtt.cpp       MQTT 传输：连接/回调/遗嘱 + 站点地图发布
//   node_telemetry.cpp  遥测与告警：1Hz 状态上行、电量温度告警、事件冷却
// ===========================================================================
// ============================================================================
// 坐标系说明（重要）
// ---------------------------------------------------------------------------
// 仿真世界是 20×14 m 的小厂区（墙在 x=±10, y=±7），而看板厂区图按 600×380 m
// 的米制坐标渲染（1 px = 0.5 m）。若直接上报仿真坐标，机器人在看板上只是角落里
// 一个点。因此本节点做一次【线性缩放 + 平移】，把仿真世界铺满看板地图：
//
//     dash_x = (sim_x + 10) * S + OFFSET_X        S = 20 (1 仿真米 = 20 看板米)
//     dash_y = (sim_y +  7) * S + OFFSET_Y        仿真世界 → 看板 x∈[100,500], y∈[50,330]
//
// 缩放系数与偏移都是参数，改参数即可适配别的世界尺寸。
// （反算公式 dashToSimX/Y 保留在这里：地图契约归本节点，task_runner 导航直接用
//   site_map.hpp 里的仿真坐标，不需要换算。）
//
// ---------------------------------------------------------------------------
// 数据流
// ---------------------------------------------------------------------------
//   上行  /odom + /joint_states → {P}/robots/{id}/state   (1 Hz, QoS1, retained)
//   任务  task_runner/state     → 合进 state 的 status / currentTask
//   地图  {P}/site/map          → 一次性 retained 发布（连接成功后）
//   告警  {P}/event             → 电量 / 电机温度事件上报
//
// ---------------------------------------------------------------------------
// 线程模型
// ---------------------------------------------------------------------------
//   libmosquitto 自己的网络线程负责收发，ROS executor 线程负责定时与订阅。
//   两者共享状态必须用 data_mutex_ 保护。
// ===========================================================================

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
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <thread>

#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/string.hpp>

#include "quadruped_mqtt_bridge/common.hpp"
#include "quadruped_mqtt_bridge/site_map.hpp"

using namespace std::chrono_literals;

class MqttBridgeNode : public rclcpp::Node
{
public:

  MqttBridgeNode();
  ~MqttBridgeNode() override;

  // ---------------- 坐标换算（看板 ↔ 仿真世界，公式见文件头） ----------------
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

  // 反算：看板目标点 → 仿真坐标。看板的点/区域都来自本节点发布的 site/map，
  // 换算公式因此只有本节点需要知道；保留 public 便于以后加"按看板坐标派单"。
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

  // ---------------- 遥测与告警：1Hz 状态上行 / 电量温度告警 / 事件冷却 ----------------
  void publishEvent(const char * level, const std::string &message);
  bool eventAllowed(const std::string &key, double cooldown_s);
  void checkAlarms();
  void onPublishTimer();

  // ---------------- 成员变量 ----------------

  // 连接与身份（三个节点共有的一组，参数名不变）
  std::string broker_host_, robot_id_, robot_name_, robot_model_;
  std::string topic_prefix_, username_, password_;
  std::string state_topic_, map_topic_;
  std::string event_topic_;           // 多节点共同发布（见 README）
  int broker_port_ = 1883, keepalive_ = 45;
  double publish_hz_ = 1.0, battery_ = 100.0;

  // 告警阈值与 alarmId 分配器（阈值只影响本节点的遥测告警）
  long long alarm_base_id_ = 1000000, alarm_seq_ = 0;
  double battery_warn_ = 20.0, battery_crit_ = 10.0;
  double temp_warn_ = 75.0, temp_crit_ = 85.0;
  bool battery_warn_reported_ = false, battery_crit_reported_ = false;
  std::map<std::string, rclcpp::Time> event_last_;  // 告警冷却表（受 data_mutex_ 保护）

  // 站点地图缩放参数（看板契约）
  double site_scale_ = 20.0, site_offset_x_ = 100.0, site_offset_y_ = 50.0;
  double site_world_w_ = 20.0, site_world_h_ = 14.0;
  bool publish_map_ = true;

  struct mosquitto *mosq_ = nullptr;

  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr sub_odom_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr sub_joint_;
  // task_runner 的任务状态（status/currentTask 跨节点来源）
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_task_state_;
  rclcpp::TimerBase::SharedPtr timer_state_;
  rclcpp::Time start_time_;

  // 共享状态（受 data_mutex_ 保护）
  std::mutex data_mutex_;
  double pos_x_ = 0.0, pos_y_ = 0.0, heading_ = 0.0, speed_ = 0.0;
  double joint_load_ = 0.0, motor_temp_ = 38.0;
  int signal_ = -55;
  std::string status_ = "idle", task_;
  bool navigating_ = false;                    // 来自 task_runner（只为日志与状态机）
  std::string remote_status_ = "idle";         // 来自 task_runner：idle/working/fault
  bool mqtt_connected_ = false, odom_ok_ = false;

  // 定时器用的快照（避免持锁跨线程调用日志宏）
  bool connected_snapshot_ = false, odom_snapshot_ = false;
};
