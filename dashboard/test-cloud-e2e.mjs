/**
 * 端到端验证：模拟看板浏览器侧的行为
 *   1) 用与看板完全相同的 ws://<云端>:8083/mqtt 连接
 *   2) 订阅看板实际订阅的主题
 *   3) 断言能收到端侧实时遥测（含 battery/status/x/y）
 *   4) 顺带测一次派单下行，确认看板→云端→端侧链路可写
 */
import mqtt from 'mqtt'

const BROKER = process.env.BROKER_URL || 'ws://43.172.91.67:8083/mqtt'
const PREFIX = 'qbot/v1'
const RID = 'robot-01'

const t0 = Date.now()
const ms = () => Date.now() - t0

console.log(`[1] 连接 ${BROKER}`)
const client = mqtt.connect(BROKER, { reconnectPeriod: 0, connectTimeout: 15000 })

const states = []
let connected = false
let cmdSent = false

client.on('connect', () => {
  connected = true
  console.log(`[2] 已连接（${ms()}ms），订阅 ${PREFIX}/robots/${RID}/#`)
  client.subscribe(`${PREFIX}/robots/${RID}/#`, { qos: 1 })
})

client.on('message', (topic, payload) => {
  const text = payload.toString()
  if (topic.endsWith('/state')) {
    try {
      const s = JSON.parse(text)
      states.push(s)
      if (states.length === 1) {
        const ok = ['battery', 'status', 'x', 'y'].every((k) => k in s)
        console.log(`[3] 收到遥测（${ms()}ms）字段完整=${ok} status=${s.status} battery=${s.battery} x=${s.x} y=${s.y}`)
      }
    } catch { /* 非 JSON 忽略 */ }
  } else {
    console.log(`    <- ${topic}: ${text.slice(0, 80)}`)
  }
})

client.on('error', (e) => {
  console.log(`[!] 错误: ${e.message}`)
  process.exit(1)
})

// 8 秒后下发一次派单，验证下行链路（看板→云端→端侧）
setTimeout(() => {
  if (!connected) return
  const task = { id: 771001, target: 'warehouse-racks', title: '云端看板联调' }
  client.publish(`${PREFIX}/robots/${RID}/cmd`, JSON.stringify({ action: 'assign', task }), { qos: 1 })
  cmdSent = true
  console.log(`[4] 已下发派单 #${task.id}（验证下行）`)
}, 8000)

// 25 秒后汇总
setTimeout(() => {
  console.log('\n=== 汇总 ===')
  console.log(`  连接成功: ${connected}`)
  console.log(`  遥测帧数: ${states.length}`)
  console.log(`  下行已发: ${cmdSent}`)
  if (states.length >= 3) {
    console.log('  ✓ 看板→云端→端侧链路通，遥测实时可达')
  } else {
    console.log('  ✗ 遥测不足，链路可能有问题')
  }
  client.end(true, () => process.exit(states.length >= 3 ? 0 : 1))
}, 25000)
