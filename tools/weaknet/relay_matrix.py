# -*- coding: utf-8 -*-
"""真实中转拓扑弱网矩阵编排（本地执行）。

拓扑：端侧机器狗 ──公网──> 中转服务器 <──公网── 本地看板

每档：
  1) 端侧 tc 施加弱网（只整形去往服务器的流量，双向）
  2) 从服务器侧观测：遥测到达、状态完整性、LWT、看板可见性
  3) 从服务器下发派单，验证下行链路能否到达端侧并执行
  4) 撤除弱网，验证自愈
"""
import json
import re
import subprocess
import sys
import time

# 站点配置走环境变量，避免把私钥路径/地址/IP 写死进仓库：
#   WEAKNET_KEY     本地到中转服务器的 ssh 私钥（留空则用 ssh 配置/agent）
#   WEAKNET_SERVER  中转服务器，形如 ubuntu@1.2.3.4
#   WEAKNET_ROBOT   端侧主机（ssh 别名或 user@host）
#   WEAKNET_LOG     端侧 launch 日志路径
#   WEAKNET_PREFIX  MQTT 主题前缀（默认 qbot/v1）
#   WEAKNET_ROBOT_ID 机器人 ID（默认 robot-01）
import os

KEY = os.environ.get("WEAKNET_KEY", "")
SRV = os.environ.get("WEAKNET_SERVER", "")
ROBOT = os.environ.get("WEAKNET_ROBOT", "")
RLOG = os.environ.get("WEAKNET_LOG", "/tmp/launch.log")
P = os.environ.get("WEAKNET_PREFIX", "qbot/v1")
RID = os.environ.get("WEAKNET_ROBOT_ID", "robot-01")

if not (SRV and ROBOT):
    raise SystemExit("请先设置 WEAKNET_SERVER 与 WEAKNET_ROBOT（见文件头注释）")
res = []


def run(cmd, timeout=90):
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                              encoding="utf-8", errors="replace", timeout=timeout)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, -1, "", "timeout")


def srv(cmd, timeout=90):
    """在腾讯云服务器上执行"""
    inner = cmd.replace('"', '\\"')
    key_opt = ("-i " + KEY + " ") if KEY else ""
    return run('ssh %s-o StrictHostKeyChecking=no -o ConnectTimeout=15 %s "%s"' % (key_opt, SRV, inner), timeout)


def robot(cmd, timeout=120):
    """在机器人侧主机上执行"""
    inner = cmd.replace('"', '\\"')
    return run('ssh -4 -o ConnectTimeout=20 %s "%s"' % (ROBOT, inner), timeout)


def check(tag, name, ok, detail=""):
    res.append((tag, name, bool(ok), detail))
    print(("    PASS " if ok else "    FAIL ") + name + ("  | " + str(detail)[:90] if detail else ""), flush=True)


def tc_apply(profile):
    r = robot("python3 /tmp/relay_tc.py apply %s" % profile, timeout=90)
    return ("已施加" in r.stdout or "基线" in r.stdout), (r.stdout or r.stderr).strip()[:100]


def tc_clear():
    robot("python3 /tmp/relay_tc.py clear", timeout=60)


def srv_state(n=5, wait=45):
    """服务器侧订阅 state，返回解析后的帧列表"""
    cmd = ("timeout %d mosquitto_sub -h 127.0.0.1 -p 1883 -t '%s/robots/%s/state' -C %d -W %d 2>/dev/null"
           % (wait + 5, P, RID, n, wait))
    r = srv(cmd, timeout=wait + 25)
    out = []
    for line in r.stdout.split("\n"):
        line = line.strip()
        if line.startswith("{"):
            try:
                out.append(json.loads(line))
            except Exception:
                pass
    return out


def srv_pub(obj):
    payload = json.dumps(obj, ensure_ascii=False)
    cmd = ("mosquitto_pub -h 127.0.0.1 -p 1883 -t '%s/robots/%s/cmd' -q 1 -m '%s'"
           % (P, RID, payload))
    return srv(cmd, timeout=60)


def robot_log(pat, n=5):
    return robot("grep -aE '%s' %s | tail -%d" % (pat, RLOG, n), timeout=60).stdout


PROFILES = [
    ("R0", "基线（真实公网，不整形）"),
    ("R1", "4G 良好"),
    ("R2", "4G 拥塞"),
    ("R3", "边缘弱信号"),
    ("R4", "卫星/极差链路"),
    ("R5", "断网 60s 恢复"),
]

print("=== 真实中转拓扑：端侧 -> 中转服务器 " + SRV + " -> 看板 ===", flush=True)

for tag, name in PROFILES:
    print("\n=== %s %s ===" % (tag, name), flush=True)
    ok, detail = tc_apply(tag)
    check(tag, "弱网规则生效", ok, detail)
    if not ok:
        tc_clear()
        continue

    if tag == "R5":
        print("    保持断网 55s ...", flush=True)
        time.sleep(55)
        lg = robot_log("意外断开|重连看门狗", 4)
        check(tag, "端侧检测到断线", ("意外断开" in lg) or ("重连看门狗" in lg), lg.strip()[:70])
        tc_clear()
        t_clear = time.time()
        back, st = None, None
        while time.time() - t_clear < 70:
            frames = srv_state(2, 12)
            if frames and frames[-1].get("battery"):
                back = time.time() - t_clear
                st = frames[-1]
                break
            time.sleep(3)
        check(tag, "恢复后看板重新收到遥测（<=30s）", back is not None and back <= 30,
              "%.0fs status=%s" % (back, st.get("status")) if back else "未恢复")
        tid = 660500
        srv_pub({"action": "assign", "task": {"id": tid, "target": "warehouse-racks", "title": "断网恢复"}})
        t1 = time.time()
        got = False
        while time.time() - t1 < 45:
            if ("任务#%d" % tid) in robot_log("任务#%d" % tid, 3):
                got = True
                break
            time.sleep(3)
        check(tag, "恢复后派单可执行", got, "%.0fs" % (time.time() - t1))
        srv_pub({"action": "cancel", "id": tid})
        continue

    time.sleep(3)
    frames = srv_state(5, 50)
    check(tag, "遥测穿越公网到达服务器", len(frames) >= 3, "收到 %d 帧" % len(frames))
    if frames:
        st = frames[-1]
        check(tag, "状态字段完整", all(k in st for k in ("battery", "status", "x", "y")),
              "status=%s battery=%s x=%s" % (st.get("status"), st.get("battery"), st.get("x")))

    tid = 660000 + int(tag[1]) * 100
    t0 = time.time()
    srv_pub({"action": "assign", "task": {"id": tid, "target": "ring-road", "title": "中转%s" % tag}})
    got = False
    while time.time() - t0 < 40:
        if ("任务#%d" % tid) in robot_log("任务#%d" % tid, 3):
            got = True
            break
        time.sleep(2)
    check(tag, "下行派单穿越公网到达端侧", got, "%.0fs" % (time.time() - t0))
    srv_pub({"action": "cancel", "id": tid})
    time.sleep(2)

    if tag != "R0":
        tc_clear()
        time.sleep(2)
        f2 = srv_state(3, 25)
        check(tag, "撤除弱网后遥测恢复", len(f2) >= 2, "收到 %d 帧" % len(f2))

tc_clear()
q = robot("python3 /tmp/relay_tc.py probe", timeout=90).stdout
print("\n=== 端侧终态 ===", flush=True)
print("\n".join(q.strip().split("\n")[:8]), flush=True)

print("\n=== 汇总 ===", flush=True)
bad = [r for r in res if not r[2]]
print("  通过 %d / %d" % (len(res) - len(bad), len(res)), flush=True)
for tag, name, _, d in bad:
    print("  FAIL [%s] %s | %s" % (tag, name, d), flush=True)
sys.exit(0 if not bad else 1)
