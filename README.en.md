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
| Voice | On-robot recording → **Opus compression (measured 12.9:1 – 16:1)** → MQTT uplink → cloud ASR (SenseVoice) → LLM/rule decision → task dispatch + TTS reply |
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
cd /run/media/wby/shuju/Document/robot-dog   # this workspace (later commands use $(pwd))
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
| Raw PCM (112 KB raw) → base64 | 150 KB | ~2.3 s ideal / tens of seconds at 15% loss |
| **Ogg/Opus (measured 5 KB ≈ 11 kbps) → base64** | **7 KB** | **~0.1 s** |

> Units: 112 KB is raw PCM; 150 KB is that stream after base64 (×4/3), which is what actually
> goes on the wire. Same for Opus: 5 KB raw, 7 KB base64. The conservative 12.9:1 – 16:1 range
> is quoted as PCM-to-wire ratio.

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
| Weak-network matrix | 28 of 41 checks passing (six profiles W1–W6: delay/jitter/loss/reorder/outage/combined) |

**What the 13 failures actually are** (each verified against the raw result log):

- **W5/W6 (link outage) are genuine failures**: after the link drops the client does not detect
  the keepalive timeout promptly, and after recovery neither `state` nor the three nodes
  reconnect inside the 25 s window — telemetry stops.
- **4 "task dispatch" failures are false negatives in the checker**: displacement for W2/W3/W4 was
  4.90 / 1.74 / 5.44 m — the robot did drive there, but the checker's "did it start" test reported
  `False` because it only looks at the first velocity frame, which packet loss can drop.
  Displacement is the trustworthy signal.
- **W1 dispatch genuinely did not execute** (0.00 m displacement).
- **Watchdog and node liveness passed 12/12**: across all six profiles `deadman_guard` never
  tripped spuriously and all four nodes stayed online — positive evidence for the safety chain.

---

## Patrol and mapping field results

> These sections carry the negative results too. Patrol is **not** a zero-failure system,
> and the map merge needed a conservative rule to avoid destroying narrow corridors.

### Long-run patrol statistics

From `/tmp/sim_v6.log` (one continuous run, 81.8 minutes at the snapshot):

| Metric | Value |
| --- | --- |
| Waypoints dispatched | 220 |
| Reached | 76 |
| Skipped after retries exhausted | 42 |
| Overall arrival rate | 34.5% |
| Arrival rate over the last 120 log entries | 57% |

**Patrol is not zero-failure.** Failures cluster where the robot stalls within 1.6 m of a shelf
or wall: Nav2 publishes velocity but the gait cannot break traction (joint velocity pinned at
the limit), and the goal aborts with `code=6`. See [Known limitations](#known-limitations).

### Regression after adding unstuck recovery (static merged map)

`patrol_node` now performs an **unstuck maneuver** — back off 1.3 s, then rotate in place 1.6 s —
before each retry. On the same machine:

| Metric | Value |
| --- | --- |
| Loop 1 | **4/5 reached**, all on the first attempt (north corridor / east wall gap / gap north mouth / south of shelves) |
| Only failure | spawn point, `code=6` × 3 attempts — still could not break free after unstuck, skipped |
| Later loops | the robot kept failing at several points after jamming south of the shelves; manual intervention required |

Conclusion: unstuck recovery converts some `code=6` aborts into arrivals (the rescan driver shows
the same effect), but CHAMP can still wedge repeatedly against shelves and corners. This is a
limitation of the gait, not a Nav2 tuning problem — a real fix needs a different gait or a
dedicated push-and-retract recovery primitive.

### Map rescan (east side of the warehouse)

The old `factory_full` map had 18.7% unknown space, concentrated on the east side, with three
points — (5.0,-6.0), (7.0,-5.5), (8.0,-5.5) — failing every plan. The rescan was done in two steps:

1. **Temporarily switch to SLAM mode** (launch without `map:=`) and run an automatic
   plan-then-follow loop (34 target points, route generated by `tools/make_rescan_route.py`).
   Each point is pre-validated with `ComputePathToPose`; on failure or abort it does one unstuck
   maneuver plus one retry before skipping. Bypassing `bt_navigator` and talking to the
   planner/controller directly avoids behaviour-tree activation failures under load.

2. **Conservative merge** of the new and old maps: everything already known in the old map is
   kept; in unknown regions only cells the new map confirms as **free** are absorbed —
   **new obstacle cells are never absorbed**. Reason: occupancy pixels from a fresh session carry
   gait jitter, and in narrow corridors 1% extra obstacle can inflate into a wall at 0.35 m
   inflation. An aggressive merge experiment left 57% of the costmap at 253/254, cutting every
   corridor and trapping the robot in a 1.2 m pocket.

| Metric | Old map | After rescan merge |
| --- | --- | --- |
| Resolution / size | 0.05 m / 404×289 | 0.05 m / 404×289 (frame unchanged) |
| Free | 75.7% | **85.0%** |
| Occupied | 5.6% | 5.6% (no new obstacles) |
| Unknown | 18.7% | **9.4%** |
| Unknown in east warehouse | 39.4% | **24.5%** |

**Unintended but valuable finding**: (7.0,-5.5) and (8.0,-5.5) turned out to be **the shelf bodies
themselves**. They were unknown in the old map, and `track_unknown_space: false` treated unknown
as free — which is why the planner believed they were reachable. They were never valid dwell
points; the adjacent aisle (5.0,-5.4) plans fine. This matters more than "we found new standable
floor": the planner no longer routes through shelving.

> An earlier note claimed "6 loops in 40 minutes, 30/30 arrivals, zero failures". That came from a
> single observation that missed anomalous states and does not match long-run statistics; it is
> corrected here.

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