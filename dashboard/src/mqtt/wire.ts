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
  /** 单条任务的版本号（跨端合并用，缺省视为 0），语义见下方 taskRevOf */
  rev?: number
  /** 终态原因（如「Nav2 未能到达目标点」「已取消」），仅机器人回写终态时携带 */
  statusNote?: string
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

/* ---------------- 任务合并协议 ---------------- */
/**
 * 任务版本号 rev 的语义（跨端合并协议，C++ 侧 task_runner_tasks.cpp、
 * 网关侧 voice-gateway.js 与看板三处必须一致）：
 *   · rev 是【单条任务对象】的单调递增整数版本号，字段缺省视为 0；
 *   · 谁改了这条任务，就把这条的 rev 改成「已知较大 rev + 1」；
 *   · 两端同一条任务不一致时【以 rev 大者为准】，rev 相等即同一版本；
 *   · 老版本不带 rev 的消息按 0 处理，保证向前兼容。
 */
export const taskRevOf = (t: { rev?: number }): number => {
  const n = Number(t.rev)
  return Number.isFinite(n) && n > 0 ? n : 0
}

/** 本端改动一条任务后调用：把 rev 提到「已知较大值 + 1」，否则会被别端回写打回 */
export const bumpTaskRev = <T extends { rev?: number }>(t: T): T => ({
  ...t,
  rev: taskRevOf(t) + 1,
})

/**
 * 按 id + rev 合并两份任务数组（base 为基准，patch 为增量）。
 * base 顺序不变；patch 里 base 没有的新 id 排到最前（沿用「新任务在前」语义）。
 *
 * 为什么必须合并而不能整表替换：fleet/tasks 是 retained 全量数组，机器人、
 * 语音网关、看板三方都往里写。任一端收到消息就整表覆盖本地状态，
 * 「收到快照 → 状态落地」这段时间内别端新增或更新的任务就会静默消失。
 */
export function mergeTasksById<T extends { id: number; rev?: number }>(
  base: T[],
  patch: T[],
): T[] {
  const out = Array.isArray(base) ? base.slice() : []
  const index = new Map<string, number>()
  out.forEach((t, i) => index.set(String(t.id), i))
  const added: T[] = []
  for (const p of Array.isArray(patch) ? patch : []) {
    const key = String(p.id)
    const i = index.get(key)
    if (i === undefined) {
      added.push(p)
      index.set(key, -1) // 防止 patch 内同 id 重复插入
      continue
    }
    if (i >= 0 && taskRevOf(p) > taskRevOf(out[i])) out[i] = p
  }
  return added.length ? added.concat(out) : out
}
