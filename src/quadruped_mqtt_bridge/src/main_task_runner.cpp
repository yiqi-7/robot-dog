// ===========================================================================
// main_task_runner.cpp —— task_runner 节点入口（指令分发 / Nav2 / 任务回写）
// ===========================================================================

#include "quadruped_mqtt_bridge/task_runner_node.hpp"

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<TaskRunnerNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
