#!/usr/bin/env bash
# ===========================================================================
# 模式 2 · 机器人一侧（在 TITAN_V 上执行）
#
#   只跑 ROS2 仿真与四个 MQTT 节点，所有数据推到云端 43.172.91.67：
#     遥测/任务/语音上行 → MQTT 1883
#     视频               → RTSP 8554（image_sender 的 ffmpeg 推流）
#
#   云端服务由 mode2_cloud_side.sh 在服务器上启动
# ===========================================================================
set -euo pipefail

# ---------------------------- 配置 ----------------------------
# 工作区路径按脚本所在位置推导（脚本在 <工作区>/deploy/ 下），
# 这样在任意目录 clone 都能直接用，无需改脚本；需要时可用 WS=... 覆盖。
WS="${WS:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
MAP="${WS}/maps/factory_full.yaml"
LAUNCH="${WS}/src/quadruped_sim/launch/quadruped_gz.launch.py"
CLOUD_HOST="${CLOUD_HOST:-43.172.91.67}"      # 腾讯云中转服务器
MQTT_PORT="${MQTT_PORT:-1883}"
RTSP_PORT="${RTSP_PORT:-8554}"
ROBOT_ID="${ROBOT_ID:-robot-01}"
LOG_ROS="/tmp/qbot_mode2_ros.log"

usage() {
  cat <<'EOF'
用法: ./mode2_robot_side.sh [选项]

机器人一侧：启动 ROS2 仿真 + MQTT 上报 + RTSP 推流，全部指向云端。

选项:
  -h, --help        显示本帮助
  --host HOST       云端地址（默认 43.172.91.67）
  --no-ros          只切换配置，不启动 ROS2
  --wait SEC        启动后等待秒数再自检（默认 115）
  --keep-rtsp       不修改 launch 的 rtsp_server（已手工改好时用）

环境变量: CLOUD_HOST MQTT_PORT RTSP_PORT ROBOT_ID

日志: /tmp/qbot_mode2_ros.log
EOF
}

NO_ROS=0; WAIT_SEC=115; KEEP_RTSP=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --host) CLOUD_HOST="$2"; shift 2 ;;
    --no-ros) NO_ROS=1; shift ;;
    --wait) WAIT_SEC="$2"; shift 2 ;;
    --keep-rtsp) KEEP_RTSP=1; shift ;;
    *) echo "未知参数: $1（-h 查看帮助）"; exit 1 ;;
  esac
done

info() { printf '\033[36m[robot]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[robot]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[robot]\033[0m %s\n' "$*" >&2; exit 1; }

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

[[ -d "$WS" ]] || die "工作区不存在: $WS"
[[ -f "$LAUNCH" ]] || die "launch 文件不存在: $LAUNCH"

info "=== 模式 2：机器人一侧（云端 ${CLOUD_HOST}）==="

# ---------------------------- 连通性预检 ----------------------------
info "预检云端连通性…"
if timeout 5 bash -c "cat < /dev/null > /dev/tcp/${CLOUD_HOST}/${MQTT_PORT}" 2>/dev/null; then
  info "MQTT ${CLOUD_HOST}:${MQTT_PORT} 可达"
else
  warn "MQTT ${CLOUD_HOST}:${MQTT_PORT} 不可达 —— 云端服务可能未启动或安全组未放行"
fi
if timeout 5 bash -c "cat < /dev/null > /dev/tcp/${CLOUD_HOST}/${RTSP_PORT}" 2>/dev/null; then
  info "RTSP ${CLOUD_HOST}:${RTSP_PORT} 可达（视频推流可用）"
else
  warn "RTSP ${CLOUD_HOST}:${RTSP_PORT} 不可达 —— 视频推流将失败，看板只能用 MQTT-JPEG 兜底帧"
fi

# ---------------------------- 推流目标 ----------------------------
if [[ "$KEEP_RTSP" -eq 0 ]]; then
  cp -n "$LAUNCH" "${LAUNCH}.rtsp.bak" 2>/dev/null || true
  # rtsp_server 与 rtsp_port 目前都写死在 launch 里，两个都要改，否则端口变了提示会骗人
  sed -i "s|^\(\s*\)\"rtsp_server\": \"[^\"]*\"|\1\"rtsp_server\": \"${CLOUD_HOST}\"|" "$LAUNCH"
  sed -i "s|^\(\s*\)\"rtsp_port\": [0-9]*|\1\"rtsp_port\": ${RTSP_PORT}|" "$LAUNCH"
  # sed 不匹配时退出码仍是 0，必须回读校验
  grep -q "\"rtsp_server\": \"${CLOUD_HOST}\"" "$LAUNCH" || die "sed 未能修改 rtsp_server，请检查 launch 文件格式"
  info "image_sender 推流目标 → rtsp://${CLOUD_HOST}:${RTSP_PORT}/cam/${ROBOT_ID}"
fi

[[ "$NO_ROS" -eq 1 ]] && { info "--no-ros 指定，已切换配置但不起 ROS2，退出"; exit 0; }

# ---------------------------- 启动 ----------------------------
stop_ros
info "启动 ROS2（broker_host=${CLOUD_HOST}）…"
( cd "$WS" && setsid nohup bash -c "
    source /opt/ros/lyrical/setup.bash
    source install/setup.bash
    export FASTRTPS_DEFAULT_PROFILES_FILE=/home/wby/fastdds_udp.xml
    exec ros2 launch quadruped_sim quadruped_gz.launch.py \
      gui:=false rviz:=false nav:=true gait:=champ teleop:=false patrol:=false \
      map:=${MAP} \
      broker_host:=${CLOUD_HOST} broker_port:=${MQTT_PORT}
  " >"$LOG_ROS" 2>&1 & )

info "等待 ${WAIT_SEC} 秒初始化…"
sleep "$WAIT_SEC"

# ---------------------------- 自检 ----------------------------
info "=== 自检 ==="
if grep -aq "MQTT 已连接" "$LOG_ROS"; then
  info "MQTT 节点连接:"
  grep -a "MQTT 已连接" "$LOG_ROS" | sed 's/^/    /' | tail -4
else
  warn "未见 MQTT 连接日志，见 $LOG_ROS"
fi
if grep -aq "image_sender 就绪" "$LOG_ROS"; then
  grep -a "image_sender 就绪" "$LOG_ROS" | tail -1 | sed 's/^/    /'
fi
if pgrep -af "ffmpeg.*${RTSP_PORT}" >/dev/null 2>&1; then
  info "ffmpeg 推流进程存活"
else
  warn "未见 ffmpeg 推流进程"
fi

cat <<EOF

======================== 模式 2 机器人侧已就绪 =======================
  云端看板 : http://${CLOUD_HOST}:8080
  MQTT     : ${CLOUD_HOST}:${MQTT_PORT}
  视频推流 : rtsp://${CLOUD_HOST}:${RTSP_PORT}/cam/${ROBOT_ID}
  日志     : ${LOG_ROS}

  云端自检 : 在服务器上执行 ./mode2_cloud_side.sh --check
  停止     : ./stop_all.sh robot
=====================================================================
EOF