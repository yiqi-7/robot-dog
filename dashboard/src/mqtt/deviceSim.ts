/**
 * 设备模拟器（mock 模式的“设备侧”）：
 * 像真实机器人/设施/调度服务一样，向总线发布 MQTT 形态的消息：
 *  - robots/{id}/state      约 0.7s 一帧遥测（米制坐标）
 *  - robots/{id}/camera     2s 一帧 JPEG 图像（dataURL 模拟二进制帧）
 *  - facilities/{id}/state  3.5s 一帧
 *  - event                  阈值/随机告警
 *  - 任务执行引擎：监听 fleet/tasks 与 robots/+/cmd，驱动任务生命周期
 */
import { MAP } from "@/config"
import { TOPICS, topicMatch } from "./topics"
import type { IBus } from "./bus"
import type { CmdMsg, EventMsg, FacilityMsg, RobotStateMsg, TaskMsg } from "./wire"
import { mergeTasksById } from "./wire"
import { DEFAULT_SITE_MAP } from "@/map/site"
import camCharging from "@/assets/cam/charging.png"
import camCorridor from "@/assets/cam/corridor.png"
import camWarehouse from "@/assets/cam/warehouse.png"
import camWorkshop from "@/assets/cam/workshop.png"
import type { AlarmLevel, RobotStatus } from "@/types"

const CAM_IMAGES = [camWorkshop, camWarehouse, camCharging, camCorridor]

const TICK_MS = 700
const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v))

/** 厂区点位 -> 地图像素坐标（1px = 0.5m） */
const SITE_POINT_COORDS: Record<string, { x: number; y: number }> = {
  "车间A-1号产线": { x: 300, y: 235 },
  "车间A-2号产线": { x: 430, y: 235 },
  "车间A-西门": { x: 100, y: 210 },
  "车间B-装配台": { x: 300, y: 520 },
  "车间B-东门": { x: 510, y: 530 },
  "仓库-货架区": { x: 800, y: 180 },
  "仓库-月台": { x: 1050, y: 180 },
  配电室: { x: 995, y: 440 },
  锅炉房: { x: 150, y: 620 },
  "充电区-1号桩": { x: 645, y: 472 },
  "充电区-2号桩": { x: 755, y: 472 },
  中控室: { x: 700, y: 640 },
  厂区北门: { x: 600, y: 40 },
  厂区环路: { x: 1100, y: 700 },
}

/** 地图点位 ID -> 地图像素坐标（对应 {P}/site/map 的米制坐标，1px = 0.5m） */
const SITE_POINT_ID_COORDS: Record<string, { x: number; y: number }> = Object.fromEntries(
  DEFAULT_SITE_MAP.points.map((p) => [p.id, { x: p.x * 2, y: p.y * 2 }]),
)

interface SimRobot {
  id: string
  name: string
  model: string
  status: RobotStatus
  battery: number
  x: number // px
  y: number
  speedPx: number // px/tick
  heading: number
  motorTemp: number
  signal: number
  uptimeH: number
  currentTask: string | null
  cam: number
  route: { x: number; y: number }[]
  wp: number
  activeTaskId: number | null
  taskTarget: { x: number; y: number } | null
  dwellUntil: number
  lastEventAt: Record<string, number>
}

const ROBOT_SEED: SimRobot[] = [
  { id: "QB-01", name: "哨兵", model: "B2-W 工业四足", status: "working", battery: 76, x: 160, y: 170, speedPx: 5.4, heading: 0, motorTemp: 46.2, signal: -58, uptimeH: 132.5, currentTask: "车间A 例行巡检", cam: 0, route: [{ x: 160, y: 170 }, { x: 440, y: 170 }, { x: 440, y: 300 }, { x: 160, y: 300 }], wp: 1, activeTaskId: null, taskTarget: null, dwellUntil: 0, lastEventAt: {} },
  { id: "QB-02", name: "雷霆", model: "B2-W 工业四足", status: "working", battery: 63, x: 50, y: 50, speedPx: 6.6, heading: 0, motorTemp: 49.8, signal: -63, uptimeH: 88.1, currentTask: "转场至厂区北门", cam: 3, route: [{ x: 50, y: 50 }, { x: 1150, y: 50 }, { x: 1150, y: 710 }, { x: 50, y: 710 }], wp: 1, activeTaskId: null, taskTarget: null, dwellUntil: 0, lastEventAt: {} },
  { id: "QB-03", name: "铁卫", model: "B2 工业四足", status: "working", battery: 84, x: 660, y: 170, speedPx: 4.6, heading: 0, motorTemp: 44.5, signal: -52, uptimeH: 210.3, currentTask: "仓库货架区巡检", cam: 1, route: [{ x: 660, y: 170 }, { x: 1040, y: 170 }, { x: 1040, y: 270 }, { x: 660, y: 270 }], wp: 1, activeTaskId: null, taskTarget: null, dwellUntil: 0, lastEventAt: {} },
  { id: "QB-04", name: "巡山", model: "B2 工业四足", status: "fault", battery: 41, x: 440, y: 490, speedPx: 0, heading: Math.PI / 2, motorTemp: 81.6, signal: -71, uptimeH: 305.8, currentTask: "电机过温，等待检修", cam: 0, route: [], wp: 0, activeTaskId: null, taskTarget: null, dwellUntil: 0, lastEventAt: {} },
  { id: "QB-05", name: "磐石", model: "B2-W 工业四足", status: "charging", battery: 34, x: 700, y: 440, speedPx: 0, heading: -Math.PI / 2, motorTemp: 31.4, signal: -49, uptimeH: 156.2, currentTask: "返回 1 号桩充电", cam: 2, route: [], wp: 0, activeTaskId: null, taskTarget: null, dwellUntil: 0, lastEventAt: {} },
  { id: "QB-06", name: "惊蛰", model: "Go2-M 巡检四足", status: "idle", battery: 92, x: 850, y: 625, speedPx: 0, heading: Math.PI, motorTemp: 28.3, signal: -55, uptimeH: 64.7, currentTask: null, cam: 2, route: [], wp: 0, activeTaskId: null, taskTarget: null, dwellUntil: 0, lastEventAt: {} },
]

const FACILITY_SEED: FacilityMsg[] = [
  { facilityId: "f1", name: "1号锅炉", zone: "锅炉房", base: 86, warnAt: 92, critAt: 100, temp: 86.4, x: 75, y: 310, ts: 0 },
  { facilityId: "f2", name: "配电柜 A", zone: "配电室", base: 41, warnAt: 50, critAt: 60, temp: 41.2, x: 497.5, y: 220, ts: 0 },
  { facilityId: "f3", name: "空压站主机", zone: "空压站", base: 55, warnAt: 65, critAt: 75, temp: 55.8, x: 235, y: 330, ts: 0 },
  { facilityId: "f4", name: "冷库", zone: "冷库", base: -18, warnAt: -12, critAt: -8, temp: -17.6, x: 542.5, y: 125, ts: 0 },
  { facilityId: "f5", name: "1号产线", zone: "车间A", base: 33, warnAt: 40, critAt: 48, temp: 33.1, x: 150, y: 117.5, ts: 0 },
  { facilityId: "f6", name: "装配台", zone: "车间B", base: 31, warnAt: 38, critAt: 45, temp: 31.3, x: 150, y: 260, ts: 0 },
  { facilityId: "f7", name: "恒温区", zone: "仓库", base: 22, warnAt: 28, critAt: 33, temp: 22.4, x: 425, y: 97.5, ts: 0 },
  { facilityId: "f8", name: "电池充电柜", zone: "充电区", base: 29, warnAt: 38, critAt: 45, temp: 29.6, x: 330, y: 220, ts: 0 },
]

const ALARM_TEMPLATES: { level: AlarmLevel; source: string; message: string }[] = [
  { level: "warning", source: "1号锅炉", message: "炉体温度接近预警阈值，请关注燃烧工况" },
  { level: "critical", source: "1号锅炉", message: "炉体温度超过临界阈值，立即处理" },
  { level: "warning", source: "QB-04", message: "右前髋关节电机温度偏高，建议停机检查" },
  { level: "critical", source: "QB-04", message: "电机温度 85°C 触发保护性停机" },
  { level: "info", source: "QB-05", message: "电量低于 35%，已自动发起回充任务" },
  { level: "warning", source: "配电柜 A", message: "A 相母排温度上升趋势异常" },
  { level: "info", source: "QB-02", message: "信号强度低于 -70dBm，已切换中继链路" },
  { level: "warning", source: "冷库", message: "库温连续 10 分钟高于 -14°C" },
  { level: "info", source: "恒温区", message: "湿度 68%RH，接近管控上限" },
  { level: "warning", source: "空压站主机", message: "排气温度 63°C，请检查冷却水循环" },
  { level: "info", source: "QB-01", message: "例行巡检完成度 62%，数据回传正常" },
  { level: "warning", source: "电池充电柜", message: "2 号充电回路温升速率偏高" },
]

const MIN = 60_000
let alarmSeq = 1000

/** 历史告警种子（启动时作为 event 补发，时间戳为过去；部分已确认） */
function seedEvents(): EventMsg[] {
  const now = Date.now()
  const seed: [number, AlarmLevel, string, string, boolean][] = [
    [3, "critical", "QB-04", "电机温度 82.4°C 触发过温保护，已原地停机", false],
    [11, "warning", "1号锅炉", "炉体温度 91.2°C，接近预警阈值 92°C", false],
    [18, "warning", "配电柜 A", "A 相母排温度 48.7°C，30 分钟上升 4.2°C", false],
    [26, "info", "QB-05", "电量 34%，自动回充任务已下发", true],
    [34, "warning", "冷库", "库温 -13.1°C，连续高于管控线", false],
    [47, "info", "QB-03", "仓库货架区巡检完成 3/4 区段", true],
    [58, "warning", "电池充电柜", "2 号回路温升 1.8°C/10min，偏高", false],
    [73, "info", "中控室", "夜班交接完成，值守人：王工", true],
    [95, "critical", "空压站主机", "排气温度 74°C 接近临界，已降载运行", true],
    [130, "info", "QB-06", "固件 v2.4.1 OTA 升级完成", true],
    [180, "warning", "恒温区", "湿度 71%RH 超过管控上限，持续 20 分钟", true],
  ]
  return seed.map(([m, level, source, message, acked], i) => ({
    alarmId: i + 1,
    ts: now - m * MIN,
    level,
    source,
    message,
    acked,
  }))
}

export class DeviceSimulator {
  private robots: SimRobot[] = ROBOT_SEED.map((r) => ({ ...r, route: r.route.map((p) => ({ ...p })), lastEventAt: {} }))
  private facilities: FacilityMsg[] = FACILITY_SEED.map((f) => ({ ...f }))
  private tasks: TaskMsg[] = []
  private executing = new Set<number>()
  private timer: ReturnType<typeof setInterval> | null = null
  private tickCount = 0
  private unsub: (() => void) | null = null
  private camCanvas: HTMLCanvasElement | null = null
  private camImages: HTMLImageElement[] = []
  private lastFacilityWarn: Record<string, number> = {}

  constructor(private bus: IBus) {}

  start() {
    // 初始快照 + 历史告警
    const now = Date.now()
    for (const f of this.facilities) this.bus.publish(TOPICS.facilityState(f.facilityId), { ...f, ts: now }, { retain: true })
    for (const r of this.robots) this.publishRobot(r, now)
    for (const e of seedEvents()) this.bus.publish(TOPICS.event(), e)
    this.bus.publish(TOPICS.fleetTasks(), this.tasks, { retain: true })
    this.bus.publish(TOPICS.siteMap(), { ...DEFAULT_SITE_MAP, updatedAt: now }, { retain: true })

    this.unsub = this.bus.subscribe((topic, payload) => {
      if (topic === TOPICS.fleetTasks()) {
        this.adoptTasks(payload as TaskMsg[])
      } else if (topicMatch(`${TOPICS.robotCmd("+")}`, topic)) {
        this.handleCmd(topic.split("/")[3], payload as CmdMsg)
      }
    })

    this.preloadCam()
    this.timer = setInterval(() => this.tick(), TICK_MS)
  }

  stop() {
    if (this.timer) clearInterval(this.timer)
    this.timer = null
    this.unsub?.()
  }

  /* ---------------- 任务引擎 ---------------- */

  private adoptTasks(list: TaskMsg[]) {
    // 按 id + rev 合并，不能整表替换（理由同 trpc.tsx 的订阅回调）：
    // 机器人回写的是全量数组，里面既有它没动过的条目，也可能带着本端更晚的改动，
    // 直接覆盖会把本端更新版本打回旧值。
    this.tasks = mergeTasksById(this.tasks, list.map((t) => ({ ...t })))
    // 撤销联动：任务被撤销时，正在执行它的机器人立即停下
    for (const r of this.robots) {
      if (r.activeTaskId != null) {
        const t = this.tasks.find((x) => x.id === r.activeTaskId)
        if (!t || t.status === "revoked" || t.status === "completed") this.clearTask(r)
      }
    }
  }

  private handleCmd(robotId: string, cmd: CmdMsg) {
    const r = this.robots.find((x) => x.id === robotId)
    if (!r) return
    if (cmd.action === "cancel" && cmd.task && r.activeTaskId === cmd.task.id) {
      this.clearTask(r)
    } else if (cmd.action === "estop") {
      r.status = "fault"
      r.currentTask = "急停触发，等待复位"
      this.clearTask(r)
      this.raiseRobotEvent(r, "critical", "ROBOT-ESTOP", `${r.name}（${r.id}）触发急停，原地停机`)
    } else if (cmd.action === "resume" && r.status === "fault") {
      r.status = "idle"
      r.currentTask = null
      r.motorTemp = Math.min(r.motorTemp, 45)
      this.raiseRobotEvent(r, "info", "ROBOT-RESUME", `${r.name}（${r.id}）已复位，恢复待机`)
    }
  }

  private clearTask(r: SimRobot) {
    r.activeTaskId = null
    r.taskTarget = null
    if (r.status !== "fault" && r.status !== "charging") {
      r.status = r.route.length > 0 ? "working" : "idle"
      r.currentTask = r.route.length > 0 ? r.currentTask : null
      if (r.route.length === 0) r.currentTask = null
    }
  }

  private tryStartTasks(now: number) {
    for (const t of this.tasks) {
      if (t.status !== "published" || this.executing.has(t.id) || !t.robotId) continue
      if (t.mode === "scheduled" && (!t.scheduledAt || t.scheduledAt > now)) continue
      const r = this.robots.find((x) => x.id === t.robotId)
      if (!r || r.status === "fault" || r.activeTaskId != null) continue
      this.beginTask(r, t, now)
    }
  }

  private beginTask(r: SimRobot, t: TaskMsg, now: number) {
    this.executing.add(t.id)
    r.activeTaskId = t.id
    const point =
      (t.target && (SITE_POINT_ID_COORDS[t.target] ?? SITE_POINT_COORDS[t.target])) ||
      (t.type === "charging" ? SITE_POINT_ID_COORDS["charger-1"] : { x: MAP.pxW / 2, y: MAP.pxH / 2 })
    r.taskTarget = { ...point }
    r.status = "working"
    r.currentTask = t.title
    r.dwellUntil = 0
    this.raiseRobotEvent(r, "info", "TASK-START", `${r.name}（${r.id}）开始执行任务「${t.title}」`, now)
  }

  private completeTask(r: SimRobot, now: number) {
    const id = r.activeTaskId
    if (id == null) return
    this.executing.delete(id)
    const t = this.tasks.find((x) => x.id === id)
    if (t && t.status === "published") {
      t.status = "completed"
      t.updatedAt = now
      this.bus.publish(TOPICS.fleetTasks(), this.tasks.map((x) => ({ ...x })), { retain: true })
    }
    this.raiseRobotEvent(r, "info", "TASK-DONE", `${r.name}（${r.id}）完成任务「${t?.title ?? id}」`, now)
    const wasCharging = t?.type === "charging"
    r.activeTaskId = null
    r.taskTarget = null
    if (wasCharging) {
      r.status = "idle"
      r.currentTask = null
    } else {
      r.status = r.route.length > 0 ? "working" : "idle"
      r.currentTask = null
    }
  }

  /* ---------------- 主循环 ---------------- */

  private tick() {
    this.tickCount += 1
    const now = Date.now()

    this.tryStartTasks(now)

    for (const r of this.robots) {
      // 任务移动优先，其次巡航路线
      const target = r.taskTarget ?? (r.status === "working" && r.route.length > 0 ? r.route[r.wp] : null)
      if (target && r.status === "working") {
        // 巡航用自身速度；待机/充电中的机器人出任务时使用默认任务速度
        const spd = r.speedPx > 0 ? r.speedPx : 5.5
        const dx = target.x - r.x
        const dy = target.y - r.y
        const dist = Math.hypot(dx, dy)
        if (dist <= spd) {
          r.x = target.x
          r.y = target.y
          if (r.taskTarget) {
            // 到达任务点
            const t = this.tasks.find((x) => x.id === r.activeTaskId)
            if (t?.type === "charging") {
              r.status = "charging"
              r.taskTarget = null
            } else if (r.dwellUntil === 0) {
              r.dwellUntil = now + 4000
            } else if (now >= r.dwellUntil) {
              this.completeTask(r, now)
            }
          } else {
            r.wp = (r.wp + 1) % r.route.length
          }
        } else {
          r.x += (dx / dist) * spd
          r.y += (dy / dist) * spd
          r.heading = Math.atan2(dy, dx)
        }
      }

      // 充电任务：充满即完成
      if (r.status === "charging" && r.activeTaskId != null && r.battery >= 95) {
        this.completeTask(r, now)
      }

      // 电量/温度/信号漂移（与 zip 原版一致）
      if (r.status === "working") r.battery = clamp(r.battery - 0.006, 5, 100)
      else if (r.status === "charging") r.battery = clamp(r.battery + 0.35, 0, 100)
      else if (r.status === "fault") r.battery = clamp(r.battery - 0.002, 0, 100)

      const tempBase = r.status === "working" ? 47 : r.status === "fault" ? 83 : r.status === "charging" ? 31 : 27
      r.motorTemp = clamp(r.motorTemp + (tempBase - r.motorTemp) * 0.02 + (Math.random() - 0.5) * 0.5, 20, 88)
      r.signal = clamp(r.signal + (Math.random() - 0.5) * 1.5, -82, -45)
      r.uptimeH += TICK_MS / 3600_000

      // 限频告警
      if (r.battery < 20 && r.status !== "charging") this.raiseRobotEvent(r, "warning", "BATT-LOW", `${r.name}（${r.id}）电量低于 20%（${r.battery.toFixed(0)}%）`, now, 120)
      if (r.motorTemp > 78) this.raiseRobotEvent(r, "warning", "MOTOR-HOT", `${r.name}（${r.id}）电机温度 ${r.motorTemp.toFixed(1)}°C`, now, 120)

      this.publishRobot(r, now)
    }

    // 设施遥测（每 5 tick ≈ 3.5s）
    if (this.tickCount % 5 === 0) {
      for (const f of this.facilities) {
        f.temp = +clamp(f.temp + (f.base - f.temp) * 0.03 + (Math.random() - 0.5) * 0.4, f.base - 6, f.base + 8).toFixed(1)
        f.ts = now
        const last = this.lastFacilityWarn[f.facilityId] ?? 0
        if (f.temp >= f.warnAt && now - last > 90_000) {
          this.lastFacilityWarn[f.facilityId] = now
          const level: AlarmLevel = f.temp >= f.critAt ? "critical" : "warning"
          this.publishEvent({
            alarmId: ++alarmSeq, ts: now, level, source: f.name,
            message: `${f.zone} 温度 ${f.temp.toFixed(1)}°C ${level === "critical" ? `超过临界阈值 ${f.critAt}°C` : `达到预警阈值 ${f.warnAt}°C`}`,
          })
        }
        this.bus.publish(TOPICS.facilityState(f.facilityId), { ...f }, { retain: true })
      }
    }

    // 随机事件
    if (this.tickCount % 26 === 0 && Math.random() < 0.45) {
      const tpl = ALARM_TEMPLATES[Math.floor(Math.random() * ALARM_TEMPLATES.length)]
      this.publishEvent({ alarmId: ++alarmSeq, ts: now, level: tpl.level, source: tpl.source, message: tpl.message })
    }

    // 摄像头图像帧（每 3 tick ≈ 2.1s，全机队）
    if (this.tickCount % 3 === 0 && this.camImages.length > 0) {
      for (const r of this.robots) this.emitCameraFrame(r, now)
    }
  }

  private publishRobot(r: SimRobot, now: number) {
    const msg: RobotStateMsg = {
      robotId: r.id,
      name: r.name,
      model: r.model,
      ts: now,
      status: r.status,
      battery: +r.battery.toFixed(1),
      x: +(r.x * 0.5).toFixed(2),
      y: +(r.y * 0.5).toFixed(2),
      speed: +(r.status === "working" ? (r.speedPx > 0 ? r.speedPx : 5.5) * 0.5 / (TICK_MS / 1000) : 0).toFixed(2),
      heading: +r.heading.toFixed(3),
      motorTemp: +r.motorTemp.toFixed(1),
      signal: Math.round(r.signal),
      uptimeH: +r.uptimeH.toFixed(2),
      currentTask: r.currentTask,
      cam: r.cam,
    }
    this.bus.publish(TOPICS.robotState(r.id), msg, { retain: true })
  }

  private publishEvent(e: EventMsg) {
    this.bus.publish(TOPICS.event(), e)
  }

  private raiseRobotEvent(r: SimRobot, level: AlarmLevel, code: string, message: string, now = Date.now(), limitSec = 0) {
    if (limitSec > 0) {
      const last = r.lastEventAt[code] ?? 0
      if (now - last < limitSec * 1000) return
      r.lastEventAt[code] = now
    }
    this.publishEvent({ alarmId: ++alarmSeq, ts: now, level, source: r.id, message })
  }

  /* ---------------- 摄像头帧 ---------------- */

  private preloadCam() {
    this.camCanvas = document.createElement("canvas")
    this.camCanvas.width = 640
    this.camCanvas.height = 360
    for (const src of CAM_IMAGES) {
      const img = new Image()
      img.onload = () => this.camImages.push(img)
      img.src = src
    }
  }

  private emitCameraFrame(r: SimRobot, now: number) {
    const ctx = this.camCanvas?.getContext("2d")
    if (!this.camCanvas || !ctx) return
    const img = this.camImages[r.cam]
    if (!img) return
    const W = this.camCanvas.width
    const H = this.camCanvas.height

    // 运动时的轻微画面抖动
    const amp = r.status === "working" ? 8 : 3
    const jx = (Math.random() - 0.5) * amp
    const jy = (Math.random() - 0.5) * amp * 0.6
    ctx.drawImage(img, jx - 8, jy - 5, W + 16, H + 10)

    // 噪点
    ctx.fillStyle = "rgba(134,239,172,0.05)"
    for (let i = 0; i < 36; i++) ctx.fillRect(Math.random() * W, Math.random() * H, 1.4, 1.4)

    // 机端 OSD 烧录
    ctx.font = "12px monospace"
    ctx.fillStyle = "rgba(187,247,208,0.9)"
    ctx.fillText(`${r.id}-CAM0${r.cam + 1} ${new Date(now).toLocaleTimeString("zh-CN", { hour12: false })}`, 10, 18)
    ctx.fillText(`BAT ${r.battery.toFixed(0)}%  E${(r.x * 0.5).toFixed(1)} N${(r.y * 0.5).toFixed(1)}`, 10, H - 10)

    this.bus.publish(TOPICS.robotCamera(r.id), this.camCanvas.toDataURL("image/jpeg", 0.62))
  }
}
