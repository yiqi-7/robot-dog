cmake_minimum_required(VERSION 3.5)
project(champ_base)

add_compile_options(-std=c++17)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclpy REQUIRED)
find_package(champ_msgs REQUIRED)
find_package(visualization_msgs REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(sensor_msgs REQUIRED)
find_package(trajectory_msgs REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(tf2_ros REQUIRED)
find_package(tf2 REQUIRED)
find_package(tf2_geometry_msgs REQUIRED)
find_package(urdf REQUIRED)
find_package(champ REQUIRED)
find_package(message_filters REQUIRED)


set(dependencies
  rclcpp
  rclpy
  champ_msgs
  visualization_msgs
  geometry_msgs
  sensor_msgs
  trajectory_msgs
  nav_msgs
  tf2_ros
  urdf
  message_filters
  champ
)

# ---- Lyrical 适配：ament_target_dependencies 已被移除 ----
set(dep_targets
  rclcpp::rclcpp
  champ_msgs::champ_msgs
  visualization_msgs::visualization_msgs
  geometry_msgs::geometry_msgs
  sensor_msgs::sensor_msgs
  trajectory_msgs::trajectory_msgs
  nav_msgs::nav_msgs
  tf2_ros::tf2_ros
  tf2::tf2
  tf2_geometry_msgs::tf2_geometry_msgs
  urdf::urdf
  message_filters::message_filters
  # champ 是 INTERFACE header-only 库，不带链接产物；头文件已由 include_directories 提供
)

# #Download velocity smoother
# set(YOCS_URL "https://github.com/chvmp/yocs_velocity_smoother")
# set(YOCS_DOWNLOAD_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../..)
# set(YOCS_DEPENDENCY_PATH ${YOCS_DOWNLOAD_PATH}/yocs_velocity_smoother)

# if (NOT EXISTS "${YOCS_DEPENDENCY_PATH}")
#   message(STATUS "Downloading yocs_velocity_smoother")
#   message(STATUS "${YOCS_DOWNLOAD_PATH}")

#   execute_process(
#     COMMAND git clone ${YOCS_URL} ${YOCS_DEPENDENCY_PATH}
#   )

#   execute_process(
#     WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../..
#     COMMAND catkin_make --pkg yocs_velocity_smoother
#   )
# endif()
# #end of velocity smoother

include_directories(
  include
  ${champ_INCLUDE_DIRS}
  ${champ_INCLUDE_DIRS}/champ/
  ${champ_INCLUDE_DIRS}/champ/champ/
)
install(DIRECTORY config
  DESTINATION share/${PROJECT_NAME} 
)

add_library(quadruped_controller SHARED src/quadruped_controller.cpp)
target_link_libraries(quadruped_controller ${dep_targets})
add_executable(quadruped_controller_node src/quadruped_controller_node.cpp)
target_link_libraries(quadruped_controller_node quadruped_controller)
target_link_libraries(quadruped_controller_node ${dep_targets})


add_library(message_relay SHARED src/message_relay.cpp)
target_link_libraries(message_relay ${dep_targets})
add_executable(message_relay_node src/message_relay_node.cpp)
target_link_libraries(message_relay_node message_relay)
target_link_libraries(message_relay_node ${dep_targets})


add_library(state_estimation SHARED src/state_estimation.cpp)
target_link_libraries(state_estimation ${dep_targets})
add_executable(state_estimation_node src/state_estimation_node.cpp)
target_link_libraries(state_estimation_node state_estimation)
target_link_libraries(state_estimation_node ${dep_targets})


install(TARGETS 
    quadruped_controller
    quadruped_controller_node
    message_relay
    message_relay_node
    state_estimation
    state_estimation_node
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)


install(TARGETS 
    quadruped_controller_node
    message_relay_node
    state_estimation_node
    DESTINATION lib/${PROJECT_NAME}
)
ament_export_include_directories(include)
ament_export_dependencies(${dependencies})

ament_package()
