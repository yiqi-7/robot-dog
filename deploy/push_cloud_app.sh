#!/usr/bin/env bash
# ============================================================================
# 把仓库里的 dashboard/ 部署到云端（或任意远端），替换手工 scp 的散装拷贝。
#
# 为什么要这个脚本：
#   以前云端 /opt/qbot/dashboard 里的 server.js、voice-gateway.js、dist/ 都是
#   人工 scp 上去的，仓库改了云端不知道，反过来云端临时改的东西仓库也没有，
#   两边会静默漂移。本脚本只从 git 已跟踪文件打包，保证云端内容可追溯到某个 commit。
#
# 用法（在开发机/TITAN_V 上执行）：
#   ./deploy/push_cloud_app.sh                     # 推到默认云端 43.172.91.67
#   ./deploy/push_cloud_app.sh --host 1.2.3.4      # 换目标
#   ./deploy/push_cloud_app.sh --key ~/.ssh/x.pem  # 指定私钥
#   ./deploy/push_cloud_app.sh --dry-run           # 只看会传什么
#   ./deploy/push_cloud_app.sh --no-build          # 不重新构建前端（只同步源码）
#   ./deploy/push_cloud_app.sh --no-restart        # 同步但不重启服务
#
# 云端侧（在云端执行一次即可）：
#   ./deploy/install_cloud_services.sh             # 安装 systemd 单元
# ============================================================================
set -euo pipefail

WS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CLOUD_HOST="43.172.91.67"
CLOUD_USER="ubuntu"
SSH_KEY=""
APP_DIR="/opt/qbot/dashboard"
DO_BUILD=1
DO_RESTART=1
DRY_RUN=0

info() { printf '\033[36m[push]\033[0m %s\n' "$*"; }
warn() { printf '\033[33m[push]\033[0m %s\n' "$*"; }
die()  { printf '\033[31m[push]\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
  sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 0
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) usage ;;
    --host)    CLOUD_HOST="${2:?--host 需要一个地址}"; shift 2 ;;
    --user)    CLOUD_USER="${2:?--user 需要一个用户名}"; shift 2 ;;
    --key)     SSH_KEY="${2:?--key 需要私钥路径}"; shift 2 ;;
    --app-dir) APP_DIR="${2:?--app-dir 需要路径}"; shift 2 ;;
    --no-build)   DO_BUILD=0; shift ;;
    --no-restart) DO_RESTART=0; shift ;;
    --dry-run)    DRY_RUN=1; shift ;;
    *) die "未知参数: $1（-h 查看帮助）" ;;
  esac
done

command -v git >/dev/null || die "需要 git"
command -v tar >/dev/null || die "需要 tar"

SSH_OPTS=(-o StrictHostKeyChecking=no -o ConnectTimeout=20)
[[ -n "$SSH_KEY" ]] && SSH_OPTS+=(-i "$SSH_KEY")
SSH="ssh ${SSH_OPTS[*]}"
SCP="scp ${SSH_OPTS[*]}"

cd "$WS"
REV="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"

# 只打包 git 已跟踪的文件：node_modules / dist 由云端自己装和构建，
# 避免把本地 300MB 依赖推上去、也避免两边的 node_modules 不一致。
info "收集 git 已跟踪的 dashboard 文件（${BRANCH}@${REV}）…"
TARBALL="/tmp/qbot-dashboard-${REV}.tar.gz"
# 不要用 --prefix=dashboard/：已在 pathspec 里指定 dashboard/，再加前缀会变成
# dashboard/dashboard/...，解包后多一层目录，package.json 反而覆盖不到。
git archive --format=tar.gz -o "$TARBALL" HEAD dashboard/
SIZE="$(du -h "$TARBALL" | cut -f1)"
NFILES="$(git ls-tree -r --name-only HEAD dashboard/ | wc -l)"
info "打包完成：${NFILES} 个文件，${SIZE}（${TARBALL}）"

if [[ "$DRY_RUN" -eq 1 ]]; then
  info "==dry-run== 将要推送的内容（前 12 项）："
  git ls-tree -r --name-only HEAD dashboard/ | head -12 | sed 's/^/    /'
  info "==dry-run== 目标 ${CLOUD_USER}@${CLOUD_HOST}:${APP_DIR}"
  info "==dry-run== 不实际传输"
  exit 0
fi

info "测试连通性…"
$SSH "${CLOUD_USER}@${CLOUD_HOST}" "echo ok >/dev/null" || die "无法连接 ${CLOUD_USER}@${CLOUD_HOST}"

info "上传…"
$SCP "$TARBALL" "${CLOUD_USER}@${CLOUD_HOST}:/tmp/" || die "上传失败"

REMOTE_SCRIPT=$(cat <<REMOTE
set -euo pipefail
T=/tmp/$(basename "$TARBALL")
echo "[remote] 备份现有配置与运行时文件…"
sudo mkdir -p ${APP_DIR}
# qbot-config.js 是运行时配置（切换 local/cloud 时生成），不能被仓库版本覆盖；
# node_modules、dist、voice-clips、models 也不在仓库里，保留原样。
KEEP="qbot-config.js node_modules dist voice-clips models package.json"

BACKUP=/tmp/qbot-app-backup-\$(date +%s).tar.gz
sudo tar czf "\$BACKUP" -C ${APP_DIR} \$(for k in \$KEEP; do [ -e "${APP_DIR}/\$k" ] && echo "\$k"; done) 2>/dev/null || true
echo "[remote] 已备份到 \$BACKUP"

echo "[remote] 解包覆盖源码…"
sudo tar xzf "\$T" -C /tmp
sudo cp -a /tmp/dashboard/. ${APP_DIR}/
sudo chown -R ubuntu:ubuntu ${APP_DIR}

# 覆盖后必须确认关键文件真的到位——之前 tar 前缀写错时这里静默用了旧文件，
# 结果构建报 Missing script: "build"，排查很久。
for f in server.js package.json index.html src/main.tsx; do
  [[ -f ${APP_DIR}/\$f ]] || { echo "[remote] 覆盖后缺少 \$f，检查 tar 包结构"; exit 1; }
done
grep -q '"build"' ${APP_DIR}/package.json || { echo "[remote] package.json 里没有 build 脚本，覆盖可能失败"; exit 1; }
echo "[remote] 关键文件校验通过"

cd ${APP_DIR}
echo "[remote] 记录部署版本：${BRANCH}@${REV}"
echo "${BRANCH}@${REV} \$(date -Iseconds)" | sudo tee ${APP_DIR}/.deployed-rev >/dev/null

if [[ "${DO_BUILD}" -eq 1 ]]; then
  echo "[remote] 安装依赖并构建前端…"
  NODE_BIN=/opt/qbot/node/bin/node
  NPM_BIN=/opt/qbot/node/bin/npm
  [[ -x "\$NPM_BIN" ]] || { echo "[remote] 找不到 \$NPM_BIN"; exit 1; }
  export PATH=/opt/qbot/node/bin:\$PATH
  sudo -u ubuntu env PATH=\$PATH \$NPM_BIN install --omit=dev --no-audit --no-fund >/tmp/qbot-push-npm.log 2>&1 || { echo "[remote] npm install 失败，见 /tmp/qbot-push-npm.log"; tail -5 /tmp/qbot-push-npm.log; exit 1; }
  # 构建要 devDependencies（vite/tsc），先全量装再构建
  sudo -u ubuntu env PATH=\$PATH \$NPM_BIN install --no-audit --no-fund >/tmp/qbot-push-npm2.log 2>&1 || { echo "[remote] npm install(dev) 失败"; exit 1; }
  sudo -u ubuntu env PATH=\$PATH \$NPM_BIN run build >/tmp/qbot-push-build.log 2>&1 || { echo "[remote] 构建失败，见 /tmp/qbot-push-build.log"; tail -10 /tmp/qbot-push-build.log; exit 1; }
  # 构建会重写 dist，运行时配置必须重新放回去
  if [[ -f ${APP_DIR}/public/qbot-config.js ]]; then
    sudo cp ${APP_DIR}/public/qbot-config.js ${APP_DIR}/dist/qbot-config.js
    sudo chown ubuntu:ubuntu ${APP_DIR}/dist/qbot-config.js
  fi
  grep -q 'qbot-config\.js' ${APP_DIR}/dist/index.html || { echo "[remote] 构建后 index.html 丢了配置引用"; exit 1; }
  grep -rq '__QBOT_CONFIG__' ${APP_DIR}/dist/assets/ || { echo "[remote] 构建后 bundle 丢了配置读取逻辑"; exit 1; }
  echo "[remote] 构建完成，运行时配置链路校验通过"
fi

if [[ "${DO_RESTART}" -eq 1 ]]; then
  # 先把输出收进变量再匹配：pipefail 下 `systemctl | grep -q` 会因 SIGPIPE 假失败，
  # 表现是"服务明明装好了却报未安装、跳过重启"。
  UNITS_OUT="\$(systemctl list-unit-files 2>/dev/null || true)"
  if grep -q '^qbot-dashboard\.service' <<<"\$UNITS_OUT"; then
    sudo systemctl restart qbot-dashboard qbot-gateway
    sleep 8
    for u in qbot-dashboard qbot-gateway; do
      printf '[remote] %-16s %s\n' "\$u" "\$(systemctl is-active \$u)"
    done
  else
    echo "[remote] 未安装 systemd 单元，跳过重启（可运行 deploy/install_cloud_services.sh）"
  fi
fi
echo "[remote] 完成。回滚：sudo tar xzf \$BACKUP -C ${APP_DIR}"
REMOTE
)

info "在云端执行部署…"
$SSH "${CLOUD_USER}@${CLOUD_HOST}" "bash -s" <<< "$REMOTE_SCRIPT"

info "全部完成：${CLOUD_USER}@${CLOUD_HOST}:${APP_DIR} ← ${BRANCH}@${REV}"
