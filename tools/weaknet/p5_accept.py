# -*- coding: utf-8 -*-
"""三节点拆分独立验收 v2。

基建修正：
  - 后台进程用 Popen(start_new_session=True, 文件句柄)，不经过 shell & 管道（消除 EOF 挂死）
  - pkill 模式用 [x] 技巧避免匹配执行进程自身
  - state 判据用 battery 字段：看板 deviceSim 会发无 battery 的仿真帧，必须区分
"""
import json
import os
import re
import subprocess
import sys
import time

# 工作区路径：优先取环境变量，便于换机器/换盘符时不改代码
WS = os.environ.get("QBOT_WS", "/run/media/wby/shuju/Document/robot-dog")
# FastDDS 的 UDP profile 随用户家目录变化，不再写死 /home/wby
FASTDDS = os.environ.get("FASTRTPS_PROFILE",
                         os.path.expanduser("~/fastdds_udp.xml"))
ENV = ("cd %s && source /opt/ros/lyrical/setup.bash && source install/setup.bash && "
       "export FASTRTPS_DEFAULT_PROFILES_FILE=%s" % FASTDDS)
P = "qbot/v1"
RID = "robot-01"
LOG = "/tmp/sim_p5.log"
res = []


def sh(cmd, timeout=60, ros=True):
    """前台命令：ros=True 时先 source 环境（ros2/mosquitto 都要）。"""
    full = (ENV + " && " + cmd) if ros else cmd
    return subprocess.run("bash -lc '%s'" % full.replace("'", "'\\''"), shell=True,
                          capture_output=True, text=True, timeout=timeout, executable="/bin/bash")


def bg(cmd, logfile):
    """后台启动：无管道依赖，立即返回。"""
    f = open(logfile, "ab")
    subprocess.Popen(["bash", "-lc", cmd], stdout=f, stderr=f, stdin=subprocess.DEVNULL,
                     start_new_session=True)


def check(name, ok, detail=""):
    res.append((name, bool(ok), detail))
    print(("  PASS " if ok else "  FAIL ") + name + ("  | " + str(detail)[:90] if detail else ""))


def mosq_sub(topic, wait=10):
    try:
        r = sh("timeout %d mosquitto_sub -h 127.0.0.1 -t '%s' -C 1 -W %d" % (wait + 4, topic, wait), timeout=wait + 10)
        return r.stdout.strip()
    except subprocess.TimeoutExpired:
        return ""


def mosq_pub(topic, payload, qos=1):
    sh("mosquitto_pub -h 127.0.0.1 -t '%s' -q %d -m '%s'" % (topic, qos, json.dumps(payload, ensure_ascii=False)),
       timeout=30)


def robot_state(wait=12):
    """取一帧【真实机器人】state：deviceSim 假帧无 battery，用 battery 判别。"""
    try:
        r = sh("timeout %d mosquitto_sub -h 127.0.0.1 -t '%s/robots/%s/state' -C 3 -W %d" % (wait + 4, P, RID, wait),
               timeout=wait + 10)
        for line in r.stdout.strip().split("\n"):
            try:
                m = json.loads(line)
                if "battery" in m:
                    return m
            except Exception:
                continue
    except subprocess.TimeoutExpired:
        pass
    return None


def node_set():
    try:
        r = sh("timeout 10 ros2 node list 2>/dev/null", timeout=15)
        return set(re.findall(r"/(\w+)$", r.stdout, re.M))
    except subprocess.TimeoutExpired:
        return set()


def log():
    try:
        return open(LOG, encoding="utf-8", errors="ignore").read()
    except OSError:
        return ""


print("=== 1. 结构核对 ===")
for exe in ("mqtt_bridge", "task_runner", "voice_link", "deadman_guard", "voice_source", "image_sender"):
    check("可执行 " + exe, os.path.exists(WS + "/install/quadruped_mqtt_bridge/lib/quadruped_mqtt_bridge/" + exe))
src_all = ""
for root, _, files in os.walk(WS + "/src/quadruped_mqtt_bridge"):
    for f in files:
        if f.endswith((".cpp", ".hpp")):
            src_all += open(os.path.join(root, f), encoding="utf-8", errors="ignore").read()
for need in ("/state'", "/cmd", "/fleet/tasks", "/event", "/audio", "/tts", "/site/map"):
    check("主题 " + need.strip("'"), need.strip("'") in src_all)
lwt = [f for f in os.listdir(WS + "/src/quadruped_mqtt_bridge/src") if f.endswith(".cpp")
       and "mosquitto_will_set" in open(WS + "/src/quadruped_mqtt_bridge/src/" + f, encoding="utf-8", errors="ignore").read()]
check("遗嘱只在 mqtt_bridge 目标", bool(lwt) and not any(("task_runner" in f or "voice_link" in f) for f in lwt), str(lwt))
r = sh("wc -l %s/src/quadruped_mqtt_bridge/src/*.cpp %s/src/quadruped_mqtt_bridge/include/quadruped_mqtt_bridge/*.hpp | tail -3" % (WS, WS))
print(r.stdout.strip().split("\n")[-1])

print("=== 2. 全新起栈 ===")
for pat in ("gz-sim-mai[n]", "ros2 launch quadruped_si[m]", "controller_serve[r]", "planner_serve[r]",
            "bt_navigato[r]", "behavior_serve[r]", "map_serve[r]", "amc[l]", "lifecycle_manage[r]",
            "collision_monito[r]", "smoother_serve[r]", "parameter_bridg[e]", "voice_sourc[e]",
            "champ_joint_rela[y]", "quadruped_controlle[r]", "patrol_nod[e]", "image_sende[r]",
            "mqtt_bridg[e]", "task_runne[r]", "voice_lin[k]", "deadman_guar[d]", "gait_controlle[r]",
            "odom_pu[b]", "camera_info_pu[b]", "ros_gz_bridg[e]"):
    sh("pkill -9 -f '%s' 2>/dev/null" % pat, timeout=15, ros=False)
time.sleep(4)
os.path.exists(LOG) and os.remove(LOG)
launch = ("ros2 launch quadruped_sim quadruped_gz.launch.py gui:=false rviz:=false nav:=true "
          "gait:=champ teleop:=false patrol:=false map:=%s/maps/factory_full.yaml" % WS)
bg(ENV + " && " + launch, LOG)
print("  已启动，等待 105s ...")
time.sleep(105)

print("=== 3a. 节点集（重试 3 次）===")
nodes = set()
for i in range(3):
    nodes = node_set()
    if {"mqtt_bridge", "task_runner", "voice_link", "amcl"} <= nodes:
        break
    time.sleep(12)
for n in ("mqtt_bridge", "task_runner", "voice_link", "deadman_guard", "map_server", "amcl",
          "planner_server", "controller_server", "bt_navigator"):
    check("节点 /" + n, n in nodes)
check("无 TF_OLD_DATA", "TF_OLD_DATA" not in log())

print("=== 3b. 遥测 ===")
st = robot_state(15)
check("真实 state 有 battery/位姿", bool(st and "battery" in st and "x" in st),
      "keys=%s" % (list(st.keys())[:8] if st else "无"))
m = [l for l in log().split("\n") if "上报:" in l]
check("上报日志>=2 条", len(m) >= 2, m[-1][-60:] if m else "无")

print("=== 3c. 语音全链路 ===")
t_start = int(time.time() * 1000) - 2000   # voice/commands 是 retained 快照，只认本轮指令
bg(ENV + " && ros2 run quadruped_mqtt_bridge voice_source --ros-args -p mode:=file "
   '-p "wav_path:=/home/wby/voice-clips/cmd_inspection.wav" -p loop:=false', "/tmp/voice_p5.log")
cmd_id = None
cur = None
for _ in range(6):
    time.sleep(6)
    try:
        r = sh("timeout 14 mosquitto_sub -h 127.0.0.1 -t '%s/voice/commands' -C 1 -W 12" % P, timeout=20)
        t = json.loads(r.stdout.strip())[0]
        if int(t.get("ts", 0)) < t_start:
            continue                       # retained 旧回执，继续等本轮那条
        cur = t
        break
    except Exception:
        continue
check("本轮语音指令已产出", cur is not None, cur.get("transcript") if cur else "未取到")
if cur:
    check("ASR→LLM 待确认", cur.get("status") == "pending_confirm",
          "%s|%s|%s" % (cur.get("transcript"), cur.get("engine"), cur.get("status")))
    if cur.get("status") == "pending_confirm":
        cmd_id = cur.get("cmdId")
if cmd_id:
    mosq_pub("%s/voice/cmd" % P, {"cmdId": cmd_id, "action": "confirm"})
    print("  已确认，等 70s ...")
    time.sleep(70)
    lg = log()
    check("task_runner 派单", "开始前往" in lg)
    check("到达完成", "任务完成" in lg or "已到达" in lg)
    try:
        r = sh("timeout 14 mosquitto_sub -h 127.0.0.1 -t '%s/fleet/tasks' -C 1 -W 12" % P, timeout=20)
        tl = json.loads(r.stdout.strip())
        tl = tl if isinstance(tl, list) else tl.get("tasks", [])
        vt = [t for t in tl if t.get("source") == "voice" and int(t.get("createdAt") or 0) >= t_start]
        check("本轮任务回写 completed", bool(vt) and vt[0].get("status") == "completed",
              "%s|%s" % (vt[0].get("id"), vt[0].get("status")) if vt else "无本轮任务")
    except Exception as e:
        check("fleet/tasks 解析", False, repr(e)[:70])

print("=== 3d. TTS ===")
before = os.path.getmtime("/tmp/qbot_tts.wav") if os.path.exists("/tmp/qbot_tts.wav") else 0
mosq_pub("%s/robots/%s/tts" % (P, RID), {"cmdId": "vc-p5-tts", "ts": int(time.time() * 1000), "text": "三节点拆分验收"})
time.sleep(9)
after = os.path.getmtime("/tmp/qbot_tts.wav") if os.path.exists("/tmp/qbot_tts.wav") else 0
check("TTS 音频更新", after > before)
check("TTS 播报日志", "TTS 播报" in log())

print("=== 3e. 急停/恢复 ===")
mosq_pub("%s/robots/%s/cmd" % (P, RID), {"action": "estop"})
time.sleep(4)
st_e = robot_state(12) or {}
check("急停后状态 fault", st_e.get("status") == "fault", st_e.get("status"))
check("急停日志", "急停" in log())
mosq_pub("%s/robots/%s/cmd" % (P, RID), {"action": "resume"})
time.sleep(4)
st_r = robot_state(12) or {}
check("恢复回 idle", st_r.get("status") == "idle", st_r.get("status"))

print("=== 4. 故障隔离 ===")
sh("pkill -9 -f 'quadruped_mqtt_bridge/task_runne[r]'", timeout=15)
time.sleep(6)
check("task_runner 死后 state 仍在", robot_state(15) is not None)
sh("pkill -9 -f 'quadruped_mqtt_bridge/mqtt_bridg[e]'", timeout=15)
time.sleep(5)
# 注意：看板 deviceSim 可能补发假帧，所以这里取 3 帧里是否有 battery
real = robot_state(12)
check("mqtt_bridge 死后真实 state 停止", real is None, str(list(real.keys())[:5]) if real else "")

print("\n=== 汇总 ===")
fails = [r for r in res if not r[1]]
print("  通过 %d / %d" % (len(res) - len(fails), len(res)))
for n, _, d in fails:
    print("  FAIL:", n, "|", d)
sys.exit(1 if fails else 0)
