# 视频链路（端侧推流 → 云端中转 → 看板拉流）

```
Gazebo rgb 相机 → /rgb_camera → image_sender(ffmpeg, H.264 禁B帧)
                                    ↓ RTSP push (TCP)
                          rtsp://<中转服务器>:8554/cam/<robotId>
                                    ↓
                         MediaMTX（云端，RTSP 8554 / WebRTC 8889）
                                    ↓ WHEP
                    看板浏览器 http://<中转服务器>:8889/cam/<id>/whep
```

## 组件与端口

| 组件 | 位置 | 端口 | 说明 |
|---|---|---|---|
| `image_sender` | 端侧 | 出向 | 订阅 `/rgb_camera`，OSD 烧录后交 ffmpeg |
| ffmpeg | 端侧 | 出向 | 640×480@15fps，`libx264 -profile baseline -bf 0`，RTSP over TCP |
| MediaMTX | 云端 | 8554 RTSP / 8889 WebRTC | `webrtcAdditionalHosts` 必须填公网 IP |
| 看板 | 本地 | 8080 页面 | WHEP 拉流走 `useWhep.ts` |

端侧启动参数（`quadruped_gz.launch.py`）：

```python
"rtsp_server": "43.172.91.67",   # 中转服务器公网 IP
"rtsp_port": 8554,
```

## MediaMTX 配置要点

```yaml
webrtc: yes
webrtcAddress: :8889
# 关键：不填公网 IP，浏览器只能拿到内网候选，公网访问必然建连失败
webrtcAdditionalHosts: [43.172.91.67]

paths:
  cam/robot-01:
  # 兜底：允许任意 cam/<robotId>，换机器狗/多机时不用改配置
  all_others:
```

> 只写显式 `paths` 而不加 `all_others` 时，任何未声明的路径都会被拒绝，
> 日志报 `path 'cam/xxx' is not configured`，表现为"看板一直转圈但端侧明明在推流"。

## 看板侧要点

1. **WHEP 地址**由 `qbot-config.js` 的 `streaming.whepBase` 决定，走云端时填
   `http://<中转服务器>:8889`。
2. **STUN**：公网/NAT 场景建议配 `streaming.iceServers = ["stun:stun.l.google.com:19302"]`；
   纯局域网留空即可。
3. **占位机器人不拉流**：`store.tsx` 在真实遥测到达前有一个 `id: "--"` 的占位机器人。
   `CameraCard` 必须跳过它（传 `null` 给 `useWhep`），否则会不停请求
   `cam/--/whep`，云端报 `path 'cam/--' is not configured` 并形成无谓重试。

## 实测数据（端侧→腾讯云→本地看板，公网）

| 检查项 | 结果 |
|---|---|
| 端侧 ffmpeg 推流连接 | TCP ESTABLISHED，持续保持 |
| 云端 MediaMTX 发布者 | `is publishing to path 'cam/robot-01'` |
| 公网 RTSP 探测（本机 ffprobe） | `h264 / 640x480 / 15fps`，可解码 |
| WHEP 端点（curl POST SDP） | `HTTP 201` + 有效 answer，含 `m=video H264/90000` |
| 看板真实浏览器（Chrome CDP） | `videoWidth=640 readyState=4`，卡片显示 `WEBRTC 实时流` |
| 画面内容分析 | 502 色 / 对比度 45 / 亮度 113（非纯色、非黑屏） |
| MediaMTX 重启后自愈 | 端侧 ffmpeg 自动重连，无需人工干预 |

## 验证脚本

```bash
# 1) 公网 RTSP 是否真有流（任意装了 ffmpeg 的机器）
ffprobe -v error -rtsp_transport tcp -select_streams v:0 \
  -show_entries stream=codec_name,width,height,avg_frame_rate \
  -i rtsp://43.172.91.67:8554/cam/robot-01 -read_intervals "%+5"

# 2) WHEP 端点是否响应（在云端或任意 Linux 上）
curl -i -X POST -H "Content-Type: application/sdp" \
  --data-binary @offer.sdp http://43.172.91.67:8889/cam/robot-01/whep

# 3) 看板真实浏览器拉流（先手工启动 Chrome 带调试端口）
chrome.exe --remote-debugging-port=9225 --no-sandbox --user-data-dir=%TEMP%\chrome-v5 about:blank
node dashboard/test-video-cdp.mjs       # 判定 videoWidth/readyState
node dashboard/test-video-frames.mjs    # 判定画面是否有真实内容
```

> 本机 Chrome 的 `--headless=new` / `--headless=old` 都起不来（启动器进程立刻退出），
> 因此验证脚本只负责连 CDP，Chrome 需外部启动；截图与像素分析能力不受影响。

## 已知风险

- **MediaMTX 当前允许匿名发布与读取**（`authInternalUsers` 里 `user: any`、IP 不限制）。
  公网 8554 开放意味着任何人都能往 `cam/robot-01` 推流覆盖真实画面，也能随意拉流。
  生产环境应加账号密码：推流端 URL 带 `rtsp://user:pass@host:8554/cam/<id>`，
  看板侧 WHEP 需相应带上凭据。
- **带宽**：1Mbps 恒定码率，端侧实测上行 RTT 851~1590ms 时可能丢帧，
  弱网档位下视频会降质但 MQTT 遥测不受影响（两条独立链路）。
