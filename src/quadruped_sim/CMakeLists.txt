cmake_minimum_required(VERSION 3.16)
project(quadruped_sim)

# ---------------------------------------------------------------------------
# 依赖包：rclcpp(节点) / 各消息包 / tf2(坐标变换广播)
# ---------------------------------------------------------------------------
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(tf2 REQUIRED)
find_package(tf2_ros REQUIRED)

# ---------------------------------------------------------------------------
# 步态由 CHAMP 负责（champ_base + quadruped_champ_config），
# 本项目自研的 gait_controller 已移除，不再在此构建。
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# odom_pub：把 /odom 位姿广播成 TF (odom -> base_link)
# ---------------------------------------------------------------------------
add_executable(odom_pub src/odom_pub.cpp)
target_link_libraries(odom_pub
  rclcpp::rclcpp
  geometry_msgs::geometry_msgs
  nav_msgs::nav_msgs
  tf2::tf2
  tf2_ros::tf2_ros)

# ---------------------------------------------------------------------------
# camera_info_pub：发布深度相机内参（gz 的 depth_camera 不自带）
# ---------------------------------------------------------------------------
add_executable(camera_info_pub src/camera_info_pub.cpp)
target_link_libraries(camera_info_pub
  rclcpp::rclcpp
  sensor_msgs::sensor_msgs)

set(BUILT_TARGETS odom_pub camera_info_pub)

# ---------------------------------------------------------------------------
# gz_window_minimize：把 Gazebo GUI 窗口最小化到任务栏
#   启动仿真时窗口默认弹到前台挡住桌面，用 ICCCM 的 WM_CHANGE_STATE 消息
#   请求最小化即可（只依赖 libX11，无需 xdotool/wmctrl、无需 root）
# ---------------------------------------------------------------------------
find_package(X11 REQUIRED)
add_executable(gz_window_minimize scripts/gz_window_minimize.c)
target_link_libraries(gz_window_minimize X11::X11)
list(APPEND BUILT_TARGETS gz_window_minimize)

# ---------------------------------------------------------------------------
# 安装：可执行文件 -> lib/<pkg>，资源目录 -> share/<pkg>
# ---------------------------------------------------------------------------
install(TARGETS ${BUILT_TARGETS} DESTINATION lib/${PROJECT_NAME})

install(DIRECTORY urdf launch worlds config
        DESTINATION share/${PROJECT_NAME})

ament_package()
