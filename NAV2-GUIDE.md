# Nav2 导航使用指南

## 一条命令启动完整栈

```bash
# 推荐：Gazebo + CHAMP 步态 + SLAM + Nav2（不含 RViz，窗口自动最小化）
ros2 launch quadruped_sim quadruped_gz.launch.py gait:=champ nav:=true rviz:=false

# 需要 RViz 可视化
ros2 launch quadruped_sim quadruped_gz.launch.py gait:=champ nav:=true rviz:=true
```

## Nav2 速度指令链路

```
controller_server ──┐
behavior_server  ──┼─> /cmd_vel_nav (Twist)
                    │        │
                    │        v
                    │   velocity_smoother
                    │        │
                    │        v
                    │   /cmd_vel_smoothed (Twist)
                    │        │
                    │        v
                    └─> collision_monitor ──> /cmd_vel (Twist) ──> CHAMP
```

### 三个必修配置（否则机器人一动不动）

| 配置 | 必须值 | 原因 |
|---|---|---|
| `enable_stamped_cmd_vel` | `false` | Lyrical 版 Nav2 默认发 `TwistStamped`，CHAMP 订阅 `Twist`，类型不匹配则 `/cmd_vel` 无发布者 |
| `collision_monitor.cmd_vel_out_topic` | `cmd_vel` | 原配置指向 `cmd_vel_nav`，与 controller 输出同名，形成闭环，指令出不来 |
| `bond_timeout` | `20.0` | 默认 4 s 在多节点同时启动时不够，lifecycle_manager 会放弃整个 bringup |

### 启动时序

| 时刻 | 启动内容 | 原因 |
|---|---|---|
| 3 s | 机器人生成 | 等 gz 起稳 |
| 5 s | 话题桥接 + 里程计 + 相机内参 | 桥接要先于使用者 |
| 6 s | SLAM | 要初始化 Ceres，且必须先发布 `map -> odom` |
| 8 s | CHAMP 控制器 | 依赖 `/joint_states` 与 `cmd_pos` 桥接 |
| 20 s | Nav2 + lifecycle_manager | 等 `map -> odom` 建立，否则 global_costmap 激活超时 |

## 实测性能（`/odom` 真值）

| 路线 | 距离 | 用时 | 终点误差 | 结果 |
|---|---|---|---|---|
| (0,−5.6) → (0,−4.1) | 1.5 m | 12.7 s | 0.23 m | 成功 |
| → (2.2,−3.2) | 2.6 m | 16.9 s | 0.24 m | 成功 |
| → (0.2,−5.4) | 2.5 m | 53.6 s | 0.15 m | 成功 |
| (0.5,−3.9) → (2.5,−3.9) 穿门洞 | 2.0 m | 11.4 s | 0.34 m | 成功 |

速度上限按 CHAMP 实际能力设为 `max_vel_x: 0.18`、`max_vel_theta: 0.5`。

## 成本地图参数

| 参数 | 值 | 说明 |
|---|---|---|
| `robot_radius` | 0.30 | 实际足端外接圆半径（原 0.4 过保守） |
| `inflation_radius` | 0.45 | 原 0.6 使 0.95 m 宽的门洞两侧膨胀后完全堵死 |

## 已知问题

1. **贴墙局部最小值**：机器人贴到墙面上后 DWB 可能出不来，报
   `Failed to make progress`，随后的 spin 恢复也会因贴墙超时失败。
   门洞本身可以通过（已验证 0.95 m 门洞穿越成功）。
2. **在线 SLAM 地图质量**：边建图边导航时，地图未覆盖到的区域会被当作
   可通行，规划器可能给出穿墙路径。跑一段距离后地图变好，规划也变稳。
   已保存 `maps/factory_latest.{pgm,yaml}`，后续可改用静态地图。
3. **残留节点抢资源**：TITAN V 上曾堆积 53 个上轮遗留的 Nav2 节点，
   导致 DDS 发现变慢、bond 与 change_state 服务超时。
   启动前建议清理：
   ```bash
   pkill -f opennav_docking; pkill -f opennav_following
   pkill -f nav2_; pkill -f slam_toolbox; pkill -f lifecycle_manager
   ```

## 发导航目标

```bash
ros2 action send_goal /navigate_to_pose nav2_msgs/action/NavigateToPose \
  "{pose: {header: {frame_id: map}, pose: {position: {x: 1.0, y: -4.0}, orientation: {w: 1.0}}}}"
```