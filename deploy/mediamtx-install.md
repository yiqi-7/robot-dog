# MediaMTX 流媒体服务器部署（43.172.91.67）

## 用途
机器人 TITAN V 推 RTSP (cam/robot-01) -> MediaMTX 中转 -> 看板 WebRTC(WHEP) 拉流。

## 部署步骤（在服务器上）
```bash
# 1. 下载（版本以 latest 为准）
curl -L -o /tmp/mediamtx.tar.gz \
  https://github.com/bluenviron/mediamtx/releases/download/v1.21.4/mediamtx_v1.21.4_linux_amd64.tar.gz
cd /tmp && tar xzf mediamtx.tar.gz
sudo install -m 755 /tmp/mediamtx /usr/local/bin/mediamtx

# 2. 配置（见同目录 mediamtx.yml，要点：hls: no 避免 :8888 与 sub2api 冲突）
sudo mkdir -p /etc/mediamtx
sudo cp mediamtx.yml /etc/mediamtx/mediamtx.yml

# 3. systemd
sudo cp mediamtx.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now mediamtx
```

## 坑位记录
1. v1.21 HLS 默认监听 :8888 —— 与服务器已有 sub2api 冲突，必须 `hls: no`
2. authInternalUsers 的 `user: any` 不能带 `pass:` 字段（启动即报错）
3. `ips: [ ]` = 放行所有来源

## 机器人端推流命令（image_sender 节点内嵌，等价命令行）
```bash
ffmpeg -f rawvideo -pix_fmt rgb24 -s 640x480 -r 15 -i pipe:0 \
  -vf format=yuv420p -c:v libx264 -profile:v baseline -bf 0 \
  -tune zerolatency -preset ultrafast -g 30 -b:v 1M \
  -f rtsp -rtsp_transport tcp rtsp://43.172.91.67:8554/cam/robot-01
```
`-bf 0` 禁 B 帧是用户明确要求（低延迟，解码不依赖未来帧）。

## 看板拉流
config.ts 的 STREAMING.whepBase = http://43.172.91.67:8889
流路径约定：cam/{robotId}，WHEP 端点 = {whepBase}/cam/{robotId}/whep
