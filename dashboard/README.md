# 厂区监控运维看板（局域网 MQTT 版）

厂区四足机器人（机器狗）数据看板：总览大屏、机器人列表、任务管理、告警中心。
数据通过**局域网 MQTT** 通信；内置模拟器可在无真实设备时演示全部功能
（模拟模式需**显式**开启，见「快速开始」）。

> 默认 `brokerUrl` 与 `whepBase` 留空，按当前页面主机名生成本机/局域网地址；
> 全新 clone 不会连接任何作者服务器。要连接远端服务，请在运行时配置中显式填写地址。


## 页面

| 路由 | 说明 |
| ---- | ---- |
| `/` | 总览：在线统计、机器人队列、厂区平面图（实时位置+轨迹）、待执行任务、选中机器人遥测与摄像头画面 |
| `/robots` | 机器人列表：点击选中，查看单机遥测、摄像头、位置追踪、关联任务与告警 |
| `/tasks` | 任务管理：新建/修改/发布/撤销/删除；即时与定时；移动/巡检/充电三类 |
| `/alarms` | 告警中心：厂区设施状态、24h 温控曲线（最多 4 条对比）、告警列表（筛选/确认） |

## 快速开始（默认真实 MQTT 模式）

`src/config.ts` 与 `public/qbot-config.js` 里 `mode` 的默认值都是 `"real"`，
所以 `npm run dev` 起来后看板会**直接去连 MQTT broker**，不会离线跑假数据。
`brokerUrl` 的取值优先级是 `qbot-config.js` > `config.ts` 的页面主机名兜底，
因此默认地址为 `ws://<当前页面主机名>:8083/mqtt`。如未启动本机 Broker，页面会显示未连接。

```bash
npm install
npm run dev        # 开发模式 http://localhost:3000
```

需要连接远端服务时，修改 `public/qbot-config.js` 里对应的项（**改完刷新浏览器即可，无需重新构建**）：

| 目的 | 改法 |
| --- | --- |
| 连本机 broker | `mqtt.brokerUrl = ""`（回退到 `ws://<页面主机名>:8083/mqtt`）或 `"ws://127.0.0.1:8083/mqtt"` |
| 连自己的服务器 | `mqtt.brokerUrl = "ws://<你的IP>:8083/mqtt"`、`streaming.whepBase = "http://<你的IP>:8889"` |
| 用假数据演示（无需 broker） | `mqtt.mode = "mock"`（内置回环 broker + 设备模拟器，6 台机器人 / 8 个设施） |

> 想做「无外部服务」的演示，**必须显式**把 `mqtt.mode` 改成 `"mock"`；
> 默认不会进模拟模式。

## 运行时配置 vs 编译期默认值

| 层 | 文件 | 何时生效 | 作用 |
| --- | --- | --- | --- |
| 运行时覆盖 | `public/qbot-config.js`（构建后为 `dist/qbot-config.js`） | 改完**刷新浏览器** | 切环境：broker 地址 / 账号密码 / 主题前缀 / WHEP 地址 / `mode` |
| 编译期默认值 | `src/config.ts` | 需重新 `npm run build` | 兜底默认值：`brokerUrl` 跟随页面主机名 `ws://<pageHost>:8083/mqtt`，`mode` 默认 `real`，`topicPrefix` 默认 `qbot/v1` |

优先级：`qbot-config.js`（写入 `window.__QBOT_CONFIG__`）> `src/config.ts`；字段留空即回退默认值。
所以**日常切环境只改 `qbot-config.js`**，`config.ts` 只提供 pageHost 兜底默认值，
不需要为了换地址重新构建。

## 本地部署（生产模式）

```bash
npm run build      # 产出 dist/（会同时拷贝 public/qbot-config.js）
node server.js     # 静态服务 http://0.0.0.0:8080
```

> 本仓库的启动方式统一走项目根目录 `deploy/` 下的脚本（两套部署模式），
> 不再是这里的 Windows 双击脚本。
> 局域网其他电脑访问 `http://<本机IP>:8080`。

## 接入真实机器人

1. 启动 Broker：**Mosquitto 必须同时监听 TCP 1883 与 WS 8083**
   - 配置样例在 `deploy/mosquitto-websockets.conf`（`listener 1883` / `listener 8083` /
     `protocol websockets` / `allow_anonymous true`）
   - 装到系统里才生效（`apt install mosquitto` 默认只有 1883）：
     ```bash
     sudo cp deploy/mosquitto-websockets.conf /etc/mosquitto/conf.d/websockets.conf
     sudo systemctl restart mosquitto
     ss -lntp | grep -E '1883|8083'     # 两行都在才算成功
     ```
   - 生产：部署 EMQX / Mosquitto，开启 1883 + WebSocket 8083，并放行防火墙 1883/8083
2. 改 `public/qbot-config.js`：`mqtt.mode: "real"`、`mqtt.brokerUrl` 填你的 broker 地址
   （本机可留空，自动回退 `ws://<页面主机名>:8083/mqtt`），**刷新浏览器即生效**，无需重新构建
3. 生产部署：`npm run build && node server.js`，或用根目录 `deploy/mode1_local_all.sh --build`

**MQTT 接口规则详见 [MQTT-INTERFACE.md](./MQTT-INTERFACE.md)**（主题、payload、QoS、任务时序、图像帧格式、告警确认）。

## 技术栈

React 19 + TypeScript + Vite + Tailwind CSS + shadcn/ui + Recharts + mqtt.js

## 目录结构

- `src/pages/` 四个页面；`src/sim/store.tsx` 总线驱动的状态层
- `src/mqtt/` 总线抽象、真实 Broker 客户端、回环模拟器与设备模拟器
- `src/providers/trpc.tsx` 任务存储（MQTT fleet/tasks）与原 tRPC 调用兼容垫片
- `contracts/task.ts` 任务枚举 / 文案 / 类型
- `src/assets/cam/` 摄像头场景底图（静态兜底 + 模拟帧源）

## 相关文档

| 文档 | 内容 |
| --- | --- |
| [MQTT-INTERFACE.md](./MQTT-INTERFACE.md) | MQTT 主题与报文契约 |
| [VOICE-INTERFACE.md](./VOICE-INTERFACE.md) | 语音链路契约（上行/下行/确认策略） |
| [VIDEO-LINK.md](./VIDEO-LINK.md) | 视频链路（RTSP → WebRTC/WHEP）拓扑与排障 |
| [../deploy/README-DEPLOY.md](../deploy/README-DEPLOY.md) | 两种部署模式的完整操作手册（含 mosquitto 8083 配置） |
