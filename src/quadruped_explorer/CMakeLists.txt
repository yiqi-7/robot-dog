cmake_minimum_required(VERSION 3.16)
project(quadruped_explorer)

# ---------------------------------------------------------------------------
# 自主导航包：依赖 Nav2 动作接口（navigate_to_pose），不碰 Gazebo
# ---------------------------------------------------------------------------
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(nav2_msgs REQUIRED)
find_package(std_msgs REQUIRED)

add_executable(patrol_node src/patrol_node.cpp)
target_link_libraries(patrol_node
  rclcpp::rclcpp
  rclcpp_action::rclcpp_action
  nav2_msgs::nav2_msgs
  std_msgs::std_msgs)

install(TARGETS patrol_node DESTINATION lib/${PROJECT_NAME})

ament_package()
