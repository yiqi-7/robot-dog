import { MQTT_CONFIG } from "@/config"

const P = MQTT_CONFIG.topicPrefix

/**
 * 主题约定（详见项目根目录 MQTT-INTERFACE.md）：
 *
 *  {P}/robots/{robotId}/state       retained 1Hz    机器人遥测
 *  {P}/robots/{robotId}/camera      0.5-2fps        摄像头图像帧（二进制 JPEG）
 *  {P}/robots/{robotId}/cmd                          看板 -> 机器人 指令
 *  {P}/robots/{robotId}/audio                        机器人 -> 语音网关 语音片段
 *  {P}/robots/{robotId}/tts                          语音网关 -> 机器人 TTS 播报文本
 *  {P}/fleet/tasks                  retained         任务列表全量快照
 *  {P}/facilities/{facilityId}/state retained ≤5s    设施遥测
 *  {P}/site/map retained                             厂区地图配置（JSON）
 *  {P}/voice/commands               retained         语音指令全量快照（语音网关 -> 看板）
 *  {P}/voice/cmd                                     看板 -> 语音网关 确认/拒绝
 *  {P}/voice/policy                 retained         语音确认策略（看板设置，网关执行）
 *  {P}/voice/text                                    测试注入：纯文本当语音指令处理
 *  {P}/event                                         告警/事件统一上报
 *  {P}/alarms/{alarmId}/ack                          告警确认回执
 */
export const TOPICS = {
  robotState: (id: string) => `${P}/robots/${id}/state`,
  robotCamera: (id: string) => `${P}/robots/${id}/camera`,
  robotCmd: (id: string) => `${P}/robots/${id}/cmd`,
  robotAudio: (id: string) => `${P}/robots/${id}/audio`,
  robotTts: (id: string) => `${P}/robots/${id}/tts`,
  fleetTasks: () => `${P}/fleet/tasks`,
  facilityState: (id: string) => `${P}/facilities/${id}/state`,
  siteMap: () => `${P}/site/map`,
  voiceCommands: () => `${P}/voice/commands`,
  voiceCmd: () => `${P}/voice/cmd`,
  voicePolicy: () => `${P}/voice/policy`,
  voiceText: () => `${P}/voice/text`,
  event: () => `${P}/event`,
  alarmAck: (alarmId: number | string) => `${P}/alarms/${alarmId}/ack`,
}

/** 看板订阅的通配主题 */
export const SUBSCRIPTIONS = [
  `${P}/robots/+/state`,
  `${P}/robots/+/camera`,
  `${P}/fleet/tasks`,
  `${P}/facilities/+/state`,
  `${P}/site/map`,
  `${P}/voice/commands`,
  `${P}/voice/policy`,
  `${P}/event`,
]

export function parseTopic(topic: string):
  | { kind: "robotState" | "robotCmd" | "robotCamera"; robotId: string }
  | { kind: "fleetTasks" }
  | { kind: "siteMap" }
  | { kind: "voiceCommands" }
  | { kind: "voicePolicy" }
  | { kind: "facilityState"; facilityId: string }
  | { kind: "event" }
  | { kind: "alarmAck"; alarmId: string }
  | null {
  const seg = topic.split("/")
  const root = seg.slice(2) // 前两段为前缀（如 qbot/v1）
  if (root[0] === "robots" && root.length === 3) {
    const kind =
      root[2] === "state"
        ? "robotState"
        : root[2] === "cmd"
          ? "robotCmd"
          : root[2] === "camera"
            ? "robotCamera"
            : null
    return kind ? ({ kind, robotId: root[1] } as never) : null
  }
  if (root[0] === "fleet" && root[1] === "tasks") return { kind: "fleetTasks" }
  if (root[0] === "site" && root[1] === "map") return { kind: "siteMap" }
  if (root[0] === "voice" && root[1] === "commands") return { kind: "voiceCommands" }
  if (root[0] === "voice" && root[1] === "policy") return { kind: "voicePolicy" }
  if (root[0] === "facilities" && root[2] === "state")
    return { kind: "facilityState", facilityId: root[1] }
  if (root[0] === "event") return { kind: "event" }
  if (root[0] === "alarms" && root[2] === "ack") return { kind: "alarmAck", alarmId: root[1] }
  return null
}

/** MQTT 主题通配符匹配（+ 单层，# 多层） */
export function topicMatch(filter: string, topic: string): boolean {
  const f = filter.split("/")
  const t = topic.split("/")
  for (let i = 0; i < f.length; i++) {
    if (f[i] === "#") return true
    if (t[i] === undefined) return false
    if (f[i] !== "+" && f[i] !== t[i]) return false
  }
  return f.length === t.length
}
