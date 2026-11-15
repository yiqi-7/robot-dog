# Nav2 导航使用指南

## 环境启动

```bash
cd ~/robot-dog
source /opt/ros/lyrical/setup.bash
source install/setup.bash
ros2 launch quadruped_sim quadruped_gz.launch.py gui:=true rviz:=true nav:=true gait:=true teleop:=false
```

启动后等待栈就绪（约 30 s），确认生命周期节点全部 `active`：

```bash
ros2 lifecycle get /bt_navigator        # active [3]
ros2 lifecycle get /controller_server   # active [3]
ros2 lifecycle get /slam_toolbox        # active [3]
```

## 发导航目标

### 方式一：RViz（推荐）

工具栏选 **Nav2 Goal**，在地图空旷处点击并拖动定朝向。

### 方式二：命令行

```bash
ros2 action send_goal /navigate_to_pose nav2_msgs/action/NavigateToPose "
pose:
  header:
    frame_id: map
    stamp: {sec: 0}
  pose:
    position: {x: 0.0, y: -2.5, z: 0.0}
    orientation: {x: 0.0, y: 0.0, z: 0.0, w: 1.0}"
```

> ⚠️ 不要写 `behavior_tree:` 字段——空值会被解析成字符串 `"None"` 导致
> `Can't find a tree with name: None` 而中止。省略该字段即用默认行为树。

## 实测性能（2026-11-15，Go2 构型）

| 指标 | 数值 |
|---|---|
| 3 m 直线导航耗时 | 52 s |
| 位置跟踪误差 | 0.26 m |
| 恢复行为触发 | 0 次 |
| 结果 | SUCCEEDED |

## 已知问题与规避

### nav2_msgs python 绑定崩溃

本环境（ROS 2 Lyrical）`nav2_msgs` 的 Python 绑定存在 bug，构造含
`geometry_msgs/Point` 的消息会触发断言崩溃：

```
geometry_msgs__msg__point__convert_from_py: Assertion `...` failed
```

**规避**：用 `ros2 action send_goal` CLI（YAML 输入）代替 Python 客户端。
Python 客户端对 `NavigateToPose` 可以用（其 `pose` 是 `PoseStamped`，
不触发 Point 转换），但 `DriveOnHeading` 等含裸 `Point` 的动作消息不行。

### 目标被拒 / Path is empty

日志若出现 `NavigatorMutex: Major error!` —— 是残留的旧导航任务占着
任务槽。等待其超时（约 60 s）自动释放，或重启 Nav2 栈再发目标。

### teleop 与 Nav2 冲突

两者都发 `/cmd_vel`，后发者赢。遥控时建议持续按住按键保持指令流，
或在启动 launch 时加 `nav:=false`。

## 保存地图（SLAM → 定位切换）

建图满意后：

```bash
ros2 run nav2_map_server map_saver_cli -f maps/factory
```

生成 `factory.pgm` + `factory.yaml`。之后可把 launch 切换为
`map_server + amcl` 定位模式（免重建图），此切换尚未在本仓库配置。
