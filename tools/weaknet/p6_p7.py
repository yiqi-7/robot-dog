# -*- coding: utf-8 -*-
"""P6+P7 合并套件：看板派单安全执行 + 弱网（tc netem 只伤 1883/8081，不碰 DDS）。"""
import json
import os
import re
import subprocess
import sys
import time

# 工作区路径：优先取环境变量，便于换机器/换盘符时不改代码
WS = os.environ.get("QBOT_WS") or os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
# FastDDS 的 UDP profile 随用户家目录变化，这里用 $HOME 推导
FASTDDS = os.environ.get("FASTRTPS_PROFILE",
                         os.path.expanduser("~/fastdds_udp.xml"))
ENV = ("cd %s && source /opt/ros/lyrical/setup.bash && source install/setup.bash && "
       "export FASTRTPS_DEFAULT_PROFILES_FILE=%s" % FASTDDS)
P = "qbot/v1"
RID = "robot-01"
LOG = "/tmp/sim_p6.log"
# sudo 密码从环境变量读取，禁止把密码写进仓库代码：
#   export WEAKNET_SUDO_PW=<密码>   脚本会拼成 "echo <密码> | sudo -S"
_sudo_pw = __import__("os").environ.get("WEAKNET_SUDO_PW", "")
SUDO = ("echo %s | sudo -S -p '' " % _sudo_pw) if _sudo_pw else "sudo -n "
res = []


def sh(cmd, timeout=60, ros=True):
    full = (ENV + " && " + cmd) if ros else cmd
    return subprocess.run("bash -lc '%s'" % full.replace("'", "'\\''"), shell=True,
                          capture_output=True, text=True, timeout=timeout, executable="/bin/bash")


def bg(cmd, logfile):
    f = open(logfile, "ab")
    subprocess.Popen(["bash", "-lc", cmd], stdout=f, stderr=f, stdin=subprocess.DEVNULL, start_new_session=True)


def check(name, ok, detail=""):
    res.append((name, bool(ok), detail))
    print(("  PASS " if ok else "  FAIL ") + name + ("  | " + str(detail)[:85] if detail else ""))


def pub(topic, obj, qos=1):
    sh("mosquitto_pub -h 127.0.0.1 -t '%s' -q %d -m '%s'" % (topic, qos, json.dumps(obj, ensure_ascii=False)), timeout=30)


def sub_raw(topic, wait=10, count=1):
    try:
        return sh("timeout %d mosquitto_sub -h 127.0.0.1 -t '%s' -C %d -W %d" % (wait + 4, topic, count, wait),
                  timeout=wait + 10).stdout.strip()
    except subprocess.TimeoutExpired:
        return ""


def real_state(wait=12):
    try:
        out = sh("timeout %d mosquitto_sub -h 127.0.0.1 -t '%s/robots/%s/state' -C 4 -W %d" % (wait + 4, P, RID, wait),
                 timeout=wait + 10).stdout
        for line in out.split("\n"):
            try:
                m = json.loads(line)
                if "battery" in m:
                    return m
            except Exception:
                pass
    except subprocess.TimeoutExpired:
        pass
    return None


def odom(wait=8):
    try:
        r = sh("timeout %d ros2 topic echo /odom --once --field pose.pose.position" % wait, timeout=wait + 5).stdout
        m = re.search(r"x:\s*(-?[\d.]+).*?y:\s*(-?[\d.]+)", r, re.S)
        return (float(m.group(1)), float(m.group(2))) if m else (None, None)
    except subprocess.TimeoutExpired:
        return (None, None)


def vmax(seconds=4):
    try:
        r = sh("timeout %d ros2 topic echo /cmd_vel --field linear.x" % seconds, timeout=seconds + 5).stdout
        vals = [abs(float(x)) for x in re.findall(r"^\s*(-?\d+\.?\d*(?:e-?\d+)?)\s*$", r, re.M)]
        return max(vals) if vals else 0.0
    except subprocess.TimeoutExpired:
        return 0.0


def logs():
    try:
        return open(LOG, encoding="utf-8", errors="ignore").read()
    except OSError:
        return ""


def tasks():
    try:
        t = json.loads(sub_raw("%s/fleet/tasks" % P, 10) or "[]")
        return t if isinstance(t, list) else t.get("tasks", [])
    except Exception:
        return []


# ---------------- 起栈 ----------------
print("=== 起栈 ===")
for pat in ("gz-sim-mai[n]", "ros2 launch quadruped_si[m]", "controller_serve[r]", "planner_serve[r]",
            "bt_navigato[r]", "behavior_serve[r]", "map_serve[r]", "amc[l]", "lifecycle_manage[r]",
            "collision_monito[r]", "smoother_serve[r]", "parameter_bridg[e]", "ros_gz_bridg[e]",
            "voice_sourc[e]", "champ_joint_rela[y]", "quadruped_controlle[r]", "patrol_nod[e]",
            "image_sende[r]", "mqtt_bridg[e]", "task_runne[r]", "voice_lin[k]", "gait_controlle[r]",
            "odom_pu[b]", "camera_info_pu[b]"):
    sh("pkill -9 -f '%s' 2>/dev/null" % pat, timeout=15, ros=False)
sh(SUDO + "tc qdisc del dev lo root 2>/dev/null", timeout=20, ros=False)
time.sleep(4)
os.path.exists(LOG) and os.remove(LOG)
bg(ENV + " && ros2 launch quadruped_sim quadruped_gz.launch.py gui:=false rviz:=false nav:=true "
   "gait:=champ teleop:=false patrol:=false map:=%s/maps/factory_full.yaml" % WS, LOG)
print("  等待 100s ...")
time.sleep(100)
n = set(re.findall(r"/(\w+)$", sh("timeout 10 ros2 node list", timeout=15).stdout, re.M))
check("栈就绪", {"mqtt_bridge", "task_runner", "voice_link", "amcl"} <= n, str(sorted(x for x in n if "server" in x or x in ("amcl",))[:4]))

# ---------------- P6 安全执行 ----------------
print("=== S1 非法目标不盲走 ===")
x0, y0 = odom()
n0 = logs().count("开始前往")
ev_proc = subprocess.Popen(
  ["bash", "-lc", "timeout 14 mosquitto_sub -h 127.0.0.1 -t '%s/event' -C 1 -W 12" % P],
  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
time.sleep(2)   # 事件不 retained：必须先订阅再触发，否则必然错过
pub("%s/robots/%s/cmd" % (P, RID), {"action": "assign", "task": {"id": 9001, "target": "no-such-place", "title": "非法"}})
time.sleep(7)
x1, y1 = odom()
moved = abs((x1 or 0) - (x0 or 0)) + abs((y1 or 0) - (y0 or 0))
check("未启动导航", logs().count("开始前往") == n0)
check("未移动", moved < 0.15, "%.2f m" % moved)
try:
    ev = ev_proc.stdout.readline().strip()
except Exception:
    ev = ""
ev_proc.kill()
check("事件上报", "拒绝" in ev or "no-such-place" in ev, ev[:60])

print("=== S2 急停门禁 ===")
pub("%s/robots/%s/cmd" % (P, RID), {"action": "estop"})
time.sleep(2)
x2, y2 = odom()
pub("%s/robots/%s/cmd" % (P, RID), {"action": "assign", "task": {"id": 9002, "target": "charger-1", "title": "急停中"}})
time.sleep(9)
x3, y3 = odom()
check("急停中派单不移动", abs((x3 or 0) - (x2 or 0)) + abs((y3 or 0) - (y2 or 0)) < 0.2,
      "%.2f m" % (abs((x3 or 0) - (x2 or 0)) + abs((y3 or 0) - (y2 or 0))))
st = real_state(10) or {}
check("急停后状态 fault", st.get("status") == "fault", st.get("status"))
pub("%s/robots/%s/cmd" % (P, RID), {"action": "resume"})
time.sleep(4)
st = real_state(12) or {}
check("恢复后状态 idle", st.get("status") == "idle", st.get("status"))

print("=== S3 重复派单幂等 ===")
n1 = logs().count("开始前往")
body = {"action": "assign", "task": {"id": 9003, "target": "warehouse-racks", "title": "幂等"}}
pub("%s/robots/%s/cmd" % (P, RID), body)
time.sleep(2)
pub("%s/robots/%s/cmd" % (P, RID), body)
time.sleep(12)
check("只执行一次", logs().count("开始前往") - n1 == 1, "%d 次" % (logs().count("开始前往") - n1))

print("=== S5 速度上限 ===")
v = vmax(6)
check("峰值 ≤0.35 m/s", 0 < v <= 0.35, "%.3f m/s" % v)

print("=== S4 取消及时 ===")
pub("%s/robots/%s/cmd" % (P, RID), {"action": "cancel", "id": 9003})
time.sleep(3)
check("取消后速度≈0", vmax(3) < 0.06, "%.3f" % vmax(2))
tk = [t for t in tasks() if str(t.get("id")) == "9003"]
check("终态已回写(revoked)", bool(tk) and tk[0].get("status") in ("revoked", "canceled"), tk[0].get("status") if tk else "无回写")

print("=== S6 控制端死亡后是否停（安全关键）===")
pub("%s/robots/%s/cmd" % (P, RID), {"action": "assign", "task": {"id": 9004, "target": "warehouse-racks", "title": "死亡"}})
time.sleep(18)   # Nav2 目标接受到出速度有延迟，急停/取消扰动后更慢
v_run = vmax(4)
sh("pkill -9 -f 'quadruped_mqtt_bridge/task_runne[r]'", timeout=15, ros=False)
time.sleep(5)
v_dead = vmax(4)
check("派单后在走", v_run > 0.05, "%.3f" % v_run)
check("【安全】执行节点死亡后机器人停止", v_dead < 0.06, "死亡后峰值 %.3f" % v_dead)
check("看门狗切断日志", "切断速度链路" in logs(), "日志未见")

print("=== S7 无僵尸任务 ===")
bad = [t.get("id") for t in tasks() if t.get("status") in ("running", "active", "executing")]
check("无僵尸 running", len(bad) == 0, str(bad))

# 恢复 task_runner
bg(ENV + " && ros2 run quadruped_mqtt_bridge task_runner", LOG)
time.sleep(8)

# ---------------- P7 弱网 ----------------
def gaps(topic, n=5, wait=40):
    """测遥测帧到达间隔（比 RTT 探针稳：不依赖偶发事件）"""
    proc = subprocess.Popen(["bash", "-lc", "timeout %d mosquitto_sub -h 127.0.0.1 -t '%s' -C %d -W %d" % (wait + 5, topic, n, wait)],
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    ts = []
    try:
        for line in proc.stdout:
            ts.append(time.time())
            if len(ts) >= n:
                break
    finally:
        proc.kill()
    return [(ts[i + 1] - ts[i]) for i in range(len(ts) - 1)]


print("=== W0 基线遥测间隔 ===")
g0 = gaps("%s/robots/%s/state" % (P, RID), 5, 20)
base = sum(g0) / len(g0) if g0 else 99
check("基线 ~1Hz", g0 and base < 3.0, "均值 %.1fs n=%d" % (base, len(g0)))
print("=== W1 施加弱网 ===")
r = sh(SUDO + "tc qdisc add dev lo root handle 1: prio && " + SUDO +
       "tc qdisc add dev lo parent 1:3 handle 30: netem delay 300ms 100ms loss 15% duplicate 5% && " + SUDO +
       "tc filter add dev lo protocol ip parent 1:0 prio 3 u32 match ip dport 1883 0xffff flowid 1:3 && " + SUDO +
       "tc filter add dev lo protocol ip parent 1:0 prio 3 u32 match ip dport 8081 0xffff flowid 1:3",
       timeout=40, ros=False)
check("tc 生效", r.returncode == 0, (r.stderr or "")[-70:])

print("=== W2 弱网下遥测存活 ===")
t0 = time.time()
g1 = gaps("%s/robots/%s/state" % (P, RID), 4, 30)
check("弱网 state 仍到达且间隔受限", len(g1) >= 2 and max(g1) < 25, "间隔 %s" % [round(x, 1) for x in g1])
check("节点未崩", sh("pgrep -f 'quadruped_mqtt_bridge/mqtt_bridg[e]'", ros=False).returncode == 0)

print("=== W3 弱网下派单仍安全执行 ===")
x0, y0 = odom()
body = {"action": "assign", "task": {"id": 9100, "target": "ring-road", "title": "弱网派单"}}
pub("%s/robots/%s/cmd" % (P, RID), body)
time.sleep(3)
pub("%s/robots/%s/cmd" % (P, RID), body)
time.sleep(28)   # 弱网下 Nav2 启动更慢（丢包 15%）
x1, y1 = odom()
moved = abs((x1 or 0) - (x0 or 0)) + abs((y1 or 0) - (y0 or 0))
check("弱网下派单执行", "9100" in logs() and moved > 0.10, "位移 %.2f m" % moved)
time.sleep(12)
cnt = len([t for t in tasks() if str(t.get("id")) == "9100"])
check("任务单实例（重传未重复建）", cnt <= 1, "%d 条" % cnt)

print("=== W4 弱网下语音链路 ===")
bg(ENV + " && ros2 run quadruped_mqtt_bridge voice_source --ros-args -p mode:=file "
   '-p "wav_path:=$HOME/voice-clips/cmd_inspection.wav" -p loop:=false', "/tmp/voice_p6.log")
t_start = int(time.time() * 1000) - 2000   # 只认本次产生的指令，排除上一轮残留
res_v, ok = "无", False
for _ in range(5):
    out = sub_raw("%s/voice/commands" % P, 20)
    try:
        c = json.loads(out)[0]
        if int(c.get("ts", 0)) < t_start:
            continue          # 旧指令，丢弃继续等
        res_v = "%s|%s|%s" % (c.get("transcript"), c.get("engine"), c.get("status"))
        ok = c.get("status") == "pending_confirm"
        if ok:
            pub("%s/voice/cmd" % P, {"cmdId": c["cmdId"], "action": "confirm"})
        break
    except Exception:
        continue
check("弱网语音→LLM 决策", ok, res_v)
print("=== W5 LWT 掉线检测 ===")
sub5 = subprocess.Popen(
  ["bash", "-lc", "timeout 55 mosquitto_sub -h 127.0.0.1 -t '%s/robots/%s/state' -W 52" % (P, RID)],
  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
time.sleep(2)                       # 订阅先就位，再杀 mqtt_bridge
sh("pkill -9 -f 'quadruped_mqtt_bridge/mqtt_bridg[e]'", ros=False)
t0 = time.time()
off = ""
try:
    for line in sub5.stdout:        # 逐行实时读，offline 到达即刻计时
        if "offline" in line:
            off = line.strip()
            break
except Exception:
    pass
sub5.kill()
dt = time.time() - t0
check("收到 offline（keepalive 10s，期望≤35s）", bool(off) and dt < 35, "%.0fs %s" % (dt, off[:60]))

print("=== W6 恢复 ===")
sh(SUDO + "tc qdisc del dev lo root 2>/dev/null", timeout=20, ros=False)
q = sh(SUDO + "tc qdisc show dev lo", ros=False, timeout=20).stdout
check("tc 已清", "netem" not in q, q.strip()[:50])
for nd in ("mqtt_bridge", "voice_link"):
    bg(ENV + " && ros2 run quadruped_mqtt_bridge %s" % nd, LOG)
time.sleep(10)
check("恢复后 state 恢复", real_state(20) is not None)
sh(SUDO + "tc qdisc del dev lo root 2>/dev/null", timeout=20, ros=False)

print("\n=== 汇总 ===")
fails = [x for x in res if not x[1]]
print("  通过 %d / %d" % (len(res) - len(fails), len(res)))
for n_, _, d in fails:
    print("  FAIL:", n_, "|", d)
sys.exit(1 if fails else 0)
