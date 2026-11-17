# Robot Dog 🐕 — 厂区四足机器人项目

基于 **ROS 2 (Lyrical) + Gazebo (gz sim 10)** 的四足机器狗仿真与运维项目。
目标是在仿真厂区里实现建图、导航、键盘遥控，并对接 MQTT 运维看板与语音指令网关。

![ROS2](https://img.shields.io/badge/ROS%202-Lyrical-blue) ![Gazebo](https://img.shields.io/badge/gz%20sim-10-orange) ![Platform](https://img.shields.io/badge/平台-Ubuntu%2026.04-green)

## 仓库包含两部分

| 目录 | 内容 |
|---|---|
| `src/quadruped_sim/` | ROS 2 仿真功能包：机器狗模型、C++ 步态、传感器、SLAM/Nav2 |
| `quadruped-ops/` | 厂区监控运维看板（局域网 MQTT 队管系统） |

两层通过 **MQTT** 通信：机器狗上报遥测、接收任务；看板负责可视化与派单。
接口约定见 [`quadruped-ops/MQTT-INTERFACE.md`](quadruped-ops/MQTT-INTERFACE.md)。

> `quadruped-ops/` 为 vendored 引入，**非本项目原创**；其体积超限的模型/二进制未入库，
> 详见 [`quadruped-ops/README-IN-REPO.md`](quadruped-ops/README-IN-REPO.md)。

## 项目结构

```
robot-dog/
├── src/                      # ROS 2 工作区源码
│   ├── champ/                # CHAMP 通用四足步态控制器（vendored 上游，含本项目适配补丁）
│   ├── quadruped_champ_config/  # CHAMP 适配本机器狗的配置包（yaml / launch）
│   └── quadruped_sim/        # 四足机器狗仿真功能包
│       ├── src/              # C++ 节点源码
│       │   ├── gait_controller.cpp   # 步态控制器（核心）
│       │   ├── odom_pub.cpp          # 里程计 TF 广播
│       │   ├── champ_joint_relay.cpp # CHAMP 关节指令 -> gz 控制器中继
│       │   └── camera_info_pub.cpp   # 深度相机内参补发
│       ├── scripts/          # gz_window_minimize.c（启动后把 Gazebo 窗口收进任务栏）
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
└── README.md
```

## 功能一览

| 模块 | 说明 | 状态 |
|---|---|---|
| 机器狗模型 | xacro：18 关节、平底足垫、站立半径 0.30 m | ✅ |
| 传感器 | IMU 100Hz + 360° 激光雷达 + 前向 8 m 深度相机 | ✅ |
| 步态控制 | C++：对角步态 + 足端运动学逆解 + 速度闭环制动 + 软启动 | ✅ |
| CHAMP 步态 | vendored CHAMP 通用步态控制器，`gait:=champ` 可切换 | ✅ |
| 仿真世界 | 厂区 20×14 m 围墙 + 货架/储罐/货箱 | ✅ |
| SLAM 建图 | slam_toolbox 在线建图 | ✅ |
| 导航 | Nav2 全栈（8 节点 + 碰撞监控） | ✅ |
| 键盘遥控 | teleop_twist_keyboard（支持前进/后退/转向） | ✅ |
| 静态地图定位 | AMCL + map_server（`map:=<yaml>` 一键切换） | ✅ |
| 多航点巡逻 | patrol_node（C++）：航点参数可配、失败自愈、进度上报；5 航点环线（北走廊→墙洞东→缝北口→货架南→出生点），单航点失败自动重试 2 次后跳过继续 | ✅ |
| 运维看板联动 | mqtt_bridge 把位姿/任务/巡逻进度上报看板 | ✅ |

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

## CHAMP 步态控制器

除了自研 `gait_controller`，本项目还把上游 **CHAMP**（通用四足步态控制器）
vendored 进 `src/champ/`，用 `src/quadruped_champ_config/` 做适配，可以用一条
命令切换：

```bash
ros2 launch quadruped_sim quadruped_gz.launch.py gait:=champ   # 用 CHAMP
ros2 launch quadruped_sim quadruped_gz.launch.py gait:=trot    # 用自研步态（默认）
ros2 launch quadruped_sim quadruped_gz.launch.py gait:=none    # 不起步态控制器
```

数据链路：

```
/cmd_vel -> quadruped_controller_node -> /champ/joint_cmd (JointState, 12 关节)
         -> champ_joint_relay         -> /quadruped/<leg>_<joint>/cmd_pos (Float64)
         -> gz JointPositionController -> 关节
```

### 为跑通 CHAMP 做的三处关键修复

| 现象 | 根因 | 修法 |
|---|---|---|
| 12 路关节指令全是 0 | `nominal_height` 0.30 正好等于腿完全伸直长度，命中逆解可达性边界 `target_to_foot >= l1+l2`，`inverse()` 提前 return，大腿/膝角度未初始化 | 改成 0.25，腿保持微屈 |
| 后腿膝关节几乎不动（实际摆幅 0.034 rad） | `knee_orientation: "><"` 给后腿算出 +1.26 rad，超出 URDF 限位 `upper=0.20` 被卡死 | 改成 `">>"`，四腿统一 0.63/−1.26 |
| 原地不动（0.000 m/s）但能转向 | 后腿髋关节绕 Z 转了 π 做镜像，而 champ 的 `urdf::getPose` 只累加位移、忽略 rpy，逆解对四条腿用同一套 x 方向约定 → 后腿足端朝反方向迈，前进分量与后腿抵消 | 给 champ 核心加 `x_mirrored` 逐腿标志，逆解入口把 x 取反；配置里 `x_mirrored_hind: true` |

前两条是配置问题，第三条是 vendored 上游代码的适配补丁，改动集中在
`quadruped_components.h`（GaitConfig 加字段）、`quadruped_leg.h`（加标志）、
`quadruped_base.h`（下发）、`kinematics.h`（逆解里做 x 映射）、
`quadruped_controller.cpp`（读参数）。

### 关键参数（src/quadruped_champ_config/config/champ.yaml）

| 参数 | 值 | 说明 |
|---|---|---|
| `nominal_height` | 0.25 | 站立时机身到足端距离，必须小于腿长 0.30 |
| `knee_orientation` | `">>"` | 四腿膝关节同号（后腿靠 URDF 镜像获得犬式姿态） |
| `x_mirrored_hind` | true | 后腿是否为镜像安装（配合 URDF 的 rpy=0 0 pi） |
| `stance_duration` | 0.25 | 支撑相时长，步态周期 = 摆腿 0.25 + 支撑 0.25 = 0.5 s (2 Hz) |
| `swing_height` | 0.05 | 摆腿抬脚高度 |
| `max_linear_velocity_x` | 0.5 | 速度上限（实际能到约 0.15 m/s，见下） |

### 实测性能（`/odom` 真值，每次复位到出生点）

| 指令 | 实测 | 跟踪率 |
|---|---|---|
| vx=0.25 m/s | 0.152 m/s | 61% |
| vx=0.15 + wz=0.4 | 0.125 m/s + 0.335 rad/s | 84% / 84% |
| wz=0.5 原地 | 0.484 rad/s | 97% |
| 连续 10 s 前进 | 2.68 m，roll/pitch 全程 ±2° 内 | 未翻倒 |

速度在 0.15 m/s 附近饱和，瓶颈是关节跟踪率（指令摆幅 0.61 rad，实际
0.37 rad，约 60%）。已把关节位置控制器增益从 p=50/60 提到 p=120/150，
速度提升 53%；再往上调（p=300/350）反而变慢，已回退。

## 文档索引

根目录只保留本 README，其余文档随所属模块放在各自目录下：

- 看板侧（`quadruped-ops/`）
  - [MQTT-INTERFACE.md](quadruped-ops/MQTT-INTERFACE.md)：厂区监控运维看板的 MQTT 接口规范（v2.1）
  - [VOICE-INTERFACE.md](quadruped-ops/VOICE-INTERFACE.md)：语音指令链路（ASR → LLM → 任务下发）
- 机器人侧（`src/quadruped_sim/docs/`）
  - [NAV2-GUIDE.md](src/quadruped_sim/docs/NAV2-GUIDE.md)：Nav2 导航使用指南（CLI 发目标、实测性能、已知问题）

## 巡逻实测数据

以 `/tmp/sim_v6.log`（一次连续运行 81.8 分钟）统计：

| 指标 | 数值 |
| --- | --- |
| 发送航点 | 220 次 |
| 到达 | 76 次 |
| 重试耗尽跳过 | 42 次 |
| 整体到达率 | 34.5% |
| 最近 120 条日志区间到达率 | 57% |

**结论：巡逻不是"零失败"**。失败集中在机器人停在货架/墙面前 1.6 m 无法起步时，
Nav2 下发速度但步态推不动（关节速度打满限幅），最终以 `code=6` 中止。
改进方向见 [已知限制](#已知限制)。

### 加脱困后的回归（静态合并图）

`patrol_node` 现在会在重试前先做一次**脱困**（后退 1.3 s + 原地转 1.6 s），
再重发目标。同一台机上实测：

| 指标 | 数值 |
| --- | --- |
| 环线 1 | **4/5 到达**（北走廊 / 墙洞东 / 缝北口 / 货架南 全部一次到达） |
| 唯一失败 | 出生点 `code=6` × 3 次，脱困后仍未能起步，跳过 |
| 后续环 | 机器人连卡在货架南侧后多点失败，需要人工干预 |

结论：脱困把一部分 `code=6` 变成到达（补扫驱动里也验证过同样的效果），
但 Champ 步态在货架/墙角顶住时仍可能连续卡死，这是步态本身的局限，
不是导航参数问题。彻底解决需要换步态或加专门的"蹬地-收回"恢复动作。

## 地图补扫（仓库东侧）

旧图 `factory_full` 有 18.7% 未知区，集中在仓库东侧，当时 (5.0,-6.0)、
(7.0,-5.5)、(8.0,-5.5) 三处"全规划失败"。补扫按下面的流程做完：

1. **临时切 SLAM 模式**（启动不带 `map:=`），用 `/tmp/rescan_driver.py`
   跑"逐点规划 + 跟踪"的自动环线（34 点）；每点先 `ComputePathToPose`
   预验证，失败/中止就先脱困再重试一次，仍不行跳过。绕开 `bt_navigator`
   直接对接 planner/controller，避免行为树在负载高时激活失败。
2. **保守合并**新图与旧图：旧图已知处**全部保留**，旧图未知处只吸收
   新图"确认自由"的格——**不吸收新图的障碍格**。原因：新会话的占据
   像素里有步态抖动噪声，窄通道里多 1% 障碍就会把 0.35 m 内切膨胀
   连成墙（实测激进合并后 costmap 57% 格子是 253/254，走廊全断，
   机器人被困在 1.2 m 的小口袋里）。
3. 合并时两图帧都锚在出生点、偏航一致，只按世界坐标平移；最后裁回
   旧图 404×289 的画面尺寸与原 origin，保证巡逻航点、AMCL 初值全部沿用。

| 指标 | 旧图 | 补扫合并后 |
| --- | --- | --- |
| 分辨率 / 尺寸 | 0.05 m / 404×289 | 0.05 m / 404×289（帧不变）|
| 自由 | 75.7% | **85.0%** |
| 占据 | 5.6% | 5.6%（不引入新障碍）|
| 未知 | 18.7% | **9.4%** |
| 仓库东区未知 | 39.4% | **24.5%** |
| (8.0,-5.5) 1 m² 未知 | 76.2% | **13.8%** |
| (7.0,-5.5) 1 m² 未知 | 45.1% | **17.7%** |

**补扫的意外结论**：(7.0,-5.5) 和 (8.0,-5.5) 这两处补扫后确认**是货架实体
本身**——旧图那里是未知，`track_unknown_space: false` 把未知当自由，
规划器才"假可达"。所以它们本来就不该当驻留点；挨着的通道
(5.0,-5.4) 规划正常（已验证 ✅）。这比"补出一块能站的地"更有价值：
新图让规划器不再从货架里穿过去。

> 早期记录曾写"40 分钟 6 轮 30/30 全到达、零失败"，该结论来自单次未含异常
> 状态的观测，与长时间运行统计不符，此处按实测数据更正。

## 已知限制

- 原地转向力矩偏弱（±15-45°/4s），四足横向扫动的偏航力矩天然有限，后续计划专门的原地转向步态
- 直腿支撑相是近奇异位形，行走时偶有打滑，机身高度会轻微波动
- CHAMP 步态速度上限约 0.15 m/s：关节位置控制器跟踪率只有约 60%，
  指令步幅越大打滑越严重；要再快需要调物理参数或换力矩控制
- 为保证与 champ 运动学模型一致，后腿镜像必须在配置里显式声明
  （`x_mirrored_hind`）；champ 上游对 rpy 镜像不感知，换模型时容易踩坑
- champ 的里程计/状态估计节点未启用，`/odom` 由 gz 的 WORLD 真值源提供
- 步态卡滞后 Nav2 报 `code=6`/105，内置脱困（后退+转身）成功率约一半，
  连续卡死仍需要人工介入或重启该航点

## License

Apache-2.0
