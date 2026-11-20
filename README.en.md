# Robot Dog

Run a quadruped inspection robot in a simulated factory with ROS 2 and Gazebo. The simulation supports mapping, localization and Nav2 waypoint patrols. A browser dashboard displays position, telemetry, alerts and video, and lets you dispatch tasks.

[简体中文](README.md) · [Deployment guide (Chinese)](deploy/README-DEPLOY.md) · [Report an issue](https://github.com/yiqi-7/robot-dog/issues)

## Try the dashboard

You need Git and Node.js 22.12 or later. The dashboard can run on built-in sample data without ROS 2, a robot or an MQTT broker.

```bash
git clone https://github.com/yiqi-7/robot-dog.git
cd robot-dog/dashboard
npm ci
```

In [public/qbot-config.js](dashboard/public/qbot-config.js), change `mqtt.mode` from `"real"` to `"mock"`. Then run:

```bash
npm run dev
```

Open <http://localhost:3000>. The fleet list, factory map and “内置模拟” (simulation) footer confirm that the sample data is active. Try the task and alert pages from there.

## Run the full simulation

The full simulation uses Ubuntu 26.04, ROS 2 Lyrical, Gazebo Sim 10, CHAMP and Nav2. Follow the [local deployment guide](deploy/README-DEPLOY.md#模式-1本地全栈单机) to prepare the ROS workspace, dashboard runtime copy and Mosquitto. After building and sourcing the workspace, run from the repository root:

```bash
./deploy/switch_config.sh local
./deploy/mode1_local_all.sh
```

Open <http://127.0.0.1:8080> for live telemetry. Check MQTT messages and video with the [verification checklist](deploy/README-DEPLOY.md#验证清单). Run `./deploy/stop_all.sh local` to stop the services. A [split relay deployment](deploy/README-DEPLOY.md#模式-2云端分离生产--远程控制) has separate setup instructions.

<details>
<summary>ROS workspace setup commands</summary>

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

</details>

## Interfaces and limitations

[Dashboard configuration](dashboard/README.md) · [MQTT interface](dashboard/MQTT-INTERFACE.md) · [Voice interface](dashboard/VOICE-INTERFACE.md) · [Video pipeline](dashboard/VIDEO-LINK.md)

Validation has been limited to simulation. LiDAR avoidance applies to the Nav2 navigation path. The independent close-range stop zone is disabled, and patrol or recovery can still fail. The example MQTT and video services allow anonymous access. Use them on a trusted network.

Project code is licensed under [MIT](LICENSE). CHAMP retains its upstream BSD-3-Clause license. See [NOTICE](NOTICE) for third-party attribution.
