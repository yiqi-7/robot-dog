# Robot Dog 🐕 — Quadruped Inspection Robot for Industrial Sites

[中文](README.md) | **English**

A quadruped robot simulation and operations project built on **ROS 2 (Lyrical) + Gazebo (gz sim 10)**.
The robot maps an industrial site, navigates and patrols it, and talks to a browser-based
operations dashboard and a voice-command gateway over **MQTT**, with the robot reaching the
dashboard through a **Tencent Cloud relay** for remote control.

![ROS2](https://img.shields.io/badge/ROS%202-Lyrical-blue)
![Gazebo](https://img.shields.io/badge/gz%20sim-10-orange)
![Platform](https://img.shields.io/badge/platform-Ubuntu%2026.04-green)
![MQTT](https://img.shields.io/badge/MQTT-mosquitto-purple)
![WebRTC](https://img.shields.io/badge/video-WebRTC%2FWHEP-ff69b4)

---

## What it does

| Capability | Implementation |
|---|---|
| Quadruped gait | CHAMP locomotion controller + custom `gait_controller`; trot gait measured at 0.35 m/s |
| Mapping & navigation | SLAM Toolbox online mapping / localization on a prebuilt map + Nav2 planning and control |
| Cloud bridge | 4 single-responsibility nodes: `mqtt_bridge` (telemetry/alarms/LWT), `task_runner` (task dispatch), `voice_link` (voice uplink + TTS), `deadman_guard` (heartbeat watchdog) |
| Vision | `image_sender`: JPEG snapshots over MQTT (fallback) + H.264 push over RTSP→WebRTC (primary) |
| Voice | On-robot recording → **Opus compression (15:1)** → MQTT uplink → cloud ASR (SenseVoice) → LLM/rule decision → task dispatch + TTS reply |
| Dashboard | React + Vite SPA: live telemetry, map, task dispatch, voice commands, video monitoring |
| Safety | Heartbeat watchdog + input-timeout zeroing + collision monitor across a three-stage velocity chain (`cmd_vel_in → cmd_vel → chassis`) |

---

## Two deployment modes

The project supports two topologies — **run everything on one machine first, then move to the cloud**:

| | Mode 1: Local all-in-one | Mode 2: Split (cloud) |
|---|---|---|
| Use case | Reading the code / single-machine demo | Production / remote control |
| ROS 2 | TITAN_V | TITAN_V |
| MQTT + video relay + voice gateway + dashboard | all on TITAN_V | Tencent Cloud `43.172.91.67` |

```bash
cd deploy

# Mode 1: everything on one machine
./switch_config.sh local && ./mode1_local_all.sh

# Mode 2: cloud
./mode2_cloud_side.sh                       # on 43.172.91.67
./mode2_robot_side.sh --host 43.172.91.67   # on TITAN_V

./stop_all.sh local|cloud|robot|all
```

See **[deploy/README-DEPLOY.md](deploy/README-DEPLOY.md)** for the full runbook.

---

## Quick start

### Dependencies

```bash
# ROS 2 Lyrical + Gazebo (gz sim 10)
sudo apt install ros-lyrical-desktop ros-lyrical-navigation2 \
     ros-lyrical-slam-toolbox ros-lyrical-ros-gz
# MQTT
sudo apt install libmosquitto-dev mosquitto mosquitto-clients nlohmann-json3-dev
# Vision / voice
sudo apt install libopencv-dev ffmpeg
```

### Build

```bash
cd ~/ros2_ws          # or this project's workspace
colcon build --symlink-install
source install/setup.bash
```

### Launch (Mode 2, cloud relay)

```bash
ros2 launch quadruped_sim quadruped_gz.launch.py \
  gui:=false rviz:=false nav:=true gait:=champ teleop:=false patrol:=false \
  map:=$(pwd)/maps/factory_full.yaml \
  broker_host:=43.172.91.67 broker_port:=1883
```

### Dashboard

```bash
http://43.172.91.67:8080          # Mode 2
http://127.0.0.1:8080             # Mode 1
```

---

## Repository layout

```
robot-dog/
├── src/
│   ├── champ/                     # CHAMP locomotion controller (vendored + local patches)
│   ├── quadruped_champ_config/    # CHAMP parameter package for this robot
│   ├── quadruped_sim/             # Simulation: model, gait, sensors, Nav2 params
│   ├── quadruped_mqtt_bridge/     # Cloud bridge: 4 nodes + vision + mic capture
│   └── quadruped_explorer/        # Autonomous nav: waypoint patrol, stuck-recovery
├── dashboard/                     # Operations dashboard (React + Vite) + voice gateway
├── deploy/                        # Deployment scripts for both modes + MediaMTX/Mosquitto configs
├── tools/weaknet/                 # Network impairment injection and matrix tests
├── maps/                          # Prebuilt maps (factory_full.yaml, ...)
└── README.md / README.en.md
```

> `quadruped-ops/` is a runtime copy of `dashboard/` (with `node_modules` and 290 MB models).
> It is **not tracked** in git; keep it locally when you need to run Mode 1.

---

## Design notes

### Node decomposition: single responsibility, loose coupling

Originally one large `mqtt_bridge` node handled telemetry, task dispatch, voice and safety.
The problems were concrete: any single failure required restarting all functionality, and large
voice frames could starve the telemetry keepalive.

Splitting into 4 independent nodes gives each its own MQTT connection and failure domain:

| Node | Responsibility | Subscriptions |
|---|---|---|
| `mqtt_bridge` | 1 Hz telemetry, alarms, LWT | none |
| `task_runner` | Consume `cmd` dispatch, drive navigation, write back task state | `robots/+/cmd` |
| `voice_link` | Voice uplink, TTS playback, piper synthesis | `robots/+/tts` |
| `deadman_guard` | Heartbeat monitoring, velocity pass-through and zeroing | `safety/heartbeat` |

### Why voice compression matters

The interface contract specifies `pcm_s16le` + base64 and notes it is "fine over LAN".
Real deployment is **public internet with weak uplink**:

| Approach | 3.6 s of speech | Time on a 512 kbit uplink |
|---|---|---|
| Raw PCM, base64 | 150 KB | ~2.3 s ideal / tens of seconds at 15% loss |
| **Ogg/Opus @ 16 kbps** | **8.2 KB** | **~0.1 s** |

The robot encodes with ffmpeg (falling back to PCM on failure); the cloud gateway decodes and
feeds ASR. Measured compression is **12.9:1 – 16:1** with no loss in recognition quality.

### Video: two channels

- **Primary**: `image_sender` → ffmpeg (H.264, B-frames disabled) → `rtsp://<relay>:8554/cam/<id>` → MediaMTX → browser over WHEP
- **Fallback**: binary JPEG snapshots over MQTT (640×360 / q90 / 1 fps, measured ~17.7 KB per frame)
- The dashboard falls back to snapshots when the primary channel drops, and to a static placeholder if both fail

Details: **[dashboard/VIDEO-LINK.md](dashboard/VIDEO-LINK.md)**.

### Safety: three layers

1. `collision_monitor` — LiDAR collision detection
2. `deadman_guard` — cuts velocity at >3 s heartbeat loss, zeroes at >0.5 s input gap (chassis-level last resort)
3. Robot-side LWT — the broker publishes the offline state if the robot disappears

---

## Measured results

| Metric | Result |
|---|---|
| Trot gait speed | 0.35 m/s (ground truth from `/odom`) |
| Telemetry rate | 1 Hz, ~0.2 KB per frame |
| Camera fallback frame | 640×360, q90, ~17.7 KB, 1 fps |
| Voice compression | 3.6 s: 112 KB PCM → 5 KB Opus → 7 KB base64 |
| Cloud relay RTT | 851–1590 ms (mean 1192 ms, jitter ±304 ms) |
| MQTT reconnect recovery | 10–16 s (`mosquitto_reconnect_delay_set(2, 10)`) |
| Weak-network matrix | 27/28 passing (profiles R1–R5: delay/jitter/loss/reorder/outage) |

---

## Documentation

| Document | Contents |
|---|---|
| [deploy/README-DEPLOY.md](deploy/README-DEPLOY.md) | Full runbook for both deployment modes |
| [dashboard/VIDEO-LINK.md](dashboard/VIDEO-LINK.md) | Video link topology, configuration, measurements, troubleshooting |
| [dashboard/MQTT-INTERFACE.md](dashboard/MQTT-INTERFACE.md) | MQTT topics and payload contracts |
| [dashboard/VOICE-INTERFACE.md](dashboard/VOICE-INTERFACE.md) | Voice contract (uplink / downlink / confirmation policy) |
| [tools/weaknet/README.md](tools/weaknet/README.md) | Network impairment injection and matrix testing |
| [deploy/mediamtx-install.md](deploy/mediamtx-install.md) | MediaMTX installation and configuration |

---

## Known limitations

- Simulation only — not validated on physical quadruped hardware
- Dashboard task IDs are allocated as `max(existing)+1`, so concurrent operators can collide
- MediaMTX and MQTT currently allow anonymous access; authentication is required before exposing them publicly
- Voice uplink requires ffmpeg + libopus on the robot
- Mode 1 has no MediaMTX by default, so video degrades to MQTT-JPEG snapshots

---

## License

Project code for learning and engineering practice. `src/champ/` is a vendored upstream
project (BSD-3-Clause); `dashboard/` is the companion operations dashboard.