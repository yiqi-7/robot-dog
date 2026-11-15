/**
 * 厂区监控运维看板 · 生产静态服务器（零依赖，仅需 Node.js 18+）
 *
 * 用法：
 *   node server.js                    # 默认 0.0.0.0:8080
 *   set PORT=9000 && node server.js   # 指定端口
 *
 * 局域网内其他电脑访问：http://<本机IP>:8080
 */
import { createServer } from 'node:http'
import { readFile, stat } from 'node:fs/promises'
import { extname, join, normalize, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = resolve(fileURLToPath(new URL('./dist', import.meta.url)))
const CLIPS = resolve(fileURLToPath(new URL('./voice-clips', import.meta.url)))
const PORT = Number(process.env.PORT || 8080)
const HOST = process.env.HOST || '0.0.0.0'

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.jpeg': 'image/jpeg',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.woff': 'font/woff',
  '.woff2': 'font/woff2',
  '.wav': 'audio/wav',
  '.map': 'application/json',
}

createServer(async (req, res) => {
  try {
    const url = decodeURIComponent((req.url || '/').split('?')[0])
    // 语音片段回放目录（voice-gateway 落盘的 WAV）
    if (url.startsWith('/voice-clips/')) {
      const clip = normalize(join(CLIPS, url.slice('/voice-clips'.length)))
      if (!clip.startsWith(CLIPS)) {
        res.writeHead(403).end('Forbidden')
        return
      }
      const st = await stat(clip).catch(() => null)
      if (!st || st.isDirectory()) {
        res.writeHead(404).end('Not Found')
        return
      }
      const body = await readFile(clip)
      res.writeHead(200, { 'Content-Type': 'audio/wav', 'Cache-Control': 'no-cache' })
      res.end(body)
      return
    }
    let file = normalize(join(ROOT, url))
    if (!file.startsWith(ROOT)) {
      res.writeHead(403).end('Forbidden')
      return
    }
    const st = await stat(file).catch(() => null)
    if (!st || st.isDirectory()) file = join(ROOT, 'index.html') // SPA 回退
    const body = await readFile(file)
    res.writeHead(200, {
      'Content-Type': MIME[extname(file).toLowerCase()] || 'application/octet-stream',
      'Cache-Control': file.includes('assets') ? 'public, max-age=31536000, immutable' : 'no-cache',
    })
    res.end(body)
  } catch {
    res.writeHead(500).end('Internal Server Error')
  }
}).listen(PORT, HOST, () => {
  console.log(`厂区监控运维看板已启动: http://localhost:${PORT}  (局域网: http://<本机IP>:${PORT})`)
  console.log(`静态目录: ${ROOT}`)
  console.log(`语音片段: ${CLIPS} → /voice-clips/`)
})
