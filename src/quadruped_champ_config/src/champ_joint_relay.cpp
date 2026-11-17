#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/float64.hpp>

#include <string>
#include <unordered_map>

/*
 * CHAMP 关节指令中继节点
 *
 * 为什么要这个节点：
 *   CHAMP 的 quadruped_controller 输出一个总的 /joint_states
 *   （sensor_msgs/JointState，position = 12 个驱动关节的目标角度，弧度）；
 *   而 gz sim 的关节位置控制接口是每个关节一个独立话题：
 *       /quadruped/<腿>_<部位>/cmd_pos   (std_msgs/Float64)
 *   两边格式对不上，所以需要一个翻译层把"一整包"拆成"12 个单发"。
 *
 * 命名规则：
 *   CHAMP 关节名（我们在 joints.yaml 里直接填了 URDF 原名）
 *       fl_abduction_joint / fl_hip_joint / fl_knee_joint ...
 *   gz 目标话题
 *       /quadruped/fl_abduction/cmd_pos ...
 *   即去掉 "_joint" 后缀，拼上 <前缀>/ 与 /cmd_pos。
 *
 * 为什么按名字映射而不是按数组顺序：
 *   不依赖 champ 内部 getJointNames 的排序约定——joints.yaml 顺序变了、
 *   上游换版本了，只要关节名不变，这里就不用改。
 */
class ChampJointRelay : public rclcpp::Node
{
public:
  ChampJointRelay() : Node("champ_joint_relay")
  {
    // 参数：champ 指令话题（launch 里已把 champ 的 joint_states 重映射过来），
    // 以及 gz 关节话题前缀（与 gz 桥接里的命名保持一致）
    cmd_topic_ = declare_parameter<std::string>("cmd_topic", "/champ/joint_cmd");
    gz_prefix_ = declare_parameter<std::string>("gz_prefix", "/quadruped");

    sub_ = create_subscription<sensor_msgs::msg::JointState>(
        cmd_topic_, 10,
        std::bind(&ChampJointRelay::onJointCmd, this, std::placeholders::_1));

    RCLCPP_INFO(get_logger(),
        "champ_joint_relay 已启动: 订阅 %s (JointState) -> 发布 %s/<关节>/cmd_pos (Float64)",
        cmd_topic_.c_str(), gz_prefix_.c_str());
  }

private:
  void onJointCmd(const sensor_msgs::msg::JointState::SharedPtr msg)
  {
    // 名字与位置必须一一对应，否则无从映射；丢帧并限频告警
    if (msg->name.size() != msg->position.size()) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
          "JointState 的 name/position 数量不一致，丢弃该帧");
      return;
    }
    for (size_t i = 0; i < msg->name.size(); ++i) {
      const std::string &name = msg->name[i];

      // 只转发 12 个驱动关节：
      //   *_foot_joint 是固定关节、lidar/depth_camera 是传感器关节，一律跳过
      if (name.size() <= 6 ||
          name.compare(name.size() - 6, 6, "_joint") != 0) {
        continue;
      }
      std::string base = name.substr(0, name.size() - 6);  // 去掉 _joint 后缀
      bool actuated =
          base.find("abduction") != std::string::npos ||
          base.find("hip")       != std::string::npos ||
          base.find("knee")      != std::string::npos;
      if (!actuated) {
        continue;
      }

      // 懒创建发布器：第一次遇到该关节时才建立话题，避免硬编码 12 个关节名
      auto &pub = pubs_[base];
      if (!pub) {
        pub = create_publisher<std_msgs::msg::Float64>(
            gz_prefix_ + "/" + base + "/cmd_pos", 10);
        RCLCPP_INFO(get_logger(), "接入关节通道: %s", (gz_prefix_ + "/" + base + "/cmd_pos").c_str());
      }
      std_msgs::msg::Float64 out;
      out.data = msg->position[i];
      pub->publish(out);
    }
  }

  std::string cmd_topic_;
  std::string gz_prefix_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr sub_;
  std::unordered_map<std::string,
      rclcpp::Publisher<std_msgs::msg::Float64>::SharedPtr> pubs_;
};

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<ChampJointRelay>());
  rclcpp::shutdown();
  return 0;
}
