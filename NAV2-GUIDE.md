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

## 静态地图模式（推荐用于正式导航）

```bash
ros2 launch quadruped_sim quadruped_gz.launch.py gait:=champ nav:=true \
  map:=/run/media/wby/shuju/Document/robot-dog/maps/factory_latest.yaml
```

- `map` 留空 -> slam_toolbox 在线建图（首次建图用）
- `map` 给 yaml -> map_server + AMCL 定位（地图固定，规划稳定）
- Lyrical 版 localization_launch.py 不带生命周期管理器，launch 里
  已自行补了 lifecycle_manager_localization（管理 map_server + amcl）
- AMCL 按差速模型配置，出生点 (0,-5.6) yaw 1.5708 由 initial_pose 给定

## 已知问题

1. **贴墙局部最小值**：机器人贴到墙面上后 DWB 可能出不来，报
   `Failed to make progress`，随后的 spin 恢复也会因贴墙超时失败。
   门洞本身可以通过（已验证 0.95 m 门洞穿越成功）。
2. **在线 SLAM 地图质量**：边建图边导航时，地图未覆盖到的区域会被当作
   可通行，规划器可能给出穿墙路径。跑一段距离后地图变好，规划也变稳。
   已保存 `maps/factory_latest.{pgm,yaml}`，后续可改用静态地图。
3. **collision_monitor 的 polygon_stop 已停用**：激光 0.5~0.6 m 处的
   墙体回波经 TF 变换后落入 stop 多边形，触发后 collision_monitor
   不再转发任何指令（连零值都不发），整条速度链断流。
   已用 polygon_stop.enabled: false 停用，避障由 costmap 障碍层承担。
4. **货架缝已放宽 1.0 -> 1.6 m（已解决）**：wh_shelf_s1 东移 0.6 m，
   全厂现在全部可达；南侧目标实测成功（误差 0.25 m）。
   注意两点配套修正：
   - global_costmap 必须有 static_layer：只有 obstacle_layer 时，
     未被激光扫过的区域全是未知格，allow_unknown=false 会直接
     "Failed to create plan"；allow_unknown=true 则出现穿墙路径。
   - 不要把 SLAM 图的未知区(205)涂成致命(0)再喂给 AMCL：似然场
     会把大片致命当真实墙体，定位发散到 9 m 外。AMCL 用原始图，
     规划器用 static_layer + allow_unknown=false 组合。
5. **窄缝穿越原问题（历史记录）**：wh_shelf_s0 与 wh_shelf_s1 之间
   x∈[3.3,4.3] 的缝，物理上机器人（约 0.5 m 宽）能过，但 DWB 以
   0.5 rad/s 的转向能力跟不住这么紧的路径，会在缝口 (3.3,-4.7)
   徘徊。目标在货架南侧时请走西/东两侧绕行，或等待调优 DWB。
5. **残留节点抢资源**：TITAN V 上曾堆积 53 个上轮遗留的 Nav2 节点，
   导致 DDS 发现变慢、bond 与 change_state 服务超时。
   启动前建议清理：
   ```bash
   pkill -f opennav_docking; pkill -f opennav_following
   pkill -f nav2_; pkill -f slam_toolbox; pkill -f lifecycle_manager
   ```

## 膨胀半径必须大于机器人半径（本节是稳定性的根）

`inflation_radius == robot_radius` 时膨胀层没有软过渡带：离障碍
0.35 m 内的格子全是硬致命 254。机器人贴墙/贴货架停稳（物理上完全
正常）后，它脚下格就是致命，NavFn 拒绝从致命格起飞 -> error 208，
BT 的 ClearEntireCostmap 恢复动作也救不了（致命格来自 static_layer，
清了会重新铺回来）。

修法（参照老师示例 class9-backup 的配置）：
- inflation_radius 0.35 -> 0.60（两处 costmap），0.35~0.60 之间是
  渐变软代价，贴墙停下不再致命
- 规划器 allow_unknown: true（静态地图已完整，与老师一致）

修完后实测：巡逻环线 3 轮 17 个航点全部一次到达，零重试零跳过。

## 巡逻节点 quadruped_sim/patrol_node

C++ 航点环线巡逻（北走廊 -> 墙洞东 -> 缝北口 -> 货架南 -> 出生点）：
- 货架缝物理 1.6 m，膨胀后自由走廊仅 0.4 m，从「墙洞东」斜插会楔在
  货架西北角，必须先到「缝北口 (4.0,-4.3)」对齐缝中心线再南下
- 失败自动重试 2 次（间隔 3 s），耗尽跳过该点继续下一环，不卡死
- 状态发布在 /patrol/status（轻量 JSON），可接前端
- 运行：ros2 run quadruped_sim patrol_node
- 航点可配置（参数 "x,y,名称" 字符串数组，坐标为 map 系米制）：
  ros2 run quadruped_sim patrol_node --ros-args \
    -p "waypoints:=[\"0.0,-2.5,点A\",\"2.2,-3.2,点B\"]" -p loop:=false
- loop:=false 单次模式：走完一轮即停；默认 true 循环巡逻

## 巡逻验收与"站在致命格上"问题（历史）

4 航点巡逻（北走廊 -> 墙洞东 -> 货架南 -> 出生点）实测：
- 单程逐段全部可达；但**连续巡逻**时，目标 3/4 偶发
  `Failed to create plan`（NavFn error 208）。
- 根因：机器人贴墙停下后，obstacle_layer 把机器人脚下格标记为致命
  （robot_radius 0.35 内有 0.25~0.5 m 的墙体回波），NavFn 从致命格
  出发无法规划。
- **重发同一目标即可恢复**（机器人挪动/激光清理后代价刷新），
  实测重发后 32.8 s 到达、误差 0.19 m。
- 上层应用应把 208 视为"可重试"错误：取消后等待 2~3 s 再发。

## 发导航目标

```bash
ros2 action send_goal /navigate_to_pose nav2_msgs/action/NavigateToPose \
  "{pose: {header: {frame_id: map}, pose: {position: {x: 1.0, y: -4.0}, orientation: {w: 1.0}}}}"
```