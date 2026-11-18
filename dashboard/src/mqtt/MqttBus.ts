import mqtt from "mqtt"
import { MQTT_CONFIG } from "@/config"
import { SUBSCRIPTIONS } from "./topics"
import type { BusStatus, IBus, MessageHandler, StatusHandler } from "./bus"

/** 浏览器环境二进制 -> dataURL（分块避免栈溢出） */
function binaryToDataUrl(data: Uint8Array): string {
  let bin = ""
  const CHUNK = 0x8000
  for (let i = 0; i < data.length; i += CHUNK) {
    bin += String.fromCharCode(...data.subarray(i, i + CHUNK))
  }
  return `data:image/jpeg;base64,${btoa(bin)}`
}

/** 真实 MQTT Broker 接入（WebSocket） */
export class MqttBus implements IBus {
  readonly label = MQTT_CONFIG.brokerUrl
  private client: mqtt.MqttClient | null = null
  private handlers = new Set<MessageHandler>()
  private statusHandlers = new Set<StatusHandler>()
  private status: BusStatus = "disconnected"

  start() {
    this.setStatus("connecting")
    this.client = mqtt.connect(MQTT_CONFIG.brokerUrl, {
      username: MQTT_CONFIG.username || undefined,
      password: MQTT_CONFIG.password || undefined,
      reconnectPeriod: MQTT_CONFIG.reconnectPeriod,
      clientId: `dashboard-${Math.random().toString(16).slice(2, 10)}`,
      clean: true,
    })
    this.client.on("connect", () => {
      this.setStatus("connected")
      this.client?.subscribe(SUBSCRIPTIONS, { qos: 1 })
    })
    this.client.on("reconnect", () => this.setStatus("connecting"))
    this.client.on("close", () => this.setStatus("disconnected"))
    this.client.on("error", () => this.setStatus("disconnected"))
    this.client.on("message", (topic, data) => {
      // 摄像头主题为二进制 JPEG 帧，转换为 dataURL 交给上层渲染
      // 注意：主题含两段前缀（qbot/v1/robots/{id}/camera 共 5 段），用末尾段判断
      const segs = topic.split("/")
      if (segs[segs.length - 1] === "camera" && segs[segs.length - 3] === "robots") {
        this.handlers.forEach((h) => h(topic, binaryToDataUrl(data)))
        return
      }
      let payload: unknown
      try {
        payload = JSON.parse(data.toString())
      } catch {
        payload = data.toString()
      }
      this.handlers.forEach((h) => h(topic, payload))
    })
  }

  stop() {
    this.client?.end(true)
    this.client = null
  }

  publish(topic: string, payload: unknown, opts?: { retain?: boolean }) {
    this.client?.publish(topic, JSON.stringify(payload), { qos: 1, retain: opts?.retain ?? false })
  }

  subscribe(handler: MessageHandler) {
    this.handlers.add(handler)
    return () => this.handlers.delete(handler)
  }

  onStatus(handler: StatusHandler) {
    this.statusHandlers.add(handler)
    handler(this.status)
    return () => this.statusHandlers.delete(handler)
  }

  private setStatus(s: BusStatus) {
    this.status = s
    this.statusHandlers.forEach((h) => h(s))
  }
}
