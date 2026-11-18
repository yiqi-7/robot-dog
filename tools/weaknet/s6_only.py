# -*- coding: utf-8 -*-
"""S6 单独复验：确认机器人真的在走 → 杀执行端 → 看门狗必须停车。"""
import json
import re
import subprocess
import time

WS = "/run/media/wby/shuju/Document/robot-dog"
ENV = ("cd %s && source /opt/ros/lyrical/setup.bash && source install/setup.bash && "
       "export FASTRTPS_DEFAULT_PROFILES_FILE=/home/wby/fastdds_udp.xml" % WS)
P, RID, LOG = "qbot/v1", "robot-01", "/tmp/sim_p6.log"


def sh(cmd, timeout=40, ros=True):
    full = (ENV + " && " + cmd) if ros else cmd
    try:
        return subprocess.run("bash -lc '%s'" % full.replace("'", "'\\''"), shell=True,
                              capture_output=True, text=True, timeout=timeout, executable="/bin/bash")
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, -1, "", "")


def vmax(sec=3):
    r = sh("timeout %d ros2 topic echo /cmd_vel --field linear.x" % sec, timeout=sec + 6)
    vals = [abs(float(x)) for x in re.findall(r"^\s*(-?\d+\.?\d*)\s*$", r.stdout, re.M)]
    return max(vals) if vals else 0.0


def logs():
    try:
        return open(LOG, encoding="utf-8", errors="ignore").read()
    except OSError:
        return ""


ok = True
print("=== 前置：task_runner 存活 ===")
alive = sh("pgrep -f 'quadruped_mqtt_bridge/task_runne[r]'", ros=False).returncode == 0
print("  task_runner:", "在" if alive else "不在")
if not alive:
    subprocess.Popen(["bash", "-lc", ENV + " && ros2 run quadruped_mqtt_bridge task_runner"],
                     stdout=open(LOG, "ab"), stderr=open(LOG, "ab"), start_new_session=True)
    time.sleep(8)

print("=== 1. 派单（远点 warehouse-racks）并等待真的走起来 ===")
sh("mosquitto_pub -h 127.0.0.1 -t '%s/robots/%s/cmd' -q 1 -m "
   "'{\"action\":\"assign\",\"task\":{\"id\":9300,\"target\":\"warehouse-racks\",\"title\":\"死亡验证\"}}'" % (P, RID))
v_run = 0.0
for i in range(16):
    v_run = vmax(3)
    if v_run > 0.05:
        break
    time.sleep(2)
print("  在走: %.3f m/s（第 %d 次采样）" % (v_run, i + 1))
ok &= v_run > 0.05

print("=== 2. 杀掉执行端 ===")
sh("pkill -9 -f 'quadruped_mqtt_bridge/task_runne[r]'", ros=False)
tripped = False
v_dead = 9.9
for i in range(6):
    time.sleep(2)
    v_dead = vmax(2)
    if "切断速度链路" in logs():
        tripped = True
    if v_dead < 0.06:
        break
print("  死亡后峰值: %.3f m/s，看门狗切断日志: %s" % (v_dead, tripped))
ok &= (v_dead < 0.06) and tripped

print("=== 3. 重启执行端 → 看门狗应放行 ===")
subprocess.Popen(["bash", "-lc", ENV + " && ros2 run quadruped_mqtt_bridge task_runner"],
                 stdout=open(LOG, "ab"), stderr=open(LOG, "ab"), start_new_session=True)
time.sleep(10)
released = "看门狗放行速度链路" in logs()
print("  放行日志:", released)
ok &= released

print("\nS6 结果:", "PASS" if ok else "FAIL")
raise SystemExit(0 if ok else 1)
