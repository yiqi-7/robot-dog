#!/usr/bin/env bash
# ===========================================================================
# 模式 1：本地全栈（TITAN_V 单机跑全部内容）
#
#   ROS2 仿真 + MQTT Broker + 语音网关 + 看板页面 全部在本机
#   用途：本地通读代码、单机自测、无外网也能跑
#
#   数据流：
#     gz-sim → ROS2 节点 → mqtt_bridge/task_runner/voice_link ─┐
#                                                              ├→ mosquitto(127.0.0.1:1883)
#     image_sender(JPEG兜底帧) ────────────────────────────────┘        │
#                                                                       ├→ 看板(8080) 浏览器 ws://127.0.0.1:8083
#     voice-gateway ← 语音上行 ─────────────────────────────────────────┘   ASR/TTS 均在本机
#
#   注意：本机未装 mediamtx，视频主通道（RTSP→WebRTC）不可用，
#         看板会自动降级到 MQTT-JPEG 兜底帧（1 fps 快照，功能可用）
# ===========================================================================
set -euo pipefail

# ---------------------------- 路径与端口配置 ----------------------------
WS="/run/media/wby/shuju/Document/robot-dog"          # ROS2 工作区
OPS="${WS}/quadruped-ops"                             # 看板/网关（含 node_modules 与模型）
MAP="${WS}/maps/factory_full.yaml"                    # 预建地图
LOG_ROS="/tmp/qbot_mode1_ros.log"                     # ROS2 日志
LOG_DASH="/tmp/qbot_mode1_dash.log"                   # 看板日志
LOG_GW="/tmp/qbot_mode1_gw.log"                       # 语音网关日志
BROKER_HOST="127.0.0.1"                               # 本机 broker
MQTT_PORT=1883
WS_PORT=8083
DASH_PORT=8080
NODE_BIN="$(command -v node || true)"

usage() {
  cat <<'EOF'
用法: ./mode1_local_all.sh [选项]

本地全栈模式：在 TITAN_V 上启动 ROS2 + MQTT + 语音网关 + 看板。

选项:
  -h, --help      显示本帮助
  --no-ros        只起云端服务（broker/网关/看板），不起 ROS2 仿真
  --no-dash       不起看板页面
  --no-gw         不起语音网关
  --build         启动前重新构建看板前端（npm run build）
  --wait SEC      启动后等待多少秒再自检（默认 115）

日志:
  ROS2   /tmp/qbot_mode1_ros.log
  看板   /tmp/qbot_mode1_dash.log
  网关   /tmp/qbot_mode1_gw.log
EOF
}

NO_ROS=0; NO_DASH=0; NO_GW=0; DO_BUILD=0; WAIT_SEC=115
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --no-ros)  NO_ROS=1; shift ;;
    --no-dash) NO_DASH=1; shift ;;
    --no-gw)   NO_GW=1; shift ;;
    --build)   DO_BUILD=1; shift ;;
    --wait)    WAIT_SEC="$2"; shift 2 ;;
    *) echo "未知参数: $1（-h 查看帮助）"; exit 1 ;;
  esac
done

# ---------------------------- 工具函数 ----------------------------
info() { printf '\033[36m[mode1]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[mode1]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[mode1]\033[0m %s\n' "$*" >&2; exit 1; }

# 停止旧的 ROS2 相关进程。模式串都写成 xxx[x] 形式，避免 pkill 匹配到脚本自身
stop_ros() {
  info "清理旧 ROS2 进程…"
  local pats=(
    "gz-sim-mai[n]" "ros2 launch quadruped_si[m]" "controller_serve[r]" "planner_serve[r]"
    "bt_navigato[r]" "behavior_serve[r]" "map_serve[r]" "amc[l]" "lifecycle_manage[r]"
    "collision_monito[r]" "smoother_serve[r]" "parameter_bridg[e]" "ros_gz_bridg[e]"
    "voice_sourc[e]" "champ_joint_rela[y]" "quadruped_controlle[r]" "patrol_nod[e]"
    "image_sende[r]" "mqtt_bridg[e]" "task_runne[r]" "voice_lin[k]" "deadman_guar[d]"
    "gait_controlle[r]" "odom_pu[b]" "camera_info_pu[b]"
  )
  local p
  for p in "${pats[@]}"; do pkill -9 -f "$p" 2>/dev/null || true; done
  sleep 4
}

# 把 launch 里 image_sender 的 rtsp_server 临时改成本机（退出时由 sed 恢复原值）
# 说明：该参数目前写死在 launch 文件里，切模式必须改文件
patch_rtsp() {
  local launch="$1" target="$2"
  local bak="${launch}.rtsp.bak"
  [[ -f "$bak" ]] || cp "$launch" "$bak"
  # 只替换 image_sender 段落里的 rtsp_server 行
  sed -i "s|^\(\s*\)\"rtsp_server\": \"[^\"]*\"|\1\"rtsp_server\": \"${target}\"|" "$launch"
  info "image_sender 推流目标 → ${target}"
}

restore_rtsp() {
  local launch="$1"
  local bak="${launch}.rtsp.bak"
  if [[ -f "$bak" ]]; then
    mv "$bak" "$launch"
    info "已恢复 launch 文件的 rtsp_server 原值"
  fi
}

# 等待端口就绪
wait_port() {
  local port="$1" name="$2" tries="${3:-30}"
  local i
  for ((i=1; i<=tries; i++)); do
    if ss -lnt 2>/dev/null | grep -q ":${port} "; then
      info "${name} 已监听 :${port}"
      return 0
    fi
    sleep 1
  done
  warn "${name} 未监听 :${port}（继续，后续自检会报）"
  return 1
}

# ---------------------------- 前置检查 ----------------------------
info "=== 模式 1：本地全栈 ==="
[[ -d "$WS" ]]  || die "工作区不存在: $WS"
[[ -d "$OPS" ]] || die "看板目录不存在: $OPS"
[[ -n "$NODE_BIN" ]] || die "未找到 node，请先安装 Node.js"

# 1) MQTT Broker
info "检查 mosquitto…"
if ! systemctl is-active --quiet mosquitto; then
  info "启动 mosquitto…"
  sudo systemctl start mosquitto || die "mosquitto 启动失败"
fi
wait_port "$MQTT_PORT" "MQTT" || die "MQTT 未就绪，模式 1 无法继续"
wait_port "$WS_PORT" "MQTT-WebSocket" || true

# 2) 视频主通道检测（本机没有 mediamtx 时降级）
if [[ -x /usr/local/bin/mediamtx ]]; then
  if ! systemctl is-active --quiet mediamtx 2>/dev/null; then
    sudo systemctl start mediamtx 2>/dev/null || true
  fi
  info "MediaMTX 可用，视频走 WebRTC 主通道"
else
  warn "未安装 mediamtx（/usr/local/bin/mediamtx 不存在）"
  warn "  → 视频主通道不可用，看板将显示 MQTT-JPEG 兜底帧（1 fps 快照）"
  warn "  → 需要主通道时：把云端 /usr/local/bin/mediamtx 与 /etc/mediamtx/mediamtx.yml 拷过来"
fi

# 3) 看板前端构建（可选）
if [[ "$DO_BUILD" -eq 1 ]]; then
  info "重新构建看板前端…"
  ( cd "$OPS" && npm run build >/tmp/qbot_mode1_build.log 2>&1 ) || die "构建失败，见 /tmp/qbot_mode1_build.log"
fi

# ---------------------------- 启动看板 ----------------------------
if [[ "$NO_DASH" -eq 0 ]]; then
  if ss -lnt 2>/dev/null | grep -q ":${DASH_PORT} "; then
    info "看板已在 :${DASH_PORT} 运行，跳过"
  else
    info "启动看板页面 (${DASH_PORT})…"
    ( cd "$OPS" && setsid nohup env PORT="$DASH_PORT" "$NODE_BIN" server.js >"$LOG_DASH" 2>&1 & )
    wait_port "$DASH_PORT" "看板" 20 || true
  fi
fi

# ---------------------------- 启动语音网关 ----------------------------
if [[ "$NO_GW" -eq 0 ]]; then
  if pgrep -f "voice-gateway\.j[s]" >/dev/null 2>&1; then
    info "语音网关已在运行，先停止旧实例"
    pkill -f "voice-gateway\.j[s]" 2>/dev/null || true
    sleep 2
  fi
  info "启动语音网关（MQTT=${BROKER_HOST}）…"
  ( cd "$OPS" && setsid nohup env \
      MQTT_URL="mqtt://${BROKER_HOST}:${MQTT_PORT}" \
      TOPIC_PREFIX="qbot/v1" \
      "$NODE_BIN" voice-gateway.js >"$LOG_GW" 2>&1 & )
  sleep 5
  grep -q "已连接" "$LOG_GW" 2>/dev/null && info "语音网关已连上 MQTT" || warn "语音网关尚未连上，见 $LOG_GW"
fi

# ---------------------------- 启动 ROS2 ----------------------------
if [[ "$NO_ROS" -eq 0 ]]; then
  LAUNCH="$WS/src/quadruped_sim/launch/quadruped_gz.launch.py"
  [[ -f "$LAUNCH" ]] || die "launch 文件不存在: $LAUNCH"

  stop_ros
  trap 'restore_rtsp "$LAUNCH"' EXIT
  patch_rtsp "$LAUNCH" "127.0.0.1"

  info "启动 ROS2 仿真（broker_host=${BROKER_HOST}）…"
  ( cd "$WS" && setsid nohup bash -c "
      source /opt/ros/lyrical/setup.bash
      source install/setup.bash
      export FASTRTPS_DEFAULT_PROFILES_FILE=/home/wby/fastdds_udp.xml
      exec ros2 launch quadruped_sim quadruped_gz.launch.py \
        gui:=false rviz:=false nav:=true gait:=champ teleop:=false patrol:=false \
        map:=${MAP} \
        broker_host:=${BROKER_HOST} broker_port:=${MQTT_PORT}
    " >"$LOG_ROS" 2>&1 & )

  info "等待 ${WAIT_SEC} 秒让 Gazebo/Nav2 初始化…"
  sleep "$WAIT_SEC"

  # ---------------------------- 自检 ----------------------------
  info "=== 自检 ==="
  local_ok=1
  if grep -aq "MQTT 已连接" "$LOG_ROS"; then
    info "MQTT 节点连接:"
    grep -a "MQTT 已连接" "$LOG_ROS" | sed 's/^/    /' | tail -4
  else
    warn "未见 MQTT 节点连接日志，见 $LOG_ROS"
    local_ok=0
  fi

  if grep -aq "image_sender 就绪" "$LOG_ROS"; then
    grep -a "image_sender 就绪" "$LOG_ROS" | tail -1 | sed 's/^/    /'
  else
    warn "image_sender 未就绪"
  fi

  # 兜底帧是否真的在流动
  if command -v mosquitto_sub >/dev/null 2>&1; then
    if timeout 8 mosquitto_sub -h "$BROKER_HOST" -p "$MQTT_PORT" \
         -t "qbot/v1/robots/robot-01/camera" -C 1 -W 6 >/dev/null 2>&1; then
      info "摄像头兜底帧正常（MQTT-JPEG）"
    else
      warn "8 秒内未收到摄像头帧（可能仍在初始化）"
    fi
  fi

  [[ "$local_ok" -eq 1 ]] && info "模式 1 启动完成" || warn "模式 1 启动完成但自检有告警"
fi

cat <<EOF

============================ 模式 1 已就绪 ============================
  看板页面 : http://127.0.0.1:${DASH_PORT}   （局域网 http://<本机IP>:${DASH_PORT}）
  MQTT     : ${BROKER_HOST}:${MQTT_PORT}   看板走 ws://${BROKER_HOST}:${WS_PORT}/mqtt
  视频     : $( [[ -x /usr/local/bin/mediamtx ]] && echo "WebRTC 主通道 (8889)" || echo "MQTT-JPEG 兜底帧（无 mediamtx）" )
  日志     :
    ROS2  ${LOG_ROS}
    看板  ${LOG_DASH}
    网关  ${LOG_GW}

  停止     : ./stop_all.sh local
=====================================================================
EOF