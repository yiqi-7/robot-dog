// ============================================================================
// camera_info_pub.cpp —— 发布深度相机的 CameraInfo（内参）
//
// gz sim 10 的 depth_camera 传感器只发布深度图和点云，
// 不发布相机内参。而 RViz / Nav2 / 点云配准都需要内参，
// 本节点按针孔模型计算并定时发布：
//     fx = fy = (图像宽/2) / tan(水平视场角/2)
//     cx = (宽-1)/2,  cy = (高-1)/2
// ============================================================================

#include <chrono>
#include <cmath>

#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/camera_info.hpp"

class CameraInfoPub : public rclcpp::Node
{
public:
  CameraInfoPub() : Node("depth_camera_info")
  {
    width_  = declare_parameter("width", 640);
    height_ = declare_parameter("height", 480);
    hfov_   = declare_parameter("horizontal_fov", 1.2);
    frame_  = declare_parameter("frame_id", "depth_camera_link");
    rate_   = declare_parameter("rate", 15.0);
    topic_  = declare_parameter("topic", "depth_camera/camera_info");

    // 针孔模型内参：方形像素 fx = fy
    fx_ = (width_ / 2.0) / std::tan(hfov_ / 2.0);
    cx_ = (width_ - 1) / 2.0;
    cy_ = (height_ - 1) / 2.0;

    pub_ = create_publisher<sensor_msgs::msg::CameraInfo>(topic_, 10);
    timer_ = create_wall_timer(
      std::chrono::milliseconds(static_cast<int>(1000.0 / rate_)),
      std::bind(&CameraInfoPub::publishInfo, this));

    RCLCPP_INFO(get_logger(),
      "camera_info 已启动: %dx%d hfov=%.2f fx=%.2f frame=%s -> %s",
      width_, height_, hfov_, fx_, frame_.c_str(), topic_.c_str());
  }

private:
  void publishInfo()
  {
    sensor_msgs::msg::CameraInfo msg;
    msg.header.stamp = now();              // 节点 use_sim_time 时自动跟随仿真时钟
    msg.header.frame_id = frame_;
    msg.width = width_;
    msg.height = height_;
    msg.distortion_model = "plumb_bob";    // gz 渲染无畸变，全 0
    msg.d = {0.0, 0.0, 0.0, 0.0, 0.0};
    // K = 相机矩阵 (3x3, 行优先)
    msg.k = {fx_, 0.0, cx_,
             0.0, fx_, cy_,
             0.0, 0.0, 1.0};
    // R = 立体校正旋转矩阵，单目取单位阵
    msg.r = {1.0, 0.0, 0.0,
             0.0, 1.0, 0.0,
             0.0, 0.0, 1.0};
    // P = 投影矩阵 (3x4)
    msg.p = {fx_, 0.0, cx_, 0.0,
             0.0, fx_, cy_, 0.0,
             0.0, 0.0, 1.0, 0.0};
    pub_->publish(msg);
  }

  int width_, height_;
  double hfov_, fx_, cx_, cy_, rate_;
  std::string frame_, topic_;
  rclcpp::Publisher<sensor_msgs::msg::CameraInfo>::SharedPtr pub_;
  rclcpp::TimerBase::SharedPtr timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<CameraInfoPub>());
  rclcpp::shutdown();
  return 0;
}
