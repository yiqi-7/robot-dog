#!/usr/bin/env bash
# ===========================================================================
# 停止脚本：按模式停止对应服务
#   local : 停 ROS2 + 语音网关 + 看板（本机全部）
#   cloud : 停 语音网关 + 看板（云端；broker/mediamtx 是 systemd 服务，不动）
#   robot : 只停 ROS2
#   all   : 以上全部
# ===========================================================================
set -euo pipefail

# 工作区路径按脚本所在位置推导（脚本在 <工作区>/deploy/ 下），
# 这样在任意目录 clone 都能直接用，无需改脚本；需要时可用 WS=... 覆盖。
WS="${WS:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
OPS="${WS}/quadruped-ops"
LAUNCH="${WS}/src/quadruped_sim/launch/quadruped_gz.launch.py"

usage() {
  cat <<'EOF'
用法: ./stop_all.sh [local|cloud|robot|all]

  local  停 ROS2 + 语音网关 + 看板（本机全栈模式）
  cloud  停 语音网关 + 看板（云端；mosquitto/mediamtx 是 systemd，不动）
  robot  只停 ROS2
  all    local + cloud 全停

选项:
  -h, --help  显示本帮助
  --with-infra 额外停止 mosquitto/mediamtx（systemd 服务，需 sudo）
EOF
}

MODE=""
WITH_INFRA=0
for a in "$@"; do
  case "$a" in
    -h|--help) usage; exit 0 ;;
    --with-infra) WITH_INFRA=1 ;;
    local|cloud|robot|all) MODE="$a" ;;
    *) echo "未知参数: $a（local|cloud|robot|all，或用 -h 查看帮助）"; exit 1 ;;
  esac
done
# 允许只写 --with-infra：此时默认按 all 处理
MODE="${MODE:-all}"

info() { printf '\033[36m[stop]\033[0m %s\n' "$*"; }

stop_ros() {
  info "停止 ROS2 相关进程…"
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
  # ffmpeg 推流子进程：模式串不写端口，端口/地址随模式变化，写死会漏杀
  pkill -15 -f "image_sende[r]" 2>/dev/null || true
  sleep 1
  pkill -9 -f "ffmpeg .*rawvideo.*-f rtsp" 2>/dev/null || true
  pkill -9 -f "sh -c -- ffmpeg .*rawvideo" 2>/dev/null || true
  sleep 3
  info "ROS2 已停止"
}

stop_apps() {
  info "停止语音网关与看板…"
  pkill -f "voice-gateway\.j[s]" 2>/dev/null || true
  # 只杀 serve 本项目 dist 的 server.js，避免误杀机器上别的 node 服务
  if [[ -d "$OPS" ]]; then
    local pid cwd
    for pid in $(pgrep -f "server\.j[s]" 2>/dev/null || true); do
      cwd="$(readlink -f "/proc/${pid}/cwd" 2>/dev/null || true)"
      if [[ "$cwd" == "$OPS" ]]; then kill -9 "$pid" 2>/dev/null || true; fi
    done
  fi
  sleep 2
  info "语音网关/看板已停止"
}

restore_launch() {
  # 若脚本切换模式时改过 launch 的 rtsp_server，这里恢复原值
  if [[ -f "${LAUNCH}.rtsp.bak" ]]; then
    mv "${LAUNCH}.rtsp.bak" "$LAUNCH"
    info "已恢复 launch 文件的 rtsp_server 原值"
  fi
}

case "$MODE" in
  local) stop_ros; stop_apps; restore_launch ;;
  cloud) stop_apps ;;
  robot) stop_ros; restore_launch ;;
  all)   stop_ros; stop_apps; restore_launch ;;
  *) echo "未知模式: $MODE（local|cloud|robot|all）"; exit 1 ;;
esac

if [[ "$WITH_INFRA" -eq 1 ]]; then
  info "停止基础设施（mosquitto / mediamtx）…"
  sudo systemctl stop mediamtx 2>/dev/null || true
  sudo systemctl stop mosquitto 2>/dev/null || true
  info "基础设施已停止"
fi

info "完成"