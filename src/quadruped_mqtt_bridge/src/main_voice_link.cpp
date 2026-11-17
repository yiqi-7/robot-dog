// ===========================================================================
// main_voice_link.cpp —— voice_link 节点入口（§3.1 音频上行 / §3.2 TTS 播报）
// ===========================================================================

#include "quadruped_mqtt_bridge/voice_link_node.hpp"

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
  auto node = std::make_shared<VoiceLinkNode>();
  rclcpp::spin(node);
  rclcpp::shutdown();
  return 0;
}
