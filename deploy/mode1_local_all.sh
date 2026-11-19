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
# 工作区路径按脚本所在位置推导（脚本在 <工作区>/deploy/ 下），
# 这样在任意目录 clone 都能直接用，无需改脚本；需要时可用 WS=... 覆盖。
WS="${WS:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OPS="${WS}/quadruped-ops"                             # 看板/网关（含 node_modules 与模型）
MAP="${WS}/maps/factory_full.yaml"                    # 预建地图
LOG_ROS="/tmp/qbot_mode1_ros.log"                     # ROS2 日志
LOG_DASH="/tmp/qbot_mode1_dash.log"                   # 看板日志
LOG_GW="/tmp/qbot_mode1_gw.log"                       # 语音网关日志
BROKER_HOST="127.0.0.1"                               # 本机 broker
MQTT_PORT=1883
WS_PORT=8083
DASH_PORT=8080
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
# ---- pipefail 安全的条件判断 ----
# set -o pipefail 下 `cmd | grep -q` 会因 grep 命中后立即退出、左侧命令继续写管道
# 而收到 SIGPIPE，整条管道返回 141，导致 if 条件假失败（同一条件时而命中时而不命中）。
# 统一先把输出收进变量再匹配，彻底避开这个坑。
listening_on() {          # listening_on <port>：本机是否在监听该 TCP 端口
  local out
  out="$(ss -lnt 2>/dev/null || true)"
  grep -q ":$1 " <<<"$out"
}

info() { printf '\033[36m[mode1]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[mode1]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[mode1]\033[0m %s\n' "$*" >&2; exit 1; }

# 停止旧的 ROS2 相关进程。模式串都写成 xxx[x] 形式，避免 pkill 匹配到脚本自身
stop_ros() {
  info "清理旧 ROS2 进程…"
  # 先礼后兵：给 image_sender 一个 SIGTERM，让它有机会走析构里的 pclose() 收掉
  # ffmpeg 子进程。直接 -9 会让 popen 出来的 `sh -c ffmpeg` 变成孤儿进程，
  # 反复执行脚本就会堆积多组孤儿，抢同一个 RTSP path。
  pkill -15 -f "image_sende[r]" 2>/dev/null || true
  sleep 2
  local pats=(
    "gz-sim-mai[n]" "ros2 launch quadruped_si[m]" "controller_serve[r]" "planner_serve[r]"
    "bt_navigato[r]" "behavior_serve[r]" "map_serve[r]" "amc[l]" "lifecycle_manage[r]"
    "collision_monito[r]" "smoother_serve[r]" "parameter_bridg[e]" "ros_gz_bridg[e]"
    "voice_sourc[e]" "champ_joint_rela[y]" "quadruped_controlle[r]" "patrol_nod[e]"
    "image_sende[r]" "mqtt_bridg[e]" "task_runne[r]" "voice_lin[k]" "deadman_guar[d]"
    "odom_pu[b]" "camera_info_pu[b]"
    "gz-sim-gu[i]-client" "gz si[m]"
  )
  local p
  for p in "${pats[@]}"; do pkill -9 -f "$p" 2>/dev/null || true; done
  # 清理 image_sender 拉起的 ffmpeg 推流进程。模式串不写端口，
  # 因为 rtsp 端口/地址会随模式变化，写死端口会漏杀。
  pkill -9 -f "ffmpeg .*rawvideo.*-f rtsp" 2>/dev/null || true
  pkill -9 -f "sh -c -- ffmpeg .*rawvideo" 2>/dev/null || true
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
  local line
  for ((i=1; i<=tries; i++)); do
    # 先收进变量再匹配：pipefail 下 `ss | grep -q` 会因 SIGPIPE 让整条管道返回非零
    line="$(ss -lnt 2>/dev/null || true)"
    if grep -q ":${port} " <<<"$line"; then
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
[[ -f "$WS/install/setup.bash" ]] || die "工作区未构建：$WS/install/setup.bash 不存在，先 colcon build"
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

  # 构建后必须校验运行时配置链路是否还在，否则会"构建成功但功能静默失效"：
  # public/qbot-config.js 要被 vite 拷进 dist/，index.html 要保留 script 标签，
  # bundle 要保留 __QBOT_CONFIG__ 读取逻辑。这三处任一丢失，
  # switch_config.sh/local|cloud 就完全不起作用了。
  local _dir="${OPS:-$OPS}"
  [[ -f "$_dir/dist/qbot-config.js" ]] || die "构建后 dist/qbot-config.js 丢失（public/ 下的配置没被拷贝）"
  grep -q 'qbot-config\.js' "$_dir/dist/index.html" || die "构建后 dist/index.html 不再引用 qbot-config.js，运行时配置会失效"
  grep -rq '__QBOT_CONFIG__' "$_dir/dist/assets/" || die "构建后 bundle 不含 __QBOT_CONFIG__ 读取逻辑，运行时配置会失效"
  info "构建后已校验运行时配置链路完整"
fi

# ---------------------------- 启动看板 ----------------------------
if [[ "$NO_DASH" -eq 0 ]]; then
  if listening_on "${DASH_PORT}"; then
    info "看板已在 :${DASH_PORT} 运行，跳过"
  else
    info "启动看板页面 (${DASH_PORT})…"
    ( cd "$OPS" || exit 1
      setsid nohup env PORT="$DASH_PORT" "$NODE_BIN" server.js >"$LOG_DASH" 2>&1 & ) || die "看板启动失败"
    wait_port "$DASH_PORT" "看板" 20 || die "看板未在 ${DASH_PORT} 监听，见 $LOG_DASH"
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
  # 本机没有 mediamtx 时的正确做法是**关掉 RTSP 推流**，而不是"保持原值"：
  # 保持原值 = 按 launch 里的 43.172.91.67 把画面推到公网云端，与模式 1
  # "本地全栈、不出网"的语义相反（实测确实推上去了）；
  # 指向 127.0.0.1 又会让 ffmpeg 空转重连。image_sender 已有 rtsp_enable
  # 参数开关，且 MQTT 兜底帧不再依赖 ffmpeg 存活。
  trap 'restore_rtsp "$LAUNCH"' EXIT INT TERM HUP
  RTSP_ARG=""
  if [[ -x /usr/local/bin/mediamtx ]]; then
    patch_rtsp "$LAUNCH" "127.0.0.1"
  else
    RTSP_ARG="rtsp_enable:=false"
    info "本机无 mediamtx → 关闭 RTSP 推流（rtsp_enable:=false），只发 MQTT 兜底帧"
  fi

  info "启动 ROS2 仿真（broker_host=${BROKER_HOST}）…"
  ( cd "$WS" && setsid nohup bash -c "
      source /opt/ros/lyrical/setup.bash
      source install/setup.bash
      export FASTRTPS_DEFAULT_PROFILES_FILE=/home/wby/fastdds_udp.xml
      exec ros2 launch quadruped_sim quadruped_gz.launch.py \
        gui:=false rviz:=false nav:=true gait:=champ teleop:=false patrol:=false \
        map:=${MAP} \
        broker_host:=${BROKER_HOST} broker_port:=${MQTT_PORT} \
        ${RTSP_ARG}
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

  if [[ "$local_ok" -eq 1 ]]; then
    info "模式 1 启动完成"
  else
    warn "模式 1 启动完成但自检未通过（见 $LOG_ROS）"
    exit 2
  fi
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