# 语音指令接口规则（VOICE-INTERFACE）

版本：v1.0 · 2026-09-13
适用：机器人端语音采集 / TTS 播报 与 本地语音网关（`voice-gateway.js`）对接。
基础连接约定（Broker、前缀 `{P}`、编码、QoS）见 `MQTT-INTERFACE.md` 第 1 节，本文不再重复。

---

## 1. 链路总览

```
机器人麦克风 → {P}/robots/{id}/audio（语音片段）
  → 语音网关：ASR（SenseVoice）→ LLM（Ollama Qwen2.5）/ 规则兜底 → 任务草稿
  → 确认策略（{P}/voice/policy，看板可改）
      · 直通  → 直接发布任务
      · 待确认 → 看板确认/拒绝（{P}/voice/cmd）
  → 任务写入 {P}/fleet/tasks（retained 快照）+ 即时任务补发 {P}/robots/{id}/cmd assign
  → TTS 播报文本 → {P}/robots/{id}/tts
```

网关降级行为：Ollama 不可达 → 规则解析兜底；ASR 模型缺失 → 语音通道关闭，仅文本注入可用。两种降级下确认流程、任务写入、TTS 播报均正常。

---

## 2. 主题一览（新增）

| 主题 | 方向 | retained | 说明 |
| --- | --- | --- | --- |
| `{P}/robots/{robotId}/audio` | 机器人 → 网关 | ❌ | 语音片段（JSON，base64 音频） |
| `{P}/robots/{robotId}/tts` | 网关 → 机器人 | ❌ | TTS 播报**文本**（机器人端自行合成语音） |
| `{P}/voice/commands` | 网关 → 看板 | ✅ | 语音指令**全量快照**（数组，新在前，最多 50 条） |
| `{P}/voice/cmd` | 看板 → 网关 | ❌ | 确认 / 拒绝待确认指令 |
| `{P}/voice/policy` | 看板 ⇄ 网关 | ✅ | 确认策略（看板设置，网关执行） |
| `{P}/voice/text` | 任意 → 网关 | ❌ | 测试注入：文本直接当语音指令处理（绕过 ASR） |

**订阅方必须按 `ts`/`cmdId` 过滤**：`{P}/voice/commands` 是 retained 全量快照，且每次状态
变更（确认/拒绝/超时/失败）都会重发整份。只订一条就想拿"刚说的这句"的脚本，实测会把上一轮
`accepted` 的旧回执当成新指令判定（本项目 P5/P6 调试期误判过两次）。看板做弹窗提示同理。

**待确认超时**：`pending_confirm` 指令在 600s 内无人确认 → 网关自动置 `rejected`
（`reason: "确认超时"`，见 `armPendingTimer`）。**未确认的语音指令不会创建 fleet 任务**——
任务只在确认或策略直通（如 `charging` 免确认、`estopDirect`）后才写入 `{P}/fleet/tasks`。

---

## 3. 机器人端对接

### 3.1 语音上传 `{P}/robots/{robotId}/audio`

QoS 1，非 retained。建议唤醒后录 2~8 秒。

```json
{
  "robotId": "QB-01",
  "ts": 1789215408721,
  "format": "pcm_s16le",
  "sampleRate": 16000,
  "channels": 1,
  "data": "<base64 编码的音频字节>"
}
```

| 字段 | 约定 |
| --- | --- |
| `format` | 当前仅支持 `pcm_s16le`（16bit 小端单声道 PCM 裸流，无 WAV 头） |
| `sampleRate` | 任意采样率均可，推荐 16000 |
| `data` | 3~5 秒语音约 100~160KB（16kHz），MQTT 局域网可直传 |

### 3.2 TTS 播报 `{P}/robots/{robotId}/tts`

QoS 1，非 retained。机器人收到后本地合成播报即可：

```json
{ "cmdId": "vc-mtz7li6l-u71m", "ts": 1789215408721, "text": "收到，哨兵巡检货架区-北排" }
```

播报文案由网关生成：收到待确认（"收到，巡检任务等待确认"）、已执行（"收到，哨兵巡检货架区-北排"）、失败（"指令无效：…"/"没听清"）、拒绝（"指令已取消"）、急停（"已急停"）。

### 3.3 指令结果查询

机器人可订阅 `{P}/voice/commands`（retained）获知自己指令的最终状态；任务本身的执行生命周期仍走 `{P}/fleet/tasks` 与 `{P}/robots/{id}/cmd`（见 MQTT-INTERFACE.md 第 4 节），语音只是任务的一个来源（任务对象带 `source: "voice"` 字段）。

---

## 4. 语音指令记录（`{P}/voice/commands` 元素）

```json
{
  "cmdId": "vc-mtz7li6l-u71m",
  "heardBy": "QB-01",
  "robotId": "QB-01",
  "ts": 1789215408721,
  "transcript": "让哨兵去货架区北排巡检",
  "parsed": {
    "title": "巡检 · 货架区-北排",
    "type": "inspection",
    "mode": "immediate",
    "target": "pt-shelf-n",
    "priority": "normal",
    "scheduledAt": null
  },
  "action": "task",
  "confidence": 0.92,
  "engine": "llm",
  "source": "voice",
  "status": "accepted",
  "taskId": 1
}
```

| 字段 | 说明 |
| --- | --- |
| `heardBy` / `robotId` | 拾音机器人 / 实际执行机器人（语音点名时两者不同，如"让 QB-02 去…"） |
| `parsed.target` | 地图点位 ID（`{P}/site/map` 的 `points[].id`） |
| `action` | `task` 任务 / `estop` 急停口令（此时 `parsed` 为 null） |
| `engine` | `llm`（Ollama 解析）/ `rule`（规则兜底） |
| `source` | `voice`（麦克风）/ `text`（测试注入） |
| `status` | `pending_confirm` 待确认 → `accepted` 已执行 / `rejected` 已拒绝 / `failed` 解析失败（见 `reason`） |
| `taskId` | accepted 后关联的 `fleet/tasks` 任务 ID |
| `audioFile` | 可选。语音片段回放地址（如 `/voice-clips/vc-xxx.wav`），仅 `source=voice` 时存在；看板同源 HTTP 直接可播 |

### 4.1 语音片段落盘与质量守卫

- 网关收到 `audio` 消息后先把原始 PCM 包上 WAV 头，存到 `voice-clips/{cmdId}.wav`（默认保留最近 100 条，环形清理），由 `server.js` 挂载到看板同源路径 `/voice-clips/` 提供回放。
- 网关日志对每段音频输出**时长 / RMS 电平 / 峰值**，例如：
  `[asr] QB-01 收到音频 5.6s RMS=0.1321 peak=0.843 → /voice-clips/vc-xxx.wav`
- **静音/过短直接判 `failed`**，不再送 ASR（近静音输入会让 SenseVoice 幻觉出 "I."/"Okay." 之类的垃圾文本）：
  - 时长 < 0.4s → `reason: "音频过短（Xs）"`
  - RMS < 0.002 → `reason: "音量过低（RMS=…），疑似未采到声音"`
  - 正常说话参考值：RMS 0.02~0.2、peak 0.1~0.9；peak 持续顶到 1.0 说明削波失真
- 多声道音频只取**第一个声道**送 ASR。

待确认指令 **10 分钟**未处理自动置为 `rejected`（原因"确认超时"）。

## 5. 确认 / 拒绝（`{P}/voice/cmd`）

```json
{ "cmdId": "vc-mtz7li6l-u71m", "action": "confirm", "ts": 1789215408721 }
```

`action`: `confirm` / `reject`。仅对 `pending_confirm` 状态的指令有效。

## 6. 确认策略（`{P}/voice/policy`，retained）

看板「策略设置」面板修改，网关实时生效：

```json
{
  "requireConfirm": { "move": true, "inspection": true, "charging": false },
  "confidenceThreshold": 0.75,
  "estopDirect": true,
  "ttsReply": true
}
```

- `requireConfirm`：该类别的语音任务是否必须人工确认后才发布
- `confidenceThreshold`：解析置信度低于阈值时，无论类别都强制确认
- `estopDirect`：急停口令（"停下"等）免确认直接下发 `robots/{id}/cmd` estop
- `ttsReply`：是否向机器人回发 TTS 播报

## 7. 测试注入（`{P}/voice/text`）

无麦克风/ASR 时联调用，payload 最小只要 `text` 字段：

```bash
node test-voice.mjs "让哨兵去充电" "QB-01 去货架区北排巡检"
```

## 8. 语音网关部署

```bash
node voice-gateway.js
# 环境变量：MQTT_URL / TOPIC_PREFIX / OLLAMA_URL / VOICE_MODEL / ASR_MODEL_DIR / PENDING_TTL_MS
#           VOICE_CLIP_DIR（语音片段目录，默认 ./voice-clips）/ VOICE_CLIP_KEEP（保留条数，默认 100）
#           LLM_BACKEND（ollama|openai，默认 ollama）/ OPENAI_BASE_URL（openai 后端时必填）
```

- ASR：`models/sensevoice/` 下放 `model.int8.onnx` + `tokens.txt`（sherpa-onnx SenseVoice 中英日韩粤模型）
- LLM：本地 Ollama，默认 `qwen2.5:1.5b`（CPU 可流畅运行）；GPU 正常时可 `set VOICE_MODEL=qwen2.5:3b` 提升解析质量。网关启动时自检并预热，缺失自动降级规则解析
- `start.bat` 已集成：Broker + 语音网关 + 看板三进程一键启动

### 8.1 没有 Ollama 时：OpenAI 兼容后端（本机 TITAN V 采用）

ollama.com 与 GitHub release 在本机网络不可达，改用 llama.cpp 的 OpenAI 兼容服务：

```bash
python3 -m venv ~/llm-venv && ~/llm-venv/bin/pip install "llama-cpp-python[server]"
# GGUF 从 hf-mirror 下载（实测 ~75KB/s，469MB 约 1 小时）
# https://hf-mirror.com/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf
~/llm-venv/bin/python -m llama_cpp.server \
  --model ~/models/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  --host 127.0.0.1 --port 8081 --n_ctx 4096 --n_threads 6 --chat_format chatml

LLM_BACKEND=openai OPENAI_BASE_URL=http://127.0.0.1:8081/v1 VOICE_MODEL=qwen2.5-0.5b \
  node voice-gateway.js
```

实测（TITAN V，CPU 推理，0.5B q4_k_m）：预热 0.1~2s，单条解析 1~3s，输出为合规 JSON。
0.5B 级模型三类系统性偏差，网关双路合并已兜住（见 §4 的 `engine` 字段说明）：

| 偏差 | 实测表现 | 兜底 |
| --- | --- | --- |
| 照抄 prompt 示例 | 输出示例里的旧点位 id（`pt-shelf-n`），地图里不存在 | 合并时校验 target ∈ `siteMap.points`，假 id 丢弃后用规则路补 |
| 时间量词算错 | "五分钟后" 算成 +30 秒 | `scheduledAt` 与规则值偏差 >5 分钟时取规则值 |
| 漏项 | 漏 `target` / `robot` | 规则路补齐 |
| 急停漏识别 | — | 规则命中急停而无视 LLM 结果，直接以规则为准（安全优先） |

中文时间词（一分钟/两分钟/半小时/三点半/十五分钟）由规则路 `cnNum` 负责，
`node test_rules.mjs` 是离线回归（12 例，当前 12/12 通过）。

---

## 9. 机器人端语音实现（本仓库仿真机）

机器人侧由两个节点完成，契约严格对齐 §3.1 / §3.2：

### 9.1 采集上行：`quadruped_mqtt_bridge/voice_source`

把麦克风/WAV 切成一段段语音，发 ROS 话题 `voice/audio_pcm`（`UInt8MultiArray`），
由 `mqtt_bridge` 按 §3.1 组装 JSON 上行（base64 裸 PCM，不含 WAV 头；
采样率/声道通过 latched 的 `voice/audio_info` 同步）。

```bash
# 自动分段（能量 VAD，静音 900ms 收尾）
ros2 run quadruped_mqtt_bridge voice_source --ros-args -p mode:=vad -p thresh:=0.02

# 用 WAV 样本代替麦克风（无麦克风环境，本机即此模式）
ros2 run quadruped_mqtt_bridge voice_source --ros-args \
  -p mode:=file -p wav_path:=/home/wby/voice-clips/cmd_inspection.wav -p loop:=false
```

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `mode` | `vad` | `vad` 麦克风自动分段 / `file` 重发 WAV |
| `device` | 空（默认声卡） | `arecord -D` 设备名 |
| `sample_rate` / `channels` | 16000 / 1 | 上行采样率 |
| `thresh` | 0.02 | 能量 VAD 阈值（归一化 RMS） |
| `silence_ms` / `min_ms` / `max_ms` | 900 / 700 / 8000 | 静音收尾 / 最短 / 最长片段 |
| `preroll_ms` | 240 | 说话前的预热缓冲，防首字被切 |
| `loop` / `gap_s` | false / 6.0 | file 模式循环重发 |

触发方式（两者共用一套状态机）：

```bash
ros2 service call /voice/capture std_srvs/srv/Trigger "{}"   # 重发当前音频
ros2 topic pub -1 /voice/ptt std_msgs/msg/Bool "{data: true}"   # 课程 ptt.sh：开始
ros2 topic pub -1 /voice/ptt std_msgs/msg/Bool "{data: false}"  # 手动收尾并上传
```

注意：节点发布前会等待 `voice/audio_pcm` 的订阅者匹配（最多 10s）。DDS 发现未完成时
直接发布，第一段语音会被静默丢弃——这是实测踩过的坑。

### 9.2 TTS 播报：`quadruped_mqtt_bridge/voice_link`

> 三节点拆分（提交 9efa299）后 TTS 由 `voice_link` 持有，`mqtt_bridge` 只管遥测/告警/遗嘱。

收到 `{P}/robots/{id}/tts` 后：转发 ROS 话题 `voice/tts_text`（供看板/日志），
同时入播报队列，用 piper 合成中文语音再 `aplay` 播放（队列串行，连发不互相打断）：

```bash
piper_bin   := ~/venvs/piper/bin/piper
piper_model := quadruped-ops/models/piper/zh_CN-huayan-medium.onnx
```

模型获取：`pip install piper-tts`（venv）+ 从课程教师包拷贝 `zh_CN-huayan-medium.onnx`
与 `.onnx.json` 到 `quadruped-ops/models/piper/`（63MB 不入库；setup_env.sh 里的
modelscope 地址现已 404）。piper 不可用时只打 WARN，不影响语音上行与任务链路。

实测（本机）：合成 2.3~2.7s wav 正常，播放走 HDMI 声卡（`aplay -l` 只有 HDMI 输出，
无模拟音箱，听到声音需接显示器音响或改 `device` 参数）。
