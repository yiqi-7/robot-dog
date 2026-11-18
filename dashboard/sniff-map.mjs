// 一次性嗅探：订阅 site/map 等 retained 消息并打印
import mqtt from "mqtt";
const client = mqtt.connect("mqtt://localhost:1883");
const timer = setTimeout(() => { console.log("TIMEOUT"); client.end(); process.exit(0); }, 6000);
client.on("connect", () => {
  client.subscribe(["qbot/v1/site/map", "qbot/v1/robots/+/state"], { qos: 0 });
});
client.on("message", (topic, buf) => {
  clearTimeout(timer);
  if (topic.includes("site/map")) {
    console.log("=== site/map retained payload ===");
    console.log(buf.toString());
  } else {
    const m = JSON.parse(buf.toString());
    console.log(`robot ${m.robotId}: x=${m.x} y=${m.y}`);
  }
  setTimeout(() => { client.end(); process.exit(0); }, 1500);
});
