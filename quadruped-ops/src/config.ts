/**
 * 全局配置 —— 部署到现场只需改这一个文件。
 *
 * mode = 'mock'：内置设备模拟器，无需任何外部服务即可演示全部功能。
 * mode = 'real'：连接局域网真实 MQTT Broker（需开启 WebSocket 端口）。
 */
export const MQTT_CONFIG = {
  /** 'mock' 内置模拟 | 'real' 真实 Broker */
  mode: "real" as "mock" | "real",

  /**
   * Broker WebSocket 地址。
   * 默认自动跟随当前页面主机名：看板（server.js:8080）与 broker.js:8083 由 start.bat
   * 在同一台机器启动，这样换电脑/换 IP 后无需改配置。
   * 若 Broker 部署在另一台机器，改成固定地址，如 "ws://192.168.6.2:8083/mqtt"。
   */
  brokerUrl: `ws://${typeof window !== "undefined" && window.location.hostname ? window.location.hostname : "localhost"}:8083/mqtt`,
  username: "",
  password: "",

  /** 主题前缀，与设备端约定一致 */
  topicPrefix: "qbot/v1",

  /** 断线重连间隔 ms */
  reconnectPeriod: 3000,
}

/** 厂区平面图：SVG 像素坐标 1200×760，实际尺寸 600m×380m（1px = 0.5m） */
export const MAP = {
  pxW: 1200,
  pxH: 760,
  mW: 600,
  mH: 380,
  /** 米 -> 像素 */
  toPx: (m: number) => m * 2,
  /** 像素 -> 米 */
  toM: (px: number) => px * 0.5,
}

/** 流媒体（MediaMTX）配置：机器人 RTSP 推流 8554，看板 WebRTC(WHEP) 拉流 8889 */
export const STREAMING = {
  /** WHEP 拉流基地址：MediaMTX 部署在云服务器（RTSP 机器人推流 -> WebRTC 看板拉流） */
  whepBase: "http://43.172.91.67:8889",
  /** 摄像头流路径约定：机器人推 rtsp://<本机IP>:8554/<path>，看板从 <whepBase>/<path>/whep 拉流 */
  camStreamPath: (robotId: string) => `cam/${robotId}`,
  /** 无流 / 断流时的重试间隔 ms */
  retryMs: 5000,
}
