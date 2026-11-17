// ===========================================================================
// node_voice.cpp —— 语音：§3.1 音频上行（base64）+ §3.2 piper 播报队列
// 从原 mqtt_bridge.cpp 拆出，行为不变；类声明见 mqtt_bridge_node.hpp
// ===========================================================================

#include "quadruped_mqtt_bridge/mqtt_bridge_node.hpp"


  // =========================================================================
  // 语音上行：PCM 片段 → {P}/robots/{id}/audio
  // 报文格式见 VOICE-INTERFACE.md §3.1（QoS1，非 retained）
  // =========================================================================
  void MqttBridge::onAudioPcm(const std::vector<uint8_t> &pcm) 
  {
    if (!mqtt_connected_ || pcm.empty()) return;
    const size_t bytes = pcm.size() - (pcm.size() % 2);   // 16bit 样本必须整对齐
    if (bytes == 0) return;

    const double dur = static_cast<double>(bytes) /
                       (2.0 * audio_channels_ * audio_sample_rate_);
    json j = {
      {"robotId",    robot_id_},
      {"ts",         wallClockMs()},
      {"format",     "pcm_s16le"},
      {"sampleRate", audio_sample_rate_},
      {"channels",   audio_channels_},
      {"data",       base64Encode(pcm.data(), bytes)},
    };
    const std::string payload = j.dump();
    const int rc = mosquitto_publish(mosq_, nullptr, audio_topic_.c_str(),
      static_cast<int>(payload.size()), payload.c_str(), 1, false);
    if (rc == MOSQ_ERR_SUCCESS) {
      RCLCPP_INFO(get_logger(), "语音已上行：%.2f s（%zu KB PCM → %zu KB base64）",
        dur, bytes / 1024, payload.size() / 1024);
    } else {
      RCLCPP_WARN(get_logger(), "语音上行失败：%s", mosquitto_strerror(rc));
    }
  }


  // =========================================================================
  // 语音播报下行（§3.2）：仿真里没有喇叭，转成 ROS 话题让上层/TTS 节点处理
  // =========================================================================
  void MqttBridge::onTts(const std::string &text) 
  {
    std::string say = text;
    std::string cmd_id;
    try {
      const json j = json::parse(text);
      say = j.value("text", "");
      cmd_id = j.value("cmdId", "");
    } catch (const std::exception &) { /* 允许纯文本报文 */ }
    if (say.empty()) return;

    RCLCPP_INFO(get_logger(), "语音播报%s：%s",
      cmd_id.empty() ? "" : (" [" + cmd_id + "]").c_str(), say.c_str());
    std_msgs::msg::String m;
    m.data = say;
    pub_tts_text_->publish(m);
    enqueueTts(say);
  }


  // =========================================================================
  // TTS 本地播报（课程标准）：piper 合成 zh_CN-huayan-medium → aplay 播放。
  // 队列化：网关可能连发多条（"收到"+"已执行"），串行播不互相打断。
  // =========================================================================
  void MqttBridge::enqueueTts(const std::string & text) 
  {
    if (text.empty()) return;
    {
      std::lock_guard<std::mutex> lk(tts_mutex_);
      tts_queue_.push_back(text);
    }
    tts_cv_.notify_one();
  }

  void MqttBridge::ttsWorker() 
  {
    while (rclcpp::ok()) {
      std::string text;
      {
        std::unique_lock<std::mutex> lk(tts_mutex_);
        tts_cv_.wait(lk, [this] { return !tts_queue_.empty() || !rclcpp::ok(); });
        if (tts_queue_.empty()) continue;
        text = tts_queue_.front();
        tts_queue_.pop_front();
      }
      RCLCPP_INFO(get_logger(), "TTS 播报: %s", text.c_str());
      const std::string inFile = "/tmp/qbot_tts_in.txt";
      const std::string outFile = "/tmp/qbot_tts.wav";
      {
        std::ofstream f(inFile);
        f << text << "\n";
      }
      const std::string synth = piper_bin_ + " -m " + piper_model_ +
        " -f " + outFile + " < " + inFile + " 2>/dev/null";
      if (std::system(synth.c_str()) == 0) {
        std::system(("aplay -q " + outFile + " 2>/dev/null").c_str());
      } else {
        // 没装 piper / 没有音频输出设备时只留日志，不影响链路
        RCLCPP_WARN(get_logger(), "TTS 合成/播放失败（piper 不可用或无声卡输出）");
      }
    }
  }
