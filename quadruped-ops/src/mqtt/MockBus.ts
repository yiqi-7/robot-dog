import { topicMatch } from "./topics"
import type { IBus, MessageHandler, StatusHandler } from "./bus"

/**
 * 内置回环 Broker：与真实 MQTT 行为一致（通配订阅 / retained 快照），
 * 设备模拟器（mqtt/deviceSim.ts）像真实设备一样向它发布消息。
 */
export class MockBus implements IBus {
  readonly label = "内置模拟器（回环）"
  private handlers = new Set<MessageHandler>()
  private statusHandlers = new Set<StatusHandler>()
  private retained = new Map<string, unknown>()

  start() {
    this.statusHandlers.forEach((h) => h("mock"))
  }

  stop() {
    this.retained.clear()
  }

  publish(topic: string, payload: unknown, opts?: { retain?: boolean }) {
    if (opts?.retain) this.retained.set(topic, payload)
    this.handlers.forEach((h) => h(topic, payload))
  }

  subscribe(handler: MessageHandler) {
    this.handlers.add(handler)
    // retained 消息回放（等价于真实 Broker 对新订阅者推送 retained）
    for (const [topic, payload] of this.retained) handler(topic, payload)
    return () => this.handlers.delete(handler)
  }

  onStatus(handler: StatusHandler) {
    this.statusHandlers.add(handler)
    handler("mock")
    return () => this.statusHandlers.delete(handler)
  }
}

/** 供模拟器判断“看板下发的 cmd”等主题 */
export { topicMatch }
