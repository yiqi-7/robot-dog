/**
 * 用无头 Chrome 验证看板视频链路（真实浏览器，走看板自己的 WHEP 逻辑）。
 *
 * 做法：
 *   1) 通过 CDP 打开看板页面
 *   2) 等待页面里 <video> 元素真正播放（readyState>=2 且 videoWidth>0）
 *   3) 抓取摄像头卡片上显示的模式文案（WEBRTC 实时流 / 图像帧兼容模式 / STANDBY）
 *   4) 截图存盘，可直接肉眼确认画面
 *
 * 用法：node test-video-cdp.mjs [页面地址]
 */
import { writeFileSync, mkdirSync } from 'node:fs'
import { setTimeout as sleep } from 'node:timers/promises'

const PAGE = process.argv[2] || 'http://127.0.0.1:8080/'
const PORT = Number(process.env.CDP_PORT || 9225)
const OUT = 'video-verify'

mkdirSync(OUT, { recursive: true })

// 本机 Chrome 的 --headless 模式起不来，且由 Node spawn 启动不稳定，
// 因此改为：Chrome 由外部先启动好（带 --remote-debugging-port），本脚本只连 CDP。
//   chrome.exe --remote-debugging-port=9225 --no-sandbox --user-data-dir=<tmp> about:blank
// 等 CDP 就绪
let target = null
for (let i = 0; i < 20; i++) {
  try {
    const r = await fetch(`http://127.0.0.1:${PORT}/json/list`)
    const list = await r.json()
    target = list.find((t) => t.type === 'page')
    if (target) break
  } catch { /* 还没起 */ }
  await sleep(500)
}
if (!target) {
  console.log(`✗ 无法连接 CDP (127.0.0.1:${PORT})；请先启动 Chrome:`)
  console.log(`  chrome.exe --remote-debugging-port=${PORT} --no-sandbox --user-data-dir=%TEMP%\\chrome-v4 about:blank`)
  process.exit(1)
}
console.log(`[1] Chrome 就绪，打开 ${PAGE}`)

const ws = new WebSocket(target.webSocketDebuggerUrl)
let msgId = 0
const pending = new Map()
ws.addEventListener('message', (ev) => {
  const m = JSON.parse(ev.data)
  if (m.id && pending.has(m.id)) {
    pending.get(m.id)(m)
    pending.delete(m.id)
  }
})
await new Promise((res) => ws.addEventListener('open', res))

const send = (method, params = {}) =>
  new Promise((res) => {
    const id = ++msgId
    pending.set(id, res)
    ws.send(JSON.stringify({ id, method, params }))
  })

const evalJs = async (expr) => {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })
  return r.result?.result?.value
}

await send('Page.enable')
await send('Runtime.enable')
await send('Page.navigate', { url: PAGE })
await sleep(6000)

// 等待真实播放（最多 40s）
let info = null
for (let i = 0; i < 20; i++) {
  info = await evalJs(`(() => {
    const vs = [...document.querySelectorAll('video')];
    const v = vs.find(x => x.videoWidth > 0) || vs[0];
    const txt = document.body.innerText;
    const mode = (txt.match(/WEBRTC 实时流|图像帧兼容模式[^\\n]*|WEBRTC 建连中[^\\n]*|等待 RTSP 推流[^\\n]*/) || [null])[0];
    return v ? {
      count: vs.length,
      videoWidth: v.videoWidth, videoHeight: v.videoHeight,
      readyState: v.readyState, paused: v.paused,
      hasStream: !!v.srcObject,
      mode,
    } : { count: 0, mode };
  })()`)
  console.log(`    [${i * 2}s] videoWidth=${info?.videoWidth} readyState=${info?.readyState} mode=${info?.mode}`)
  if (info?.videoWidth > 0 && info?.readyState >= 2) break
  await sleep(2000)
}

// 截图
const shot = await send('Page.captureScreenshot', { format: 'png' })
if (shot.result?.data) {
  writeFileSync(`${OUT}/dashboard.png`, Buffer.from(shot.result.data, 'base64'))
  console.log(`[2] 截图已存 ${OUT}/dashboard.png`)
}

// 控制台里的 broker 日志
const logs = await evalJs(`(() => document.title)()`)

console.log('\n=== 结果 ===')
const playing = info?.videoWidth > 0 && info?.readyState >= 2 && !info?.paused
console.log(`  页面标题: ${logs}`)
console.log(`  video 元素数: ${info?.count}`)
console.log(`  分辨率: ${info?.videoWidth}x${info?.videoHeight}`)
console.log(`  readyState: ${info?.readyState} (>=2 表示可播放)`)
console.log(`  有 MediaStream: ${info?.hasStream}`)
console.log(`  卡片模式: ${info?.mode}`)
console.log(playing ? '  ✓ 视频真实播放中' : '  ✗ 视频未播放')

ws.close()
process.exit(playing ? 0 : 1)
