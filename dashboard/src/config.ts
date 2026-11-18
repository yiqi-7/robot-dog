/**
 * 全局配置 —— 部署到现场只需改这一个文件。
 *
 * mode = 'mock'：内置设备模拟器，无需任何外部服务即可演示全部功能。
 * mode = 'real'：连接真实 MQTT Broker（需开启 WebSocket 端口）。
 *
 * 运行时覆盖：在 index.html 里先加载 ./qbot-config.js，它设置 window.__QBOT_CONFIG__。
 * 好处是切换环境（本机 ↔ 云端中转）只改那一个 JS 文件，不必重新构建。
 * 未覆盖的字段回退到下面的默认值。
 */
type RuntimeOverride = {
  mqtt?: {
    mode?: "mock" | "real"
    brokerUrl?: string
    username?: string
    password?: string
    topicPrefix?: string
    reconnectPeriod?: number
  }
  streaming?: {
    whepBase?: string
    /** ICE 服务器列表，如 ["stun:stun.l.google.com:19302"]；局域网可留空 */
    iceServers?: string[]
  }
}

/** 读取运行时覆盖配置；没有则返回 undefined */
const runtime: RuntimeOverride | undefined =
  typeof window !== "undefined"
    ? (window as unknown as { __QBOT_CONFIG__?: RuntimeOverride }).__QBOT_CONFIG__
    : undefined

/** 当前页面主机名：本机部署时看板与 broker 同机，跟随它即可免配置 */
const pageHost =
  typeof window !== "undefined" && window.location.hostname
    ? window.location.hostname
    : "localhost"

/** 覆盖值非空则用覆盖值，否则用默认值 */
const pick = (v: string | undefined, fallback: string): string =>
  v && v.trim() ? v.trim() : fallback

export const MQTT_CONFIG = {
  /** 'mock' 内置模拟 | 'real' 真实 Broker */
  mode: (runtime?.mqtt?.mode ?? "real") as "mock" | "real",

  /**
   * Broker WebSocket 地址。
   * 默认自动跟随当前页面主机名：看板（server.js:8080）与 broker 的 WS 端口 8083 由
   * 在同一台机器启动，这样换电脑/换 IP 后无需改配置。
   * 走云端中转时由 qbot-config.js 覆盖成 "ws://<服务器IP>:8083/mqtt"。
   */
  brokerUrl: pick(runtime?.mqtt?.brokerUrl, `ws://${pageHost}:8083/mqtt`),
  username: pick(runtime?.mqtt?.username, ""),
  password: pick(runtime?.mqtt?.password, ""),

  /** 主题前缀，与设备端约定一致 */
  topicPrefix: pick(runtime?.mqtt?.topicPrefix, "qbot/v1"),

  /** 断线重连间隔 ms */
  reconnectPeriod: runtime?.mqtt?.reconnectPeriod ?? 3000,
}

// 启动时打印实际生效的连接地址，排查「看板连错 broker」时一眼可见
if (typeof window !== "undefined") {
  // eslint-disable-next-line no-console
  console.info(
    "[qbot] broker =", MQTT_CONFIG.brokerUrl,
    "| 来源 =", runtime?.mqtt?.brokerUrl ? "qbot-config.js" : "页面主机名默认",
    "| mode =", MQTT_CONFIG.mode,
  )
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
  /**
   * WHEP 拉流基地址。默认跟随页面主机名（MediaMTX 与看板同机启动）。
   * 走云端中转时由 qbot-config.js 覆盖成 "http://<服务器IP>:8889"。
   */
  whepBase: pick(runtime?.streaming?.whepBase, `http://${pageHost}:8889`),
  /** 摄像头流路径约定：机器人推 rtsp://<本机IP>:8554/<path>，看板从 <whepBase>/<path>/whep 拉流 */
  camStreamPath: (robotId: string) => `cam/${robotId}`,
  /** 无流 / 断流时的重试间隔 ms */
  retryMs: 5000,
  /**
   * ICE 服务器。局域网直连（看板与 MediaMTX 同机/同网段）留空即可，
   * 走公网中转时建议配置 STUN，帮助 NAT 打洞（否则只能靠 host/prflx 候选，建连不稳）。
   */
  iceServers: (runtime?.streaming?.iceServers ?? []) as string[],
}
