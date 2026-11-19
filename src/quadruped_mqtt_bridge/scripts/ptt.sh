#!/bin/bash
# 按键说话触发器（课程标准 ptt.sh 适配版）：回车 开始录音 / 再按回车 手动结束并上传
# 对应 quadruped_mqtt_bridge/voice_source 的 /voice/ptt 订阅。
source /opt/ros/lyrical/setup.bash
# 仓库根 = 本脚本所在目录的上三级（scripts -> 包 -> src -> 仓库根）
WS="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
source "${WS}/install/setup.bash"

echo "按键说话已就绪"
echo "  · 回车    = 开始录音"
echo "  · 再回车  = 结束并上传（VAD 最长 8 秒）"
echo "  · Ctrl+C  = 退出脚本"
echo

rec=0
while true; do
  read -r -n 1 -s _
  if [ "$rec" = 0 ]; then
    rec=1
    ros2 topic pub -1 /voice/ptt std_msgs/msg/Bool "{data: true}" >/dev/null 2>&1 &
    echo "[$(date +%H:%M:%S)] ● 录音中…"
  else
    rec=0
    ros2 topic pub -1 /voice/ptt std_msgs/msg/Bool "{data: false}" >/dev/null 2>&1 &
    echo "[$(date +%H:%M:%S)] ■ 手动结束，上传中…"
  fi
done
