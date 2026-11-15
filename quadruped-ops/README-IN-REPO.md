# quadruped-ops 为什么在这个仓库里

> 本文档回答三个问题：**这是什么、为什么放在 robot-dog 仓库、缺了什么怎么补。**

## 一、这是什么

**厂区监控运维看板**——一套局域网 MQTT 机器人队管系统（上位机 / 指挥中心），
与本仓库的 `src/quadruped_sim`（机器狗本体仿真）是**上层与下层**的关系：

```
robot-dog（下层：机器狗本体）        quadruped-ops（上层：指挥中心）
├─ 步态控制 / 传感器 / SLAM           ├─ 大屏看板（位置/电量/温度/摄像头）
├─ Nav2 自动导航                      ├─ 任务派发（去哪个点位巡检）
└─ 输出：坐标、速度、相机画面  ⇄      └─ 语音指挥 + 告警中心
              MQTT 局域网交换数据
```

两端共同遵守 `quadruped-ops/MQTT-INTERFACE.md`（v2.1）与 `VOICE-INTERFACE.md` 的约定。

## 二、为什么放进 robot-dog 仓库

| 原因 | 说明 |
|---|---|
| **单仓库联调** | 一次 `git clone` 就能拿到"机器狗 + 看板"两端，不必维护两个仓库 |
| **接口同源** | `MQTT-INTERFACE.md` 是两端的合同，放在一起不会出现版本错配 |
| **改动可追溯** | 为联调做的适配（配置、桥接）能和机器人代码一起 review / 回滚 |
| **对齐课程** | 第三章起需要两端同时在场，集中管理符合项目实际形态 |

⚠️ **来源说明**：`quadruped-ops/` 原始代码由课程/项目方提供，本仓库为**引入（vendored）**
而非原创。自研部分是 `src/quadruped_sim/`。两者请勿混淆归属。

## 三、哪些文件没有入库（以及为什么）

GitHub 单文件上限 100 MB，以下内容体积超限或可从官方重新获取，故通过
`quadruped-ops/.gitignore` 排除：

| 排除项 | 体积 | 原因 | 如何恢复 |
|---|---|---|---|
| `models/sensevoice/model.int8.onnx` | **229 MB** | 超出 GitHub 单文件限制，无法推送 | 从课程资源包/官方获取 SenseVoice 模型 |
| `runtime/node.exe` | 89 MB | 便携版 Windows Node，可被系统 Node 替代 | 装 Node 18+ 即可 |
| `mediamtx/mediamtx.exe` | 54 MB | 第三方流媒体服务器二进制 | https://github.com/bluenviron/mediamtx/releases 下载 |
| `node_modules/` | 280 MB | 依赖包，可再生成 | `npm install` |
| `voice-clips/` | — | 运行时生成的语音片段 | 自动创建 |

> 入库部分约 **13 MB**（源码 + 配置 + 已构建的 `dist/` + 文档），保证结构与逻辑完整可读。

## 四、恢复完整运行环境

```bash
cd quadruped-ops

# 1. 依赖
npm install

# 2. 补回二进制（按需）
#    - Node：用系统 Node 18+ 即可，或放回 runtime/node.exe
#    - MediaMTX：解压到 mediamtx/ 目录
#    - ASR 模型：放回 models/sensevoice/model.int8.onnx + tokens.txt

# 3. 启动（Windows）
start.bat                      # MediaMTX + Broker + 语音网关 + 看板 一键起
# 或手动
node broker.js                 # MQTT Broker: TCP 1883 / WS 8083
node voice-gateway.js          # 语音网关（无模型时自动降级为纯文本注入）
node server.js                 # 看板静态服务: http://localhost:8080
```

Linux/macOS 下需自行替换 `mediamtx.exe` → `mediamtx`、`runtime/node.exe` → 系统 `node`，
并改写 `start.bat` 为对应 shell 脚本。

## 五、与机器狗联调的对接点

| 方向 | MQTT 主题 | 说明 |
|---|---|---|
| 机器狗 → 看板 | `qbot/v1/robots/{id}/state` | 1 Hz 遥测：坐标、速度、电量、状态 |
| 看板 → 机器狗 | `qbot/v1/robots/{id}/cmd` | `estop` / `resume` / `assign` |
| 机器狗 → 网关 | `qbot/v1/robots/{id}/audio` | 语音片段（PCM） |
| 网关 → 机器狗 | `qbot/v1/robots/{id}/tts` | TTS 播报文本 |

机器人侧实现位于 `src/quadruped_sim/src/mqtt_bridge.cpp`（C++）。
