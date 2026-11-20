# Robot Dog

用 ROS 2 和 Gazebo 在虚拟厂区里运行四足巡检机器人。仿真支持建图、定位和 Nav2 航点巡逻。通过浏览器看板派发任务，查看位置、遥测、告警和视频。

[English](README.en.md) · [部署指南](deploy/README-DEPLOY.md) · [问题反馈](https://github.com/yiqi-7/robot-dog/issues)

## 先体验看板

准备 Git 和 Node.js 22.12 或更新版本。看板内置模拟数据，可在没有 ROS 2、机器人和 MQTT Broker 的电脑上运行。

```bash
git clone https://github.com/yiqi-7/robot-dog.git
cd robot-dog/dashboard
npm ci
```

打开 [public/qbot-config.js](dashboard/public/qbot-config.js)，把 `mqtt.mode` 从 `"real"` 改为 `"mock"`，然后运行：

```bash
npm run dev
```

打开 <http://localhost:3000>。看到机器人队列、厂区地图以及页脚的「内置模拟」，就可以在看板中试用任务与告警页面。

## 运行完整仿真

完整仿真使用 Ubuntu 26.04、ROS 2 Lyrical、Gazebo Sim 10、CHAMP 步态控制和 Nav2 导航。按[本地部署指南](deploy/README-DEPLOY.md#模式-1本地全栈单机)准备 ROS 工作区、看板运行副本和 Mosquitto。构建并加载 ROS 工作区后，在仓库根目录运行：

```bash
./deploy/switch_config.sh local
./deploy/mode1_local_all.sh
```

在 <http://127.0.0.1:8080> 查看实时数据，或按[验证清单](deploy/README-DEPLOY.md#验证清单)检查 MQTT 遥测与视频。运行 `./deploy/stop_all.sh local` 停止服务。[云端中转部署](deploy/README-DEPLOY.md#模式-2云端分离生产--远程控制)另有配置步骤。

<details>
<summary>ROS 工作区准备命令</summary>

先按照 [ROS 2 Lyrical 文档](https://docs.ros.org/en/lyrical/Get-Started/Installation/Ubuntu-Install-Debs.html)配置软件源。在 Ubuntu 上从仓库根目录运行：

```bash
sudo apt install ros-lyrical-desktop ros-lyrical-navigation2 \
  ros-lyrical-slam-toolbox ros-lyrical-ros-gz ros-dev-tools \
  libmosquitto-dev mosquitto mosquitto-clients nlohmann-json3-dev \
  libopencv-dev libx11-dev ffmpeg
source /opt/ros/lyrical/setup.bash
colcon build --symlink-install
source install/setup.bash
```

</details>

## 接口与限制

[看板配置](dashboard/README.md) · [MQTT 接口](dashboard/MQTT-INTERFACE.md) · [语音接口](dashboard/VOICE-INTERFACE.md) · [视频链路](dashboard/VIDEO-LINK.md)

项目目前只在仿真中验证。Nav2 的激光雷达避障仅覆盖导航链路，独立的近距离急停区域未启用。巡逻与恢复仍可能失败。示例 MQTT 和视频服务允许匿名访问，请在受信网络内使用。

项目代码采用 [MIT](LICENSE) 许可。CHAMP 采用上游 BSD-3-Clause 许可，其他第三方归属见 [NOTICE](NOTICE)。
