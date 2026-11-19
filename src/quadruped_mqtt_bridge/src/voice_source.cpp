// 语音采集节点：把"一句话"切成 16 kHz 单声道 PCM 裸流，发布到 ROS 侧，
// 由 mqtt_bridge 按 VOICE-INTERFACE.md §3.1 上行到 {P}/robots/{id}/audio。
//
// 设计要点：
// 1. 分段策略 = 能量 VAD（静音检测）。语音网关那边要求"唤醒后录 2~8 秒"，
//    所以这里不做连续流，而是一段一段发：检测到有人说话就开始攒，
//    静音超过 silence_ms 或攒够 max_ms 就收尾发布。太短（< min_ms）丢弃，
//    避免把咳嗽/键盘声当指令送给 ASR 白烧算力。
// 2. 预滚（preroll）：VAD 判定"开始说话"的那一帧，其实首音节已经过去了半个
//    帧长以上，直接从这个帧开始会让 ASR 少听头一个字。所以空闲时始终缓存
//    最近 preroll_ms 毫秒的音频，触发时一起带上。实测 zh.wav 不带预滚时
//    首字"开"会被吃掉。
// 3. 采集实现用 popen 调 arecord 拿裸 PCM，不引入 ALSA/portal 依赖。
//    代价是没法精细控制缓冲延迟，对本用途（秒级语音片段）完全够。
// 4. mode:=file 用 WAV 文件当"假麦克风"：这台 TITAN V 的 ALC897 实测没插
//    麦克风（arecord 3 秒 RMS=0.0000），但 ASR/大模型/指令链路必须能在
//    无人对着机器狗喊话的情况下验证，所以保留文件注入通道。
// 5. 音频格式随 voice/audio_info（transient-local）latch 发布，mqtt_bridge
//    读它填 §3.1 的 sampleRate/channels，避免两处参数各自写死导致错位。
//
// 运行：
//   ros2 run quadruped_mqtt_bridge voice_source                # 麦克风 + VAD
//   ros2 run quadruped_mqtt_bridge voice_source --ros-args \
//     -p mode:=file -p wav_path:=<某个.wav> -p loop:=true
//   ros2 service call /voice/capture std_srvs/srv/Trigger     # 手动录一段（PTT）

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int8_multi_array.hpp>
#include <std_srvs/srv/trigger.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

using std_msgs::msg::String;
using std_msgs::msg::UInt8MultiArray;
using Trigger = std_srvs::srv::Trigger;

namespace {

// 16 bit 小端 PCM 的 RMS（0~1 归一化）。只看第一声道，与网关 audioStats 一致。
double frameRms(const uint8_t *data, size_t bytes, int channels)
{
  const size_t frames = bytes / (2 * static_cast<size_t>(channels));
  if (frames == 0) return 0.0;
  double sum = 0.0;
  for (size_t i = 0; i < frames; ++i) {
    const int16_t v = static_cast<int16_t>(data[i * 2 * channels] |
                                           (data[i * 2 * channels + 1] << 8));
    const double d = static_cast<double>(v) / 32768.0;
    sum += d * d;
  }
  return std::sqrt(sum / static_cast<double>(frames));
}

// 手写最小 WAV 解析：只支持 PCM 16 bit（SenseVoice 测试样本就是这种）。
// 逐块扫描而不是假设 fmt/data 顺序固定，因为不同工具导出的块顺序不一样。
struct WavData {
  bool ok = false;
  int channels = 1;
  int rate = 16000;
  std::vector<uint8_t> pcm;    // 裸 16bit 小端样本
  std::string err;
};

WavData loadWav(const std::string &path)
{
  WavData w;
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) { w.err = "打不开文件"; return w; }
  char riff[4] = {0};
  std::fread(riff, 1, 4, f);
  if (std::memcmp(riff, "RIFF", 4) != 0) {
    w.err = "不是 RIFF/WAVE"; std::fclose(f); return w;
  }
  char wave[4] = {0};
  std::fseek(f, 8, SEEK_SET);
  std::fread(wave, 1, 4, f);
  if (std::memcmp(wave, "WAVE", 4) != 0) {
    w.err = "不是 WAVE"; std::fclose(f); return w;
  }

  bool have_fmt = false;
  uint16_t fmt_tag = 0, fmt_ch = 1, fmt_bits = 16;
  uint32_t fmt_rate = 16000;

  uint32_t pos = 12;
  for (int guard = 0; guard < 64; ++guard) {
    if (std::fseek(f, pos, SEEK_SET) != 0) break;
    char id[4] = {0};
    uint32_t size = 0;
    if (std::fread(id, 1, 4, f) != 4 || std::fread(&size, 4, 1, f) != 1) break;
    if (std::memcmp(id, "fmt ", 4) == 0) {
      std::fread(&fmt_tag, 2, 1, f);
      std::fread(&fmt_ch, 2, 1, f);
      std::fread(&fmt_rate, 4, 1, f);
      std::fseek(f, static_cast<long>(pos) + 16 + 6, SEEK_SET);
      std::fread(&fmt_bits, 2, 1, f);
      have_fmt = true;
    } else if (std::memcmp(id, "data", 4) == 0 && have_fmt) {
      w.channels = fmt_ch;
      w.rate = static_cast<int>(fmt_rate);
      if (fmt_tag != 1) { w.err = "非 PCM 编码（需要 16bit PCM）"; std::fclose(f); return w; }
      if (fmt_bits != 16) { w.err = "非 16bit（当前 " + std::to_string(fmt_bits) + "bit）"; std::fclose(f); return w; }
      const size_t n = size;
      w.pcm.resize(n);
      std::fseek(f, static_cast<long>(pos) + 8, SEEK_SET);
      const size_t got = std::fread(w.pcm.data(), 1, n, f);
      w.pcm.resize(got - (got % 2));       // 必须整样本对齐
      w.ok = got > 0;
      if (!w.ok) w.err = "data 块为空";
      std::fclose(f);
      return w;
    }
    pos += 8 + size + (size & 1u);          // 块按偶数字节对齐
  }
  w.err = "没找到 data 块";
  std::fclose(f);
  return w;
}

}  // namespace

class VoiceSource : public rclcpp::Node
{
public:
  VoiceSource() : rclcpp::Node("voice_source")
  {
    // ---------------- 参数 ----------------
    mode_         = declare_parameter("mode", "vad");        // vad | file
    device_       = declare_parameter("device", "default");  // ALSA 捕获设备名
    sample_rate_  = declare_parameter("sample_rate", 16000);
    channels_     = declare_parameter("channels", 1);
    frame_ms_     = declare_parameter("frame_ms", 20);       // VAD 帧长
    thresh_       = declare_parameter("thresh", 0.02);       // 起说 RMS 门限
    silence_ms_   = declare_parameter("silence_ms", 900);    // 收尾静音时长
    min_ms_       = declare_parameter("min_ms", 700);        // 短于此时丢弃
    max_ms_       = declare_parameter("max_ms", 8000);       // 硬上限
    preroll_ms_   = declare_parameter("preroll_ms", 240);    // 补回说话前的缓冲
    wav_path_     = declare_parameter("wav_path", "");
    loop_         = declare_parameter("loop", false);
    gap_s_        = declare_parameter("gap_s", 3.0);

    if (mode_ != "vad" && mode_ != "file") {
      RCLCPP_ERROR(get_logger(), "mode 只支持 vad / file，收到 %s，按 vad 跑", mode_.c_str());
      mode_ = "vad";
    }

    // ---------------- 接口 ----------------
    // 大消息（5 秒 16k 单声道 = 160 KB），用可靠 QoS、深度给足
    pub_pcm_   = create_publisher<UInt8MultiArray>("voice/audio_pcm", rclcpp::QoS(10));
    pub_info_  = create_publisher<String>("voice/audio_info",
                      rclcpp::QoS(rclcpp::KeepLast(1)).transient_local().reliable());
    pub_status_ = create_publisher<String>("voice/source_status", 10);

    // 课程标准按键说话接口：ptt.sh 用 `ros2 topic pub /voice/ptt` 触发，
    // true 开录、false 手动收尾。和 voice/capture 服务共用同一套状态机。
    sub_ptt_ = create_subscription<std_msgs::msg::Bool>(
      "voice/ptt", 10,
      [this](std_msgs::msg::Bool::SharedPtr msg) {
        if (msg->data) {
          if (mode_ == "file") ptt_file_ = true; else ptt_armed_ = true;
          RCLCPP_INFO(get_logger(), "PTT 按下（%s）", mode_.c_str());
        } else {
          ptt_stop_ = true;      // VAD 循环里检测到就立即收尾
          ptt_file_ = false;
          RCLCPP_INFO(get_logger(), "PTT 松开，收尾上传");
        }
      });

    // PTT（按住说话/服务触发）：让"手动录一段"和 VAD 自动分段共用一套循环
    srv_capture_ = create_service<Trigger>(
      "voice/capture",
      [this](const std::shared_ptr<Trigger::Request>,
             std::shared_ptr<Trigger::Response> resp) {
        if (mode_ == "file") {
          ptt_file_ = true;                       // 文件模式下 = 重新发一遍样本
          resp->success = true;
          resp->message = "已排队重发音频文件";
        } else {
          ptt_armed_ = true;                       // 麦克风模式下 = 强制开录
          resp->success = true;
          resp->message = "开始录制（最长 " + std::to_string(max_ms_ / 1000) + " s）";
        }
      });

    publishInfo();
    // 下面这行原本把三元表达式（结果是 std::string）直接传给 %s，
    // 属于未定义行为。先落成局部变量再取 c_str()，别让它进变参列表。
    const std::string mode_suffix =
      (mode_ == "file") ? (" (" + wav_path_ + (loop_ ? ", loop" : "") + ")")
                        : std::string();
    RCLCPP_INFO(get_logger(),
      "语音采集已启动\n"
      "  模式     : %s%s\n"
      "  采样     : %d Hz / %d 声道 / 16bit\n"
      "  VAD      : 帧 %d ms，门限 %.3f，静音 %d ms 收尾，%.1f~%.1f s\n"
      "  话题     : voice/audio_pcm（片段）+ voice/audio_info（格式）\n"
      "  服务     : voice/capture（std_srvs/srv/Trigger）\n"
      "  按键说话 : voice/ptt（std_msgs/Bool，true 开录 / false 收尾）",
      mode_.c_str(),
      mode_suffix.c_str(),
      sample_rate_, channels_, frame_ms_, thresh_, silence_ms_,
      min_ms_ / 1000.0, max_ms_ / 1000.0);

    worker_ = std::thread([this]() { run(); });
  }

  ~VoiceSource() override
  {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
  }

private:
  // =========================================================================
  // 元数据：告诉 mqtt_bridge 现在的采样率/声道（§3.1 的 sampleRate/channels）
  // =========================================================================
  void publishInfo()
  {
    String m;
    m.data = "{\"format\":\"pcm_s16le\",\"sampleRate\":" + std::to_string(sample_rate_) +
             ",\"channels\":" + std::to_string(channels_) + ",\"source\":\"" + mode_ + "\"}";
    pub_info_->publish(m);
  }

  void publishStatus(const std::string &state, double dur_s, double rms)
  {
    String m;
    m.data = "{\"state\":\"" + state + "\",\"segments\":" + std::to_string(segment_count_) +
             ",\"lastDuration\":" + std::to_string(dur_s).substr(0, 4) +
             ",\"lastRms\":" + std::to_string(rms).substr(0, 6) +
             ",\"mode\":\"" + mode_ + "\"}";
    pub_status_->publish(m);
  }

  // =========================================================================
  // 片段发布：UInt8MultiArray 装裸 PCM，字节数必须偶数（16bit 对齐）
  // =========================================================================
  void publishSegment(std::vector<uint8_t> &&pcm, double rms, const char *why)
  {
    if (pcm.size() % 2) pcm.pop_back();
    const double dur = static_cast<double>(pcm.size()) /
                       (2.0 * channels_ * sample_rate_);
    if (pcm.empty() || dur * 1000.0 < min_ms_) {
      RCLCPP_INFO(get_logger(), "片段过短（%.2f s < %.1f s）丢弃 [%s]",
        dur, min_ms_ / 1000.0, why);
      publishStatus("discarded", dur, rms);
      return;
    }
    UInt8MultiArray m;
    m.data = std::move(pcm);
    pub_pcm_->publish(m);
    ++segment_count_;
    RCLCPP_INFO(get_logger(), "已发布语音片段 #%d：%.2f s（%zu 字节，RMS=%.3f）[%s]",
      segment_count_, m.data.size() ? dur : 0.0, m.data.size(), rms, why);
    publishStatus("sent", dur, rms);
  }

  // =========================================================================
  // 主循环
  // =========================================================================
  void run()
  {
    if (mode_ == "file") runFile(); else runVad();
  }

  // ---------------- 文件模式：WAV 当假麦克风 ----------------
  void runFile()
  {
    do {
      WavData w = loadWav(wav_path_);
      if (!w.ok) {
        RCLCPP_ERROR(get_logger(), "读 WAV 失败（%s）：%s，3 s 后重试",
          wav_path_.c_str(), w.err.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(3));
        continue;
      }
      // 文件里写的采样率/声道才是真相，用它覆盖参数，元数据同步重发
      sample_rate_ = w.rate;
      channels_ = w.channels;
      publishInfo();
      const double dur = static_cast<double>(w.pcm.size()) /
                         (2.0 * channels_ * sample_rate_);
      RCLCPP_INFO(get_logger(), "音频文件 %s：%.2f s @%d Hz/%d ch，开始上行",
        wav_path_.c_str(), dur, w.rate, w.channels);

      // 等订阅者匹配再发：文件模式起来 1 ms 就发布，此时 DDS 发现还没完成，
      // 可靠 QoS 在没有匹配订阅者时不会缓存而是直接丢弃（实测首段必丢）。
      // 真机上采集节点也可能比 mqtt_bridge 先起，所以这里统一等一下。
      const auto match_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
      while (rclcpp::ok() && !stop_ && pub_pcm_->get_subscription_count() == 0 &&
             std::chrono::steady_clock::now() < match_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (pub_pcm_->get_subscription_count() == 0) {
        RCLCPP_WARN(get_logger(), "10 s 内没有订阅者匹配 voice/audio_pcm，仍尝试发布");
      } else {
        RCLCPP_INFO(get_logger(), "订阅者已匹配（%zu 个），开始发布",
          pub_pcm_->get_subscription_count());
      }

      // 前面垫 300 ms 静音，模拟"先按下录音键再开口"，也让网关的质量守卫
      // 看到真实的起止能量包络
      std::vector<uint8_t> seg(static_cast<size_t>(0.3 * sample_rate_) * 2 * channels_, 0);
      seg.insert(seg.end(), w.pcm.begin(), w.pcm.end());
      publishSegment(std::move(seg), frameRms(w.pcm.data(), w.pcm.size(), channels_), "file");

      if (!loop_) {
        // 单次模式：发完就待命，等 voice/capture 服务再发一次
        while (!stop_ && rclcpp::ok()) {
          if (ptt_file_) { ptt_file_ = false; break; }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stop_ || !rclcpp::ok()) return;
        continue;
      }
      const auto until = std::chrono::steady_clock::now() +
                         std::chrono::duration<double>(gap_s_);
      while (!stop_ && rclcpp::ok() && std::chrono::steady_clock::now() < until) {
        if (ptt_file_) { ptt_file_ = false; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    } while (!stop_ && rclcpp::ok());
  }

  // ---------------- VAD 模式：arecord + 能量分段 ----------------
  void runVad()
  {
    char cmd[256];
    std::snprintf(cmd, sizeof(cmd),
      "arecord -q -D %s -f S16_LE -r %d -c %d -t raw 2>/dev/null",
      device_.c_str(), sample_rate_, channels_);

    while (!stop_ && rclcpp::ok()) {
      // mode 只能写 "r"，不能写 "rb"。
      // glibc 2.43 的 popen 会校验 mode 字符串，只接受 "r"/"w"（可带 "e"），
      // 传 "rb" 直接返回 NULL 且 errno=EINVAL——而 fopen 是接受 "rb" 的，
      // 很容易照着 fopen 的习惯写错。实测本机 ldd 2.43 上 "rb" 必 NULL、
      // "r" 正常；这个错误会让语音采集每 3 s 失败重试一次、永远录不到音。
      FILE *pipe = ::popen(cmd, "r");
      if (!pipe) {
        RCLCPP_ERROR(get_logger(), "启动 arecord 失败：%s（errno=%d），3 s 后重试",
          cmd, errno);
        std::this_thread::sleep_for(std::chrono::seconds(3));
        continue;
      }
      RCLCPP_INFO(get_logger(), "麦克风采集中：%s（门限 RMS %.3f）", device_.c_str(), thresh_);

      const size_t frame_samples =
        static_cast<size_t>(sample_rate_) * frame_ms_ / 1000;         // 每帧每声道样本数
      const size_t frame_bytes = frame_samples * 2 * channels_;
      const size_t silence_frames_needed =
        std::max<size_t>(1, static_cast<size_t>(silence_ms_) / frame_ms_);
      const size_t preroll_frames =
        std::max<size_t>(1, static_cast<size_t>(preroll_ms_) / frame_ms_);
      const double frame_rms_scale = 1.0;   // 帧长已含在 frame_bytes 里

      std::vector<uint8_t> buf(frame_bytes);
      std::deque<std::vector<uint8_t>> preroll;
      std::vector<uint8_t> seg;
      bool speaking = false;
      size_t silence_frames = 0;
      double seg_rms_sum = 0.0;
      size_t seg_frames = 0;

      while (!stop_ && rclcpp::ok()) {
        const size_t got = std::fread(buf.data(), 1, frame_bytes, pipe);
        if (got != frame_bytes) break;      // arecord 退出/设备断开

        const double rms = frameRms(buf.data(), got, channels_) * frame_rms_scale;
        const bool voiced = rms >= thresh_;

        if (!speaking) {
          if (voiced || ptt_armed_) {
            speaking = true;
            ptt_armed_ = false;
            seg.clear();
            for (auto &f : preroll) seg.insert(seg.end(), f.begin(), f.end());
            preroll.clear();
            seg_rms_sum = 0.0;
            seg_frames = 0;
            silence_frames = 0;
            RCLCPP_INFO(get_logger(), "检测到语音（RMS=%.3f），开始攒片段", rms);
          } else {
            preroll.push_back(buf);
            if (preroll.size() > preroll_frames) preroll.pop_front();
          }
        } else {
          seg.insert(seg.end(), buf.begin(), buf.end());
          seg_rms_sum += rms;
          ++seg_frames;
          if (voiced) silence_frames = 0; else ++silence_frames;

          const double seg_ms =
            static_cast<double>(seg.size()) / (2.0 * channels_ * sample_rate_) * 1000.0;
          if (ptt_stop_.exchange(false)) {
            // 手动松开 PTT：不等静音窗口，立刻收尾
            speaking = false;
            publishSegment(std::move(seg),
              seg_frames ? seg_rms_sum / seg_frames : 0.0, "ptt-release");
            seg.clear();
          } else if (silence_frames >= silence_frames_needed) {
            speaking = false;
            publishSegment(std::move(seg),
              seg_frames ? seg_rms_sum / seg_frames : 0.0, "vad-silence");
            seg.clear();
          } else if (seg_ms >= max_ms_) {
            speaking = false;
            publishSegment(std::move(seg),
              seg_frames ? seg_rms_sum / seg_frames : 0.0, "vad-maxlen");
            seg.clear();
          }
        }
      }
      ::pclose(pipe);
      if (!stop_ && rclcpp::ok()) {
        RCLCPP_WARN(get_logger(), "arecord 中断，1 s 后重开");
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
  }

  // ---------------- 成员 ----------------
  std::string mode_, device_, wav_path_;
  int sample_rate_ = 16000, channels_ = 1;
  int frame_ms_ = 20, silence_ms_ = 900, min_ms_ = 700, max_ms_ = 8000, preroll_ms_ = 240;
  double thresh_ = 0.02, gap_s_ = 3.0;
  bool loop_ = false;

  rclcpp::Subscription<std_msgs::msg::Bool>::SharedPtr sub_ptt_;

  rclcpp::Publisher<UInt8MultiArray>::SharedPtr pub_pcm_;
  rclcpp::Publisher<String>::SharedPtr pub_info_, pub_status_;
  rclcpp::Service<Trigger>::SharedPtr srv_capture_;

  std::thread worker_;
  std::atomic<bool> stop_{false}, ptt_armed_{false}, ptt_file_{false}, ptt_stop_{false};
  int segment_count_ = 0;
};

int main(int argc, char **argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<VoiceSource>());
  rclcpp::shutdown();
  return 0;
}
