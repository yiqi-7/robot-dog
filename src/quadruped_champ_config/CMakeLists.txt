cmake_minimum_required(VERSION 3.10)
project(quadruped_champ_config)
find_package(ament_cmake REQUIRED)
# 纯配置包：只安装 config 与 launch，无编译产物
install(DIRECTORY config launch DESTINATION share/${PROJECT_NAME})
ament_package()
