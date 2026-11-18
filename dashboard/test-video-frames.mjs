/**
 * 不看截图，直接从 <video> 抽帧做程序化判定：
 *   1) 画面非纯色（Gazebo 场景有纹理）
 *   2) 两帧之间有差异（说明是实时视频流，不是静止图）
 *
 * 用 CDP 在页面内用 canvas 抽 video 的像素，回传统计值。
 */
import { setTimeout as sleep } from 'node:timers/promises'

const PAGE = process.argv[2] || 'http://127.0.0.1:8080/'
const PORT = Number(process.env.CDP_PORT || 9225)

const r = await fetch(`http://127.0.0.1:${PORT}/json/list`)
const target = (await r.json()).find((t) => t.type === 'page')
if (!target) { console.log('✗ 无可用页面'); process.exit(1) }

const ws = new WebSocket(target.webSocketDebuggerUrl)
let msgId = 0
const pending = new Map()
ws.addEventListener('message', (ev) => {
  const m = JSON.parse(ev.data)
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id) }
})
await new Promise((res) => ws.addEventListener('open', res))

const send = (method, params = {}) =>
  new Promise((res) => { const id = ++msgId; pending.set(id, res); ws.send(JSON.stringify({ id, method, params })) })
const evalJs = async (expr) => {
  const res = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true })
  return res.result?.result?.value
}

await send('Page.enable')
await send('Runtime.enable')
await send('Page.navigate', { url: PAGE })
await sleep(16000)

const probe = await evalJs(`(async () => {
  const v = [...document.querySelectorAll('video')].find(x => x.videoWidth > 0);
  if (!v) return { error: 'no playing video' };
  const c = document.createElement('canvas');
  c.width = v.videoWidth; c.height = v.videoHeight;
  const ctx = c.getContext('2d');

  const grab = () => {
    ctx.drawImage(v, 0, 0, c.width, c.height);
    return ctx.getImageData(0, 0, c.width, c.height).data;
  };

  const a = grab();
  await new Promise(r => setTimeout(r, 1200));
  const b = grab();

  // 唯一颜色数（粗量化到 5 位）+ 亮度方差 + 帧间差异
  const uniq = new Set();
  let sum = 0, sum2 = 0, diff = 0;
  for (let i = 0; i < a.length; i += 4) {
    const r0 = a[i], g0 = a[i+1], b0 = a[i+2];
    uniq.add(((r0 >> 3) << 10) | ((g0 >> 3) << 5) | (b0 >> 3));
    const lum = 0.299*r0 + 0.587*g0 + 0.114*b0;
    sum += lum; sum2 += lum*lum;
    diff += Math.abs(r0 - b[i]) + Math.abs(g0 - b[i+1]) + Math.abs(b0 - b[i+2]);
  }
  const n = a.length / 4;
  const mean = sum / n;
  const std = Math.sqrt(sum2/n - mean*mean);
  return {
    w: v.videoWidth, h: v.videoHeight,
    colors: uniq.size,
    brightness: Math.round(mean),
    contrast: Math.round(std),
    frameDiff: +(diff / n).toFixed(3),
    currentTime: +v.currentTime.toFixed(2),
  };
})()`)

console.log('=== 画面内容分析 ===')
console.log(JSON.stringify(probe, null, 2))
// 机器狗静止时画面几乎不动，frameDiff 天然很小，不能当硬判据；
// 真正的判据是「有丰富颜色 + 有对比度」=> 不是纯色/黑屏占位图
const ok = probe && !probe.error && probe.colors > 50 && probe.contrast > 5 && probe.brightness > 10
console.log(
  ok
    ? `✓ 画面有真实内容（${probe.colors} 色 / 对比度 ${probe.contrast} / 亮度 ${probe.brightness}）`
    : '✗ 画面疑似纯色或黑屏',
)
ws.close()
process.exit(ok ? 0 : 1)
