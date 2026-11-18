#pragma once
// ===========================================================================
// task_runner_node.hpp —— 节点②task_runner（TaskRunnerNode）类声明
//
// 【单一职责】本节点是"机器人怎么动"的唯一决策者：
//   · MQTT 下行分发：estop / resume / assign / cancel（cmd 主题）
//   · 任务列表：订阅 fleet/tasks 全量快照，任务结束【回写】status（retained）
//   · Nav2 NavigateToPose 动作客户端：派单 → 规划避障 → 到位/失败回调
//   · 急停：直接发 /cmd_vel 零速 + 撤销导航目标（不依赖 Nav2 是否响应）
//   · 初始位姿对齐（/initialpose）与派单期间暂停巡逻（/patrol_enable）
//
// 【MQTT 订阅】cmd、fleet/tasks        【MQTT 发布】fleet/tasks（回写）、event
//   event 主题允许多节点共同发布：本节点报急停/导航失败/任务完成，
//   mqtt_bridge 报电量/电机温度。两边 alarmId 号段错开，不会撞号。
//
// 【ROS 侧】订阅 /odom（超时与初始位姿）、/patrol/status（巡逻进度→看板 currentTask）；
//   发布 /cmd_vel、/initialpose、/patrol/enable，以及 task_runner/state
//   （把 status/currentTask 告诉 mqtt_bridge，state 报文的对外字段因此保持不变）。
//
// 【没有遗嘱消息】LWT 只由 mqtt_bridge 注册（独占），否则三个连接任一个断开
//   都会让看板误判整机掉线。
//
// 实现文件：
//   task_runner_node.cpp   构造/析构：参数、话题、定时器、MQTT 连接与下行分发
//   task_runner_tasks.cpp  任务执行：Nav2 动作 / 回写 / 超时 / 初始位姿 / 巡逻暂停
// ===========================================================================
//
// ---------------------------------------------------------------------------
// 数据流
// ---------------------------------------------------------------------------
//   下发  {P}/robots/{id}/cmd   → estop / resume / assign / cancel
//   任务  {P}/fleet/tasks       → 订阅看板的全量任务快照；任务结束【回写】状态
//   告警  {P}/event             → 急停 / 导航失败 / 任务完成事件
//
// ---------------------------------------------------------------------------
// 线程模型
// ---------------------------------------------------------------------------
//   libmosquitto 自己的网络线程负责收发（onMessageCb 在这里跑），
//   ROS executor 线程负责定时器/动作回调。共享状态必须用 data_mutex_ 保护：
//   急停从 MQTT 线程进来，会同时碰导航状态与 ROS 发布器。
// ===========================================================================

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <mosquitto.h>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <geometry_msgs/msg/pose_with_covariance_stamped.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>

#include "quadruped_mqtt_bridge/common.hpp"
#include "quadruped_mqtt_bridge/site_map.hpp"

using namespace std::chrono_literals;

// Nav2 的导航动作：派单统一走它，由 Nav2 负责全局/局部规划与避障。
// 【历史教训】早期版本自己算差角直接发 cmd_vel（纯"朝目标直走"），
// 没有规划与避障，机器人在配电室墙角一头顶住墙、关节速度打满限幅值，
// 表现为"卡住不动"。改用 Nav2 后由代价地图绕开障碍。
using NavigateToPose = nav2_msgs::action::NavigateToPose;
using NavGoalHandle = rclcpp_action::ClientGoalHandle<NavigateToPose>;

class TaskRunnerNode : public rclcpp::Node
{
public:

  TaskRunnerNode();
  ~TaskRunnerNode() override;

  // ---------------- MQTT 传输 ----------------
  void initMqtt();
  static void onConnectCb(struct mosquitto * /*mosq*/, void *obj, int rc);
  static void onDisconnectCb(struct mosquitto * /*mosq*/, void *obj, int rc);
  static void onMessageCb(struct mosquitto * /*mosq*/, void *obj,
                          const struct mosquitto_message *msg);

  // ---------------- 下行指令分发（estop / resume / assign / cancel） ----------------
  void handleCmd(const std::string &text);

  // ---------------- 事件上报（event 主题的多发布者之一） ----------------
  void publishEvent(const char * level, const std::string &message);

  // ---------------- 任务执行：assign / Nav2 动作 / 状态回写 / 初始位姿 / 巡逻暂停 ----------------
  void startNavigation(const std::string &target_id, const std::string &title, int task_id,
                       const json &task_obj = json());
  void sendNavGoal(double x, double y, const std::string & name);
  enum class Outcome { Success, Canceled, Failed };

  // 结束当前任务：清状态、停车、恢复巡逻、回写任务状态
  void finishTask(const std::string & detail, Outcome outcome = Outcome::Canceled);
  void cancelNavigation(const char * reason);
  // 回写单条任务状态：按 id 合并（只改这一条，其余条目原样保留），
  // reason 非空时写入 statusNote（终态原因，看板据此显示"为什么撤销"）。
  void writeBackTask(int task_id, const char *new_status,
                     const std::string &reason = std::string());
  void onFleetTasks(const std::string &text);
  void onNavTimer();

  // ---------------- 待下发 Nav2 目标（重试） ----------------
  void enqueueNavGoal(double x, double y, const std::string & name, int task_id);
  void clearPendingGoals();
  void pumpPendingGoal();
  void publishInitialPose();
  void pausePatrol(bool paused);

  // 把 status / currentTask 广播给 mqtt_bridge（state 报文的跨节点来源）
  void publishTaskState();

  // ---------------- 成员变量 ----------------

  // 连接与身份（参数名与 mqtt_bridge 完全一致）
  std::string broker_host_, robot_id_, robot_name_;
  std::string topic_prefix_, username_, password_;
  std::string cmd_topic_, fleet_tasks_topic_, event_topic_;
  int broker_port_ = 1883, keepalive_ = 10;   // 10s：LWT 掉线判定窗口 ≈ 15s

  // 告警号段：与 mqtt_bridge 共用 alarm_base_id 基址，本节点偏移 10 万，
  // 两个节点的 alarmId 不会撞（看板按 alarmId 去重/排序）。
  long long alarm_base_id_ = 1000000, alarm_seq_ = 0;

  // 前往目标点的行驶参数（Nav2 用；nav_speed/nav_tolerance 供以后接
  // controller 参数或自行限速时使用，默认值与拆分前一致）
  double nav_speed_ = 0.25, nav_tolerance_ = 0.30, nav_timeout_s_ = 120.0;

  struct mosquitto *mosq_ = nullptr;

  // 3.5 任务列表：只保存【最新一次收到的远端表】作为合并基准，
  // 不再把它当作"可以整表覆盖发布"的本地缓存（那会删掉别人新增的任务）。
  json fleet_tasks_;                    // 最近一次收到的远端全量数组（合并基准）
  json cached_task_;                    // assign 报文里的 task 对象

  // 本机对某个任务写过的终态。rev 是写这条终态时采用的版本号（见 writeBackTask）：
  // 收到远端列表时只有"本地 rev >= 远端 rev"才补回，避免拿旧状态压掉远端新数据。
  struct TerminalStatus
  {
    std::string status;      // completed / revoked
    long long ts = 0;        // 写入时刻（epoch ms）
    long long rev = 0;       // 写入时采用的 rev
  };
  std::map<int, TerminalStatus> task_terminal_;   // id → 本机终态
  int active_task_id_ = 0;              // 正在执行的任务 id（回写用）

  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr sub_odom_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_patrol_;
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr pub_cmd_vel_;
  rclcpp::Publisher<geometry_msgs::msg::PoseWithCovarianceStamped>::SharedPtr pub_initial_pose_;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr pub_patrol_enable_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_task_state_;
  // 心跳看门狗用：执行端存活信号（1Hz），执行端崩溃时下游据此停机
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_heartbeat_;
  rclcpp::TimerBase::SharedPtr timer_hb_;
  rclcpp_action::Client<NavigateToPose>::SharedPtr nav_client_;
  rclcpp::TimerBase::SharedPtr timer_nav_;
  rclcpp::Time start_time_, nav_start_time_;

  // 共享状态（受 data_mutex_ 保护）
  std::mutex data_mutex_;
  double pos_x_ = 0.0, pos_y_ = 0.0, heading_ = 0.0, speed_ = 0.0;
  std::string status_ = "idle", task_;
  std::string patrol_status_;
  bool mqtt_connected_ = false, odom_ok_ = false;

  // 导航状态
  bool navigating_ = false;
  // 急停闩锁：急停后拒绝一切新的派单，只有 resume 解除（防止"急停后被派单"）
  bool estop_latched_ = false;
  double nav_target_x_ = 0.0, nav_target_y_ = 0.0;
  std::string nav_target_name_;
  std::shared_ptr<NavGoalHandle> nav_goal_;   // 当前 Nav2 目标句柄（用于取消）
  bool initial_pose_sent_ = false;            // 初始位姿是否已对齐

  // ---------------- Nav2 目标重试 ----------------
  // 启动时序：bridge ≈5s、Nav2 ≈20s。启动后立刻派单时 action server 往往还没
  // 起来；旧实现只等 2s 就放弃、而 onNavTimer 从不重发 → 任务必挂满 120s 超时。
  // 现在把"还没成功下发"的目标放进队列，由 onNavTimer 每 2s 重试一次，
  // 最多 15 次（≈30s），用完才判失败并回写 status=revoked（带原因）。
  static constexpr int kNavRetryMax = 15;          // 最多尝试次数
  static constexpr int kNavRetryIntervalS = 2;     // 两次尝试的间隔（秒）
  static constexpr int kNavInflightTimeoutS = 5;   // 发出后等 goal_response 的上限（秒）

  struct PendingGoal
  {
    double x = 0.0, y = 0.0;
    std::string name;
    int task_id = 0;
    unsigned long long seq = 0;      // 入队序号：区分"换了任务但坐标相同"的情况
    int attempts = 0;                // 已尝试次数（日志里的"第 k 次"）
    bool inflight = false;           // 已发出、正等 goal_response 回调
    std::chrono::steady_clock::time_point next_try;   // 下次允许尝试的时刻
    std::chrono::steady_clock::time_point sent_at;    // 本次发出时刻
  };
  std::deque<PendingGoal> pending_goals_;   // 待下发目标（队首即当前要发的）
  unsigned long long pending_seq_ = 0;
};
