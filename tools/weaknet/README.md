# 弱网与中转拓扑测试脚本

测试真实部署拓扑下的弱网表现：

```
本地看板 ──公网──> 中转服务器（MQTT broker） <──公网── 端侧机器狗
```

与"在单机上用 `tc` 整形 `lo` 回环"不同：这里整形的是**端侧到中转服务器的真实出口流量**，
观测点在服务器侧（等价于看板收到的内容），能覆盖非对称链路、指令乱序/重复、
看板状态陈旧、双向链路独立失效等真实故障模式。

## 环境变量

所有脚本的站点信息都走环境变量，不写死在代码里：

| 变量 | 说明 |
|---|---|
| `WEAKNET_SERVER` | 中转服务器 IP（`tc` 只整形去往它的流量） |
| `WEAKNET_NIC` | 端侧出口网卡，`ip route get $WEAKNET_SERVER` 可查到 |
| `WEAKNET_LOG` | 端侧 `ros2 launch` 日志路径，默认 `/tmp/launch.log` |
| `WEAKNET_SUDO` | sudo 前缀；已配免密时留空，否则填 `echo <密码> \| sudo -S -p '' ` |
| `WEAKNET_SUDO_PW` | 同上的密码形式（供 `p6_p7.py` 使用） |
| `WEAKNET_ROBOT` | 端侧 ssh 目标（ssh 别名或 `user@host`） |
| `WEAKNET_KEY` | 本地到服务器的 ssh 私钥，留空则用 ssh 配置/agent |
| `WEAKNET_PREFIX` | MQTT 主题前缀，默认 `qbot/v1` |
| `WEAKNET_ROBOT_ID` | 机器人 ID，默认 `robot-01` |

## 脚本

| 脚本 | 运行位置 | 作用 |
|---|---|---|
| `relay_tc.py` | 端侧 | 施加/清除弱网：`apply R1`、`clear`、`probe`（看连接事件与节点存活） |
| `relay_matrix.py` | 本地 | 六档弱网矩阵编排：施加 → 服务器侧观测遥测 → 下发派单 → 撤网验证自愈 |
| `relay_r5.py` | 本地 | 只跑 R5 断网档（60s 断开 → 恢复），用于回归重连退避 |
| `p5_accept.py` | 端侧 | 五节点桥接验收（遥测/派单/语音/急停/遗嘱） |
| `p6_p7.py` | 端侧 | 安全项与弱网回归 |
| `s6_only.py` | 端侧 | 单项复验（派单到目标点产生速度） |

## 档位

`tc netem` 参数按移动网络特征设计：

| 档位 | netem |
|---|---|
| R0 基线 | 不整形（测真实公网质量） |
| R1 4G 良好 | `delay 100ms 40ms distribution normal loss 1%` |
| R2 4G 拥塞 | `delay 300ms 150ms distribution normal loss 5% duplicate 2%` |
| R3 边缘弱信号 | `delay 800ms 400ms loss 15% 25% duplicate 5%` |
| R4 卫星/极差 | `delay 1500ms 800ms loss 30% 50% reorder 20% 50%` |
| R5 断网 | `loss 100%` |

注意 `tc` 规则必须**双向**匹配（`match ip dst` 与 `match ip src`），只匹配 `dport`
会漏掉回程，导致只劣化上行。

## 用法示例

```bash
# 端侧：施加 R3 边缘弱信号
WEAKNET_SERVER=1.2.3.4 WEAKNET_NIC=wlan0 python3 relay_tc.py apply R3
# 端侧：清除
WEAKNET_SERVER=1.2.3.4 WEAKNET_NIC=wlan0 python3 relay_tc.py clear

# 本地：跑完整矩阵
WEAKNET_SERVER=ubuntu@1.2.3.4 WEAKNET_ROBOT=my-robot \
WEAKNET_KEY=~/.ssh/id_ed25519 WEAKNET_LOG=/tmp/launch.log \
python3 relay_matrix.py
```

清理务必执行 `relay_tc.py clear`，否则网卡上残留的 `netem` 会影响后续所有流量
（`tc qdisc show dev <网卡>` 应显示 `noqueue`）。
