# quadruped_champ_config —— CHAMP 步态适配包

把上游 CHAMP（`src/champ/`，通用四足运动学求解器）适配到本仓库机器狗的**配置包**。
只有仿真机需要装（真机的 CHAMP 接线走机器人厂商自己的 bringup，但本包的
`config/champ.yaml` 参数可直接复用）。

## 为什么独立成包（而不是塞进 quadruped_sim）

- CHAMP 上游的惯例就是"每台机器人一个 config 包"（官方模板叫 champ_config）；
- 真机也要用同一套步态参数，配置不该和 Gazebo 绑死；
- 但**早期版本犯了一个依赖倒挂**：本包的 launch 直接去读
  `quadruped_sim` 的 URDF，导致 package.xml 里出现
  `<exec_depend>quadruped_sim</exec_depend>`——真机部署时被迫装一个用不上的仿真包。

> **依赖方向原则：只能"具体 → 通用"，不能倒过来。**
> 仿真包知道自己的模型在哪，所以由它把路径传给本包；本包对 quadruped_sim 零引用。

## 文件结构（单一职责）

| 文件 | 职责 |
| --- | --- |
| `config/champ.yaml` | 步态参数（关节名/摆动时长/步高等），合并了上游 joints+links+gait 三份模板 |
| `launch/champ_controller.launch.py` | 启动 champ_base 控制器 + 关节中继；**接收 `urdf_path` 参数** |
| `src/champ_joint_relay.cpp` | 指令中继：`/champ/joint_cmd`(JointState) → `/quadruped/<关节>/cmd_pos`(Float64) |

## 用法

```bash
# 推荐：主 launch 自动把 urdf_path 传进来
ros2 launch quadruped_sim quadruped_gz.launch.py gait:=champ

# 单独启动（依赖反转的直接体现：传任意一个模型路径都能跑）
ros2 launch quadruped_champ_config champ_controller.launch.py \
    urdf_path:=$(ros2 pkg prefix quadruped_sim)/share/quadruped_sim/urdf/quadruped.urdf.xacro
```

不传 `urdf_path` 会直接报错并提示用法——**宁可失败也不悄悄回读仿真包**，
否则依赖倒挂会悄悄回来。

## 数据链路

```
/cmd_vel
  -> quadruped_controller_node (champ_base，cmd_vel/smooth 已重映射到 /cmd_vel)
  -> /champ/joint_cmd          (12 关节目标角度)
  -> champ_joint_relay         (本包，JointState → 12 路 Float64)
  -> /quadruped/<关节>/cmd_pos (gz 关节位置控制器)
```

## 依赖（改后）

`champ_base`、`xacro`、`robot_state_publisher`——全部是通用包，
对 `quadruped_sim` / `quadruped_mqtt_bridge` / `quadruped_explorer` 零依赖。
