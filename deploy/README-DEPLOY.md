# 部署指南：两种运行模式

本项目支持两种部署形态。**先用模式 1 在单机上通读全部代码，再切模式 2 上云端做远程控制。**

| | 模式 1：本地全栈 | 模式 2：云端分离 |
|---|---|---|
| 适用场景 | 本地学代码、单机演示、无外网 | 生产部署、远程控制 |
| ROS2 仿真 | TITAN_V | TITAN_V |
| MQTT Broker | TITAN_V | 腾讯云 `43.172.91.67` |
| 视频中转（MediaMTX） | TITAN_V（需自装，未装则降级） | 腾讯云 |
| 语音网关（ASR/LLM） | TITAN_V | 腾讯云 |
| 看板页面 | TITAN_V:8080 | 云端:8080 |
| 浏览器访问 | `http://<TITAN_V-IP>:8080` | `http://43.172.91.67:8080` |

---

## 模式 1：本地全栈（TITAN_V 单机）

```bash
cd /run/media/wby/shuju/Document/robot-dog/tools/deploy

./switch_config.sh local        # 看板配置指向 127.0.0.1
./mode1_local_all.sh            # 一条命令起全部
```

启动内容（按顺序）：

1. `mosquitto`（systemd，监听 1883 + 8083 WebSocket）
2. 看板 `server.js`（8080，从 `quadruped-ops/dist/` 提供静态文件）
3. 语音网关 `voice-gateway.js`（`MQTT_URL=mqtt://127.0.0.1:1883`）
4. ROS2 仿真 + 四个 MQTT 节点（`broker_host:=127.0.0.1`）

常用选项：

```bash
./mode1_local_all.sh --no-ros      # 只起 broker/看板/网关，不跑仿真
./mode1_local_all.sh --no-gw       # 不起语音网关
./mode1_local_all.sh --build       # 启动前重新构建前端
./mode1_local_all.sh --wait 150    # 延长初始化等待
```

停止：`./stop_all.sh local`

### 模式 1 的视频说明

TITAN_V 默认**没有** MediaMTX，因此视频主通道（RTSP→WebRTC）不可用，看板会自动降级到
**MQTT-JPEG 兜底帧**（1 fps 快照，约 17.7KB/帧 @640×360）。功能可用，只是不流畅。

需要主通道时，把云端的二进制和配置拷过来：

```bash
# 在云端
sudo tar czf /tmp/mediamtx.tgz /usr/local/bin/mediamtx /etc/mediamtx/mediamtx.yml
# 拷到 TITAN_V 后
sudo tar xzf mediamtx.tgz -C /
# 把 webrtcAdditionalHosts 改成 TITAN_V 的局域网 IP，再
sudo systemctl enable --now mediamtx   # 或手工 /usr/local/bin/mediamtx /etc/mediamtx/mediamtx.yml
```

---

## 模式 2：云端分离（生产 / 远程控制）

### ① 云端一侧（腾讯云 43.172.91.67）

```bash
# 云端放一份看板代码（含 node_modules）
export APP_DIR=/opt/qbot/dashboard
./mode2_cloud_side.sh
```

启动内容：`mosquitto`（1883/8083）、`mediamtx`（8554/8889）、`voice-gateway.js`、`server.js`（8080）

健康检查：`./mode2_cloud_side.sh --check`

### ② 机器人一侧（TITAN_V）

```bash
./switch_config.sh cloud        # 若看板也在本机跑，切到云端地址
./mode2_robot_side.sh
```

启动内容：ROS2 + 四个 MQTT 节点（`broker_host:=43.172.91.67`）+ ffmpeg 推流到
`rtsp://43.172.91.67:8554/cam/robot-01`

脚本会先做连通性预检（MQTT 1883 / RTSP 8554 是否可达），不可达会明确告警。

停止：`./stop_all.sh cloud`（云端）、`./stop_all.sh robot`（端侧）

---

## 端口一览

| 端口 | 服务 | 说明 |
|---|---|---|
| 1883 | MQTT TCP | 端侧四个节点上报/订阅 |
| 8083 | MQTT over WebSocket | 浏览器看板连接 |
| 8554 | RTSP | 端侧 ffmpeg 推流入口 |
| 8889 | WebRTC/WHEP | 浏览器拉流 |
| 8080 | HTTP | 看板页面 |

云端安全组需放行 **1883 / 8083 / 8554 / 8889 / 8080**。

---

## 验证清单

### 模式 1

```bash
# 1. 看板可访问
curl -s -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8080/

# 2. 遥测在流动
mosquitto_sub -h 127.0.0.1 -t 'qbot/v1/robots/robot-01/state' -C 2

# 3. 摄像头兜底帧
mosquitto_sub -h 127.0.0.1 -t 'qbot/v1/robots/robot-01/camera' -F '%l' -C 3

# 4. WebSocket 握手（看板实际通道）
curl -s -i -N -H 'Connection: Upgrade' -H 'Upgrade: websocket' \
  -H 'Sec-WebSocket-Version: 13' -H 'Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==' \
  -H 'Sec-WebSocket-Protocol: mqtt' http://127.0.0.1:8083/mqtt --max-time 3 | head -2
```

### 模式 2 额外项

```bash
# 云端：视频是否真有流（本机有 ffmpeg 时）
ffprobe -v error -rtsp_transport tcp -select_streams v:0 \
  -show_entries stream=codec_name,width,height,avg_frame_rate \
  -i rtsp://43.172.91.67:8554/cam/robot-01 -read_intervals '%+5'

# 云端：WHEP 端点是否响应
curl -s -o /dev/null -w 'HTTP %{http_code}\n' -X POST \
  -H 'Content-Type: application/sdp' --data-binary @offer.sdp \
  http://43.172.91.67:8889/cam/robot-01/whep
```

---

## 常见故障

| 现象 | 原因 | 处理 |
|---|---|---|
| 看板一直"机器人离线" | 浏览器连的 broker 地址不对 | `./switch_config.sh show` 确认；改完刷新浏览器（无需重启） |
| 看板显示"等待 RTSP 推流" | 推流没到 / 路径没配 | 云端 `journalctl -u mediamtx \| grep publishing`；确认 `paths` 有 `all_others` |
| 视频卡片报 `path 'cam/--' is not configured` | 占位机器人也在拉流 | 已修（`CameraCard` 跳过 `id === "--"`），确认用的是新构建 |
| 模式 1 没有视频只有快照 | 本机没装 MediaMTX | 见上文"模式 1 的视频说明"，属预期行为 |
| 语音一直"没听清" | ASR 模型缺失 / 音频格式不支持 | 网关日志看 `[asr]` 行；确认 `models/sensevoice` 存在、`format` 为 `opus` 或 `pcm_s16le` |
| 端侧连不上云端 MQTT | 安全组未放行 / 云端未起 | `./mode2_robot_side.sh --no-ros` 做预检 |
| 重复执行脚本后进程叠加 | 旧进程没清干净 | 先 `./stop_all.sh all --with-infra`，再启动 |

---

## launch 参数说明

`quadruped_gz.launch.py` 已支持 `broker_host` / `broker_port`，会传给四个节点。
但 `image_sender` 的 **RTSP 推流地址 `rtsp_server` 仍是写死的**，切换模式时脚本用 `sed`
临时改写并在退出时恢复（备份为 `quadruped_gz.launch.py.rtsp.bak`）。

> 更彻底的做法是把 `rtsp_server` 也提升为 launch 参数，这样就不需要改文件。
> 目前的做法是为了不引入额外的接口变更。
