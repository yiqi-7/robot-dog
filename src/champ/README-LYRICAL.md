# CHAMP（vendored）— Lyrical 适配说明

## 这是什么

CHAMP 是开源四足机器人框架（https://github.com/chvmp/champ），`ros2` 分支。
本目录是从上游 **原样拷贝**（vendored）进来的源码，加上少量 **Lyrical 兼容补丁**。

## 为什么 vendored 而不是 apt / 其他方式

- Lyrical 没有 `ros-lyrical-champ*` 二进制包；
- 上游 `ros2` 分支最后提交 2023-02-24，停止维护，不会适配 Lyrical；
- 放进仓库才能与我们的 URDF / gz 桥接一起版本化、一起演进。

## 来源（provenance）

| 项 | 值 |
|---|---|
| 上游仓库 | https://github.com/chvmp/champ.git |
| 分支 | ros2 |
| 顶层 commit | `049b33ccaf77847bbea168cbb0a11c260ce119cd`（2023-02-25，Merge PR #114） |
| libchamp 子模块 | 同仓库子目录 `champ/`，同一 commit（上游即如此组织） |
| 拷贝时间 | 2026-11，本仓库 vendor 提交 |

## 包含哪些包

| 包 | 用途 | 是否改动 |
|---|---|---|
| `champ` | libchamp 运动学核心（header-only INTERFACE 库） | 无 |
| `champ_msgs` | ContactsStamped 等消息 | 无 |
| `champ_base` | quadruped_controller（步态生成）/ state_estimation / message_relay 三个节点 | ✅ 见下 |
| `champ_description` | 上游示例机器人 URDF/meshes（仅留作参照与占位） | 无 |

**未包含**：`champ_gazebo`（Gazebo Classic 插件，gz sim 用不上）、`champ_navigation`（依赖 Nav2 全家桶，后续 5.2 再评估）、`champ_bringup`（Classic Gazebo 启动文件）。

## 对上游打了哪些补丁（全部在 champ_base 内）

### 1. 头文件改名（Lyrical 把 tf2 / message_filters 系头文件统一 `.h` → `.hpp`）

| 文件 | 改动 |
|---|---|
| `include/quadruped_controller.h` | `tf2/transform_datatypes.h`、`tf2/LinearMath/Quaternion.h`、`Matrix3x3.h` → `.hpp` |
| `include/state_estimation.h` | `tf2/LinearMath/Quaternion.h`、`Matrix3x3.h`、`tf2_ros/transform_broadcaster.h` → `.hpp`；`message_filters/subscriber.h`、`synchronizer.h`、`sync_policies/approximate_time.h` → `.hpp`；`urdf/model.h` → `.hpp` |
| `include/message_relay.h` | 同上三个 tf2 头 → `.hpp` |

### 2. CMakeLists.txt：`ament_target_dependencies` 已在 Lyrical 移除

改为 `target_link_libraries(<target> ${dep_targets})`，其中 dep_targets 用命名空间 target
（`rclcpp::rclcpp`、`champ_msgs::champ_msgs`、…）。注意：

- **`champ` 不能进链接列表**——它是 header-only INTERFACE 库且未导出 target，
  裸写会被当成 `-lchamp` 导致链接失败；头文件由 `include_directories(${champ_INCLUDE_DIRS}/...)` 提供。
- 新增 `find_package(tf2 REQUIRED)` / `tf2_geometry_msgs REQUIRED`。

### 3. `message_filters::Subscriber::subscribe` 新签名

旧的两参重载 `(node, topic)` 已删除，新签名要求显式 QoS：

```cpp
// 旧：
joint_states_subscriber_.subscribe(reinterpret_cast<rclcpp::Node*>(this), "joint_states");
// 新（Lyrical）：
joint_states_subscriber_.subscribe(*this, "joint_states", rclcpp::QoS(10));
```

（`state_estimation.cpp` 两处。）

## 与上游同步的方式

上游已停更（2023-02 后基本无提交）。若日后要同步：

1. clone 上游到临时目录，checkout 上述 commit；
2. 重新套用本文件列出的三类补丁（均为机械替换，可用 `sed`/`python` 脚本重放）；
3. 用新内容覆盖 `champ/` 下对应包，**不要**覆盖本 README。

## 关键接口速记（供集成参考）

- 节点：`quadruped_controller_node`
- 订阅：`cmd_vel/smooth`（注意不是 `cmd_vel`）、`body_pose`
- 参数：`urdf`（URDF 字符串）、`joints_map` / `links_map` / `gait.*`（来自 config yaml）、
  `gazebo`（true 时改发 JointTrajectory 给 Classic Gazebo 控制器；**gz sim 下必须 false**）、
  `publish_joint_states`、`loop_rate`
- 输出：`/joint_states`（`sensor_msgs/JointState`，位置为关节目标角度，
  顺序由 `joints_map` 决定）→ 由我们自己的 relay 节点转成 gz 的 12 个 `cmd_pos` Float64。
