// ===========================================================================
// main.cpp —— mqtt_bridge 节点入口（遥测 / 站点地图 / 告警）
// ===========================================================================

#include "quadruped_mqtt_bridge/mqtt_bridge_node.hpp"

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<MqttBridgeNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
