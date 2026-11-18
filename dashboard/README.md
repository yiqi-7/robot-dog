# 厂区监控运维看板（局域网 MQTT 版）

厂区四足机器人（机器狗）数据看板：总览大屏、机器人列表、任务管理、告警中心。
数据通过**局域网 MQTT** 通信；内置模拟器可在无真实设备时演示全部功能。

## 页面

| 路由 | 说明 |
| ---- | ---- |
| `/` | 总览：在线统计、机器人队列、厂区平面图（实时位置+轨迹）、待执行任务、选中机器人遥测与摄像头画面 |
| `/robots` | 机器人列表：点击选中，查看单机遥测、摄像头、位置追踪、关联任务与告警 |
| `/tasks` | 任务管理：新建/修改/发布/撤销/删除；即时与定时；移动/巡检/充电三类 |
| `/alarms` | 告警中心：厂区设施状态、24h 温控曲线（最多 4 条对比）、告警列表（筛选/确认） |

## 快速开始（演示模式，无需外部服务）

```bash
npm install
npm run dev        # 开发模式 http://localhost:3000
```

## 本地部署（生产模式）

```bash
npm run build      # 产出 dist/
node server.js     # 静态服务 http://0.0.0.0:8080
```

Windows 下也可直接双击 `start.bat`（自动打开浏览器）；双击 `stop.bat` 一键停止全部服务（Broker + 语音网关 + 看板，Ollama 不动）。
局域网其他电脑访问 `http://<本机IP>:8080`。

## 接入真实机器人

1. 启动 Broker：
   - 联调：项目内置开发用 Broker —— `node broker.js`（或 `npm run broker` / `broker.bat`），同时监听 **TCP 1883**（机器人）与 **WS 8083 `/mqtt`**（看板），无认证仅限局域网
   - 生产：部署 EMQX / Mosquitto，开启 1883 + WebSocket 8083
   - Windows 首次启动若弹出防火墙提示，请勾选允许（机器人从局域网访问需要放行 1883/8083）
2. 修改 `src/config.ts`：`mode: 'real'`（`brokerUrl` 默认已指向本机 `ws://192.168.6.2:8083/mqtt`，按需改 IP / 账号密码）
3. `npm run build && node server.js` 重新部署（或一键 `start.bat`：Broker + 看板一起起）

**MQTT 接口规则详见 [MQTT-INTERFACE.md](./MQTT-INTERFACE.md)**（主题、payload、QoS、任务时序、图像帧格式、告警确认）。

## 技术栈

React 19 + TypeScript + Vite + Tailwind CSS + shadcn/ui + Recharts + mqtt.js

## 目录结构

- `src/pages/` 四个页面；`src/sim/store.tsx` 总线驱动的状态层
- `src/mqtt/` 总线抽象、真实 Broker 客户端、回环模拟器与设备模拟器
- `src/providers/trpc.tsx` 任务存储（MQTT fleet/tasks）与原 tRPC 调用兼容垫片
- `contracts/task.ts` 任务枚举 / 文案 / 类型
- `src/assets/cam/` 摄像头场景底图（静态兜底 + 模拟帧源）
