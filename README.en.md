# Robot Dog

Run a quadruped inspection robot in a Gazebo factory. Use the browser dashboard to dispatch tasks and view the map, telemetry and alerts. The project has only been validated in simulation.

[Quick start](#quick-start-try-the-dashboard) · [Full simulation](#next-steps) · [简体中文](README.md)

## Quick start: try the dashboard

You need Git and Node.js 22.12 or later (with npm). This walkthrough uses built-in sample data and needs no ROS 2, robot or MQTT broker.

```bash
git clone https://github.com/yiqi-7/robot-dog.git
cd robot-dog/dashboard
npm ci
```

In your current `dashboard` directory, open [public/qbot-config.js](dashboard/public/qbot-config.js) and change `mqtt.mode` from `"real"` to `"mock"`. From the same directory, run:

```bash
npm run dev
```

Open the address printed in the terminal (default: <http://localhost:3000>). Check that the footer says “内置模拟” (simulation), then choose another robot in the fleet list. The selected robot panel should switch to its ID and show its battery level and other telemetry. Press `Ctrl+C` to stop.

## Next steps

- [Full simulation](deploy/README-DEPLOY.md#模式-1本地全栈单机): try mapping, localization and waypoint patrols with ROS 2 Lyrical, Gazebo Sim 10, CHAMP and Nav2 on Ubuntu 26.04.
- [Split relay deployment](deploy/README-DEPLOY.md#模式-2云端分离生产--远程控制): run simulation and relay services on separate hosts. The example MQTT and video services allow anonymous access; use them on a trusted network.
- [Dashboard configuration](dashboard/README.md) · [MQTT interface](dashboard/MQTT-INTERFACE.md) · [Voice interface](dashboard/VOICE-INTERFACE.md) · [Video pipeline](dashboard/VIDEO-LINK.md)

<details>
<summary>Full simulation: setup and launch</summary>

Set up the package sources following the [ROS 2 Lyrical instructions](https://docs.ros.org/en/lyrical/Get-Started/Installation/Ubuntu-Install-Debs.html). On Ubuntu, run the following from the repository root:

```bash
sudo apt install ros-lyrical-desktop ros-lyrical-navigation2 \
  ros-lyrical-slam-toolbox ros-lyrical-ros-gz ros-dev-tools \
  libmosquitto-dev mosquitto mosquitto-clients nlohmann-json3-dev \
  libopencv-dev libx11-dev ffmpeg
source /opt/ros/lyrical/setup.bash
colcon build --symlink-install
source install/setup.bash
```

Follow the [deployment guide](deploy/README-DEPLOY.md#模式-1本地全栈单机) to prepare the `quadruped-ops/` dashboard runtime copy and enable Mosquitto TCP and WebSocket listeners. From the repository root, run:

```bash
./deploy/switch_config.sh local
./deploy/mode1_local_all.sh
```

Open <http://127.0.0.1:8080> and follow the [verification checklist](deploy/README-DEPLOY.md#验证清单) for MQTT telemetry and video. Run `./deploy/stop_all.sh local` to stop. LiDAR avoidance applies to the Nav2 navigation path. The independent close-range stop zone is disabled, and patrol or recovery can still fail.

</details>

[Report an issue](https://github.com/yiqi-7/robot-dog/issues) · [MIT license](LICENSE) · [Third-party notices, including CHAMP](NOTICE)
