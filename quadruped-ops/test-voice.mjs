// 语音指令联调脚本：向 {P}/voice/text 注入文本（模拟 ASR 输出），并打印 voice/commands 快照
// 用法: node test-voice.mjs "让哨兵去充电" ["QB-01 去货架区北排巡检" ...]
import mqtt from "mqtt";

const texts = process.argv.slice(2);
if (texts.length === 0) {
  console.log('用法: node test-voice.mjs "指令文本" [...]');
  process.exit(1);
}

const client = mqtt.connect("mqtt://localhost:1883");
client.on("connect", () => {
  client.subscribe("qbot/v1/voice/commands", { qos: 1 });
  for (const text of texts) {
    client.publish("qbot/v1/voice/text", JSON.stringify({ text, robotId: "QB-01", ts: Date.now() }), { qos: 1 });
    console.log(`injected: 「${text}」`);
  }
  setTimeout(() => { client.end(); process.exit(0); }, 4000);
});
client.on("message", (_t, buf) => {
  const cmds = JSON.parse(buf.toString());
  for (const c of cmds.slice(0, texts.length + 1)) {
    console.log(`  [${c.status}] 「${c.transcript}」 engine=${c.engine} conf=${c.confidence} action=${c.action}` +
      (c.parsed ? ` -> ${c.parsed.type} @ ${c.parsed.target} mode=${c.parsed.mode} robot=${c.robotId}` : "") +
      (c.reason ? ` reason=${c.reason}` : "") + (c.taskId != null ? ` taskId=${c.taskId}` : ""));
  }
});
