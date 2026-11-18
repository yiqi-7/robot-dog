// ============================================================================
// image_sender.cpp —— 视觉上云节点（课程 4.1 图像采集与压缩 + 4.2 视觉上云）
//
// 双路上报（同一帧处理，两个去向）：
//   ① MQTT 快照帧：OSD 烧录 -> JPEG(q90) -> qbot/v1/robots/{id}/camera
//      （二进制 JPEG，QoS 0，节流 jpeg_fps）
//   ② RTSP 实时流：OSD 烧录 -> 内嵌 ffmpeg 子进程 -> H.264(禁 B 帧)
//      -> rtsp://{server}:8554/cam/{robot_id} -> MediaMTX -> 看板 WebRTC
//
// 帧处理流水线（每帧都做）：
//   /rgb_camera (rgb8 640x480) -> cv::Mat(需要 BGR，先 cvtColor)
//   -> cv::putText 烧录 时间戳/机器人ID/位姿 -> 双路分发
//
// H.264 关键点（用户要求）：
//   -bf 0        禁用 B 帧（低延迟，解码端不依赖未来帧）
//   -tune zerolatency + -preset ultrafast  实时优先
//   -profile:v baseline  兼容 WebRTC 浏览器解码
// ============================================================================

#include <mosquitto.h>
#include <rclcpp/rclcpp.hpp>
#include <sensor_msgs/msg/image.hpp>
#include <nav_msgs/msg/odometry.hpp>

#include <opencv2/opencv.hpp>

#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

class ImageSender : public rclcpp::Node
{
public:
  ImageSender() : Node("image_sender")
  {
    // ffmpeg 管道对端死亡时 fwrite 会触发 SIGPIPE（默认终止本进程）。
    // 忽略之，让 fwrite 返回错误走"重启 ffmpeg 子进程"逻辑
    signal(SIGPIPE, SIG_IGN);
    // ---------------- 参数 ----------------
    robot_id_   = declare_parameter("robot_id", "robot-01");
    // JPEG 压缩质量 1~100：用户要求默认 90，可命令行 -p jpeg_quality:=N 调整
    jpeg_quality_ = declare_parameter("jpeg_quality", 90);
    jpeg_fps_   = declare_parameter("jpeg_fps", 1.0);   // MQTT 快照节流
    broker_     = declare_parameter("broker_host", "127.0.0.1");
    broker_port_ = declare_parameter("broker_port", 1883);
    rtsp_server_ = declare_parameter("rtsp_server", "43.172.91.67");
    rtsp_port_  = declare_parameter("rtsp_port", 8554);
    width_  = declare_parameter("width", 640);
    height_ = declare_parameter("height", 480);
    // MQTT 快照独立分辨率：与 RTSP 流解耦，兜底帧不需要和视频流同分辨率
    snapshot_width_  = declare_parameter("snapshot_width", 640);
    snapshot_height_ = declare_parameter("snapshot_height", 360);
    max_snapshot_bytes_ = declare_parameter("max_snapshot_bytes", 200 * 1024);
    fps_    = declare_parameter("fps", 15.0);

    cam_topic_  = declare_parameter("cam_topic", "rgb_camera");
    topic_prefix_ = declare_parameter("topic_prefix", "qbot/v1");

    state_topic_ = topic_prefix_ + "/robots/" + robot_id_ + "/camera";

    // ---------------- ROS 订阅 ----------------
    // 位姿用于 OSD 烧录（odom 系坐标 + 速度）
    sub_odom_ = create_subscription<nav_msgs::msg::Odometry>(
      "odom", 10,
      [this](nav_msgs::msg::Odometry::SharedPtr msg) {
        std::lock_guard<std::mutex> lk(data_mutex_);
        pos_x_ = msg->pose.pose.position.x;
        pos_y_ = msg->pose.pose.position.y;
        speed_ = std::hypot(msg->twist.twist.linear.x, msg->twist.twist.linear.y);
      });

    // 相机帧：传感器 QoS（gz 桥是 best_effort，必须匹配否则收不到）
    rclcpp::SubscriptionOptions opt;
    sub_cam_ = create_subscription<sensor_msgs::msg::Image>(
      cam_topic_, rclcpp::SensorDataQoS(),
      [this](sensor_msgs::msg::Image::SharedPtr msg) { onImage(msg); },
      opt);

    // ---------------- ffmpeg 子进程（RTSP 推流路）----------------
    // 先起 ffmpeg：避免 popen 子进程继承之后才创建的 MQTT socket fd，
    // 否则 lib 关闭连接后 TCP 会因子进程持有 fd 而悬挂，状态难辨
    startFfmpeg();

    // ---------------- MQTT（JPEG 快照路）----------------
    mosquitto_lib_init();
    mosq_ = mosquitto_new("image_sender", true, this);
    mosquitto_connect_async(mosq_, broker_.c_str(), broker_port_, 45);
    mosquitto_loop_start(mosq_);

    // 看门狗：loop_start 的自动重连不够可靠（实测断开后不再重连），
    // 每 5 秒检查一次 socket，掉线就主动重连
    mqtt_watchdog_ = create_wall_timer(std::chrono::seconds(5), [this]() {
      if (mosquitto_socket(mosq_) < 0) {
        RCLCPP_WARN(get_logger(), "MQTT 掉线，主动重连...");
        mosquitto_reconnect_async(mosq_);
      }
    });

    last_jpeg_ = now();

    RCLCPP_INFO(get_logger(),
      "image_sender 就绪：JPEG q%ld @%.1fHz -> mqtt %s；H264(禁B帧) -> rtsp://%s:%ld/cam/%s",
      jpeg_quality_, jpeg_fps_, state_topic_.c_str(),
      rtsp_server_.c_str(), rtsp_port_, robot_id_.c_str());
  }

  ~ImageSender() override
  {
    if (ffmpeg_pipe_) {
      pclose(ffmpeg_pipe_);            // 关 stdin 让 ffmpeg 收尾退出
    }
    if (mosq_) {
      mosquitto_loop_stop(mosq_, true);
      mosquitto_destroy(mosq_);
    }
    mosquitto_lib_cleanup();
  }

private:
  // ---------------- ffmpeg 子进程管理 ----------------
  void startFfmpeg()
  {
    // 输入：裸 RGB24 像素经 stdin 管道喂入（yuv420p 转换交给 ffmpeg 滤镜）
    // 输出：RTSP TCP 推流到 MediaMTX
    const std::string cmd =
      "ffmpeg -hide_banner -loglevel warning "
      "-f rawvideo -pix_fmt rgb24 "
      "-s " + std::to_string(width_) + "x" + std::to_string(height_) + " "
      "-r " + std::to_string(fps_) + " "
      "-i pipe:0 "
      "-vf format=yuv420p "
      "-c:v libx264 "
      "-profile:v baseline "          // 浏览器 WebRTC 兼容
      "-bf 0 "                        // ★ 禁用 B 帧（用户要求）
      "-tune zerolatency -preset ultrafast "
      "-g " + std::to_string(static_cast<int>(fps_ * 2)) + " "  // 2 秒一个关键帧
      "-b:v 1M -maxrate 1M -bufsize 1M "
      "-f rtsp -rtsp_transport tcp "
      "rtsp://" + rtsp_server_ + ":" + std::to_string(rtsp_port_) +
      "/cam/" + robot_id_;

    // popen 写模式：stdin 由我们喂数据
    ffmpeg_pipe_ = popen(cmd.c_str(), "w");
    if (!ffmpeg_pipe_) {
      RCLCPP_ERROR(get_logger(),
        "ffmpeg 子进程启动失败: %s（检查 ffmpeg 是否安装、命令是否合法）",
        strerror(errno));
      return;
    }
    RCLCPP_INFO(get_logger(), "ffmpeg 推流子进程已启动");
  }

  // ---------------- OSD 烧录 ----------------
  void burnOsd(cv::Mat & frame)
  {
    std::lock_guard<std::mutex> lk(data_mutex_);

    // 时间戳：本地日期时间（画面回看时定位归属）
    const auto t = std::time(nullptr);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));

    const auto font = cv::FONT_HERSHEY_SIMPLEX;
    const auto green = cv::Scalar(80, 220, 80);   // BGR 浅绿
    const auto black = cv::Scalar(0, 0, 0);       // 描边底色，保证亮暗背景都可读

    // 左上：时间戳
    cv::putText(frame, tbuf, {10, 25}, font, 0.55, black, 3, cv::LINE_AA);
    cv::putText(frame, tbuf, {10, 25}, font, 0.55, green, 1, cv::LINE_AA);
    // 右上：机器人 ID
    const std::string id = "ID: " + robot_id_;
    const auto sz = cv::getTextSize(id, font, 0.55, 1, nullptr);
    cv::putText(frame, id, {width_ - sz.width - 10, 25}, font, 0.55, black, 3, cv::LINE_AA);
    cv::putText(frame, id, {width_ - sz.width - 10, 25}, font, 0.55, green, 1, cv::LINE_AA);
    // 左下：位姿与速度（map 系米制，与看板地图一致）
    char pose[64];
    snprintf(pose, sizeof(pose), "pos(%.2f, %.2f)  v=%.2f m/s", pos_x_, pos_y_, speed_);
    cv::putText(frame, pose, {10, height_ - 12}, font, 0.5, black, 3, cv::LINE_AA);
    cv::putText(frame, pose, {10, height_ - 12}, font, 0.5, green, 1, cv::LINE_AA);
  }

  // ---------------- 帧回调：烧录后双路分发 ----------------
  void onImage(const sensor_msgs::msg::Image::SharedPtr & msg)
  {
    if (!ffmpeg_pipe_) return;   // 推流挂了就先不烧 JPEG（避免半死状态）

    // rgb8 -> cv::Mat（零拷贝头），再转 BGR（OpenCV JPEG/putText 期望 BGR）
    cv::Mat rgb(msg->height, msg->width, CV_8UC3,
                const_cast<uint8_t *>(msg->data.data()), msg->step);
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    burnOsd(bgr);

    // ① RTSP 路：裸 BGR24 喂 ffmpeg stdin（ffmpeg 内部转 yuv420p + 编码）
    const size_t total = bgr.total() * bgr.elemSize();
    if (fwrite(bgr.data, 1, total, ffmpeg_pipe_) != total || ferror(ffmpeg_pipe_)) {
      RCLCPP_WARN(get_logger(), "ffmpeg 管道断开，尝试重启子进程");
      pclose(ffmpeg_pipe_);
      ffmpeg_pipe_ = nullptr;
      std::thread([this]() { startFfmpeg(); }).detach();
      return;
    }

    // ② MQTT 路：按 jpeg_fps 节流发 JPEG 快照
    if ((now() - last_jpeg_).seconds() < 1.0 / jpeg_fps_) return;
    last_jpeg_ = now();

    // 缩放到快照分辨率再编码：兜底帧追求"能看清"，不需要视频流的分辨率
    cv::Mat snap = bgr;
    if (snapshot_width_ > 0 && snapshot_height_ > 0 &&
        (bgr.cols != snapshot_width_ || bgr.rows != snapshot_height_)) {
      cv::resize(bgr, snap, cv::Size(snapshot_width_, snapshot_height_),
                 0, 0, cv::INTER_AREA);
    }
    std::vector<uchar> jpg;
    std::vector<int> p{cv::IMWRITE_JPEG_QUALITY,
                       static_cast<int>(jpeg_quality_)};
    cv::imencode(".jpg", snap, jpg, p);
    // 兜底帧也做体积保护：超过上限就降质量重编一次，避免弱网下砸出大包
    if (jpg.size() > static_cast<size_t>(max_snapshot_bytes_) && jpeg_quality_ > 40) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000,
        "快照 %zu 字节超过上限 %d，降质量重编", jpg.size(), max_snapshot_bytes_);
      std::vector<int> p2{cv::IMWRITE_JPEG_QUALITY, 40};
      cv::imencode(".jpg", snap, jpg, p2);
    }
    int rc = mosquitto_publish(mosq_, nullptr, state_topic_.c_str(),
      static_cast<int>(jpg.size()), jpg.data(), 0, false);
    if (rc != MOSQ_ERR_SUCCESS) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
        "JPEG publish 失败: %s", mosquitto_strerror(rc));
    }
  }

  // ---------------- 成员 ----------------
  int snapshot_width_ = 640, snapshot_height_ = 360;
  int max_snapshot_bytes_ = 200 * 1024;   // 课程约定：单帧 <= 200KB
  std::string robot_id_, state_topic_, broker_, rtsp_server_;
  std::string cam_topic_, topic_prefix_;
  long jpeg_quality_;
  double jpeg_fps_, fps_;
  int broker_port_, rtsp_port_, width_, height_;

  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr sub_cam_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr sub_odom_;

  std::mutex data_mutex_;
  double pos_x_ = 0, pos_y_ = 0, speed_ = 0;

  mosquitto * mosq_ = nullptr;
  rclcpp::TimerBase::SharedPtr mqtt_watchdog_;
  FILE * ffmpeg_pipe_ = nullptr;
  rclcpp::Time last_jpeg_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<ImageSender>());
  rclcpp::shutdown();
  return 0;
}