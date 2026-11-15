# Robot Dog 🐕 — 厂区四足机器人项目

基于 **ROS 2 (Lyrical) + Gazebo (gz sim 10)** 的四足机器狗仿真与运维项目。
目标是在仿真厂区里实现建图、导航、键盘遥控，并对接 MQTT 运维看板与语音指令网关。

![ROS2](https://img.shields.io/badge/ROS%202-Lyrical-blue) ![Gazebo](https://img.shields.io/badge/gz%20sim-10-orange) ![Platform](https://img.shields.io/badge/平台-Ubuntu%2026.04-green)

## 仓库包含两部分

| 目录 | 内容 | 归属 |
|---|---|---|
| `src/quadruped_sim/` | ROS 2 仿真功能包：机器狗模型、C++ 步态、传感器、SLAM/Nav2 | **本项目自研** |
| `quadruped-ops/` | 厂区监控运维看板（局域网 MQTT 队管系统） | 课程/项目方提供，原样引入 |

两层通过 **MQTT** 通信：机器狗上报遥测、接收任务；看板负责可视化与派单。
接口约定见 [`quadruped-ops/MQTT-INTERFACE.md`](quadruped-ops/MQTT-INTERFACE.md)。

> `quadruped-ops/` 为 vendored 引入，**非本项目原创**；其体积超限的模型/二进制未入库，
> 详见 [`quadruped-ops/README-IN-REPO.md`](quadruped-ops/README-IN-REPO.md)。

## 项目结构

```
robot-dog/
├── src/                      # ROS 2 工作区源码
│   └── quadruped_sim/        # 四足机器狗仿真功能包
│       ├── src/              # C++ 节点源码
│       │   ├── gait_controller.cpp   # 步态控制器（核心）
│       │   ├── odom_pub.cpp          # 里程计 TF 广播
│       │   └── camera_info_pub.cpp   # 深度相机内参补发
│       ├── launch/           # 启动文件（一键拉起全部栈）
│       ├── urdf/             # 机器狗 xacro 模型
│       ├── worlds/           # 仿真厂区世界
│       ├── config/           # Nav2 / RViz 配置
│       ├── CMakeLists.txt
│       └── package.xml
├── quadruped-ops/            # 厂区监控运维看板（MQTT 队管系统，见其 README-IN-REPO.md）
├── models/                   # 外部模型文件（语音 TTS 等，体积大不入库部分见 .gitignore）
├── maps/                     # SLAM 建图输出（运行时生成）
├── captures/                 # 实验截图（运行时生成）
├── recordings/               # 录制数据（运行时生成）
├── MQTT-INTERFACE.md         # 厂区监控看板 MQTT 接口规范
├── VOICE-INTERFACE.md        # 语音指令接口规范
└── README.md
```

## 功能一览

| 模块 | 说明 | 状态 |
|---|---|---|
| 机器狗模型 | xacro：18 关节、平底足垫、站立半径 0.30 m | ✅ |
| 传感器 | IMU 100Hz + 360° 激光雷达 + 前向 8 m 深度相机 | ✅ |
| 步态控制 | C++：对角步态 + 足端运动学逆解 + 速度闭环制动 + 软启动 | ✅ |
| 仿真世界 | 厂区 20×14 m 围墙 + 货架/储罐/货箱 | ✅ |
| SLAM 建图 | slam_toolbox 在线建图 | ✅ |
| 导航 | Nav2 全栈（8 节点 + 碰撞监控） | ✅ |
| 键盘遥控 | teleop_twist_keyboard（支持前进/后退/转向） | ✅ |

## 快速开始

### 依赖

```bash
# ROS 2 Lyrical + Gazebo（gz sim 10）
sudo apt install ros-lyrical-desktop ros-lyrical-ros-gz ros-lyrical-navigation2 \
                 ros-lyrical-nav2-bringup ros-lyrical-slam-toolbox ros-lyrical-teleop-twist-keyboard
```

### 编译

```bash
cd ~/robot-dog
source /opt/ros/lyrical/setup.bash
colcon build --symlink-install --packages-select quadruped_sim
source install/setup.bash
```

### 一键启动（Gazebo GUI + RViz + SLAM + Nav2）

```bash
ros2 launch quadruped_sim quadruped_gz.launch.py \
    gui:=true rviz:=true nav:=true gait:=true teleop:=false
```

启动参数说明：

| 参数 | 默认 | 说明 |
|---|---|---|
| `gui` | true | Gazebo GUI（false 时仅后台服务器，省资源） |
| `rviz` | true | 启动 RViz2 可视化 |
| `nav` | true | 启动 slam_toolbox + Nav2 导航栈 |
| `gait` | true | 启动 C++ 步态控制器 |
| `teleop` | false | 键盘遥控（需要交互式终端，建议手动另开） |

### 键盘遥控

```bash
# 需要交互式终端（launch 里启动会因无 TTY 退出）
ros2 run teleop_twist_keyboard teleop_twist_keyboard
```

按键：`i` 前进 / `,` 后退 / `j` 左转 / `l` 右转 / `k` 停止。

### 导航目标

RViz2 工具栏选择 **Nav2 Goal**，在地图上点一个位置拖动定朝向，机器狗自动规划路径并走过去。

## 机器狗规格

| 项 | 值 |
|---|---|
| 站立半径 | 0.30 m（足端 (±0.24, ±0.18)） |
| 站立高度 | 0.315 m |
| 大腿/小腿 | 各 0.13 m |
| 机身 | 0.36 × 0.24 × 0.08 m，6 kg |
| 关节 | 4 腿 × 3 关节（外摆/髋/膝），PD 位置控制 |
| 足端 | 平底足垫 0.075×0.055×0.030 m（μ=1.5） |
| 激光雷达 | 360° ±π，8 m 量程，10 Hz |
| 深度相机 | 前向 8 m，640×480 @15 Hz，HFOV 69° |
| IMU | 100 Hz，装于机身 |

## 步态控制器（gait_controller.cpp）

核心设计（详见源码注释）：

1. **足端逆解**：给定足端目标位置 + 膝角，二连杆解析逆解反解髋角，自动补偿膝关节折叠带来的前后偏移（这是"后退走不动"的根因）
2. **速度闭环制动**：`v_eff = v_cmd + kv·(v_cmd − v_meas)`，指令归零后若机身还在滑，步态主动反向迈步抵消惯性
3. **软启动**：spawn 后前 2.5 s 关节指令从实际角度斜坡过渡到站立姿态，避免瘫倒瞬间蹬飞机身
4. **原地转向模式**：纯转向时放慢步频、加大横向扫幅换取抓地力
5. **指令看门狗**：cmd_vel 流中断 0.5 s 自动停车

## 接口文档

- [MQTT-INTERFACE.md](MQTT-INTERFACE.md)：厂区监控运维看板的 MQTT 接口规范（v2.1）
- [VOICE-INTERFACE.md](VOICE-INTERFACE.md)：语音指令链路（ASR → LLM → 任务下发）
- [NAV2-GUIDE.md](NAV2-GUIDE.md)：Nav2 导航使用指南（CLI 发目标、实测性能、已知问题）

## 已知限制

- 原地转向力矩偏弱（±15-45°/4s），四足横向扫动的偏航力矩天然有限，后续计划专门的原地转向步态
- 直腿支撑相是近奇异位形，行走时偶有打滑，机身高度会轻微波动

## License

Apache-2.0
