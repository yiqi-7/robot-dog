// ===========================================================================
// voice_link_node.cpp —— 语音链路节点：§3.1 音频上行（base64）+ §3.2 TTS 播报
//
// 拆分自原 mqtt_bridge 的 node_core.cpp（语音订阅/发布器装配）与
// node_voice.cpp（上行编码 + piper 队列）。行为不变：
//   · 上行报文 { robotId, ts, format:"pcm_s16le", sampleRate, channels, data }
//   · 下行报文 { cmdId, ts, text }（纯文本也兼容）
//   · 采样率/声道以采集端 latch 的 voice/audio_info 为准（曾经 16k/8k 错位
//     导致 ASR 全是噪声），参数只作兜底默认值。
// ===========================================================================

#include "quadruped_mqtt_bridge/voice_link_node.hpp"

VoiceLinkNode::VoiceLinkNode() : rclcpp::Node("voice_link")
{
  // ---------------- 参数（名字与默认值一律不变） ----------------
  broker_host_   = declare_parameter("broker_host", "127.0.0.1");
  broker_port_   = declare_parameter("broker_port", 1883);
  robot_id_      = declare_parameter("robot_id", "robot-01");
  robot_name_    = declare_parameter("robot_name", "仿真狗");
  topic_prefix_  = declare_parameter("topic_prefix", "qbot/v1");
  keepalive_     = declare_parameter("keepalive", 10);   // 与 mqtt_bridge 一致，掉线窗口 ≈ 15s
  username_      = declare_parameter("username", "");
  password_      = declare_parameter("password", "");

  // 语音链路（VOICE-INTERFACE.md §3.1/§3.2）
  voice_enabled_     = declare_parameter("voice_enabled", true);
  audio_ros_topic_   = declare_parameter("audio_topic", "voice/audio_pcm");
  audio_sample_rate_ = declare_parameter("audio_sample_rate", 16000);
  audio_channels_    = declare_parameter("audio_channels", 1);
  // TTS 本地播报：piper（模型 course 提供 zh_CN-huayan-medium）
  piper_bin_   = declare_parameter("piper_bin", "/home/wby/venvs/piper/bin/piper");
  piper_model_ = declare_parameter("piper_model",
    "/run/media/wby/shuju/Document/robot-dog/quadruped-ops/models/piper/zh_CN-huayan-medium.onnx");

  audio_topic_ = topic_prefix_ + "/robots/" + robot_id_ + "/audio";
  tts_topic_   = topic_prefix_ + "/robots/" + robot_id_ + "/tts";

  // ---------------- 语音上行（§3.1） ----------------
  // voice_source 把"一句话"发成 16 kHz 单声道 PCM 片段，这里转成 §3.1 的
  // JSON（base64）上行。格式信息走 latch 话题，采样率/声道以采集端为准，
  // 避免桥这边参数写死和采集端不一致。
  pub_tts_text_ = create_publisher<std_msgs::msg::String>("voice/tts_text", 10);
  if (voice_enabled_) {
    sub_audio_ = create_subscription<std_msgs::msg::UInt8MultiArray>(
      audio_ros_topic_, rclcpp::QoS(10),
      [this](std_msgs::msg::UInt8MultiArray::SharedPtr msg) { onAudioPcm(msg->data); });

    sub_audio_info_ = create_subscription<std_msgs::msg::String>(
      "voice/audio_info", rclcpp::QoS(rclcpp::KeepLast(1)).transient_local().reliable(),
      [this](std_msgs::msg::String::SharedPtr msg) {
        try {
          const json j = json::parse(msg->data);
          std::lock_guard<std::mutex> lk(data_mutex_);
          audio_sample_rate_ = j.value("sampleRate", audio_sample_rate_);
          audio_channels_    = j.value("channels", audio_channels_);
          RCLCPP_INFO(get_logger(), "语音格式：%d Hz / %d 声道",
            audio_sample_rate_, audio_channels_);
        } catch (const std::exception &) { /* 格式信息损坏就沿用默认值 */ }
      });
  }

  // ---------------- MQTT ----------------
  initMqtt();

  // TTS 队列线程：合成/播放都是阻塞的系统调用，必须在线程里做，
  // 否则 MQTT 回调线程被 aplay 卡住整个语音链路（含上行）。
  if (voice_enabled_) {
    tts_thread_ = std::thread([this] { ttsWorker(); });
    RCLCPP_INFO(get_logger(), "语音链路已启用：ROS %s → MQTT %s ；播报 %s → /voice/tts_text",
      audio_ros_topic_.c_str(), audio_topic_.c_str(), tts_topic_.c_str());
  }

  RCLCPP_INFO(get_logger(),
    "MQTT 语音节点已启动%s\n"
    "  Broker     : %s:%d\n"
    "  机器人     : %s (%s)\n"
    "  音频上行   : %s → %s\n"
    "  播报下行   : %s → piper → aplay（串行队列）\n"
    "  遗嘱消息   : 不注册（由 mqtt_bridge 独占）",
    voice_enabled_ ? "" : "（voice_enabled=false，仅占位）",
    broker_host_.c_str(), broker_port_,
    robot_id_.c_str(), robot_name_.c_str(),
    voice_enabled_ ? audio_ros_topic_.c_str() : "未启用", audio_topic_.c_str(),
    tts_topic_.c_str());
}

VoiceLinkNode::~VoiceLinkNode()
{
  if (tts_thread_.joinable()) {
    tts_cv_.notify_all();
    tts_thread_.join();
  }
  if (mosq_) {
    mosquitto_disconnect(mosq_);        // 正常断开（本节点无遗嘱）
    mosquitto_loop_stop(mosq_, false);
    mosquitto_destroy(mosq_);
    mosq_ = nullptr;
  }
  mosquitto_lib_cleanup();
}


// =========================================================================
// MQTT 初始化与回调
// =========================================================================
void VoiceLinkNode::initMqtt()
{
  mosquitto_lib_init();
  // 客户端 ID 必须与 mqtt_bridge / task_runner 不同，否则同 ID 连接会被
  // Broker 互相顶掉（表现为语音上行偶发断流、播报偶尔收不到）。
  const std::string client_id = robot_id_ + "-voice";
  mosq_ = mosquitto_new(client_id.c_str(), true, this);
  if (!mosq_) {
    RCLCPP_ERROR(get_logger(), "mosquitto_new 失败");
    return;
  }

  // 本节点【不注册遗嘱消息】：LWT 只归 mqtt_bridge（独占 state 主题的 offline）。

  if (!username_.empty())
    mosquitto_username_pw_set(mosq_, username_.c_str(), password_.c_str());

  mosquitto_connect_callback_set(mosq_, &VoiceLinkNode::onConnectCb);
  mosquitto_message_callback_set(mosq_, &VoiceLinkNode::onMessageCb);
  mosquitto_disconnect_callback_set(mosq_, &VoiceLinkNode::onDisconnectCb);

  int rc = mosquitto_connect_async(mosq_, broker_host_.c_str(), broker_port_, keepalive_);
  if (rc != MOSQ_ERR_SUCCESS) {
    RCLCPP_ERROR(get_logger(), "connect_async 失败: %s", mosquitto_strerror(rc));
    return;
  }
  mosquitto_loop_start(mosq_);   // 独立网络线程，内部自动重连
}

void VoiceLinkNode::onConnectCb(struct mosquitto * /*mosq*/, void *obj, int rc)
{
  auto *self = static_cast<VoiceLinkNode *>(obj);
  if (rc != 0) {
    RCLCPP_WARN(self->get_logger(), "MQTT 连接被拒绝: %s", mosquitto_connack_string(rc));
    return;
  }
  {
    std::lock_guard<std::mutex> lk(self->data_mutex_);
    self->mqtt_connected_ = true;
  }
  RCLCPP_INFO(self->get_logger(), "MQTT 已连接 %s:%d（语音节点）",
    self->broker_host_.c_str(), self->broker_port_);

  // 语音播报（§3.2）：网关把要念给机器人听的话发到 tts 主题
  if (self->voice_enabled_) {
    int rc_tts = mosquitto_subscribe(self->mosq_, nullptr, self->tts_topic_.c_str(), 1);
    if (rc_tts == MOSQ_ERR_SUCCESS) {
      RCLCPP_INFO(self->get_logger(), "已订阅语音播报 %s", self->tts_topic_.c_str());
    } else {
      RCLCPP_ERROR(self->get_logger(), "订阅 tts 失败: %s", mosquitto_strerror(rc_tts));
    }
  }
}

void VoiceLinkNode::onDisconnectCb(struct mosquitto * /*mosq*/, void *obj, int rc)
{
  auto *self = static_cast<VoiceLinkNode *>(obj);
  {
    std::lock_guard<std::mutex> lk(self->data_mutex_);
    self->mqtt_connected_ = false;
  }
  if (rc == 0) RCLCPP_INFO(self->get_logger(), "MQTT 正常断开");
  else RCLCPP_WARN(self->get_logger(), "MQTT 意外断开 (rc=%d)，自动重连中", rc);
}


// =========================================================================
// 下行报文入口（运行在 MQTT 网络线程）：只认 tts 主题
// =========================================================================
void VoiceLinkNode::onMessageCb(struct mosquitto * /*mosq*/, void *obj,
                                const struct mosquitto_message *msg)
{
  if (!msg || !msg->payload || msg->payloadlen <= 0) return;
  auto *self = static_cast<VoiceLinkNode *>(obj);
  const std::string topic = msg->topic ? msg->topic : "";
  const std::string text(static_cast<const char *>(msg->payload), msg->payloadlen);

  // ---- 语音播报（§3.2）：念给机器人的话，转给本地 TTS ----
  if (topic == self->tts_topic_) {
    self->onTts(text);
    return;
  }

  // 每个节点只订阅自己的主题；其它主题只可能是通配符笔误，DEBUG 记一笔即可
  RCLCPP_DEBUG(self->get_logger(), "voice_link 忽略未知主题 %s", topic.c_str());
}


// =========================================================================
// 语音上行：PCM 片段 → {P}/robots/{id}/audio
// 报文格式见 VOICE-INTERFACE.md §3.1（QoS1，非 retained）
// =========================================================================
void VoiceLinkNode::onAudioPcm(const std::vector<uint8_t> &pcm)
{
  int rate = 16000, ch = 1;
  bool connected = false;
  {
    std::lock_guard<std::mutex> lk(data_mutex_);
    rate = audio_sample_rate_;
    ch = audio_channels_;
    connected = mqtt_connected_;
  }
  if (!connected || pcm.empty()) return;

  const size_t bytes = pcm.size() - (pcm.size() % 2);   // 16bit 样本必须整对齐
  if (bytes == 0) return;

  const double dur = static_cast<double>(bytes) / (2.0 * ch * rate);
  json j = {
    {"robotId",    robot_id_},
    {"ts",         qmb::wallClockMs()},
    {"format",     "pcm_s16le"},
    {"sampleRate", rate},
    {"channels",   ch},
    {"data",       qmb::base64Encode(pcm.data(), bytes)},
  };
  const std::string payload = j.dump();
  const int rc = qmb::publishText(mosq_, audio_topic_, payload, 1, /*retain=*/false);
  if (rc == MOSQ_ERR_SUCCESS) {
    RCLCPP_INFO(get_logger(), "语音已上行：%.2f s（%zu KB PCM → %zu KB base64）",
      dur, bytes / 1024, payload.size() / 1024);
  } else {
    RCLCPP_WARN(get_logger(), "语音上行失败：%s", mosquitto_strerror(rc));
  }
}


// =========================================================================
// 语音播报下行（§3.2）：转成 ROS 话题 + 本地 piper 播报
// =========================================================================
void VoiceLinkNode::onTts(const std::string &text)
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
void VoiceLinkNode::enqueueTts(const std::string & text)
{
  if (text.empty()) return;
  {
    std::lock_guard<std::mutex> lk(tts_mutex_);
    tts_queue_.push_back(text);
  }
  tts_cv_.notify_one();
}

void VoiceLinkNode::ttsWorker()
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
