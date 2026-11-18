/**
 * 内置开发用 MQTT Broker（aedes，零安装依赖，仅需 Node.js 18+）
 *
 * 一条命令同时提供：
 *   - TCP  1883  mqtt://<本机IP>:1883        机器人 / 设施端连接
 *   - WS   8083  ws://<本机IP>:8083/mqtt     看板前端连接（src/config.ts 的 brokerUrl）
 *
 * 用法：node broker.js  （或 npm run broker）
 * 注意：无认证、无 TLS，仅用于局域网开发联调；生产环境请换 EMQX / Mosquitto。
 */
import { Aedes } from 'aedes'
import { createServer as createTcpServer } from 'node:net'
import { createServer as createHttpServer } from 'node:http'
import { WebSocketServer } from 'ws'
import createWebSocketStream from 'websocket-stream'

const TCP_PORT = Number(process.env.MQTT_TCP_PORT || 1883)
const WS_PORT = Number(process.env.MQTT_WS_PORT || 8083)
const HOST = process.env.HOST || '0.0.0.0'

const aedes = await Aedes.createBroker()

// ---- TCP 1883（机器人） ----
const tcpServer = createTcpServer(aedes.handle)
tcpServer.listen(TCP_PORT, HOST, () => {
  console.log(`[broker] MQTT over TCP 已监听: mqtt://<本机IP>:${TCP_PORT}`)
})

// ---- WebSocket 8083/mqtt（看板） ----
const httpServer = createHttpServer()
const wss = new WebSocketServer({ noServer: true })
httpServer.on('upgrade', (req, socket, head) => {
  if (!req.url || !req.url.startsWith('/mqtt')) {
    socket.destroy()
    return
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    const stream = createWebSocketStream(ws)
    aedes.handle(stream)
  })
})
httpServer.listen(WS_PORT, HOST, () => {
  console.log(`[broker] MQTT over WebSocket 已监听: ws://<本机IP>:${WS_PORT}/mqtt`)
})

// ---- 运行日志 ----
const clients = new Set()
aedes.on('client', (c) => {
  clients.add(c.id)
  console.log(`[broker] 客户端上线: ${c.id}（当前 ${clients.size} 个）`)
})
aedes.on('clientDisconnect', (c) => {
  clients.delete(c.id)
  console.log(`[broker] 客户端下线: ${c.id}（当前 ${clients.size} 个）`)
})
aedes.on('clientError', (c, e) => {
  console.log(`[broker] 客户端错误: ${c?.id ?? '?'} ${e.message}`)
})
aedes.on('connackSent', (c) => {
  console.log(`[broker] 连接确认: ${c.id}`)
})

console.log('[broker] 开发用 Broker 启动中（无认证，仅限局域网联调）…')
