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
# gait_controller：爬行步态控制器
# 订阅 cmd_vel / cmd_vel_nav / odom，闭环速度反馈，输出 12 关节位置指令
# ---------------------------------------------------------------------------
add_executable(gait_controller src/gait_controller.cpp)
target_link_libraries(gait_controller
  rclcpp::rclcpp
  std_msgs::std_msgs
  geometry_msgs::geometry_msgs
  nav_msgs::nav_msgs
  sensor_msgs::sensor_msgs)

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

# ---------------------------------------------------------------------------
# mqtt_bridge：机器狗 ↔ 云端看板 MQTT 桥（第三章 3.2）
#   上行：/odom + /joint_states -> {P}/robots/{id}/state
#   下行：{P}/robots/{id}/cmd  -> /cmd_vel
# 依赖 libmosquitto-dev（C 客户端）+ nlohmann-json（组 JSON）
# ---------------------------------------------------------------------------
add_executable(champ_joint_relay src/champ_joint_relay.cpp)
# 巡逻节点：航点环线 + 208 可重试 + 状态播报（见 NAV2-GUIDE.md）
add_executable(image_sender src/image_sender.cpp)
add_executable(patrol_node src/patrol_node.cpp)
target_link_libraries(champ_joint_relay rclcpp::rclcpp sensor_msgs::sensor_msgs std_msgs::std_msgs)
# 巡逻节点依赖 action 客户端与 nav2_msgs（rclcpp_action 随 rclcpp 头引入）
find_package(nav2_msgs REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
target_link_libraries(patrol_node rclcpp::rclcpp rclcpp_action::rclcpp_action nav2_msgs::nav2_msgs std_msgs::std_msgs)

target_link_libraries(image_sender
  rclcpp::rclcpp
  sensor_msgs::sensor_msgs
  nav_msgs::nav_msgs
  ${OpenCV_LIBS}
  ${MOSQUITTO_LIBRARY}
)
set(BUILT_TARGETS champ_joint_relay patrol_node image_sender gait_controller odom_pub camera_info_pub)

# ---------------------------------------------------------------------------
# gz_window_minimize：把 Gazebo GUI 窗口最小化到任务栏
#   启动仿真时窗口默认弹到前台挡住桌面，用 ICCCM 的 WM_CHANGE_STATE 消息
#   请求最小化即可（只依赖 libX11，无需 xdotool/wmctrl、无需 root）
# ---------------------------------------------------------------------------
find_package(X11 REQUIRED)
add_executable(gz_window_minimize scripts/gz_window_minimize.c)
target_link_libraries(gz_window_minimize X11::X11)
list(APPEND BUILT_TARGETS gz_window_minimize)

find_library(MOSQUITTO_LIBRARY NAMES mosquitto)
find_path(MOSQUITTO_INCLUDE_DIR NAMES mosquitto.h)
find_package(nlohmann_json QUIET)

if(NOT MOSQUITTO_LIBRARY OR NOT MOSQUITTO_INCLUDE_DIR)
  message(WARNING
    "未找到 libmosquitto，跳过 mqtt_bridge。安装：sudo apt install libmosquitto-dev")
else()
  add_executable(mqtt_bridge src/mqtt_bridge.cpp)
  target_include_directories(mqtt_bridge PRIVATE ${MOSQUITTO_INCLUDE_DIR})
  # 派单导航走 Nav2 动作接口，因此要链 nav2_msgs 与 rclcpp_action
  target_link_libraries(mqtt_bridge
    rclcpp::rclcpp
    rclcpp_action::rclcpp_action
    nav2_msgs::nav2_msgs
    geometry_msgs::geometry_msgs
    nav_msgs::nav_msgs
    sensor_msgs::sensor_msgs
    std_msgs::std_msgs
    ${MOSQUITTO_LIBRARY})
  if(nlohmann_json_FOUND)
    target_link_libraries(mqtt_bridge nlohmann_json::nlohmann_json)
  endif()
  list(APPEND BUILT_TARGETS mqtt_bridge)
  message(STATUS "mqtt_bridge 已启用：${MOSQUITTO_LIBRARY}")
endif()

# ---------------------------------------------------------------------------
# 安装：可执行文件 -> lib/<pkg>，资源目录 -> share/<pkg>
# ---------------------------------------------------------------------------
install(TARGETS ${BUILT_TARGETS} DESTINATION lib/${PROJECT_NAME})

install(DIRECTORY urdf launch worlds config
        DESTINATION share/${PROJECT_NAME})

ament_package()
