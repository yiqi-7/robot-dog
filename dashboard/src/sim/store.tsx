import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import type { Alarm, Facility, Robot, TempFrame, TrackPoint } from "@/types";
import { MQTT_CONFIG } from "@/config";
import { parseTopic, TOPICS } from "@/mqtt/topics";
import type { BusStatus, IBus } from "@/mqtt/bus";
import { MqttBus } from "@/mqtt/MqttBus";
import { MockBus } from "@/mqtt/MockBus";
import { DeviceSimulator } from "@/mqtt/deviceSim";
import type { EventMsg, FacilityMsg, RobotStateMsg, SiteMapMsg, VoiceCommandMsg, VoicePolicyMsg, CmdMsg, TaskMsg } from "@/mqtt/wire";
import { DEFAULT_VOICE_POLICY } from "@/mqtt/wire";
import { DEFAULT_SITE_MAP } from "@/map/site";

/* ---------------- 说明 ----------------
 * 看板状态全部来自 MQTT 总线（mock 模式 = 内置回环 Broker + 设备模拟器）。
 * 本文件只负责：订阅总线 -> 维护前端视图模型（Robot/Facility/Alarm/TempFrame）。
 * -------------------------------------- */

const MIN = 60_000;

/** 演示用：生成过去 24h（30 分钟一格）的温度背景曲线 */
function seedTempHistory(facilities: Facility[]): TempFrame[] {
  const frames: TempFrame[] = [];
  const now = Date.now();
  for (let i = 47; i >= 0; i--) {
    const t = new Date(now - i * 30 * MIN);
    const label = `${String(t.getHours()).padStart(2, "0")}:${String(t.getMinutes()).padStart(2, "0")}`;
    const frame: TempFrame = { label };
    for (const f of facilities) {
      const diurnal = Math.sin((t.getHours() / 24) * Math.PI * 2 - 1.2) * 1.6;
      const noise = (Math.random() - 0.5) * 1.4;
      frame[f.id] = +(f.base + diurnal + noise).toFixed(1);
    }
    frames.push(frame);
  }
  return frames;
}

function timeLabel(d: Date) {
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
}

/** 遥测消息 -> 前端机器人视图模型（米 -> 像素） */
function msgToRobot(prev: Robot | undefined, m: RobotStateMsg): Robot {
  const x = m.x * 2;
  const y = m.y * 2;
  let trail: TrackPoint[] = prev?.trail ?? [];
  const last = trail[trail.length - 1];
  if (!last || Math.hypot(x - last.x, y - last.y) > 2) {
    trail = [...trail.slice(-60), { x, y }];
  }
  return {
    id: m.robotId,
    name: m.name,
    model: m.model,
    status: m.status,
    battery: m.battery,
    x,
    y,
    speed: m.speed / 0.18, // 页面按 speed*0.18 显示 m/s
    heading: m.heading,
    motorTemp: m.motorTemp,
    signal: m.signal,
    uptimeH: m.uptimeH,
    currentTask: m.currentTask,
    cam: m.cam,
    trail,
    route: prev?.route ?? [],
    wp: prev?.wp ?? 0,
  };
}

const PLACEHOLDER_ROBOT: Robot = {
  id: "--", name: "等待接入", model: "-", status: "idle", battery: 0,
  x: 0, y: 0, speed: 0, heading: 0, motorTemp: 0, signal: -99,
  uptimeH: 0, currentTask: null, cam: 0, trail: [], route: [], wp: 0,
};

interface SimState {
  now: Date;
  robots: Robot[];
  facilities: Facility[];
  temps: TempFrame[];
  alarms: Alarm[];
  /** 各机器人最新摄像头图像帧（MQTT camera 主题） */
  frames: Record<string, { url: string; ts: number }>;
  busStatus: BusStatus;
  busLabel: string;
  bus: IBus;
  selectedId: string;
  selectRobot: (id: string) => void;
  ackAlarm: (id: number) => void;
  ackAll: () => void;
  selectedRobot: Robot;
  siteMap: SiteMapMsg;
  /** 语音指令快照（语音网关发布，按时间倒序） */
  voiceCommands: VoiceCommandMsg[];
  voicePolicy: VoicePolicyMsg;
  /** 确认 / 拒绝一条待确认语音指令 */
  confirmVoice: (cmdId: string, approved: boolean) => void;
  /** 更新语音确认策略（retained 持久化到 Broker） */
  setVoicePolicy: (p: VoicePolicyMsg) => void;
  /** 向机器人下发指令（assign / cancel / estop / resume） */
  sendRobotCmd: (id: string, action: CmdMsg["action"], task?: TaskMsg | null) => void;
}

const SimContext = createContext<SimState | null>(null);

export function SimProvider({ children }: { children: ReactNode }) {
  const [now, setNow] = useState(() => new Date());
  const [robotsMap, setRobotsMap] = useState<Record<string, Robot>>({});
  const [facilitiesMap, setFacilitiesMap] = useState<Record<string, Facility>>({});
  const [temps, setTemps] = useState<TempFrame[]>([]);
  const [alarms, setAlarms] = useState<Alarm[]>([]);
  const [frames, setFrames] = useState<Record<string, { url: string; ts: number }>>({});
  const [siteMap, setSiteMap] = useState<SiteMapMsg>(DEFAULT_SITE_MAP);
  const [voiceCommands, setVoiceCommands] = useState<VoiceCommandMsg[]>([]);
  const [voicePolicy, setVoicePolicyState] = useState<VoicePolicyMsg>(DEFAULT_VOICE_POLICY);
  const [busStatus, setBusStatus] = useState<BusStatus>("connecting");
  const [selectedId, setSelectedId] = useState("QB-01");

  const busRef = useRef<IBus | null>(null);
  if (!busRef.current) busRef.current = MQTT_CONFIG.mode === "real" ? new MqttBus() : new MockBus();
  const simRef = useRef<DeviceSimulator | null>(null);
  const facilitiesRef = useRef<Record<string, Facility>>({});
  const tempSeededRef = useRef(false);

  useEffect(() => {
    facilitiesRef.current = facilitiesMap;
  }, [facilitiesMap]);

  useEffect(() => {
    const bus = busRef.current!;
    if (MQTT_CONFIG.mode === "mock") {
      simRef.current = new DeviceSimulator(bus);
    }
    const offStatus = bus.onStatus(setBusStatus);
    const offMsg = bus.subscribe((topic, payload) => {
      const parsed = parseTopic(topic);
      if (!parsed) return;
      switch (parsed.kind) {
        case "siteMap": setSiteMap(payload as SiteMapMsg); break;
        case "voiceCommands": setVoiceCommands(payload as VoiceCommandMsg[]); break;
        case "voicePolicy": {
          const p = payload as Partial<VoicePolicyMsg>;
          setVoicePolicyState({
            ...DEFAULT_VOICE_POLICY,
            ...p,
            requireConfirm: { ...DEFAULT_VOICE_POLICY.requireConfirm, ...(p.requireConfirm ?? {}) },
          });
          break;
        }
        case "robotState": {
          const m = payload as RobotStateMsg;
          setRobotsMap((prev) => ({ ...prev, [m.robotId]: msgToRobot(prev[m.robotId], m) }));
          break;
        }
        case "robotCamera": {
          const url = payload as string;
          setFrames((prev) => ({ ...prev, [parsed.robotId]: { url, ts: Date.now() } }));
          break;
        }
        case "facilityState": {
          const m = payload as FacilityMsg;
          const f: Facility = {
            id: m.facilityId, name: m.name, zone: m.zone, base: m.base,
            warnAt: m.warnAt, critAt: m.critAt, temp: m.temp, mx: m.x * 2, my: m.y * 2,
          };
          setFacilitiesMap((prev) => ({ ...prev, [f.id]: f }));
          break;
        }
        case "event": {
          const e = payload as EventMsg;
          const a: Alarm = { id: e.alarmId, time: new Date(e.ts), level: e.level, source: e.source, message: e.message, acked: e.acked ?? false };
          setAlarms((prev) => [...prev, a].sort((x, y) => y.time.getTime() - x.time.getTime()).slice(0, 60));
          break;
        }
      }
    });

    bus.start();
    simRef.current?.start();

    // 时钟 + 温控曲线推进
    const clock = setInterval(() => setNow(new Date()), 1000);
    const tempTimer = setInterval(() => {
      const facs = Object.values(facilitiesRef.current);
      if (facs.length === 0) return;
      // 首帧：mock 模式补 24h 背景曲线；real 模式以当前值平铺
      if (!tempSeededRef.current) {
        tempSeededRef.current = true;
        if (MQTT_CONFIG.mode === "mock") {
          setTemps(seedTempHistory(facs));
          return;
        }
        const frame: TempFrame = { label: timeLabel(new Date()) };
        for (const f of facs) frame[f.id] = f.temp;
        setTemps([frame]);
        return;
      }
      setTemps((prev) => {
        if (prev.length === 0) return prev;
        const next = [...prev];
        const last = { ...next[next.length - 1] };
        for (const f of facs) last[f.id] = f.temp;
        next[next.length - 1] = last;
        return next;
      });
    }, 5000);
    // 每 ~28s 推进一格（演示节奏；真实部署由后端按 30min 聚合）
    const frameTimer = setInterval(() => {
      const facs = Object.values(facilitiesRef.current);
      if (facs.length === 0 || !tempSeededRef.current) return;
      setTemps((prev) => {
        if (prev.length === 0) return prev;
        const frame: TempFrame = { label: timeLabel(new Date()) };
        for (const f of facs) frame[f.id] = f.temp;
        return [...prev.slice(1), frame];
      });
    }, 28_000);

    return () => {
      offStatus();
      offMsg();
      clearInterval(clock);
      clearInterval(tempTimer);
      clearInterval(frameTimer);
      simRef.current?.stop();
      bus.stop();
    };
  }, []);

  const selectRobot = useCallback((id: string) => setSelectedId(id), []);
  const ackAlarm = useCallback((id: number) => {
    busRef.current?.publish(TOPICS.alarmAck(id), { alarmId: id, ts: Date.now(), acked: true });
    setAlarms((prev) => prev.map((a) => (a.id === id ? { ...a, acked: true } : a)));
  }, []);
  const ackAll = useCallback(() => {
    setAlarms((prev) => {
      for (const a of prev) {
        if (!a.acked) busRef.current?.publish(TOPICS.alarmAck(a.id), { alarmId: a.id, ts: Date.now(), acked: true });
      }
      return prev.map((a) => ({ ...a, acked: true }));
    });
  }, []);
  const confirmVoice = useCallback((cmdId: string, approved: boolean) => {
    busRef.current?.publish(TOPICS.voiceCmd(), { cmdId, action: approved ? "confirm" : "reject", ts: Date.now() });
  }, []);
  const setVoicePolicy = useCallback((p: VoicePolicyMsg) => {
    setVoicePolicyState(p);
    busRef.current?.publish(TOPICS.voicePolicy(), p, { retain: true });
  }, []);
  const sendRobotCmd = useCallback((id: string, action: CmdMsg["action"], task: TaskMsg | null = null) => {
    busRef.current?.publish(TOPICS.robotCmd(id), {
      cmdId: `CMD-${Date.now().toString(36)}`,
      ts: Date.now(),
      action,
      task,
    });
  }, []);

  const robots = useMemo(
    () => Object.values(robotsMap).sort((a, b) => a.id.localeCompare(b.id)),
    [robotsMap],
  );
  const facilities = useMemo(
    () => Object.values(facilitiesMap).sort((a, b) => a.id.localeCompare(b.id)),
    [facilitiesMap],
  );

  const value = useMemo<SimState>(() => {
    const selectedRobot = robots.find((r) => r.id === selectedId) ?? robots[0] ?? PLACEHOLDER_ROBOT;
    return {
      now,
      robots,
      facilities,
      temps,
      alarms,
      frames,
      busStatus,
      busLabel: busRef.current?.label ?? "",
      bus: busRef.current!,
      selectedId,
      selectRobot,
      ackAlarm,
      ackAll,
      selectedRobot,
      siteMap,
      voiceCommands,
      voicePolicy,
      confirmVoice,
      setVoicePolicy,
      sendRobotCmd,
    };
  }, [now, robots, facilities, temps, alarms, frames, busStatus, selectedId, siteMap, voiceCommands, voicePolicy, selectRobot, ackAlarm, ackAll, confirmVoice, setVoicePolicy, sendRobotCmd]);

  return <SimContext.Provider value={value}>{children}</SimContext.Provider>;
}

export function useSim() {
  const ctx = useContext(SimContext);
  if (!ctx) throw new Error("useSim 必须在 SimProvider 内使用");
  return ctx;
}
