// 语音链路联调：读取 WAV 文件，以 pcm_s16le/base64 发布到 {P}/robots/{id}/audio
// 用法: node test-audio.mjs <wav文件> [robotId]
import mqtt from "mqtt";
import fs from "node:fs";

const wavPath = process.argv[2] ?? "models/sensevoice/test_wavs/zh.wav";
const robotId = process.argv[3] ?? "QB-01";

const buf = fs.readFileSync(wavPath);
if (buf.toString("ascii", 0, 4) !== "RIFF") { console.error("not a wav"); process.exit(1); }
// 解析 fmt / data chunk
let offset = 12, sampleRate = 16000, channels = 1, bits = 16, data = null;
while (offset + 8 <= buf.length) {
  const id = buf.toString("ascii", offset, offset + 4);
  const size = buf.readUInt32LE(offset + 4);
  if (id === "fmt ") {
    channels = buf.readUInt16LE(offset + 10);
    sampleRate = buf.readUInt32LE(offset + 12);
    bits = buf.readUInt16LE(offset + 22);
  } else if (id === "data") {
    data = buf.subarray(offset + 8, offset + 8 + size);
    break;
  }
  offset += 8 + size + (size % 2);
}
if (!data) { console.error("no data chunk"); process.exit(1); }
console.log(`wav: ${sampleRate}Hz ${channels}ch ${bits}bit, pcm ${data.length} bytes`);

const client = mqtt.connect("mqtt://localhost:1883");
client.on("connect", () => {
  client.publish(`qbot/v1/robots/${robotId}/audio`, JSON.stringify({
    robotId, ts: Date.now(), format: "pcm_s16le", sampleRate, channels,
    data: data.toString("base64"),
  }), { qos: 1 }, (err) => {
    console.log(err ? `publish failed: ${err}` : "audio published");
    client.end(); process.exit(err ? 1 : 0);
  });
});
