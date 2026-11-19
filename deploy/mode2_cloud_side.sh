#!/usr/bin/env bash
# ===========================================================================
# 模式 2 · 云端一侧（在中转服务器上执行）
#
#   本脚本负责云端全部服务：MQTT Broker + MediaMTX + 语音网关 + 看板页面
#   机器人侧由 mode2_robot_side.sh 启动，只跑 ROS2 并推流/上报到本机
#
#   数据流：
#     机器人 ──MQTT 1883──▶ mosquitto ──8083(ws)──▶ 浏览器看板(8080)
#     机器人 ──RTSP 8554──▶ MediaMTX ──8889(WHEP)──▶ 浏览器看板
#     机器人 ──语音上行──▶ voice-gateway(本机) → ASR/LLM → 任务下发
# ===========================================================================
set -euo pipefail

# 真实部署地址放在 deploy/qbot.env（不入库），参考同目录 qbot.env.example
_qbot_env="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/qbot.env"
if [[ -f "$_qbot_env" ]]; then . "$_qbot_env"; fi

# ---------------------------- 配置 ----------------------------
APP_DIR="${APP_DIR:-/opt/qbot/dashboard}"     # 看板/网关代码目录（需含 node_modules）
DASH_PORT="${DASH_PORT:-8080}"
MQTT_PORT="${MQTT_PORT:-1883}"
WS_PORT="${WS_PORT:-8083}"
RTSP_PORT="${RTSP_PORT:-8554}"
WHEP_PORT="${WHEP_PORT:-8889}"
PUBLIC_IP="${PUBLIC_IP:-}"                    # MediaMTX 对外宣告地址，由 qbot.env 提供
LOG_DASH="/tmp/qbot_cloud_dash.log"
LOG_GW="/tmp/qbot_cloud_gw.log"
# 定位 node：优先系统 PATH，其次常见独立安装目录（云端装在 /opt/qbot/node）
resolve_node() {
  local c
  for c in "$(command -v node 2>/dev/null || true)" \
           /opt/qbot/node/bin/node \
           /usr/local/bin/node \
           /usr/bin/node; do
    if [[ -n "$c" && -x "$c" ]]; then
      printf '%s' "$c"
      return 0
    fi
  done
  return 1
}
NODE_BIN="${NODE_BIN:-$(resolve_node || true)}"
# 把 node 所在目录加入 PATH，npm/ffmpeg 等工具链才能正常调用
if [[ -n "$NODE_BIN" ]]; then
  export PATH="$(dirname "$NODE_BIN"):${PATH}"
fi

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


# ---- pipefail 安全的条件判断 ----
# set -o pipefail 下 `cmd | grep -q` 会因 grep 命中后立即退出、左侧命令继续写管道
# 而收到 SIGPIPE，整条管道返回 141，导致 if 条件假失败（同一条件时而命中时而不命中）。
# 统一先把输出收进变量再匹配，彻底避开这个坑。
listening_on() {          # listening_on <port>：本机是否在监听该 TCP 端口
  local out
  out="$(ss -lnt 2>/dev/null || true)"
  grep -q ":$1 " <<<"$out"
}

unit_exists() {           # unit_exists <unit-prefix>：是否存在该 systemd 单元
  local out
  out="$(systemctl list-unit-files 2>/dev/null || true)"
  grep -q "^$1" <<<"$out"
}
info() { printf '\033[36m[cloud]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[cloud]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[cloud]\033[0m %s\n' "$*" >&2; exit 1; }

wait_port() {
  local port="$1" name="$2" tries="${3:-30}" i
  local line
  for ((i=1; i<=tries; i++)); do
    line="$(ss -lnt 2>/dev/null || true)"
    if grep -q ":${port} " <<<"$line"; then info "${name} 已监听 :${port}"; return 0; fi
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
    if listening_on "${port}"; then info "${name} :${port} 监听中"; else warn "${name} :${port} 未监听"; bad=1; fi
  done
  # RTSP 是否有推流/拉流连接
  #
  # 注意 ss 的输出把状态放在最左列（`ESTAB 0 0 [::ffff:10.4.0.7]:8554 ...`），
  # 所以正则必须是 `ESTAB.*:端口`；写成 `:端口 .*ESTAB` 永远匹配不到。
  # MediaMTX 的 API 在本项目里是关掉的，无法精确区分"发布者"与"观看者"，
  # 因此这里报告的是"存在活跃的 RTSP 连接"，两端计数分开统计更直观。
  local rtsp_n
  rtsp_n=$(ss -tn 2>/dev/null | grep -cE "ESTAB.*:${RTSP_PORT}\b" || true)
  if [[ "${rtsp_n:-0}" -ge 1 ]]; then
    info "RTSP 有 ${rtsp_n} 条活跃连接（推流或拉流）"
  else
    warn "RTSP 无活跃连接（机器人侧未启动推流？）"
    bad=1
  fi
  # MQTT 是否有机器人**实时**遥测
  #
  # 注意：机器人掉线时 broker 代发的遗嘱是 retained 的，`-C 1` 会立刻拿到这条
  # 陈旧消息，从而误判"机器人在线"（实测内容为 battery=null、status=offline）。
  # 因此要求连续收到 2 条且最后一条不是 offline：在线时 1Hz 推送，8 秒内必有 2 条。
  if command -v mosquitto_sub >/dev/null 2>&1; then
    local_tele=$(timeout 14 mosquitto_sub -h 127.0.0.1 -p "$MQTT_PORT" \
         -t "qbot/v1/robots/robot-01/state" -C 2 -W 12 2>/dev/null || true)
    local_cnt=$(printf '%s' "$local_tele" | grep -c . || true)
    if [[ "${local_cnt:-0}" -ge 2 ]] && [[ "$(printf '%s' "$local_tele" | tail -1)" != *'"offline"'* ]]; then
      info "收到机器人实时遥测（robot-01/state，连续 2 帧）"
    elif [[ "${local_cnt:-0}" -ge 1 ]]; then
      warn "只收到 1 条遥测（疑似 retained 的遗嘱/陈旧快照），机器人可能未在线"
      bad=1
    else
      warn "12 秒内未收到遥测（机器人侧未启动？）"
      bad=1
    fi
  fi
  return "$bad"
}

if [[ "$ONLY_CHECK" -eq 1 ]]; then health_check; exit $?; fi

# ---------------------------- 前置 ----------------------------
[[ -n "$NODE_BIN" ]] || die "未找到 node"
[[ -d "$APP_DIR" ]] || die "看板目录不存在: $APP_DIR（用 --app-dir 指定）"
[[ -d "$APP_DIR/node_modules" ]] || warn "警告: $APP_DIR 缺少 node_modules，看板/网关可能起不来"
[[ -f "$APP_DIR/server.js" ]] || die "$APP_DIR 缺少 server.js"
[[ -f "$APP_DIR/dist/index.html" ]] || warn "警告: $APP_DIR/dist/index.html 不存在，看板页面会 404（前端未构建？）"

info "=== 模式 2：云端一侧 ==="

# 1) MQTT Broker
if ! systemctl is-active --quiet mosquitto; then
  info "启动 mosquitto…"
  sudo systemctl start mosquitto || die "mosquitto 启动失败"
fi
wait_port "$MQTT_PORT" "MQTT" || die "MQTT 未就绪"
wait_port "$WS_PORT" "MQTT-WebSocket" || warn "WebSocket 未监听，看板将无法连接"

# 2) MediaMTX
if [[ -x /usr/local/bin/mediamtx ]] || unit_exists 'mediamtx'; then
  if ! systemctl is-active --quiet mediamtx; then
    info "启动 mediamtx…"
    sudo systemctl start mediamtx || warn "mediamtx 启动失败"
  fi
  wait_port "$RTSP_PORT" "RTSP" || warn "RTSP 未监听"
  wait_port "$WHEP_PORT" "WHEP" || warn "WHEP 未监听"
  # 关键：MediaMTX 必须向浏览器宣告公网 IP，否则 ICE 建连失败
  if ! grep -Esq "^[[:space:]]*webrtcAdditionalHosts:.*${PUBLIC_IP//./\\.}" /etc/mediamtx/mediamtx.yml 2>/dev/null; then
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

  # 构建后必须校验运行时配置链路是否还在，否则会"构建成功但功能静默失效"：
  # public/qbot-config.js 要被 vite 拷进 dist/，index.html 要保留 script 标签，
  # bundle 要保留 __QBOT_CONFIG__ 读取逻辑。这三处任一丢失，
  # switch_config.sh/local|cloud 就完全不起作用了。
  local _dir="${APP_DIR}"
  [[ -f "$_dir/dist/qbot-config.js" ]] || die "构建后 dist/qbot-config.js 丢失（public/ 下的配置没被拷贝）"
  grep -q 'qbot-config\.js' "$_dir/dist/index.html" || die "构建后 dist/index.html 不再引用 qbot-config.js，运行时配置会失效"
  grep -rq '__QBOT_CONFIG__' "$_dir/dist/assets/" || die "构建后 bundle 不含 __QBOT_CONFIG__ 读取逻辑，运行时配置会失效"
  info "构建后已校验运行时配置链路完整"
fi

# 4) 看板页面
if [[ "$NO_DASH" -eq 0 ]]; then
  # 装了 systemd 单元（deploy/qbot-*.service）就交给 systemd，崩溃/重启自动恢复；
  # 没装则回退到下面的手工 setsid 启动。
  if unit_exists "qbot-dashboard.service"; then
    info "看板由 systemd 管理（qbot-dashboard.service）"
    sudo systemctl restart qbot-dashboard || die "qbot-dashboard 启动失败，见 journalctl -u qbot-dashboard"
    wait_port "$DASH_PORT" "看板" 25 || die "看板未在 ${DASH_PORT} 监听，见 journalctl -u qbot-dashboard"
  elif listening_on "${DASH_PORT}"; then
    info "看板已在 :${DASH_PORT} 运行"
  else
    info "启动看板 (${DASH_PORT})…"
    ( cd "$APP_DIR" || exit 1
      setsid nohup env PORT="$DASH_PORT" "$NODE_BIN" server.js >"$LOG_DASH" 2>&1 & ) || die "看板启动失败"
    wait_port "$DASH_PORT" "看板" 20 || die "看板未在 ${DASH_PORT} 监听，见 $LOG_DASH"
  fi
fi

# 5) 语音网关
if [[ "$NO_GW" -eq 0 ]]; then
  if unit_exists "qbot-gateway.service"; then
    info "语音网关由 systemd 管理（qbot-gateway.service）"
    # SenseVoice 模型加载要 1-2 秒，重启后给足时间再判定
    sudo systemctl restart qbot-gateway || die "qbot-gateway 启动失败，见 journalctl -u qbot-gateway"
    sleep 6
    if systemctl is-active --quiet qbot-gateway; then
      info "语音网关运行中（journalctl -u qbot-gateway 查看日志）"
    else
      warn "语音网关未运行，见 journalctl -u qbot-gateway"
    fi
  else
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
fi

sleep 2
if ! health_check; then
  warn "健康检查有未通过项，见上面的告警；服务可能未完全就绪"
fi

cat <<EOF

========================= 模式 2 云端已就绪 =========================
  看板页面 : http://${PUBLIC_IP}:${DASH_PORT}
  MQTT     : ${PUBLIC_IP}:${MQTT_PORT}   （浏览器 ws://${PUBLIC_IP}:${WS_PORT}/mqtt）
  视频     : rtsp://${PUBLIC_IP}:${RTSP_PORT}/cam/robot-01 → WHEP http://${PUBLIC_IP}:${WHEP_PORT}
  日志     :
    看板  ${LOG_DASH}
    网关  ${LOG_GW}

  下一步   : 在机器人侧主机上执行 ./mode2_robot_side.sh
  健康检查 : ./mode2_cloud_side.sh --check
  停止     : ./stop_all.sh cloud
=====================================================================
EOF