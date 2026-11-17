cmake_minimum_required(VERSION 3.16)
project(quadruped_mqtt_bridge)

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
# mqtt_bridge：机器狗 ↔ 看板 MQTT 桥（状态上行 / 任务执行 / 语音 §3.1 上行 / TTS 播报）
# ---------------------------------------------------------------------------
add_executable(mqtt_bridge
  src/node_core.cpp
  src/node_mqtt.cpp
  src/node_telemetry.cpp
  src/node_tasks.cpp
  src/node_voice.cpp
  src/main.cpp)
target_include_directories(mqtt_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(mqtt_bridge PRIVATE ${MOSQUITTO_INCLUDE_DIR})
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

# ---------------------------------------------------------------------------
# voice_source：麦克风 VAD 分段 / WAV 样本 / 课程 ptt.sh → voice/audio_pcm
# ---------------------------------------------------------------------------
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
