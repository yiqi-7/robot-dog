// 巡逻节点：按预设航点序列循环发送 NavigateToPose 目标。
// 设计要点（对应实测结论，见 NAV2-GUIDE.md）：
//  - 遇 error_code 208（NavFn "Failed to create plan"，机器人贴墙
//    停稳后脚下格被标致命）视为可重试错误：取消后等 3 s 重发，最多 2 次。
//  - 重试前先做"脱困"（后退+原地转）：机器人多半是顶在障碍/膨胀区里
//    推不动（Champ 步态卡滞，controller 报 105），纯等重发没用；
//    挪一下再试，实测能把相当一部分 code=6/105 变成到达。
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
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <thread>

using NavigateToPose = nav2_msgs::action::NavigateToPose;
using GoalHandle = rclcpp_action::ClientGoalHandle<NavigateToPose>;
using namespace std::chrono_literals;

namespace quadruped_sim
{

struct Waypoint
{
  double x;
  double y;
  std::string name;   // 值语义：参数解析出的临时串必须拷贝，不能用指针
};

// 航点来源：ROS 参数 waypoints（字符串数组，每项 "x,y,名称"），默认
// 是工厂实测环线。参数化后运维不改代码就能换路线。
// 默认路线中的「缝北口」是对齐航点：货架缝物理宽 1.6 m，但两侧各膨胀
// 0.35 m（robot_radius）后，全局代价图里只剩 x=3.8~4.2 的 0.4 m 自由
// 走廊。若直接从「墙洞东」斜插过去，机器人会楔在货架西北角（该格代价
// =99 致命），NavFn 拒绝从致命格起飞（error 208），BT 随后报 105 放弃。
// 先到缝中心线 x=4.0 摆正再南下即可。
static const std::vector<std::string> kDefaultRoute = {
  "0.0,-2.5,北走廊",
  "2.2,-3.2,墙洞东",
  "4.0,-4.3,缝北口",
  "2.5,-5.8,货架南",
  "0.2,-5.4,出生点",
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
    // 航点列表："x,y,名称"（坐标为 map 系米制；名称用于日志与看板）
    declare_parameter<std::vector<std::string>>(
      "waypoints", kDefaultRoute);

    const auto wp_param = get_parameter("waypoints");
    if (wp_param.get_type() != rclcpp::ParameterType::PARAMETER_STRING_ARRAY) {
      RCLCPP_ERROR(get_logger(), "waypoints 参数类型异常（应为字符串数组）");
    }
    for (const auto & item : wp_param.as_string_array()) {
      const auto c1 = item.find(',');
      const auto c2 = c1 == std::string::npos ? std::string::npos : item.find(',', c1 + 1);
      if (c1 == std::string::npos || c2 == std::string::npos) {
        RCLCPP_WARN(get_logger(), "航点格式应为 x,y,名称，忽略: %s", item.c_str());
        continue;
      }
      try {
        route_.push_back({std::stod(item.substr(0, c1)),
                          std::stod(item.substr(c1 + 1, c2 - c1 - 1)),
                          item.substr(c2 + 1)});
      } catch (const std::exception &) {
        RCLCPP_WARN(get_logger(), "航点坐标解析失败，忽略: %s", item.c_str());
      }
    }
    if (route_.size() < 2) {
      RCLCPP_ERROR(get_logger(), "有效航点不足 2 个，巡逻无意义");
    }
    RCLCPP_INFO(get_logger(), "巡逻节点就绪，%zu 个航点", route_.size());

    client_ = rclcpp_action::create_client<NavigateToPose>(this, "navigate_to_pose");
    status_pub_ = create_publisher<std_msgs::msg::String>("/patrol/status", 10);
    // 脱困速度发布器：直发 /cmd_vel（Champ 的最终速度入口）。
    // 正常巡航绝不碰这条路径，只在"确认卡滞"后发短脉冲；此时 Nav2
    // 目标已中止、controller 停发速度，不存在两个来源打架。
    unstick_pub_ = create_publisher<geometry_msgs::msg::Twist>("/cmd_vel", 10);
    // 派单互斥开关：mqtt_bridge 派单时置 false，任务结束后置 true
    enable_sub_ = create_subscription<std_msgs::msg::Bool>(
      "/patrol/enable", 10,
      [this](std_msgs::msg::Bool::SharedPtr msg) {
        if (enabled_ != msg->data) {
          enabled_ = msg->data;
          RCLCPP_INFO(get_logger(), "巡逻%s", enabled_ ? "恢复" : "暂停");
        }
      });
    timer_ = create_wall_timer(500ms, std::bind(&PatrolNode::step, this));
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
    const auto & a = route_[waypoint_index_];
    const auto & b = route_[(waypoint_index_ + 1) % route_.size()];
    const double dist = std::hypot(a.x - b.x, a.y - b.y);
    return (dist / 0.06 + 90.0) * scale;
  }

  void sendGoal()
  {
    const auto & wp = route_[waypoint_index_];
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
                waypoint_index_ + 1, route_.size(), wp.name.c_str(), attempt_ + 1);
    publishStatus("going", std::string("前往 ") + wp.name);
  }

  // 卡滞脱困：后退一小段 + 原地转一个角度。
  // Champ 步态"顶住"障碍时，Nav2 有路径、速度也下发了，但关节速度
  // 打满限幅底盘不前进，controller 以 105 "Failed to make progress"
  // 中止。原地等再久都是白等；挪一下再重发目标，实测能把相当一部分
  // code=6/105 变成到达。
  // 阻塞约 3.2 s：期间 500ms 的 step() 回调不会插新目标（active_==false
  // 且 cooldown_ 未到），安全。
  void unstick()
  {
    RCLCPP_WARN(get_logger(), "执行脱困：后退+转身");
    publishStatus("unstick", "卡滞脱困中");
    auto pulse = [this](double lx, double az, int ms) {
      geometry_msgs::msg::Twist t;
      t.linear.x = lx;
      t.angular.z = az;
      for (int sent = 0; sent < ms; sent += 100) {
        unstick_pub_->publish(t);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      geometry_msgs::msg::Twist stop;
      for (int i = 0; i < 3; ++i) { unstick_pub_->publish(stop); }
      std::this_thread::sleep_for(std::chrono::milliseconds(800));
    };
    pulse(-0.12, 0.0, 1300);   // 后退 1.3 s
    pulse(0.0, 0.5, 1600);     // 原地转 1.6 s
  }

  void onResult(const GoalHandle::WrappedResult & result)
  {
    active_ = false;
    const auto & wp = route_[waypoint_index_];
    if (result.code == rclcpp_action::ResultCode::SUCCEEDED) {
      RCLCPP_INFO(get_logger(), "航点 %s 到达 ✅", wp.name.c_str());
      publishStatus("reached", std::string("已到达 ") + wp.name);
      waypoint_index_ = (waypoint_index_ + 1) % route_.size();
      // 单次模式（loop:=false）：走完最后一个航点即收工
      if (!get_parameter("loop").as_bool() && waypoint_index_ == 0) {
        RCLCPP_INFO(get_logger(), "路线走完，单次模式结束");
        publishStatus("done", "巡逻完成");
        timer_->cancel();
        return;
      }
      attempt_ = 0;
      cooldown_ = 3;
      return;
    }

    // 208 = NavFn 规划失败（可重试，见 NAV2-GUIDE.md）；其余失败码也重试但更保守
    if (attempt_ < 2) {
      unstick();
      ++attempt_;
      RCLCPP_WARN(get_logger(), "航点 %s 失败 (code=%d)，脱困完成，3 s 后重试 %d/2",
                  wp.name.c_str(), static_cast<int>(result.code), attempt_);
      publishStatus("retry", std::string("重试中 ") + wp.name);
      cooldown_ = 3;
    } else {
      RCLCPP_ERROR(get_logger(), "航点 %s 重试耗尽，跳过并继续下一个", wp.name.c_str());
      publishStatus("skipped", std::string("已跳过 ") + wp.name);
      waypoint_index_ = (waypoint_index_ + 1) % route_.size();
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
    // 派单接管时暂停巡逻：mqtt_bridge 发 /patrol/enable = false。
    // 两个节点都发 NavigateToPose，若不互斥会互相取消/打断目标，
    // 表现为机器人原地犹豫、任务永不完成。
    if (!enabled_) {
      if (active_) {
        RCLCPP_INFO(get_logger(), "收到暂停指令，取消当前航点");
        if (goal_handle_) { client_->async_cancel_goal(goal_handle_); }
        active_ = false;
      }
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
  rclcpp::Publisher<geometry_msgs::msg::Twist>::SharedPtr unstick_pub_;
  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr enable_sub_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::shared_ptr<GoalHandle> goal_handle_;
  std::vector<Waypoint> route_;
  size_t waypoint_index_;
  int attempt_;
  int cooldown_ = 0;
  bool active_ = false;
  bool enabled_ = true;   // 巡逻开关（/patrol/enable，派单期间置 false）
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
