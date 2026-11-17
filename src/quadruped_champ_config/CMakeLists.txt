cmake_minimum_required(VERSION 3.10)
project(quadruped_champ_config)
find_package(ament_cmake REQUIRED)
# 关节指令中继：/champ/joint_cmd (JointState) -> /quadruped/<关节>/cmd_pos (Float64)
# 它是 CHAMP 与 gz 关节位置控制器之间的胶水，属于本包而非仿真包。
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(std_msgs REQUIRED)

add_executable(champ_joint_relay src/champ_joint_relay.cpp)
target_link_libraries(champ_joint_relay
  rclcpp::rclcpp sensor_msgs::sensor_msgs std_msgs::std_msgs)

install(TARGETS champ_joint_relay DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY config launch DESTINATION share/${PROJECT_NAME})
ament_package()
