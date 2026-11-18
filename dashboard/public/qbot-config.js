/*
 * 运行时配置 —— 切换环境只改这个文件，无需重新构建。
 *
 * 优先级：本文件（window.__QBOT_CONFIG__）> src/config.ts 里的默认值。
 * 字段留空（或整行删掉）= 用默认值（跟随页面主机名）。
 *
 * 三种常见用法：
 *
 * 1) 本机自测（看板 + broker 同机）：mqtt.brokerUrl 留空即可
 *
 * 2) 云端中转（看板在本地，broker 在腾讯云，机器人也连云端）：
 *      mqtt.brokerUrl = "ws://<服务器IP>:8083/mqtt"
 *      streaming.whepBase = "http://<服务器IP>:8889"
 *
 * 3) 局域网 broker 在另一台机器：
 *      mqtt.brokerUrl = "ws://192.168.1.50:8083/mqtt"
 *
 * 注意：本文件必须由 index.html 里的 <script src="./qbot-config.js"> 同步加载，
 * 且要在主 bundle 之前执行（bundle 是 type="module"，天然延后执行）。
 */
window.__QBOT_CONFIG__ = {
  mqtt: {
    // "real" 连真实 broker；"mock" 用内置模拟器演示
    mode: "real",
    // 云端中转服务器：看板 → 腾讯云 broker → 端侧机器狗
    // 本机自测时改成 "" 即可回退到 ws://<当前页面主机名>:8083/mqtt
    brokerUrl: "ws://43.172.91.67:8083/mqtt",
    username: "",
    password: "",
    topicPrefix: "qbot/v1",
  },
  streaming: {
    // 云端 MediaMTX（WHEP 拉流）；本机自测时改成 "" 回退到页面主机名
    whepBase: "http://43.172.91.67:8889",
    // 公网/NAT 场景加 STUN 提升建连成功率；局域网可留空数组
    iceServers: ["stun:stun.l.google.com:19302"],
  },
}
