cmake_minimum_required(VERSION 3.16)
project(quadruped_mqtt_bridge)

# 站点表用 inline const 变量（C++17），三个节点共享同一份定义
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# 上云桥接包：本包整体依赖 libmosquitto（不装则整个包没有意义，直接报错）
# ---------------------------------------------------------------------------
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_action REQUIRED)
find_package(std_msgs REQUIRED)
find_package(std_srvs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(nav2_msgs REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

find_library(MOSQUITTO_LIBRARY NAMES mosquitto)
find_path(MOSQUITTO_INCLUDE_DIR NAMES mosquitto.h)
if(NOT MOSQUITTO_LIBRARY OR NOT MOSQUITTO_INCLUDE_DIR)
  message(FATAL_ERROR "缺少 libmosquitto。安装：sudo apt install libmosquitto-dev nlohmann-json3-dev")
endif()
find_package(nlohmann_json QUIET)

set(BUILT_TARGETS)

# ---------------------------------------------------------------------------
# 三个单一职责节点（各自独立 mosquitto 连接；参数名与 MQTT 主题全部沿用旧版）
#
#   mqtt_bridge  遥测上行（state 1Hz）+ 站点地图（site/map retained）+ 告警（event）
#                —— 唯一注册遗嘱消息（LWT = state/offline）的节点，只写不读
#   task_runner  指令下行（cmd）+ 任务回写（fleet/tasks）+ Nav2 动作客户端 + 急停
#   voice_link   语音上行（robots/{id}/audio）+ TTS 播报（robots/{id}/tts）
#
# 共享代码只有头文件（common.hpp 无状态工具 / site_map.hpp 站点表）：
# 三个可执行文件各自编译进去，没有中间库，避免"改一处要重装三个包"的耦合。
# ---------------------------------------------------------------------------

# ---- ① mqtt_bridge：遥测 + 站点地图 + 告警 ----
add_executable(mqtt_bridge
  src/node_core.cpp
  src/node_mqtt.cpp
  src/node_telemetry.cpp
  src/main.cpp)
target_include_directories(mqtt_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(mqtt_bridge PRIVATE ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(mqtt_bridge
  rclcpp::rclcpp
  nav_msgs::nav_msgs
  sensor_msgs::sensor_msgs
  std_msgs::std_msgs
  ${MOSQUITTO_LIBRARY})
if(nlohmann_json_FOUND)
  target_link_libraries(mqtt_bridge nlohmann_json::nlohmann_json)
endif()
list(APPEND BUILT_TARGETS mqtt_bridge)

# ---- ② task_runner：指令分发 + Nav2 导航 + 任务回写 ----
add_executable(task_runner
  src/task_runner_node.cpp
  src/task_runner_tasks.cpp
  src/main_task_runner.cpp)
target_include_directories(task_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(task_runner PRIVATE ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(task_runner
  rclcpp::rclcpp
  rclcpp_action::rclcpp_action
  nav2_msgs::nav2_msgs
  geometry_msgs::geometry_msgs
  nav_msgs::nav_msgs
  std_msgs::std_msgs
  ${MOSQUITTO_LIBRARY})
if(nlohmann_json_FOUND)
  target_link_libraries(task_runner nlohmann_json::nlohmann_json)
endif()
list(APPEND BUILT_TARGETS task_runner)

# ---- ③ voice_link：语音上行 + TTS 播报 ----
add_executable(voice_link
  src/voice_link_node.cpp
  src/main_voice_link.cpp)
target_include_directories(voice_link PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(voice_link PRIVATE ${MOSQUITTO_INCLUDE_DIR})
target_link_libraries(voice_link
  rclcpp::rclcpp
  std_msgs::std_msgs
  ${MOSQUITTO_LIBRARY})
if(nlohmann_json_FOUND)
  target_link_libraries(voice_link nlohmann_json::nlohmann_json)
endif()
list(APPEND BUILT_TARGETS voice_link)

# ---------------------------------------------------------------------------
# voice_source：麦克风 VAD 分段 / WAV 样本 / 课程 ptt.sh → voice/audio_pcm
# ---------------------------------------------------------------------------
add_executable(deadman_guard src/deadman_guard.cpp)
list(APPEND BUILT_TARGETS deadman_guard)
target_link_libraries(deadman_guard rclcpp::rclcpp
  ${geometry_msgs_TARGETS} ${std_msgs_TARGETS})

add_executable(voice_source src/voice_source.cpp)
target_link_libraries(voice_source
  rclcpp::rclcpp
  std_msgs::std_msgs
  std_srvs::std_srvs)
list(APPEND BUILT_TARGETS voice_source)

# ---------------------------------------------------------------------------
# image_sender：JPEG 快照（MQTT）+ H264 推流（RTSP）
# ---------------------------------------------------------------------------
add_executable(image_sender src/image_sender.cpp)
target_link_libraries(image_sender
  rclcpp::rclcpp
  sensor_msgs::sensor_msgs
  nav_msgs::nav_msgs
  ${OpenCV_LIBS}
  ${MOSQUITTO_LIBRARY})
list(APPEND BUILT_TARGETS image_sender)

install(TARGETS ${BUILT_TARGETS} DESTINATION lib/${PROJECT_NAME})
install(PROGRAMS scripts/ptt.sh DESTINATION lib/${PROJECT_NAME})

ament_package()

# ---------------------------------------------------------------------------
# 单元测试（gtest）：common.hpp 纯函数 + site_map.hpp 静态表
# 只覆盖【不需要 ROS 运行时】的部分；节点类要 broker / Nav2 action server，另算。
#   colcon test --packages-select quadruped_mqtt_bridge
#   colcon test-result --verbose
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_common test/test_common.cpp)
  target_include_directories(test_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MOSQUITTO_INCLUDE_DIR})
  # common.hpp 里 publishText 直接调 mosquitto_publish：空句柄用例会让它被 odr-use
  target_link_libraries(test_common ${MOSQUITTO_LIBRARY})
  if(nlohmann_json_FOUND)
    target_link_libraries(test_common nlohmann_json::nlohmann_json)
  endif()
  install(TARGETS test_common DESTINATION lib/${PROJECT_NAME})
endif()
