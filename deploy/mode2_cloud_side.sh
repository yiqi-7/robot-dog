#!/usr/bin/env bash
# ===========================================================================
# 模式 2 · 云端一侧（在腾讯云 43.172.91.67 上执行）
#
#   本脚本负责云端全部服务：MQTT Broker + MediaMTX + 语音网关 + 看板页面
#   机器人侧（TITAN_V）由 mode2_robot_side.sh 启动，只跑 ROS2 并推流/上报到本机
#
#   数据流：
#     机器人 ──MQTT 1883──▶ mosquitto ──8083(ws)──▶ 浏览器看板(8080)
#     机器人 ──RTSP 8554──▶ MediaMTX ──8889(WHEP)──▶ 浏览器看板
#     机器人 ──语音上行──▶ voice-gateway(本机) → ASR/LLM → 任务下发
# ===========================================================================
set -euo pipefail

# ---------------------------- 配置 ----------------------------
APP_DIR="${APP_DIR:-/opt/qbot/dashboard}"     # 看板/网关代码目录（需含 node_modules）
DASH_PORT="${DASH_PORT:-8080}"
MQTT_PORT="${MQTT_PORT:-1883}"
WS_PORT="${WS_PORT:-8083}"
RTSP_PORT="${RTSP_PORT:-8554}"
WHEP_PORT="${WHEP_PORT:-8889}"
PUBLIC_IP="${PUBLIC_IP:-43.172.91.67}"        # MediaMTX 对外宣告的候选地址
LOG_DASH="/tmp/qbot_cloud_dash.log"
LOG_GW="/tmp/qbot_cloud_gw.log"
NODE_BIN="$(command -v node || true)"

usage() {
  cat <<'EOF'
用法: ./mode2_cloud_side.sh [选项]

云端一侧：启动 MQTT Broker、MediaMTX、语音网关、看板页面。

选项:
  -h, --help        显示本帮助
  --app-dir DIR     看板代码目录（默认 /opt/qbot/dashboard）
  --no-dash         不起看板页面
  --no-gw           不起语音网关
  --build           启动前构建看板前端（npm run build）
  --check           只做健康检查，不启动任何服务

环境变量:
  APP_DIR DASH_PORT MQTT_PORT WS_PORT RTSP_PORT WHEP_PORT PUBLIC_IP
EOF
}

NO_DASH=0; NO_GW=0; DO_BUILD=0; ONLY_CHECK=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --app-dir) APP_DIR="$2"; shift 2 ;;
    --no-dash) NO_DASH=1; shift ;;
    --no-gw)   NO_GW=1; shift ;;
    --build)   DO_BUILD=1; shift ;;
    --check)   ONLY_CHECK=1; shift ;;
    *) echo "未知参数: $1（-h 查看帮助）"; exit 1 ;;
  esac
done

info() { printf '\033[36m[cloud]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[cloud]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[cloud]\033[0m %s\n' "$*" >&2; exit 1; }

wait_port() {
  local port="$1" name="$2" tries="${3:-30}" i
  for ((i=1; i<=tries; i++)); do
    if ss -lnt 2>/dev/null | grep -q ":${port} "; then info "${name} 已监听 :${port}"; return 0; fi
    sleep 1
  done
  warn "${name} 未监听 :${port}"; return 1
}

# ---------------------------- 健康检查 ----------------------------
health_check() {
  info "=== 云端健康检查 ==="
  local bad=0
  for svc in mosquitto mediamtx; do
    if systemctl is-active --quiet "$svc"; then info "${svc}: active"; else warn "${svc}: 未运行"; bad=1; fi
  done
  for p in "$MQTT_PORT:MQTT" "$WS_PORT:MQTT-WS" "$RTSP_PORT:RTSP" "$WHEP_PORT:WHEP" "$DASH_PORT:看板"; do
    local port="${p%%:*}" name="${p##*:}"
    if ss -lnt 2>/dev/null | grep -q ":${port} "; then info "${name} :${port} 监听中"; else warn "${name} :${port} 未监听"; bad=1; fi
  done
  # RTSP 是否有端侧推流
  if ss -tn 2>/dev/null | grep -q ":${RTSP_PORT} .*ESTAB"; then
    info "RTSP 有端侧推流连接"
  else
    warn "RTSP 无推流连接（机器人侧未启动？）"
  fi
  # MQTT 是否有机器人遥测
  if command -v mosquitto_sub >/dev/null 2>&1; then
    if timeout 12 mosquitto_sub -h 127.0.0.1 -p "$MQTT_PORT" \
         -t "qbot/v1/robots/robot-01/state" -C 1 -W 10 >/dev/null 2>&1; then
      info "收到机器人遥测（robot-01/state）"
    else
      warn "10 秒内未收到遥测（机器人侧未启动？）"
    fi
  fi
  return "$bad"
}

if [[ "$ONLY_CHECK" -eq 1 ]]; then health_check; exit $?; fi

# ---------------------------- 前置 ----------------------------
[[ -n "$NODE_BIN" ]] || die "未找到 node"
[[ -d "$APP_DIR" ]] || die "看板目录不存在: $APP_DIR（用 --app-dir 指定）"
[[ -d "$APP_DIR/node_modules" ]] || warn "警告: $APP_DIR 缺少 node_modules，看板/网关可能起不来"

info "=== 模式 2：云端一侧 ==="

# 1) MQTT Broker
if ! systemctl is-active --quiet mosquitto; then
  info "启动 mosquitto…"
  sudo systemctl start mosquitto || die "mosquitto 启动失败"
fi
wait_port "$MQTT_PORT" "MQTT" || die "MQTT 未就绪"
wait_port "$WS_PORT" "MQTT-WebSocket" || warn "WebSocket 未监听，看板将无法连接"

# 2) MediaMTX
if [[ -x /usr/local/bin/mediamtx ]] || systemctl list-unit-files 2>/dev/null | grep -q '^mediamtx'; then
  if ! systemctl is-active --quiet mediamtx; then
    info "启动 mediamtx…"
    sudo systemctl start mediamtx || warn "mediamtx 启动失败"
  fi
  wait_port "$RTSP_PORT" "RTSP" || warn "RTSP 未监听"
  wait_port "$WHEP_PORT" "WHEP" || warn "WHEP 未监听"
  # 关键：MediaMTX 必须向浏览器宣告公网 IP，否则 ICE 建连失败
  if ! sudo grep -q "$PUBLIC_IP" /etc/mediamtx/mediamtx.yml 2>/dev/null; then
    warn "mediamtx.yml 中未见 ${PUBLIC_IP}，公网 WebRTC 可能建连失败"
    warn "  应配置: webrtcAdditionalHosts: [${PUBLIC_IP}]"
  fi
else
  warn "未检测到 mediamtx，视频主通道不可用（看板降级到 MQTT-JPEG）"
fi

# 3) 看板前端构建（可选）
if [[ "$DO_BUILD" -eq 1 ]]; then
  info "构建看板前端…"
  ( cd "$APP_DIR" && npm run build >/tmp/qbot_cloud_build.log 2>&1 ) || die "构建失败，见 /tmp/qbot_cloud_build.log"
fi

# 4) 看板页面
if [[ "$NO_DASH" -eq 0 ]]; then
  if ss -lnt 2>/dev/null | grep -q ":${DASH_PORT} "; then
    info "看板已在 :${DASH_PORT} 运行"
  else
    info "启动看板 (${DASH_PORT})…"
    ( cd "$APP_DIR" && setsid nohup env PORT="$DASH_PORT" "$NODE_BIN" server.js >"$LOG_DASH" 2>&1 & )
    wait_port "$DASH_PORT" "看板" 20 || true
  fi
fi

# 5) 语音网关
if [[ "$NO_GW" -eq 0 ]]; then
  if pgrep -f "voice-gateway\.j[s]" >/dev/null 2>&1; then
    info "重启语音网关…"
    pkill -f "voice-gateway\.j[s]" 2>/dev/null || true
    sleep 2
  fi
  info "启动语音网关…"
  ( cd "$APP_DIR" && setsid nohup env \
      MQTT_URL="mqtt://127.0.0.1:${MQTT_PORT}" \
      TOPIC_PREFIX="qbot/v1" \
      "$NODE_BIN" voice-gateway.js >"$LOG_GW" 2>&1 & )
  sleep 5
  grep -q "已连接" "$LOG_GW" 2>/dev/null && info "语音网关已连上 MQTT" || warn "语音网关未连上，见 $LOG_GW"
fi

sleep 2
health_check || true

cat <<EOF

========================= 模式 2 云端已就绪 =========================
  看板页面 : http://${PUBLIC_IP}:${DASH_PORT}
  MQTT     : ${PUBLIC_IP}:${MQTT_PORT}   （浏览器 ws://${PUBLIC_IP}:${WS_PORT}/mqtt）
  视频     : rtsp://${PUBLIC_IP}:${RTSP_PORT}/cam/robot-01 → WHEP http://${PUBLIC_IP}:${WHEP_PORT}
  日志     :
    看板  ${LOG_DASH}
    网关  ${LOG_GW}

  下一步   : 在机器人（TITAN_V）上执行 ./mode2_robot_side.sh
  健康检查 : ./mode2_cloud_side.sh --check
  停止     : ./stop_all.sh cloud
=====================================================================
EOF