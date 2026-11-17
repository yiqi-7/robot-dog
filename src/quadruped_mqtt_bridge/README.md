# quadruped_mqtt_bridge —— 上云桥接包

机器人与看板（云端）之间的全部连接都在这个包里。**真机与仿真机都要装**，它不依赖
Gazebo（对比：`quadruped_sim` 只在仿真机上装）。

## 文件结构（按功能维度拆分，改哪块只动哪个文件）

```
include/quadruped_mqtt_bridge/
└── mqtt_bridge_node.hpp   类声明：参数/成员/坐标换算，实现分在下面 5 个文件
src/
├── node_core.cpp          构造/析构：参数声明、话题与定时器装配、Nav2 动作客户端
├── node_mqtt.cpp          MQTT 传输：mosquitto 连接 / 回调 / 遗嘱消息 / site map 发布
├── node_telemetry.cpp     遥测与告警：1Hz 状态上行、电量/温度告警、事件冷却（同 key 限频）
├── node_tasks.cpp         任务执行：assign 派单 / Nav2 导航 / 状态回写 / 初始位姿 / 巡逻暂停
├── node_voice.cpp         语音：§3.1 音频上行（base64）+ §3.2 piper 播报队列
├── main.cpp               入口
├── voice_source.cpp       采集节点：麦克风 VAD 分段 / WAV 样本 / 课程 ptt.sh → voice/audio_pcm
└── image_sender.cpp       视觉：JPEG 快照（MQTT）+ H264 推流（RTSP）
scripts/ptt.sh             按键说话脚本（回车开录 / 再回车收尾上传）
```

## 常见扩展点

| 想做的事 | 改哪里 |
| --- | --- |
| 状态报文加字段 | `node_telemetry.cpp` 的 `onPublishTimer()` |
| 新增下行指令（kind） | `node_mqtt.cpp` 的 `onMessageCb()` 分发 + `node_tasks.cpp` 实现 |
| 新增告警类型 | `node_telemetry.cpp` 的 `checkAlarms()`，用 `eventAllowed()` 做限频 |
| 调整任务状态回写规则 | `node_tasks.cpp` 的 `writeBackTask()` / `finishTask()` |
| 语音协议字段变化 | `node_voice.cpp`（上行）与 `quadruped-ops/VOICE-INTERFACE.md` 同步 |
| 换 TTS 引擎（piper 之外） | `node_voice.cpp` 的 `ttsWorker()` |

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
