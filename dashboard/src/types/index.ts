// 前端模拟数据的类型定义（任务数据来自后端数据库，见 contracts/task.ts）

export type RobotStatus = "working" | "idle" | "charging" | "fault";

export const ROBOT_STATUS_LABELS: Record<RobotStatus, string> = {
  working: "作业中",
  idle: "待机",
  charging: "充电中",
  fault: "故障",
};

export interface TrackPoint {
  x: number;
  y: number;
}

export interface Robot {
  id: string;
  name: string;
  model: string;
  status: RobotStatus;
  battery: number;
  x: number;
  y: number;
  /** 像素/ tick */
  speed: number;
  /** 朝向（弧度） */
  heading: number;
  motorTemp: number;
  /** 信号强度 dBm */
  signal: number;
  uptimeH: number;
  currentTask: string | null;
  /** 摄像头画面（0-3，对应 assets/cam 下的图） */
  cam: number;
  trail: TrackPoint[];
  /** 巡航路径与当前目标点下标 */
  route: TrackPoint[];
  wp: number;
}

export type AlarmLevel = "info" | "warning" | "critical";

export const ALARM_LEVEL_LABELS: Record<AlarmLevel, string> = {
  info: "提示",
  warning: "警告",
  critical: "严重",
};

export interface Alarm {
  id: number;
  time: Date;
  level: AlarmLevel;
  source: string;
  message: string;
  acked: boolean;
}

export interface Facility {
  id: string;
  name: string;
  zone: string;
  /** 温度基准值（°C） */
  base: number;
  warnAt: number;
  critAt: number;
  temp: number;
  /** 厂区平面图上的标注位置 */
  mx: number;
  my: number;
}

export type FacilityStatus = "normal" | "warning" | "critical";

export function facilityStatus(f: Facility): FacilityStatus {
  if (f.temp >= f.critAt) return "critical";
  if (f.temp >= f.warnAt) return "warning";
  return "normal";
}

export const FACILITY_STATUS_LABELS: Record<FacilityStatus, string> = {
  normal: "正常",
  warning: "超温预警",
  critical: "严重超温",
};

/** 温控曲线的一帧：label 为时间刻度，其余键为设施 id */
export interface TempFrame {
  label: string;
  [fid: string]: number | string;
}
