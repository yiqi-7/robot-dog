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

// _GNU_SOURCE 必须在所有系统头之前定义：下面的 ffmpeg 子进程管理要用到
// environ（__USE_GNU）和 posix_spawn_file_actions_addclosefrom_np（__USE_MISC），
// 而 -std=c++17 会定义 __STRICT_ANSI__，默认不暴露这两个符号。
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

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
#include <vector>

// POSIX：ffmpeg 子进程改用 posix_spawnp + 裸 pipe 管理（见 startFfmpegLocked）
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__GLIBC__)
#  if defined(__GLIBC_PREREQ)
#    if __GLIBC_PREREQ(2, 34)
// 子进程里关闭所有 >=3 的 fd（见 startFfmpegLocked）
#      define IMAGE_SENDER_HAS_CLOSEFROM_NP 1
#    endif
#  endif
#endif

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
    // MQTT 鉴权：与 mqtt_bridge / task_runner / voice_link 同名同义。
    // 云端一旦开鉴权，缺这两个参数的节点会被 Broker 拒绝连接，看板表现为
    // "遥测有、画面没有"。为空则不调用 username_pw_set（匿名连接）。
    username_   = declare_parameter("username", "");
    password_   = declare_parameter("password", "");
    // 默认指向本机：别人的机器上不该默默把画面推到某个公网地址。
    // 模式 2 由 launch 参数 rtsp_server:=<中转服务器> 显式传入。
    rtsp_server_ = declare_parameter("rtsp_server", "127.0.0.1");
    rtsp_port_  = declare_parameter("rtsp_port", 8554);
    // 是否推 RTSP 视频流。模式 1（本地全栈、没有本地中转）必须传 false：
    // 否则 ffmpeg 会按默认地址把画面推到公网云端，与"模式 1 不出网"的语义相反。
    rtsp_enable_ = declare_parameter("rtsp_enable", true);
    width_  = declare_parameter("width", 640);
    height_ = declare_parameter("height", 480);
    // MQTT 快照独立分辨率：与 RTSP 流解耦，兜底帧不需要和视频流同分辨率
    snapshot_width_  = declare_parameter("snapshot_width", 640);
    snapshot_height_ = declare_parameter("snapshot_height", 360);
    max_snapshot_bytes_ = declare_parameter("max_snapshot_bytes", 200 * 1024);
    fps_    = declare_parameter("fps", 15.0);
    if (jpeg_fps_ <= 0.0) {
      RCLCPP_WARN(get_logger(), "jpeg_fps=%.2f 非法，回退到 1.0", jpeg_fps_);
      jpeg_fps_ = 1.0;
    }

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
    // 先起 ffmpeg：避免子进程继承之后才创建的 MQTT socket fd，否则 lib 关闭
    // 连接后 TCP 会因子进程持有 fd 而悬挂，状态难辨（重启路径由 closefrom 兜住）
    {
      std::lock_guard<std::mutex> lk(pipe_mutex_);
      ffmpeg_last_start_ = now();
      startFfmpegLocked();
    }
    // 重连专用定时器（1 Hz）：图像回调里只"置重启标志"，真正的收尸 + 重启
    // 在这里同步完成。取代原来 std::thread(...).detach() 的写法——detached 线程
    // 会与执行器线程无锁并发读写同一个管道句柄，而且它持有 this，析构时就成
    // use-after-free。定时器是成员，析构第一步就 reset() 掉，不会再有回调。
    ffmpeg_restart_timer_ = create_wall_timer(std::chrono::seconds(1), [this]() {
      onFfmpegTimer();
    });

    // ---------------- MQTT（JPEG 快照路）----------------
    mosquitto_lib_init();
    // 客户端 ID 必须带 robot_id + "-cam"：与 mqtt_bridge（robot_id 本体）/
    // task_runner（-task）/ voice_link（-voice）区分开。同 ID 的多个连接会被
    // Broker 互相顶掉，多机同场时表现为"画面时有时无"这种假故障。
    const std::string client_id = robot_id_ + "-cam";
    mosq_ = mosquitto_new(client_id.c_str(), true, this);
    if (!mosq_) {
      // 必须判空（写法与 node_mqtt.cpp:20-24 一致）：mosq_ 为空时后面所有
      // mosquitto_publish 都静默失败（只返回 MOSQ_ERR_INVAL），而看门狗还会
      // 每 5 s 报一次"掉线重连"，看着像网络问题，实际是初始化失败，极难排查。
      RCLCPP_ERROR(get_logger(),
        "mosquitto_new 失败（client_id=%s）：MQTT 快照路上报不可用，仅保留 RTSP 路",
        client_id.c_str());
    } else {
      if (!username_.empty()) {
        // 鉴权凭据必须在 connect 之前设置，否则 Broker 开鉴权后连接直接被拒
        mosquitto_username_pw_set(mosq_, username_.c_str(), password_.c_str());
      }
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
    }

    last_jpeg_ = now();

    // 格式符必须与成员声明的真实宽度一致：jpeg_quality_ 是 long（见成员声明），
    // rtsp_port_ 是 int。原来两处都写 %ld，port 那处是错的（变参类型不匹配属未定义行为）。
    RCLCPP_INFO(get_logger(),
      "image_sender 就绪：JPEG q%ld @%.1fHz -> mqtt %s；H264(禁B帧) -> rtsp://%s:%d/cam/%s",
      jpeg_quality_, jpeg_fps_, state_topic_.c_str(),
      rtsp_server_.c_str(), rtsp_port_, robot_id_.c_str());
  }

  ~ImageSender() override
  {
    // 析构三步，顺序就是安全性本身：
    //  ① 先 reset 两个定时器：之后即使还有回调排在队列里，onFfmpegTimer()
    //     也会在锁内看到 ffmpeg_stopping_==true 直接返回，不可能再拉起子进程；
    //  ② shutdownFfmpeg() 在锁内做**唯一一次**收尸（关写端 -> SIGTERM ->
    //     限时 waitpid -> SIGKILL），完成后 fd/pid 都置 -1，所以不存在第二次
    //     关闭，也不存在与 detached 线程抢同一个句柄的 use-after-free；
    //  ③ 最后才拆 MQTT。
    ffmpeg_restart_timer_.reset();
    mqtt_watchdog_.reset();
    shutdownFfmpeg();
    if (mosq_) {
      mosquitto_loop_stop(mosq_, true);
      mosquitto_destroy(mosq_);
      mosq_ = nullptr;
    }
    mosquitto_lib_cleanup();
  }

private:
  // ---------------- ffmpeg 子进程管理 ----------------
  //
  // 为什么不再用 popen/pclose（原实现）：
  //  ① pclose 拿不到子进程 pid，做不了"超时先 SIGTERM 再 SIGKILL"。ffmpeg 卡在
  //     RTSP TCP 重连时会一直不退出，pclose 就永久阻塞，systemd stop 只能 SIGKILL；
  //  ② pclose 内部还要冲 stdio 缓冲，管道写满时同样可能无限等待。
  // 所以改成 pipe + posix_spawnp：pid 由我们持有，关闭路径全程有超时上限。
  // 选 posix_spawnp 而不是 fork+exec 的理由：本进程有 libmosquitto 的网络线程，
  // fork 之后子进程只能跑 async-signal-safe 代码，稍不注意就死锁；posix_spawnp
  // 走 vfork 路径、PATH 查找在父进程完成，子进程侧只做 dup2/close/exec，最稳。
  //
  // 锁约定：ffmpeg_fd_ / ffmpeg_pid_ / ffmpeg_broken_ / ffmpeg_stopping_ /
  // ffmpeg_fails_ / ffmpeg_last_start_ 的全部读写都在 pipe_mutex_ 之内。
  // 函数名后缀 Locked 表示"调用者必须已持有 pipe_mutex_"。
  bool startFfmpegLocked()
  {
    // 未启用 RTSP 时根本不拉起 ffmpeg：省掉一个必然失败的子进程和它的重连日志
    if (!rtsp_enable_) {
      RCLCPP_INFO(get_logger(), "RTSP 推流已禁用（rtsp_enable=false），只发 MQTT 兜底帧");
      return false;
    }
    if (ffmpeg_stopping_) return false;   // 析构已开始：绝不再拉起
    if (ffmpeg_fd_ >= 0) return true;     // 幂等：已经在跑

    // 输入：裸 RGB24 像素经 stdin 管道喂入（yuv420p 转换交给 ffmpeg 滤镜）
    // 输出：RTSP TCP 推流到 MediaMTX
    // 参数用 argv 直接传，不经过 shell：没有引号/转义问题，pid 也就是 ffmpeg 本身
    std::vector<std::string> args = {
      "ffmpeg", "-hide_banner", "-loglevel", "warning",
      "-f", "rawvideo", "-pix_fmt", "rgb24",
      "-s", std::to_string(width_) + "x" + std::to_string(height_),
      "-r", std::to_string(fps_),
      "-i", "pipe:0",
      "-vf", "format=yuv420p",
      "-c:v", "libx264",
      "-profile:v", "baseline",                          // 浏览器 WebRTC 兼容
      "-bf", "0",                                        // ★ 禁用 B 帧（用户要求）
      "-tune", "zerolatency", "-preset", "ultrafast",
      "-g", std::to_string(static_cast<int>(fps_ * 2)),  // 2 秒一个关键帧
      "-b:v", "1M", "-maxrate", "1M", "-bufsize", "1M",
      "-f", "rtsp", "-rtsp_transport", "tcp",
      "rtsp://" + rtsp_server_ + ":" + std::to_string(rtsp_port_) + "/cam/" + robot_id_,
    };
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (auto & a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    int fds[2] = {-1, -1};
    if (pipe(fds) != 0) {
      RCLCPP_ERROR(get_logger(), "ffmpeg 管道创建失败: %s", strerror(errno));
      return false;
    }
    // 写端设 O_NONBLOCK：ffmpeg 卡在 RTSP 重连时会停止读 stdin，管道写满后
    // 阻塞写会把图像回调永久卡死（旧实现用阻塞 fwrite 就有这个隐患）。
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL, 0) | O_NONBLOCK);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[0], STDIN_FILENO);
    // 父进程侧的管道 fd 不能留在子进程里，否则 ffmpeg 永远等不到 stdin 的 EOF
    if (fds[0] != STDIN_FILENO) posix_spawn_file_actions_addclose(&fa, fds[0]);
    if (fds[1] != STDIN_FILENO) posix_spawn_file_actions_addclose(&fa, fds[1]);
#ifdef IMAGE_SENDER_HAS_CLOSEFROM_NP
    // 再把子进程里 >=3 的 fd 全关掉：重启 ffmpeg 时它会继承 libmosquitto 的
    // socket，子进程持有该 fd 会让库关掉连接后 TCP 悬挂（见构造函数里的注释）
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif

    // 子进程自成一个进程组（pgid == pid）：退出时 kill(-pid) 能把 ffmpeg 的
    // 子孙一起带走，不会留下占着 RTSP 连接的孤儿进程
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, "ffmpeg", &fa, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(fds[0]);                        // 父进程只留写端
    if (rc != 0) {
      close(fds[1]);
      RCLCPP_ERROR(get_logger(),
        "ffmpeg 子进程启动失败: %s（检查 ffmpeg 是否安装、命令是否合法）", strerror(rc));
      return false;
    }
    ffmpeg_fd_ = fds[1];
    ffmpeg_pid_ = pid;
    ffmpeg_broken_ = false;
    RCLCPP_INFO(get_logger(), "ffmpeg 推流子进程已启动（pid=%d）", static_cast<int>(pid));
    return true;
  }

  // 把一帧裸像素写进 ffmpeg stdin（调用者持有 pipe_mutex_）。
  // 限时 100 ms：写不进去说明 ffmpeg 已经不读数据了，直接判死交给重连路径，
  // 而不是在图像回调里无限期阻塞。半帧残留无所谓——紧接着整条流水线会被重启，
  // ffmpeg 重新从帧边界开始接收。
  bool writeFrameLocked(const void * data, size_t total)
  {
    if (ffmpeg_fd_ < 0) return false;
    const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(100);
    const auto * p = static_cast<const uint8_t *>(data);
    size_t off = 0;
    while (off < total) {
      const ssize_t n = ::write(ffmpeg_fd_, p + off, total - off);
      if (n > 0) {
        off += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        struct pollfd pfd = {ffmpeg_fd_, POLLOUT, 0};
        ::poll(&pfd, 1, 20);      // 稍等可写，尽量不让画面抖
        continue;
      }
      return false;               // EPIPE 等：ffmpeg 已退出
    }
    return true;
  }

  // 唯一关闭 ffmpeg 的地方（调用者持有 pipe_mutex_）：
  // 关写端 -> SIGTERM(整个进程组) -> 最多 500 ms 等退出 -> SIGKILL -> 再等 500 ms。
  // 全程有限时长，所以"pclose 卡住 -> 节点退不出去"的问题不存在。
  void reapFfmpegLocked()
  {
    if (ffmpeg_fd_ >= 0) {
      close(ffmpeg_fd_);
      ffmpeg_fd_ = -1;
    }
    if (ffmpeg_pid_ <= 0) return;
    const pid_t pid = ffmpeg_pid_;
    // 先把 pid 清掉：即使下面等不到子进程退出而放弃，也绝不会有第二次 signal/wait
    ffmpeg_pid_ = -1;

    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    if (waitPidBounded(pid, 500) == 0) {
      RCLCPP_WARN(get_logger(), "ffmpeg(pid=%d) 未在 500ms 内退出，改用 SIGKILL",
        static_cast<int>(pid));
      kill(-pid, SIGKILL);
      kill(pid, SIGKILL);
      if (waitPidBounded(pid, 500) == 0) {
        // 只剩"卡在不可中断睡眠(D)"这种极端情况。此处必须放弃等待，否则又回到
        // 无限阻塞；节点正在退出，残留进程由内核/父进程退出时收尾。
        RCLCPP_ERROR(get_logger(), "ffmpeg(pid=%d) 无法回收，放弃等待",
          static_cast<int>(pid));
      }
    }
  }

  // 重连路径：由 1 Hz 定时器同步调用（不再有 detached 线程）
  void onFfmpegTimer()
  {
    std::lock_guard<std::mutex> lk(pipe_mutex_);
    if (ffmpeg_stopping_ || !rtsp_enable_) return;
    if (ffmpeg_fd_ >= 0 && !ffmpeg_broken_) return;   // 正常，免打扰
    // 退避：连续起不来时拉长间隔，避免"ffmpeg 立刻失败 -> 每秒 fork 一个"的风暴
    const double backoff = (ffmpeg_fails_ < 3) ? 3.0 : 10.0;
    if ((now() - ffmpeg_last_start_).seconds() < backoff) return;
    if (ffmpeg_fd_ >= 0 || ffmpeg_pid_ > 0) reapFfmpegLocked();   // 先收尸再重启
    ffmpeg_last_start_ = now();
    if (startFfmpegLocked()) {
      ffmpeg_fails_ = 0;
      RCLCPP_INFO(get_logger(), "ffmpeg 推流已恢复");
    } else {
      ++ffmpeg_fails_;
      const double next = (ffmpeg_fails_ < 3) ? 3.0 : 10.0;
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000,
        "ffmpeg 重启失败（连续 %d 次），%.0f 秒后重试", ffmpeg_fails_, next);
    }
  }

  // 析构用：置停止标志 + 一次性收尸（调用者不用持锁）
  void shutdownFfmpeg()
  {
    std::lock_guard<std::mutex> lk(pipe_mutex_);
    ffmpeg_stopping_ = true;
    reapFfmpegLocked();
  }

  // waitpid 轮询：返回 1 表示已回收，0 表示超时仍在跑
  int waitPidBounded(pid_t pid, int timeout_ms)
  {
    const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms);
    for (;;) {
      int status = 0;
      const pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) return 1;
      if (r < 0 && errno != EINTR) return 1;   // ECHILD：已被回收，当成完成
      if (std::chrono::steady_clock::now() >= deadline) return 0;
      usleep(10 * 1000);
    }
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
    // 注意：这里不能因为 ffmpeg 挂了就整体 return。旧版第一行是"管道为空就
    // 直接 return"，结果 ffmpeg 连不上（离线、云端没起、或本机没配中转）时，
    // 连本该独立的 MQTT 兜底帧也一起不发，看板直接黑屏——"双路上报"退化成
    // "单路失败即全灭"。现在两条路互不牵连：RTSP 路写失败只置重启标志，
    // MQTT 兜底帧照发。**这个解耦设计必须保留**。

    // rgb8 -> cv::Mat（零拷贝头），再转 BGR（OpenCV JPEG/putText 期望 BGR）
    cv::Mat rgb(msg->height, msg->width, CV_8UC3,
                const_cast<uint8_t *>(msg->data.data()), msg->step);
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);
    burnOsd(bgr);

    // ① RTSP 路（可选）：裸 BGR24 喂 ffmpeg stdin（ffmpeg 内部转 yuv420p + 编码）
    if (rtsp_enable_) {
      // 只在写这一帧的极短时间内持锁；写失败只"置重启标志"，收尸与重启一律交给
      // onFfmpegTimer()（1 Hz）同步完成。这里绝不再起 detached 线程，因此不存在
      // "detached 线程与执行器线程无锁并发读写同一句柄"的数据竞争，也不会在析构
      // 期间被 detached 线程访问到已销毁的 this（旧实现的两个 UAF 来源）。
      const size_t total = bgr.total() * bgr.elemSize();
      bool ok = false;
      {
        std::lock_guard<std::mutex> lk(pipe_mutex_);
        ok = (ffmpeg_fd_ >= 0) && !ffmpeg_broken_ && writeFrameLocked(bgr.data, total);
        if (!ok) ffmpeg_broken_ = true;    // 交给重连定时器处理
      }
      if (!ok) {
        RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
          "ffmpeg 管道不可写，已标记待重启（由重连定时器负责）");
        // 不 return：下面的 MQTT 兜底帧照发
      }
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
    // 必须检查 imencode 返回值：失败时 jpg 为空，后面会 publish 一个 0 字节帧，
    // 看板侧表现为"有帧但解不出来"，比干脆不发帧更难排查
    if (!cv::imencode(".jpg", snap, jpg, p) || jpg.empty()) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 5000,
        "JPEG 编码失败（%dx%d），跳过本帧", snap.cols, snap.rows);
      return;
    }
    // 兜底帧也做体积保护：超过上限就降质量重编一次，避免弱网下砸出大包
    if (jpg.size() > static_cast<size_t>(max_snapshot_bytes_) && jpeg_quality_ > 40) {
      RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000,
        "快照 %zu 字节超过上限 %d，降质量重编", jpg.size(), max_snapshot_bytes_);
      std::vector<int> p2{cv::IMWRITE_JPEG_QUALITY, 40};
      if (!cv::imencode(".jpg", snap, jpg, p2) || jpg.empty()) {
        RCLCPP_WARN_THROTTLE(get_logger(), *get_clock(), 10000,
          "JPEG 降质量重编失败，跳过本帧");
        return;
      }
    }
    // mosquitto_new 失败时 mosq_ 为空：ctor 已报过 ERROR，这里静默跳过，
    // 免得每帧都拿 MOSQ_ERR_INVAL 刷屏
    if (!mosq_) return;
    // 兜底帧的发布调用：这是「RTSP 不可用时看板还能看到画面」的唯一依赖，
    // 任何重构都不要把它连同上面的 rc 判断一起删掉（QoS 0、非 retained：
    // 快照是高频瞬时数据，留 retained 只会让新订阅者收到一张过期图）。
    const int rc = mosquitto_publish(mosq_, nullptr, state_topic_.c_str(),
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
  std::string username_, password_;   // MQTT 鉴权（与另外三个节点同名同义）
  std::string cam_topic_, topic_prefix_;
  long jpeg_quality_;
  double jpeg_fps_, fps_;
  int broker_port_, rtsp_port_, width_, height_;
  bool rtsp_enable_ = true;

  rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr sub_cam_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr sub_odom_;

  std::mutex data_mutex_;
  double pos_x_ = 0, pos_y_ = 0, speed_ = 0;

  mosquitto * mosq_ = nullptr;
  rclcpp::TimerBase::SharedPtr mqtt_watchdog_;
  rclcpp::Time last_jpeg_;

  // ---- ffmpeg 子进程状态：全部受 pipe_mutex_ 保护 ----
  // 用 fd + pid 取代原来的 FILE*：pid 是"能超时关掉子进程"的前提（pclose 拿不到）。
  std::mutex pipe_mutex_;
  int ffmpeg_fd_ = -1;             // ffmpeg stdin 写端；-1 表示没有在跑
  pid_t ffmpeg_pid_ = -1;          // 子进程 pid（同时是它的进程组 id）
  bool ffmpeg_broken_ = false;     // 需要重启（写失败或启动失败）
  bool ffmpeg_stopping_ = false;   // 析构已开始：永不再拉起子进程
  int ffmpeg_fails_ = 0;           // 连续启动失败次数（用于退避）
  rclcpp::Time ffmpeg_last_start_;
  rclcpp::TimerBase::SharedPtr ffmpeg_restart_timer_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<ImageSender>());
  rclcpp::shutdown();
  return 0;
}