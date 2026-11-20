# Robot Dog: ROS 2 Quadruped Inspection Simulation

[中文](README.md) | **English**

A quadruped inspection robot simulation built with **ROS 2 Lyrical, Gazebo Sim 10, CHAMP, and Nav2**. The repository includes the robot model, mapping/localization/navigation, waypoint patrol, MQTT telemetry and task interfaces, voice pipeline, video transport, and a browser-based operations dashboard.

[Quick start](#quick-start-mode-1) · [Deployment guide](deploy/README-DEPLOY.md) · [Report an issue](https://github.com/yiqi-7/robot-dog/issues)

> This project is validated primarily in simulation and has not been validated on physical quadruped hardware. “Obstacle avoidance” here means LiDAR/costmap avoidance while navigating through Nav2; it is not an independent emergency-stop guarantee for every motion mode.

## Capabilities

| Subsystem | Current implementation |
|---|---|
| Locomotion | CHAMP; no in-house gait controller is included |
| Mapping and navigation | SLAM Toolbox, map-based localization, Nav2; runtime `/scan` obstacle layers |
| Tasks and patrol | MQTT dispatch, Nav2 waypoint patrol, retry and unstuck behavior |
| Telemetry and safety | MQTT state/alarms, heartbeat watchdog, command-velocity timeout zeroing |
| Voice | Robot-side capture and Opus uplink; ASR/TTS gateway; models are obtained separately |
| Video | MQTT-JPEG fallback; optional RTSP / MediaMTX / WebRTC (WHEP) primary stream |
| Dashboard | React + Vite single-page app for telemetry, map, tasks, alarms, and video |

### Obstacle avoidance and safety boundary

Both local and global Nav2 costmaps subscribe to `/scan` and enable an obstacle layer. The navigation controller can plan and adjust motion around observed obstacles. The `collision_monitor` node is configured, but its `polygon_stop` zone is currently disabled; do not describe it as an active close-range collision emergency stop. Motion commands that bypass Nav2 are not protected by Nav2 costmaps. Physical deployments still require an independent emergency stop and safety validation.

## Deployment modes

| Mode | Use | Components |
|---|---|---|
| Mode 1: local all-in-one | Development, simulation, single-host demo | ROS 2/Gazebo, MQTT, dashboard, and voice gateway on one host; without MediaMTX, video uses JPEG snapshots |
| Mode 2: split relay | Remote-access demo/deployment | Robot host runs simulation and ROS 2 nodes; a relay server runs MQTT, dashboard, voice gateway, and optional MediaMTX |

Real addresses are not stored in this repository. For Mode 2, create a local config on each relevant host:

```bash
cp deploy/qbot.env.example deploy/qbot.env
# Edit deploy/qbot.env; set CLOUD_HOST and, for NAT, PUBLIC_IP
```

`deploy/qbot.env` is git-ignored. See the [deployment guide](deploy/README-DEPLOY.md) for prerequisites, service setup, ports, and troubleshooting. MQTT and video examples currently have no authentication; **do not expose them directly to untrusted networks**.

## Quick start: Mode 1

Target environment: Ubuntu 26.04, ROS 2 Lyrical, and Gazebo Sim 10. Mode 1 also requires Node.js 22.12 or later and npm. Install dependencies and source ROS:

```bash
sudo apt install ros-lyrical-desktop ros-lyrical-navigation2 \
  ros-lyrical-slam-toolbox ros-lyrical-ros-gz \
  libmosquitto-dev mosquitto mosquitto-clients nlohmann-json3-dev \
  libopencv-dev libx11-dev ffmpeg
source /opt/ros/lyrical/setup.bash
```

Build the ROS workspace:

```bash
colcon build --symlink-install
source install/setup.bash
```

The Mode 1 script also requires the ignored `quadruped-ops/` runtime copy (dashboard dependencies, build output, and optional models):

```bash
cp -a dashboard quadruped-ops
cd quadruped-ops
npm ci
npm run build
cd ..
```

Mosquitto must listen on TCP `1883` and WebSocket `8083`. Install the repository configuration and restart it:

```bash
sudo cp deploy/mosquitto-websockets.conf /etc/mosquitto/conf.d/websockets.conf
sudo systemctl restart mosquitto
```

Start and stop:

```bash
./deploy/switch_config.sh local
./deploy/mode1_local_all.sh
# Stop local mode
./deploy/stop_all.sh local
```

Nav2, CHAMP, patrol, and `voice_source` are enabled by default. Pass `voice:=false` to the launch file on hosts without a microphone. See the [deployment guide](deploy/README-DEPLOY.md) for script options and complete setup. Open `http://127.0.0.1:8080` after startup.

Verify that the dashboard responds:

```bash
curl -fsS -o /dev/null -w 'HTTP %{http_code}\n' http://127.0.0.1:8080/
```

On success, this prints `HTTP 200`. For end-to-end robot telemetry and video checks, see the [deployment verification checklist](deploy/README-DEPLOY.md#验证清单).

## Repository layout

```text
src/champ/                     Upstream CHAMP controller
src/quadruped_champ_config/    Project CHAMP parameters and model adaptation
src/quadruped_sim/             Gazebo model, launch, and Nav2 configuration
src/quadruped_explorer/        Waypoint patrol and recovery logic
src/quadruped_mqtt_bridge/     MQTT, task, voice, video, and safety nodes
dashboard/                     Dashboard source, voice gateway, and interface docs
deploy/                        Local/cloud scripts and service configuration
maps/                          Example maps
tools/weaknet/                 Weak-network test tools
```

## Tests and status

Build and test in a ROS 2 environment:

```bash
colcon build --symlink-install
colcon test
colcon test-result --verbose
```

Dashboard tests:

```bash
cd dashboard
npm ci
npm test
npm run build
```

Existing simulation verification covers 8 ROS packages, 29 C++ tests, and the dashboard test suite. These results are not physical-hardware acceptance. Long simulation patrols still encounter Nav2 planning/execution failures and gait stalls; unattended operation is not reliable.

## Documentation

- [Deployment guide](deploy/README-DEPLOY.md)
- [Dashboard guide](dashboard/README.md)
- [MQTT interface](dashboard/MQTT-INTERFACE.md)
- [Voice interface](dashboard/VOICE-INTERFACE.md)
- [Video pipeline](dashboard/VIDEO-LINK.md)
- [CHAMP config package](src/quadruped_champ_config/README.md)
- [MQTT bridge package](src/quadruped_mqtt_bridge/README.md)
- [Weak-network testing](tools/weaknet/README.md)
- [License and third-party notices](LICENSE) / [NOTICE](NOTICE)

## Known limitations

- Simulation only; real hardware, terrain contact, and sensor reliability have not been validated.
- Nav2 costmap avoidance applies only to the Nav2 command path; the `collision_monitor` stop polygon is disabled.
- Patrol and recovery can fail or stall; long unattended runs are unreliable.
- MQTT/MediaMTX examples allow anonymous access; configure authentication, access controls, and TLS before public deployment.
- ASR and some runtime models are not distributed in this repository; obtain them as documented and follow upstream licenses.
- Mode 1 uses MQTT-JPEG by default; smooth WebRTC video requires a separate MediaMTX deployment.

## License

Repository-owned code is released under [MIT](LICENSE). `src/champ/` retains the upstream CHAMP BSD-3-Clause license; see [NOTICE](NOTICE) for third-party attribution.
