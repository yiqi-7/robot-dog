# -*- coding: utf-8 -*-
"""真实中转拓扑弱网测试（端侧 tc 整形 + 服务器侧观测，两段分开执行）。

用法（在机器人侧主机上跑）：
  python3 relay_weaknet.py apply <档位>    # 施加弱网
  python3 relay_weaknet.py clear           # 清除
  python3 relay_weaknet.py probe           # 观测端侧连接状态
观测端（本地）另用 relay_probe.ps1 从服务器侧看数据。

档位：
  R1 4G 良好      delay 100ms 40ms, loss 1%
  R2 4G 拥塞      delay 300ms 150ms, loss 5%, duplicate 2%
  R3 边缘弱信号   delay 800ms 400ms, loss 15%, duplicate 5%
  R4 卫星/极差    delay 1500ms 800ms, loss 30%, reorder 20%
  R5 断网         loss 100%
"""
import subprocess
import sys

# 站点配置走环境变量：
#   WEAKNET_SERVER  中转服务器 IP（tc 只整形去往它的流量）
#   WEAKNET_NIC     端侧出口网卡（`ip route get <服务器IP>` 可查到）
#   WEAKNET_LOG     端侧 launch 日志路径
#   WEAKNET_SUDO    sudo 前缀，默认空（已配好免密时无需密码）
import os

SERVER = os.environ.get("WEAKNET_SERVER", "")
NIC = os.environ.get("WEAKNET_NIC", "")
RLOG = os.environ.get("WEAKNET_LOG", "/tmp/launch.log")
SUDO = os.environ.get("WEAKNET_SUDO", "")

if not (SERVER and NIC):
    raise SystemExit("请先设置 WEAKNET_SERVER 与 WEAKNET_NIC（见文件头注释）")

NETEM = {
    "R0": None,
    "R1": "delay 100ms 40ms distribution normal loss 1%",
    "R2": "delay 300ms 150ms distribution normal loss 5% duplicate 2%",
    "R3": "delay 800ms 400ms loss 15% 25% duplicate 5%",
    "R4": "delay 1500ms 800ms loss 30% 50% reorder 20% 50%",
    "R5": "loss 100%",
}


def sh(cmd, timeout=40):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                              timeout=timeout, executable="/bin/bash")
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, -1, "", "timeout")


def clear():
    sh(SUDO + "tc qdisc del dev %s root 2>/dev/null" % NIC, timeout=25)


def apply(profile):
    clear()
    netem = NETEM.get(profile)
    if not netem:
        print("R0 基线：不整形")
        return 0
    cmds = [SUDO + "tc qdisc add dev %s root handle 1: prio" % NIC,
            SUDO + "tc qdisc add dev %s parent 1:3 handle 30: netem %s" % (NIC, netem)]
    # 双向：去程 dst、回程 src 都指向中转服务器
    for d in ("dst", "src"):
        cmds.append(SUDO + "tc filter add dev %s protocol ip parent 1:0 prio 3 u32 "
                    "match ip %s %s/32 flowid 1:3" % (NIC, d, SERVER))
    for c in cmds:
        r = sh(c, timeout=30)
        if r.returncode != 0:
            print("FAIL:", (r.stderr or r.stdout).strip()[:120])
            return 1
    print("%s 已施加: %s（网卡 %s → %s）" % (profile, netem, NIC, SERVER))
    return 0


def probe():
    """端侧视角：连接状态、重连日志、进程存活"""
    print("=== tc 现状 ===")
    print(sh(SUDO + "tc qdisc show dev %s" % NIC, timeout=20).stdout.strip()[:200])
    print("=== 连接事件（最近 8 条）===")
    print(sh("grep -aE 'MQTT 已连接|MQTT 意外断开' %s | tail -8" % RLOG, timeout=25).stdout.strip())
    print("=== 节点存活 ===")
    for nd in ("mqtt_bridge", "task_runner", "voice_link", "deadman_guard"):
        c = sh("pgrep -cf 'quadruped_mqtt_bridge/%s'" % nd, timeout=15).stdout.strip()
        print("  %s: %s" % (nd, c or 0))
    return 0


if __name__ == "__main__":
    act = sys.argv[1] if len(sys.argv) > 1 else "probe"
    if act == "clear":
        clear()
        print("tc 已清除")
        sys.exit(0)
    if act == "apply":
        sys.exit(apply(sys.argv[2] if len(sys.argv) > 2 else "R0"))
    sys.exit(probe())
