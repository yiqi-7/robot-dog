/**
 * 消息总线抽象：MockBus（内置回环模拟）与 MqttBus（真实 Broker）实现同一接口。
 * 上层只面向 IBus 编程，切换数据源无需改业务代码。
 *
 * payload 约定：JSON 对象为 object；摄像头帧统一为 dataURL 字符串
 * （真实链路上是二进制 JPEG，由 MqttBus 转换）。
 */

export type BusStatus = "connected" | "connecting" | "disconnected" | "mock"

export type MessageHandler = (topic: string, payload: unknown) => void
export type StatusHandler = (status: BusStatus) => void

export interface IBus {
  readonly label: string
  start(): void
  stop(): void
  publish(topic: string, payload: unknown, opts?: { retain?: boolean }): void
  subscribe(handler: MessageHandler): () => void
  onStatus(handler: StatusHandler): () => void
}
