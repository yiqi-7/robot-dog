#!/usr/bin/env bash
# ===========================================================================
# 切换看板运行时配置（qbot-config.js）
#
#   看板前端由 server.js 从 dist/ 提供，运行时配置读取 dist/qbot-config.js，
#   所以切模式必须改 dist/ 下这一份（改 public/ 不会生效，除非重新 build）。
#
#   用法：
#     ./switch_config.sh local     # 指向本机 127.0.0.1
#     ./switch_config.sh cloud     # 指向云端 43.172.91.67
#     ./switch_config.sh show      # 查看当前生效配置
#     ./switch_config.sh restore   # 从备份恢复
# ===========================================================================
set -euo pipefail

# 路径允许环境变量覆盖：云端没有 /run/media/... 这个盘，
# 直接写死会让云端执行时 mkdir 失败、set -e 退出
WS="${WS:-/run/media/wby/shuju/Document/robot-dog}"
OPS="${OPS:-${WS}/quadruped-ops}"
CLOUD_HOST="${CLOUD_HOST:-43.172.91.67}"
MQTT_PORT="${MQTT_PORT:-1883}"
WS_PORT="${WS_PORT:-8083}"
WHEP_PORT="${WHEP_PORT:-8889}"
DASH_PORT="${DASH_PORT:-8080}"

# 运行时配置可能在这两个位置，都要同步
TARGETS=("${OPS}/dist/qbot-config.js" "${OPS}/public/qbot-config.js")

usage() {
  cat <<'EOF'
用法: ./switch_config.sh <local|cloud|show|restore>

  local    看板连本机（ws://127.0.0.1:8083，WHEP http://127.0.0.1:8889，无 STUN）
  cloud    看板连云端（ws://43.172.91.67:8083，WHEP http://43.172.91.67:8889，带 STUN）
  show     打印当前生效配置
  restore  从 .bak 备份恢复

环境变量: CLOUD_HOST MQTT_PORT WS_PORT WHEP_PORT DASH_PORT
EOF
}

info() { printf '\033[36m[config]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[config]\033[0m %s\n' "$*" >&2; exit 1; }

gen_local() {
  cat <<EOF
// 看板运行时配置 —— 由 switch_config.sh 生成（local 模式）
//
// 结构必须与 src/config.ts 的 RuntimeOverride 一致：mqtt.* 与 streaming.* 两层嵌套。
// 写成平铺（{ mode, brokerUrl }）不会被读取，会静默回退到「页面主机名」默认值。
// 本文件由 index.html 以普通 <script> 在模块包之前加载，无需重新构建即可生效。
window.__QBOT_CONFIG__ = {
  mqtt: {
    mode: "real",
    brokerUrl: "ws://127.0.0.1:${WS_PORT}/mqtt",
    username: "",
    password: "",
    topicPrefix: "qbot/v1",
    reconnectPeriod: 3000,
  },
  streaming: {
    whepBase: "http://127.0.0.1:${WHEP_PORT}",
    // 本机直连，无需 STUN
    iceServers: [],
  },
};
EOF
}

gen_cloud() {
  cat <<EOF
// 看板运行时配置 —— 由 switch_config.sh 生成（cloud 模式）
//
// 结构必须与 src/config.ts 的 RuntimeOverride 一致：mqtt.* 与 streaming.* 两层嵌套。
window.__QBOT_CONFIG__ = {
  mqtt: {
    mode: "real",
    brokerUrl: "ws://${CLOUD_HOST}:${WS_PORT}/mqtt",
    username: "",
    password: "",
    topicPrefix: "qbot/v1",
    reconnectPeriod: 3000,
  },
  streaming: {
    whepBase: "http://${CLOUD_HOST}:${WHEP_PORT}",
    // 公网/NAT 场景加 STUN，提升 ICE 建连成功率
    iceServers: ["stun:stun.l.google.com:19302"],
  },
};
EOF
}

MODE="${1:-show}"

case "$MODE" in
  -h|--help) usage; exit 0 ;;
  local|cloud)
    for t in "${TARGETS[@]}"; do
      mkdir -p "$(dirname "$t")"
      # 只在首次切换时留底（.orig.bak），之后切换不覆盖它，
      # 这样 restore 总能回到出厂/上一版本，而不是"上一次切换的结果"
      [[ -f "${t}.orig.bak" ]] || [[ ! -f "$t" ]] || cp "$t" "${t}.orig.bak"
      if [[ "$MODE" == "local" ]]; then gen_local > "$t"; else gen_cloud > "$t"; fi
      info "已写入 $MODE 配置: $t"
    done
    info "看板 server.js 每次请求都重读文件，刷新浏览器即可生效（无需重启）"
    ;;
  show)
    for t in "${TARGETS[@]}"; do
      [[ -f "$t" ]] || { info "$t 不存在"; continue; }
      info "=== $t ==="
      grep -E "brokerUrl|whepBase|iceServers|mode:|mqtt:|streaming:" "$t" | sed 's/^/    /'
    done
    ;;
  restore)
    for t in "${TARGETS[@]}"; do
      if [[ -f "${t}.orig.bak" ]]; then
        cp "${t}.orig.bak" "$t"     # 用 cp 而非 mv，保留快照可反复恢复
        info "已恢复 $t"
      else
        info "无原始备份: $t"
      fi
    done
    ;;
  *) usage; exit 1 ;;
esac