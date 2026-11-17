// =============================================================================
// deadman_guard —— 执行安全看门狗（速度链中继）
//
// 速度链：controller_server → cmd_vel_nav → smoother → cmd_vel_smoothed
//         → collision_monitor → cmd_vel_in → 【本节点】→ cmd_vel → 底盘
//
// 背景：task_runner 是唯一持有 Nav2 动作客户端的执行端。它一旦崩溃，Nav2
// 服务器仍会继续发布速度指令（"控制端死亡机器人继续走"）。本节点监视
// task_runner 的 1Hz 心跳（safety/heartbeat）：心跳超时立即切断并持续发
// 零速；心跳恢复自动放行。这是底盘层的最后一道停机手段。
//
// 兼容性：从未见过心跳（如纯手动 teleop、无 task_runner 的场景）时放行，
// 不影响独立调试。
// =============================================================================
#include <rclcpp/rclcpp.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <std_msgs/msg/string.hpp>
#include <algorithm>

class DeadmanGuard : public rclcpp::Node
{
public:
  DeadmanGuard() : Node("deadman_guard")
  {
    hb_timeout_ = declare_parameter("hb_timeout", 3.0);
    sub_in_ = create_subscription<geometry_msgs::msg::Twist>(
      "cmd_vel_in", 10,
      [this](geometry_msgs::msg::Twist::SharedPtr msg) { last_in_ = *msg; in_seen_ = true; });
    sub_hb_ = create_subscription<std_msgs::msg::String>(
      "safety/heartbeat", 10,
      [this](std_msgs::msg::String::SharedPtr) { last_hb_ = now(); hb_seen_ = true; });
    pub_out_ = create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
    timer_ = create_wall_timer(std::chrono::milliseconds(100), [this]() { tick(); });
    RCLCPP_INFO(get_logger(),
      "看门狗已上线：cmd_vel_in → cmd_vel，心跳超时 %.1fs 切断（底盘层最后停机手段）",
      hb_timeout_);
  }

private:
  void tick()
  {
    // 放行条件：从未见过心跳（无执行端场景）或心跳新鲜
    const bool alive = !hb_seen_ || (now() - last_hb_).seconds() <= hb_timeout_;
    if (alive) {
      if (tripped_) {
        RCLCPP_WARN(get_logger(), "执行端心跳恢复，看门狗放行速度链路");
        tripped_ = false;
      }
      if (in_seen_) {
        pub_out_->publish(last_in_);
      }
      return;
    }
    if (!tripped_) {
      RCLCPP_ERROR(get_logger(),
        "执行端(task_runner)心跳丢失 >%.1fs，看门狗切断速度链路并持续发零速", hb_timeout_);
      tripped_ = true;
    }
    geometry_msgs::msg::Twist zero;
    pub_out_->publish(zero);
  }

  double hb_timeout_;
  bool in_seen_ = false, hb_seen_ = false, tripped_ = false;
  geometry_msgs::msg::Twist last_in_;
  rclcpp::Time last_hb_{0, 0, RCL_ROS_TIME};
  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr sub_in_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_hb_;
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr pub_out_;
  rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<DeadmanGuard>());
  rclcpp::shutdown();
  return 0;
}
