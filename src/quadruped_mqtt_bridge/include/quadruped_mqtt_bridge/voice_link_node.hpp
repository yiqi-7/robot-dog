#pragma once
// ===========================================================================
// voice_link_node.hpp —— 节点③voice_link（VoiceLinkNode）类声明
//
// 【单一职责】机器人语音链路的两端都在这个节点里，别人不用管：
//   · §3.1 音频上行：ROS voice/audio_pcm（+ latched voice/audio_info 带格式）
//     → base64 → MQTT {P}/robots/{id}/audio
//   · §3.2 播报下行：MQTT {P}/robots/{id}/tts → piper 合成 → aplay 播报
//     同时把要念的文本发 ROS 话题 /voice/tts_text（给上层/看板留痕）
//   · TTS 串行队列线程：网关可能连发多条（"收到"+"已执行"），排队播不互相打断
//
// 【MQTT 订阅】robots/{id}/tts        【MQTT 发布】robots/{id}/audio
// 【没有遗嘱消息】LWT 只由 mqtt_bridge 注册（独占）。
//
// 【为什么不让 voice_source 直连 MQTT】麦克风采集节点保持纯 ROS：
// 这样"换采集方式（VAD/WAV/ptt.sh）"与"上云协议"互不影响，
// 且语音链路的采样率、格式、重试全部集中在这一个节点，遗嘱/背压不会散成三份。
//
// 实现文件：voice_link_node.cpp（构造/析构 + MQTT + 音频上行 + TTS 队列）
// ===========================================================================

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <mosquitto.h>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>

#include "quadruped_mqtt_bridge/common.hpp"

using namespace std::chrono_literals;

class VoiceLinkNode : public rclcpp::Node
{
public:

  VoiceLinkNode();
  ~VoiceLinkNode() override;

  // ---------------- MQTT 传输 ----------------
  void initMqtt();
  static void onConnectCb(struct mosquitto * /*mosq*/, void *obj, int rc);
  static void onDisconnectCb(struct mosquitto * /*mosq*/, void *obj, int rc);
  static void onMessageCb(struct mosquitto * /*mosq*/, void *obj,
                          const struct mosquitto_message *msg);

  // ---------------- 语音：§3.1 音频上行 / §3.2 piper 播报队列 ----------------
  void onAudioPcm(const std::vector<uint8_t> &pcm);
  void onTts(const std::string &text);
  void enqueueTts(const std::string & text);
  void ttsWorker();

  // ---------------- 成员变量 ----------------

  // 连接与身份（参数名与另外两个节点一致）
  std::string broker_host_, robot_id_, robot_name_;
  std::string topic_prefix_, username_, password_;
  std::string audio_topic_, tts_topic_;
  int broker_port_ = 1883, keepalive_ = 30;   // 语音节点单独放宽：124.8KB 单帧在 512kbit
                                             // 下传输 >10s，keepalive 10s 会被大帧顶掉，
                                             // 导致「已连接→断开」重连风暴（弱网 W6 实测）

  struct mosquitto *mosq_ = nullptr;

  // 语音（§3.1 上行 / §3.2 下行）
  bool voice_enabled_ = true;
  std::string audio_ros_topic_;
  int audio_sample_rate_ = 16000, audio_channels_ = 1;
  std::string piper_bin_, piper_model_;

  rclcpp::Subscription<std_msgs::msg::UInt8MultiArray>::SharedPtr sub_audio_;
  rclcpp::Subscription<std_msgs::msg::String>::SharedPtr sub_audio_info_;
  rclcpp::Publisher<std_msgs::msg::String>::SharedPtr pub_tts_text_;

  std::thread tts_thread_;
  std::mutex tts_mutex_;
  std::condition_variable tts_cv_;
  std::deque<std::string> tts_queue_;

  // 共享状态（MQTT 网络线程写 mqtt_connected_，采集回调读；格式信息同理）
  std::mutex data_mutex_;
  bool mqtt_connected_ = false;
};
