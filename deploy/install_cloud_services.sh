#!/usr/bin/env bash
# ============================================================================
# 在云端安装 qbot 的 systemd 单元（看板 + 语音网关），并设为开机自启。
#
# 为什么要这个脚本：
#   之前看板和语音网关是用 setsid nohup 手工起的，云主机一重启就没了，
#   PPID=1 的孤儿进程也不好管。改成 systemd 后崩溃会自动拉起、重启自动恢复。
#
# 用法（在云端执行）：
#   ./install_cloud_services.sh            # 安装 + 启动 + 设自启
#   ./install_cloud_services.sh --check    # 只看状态
#   ./install_cloud_services.sh --uninstall
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${APP_DIR:-/opt/qbot/dashboard}"
NODE_BIN="${NODE_BIN:-/opt/qbot/node/bin/node}"
UNITS=(qbot-dashboard qbot-gateway)
ACTION="install"

info() { printf '\033[36m[svc]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[svc]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[svc]\033[0m %s\n' "$*" >&2; exit 1; }

LISTENING_SVC=""
for a in "$@"; do
  case "$a" in
    -h|--help) sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --check) ACTION="check" ;;
    --uninstall) ACTION="uninstall" ;;
    *) die "未知参数: $a" ;;
  esac
done

unit_exists() {  # 避免 cmd | grep -q 在 pipefail 下的 SIGPIPE 假失败
  local out
  out="$(systemctl list-unit-files 2>/dev/null || true)"
  grep -q "^$1" <<<"$out"
}

case "$ACTION" in
  check)
    for u in "${UNITS[@]}"; do
      if unit_exists "${u}.service"; then
        printf '  %-16s %s / %s\n' "$u" "$(systemctl is-active $u 2>/dev/null)" "$(systemctl is-enabled $u 2>/dev/null)"
      else
        printf '  %-16s 未安装\n' "$u"
      fi
    done
    exit 0
    ;;

  uninstall)
    for u in "${UNITS[@]}"; do
      if unit_exists "${u}.service"; then
        sudo systemctl disable --now "$u" 2>/dev/null || true
        sudo rm -f "/etc/systemd/system/${u}.service"
        info "已卸载 $u"
      fi
    done
    sudo systemctl daemon-reload
    exit 0
    ;;

  install)
    [[ -x "$NODE_BIN" ]] || die "找不到 node：$NODE_BIN（可用 NODE_BIN=... 覆盖）"
    [[ -f "$APP_DIR/server.js" ]] || die "$APP_DIR 缺少 server.js，先用 push_cloud_app.sh 同步"
    [[ -f "$APP_DIR/voice-gateway.js" ]] || die "$APP_DIR 缺少 voice-gateway.js"
    [[ -d "$APP_DIR/node_modules" ]] || warn "警告：$APP_DIR 缺 node_modules，服务可能起不来"
    [[ -f "$APP_DIR/dist/index.html" ]] || warn "警告：$APP_DIR/dist 未构建，看板会 404"

    info "安装 systemd 单元…"
    for u in "${UNITS[@]}"; do
      src="$HERE/${u}.service"
      [[ -f "$src" ]] || die "缺少单元文件 $src"
      # 单元文件里写的是默认路径，这里按实际环境替换，避免路径不一致
      sed -e "s#/opt/qbot/node/bin/node#${NODE_BIN}#g" \
          -e "s#/opt/qbot/dashboard#${APP_DIR}#g" \
          "$src" | sudo tee "/etc/systemd/system/${u}.service" >/dev/null
      sudo chmod 644 "/etc/systemd/system/${u}.service"
    done
    sudo systemctl daemon-reload
    sudo systemctl enable --now "${UNITS[@]}"
    sleep 10

    bad=0
    for u in "${UNITS[@]}"; do
      st="$(systemctl is-active $u 2>/dev/null || true)"
      printf '  %-16s %s\n' "$u" "$st"
      [[ "$st" == "active" ]] || bad=1
    done
    if [[ "$bad" -eq 1 ]]; then
      warn "有服务未起来，查看：journalctl -u qbot-gateway -n 30 --no-pager"
      exit 1
    fi
    info "完成。日志：journalctl -u qbot-dashboard / -u qbot-gateway"
    ;;
esac
