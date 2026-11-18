# 厂区监控运维看板 · MQTT 接口规则

版本：v2.1 · 2026-09-12
适用：厂区四足机器人数据看板（`quadruped-ops`）与机器人端 / 设施端 / 调度服务的局域网数据对接。
变更：v2.1 新增 `{P}/site/map` 厂区地图配置接口（第 7 节），任务 `target` 改用地图点位 ID。

---

## 1. 连接约定

| 项 | 约定 |
| --- | --- |
| Broker | 局域网任意 MQTT Broker；**看板走 WebSocket**（默认 `ws://192.168.6.2:8083/mqtt`）；机器人 / 设施 / 调度服务走 TCP `mqtt://192.168.6.2:1883`。**本仓库统一用 Mosquitto**（`deploy/mosquitto-websockets.conf` 同时开 TCP 1883 与 WS 8083）|
| 主题前缀 | 默认 `qbot/v1`（全文以 `{P}` 代指），改前缀只需改看板 `src/config.ts` 的 `topicPrefix` |
| 看板接入 | `src/config.ts` 设 `mode: 'real'` 并填 `brokerUrl` / 账号密码，重新 `npm run build` |
| Payload 编码 | 除摄像头帧外全部为 **JSON（UTF-8）**；摄像头帧为**二进制 JPEG** |
| 时间戳 | 所有 `ts` 及日期字段为 **Unix epoch 毫秒** |
| 坐标系 | 厂区平面，单位**米**；范围 0–600 (E/X) × 0–380 (N/Y)；看板地图按 1px = 0.5m 换算；`heading` 弧度（0 = +E，逆时针为正） |
| QoS | 遥测 / 状态 / 任务 QoS 1（含 retained）；指令 QoS 1；摄像头帧 QoS 0 |

看板启动后订阅：

```
{P}/robots/+/state
{P}/robots/+/camera
{P}/fleet/tasks
{P}/facilities/+/state
{P}/site/map
{P}/event
```

---

## 2. 主题一览

| 主题 | 方向 | retained | 频率 | 说明 |
| --- | --- | --- | --- | --- |
| `{P}/robots/{robotId}/state` | 机器人 → 看板 | ✅ | 约 0.7s | 机器人遥测（含静态字段） |
| `{P}/robots/{robotId}/camera` | 机器人 → 看板 | ❌ | 0.5–2 fps | 摄像头图像帧（二进制 JPEG，**兜底链路**，主推流方案见 3.2） |
| `{P}/robots/{robotId}/cmd` | 看板 → 机器人 | ❌ | 事件触发 | 指令（任务下发 / 撤销 / 急停 / 复位） |
| `{P}/fleet/tasks` | 看板 ⇄ 调度方 | ✅ | 变更时 | 任务列表**全量快照**（JSON 数组） |
| `{P}/facilities/{facilityId}/state` | 设施 → 看板 | ✅ | ≤ 5s | 设施遥测（温度 / 阈值） |
| `{P}/site/map` | 地图服务 / 机器人端 → 看板 | ✅ | 变更时 | 厂区地图配置（区域 + 任务点位，第 7 节） |
| `{P}/event` | 任意端 → 看板 | ❌ | 事件触发 | 告警 / 事件统一上报 |
| `{P}/alarms/{alarmId}/ack` | 看板 → 上报方 | ❌ | 事件触发 | 告警确认回执 |
| `{P}/robots/{robotId}/audio` · `{P}/voice/*` · `{P}/robots/{robotId}/tts` | 语音链路 | 部分 | — | 语音指令（详见 `VOICE-INTERFACE.md`） |

**离线标记**：机器人连接 Broker 时设置 LWT —— 向 `{P}/robots/{robotId}/state` 发送 retained 消息且 `status:"fault"`（或自定义离线态），看板据此显示故障 / 离线。

---

## 3. 机器人消息

### 3.1 遥测 `{P}/robots/{robotId}/state`（retained，约 0.7s）

```json
{
  "robotId": "QB-01",
  "name": "哨兵",
  "model": "B2-W 工业四足",
  "ts": 1789008000000,
  "status": "working",
  "battery": 76.3,
  "x": 80.0,
  "y": 85.0,
  "speed": 3.9,
  "heading": 0.61,
  "motorTemp": 46.2,
  "signal": -58,
  "uptimeH": 132.5,
  "currentTask": "车间A 例行巡检",
  "cam": 0
}
```

| 字段 | 取值 / 单位 |
| --- | --- |
| `status` | `working` 作业中 / `idle` 待机 / `charging` 充电中 / `fault` 故障 |
| `battery` | 0–100（%） |
| `x` `y` | 米（平面坐标） |
| `speed` | m/s |
| `motorTemp` | ℃ |
| `signal` | dBm |
| `currentTask` | 当前任务标题或 `null` |
| `cam` | 云台视角编号 0–3（前端底图兜底用） |

### 3.2 摄像头画面（推拉流方案，MediaMTX）

视频图传**不走 MQTT**，走本机部署的 MediaMTX 流媒体服务（`mediamtx`，由 `deploy/mode1_local_all.sh` 或云端 systemd 一并启动）：

| 环节 | 协议 | 地址 |
| --- | --- | --- |
| 机器人 → MediaMTX（**推流**） | RTSP | `rtsp://<看板主机IP>:8554/cam/{robotId}` |
| 看板 → MediaMTX（**拉流**） | WebRTC (WHEP) | `http://<看板主机IP>:8889/cam/{robotId}/whep` |
| 调试 API | HTTP | `http://localhost:9997/v3/paths/list` 查看在线流 |

- **路径约定**：`cam/{robotId}`（如 `cam/QB-01`）。任意路径名都接受，但看板只按此约定拉流。
- **编码要求**：视频 **H.264**（Baseline/Main/High 均可），音频可带 AAC/Opus（可不带）。分辨率建议 1280×720 或 1920×1080，帧率 15–30fps，码率建议 ≤ 2Mbps（局域网充裕可调高）。
- **推流示例**（机器人端 ffmpeg，TCP 传输最稳）：
  ```bash
  ffmpeg -f v4l2 -i /dev/video0 -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency -g 50 \
    -f rtsp -rtsp_transport tcp rtsp://192.168.6.2:8554/cam/QB-01
  ```
- **断流行为**：推流停止后 MediaMTX 自动释放路径，看板画面 5 秒内回退为"等待推流 + 静态底图"，重新推流后看板自动恢复，无需刷新。
- **防火墙**：放行 TCP 8554（推流）、TCP 8889（拉流协商）、**UDP 8189**（WebRTC 媒体）。
- 兜底链路：`{P}/robots/{robotId}/camera` MQTT 图像帧主题保留兼容——WebRTC 无流时若 6 秒内有 MQTT 帧则显示之，都不再有才回退静态底图。新接入请直接用推流，图像帧链路将逐步废弃。

### 3.3 指令 `{P}/robots/{robotId}/cmd`（看板 → 机器人）

```json
{
  "cmdId": "CMD-xxx",
  "ts": 1789008000000,
  "action": "assign",
  "task": { "…任务对象（TaskMsg，见 4.1）…" }
}
```

| `action` | 含义 | `task` |
| --- | --- | --- |
| `assign` | 指派任务（即时任务发布时、定时任务到点时下发） | 完整任务对象 |
| `cancel` | 撤销任务 | 被撤销的任务对象 |
| `estop` | 急停 | `null` |
| `resume` | 复位 / 解除急停 | `null` |

机器人收到 `assign` 后开始执行，执行中通过 `state.currentTask` 上报任务标题；完成后由调度方回写 `{P}/fleet/tasks`。

---

## 4. 任务

### 4.1 任务对象（`fleet/tasks` 数组元素）

```json
{
  "id": 12,
  "title": "午间车间巡检",
  "type": "inspection",
  "mode": "scheduled",
  "robotId": "QB-01",
  "target": "车间A-1号产线",
  "priority": "high",
  "status": "published",
  "scheduledAt": 1789008000000,
  "description": "注意避让叉车",
  "createdAt": 1789007000000,
  "updatedAt": 1789007500000
}
```

| 字段 | 取值 |
| --- | --- |
| `type` | `move` 移动 / `inspection` 巡检 / `charging` 充电 |
| `mode` | `immediate` 即时 / `scheduled` 定时 |
| `robotId` | 执行机器人 ID 或 `null`（未指派） |
| `target` | 目标点位名称（厂区点位表，如 `仓库-货架区`、`充电区-1号桩`）或 `null` |
| `priority` | `low` / `normal` / `high` / `urgent` |
| `status` | `draft` 草稿 / `published` 已发布 / `completed` 已完成 / `revoked` 已撤销 |
| `scheduledAt` | 定时任务触发时间（epoch ms）；即时任务为 `null` |

### 4.2 任务列表 `{P}/fleet/tasks`（retained，全量快照）

- 看板是任务列表的**主要写入方**：新建 / 修改 / 发布 / 撤销 / 删除都会把**完整任务数组** retained 发布到该主题。
- 调度方 / 机器人执行完成后**回写**同一主题（`status` → `completed`）。
- 时序：
  - **即时任务**：看板发布 `published` 全量列表 + 向目标机器人发 `cmd/assign` → 执行 → 回写 `completed`。
  - **定时任务**：看板发布 `published`（**不下发 cmd**）→ 到点由调度方下发 `cmd/assign` → 执行 → 回写 `completed`。
  - **撤销**：看板将 `status` 置为 `revoked` 发布全量列表，同时向执行机器人发 `cmd/cancel`。

### 4.3 状态机

```
draft ──发布──▶ published ──(即时: 立即 / 定时: 到点)──▶ 执行中 ──▶ completed
   │                │
   └─删除(draft)    └────────── 撤销 ──────────▶ revoked
```

约束：仅草稿可删除；已撤销 / 已完成不可修改、不可撤销。

---

## 5. 设施与告警

### 5.1 设施遥测 `{P}/facilities/{facilityId}/state`（retained，≤5s）

```json
{
  "facilityId": "f1",
  "name": "1号锅炉",
  "zone": "锅炉房",
  "base": 86,
  "warnAt": 92,
  "critAt": 100,
  "temp": 91.2,
  "x": 75,
  "y": 310,
  "ts": 1789008000000
}
```

- `base` 基准温度、`warnAt` 预警阈值、`critAt` 临界阈值（℃）；看板据此渲染状态色与温控曲线参考线。
- `x` `y` 为米制平面坐标（地图标注位置）。

### 5.2 告警事件 `{P}/event`

机器人、设施、调度服务统一上报：

```json
{
  "alarmId": 1042,
  "ts": 1789008000000,
  "level": "critical",
  "source": "1号锅炉",
  "message": "炉体温度 101.3°C 超过临界阈值 100°C，立即处理"
}
```

| 字段 | 取值 |
| --- | --- |
| `level` | `info` 提示 / `warning` 警告 / `critical` 严重 |
| `source` | 告警来源名称（机器人 ID、设施名或系统名） |
| `alarmId` | 全局递增唯一 ID（上报方分配） |

### 5.3 告警确认 `{P}/alarms/{alarmId}/ack`

```json
{ "alarmId": 1042, "ts": 1789008000000, "acked": true }
```

---

## 6. 模拟模式

`src/config.ts` 中 `mode: 'mock'` 时，看板内置**回环 Broker + 设备模拟器**：6 台机器人（巡航 / 充电 / 故障样例）、8 个设施、任务执行引擎（发布即时任务即执行、定时任务到点执行、撤销即停）、按机器人视角生成 JPEG 图像帧，启动时发布 retained 内置默认地图（`{P}/site/map`）。全部消息流经与真实链路完全一致的主题与 payload，可无依赖演示完整业务流。
---

## 7. 厂区地图与任务点接口

### 7.1 地图配置 `{P}/site/map`（retained，变更时发布）

地图配置发布到 `{P}/site/map`，**必须 retained**（看板刷新后靠 retained 消息恢复地图），JSON 整体替换。坐标单位为**米**，原点为地图左上角，与机器人遥测 `x/y` 同一坐标系；默认厂区范围 `x: 0..600`、`y: 0..380`，看板按 1px = 0.5m 渲染，画布尺寸随 `width/height` 自适应。

```json
{
  "version": 1,
  "width": 600,
  "height": 380,
  "zones": [
    { "id": "workshop-a", "name": "车间A", "x": 35, "y": 35, "width": 230, "height": 140 }
  ],
  "points": [
    { "id": "a-line-1", "name": "车间A-1号产线", "x": 90, "y": 85, "kind": "waypoint", "zone": "workshop-a" },
    { "id": "charger-1", "name": "充电区-1号桩", "x": 322.5, "y": 236, "kind": "charger", "zone": "charging" }
  ],
  "updatedAt": 1730000000000
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `version` | number | 配置版本号，递增即可，看板不做语义判断 |
| `width` / `height` | number | 地图物理尺寸（米），决定看板画布范围 |
| `zones[].id` / `zones[].name` | string | 区域稳定标识 / 显示名 |
| `zones[].x/y/width/height` | number | 区域矩形（米），左上角原点 |
| `points[].id` | string | **任务引用的稳定标识**，任务的 `target` 字段传该 ID |
| `points[].name` | string | 显示名（看板地图与任务列表展示用） |
| `points[].x/y` | number | 点位坐标（米） |
| `points[].kind` | string | `waypoint`（默认，蓝色任务点）/ `charger`（绿色充电桩符号） |
| `points[].zone` | string | 可选，所属区域 ID |
| `updatedAt` | number | 配置更新时间（epoch 毫秒） |

行为约定：

- 看板未收到 retained 配置时，使用内置默认地图渲染；收到后**整体替换**（非增量合并）。
- `width/height` 可为任意正值（如小型车间 `20 × 14`）：看板自动把地图铺满面板，图标、文字、线宽等视觉元素在屏幕上保持恒定大小，不随地图尺寸缩放。
- 机器人状态中的 `x/y` 使用同一米制坐标系，无需额外转换。
- 任务发布到 `{P}/robots/{robotId}/cmd` 的 `assign.task.target` 必须使用地图点位 ID（如 `charger-1`）；旧版中文名称看板仍兼容显示，新任务请一律用 ID。机器人端可用 `{P}/fleet/tasks` retained 快照获取完整任务列表。
