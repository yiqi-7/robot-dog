#!/usr/bin/env bash
# ===========================================================================
# 停止脚本：按模式停止对应服务
#   local : 停 ROS2 + 语音网关 + 看板（本机全部）
#   cloud : 停 语音网关 + 看板（云端；broker/mediamtx 是 systemd 服务，不动）
#   robot : 只停 ROS2
#   all   : 以上全部
# ===========================================================================
set -euo pipefail

WS="/run/media/wby/shuju/Document/robot-dog"
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

MODE="${1:-all}"
WITH_INFRA=0
for a in "$@"; do
  case "$a" in
    -h|--help) usage; exit 0 ;;
    --with-infra) WITH_INFRA=1 ;;
  esac
done

info() { printf '\033[36m[stop]\033[0m %s\n' "$*"; }

stop_ros() {
  info "停止 ROS2 相关进程…"
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
  # ffmpeg 推流子进程
  pkill -9 -f "ffmpeg.*8554" 2>/dev/null || true
  sleep 3
  info "ROS2 已停止"
}

stop_apps() {
  info "停止语音网关与看板…"
  pkill -f "voice-gateway\.j[s]" 2>/dev/null || true
  pkill -f "node.*server\.j[s]" 2>/dev/null || true
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