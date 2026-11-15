import type { AlarmLevel, RobotStatus } from "@/types"
import type { Task, TaskPriority, TaskStatus, TaskMode, TaskType } from "@contracts/task"

/**
 * MQTT 线缆格式（Wire Format）：JSON 可序列化，日期一律 epoch ms，
 * 坐标一律米（厂区 600m×380m，前端按 1px=0.5m 换算为地图像素）。
 */

export interface RobotStateMsg {
  robotId: string
  name: string
  model: string
  ts: number
  status: RobotStatus // working | idle | charging | fault
  battery: number // 0-100
  x: number // 米
  y: number // 米
  speed: number // m/s
  heading: number // 弧度
  motorTemp: number // ℃
  signal: number // dBm
  uptimeH: number
  currentTask: string | null
  cam: number // 云台视角 0-3
}

export interface FacilityMsg {
  facilityId: string
  name: string
  zone: string
  base: number
  warnAt: number
  critAt: number
  temp: number
  x: number // 米
  y: number // 米
  ts: number
}

export interface SiteMapMsg {
  version: number;
  width: number;
  height: number;
  zones: Array<{ id: string; name: string; x: number; y: number; width: number; height: number }>;
  points: Array<{ id: string; name: string; x: number; y: number; kind?: "waypoint" | "charger"; zone?: string }>;
  updatedAt: number;
}

export interface EventMsg {
  alarmId: number
  ts: number
  level: AlarmLevel
  source: string
  message: string
  /** 可选：历史事件可携带已确认状态 */
  acked?: boolean
}

/** 任务对象线上形态（contracts/task.ts Task 的 JSON 版） */
export interface TaskMsg {
  id: number
  title: string
  type: TaskType
  mode: TaskMode
  robotId: string | null
  target: string | null
  priority: TaskPriority
  status: TaskStatus
  scheduledAt: number | null
  description: string | null
  createdAt: number
  updatedAt: number
  /** 任务来源：manual 看板手工 / voice 语音指令；缺省视为 manual */
  source?: "manual" | "voice"
}

/* ---------------- 语音指令 ---------------- */

/** 机器人 -> 语音网关：语音片段（{P}/robots/{id}/audio） */
export interface AudioMsg {
  robotId: string
  ts: number
  format: "opus" | "pcm_s16le"
  sampleRate: number
  channels: number
  /** base64 编码的音频数据 */
  data: string
}

/** 语音确认策略（{P}/voice/policy，retained，看板设置 / 网关执行） */
export interface VoicePolicyMsg {
  /** 各类别是否需要人工确认后才执行 */
  requireConfirm: { move: boolean; inspection: boolean; charging: boolean }
  /** 置信度低于该阈值时，无论类别都强制确认 */
  confidenceThreshold: number
  /** 急停类口令（"停下"等）免确认直接执行 */
  estopDirect: boolean
  /** 是否向机器人回发 TTS 播报 */
  ttsReply: boolean
}

export const DEFAULT_VOICE_POLICY: VoicePolicyMsg = {
  requireConfirm: { move: true, inspection: true, charging: false },
  confidenceThreshold: 0.75,
  estopDirect: true,
  ttsReply: true,
}

/** 语音指令记录（{P}/voice/commands 快照数组的元素） */
export interface VoiceCommandMsg {
  cmdId: string
  /** 拾到语音的机器人 */
  heardBy: string
  /** 实际指派执行的机器人（点名时可能与 heardBy 不同） */
  robotId: string | null
  ts: number
  /** ASR 识别文本（文本注入时为原始输入） */
  transcript: string
  /** 解析出的任务草稿；急停口令时 action 为 "estop"，parsed 为 null */
  parsed: {
    title: string
    type: TaskType
    mode: TaskMode
    target: string | null
    priority: TaskPriority
    scheduledAt: number | null
  } | null
  action: "task" | "estop"
  confidence: number
  engine: "llm" | "rule"
  source: "voice" | "text"
  status: "pending_confirm" | "accepted" | "rejected" | "failed"
  /** failed 原因或补充说明 */
  reason?: string
  /** accepted 后关联的任务 ID */
  taskId?: number
  /** 语音片段回放地址（source=voice 时由网关落盘 WAV，经 server.js 的 /voice-clips/ 提供） */
  audioFile?: string
}

/** 看板 -> 语音网关：确认 / 拒绝（{P}/voice/cmd） */
export interface VoiceCmdActionMsg {
  cmdId: string
  action: "confirm" | "reject"
  ts: number
}

/** 语音网关 -> 机器人：TTS 播报文本（{P}/robots/{id}/tts） */
export interface TtsMsg {
  cmdId: string
  ts: number
  text: string
}

export interface CmdMsg {
  cmdId: string
  ts: number
  action: "assign" | "cancel" | "estop" | "resume"
  task: TaskMsg | null
}

export const taskToMsg = (t: Task): TaskMsg => ({
  ...t,
  scheduledAt: t.scheduledAt ? t.scheduledAt.getTime() : null,
  createdAt: t.createdAt.getTime(),
  updatedAt: t.updatedAt.getTime(),
})

export const taskFromMsg = (m: TaskMsg): Task => ({
  ...m,
  scheduledAt: m.scheduledAt != null ? new Date(m.scheduledAt) : null,
  createdAt: new Date(m.createdAt),
  updatedAt: new Date(m.updatedAt),
})
