# quadruped_mqtt_bridge —— 上云桥接包

机器人与看板（云端）之间的全部连接都在这个包里。**真机与仿真机都要装**，它不依赖
Gazebo（对比：`quadruped_sim` 只在仿真机上装）。

## 三个节点（单一职责，各自独立连 broker）

原来是一个进程 `mqtt_bridge` 干完遥测 + 任务 + 语音三件事。现在按职责拆成三个
节点，**MQTT 主题、报文字段、参数名全部不变**，网关/看板无感：

| 节点 | 职责 | MQTT 订阅 | MQTT 发布 | 遗嘱 |
| --- | --- | --- | --- | --- |
| `mqtt_bridge` | 遥测上行（1 Hz state）、电量/温度告警、站点地图 | 无（只写不读） | `state`、`event`、`site/map` | ✅ **独占** |
| `task_runner` | cmd 下行分发（estop/resume/assign/cancel）、Nav2 导航、任务回写、急停 | `cmd`、`fleet/tasks` | `fleet/tasks`（回写）、`event` | ❌ |
| `voice_link` | §3.1 音频上行（base64）、§3.2 TTS 播报队列 | `robots/{id}/tts` | `robots/{id}/audio` | ❌ |

约定（改代码前先看这三条）：

1. **遗嘱消息（LWT）只由 `mqtt_bridge` 注册**。它把 `state` 主题的 `status=offline`
   作为遗嘱，机器人掉线/断电/被 kill -9 时 Broker 代发，看板秒级感知。
   另外两个节点**不能**注册遗嘱，否则副节点重启会让看板误判整机离线。
2. **`event` 主题是多发布者**：`mqtt_bridge` 报电量/电机温度，`task_runner` 报
   急停/导航失败/任务完成（level=info 也照发）。两边共用 `alarm_base_id` 基址、
   号段错开（`task_runner` 偏移 10 万），`alarmId` 不会撞号；看板侧不需要区分来源
   （报文里的 `source` 字段仍是机器人名）。
3. 三个节点各自 `mosquitto_new`，**客户端 ID 必须不同**：
   `robot_id`（遥测）/ `robot_id + "-task"` / `robot_id + "-voice"`。
   同一个 ID 的两个连接会被 Broker 互相顶掉，表现为"遥测时有时无、指令偶发收不到"。

### 跨节点内部话题（不是 MQTT 契约）

`task_runner` → `mqtt_bridge`：`task_runner/state`（`std_msgs/String`，latched JSON
`{status, task, navigating}`）。`state` 报文里的 `status` 与 `currentTask` 一直由任务
归属方决定，拆节点后只能靠这条 ROS 话题传；这也是拆分后唯一新增的话题。

## 文件结构（按节点 + 关注点拆分，改哪块只动哪个文件）

```
include/quadruped_mqtt_bridge/
├── common.hpp            公共小工具（base64Encode / wrapPi / yawFromQuat / wallClockMs / publishText）
├── site_map.hpp          站点表 kZones/kPoints（仿真世界米制，mqtt_bridge 与 task_runner 共用）
├── mqtt_bridge_node.hpp  MqttBridgeNode 类声明（坐标换算 + 遥测/告警/地图）
├── task_runner_node.hpp  TaskRunnerNode 类声明（cmd 分发 + Nav2 + 回写 + 急停）
└── voice_link_node.hpp   VoiceLinkNode 类声明（语音上行 + TTS 队列）
src/
├── node_core.cpp         ① mqtt_bridge 构造/析构：参数、/odom + /joint_states 订阅、1Hz 定时器
├── node_mqtt.cpp         ① mqtt_bridge MQTT 传输：连接 / 回调 / 遗嘱消息 / site map 发布
├── node_telemetry.cpp    ① mqtt_bridge 遥测与告警：状态上行、电量/温度告警、事件冷却（同 key 限频）
├── main.cpp              ① mqtt_bridge 入口
├── task_runner_node.cpp  ② task_runner 构造/析构 + MQTT 连接 + 指令分发（handleCmd）+ event 上报
├── task_runner_tasks.cpp ② task_runner 任务执行：assign / Nav2 动作 / 回写 / 超时 / 初始位姿 / 巡逻暂停
├── main_task_runner.cpp  ② task_runner 入口
├── voice_link_node.cpp   ③ voice_link 构造 + MQTT + §3.1 音频上行 + §3.2 piper 播报队列
├── main_voice_link.cpp   ③ voice_link 入口
├── voice_source.cpp      采集节点（独立可执行）：麦克风 VAD 分段 / WAV 样本 / ptt.sh → voice/audio_pcm
└── image_sender.cpp      视觉节点（独立可执行）：JPEG 快照(MQTT) + H264 推流(RTSP)
scripts/ptt.sh            按键说话脚本（回车开录 / 再回车收尾上传）
```

## 参数归属（参数名一个都没改，只是各归各的节点）

| 参数 | 归属节点 |
| --- | --- |
| `broker_host` / `broker_port` / `topic_prefix` / `robot_id` / `robot_name` / `keepalive` / `username` / `password` | 三个节点都有（连接与身份） |
| `publish_hz` / `battery_init` / `battery_warn` / `battery_crit` / `motor_temp_warn` / `motor_temp_crit` / `alarm_base_id` / `site_scale` / `site_offset_x` / `site_offset_y` / `site_world_width` / `site_world_height` / `publish_site_map` / `robot_model` | `mqtt_bridge` |
| `nav_speed` / `nav_tolerance` / `nav_timeout` / `alarm_base_id` | `task_runner` |
| `voice_enabled` / `audio_topic` / `audio_sample_rate` / `audio_channels` / `piper_bin` / `piper_model` | `voice_link` |

## 常见扩展点

| 想做的事 | 改哪里 |
| --- | --- |
| 状态报文加字段 | `node_telemetry.cpp` 的 `onPublishTimer()` |
| 新增告警类型（电量/温度这类本体告警） | `node_telemetry.cpp` 的 `checkAlarms()`，用 `eventAllowed()` 做限频 |
| 新增下行指令（action） | `task_runner_node.cpp` 的 `handleCmd()` + `task_runner_tasks.cpp` 实现 |
| 调整任务状态回写规则 | `task_runner_tasks.cpp` 的 `writeBackTask()` / `finishTask()` |
| 改看板坐标缩放 / 世界尺寸 | `mqtt_bridge` 参数 `site_*` + `mqtt_bridge_node.hpp` 的换算公式 |
| 增删站点 / 改点位坐标 | `site_map.hpp`（一处改，看板地图与导航目标同步生效） |
| 语音协议字段变化 | `voice_link_node.cpp`（上行）与 `quadruped-ops/VOICE-INTERFACE.md` 同步 |
| 换 TTS 引擎（piper 之外） | `voice_link_node.cpp` 的 `ttsWorker()` |
| 把任务状态接到别的消费方 | 订阅 `task_runner/state`（latched JSON） |

## 依赖

```bash
sudo apt install libmosquitto-dev nlohmann-json3-dev libopencv-dev
```

这几个是系统库，**不在 package.xml 之外**：`package.xml` 里已声明
`libmosquitto-dev` / `nlohmann-json-dev` / `libopencv-dev`，有 rosdep 的环境直接
`rosdep install --from-paths src --ignore-src -r -y` 即可；本机没有 rosdep（且
`rosdep init` 需访问 raw.githubusercontent），用上面的 apt 命令。

缺 libmosquitto 时本包直接 **编译失败并提示**（不再静默跳过——历史上因此出现过
"节点没编译、看板没数据"的假故障）。

## 协议

见 `quadruped-ops/MQTT-INTERFACE.md`（连接/主题/编码）与
`quadruped-ops/VOICE-INTERFACE.md`（语音 §3.1 上行 / §3.2 播报 / §5 确认）。

## 执行安全（watchdog / 门禁）

速度链：`controller_server → cmd_vel_nav → smoother → cmd_vel_smoothed →
collision_monitor → cmd_vel_in → deadman_guard → cmd_vel → 底盘`。

| 机制 | 位置 | 行为 |
| --- | --- | --- |
| 急停闩锁 | task_runner | estop 后置位，拒绝一切 assign；只有 resume 解除 |
| 派单幂等 | task_runner | 同一 task id 执行中/已有终态 → 忽略（QoS1 重传、看板连点） |
| 单任务互斥 | task_runner | 有任务在执行时拒绝新派单并上报 warn 事件 |
| 取消校验 | task_runner | cancel 带 id 时必须与在执行任务一致，避免误停别的任务 |
| 非法目标 | task_runner | 目标不在点位表 → 拒绝 + event(warn) + 回写 revoked |
| **心跳看门狗** | deadman_guard | 订阅 `safety/heartbeat`(1Hz)；心跳超 3s → 切断速度链路并持续发零速，恢复自动放行 |
| 执行层兜底 | gait_controller | 同一心跳逻辑（trot 直驱模式下生效，与上一行互为冗余） |
| 掉线检测 | mqtt_bridge | keepalive 10s（LWT ≈ 15s 内可见），三节点均 10s |

`deadman_guard` 存在的理由：task_runner 是唯一持有 Nav2 动作客户端的执行端，
它崩溃后 Nav2 服务器仍会继续发布速度指令——最终执行层必须有自己的停机手段。
