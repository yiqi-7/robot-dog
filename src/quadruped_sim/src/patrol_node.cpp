// 巡逻节点：按预设航点序列循环发送 NavigateToPose 目标。
// 设计要点（对应实测结论，见 NAV2-GUIDE.md）：
//  - 遇 error_code 208（NavFn "Failed to create plan"，机器人贴墙
//    停稳后脚下格被标致命）视为可重试错误：取消后等 3 s 重发，最多 2 次。
//  - 机器人实际速度约 0.08 m/s，目标超时按航点间距 / 0.06 + 90 s 估算，
//    宁可超时不误杀。
//  - 全程通过 /patrol/status（std_msgs/String，JSON）播报进度，
//    前端/日志可直接消费。
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <nav2_msgs/action/navigate_to_pose.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <std_msgs/msg/string.hpp>

using NavigateToPose = nav2_msgs::action::NavigateToPose;
using GoalHandle = rclcpp_action::ClientGoalHandle<NavigateToPose>;
using namespace std::chrono_literals;

namespace quadruped_sim
{

struct Waypoint
{
  double x;
  double y;
  const char * name;
};

// 工厂巡逻环线。
// 关键：货架缝物理宽 1.6 m，但两侧各膨胀 0.35 m（robot_radius）后，
// 全局代价图里只剩 x=3.8~4.2 的 0.4 m 自由走廊。若直接从「墙洞东」
// 斜插过去，机器人会楔在货架西北角（该格代价=99 致命），NavFn 拒绝
// 从致命格起飞（error 208），BT 随后报 105 放弃。
// 因此插入「缝北口」航点，先把机器人摆到缝中心线 x=4.0 再南下。
static const std::vector<Waypoint> kPatrolRoute = {
  {0.0,  -2.5, "北走廊"},
  {2.2,  -3.2, "墙洞东"},
  {4.0,  -4.3, "缝北口"},   // 对齐航点：货架缝中心线，避免斜插楔角
  {2.5,  -6.5, "货架南"},
  {0.2,  -5.4, "出生点"},
};

class PatrolNode : public rclcpp::Node
{
public:
  PatrolNode()
  : Node("patrol_node"), waypoint_index_(0), attempt_(0)
  {
    // 参数：循环巡逻（默认 true）；单点超时倍率（默认 1.0）
    declare_parameter<bool>("loop", true);
    declare_parameter<double>("timeout_scale", 1.0);

    client_ = rclcpp_action::create_client<NavigateToPose>(this, "navigate_to_pose");
    status_pub_ = create_publisher<std_msgs::msg::String>("/patrol/status", 10);
    timer_ = create_wall_timer(500ms, std::bind(&PatrolNode::step, this));
    RCLCPP_INFO(get_logger(), "巡逻节点就绪，%zu 个航点", kPatrolRoute.size());
  }

private:
  void publishStatus(const std::string & state, const std::string & detail)
  {
    std_msgs::msg::String m;
    // 轻量 JSON：{"state":"...","wp":3,"wp_name":"...","detail":"..."}
    m.data = "{\"state\":\"" + state + "\",\"wp\":" + std::to_string(waypoint_index_ + 1) +
             ",\"wp_name\":\"" + detail + "\"}";
    status_pub_->publish(m);
  }

  double waypointTimeout() const
  {
    // 航点间距 / 保守速度 0.06 m/s + 90 s 余量，再乘用户倍率
    const double scale = get_parameter("timeout_scale").as_double();
    const auto & a = kPatrolRoute[waypoint_index_];
    const auto & b = kPatrolRoute[(waypoint_index_ + 1) % kPatrolRoute.size()];
    const double dist = std::hypot(a.x - b.x, a.y - b.y);
    return (dist / 0.06 + 90.0) * scale;
  }

  void sendGoal()
  {
    const auto & wp = kPatrolRoute[waypoint_index_];
    NavigateToPose::Goal goal;
    goal.pose.header.frame_id = "map";
    goal.pose.header.stamp = now();
    goal.pose.pose.position.x = wp.x;
    goal.pose.pose.position.y = wp.y;
    goal.pose.pose.orientation.w = 1.0;

    auto opts = rclcpp_action::Client<NavigateToPose>::SendGoalOptions();
    opts.goal_response_callback =
      [this](const GoalHandle::SharedPtr & h) { goal_handle_ = h; };
    opts.result_callback =
      [this](const GoalHandle::WrappedResult & result) { onResult(result); };
    client_->async_send_goal(goal, opts);
    goal_start_ = std::chrono::steady_clock::now();
    goal_timeout_ = waypointTimeout();
    active_ = true;
    RCLCPP_INFO(get_logger(), "发送航点 %zu/%zu (%s) 第 %d 次",
                waypoint_index_ + 1, kPatrolRoute.size(), wp.name, attempt_ + 1);
  }

  void onResult(const GoalHandle::WrappedResult & result)
  {
    active_ = false;
    const auto & wp = kPatrolRoute[waypoint_index_];
    if (result.code == rclcpp_action::ResultCode::SUCCEEDED) {
      RCLCPP_INFO(get_logger(), "航点 %s 到达 ✅", wp.name);
      publishStatus("reached", wp.name);
      waypoint_index_ = (waypoint_index_ + 1) % kPatrolRoute.size();
      attempt_ = 0;
      cooldown_ = 3;
      return;
    }

    // 208 = NavFn 规划失败（可重试，见 NAV2-GUIDE.md）；其余失败码也重试但更保守
    if (attempt_ < 2) {
      ++attempt_;
      RCLCPP_WARN(get_logger(), "航点 %s 失败 (code=%d)，3 s 后重试 %d/2",
                  wp.name, static_cast<int>(result.code), attempt_);
      publishStatus("retry", wp.name);
      cooldown_ = 3;
    } else {
      RCLCPP_ERROR(get_logger(), "航点 %s 重试耗尽，跳过并继续下一个", wp.name);
      publishStatus("skipped", wp.name);
      waypoint_index_ = (waypoint_index_ + 1) % kPatrolRoute.size();
      attempt_ = 0;
      cooldown_ = 5;
    }
  }

  void step()
  {
    if (!client_->wait_for_action_server(0ms)) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000, "等待 navigate_to_pose server");
      return;
    }
    if (active_) {
      const auto elapsed = std::chrono::steady_clock::now() - goal_start_;
      if (std::chrono::duration<double>(elapsed).count() > goal_timeout_) {
        // 超时兜底：取消当前目标，触发 result_callback 走重试逻辑
        RCLCPP_WARN(get_logger(), "航点超时，取消");
        // 记录目标后经 client_ 取消，结果回调会走重试分支
        if (goal_handle_) { client_->async_cancel_goal(goal_handle_); }
      }
      return;
    }
    if (cooldown_ > 0) { --cooldown_; return; }
    sendGoal();
  }

  rclcpp_action::Client<NavigateToPose>::SharedPtr client_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr status_pub_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::shared_ptr<GoalHandle> goal_handle_;
  size_t waypoint_index_;
  int attempt_;
  int cooldown_ = 0;
  bool active_ = false;
  std::chrono::steady_clock::time_point goal_start_;
  double goal_timeout_ = 120.0;
};

}  // namespace quadruped_sim

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<quadruped_sim::PatrolNode>());
  rclcpp::shutdown();
  return 0;
}
