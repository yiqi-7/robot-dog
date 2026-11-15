// ============================================================================
// gait_controller.cpp —— 四足机器人步态控制器（C++ 版，Go2 构型）
//
// 机器人构型（参照 Unitree Go2）
// --------
// 前后腿【镜像】：URDF 中后腿髋俯仰关节安装时绕 Z 轴旋转 180°。
// 四条腿使用完全相同的关节角，几何上自动呈现：
//   前腿：大腿向后倾、小腿向前收（肘部朝后）
//   后腿：大腿向前倾、小腿向后收（膝盖朝前）
// 支撑反力向内挤而非向外撇 —— 这是抗侧滑、转向有力的力学根源。
//
// 核心原理
// --------
// 1. 对角小跑（trot）：fl+rr 同相、fr+rl 反相，任意时刻 ≥2 对角脚支撑。
//
// 2. 恒高足端轨迹：支撑相足端在机身坐标系里沿水平直线后扫（z 不变），
//    用完整二连杆逆解同时解髋角膝角，避免膝角固定摆髋产生的弧线抬脚。
//
// 3. 足端速度匹配：支撑相足端相对机身的速度 = -(v - w*y_i, w*x_i)，
//    脚在地面上不侧滑；转向靠前后腿横向反摆产生偏航力矩。
//
// 4. 速度闭环反馈（解决"漂移停不下来"）：
//       v_eff = v_cmd + kv * (v_cmd - v_meas)
//    指令归零后若机身还在滑，v_eff 自动变负 -> 步态主动反向迈步制动。
//
// 关节映射（见 urdf；后腿 hip/knee 因安装镜像，正方向与机体系相反）：
//   abduction 绕 X 轴：+ 转动 -> 足端向 -y（四腿一致，外摆关节在镜像之外）
//   hip       绕 Y 轴：前腿 + 转动 -> 足端向 -x（后）；后腿相反
//   knee      绕 Y 轴：- 转动 -> 抬腿方向（前后腿同号，镜像自动成立）
// ============================================================================

#include <chrono>
#include <cmath>
#include <map>
#include <array>
#include <string>

#include "geometry_msgs/msg/twist.hpp"
#include "geometry_msgs/msg/twist_stamped.hpp"
#include "nav_msgs/msg/odometry.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/joint_state.hpp"
#include "std_msgs/msg/float64.hpp"

class GaitController : public rclcpp::Node
{
public:
  GaitController() : Node("gait_controller")
  {
    // ---------------- 步态参数 ----------------
    freq_        = declare_parameter("frequency", 1.6);   // 步频 Hz（对角小跑）
    duty_        = declare_parameter("duty", 0.55);       // 支撑相占比
    swing_lift_  = declare_parameter("swing_lift", 0.05); // 摆动相抬脚高度 m
    l1_          = declare_parameter("upper_len", 0.13);  // 大腿长 m
    l2_          = declare_parameter("lower_len", 0.13);  // 小腿长 m
    // Go2 式弯折站立姿态（四条腿关节角相同，前后腿镜像由 URDF 安装角实现）
    hip_stand_   = declare_parameter("hip_stand", 0.45);  // 站立髋俯仰角 rad
    knee_stand_  = declare_parameter("knee_stand", -0.90); // 站立膝角 rad
    abd_stand_   = declare_parameter("abd_stand", 0.10);  // 站立内八字预紧角 rad
    foot_x_      = declare_parameter("foot_x", 0.24);     // 足端前后安装位置 m
    foot_y_      = declare_parameter("foot_y", 0.18);     // 足端左右安装位置 m
    stride_gain_ = declare_parameter("stride_gain", 1.0); // 步幅系数（足端位移 = 机身位移）
    turn_gain_   = declare_parameter("turn_gain", 2.0);   // 转向横向位移增益
    max_lin_     = declare_parameter("max_linear", 0.35); // 线速度限幅 m/s
    max_ang_     = declare_parameter("max_angular", 1.0); // 角速度限幅 rad/s
    cmd_timeout_ = declare_parameter("cmd_timeout", 0.5); // 指令流超时 s
    kv_gain_     = declare_parameter("kv_gain", 0.5);     // 线速度反馈增益（制动强度）
    kw_gain_     = declare_parameter("kw_gain", 1.0);     // 角速度反馈增益（转向刚度）
    turn_freq_scale_ = declare_parameter("turn_freq_scale", 0.6); // 原地转向步频缩放
    turn_amp_boost_  = declare_parameter("turn_amp_boost", 1.8);  // 原地转向横扫增强
    soft_start_s_    = declare_parameter("soft_start", 1.2); // 软启动斜坡时长 s
    // Go2 构型：spawn 高度即弯折站姿高度，软启动在落地前完成关节折叠，
    // 落地瞬间即为稳定站姿，避免"直腿落地再折叠"时足端拖地扭转机身

    // 支撑相时长（每条腿单步触地时间）
    t_stance_ = duty_ / freq_;

    // 弯折站立时足端相对髋【关节】的竖直距离（所有腿行走时的基准高度）
    //   z_stance = -(L1*cos(hip0) + L2*cos(hip0+knee0)) ≈ -0.244 m
    stance_z_ = -(l1_ * std::cos(hip_stand_) +
                  l2_ * std::cos(hip_stand_ + knee_stand_));

    // 足端工作空间限幅：基准高度下足端前后可达半径
    //   x_max = sqrt((L1+L2)^2 - stance_z^2) 再留 10% 余量
    x_work_ = 0.9 * std::sqrt(std::pow(l1_ + l2_, 2) - stance_z_ * stance_z_);

    // ---------------- 指令与反馈订阅 ----------------
    // 键盘 teleop 发 Twist 到 /cmd_vel
    sub_cmd_ = create_subscription<geometry_msgs::msg::Twist>(
      "cmd_vel", 10,
      [this](geometry_msgs::msg::Twist::SharedPtr msg) {
        vx_cmd_ = clamp(msg->linear.x,  -max_lin_, max_lin_);
        wz_cmd_ = clamp(msg->angular.z, -max_ang_, max_ang_);
        last_cmd_time_ = now();
      });
    // Nav2 的 collision_monitor 输出是 TwistStamped，走独立话题
    sub_cmd_st_ = create_subscription<geometry_msgs::msg::TwistStamped>(
      "cmd_vel_nav", 10,
      [this](geometry_msgs::msg::TwistStamped::SharedPtr msg) {
        vx_cmd_ = clamp(msg->twist.linear.x,  -max_lin_, max_lin_);
        wz_cmd_ = clamp(msg->twist.angular.z, -max_ang_, max_ang_);
        last_cmd_time_ = now();
      });
    // /odom 提供机身实测速度，用于闭环制动与转向刚度
    sub_odom_ = create_subscription<nav_msgs::msg::Odometry>(
      "odom", 10,
      [this](nav_msgs::msg::Odometry::SharedPtr msg) {
        vx_meas_ = msg->twist.twist.linear.x;
        wz_meas_ = msg->twist.twist.angular.z;
        odom_valid_ = true;
      });
    // /joint_states 提供当前关节角，用于软启动斜坡
    // （spawn 与本节点启动之间存在无指令空窗，机器人会瘫倒；
    //   若瞬间下发站立指令，四条腿暴力蹬地会把机身推出数米）
    sub_js_ = create_subscription<sensor_msgs::msg::JointState>(
      "joint_states", 10,
      [this](sensor_msgs::msg::JointState::SharedPtr msg) {
        for (size_t i = 0; i < msg->name.size() && i < msg->position.size(); ++i) {
          cur_pos_[msg->name[i]] = msg->position[i];
        }
        js_valid_ = true;
      });

    // ---------------- 12 个关节指令发布者 ----------------
    for (const auto & leg : LEG_NAMES) {
      for (const auto & j : {"abduction", "hip", "knee"}) {
        std::string topic = "/quadruped/" + leg + "_" + j + "/cmd_pos";
        pubs_[leg + "_" + j] =
          create_publisher<std_msgs::msg::Float64>(topic, 10);
      }
    }

    // ---------------- 50 Hz 主循环 ----------------
    timer_ = create_wall_timer(
      std::chrono::milliseconds(20), std::bind(&GaitController::step, this));

    RCLCPP_INFO(get_logger(),
      "步态控制器已启动(Go2 构型): 步频 %.2f Hz, 支撑相 %.2f, "
      "站立(hip=%.2f knee=%.2f) 足高 %.3f m, 抬脚 %.2f m, 足端行程 ±%.2f m, "
      "反馈 kv=%.1f kw=%.1f",
      freq_, duty_, hip_stand_, knee_stand_, -stance_z_, swing_lift_,
      x_work_, kv_gain_, kw_gain_);
  }

private:
  // ---- 腿相位偏移：trot 对角小跑（fl+rr 同相，fr+rl 反相） ----
  // 实测对比：爬行时序下膝弯过深（-1.26 rad）导致膝关节 PD 撑不住体重，
  // 对角时序下支撑腿更接近伸直位，力矩小、跟踪好
  const std::array<std::string, 4> LEG_NAMES = {"fl", "rr", "fr", "rl"};
  const std::map<std::string, double> PHASE_OFFSET = {
    {"fl", 0.00}, {"rr", 0.00}, {"fr", 0.50}, {"rl", 0.50}};

  static double clamp(double v, double lo, double hi)
  {
    return std::max(lo, std::min(hi, v));
  }

  // ------------------------------------------------------------------
  // 二连杆逆解：给定足端在髋挂点坐标系的目标位置 (x, z)，
  // 同时解出髋角和膝角，使足端精确到达该点。
  //
  // 正运动学：x = -(L1*sin(h) + L2*sin(h+k))
  //           z = -(L1*cos(h) + L2*cos(h+k))
  // 映射到标准平面坐标 (X=z 为竖直向下分量, Y=x 为前后分量)，
  // 令 a1 = h + PI（等效从 X 轴负方向起量），a2 = k，套用标准公式：
  //   cos(k) = (d^2 - L1^2 - L2^2) / (2*L1*L2)
  //   h = atan2(x, z) - atan2(L2*sin(k), L1 + L2*cos(k)) - PI
  // 膝角取负号分支（与 URDF 的抬腿方向一致）。
  // ------------------------------------------------------------------
  void legIK(double x, double z, double & hip, double & knee)
  {
    double d2 = x * x + z * z;
    double max_d = l1_ + l2_ - 1e-4;          // 工作空间保护
    if (d2 > max_d * max_d) {                 // 目标超程则沿方向收缩到可达半径
      double scale = max_d / std::sqrt(d2);
      x *= scale;
      z *= scale;
      d2 = max_d * max_d;
    }
    double c2 = (d2 - l1_ * l1_ - l2_ * l2_) / (2.0 * l1_ * l2_);
    knee = -std::acos(clamp(c2, -1.0, 1.0));  // 负膝角 = 抬腿方向
    hip = std::atan2(x, z) -
          std::atan2(l2_ * std::sin(knee), l1_ + l2_ * std::cos(knee)) - M_PI;
    // atan2 在足端越过髋挂点正下方（前后两侧）时会分支跳变，
    // 出现 ±2π 量级的等价角，必须规整回 [-PI, PI] 再取正确分支
    hip = std::atan2(std::sin(hip), std::cos(hip));
    // 髋关节限位保护（URDF ±1.20 rad）
    hip = clamp(hip, -1.2, 1.2);
  }

  // 指令超时看门狗：指令流中断后自动归零，防止带着旧指令自己跑
  void watchdog()
  {
    if (last_cmd_time_.nanoseconds() == 0) {
      return;
    }
    if ((now() - last_cmd_time_).seconds() > cmd_timeout_) {
      if (vx_cmd_ != 0.0 || wz_cmd_ != 0.0) {
        RCLCPP_INFO(get_logger(), "cmd_vel 超时，自动停车");
      }
      vx_cmd_ = 0.0;
      wz_cmd_ = 0.0;
    }
  }

  // 发布站立姿态：Go2 式弯折站立（hip=+0.45, knee=-0.90，四腿关节角相同；
  // 前腿呈现"肘部朝后"、后腿呈现"膝盖朝前"的镜像对折形态）
  // abd 给 ±0.10 的内八字预紧：四腿向内收拢卡住机身，消除因关节
  // 跟踪误差导致个别足端悬空的支撑死区（3 足支撑会持续扭转机身）
  void stand()
  {
    for (const auto & leg : LEG_NAMES) {
      // 外八预紧：全腿统一负角，撑住机身（对称，无净偏航力矩）
      publish(leg, "abduction", -abd_stand_);
      publish(leg, "hip", hip_stand_);
      publish(leg, "knee", knee_stand_);
    }
  }

  // 软启动斜坡系数：0 = 完全跟随当前位置, 1 = 完全使用目标位置
  double softStartRamp()
  {
    if (start_time_.nanoseconds() == 0) {
      start_time_ = now();
    }
    double t = (now() - start_time_).seconds();
    return clamp(t / soft_start_s_, 0.0, 1.0);
  }

  static double blend(double from, double to, double ramp)
  {
    return from + (to - from) * ramp;
  }

  // 读取当前实际关节角（无数据时返回 0）
  double curPos(const std::string & joint)
  {
    auto it = cur_pos_.find(joint);
    return it != cur_pos_.end() && js_valid_ ? it->second : 0.0;
  }

  // ------------------------------------------------------------------
  // 主循环：速度闭环 -> 相位推进 -> 逐腿足端轨迹 -> 逆解 -> 下发指令
  // ------------------------------------------------------------------
  void step()
  {
    watchdog();

    // 停车条件：指令为零 且 机身实测速度接近零（滑行中不提前站立，交给反馈制动）
    if (std::abs(vx_cmd_) < 1e-3 && std::abs(wz_cmd_) < 1e-3 &&
        (!odom_valid_ ||
         (std::abs(vx_meas_) < 0.04 && std::abs(wz_meas_) < 0.35)))
    {
      phase_ = 0.0;
      stand();
      return;
    }

    // ---- 速度闭环反馈 ----
    // 指令为零时误差 = -实测速度，步态主动反向迈步抵消惯性（主动制动）
    // 角速度修正项限幅 ±0.12 rad/s：步态周期有 ~0.5s 迟滞，增益过大会振荡
    double v_err = vx_cmd_ - vx_meas_;
    double w_err = wz_cmd_ - wz_meas_;
    double v = clamp(vx_cmd_ + kv_gain_ * v_err, -max_lin_, max_lin_);
    double w = clamp(wz_cmd_ + clamp(kw_gain_ * w_err, -0.12, 0.12),
                     -max_ang_, max_ang_);

    // ---- 原地转向模式 ----
    // 用【指令】判断触发（而不是反馈速度）：前一动作的惯性滑移
    // 会让反馈速度超阈值，导致转向模式永远无法激活
    // 纯转向时横向扫动偏航力矩弱、快步频易打滑：放慢步频换取抓地
    double freq_eff = freq_;
    double t_stance_eff = t_stance_;
    double turn_eff = turn_gain_;
    if (std::abs(vx_cmd_) < 0.08 && std::abs(wz_cmd_) > 0.15) {
      freq_eff = freq_ * turn_freq_scale_;
      t_stance_eff = duty_ / freq_eff;   // 步幅计算必须用同一套支撑时长
      turn_eff = turn_gain_ * turn_amp_boost_;
    }

    phase_ = std::fmod(phase_ + freq_eff * dt_, 1.0);

    // ---- 逐腿计算 ----
    for (const auto & leg : LEG_NAMES) {
      double p = std::fmod(phase_ + PHASE_OFFSET.at(leg), 1.0);
      double x_i = (leg == "fl" || leg == "fr" ? 1.0 : -1.0) * foot_x_;
      double y_i = (leg == "fl" || leg == "rl" ? 1.0 : -1.0) * foot_y_;

      // 支撑相足端相对机身的位移半幅（由机身期望速度反推），并限幅到工作空间
      double a_x = clamp((v - w * y_i) * t_stance_eff * 0.5 * stride_gain_,
                         -x_work_, x_work_);
      double a_y = clamp((w * x_i) * t_stance_eff * 0.5 * turn_eff,
                         -0.12, 0.12);   // 横向扫幅上限 0.12 m（再大足端会被抬起过多）

      double wave, lift;
      if (p < duty_) {                        // 支撑相：足端沿水平直线后扫
        double s = p / duty_;
        wave = 2.0 * s - 1.0;                 // -1 -> +1
        lift = 0.0;                           // 高度恒定 —— 稳定的关键
      } else {                                // 摆动相：足端抬起向前回摆
        double s = (p - duty_) / (1.0 - duty_);
        wave = 1.0 - 2.0 * s;                 // +1 -> -1
        lift = swing_lift_ * std::sin(M_PI * s);  // 半程抬到最高
      }

      // 足端目标位置（相对髋【关节】，机身坐标系）
      // 基准高度 = 弯折站立足高 stance_z_（行走全程保持该折叠形态，
      // 只是足端在其可达空间内平移），摆动相在此基础上叠加抬脚高度
      double x_des = -a_x * wave;
      double y_des = -a_y * wave;
      double z_des = stance_z_ + lift;

      // ---- 后腿镜像（Go2 构型核心）----
      // URDF 中后腿髋俯仰关节安装时旋转了 180°（局部 x 轴反向），
      // 因此后腿的逆解必须把前后方向的目标先在【腿局部坐标系】里镜像：
      // 机体系里足端向 -x 后扫，镜像后局部 +x 方向 —— 关节角与前腿
      // 完全相同的规律，落点却自动镜像，形成前后腿对折形态
      double x_local = (leg == "rl" || leg == "rr") ? -x_des : x_des;

      // 矢状面(前后-竖直)二连杆逆解：同时得到髋角与膝角
      double hip, knee;
      legIK(x_local, z_des, hip, knee);

      // 外摆角：由横向期望位置反解（外摆关节在镜像之外，四腿同号；
      // reach 取模长，镜像只翻转 x 方向不改变距离）
      double reach = std::hypot(x_local, z_des);
      double abd = -std::asin(clamp(y_des / std::max(reach, 1e-3), -1.0, 1.0));

      publish(leg, "hip", hip);
      publish(leg, "abduction", abd);
      publish(leg, "knee", knee);
    }
  }

  void publish(const std::string & leg, const std::string & joint, double value)
  {
    std_msgs::msg::Float64 msg;
    // 软启动期间所有关节指令都从当前实际角度斜坡过渡到目标角度
    double ramp = softStartRamp();
    msg.data = blend(curPos(leg + "_" + joint + "_joint"), value, ramp);
    pubs_[leg + "_" + joint]->publish(msg);
  }

  // ---------------- 成员变量 ----------------
  double freq_, duty_, swing_lift_;
  double hip_stand_, knee_stand_, abd_stand_;   // Go2 式弯折站立关节角
  double l1_, l2_, foot_x_, foot_y_;
  double stride_gain_, turn_gain_, max_lin_, max_ang_;
  double cmd_timeout_, kv_gain_, kw_gain_;
  double turn_freq_scale_, turn_amp_boost_;
  double soft_start_s_{2.5};
  rclcpp::Time start_time_{0, 0, RCL_ROS_TIME};
  std::map<std::string, double> cur_pos_;   // 当前实际关节角（来自 /joint_states）
  bool js_valid_{false};
  double t_stance_, x_work_, stance_z_{-0.244}, dt_{0.02}, phase_{0.0};

  double vx_cmd_{0.0}, wz_cmd_{0.0};          // 指令速度（限幅后）
  double vx_meas_{0.0}, wz_meas_{0.0};        // /odom 实测速度
  bool odom_valid_{false};
  rclcpp::Time last_cmd_time_{0, 0, RCL_ROS_TIME};

  rclcpp::Subscription<geometry_msgs::msg::Twist>::SharedPtr sub_cmd_;
  rclcpp::Subscription<geometry_msgs::msg::TwistStamped>::SharedPtr sub_cmd_st_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr sub_odom_;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr sub_js_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::map<std::string, rclcpp::Publisher<std_msgs::msg::Float64>::SharedPtr> pubs_;
};

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<GaitController>());
  rclcpp::shutdown();
  return 0;
}
