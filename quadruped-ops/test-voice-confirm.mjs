// 确认流程联调：读 voice/commands 快照，确认第一条 pending_confirm，观察任务落库
import mqtt from "mqtt";

const client = mqtt.connect("mqtt://localhost:1883");
let done = false;
client.on("connect", () => {
  client.subscribe(["qbot/v1/voice/commands", "qbot/v1/fleet/tasks"], { qos: 1 });
});
client.on("message", (topic, buf) => {
  if (done) return;
  if (topic === "qbot/v1/voice/commands") {
    const cmds = JSON.parse(buf.toString());
    const pending = cmds.find((c) => c.status === "pending_confirm");
    if (!pending) { console.log("no pending command"); return; }
    console.log(`confirming: [${pending.cmdId}] 「${pending.transcript}」 -> ${pending.parsed?.type} @ ${pending.parsed?.target}`);
    done = true;
    client.publish("qbot/v1/voice/cmd", JSON.stringify({ cmdId: pending.cmdId, action: "confirm", ts: Date.now() }), { qos: 1 });
    setTimeout(() => { client.end(); process.exit(0); }, 3000);
  }
  if (topic === "qbot/v1/fleet/tasks") {
    const tasks = JSON.parse(buf.toString());
    const voice = tasks.filter((t) => t.source === "voice");
    if (voice.length > 0) {
      console.log("voice tasks in fleet/tasks:");
      for (const t of voice) console.log(`  #${t.id} [${t.status}] ${t.title} robot=${t.robotId} target=${t.target} mode=${t.mode}`);
    }
  }
});
