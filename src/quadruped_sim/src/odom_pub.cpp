// ============================================================================
// odom_pub.cpp —— 把 /odom 里程计位姿转成 TF（odom -> base_link）
//
// Nav2 / RViz / slam_toolbox 都依赖 TF 树定位机器人。
// gz 的 OdometryPublisher 插件输出 /odom 话题，本节点把它广播成 TF 变换。
// 时间戳直接使用里程计消息自带的时间，保证与仿真时钟同步。
// ============================================================================

#include "geometry_msgs/msg/transform_stamped.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "rclcpp/rclcpp.hpp"
#include "tf2_ros/transform_broadcaster.h"

class OdomPub : public rclcpp::Node
{
public:
  OdomPub() : Node("odom_pub")
  {
    // 参数：坐标 frame 名，可按需重映射
    odom_frame_ = declare_parameter("odom_frame", "odom");
    base_frame_ = declare_parameter("base_frame", "base_link");
    odom_topic_ = declare_parameter("odom_topic", "odom");

    // TF 广播器
    broadcaster_ = std::make_unique<tf2_ros::TransformBroadcaster>(*this);

    // 订阅里程计，每条消息对应一次 TF 广播
    sub_ = create_subscription<nav_msgs::msg::Odometry>(
      odom_topic_, 10,
      [this](nav_msgs::msg::Odometry::SharedPtr msg) {
        geometry_msgs::msg::TransformStamped t;
        t.header.stamp = msg->header.stamp;          // 用消息时间戳，和 sim time 一致
        t.header.frame_id = odom_frame_;
        t.child_frame_id = base_frame_;
        t.transform.translation.x = msg->pose.pose.position.x;
        t.transform.translation.y = msg->pose.pose.position.y;
        t.transform.translation.z = msg->pose.pose.position.z;
        t.transform.rotation = msg->pose.pose.orientation;
        broadcaster_->sendTransform(t);
      });

    RCLCPP_INFO(get_logger(), "odom_pub 已启动: %s -> %s (来自 %s)",
      odom_frame_.c_str(), base_frame_.c_str(), odom_topic_.c_str());
  }

private:
  std::string odom_frame_, base_frame_, odom_topic_;
  std::unique_ptr<tf2_ros::TransformBroadcaster> broadcaster_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr sub_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<OdomPub>());
  rclcpp::shutdown();
  return 0;
}
