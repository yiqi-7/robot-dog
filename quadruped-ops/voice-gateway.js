/**
 * 语音网关（voice-gateway）
 *
 * 链路：机器人麦克风 → {P}/robots/{id}/audio（语音片段）
 *       → ASR（SenseVoice，可选）→ 文本
 *       → LLM（Ollama，可选）/ 规则解析 → 结构化任务
 *       → 确认策略（{P}/voice/policy）→ 看板确认 / 直通
 *       → 写 {P}/fleet/tasks + 下发 robots/{id}/cmd assign
 *       → TTS 播报 {P}/robots/{id}/tts
 *
 * 无 Ollama / 无 ASR 模型时自动降级为规则解析 / 仅文本注入（{P}/voice/text），
 * 全链路仍可工作。
 *
 * 运行：node voice-gateway.js
 * 环境变量：
 *   MQTT_URL      默认 mqtt://localhost:1883
 *   TOPIC_PREFIX  默认 qbot/v1
 *   OLLAMA_URL    默认 http://localhost:11434
 *   LLM_BACKEND   ollama（默认）| openai（llama.cpp server / vLLM 等兼容端点）
 *   OPENAI_BASE_URL  openai 后端的基地址，默认 http://127.0.0.1:8081/v1
 *   VOICE_MODEL   默认 qwen2.5:3b
 *   ASR_MODEL_DIR 默认 ./models/sensevoice（需 model.onnx + tokens.txt）
 *   PENDING_TTL_MS 待确认指令超时（默认 10 分钟）
 *   VOICE_CLIP_DIR 语音片段落盘目录（默认 ./voice-clips，由 server.js 挂到 /voice-clips/）
 *   VOICE_CLIP_KEEP 最多保留的片段数（默认 100）
 */
import mqtt from "mqtt";
import fs from "node:fs";
import path from "node:path";

const MQTT_URL = process.env.MQTT_URL || "mqtt://localhost:1883";
const P = process.env.TOPIC_PREFIX || "qbot/v1";
const OLLAMA_URL = process.env.OLLAMA_URL || "http://localhost:11434";
const VOICE_MODEL = process.env.VOICE_MODEL || "qwen2.5:1.5b";
// LLM 后端：ollama（默认，/api/chat + format schema）| openai（/v1/chat/completions，
// 供 llama.cpp server / vLLM / LM Studio 等兼容端点用）
const LLM_BACKEND = (process.env.LLM_BACKEND || "ollama").toLowerCase();
const OPENAI_BASE_URL = (process.env.OPENAI_BASE_URL || "http://127.0.0.1:8081/v1").replace(/\/$/, "");
const ASR_MODEL_DIR = process.env.ASR_MODEL_DIR || path.join(process.cwd(), "models", "sensevoice");
const PENDING_TTL_MS = Number(process.env.PENDING_TTL_MS || 10 * 60_000);
const CLIP_DIR = process.env.VOICE_CLIP_DIR || path.join(process.cwd(), "voice-clips");
const CLIP_KEEP = Number(process.env.VOICE_CLIP_KEEP || 100);

const T = {
  robotState: (id) => `${P}/robots/${id}/state`,
  robotAudio: (id) => `${P}/robots/${id}/audio`,
  robotCmd: (id) => `${P}/robots/${id}/cmd`,
  robotTts: (id) => `${P}/robots/${id}/tts`,
  siteMap: () => `${P}/site/map`,
  fleetTasks: () => `${P}/fleet/tasks`,
  voiceCommands: () => `${P}/voice/commands`,
  voiceCmd: () => `${P}/voice/cmd`,
  voicePolicy: () => `${P}/voice/policy`,
  voiceText: () => `${P}/voice/text`,
  event: () => `${P}/event`,
};

const TASK_TYPES = ["move", "inspection", "charging"];
const TYPE_LABELS = { move: "移动", inspection: "巡检", charging: "充电" };

const DEFAULT_POLICY = {
  requireConfirm: { move: true, inspection: true, charging: false },
  confidenceThreshold: 0.75,
  estopDirect: true,
  ttsReply: true,
};

/* ---------------- 运行态 ---------------- */
let siteMap = { width: 600, height: 380, zones: [], points: [] };
const robots = new Map(); // id -> { name, status }
let tasks = [];
let policy = structuredClone(DEFAULT_POLICY);
let commands = []; // VoiceCommandMsg[]，新在前
const pendingTimers = new Map(); // cmdId -> timeout

/* ---------------- ASR（可选，sherpa-onnx SenseVoice） ---------------- */
let recognizer = null;
async function initAsr() {
  const modelFile = [path.join(ASR_MODEL_DIR, "model.int8.onnx"), path.join(ASR_MODEL_DIR, "model.onnx")]
    .find((f) => fs.existsSync(f));
  const tokensFile = path.join(ASR_MODEL_DIR, "tokens.txt");
  if (!modelFile || !fs.existsSync(tokensFile)) {
    console.log(`[asr] 模型缺失（${ASR_MODEL_DIR} 需 model.int8.onnx/model.onnx + tokens.txt），语音通道关闭，仅文本注入可用`);
    return;
  }
  try {
    const mod = await import("sherpa-onnx-node");
    const sherpa = mod.default ?? mod;
    recognizer = new sherpa.OfflineRecognizer({
      featConfig: { sampleRate: 16000, featureDim: 80 },
      modelConfig: {
        senseVoice: { model: modelFile, useInverseTextNormalization: 1 },
        tokens: tokensFile,
        numThreads: 2,
        provider: "cpu",
        debug: 0,
      },
    });
    console.log(`[asr] SenseVoice 已加载（${ASR_MODEL_DIR}）`);
  } catch (e) {
    console.log(`[asr] sherpa-onnx-node 不可用：${e.message}；语音通道关闭，仅文本注入可用`);
  }
}

function transcribePcm(buf, sampleRate, channels = 1) {
  if (!recognizer) return null;
  const frames = Math.floor(buf.length / (2 * channels));
  const samples = new Float32Array(frames);
  for (let i = 0; i < frames; i++) {
    // 多声道只取第一个声道（机器人端如成立体声，通常是左右同内容）
    samples[i] = buf.readInt16LE(i * 2 * channels) / 32768;
  }
  const stream = recognizer.createStream();
  stream.acceptWaveform({ sampleRate, samples });
  recognizer.decode(stream);
  const text = recognizer.getResult(stream).text?.trim() ?? "";
  stream.free?.();
  return text;
}

/* ---------------- 音频落盘与质量统计 ---------------- */
/** PCM s16le 裸流包 WAV 头 */
function wavWrap(pcm, sampleRate, channels) {
  const header = Buffer.alloc(44);
  header.write("RIFF", 0);
  header.writeUInt32LE(36 + pcm.length, 4);
  header.write("WAVE", 8);
  header.write("fmt ", 12);
  header.writeUInt32LE(16, 16); // fmt chunk 大小
  header.writeUInt16LE(1, 20); // PCM
  header.writeUInt16LE(channels, 22);
  header.writeUInt32LE(sampleRate, 24);
  header.writeUInt32LE(sampleRate * channels * 2, 28); // byteRate
  header.writeUInt16LE(channels * 2, 32); // blockAlign
  header.writeUInt16LE(16, 34); // bitsPerSample
  header.write("data", 36);
  header.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([header, pcm]);
}

/** 保存语音片段，返回看板可访问的 URL 路径；保留最近 CLIP_KEEP 条 */
function saveClip(cmdId, pcm, sampleRate, channels) {
  fs.mkdirSync(CLIP_DIR, { recursive: true });
  fs.writeFileSync(path.join(CLIP_DIR, `${cmdId}.wav`), wavWrap(pcm, sampleRate, channels));
  try {
    const files = fs.readdirSync(CLIP_DIR).filter((f) => f.endsWith(".wav"))
      .map((f) => ({ f, t: fs.statSync(path.join(CLIP_DIR, f)).mtimeMs }))
      .sort((a, b) => b.t - a.t);
    for (const old of files.slice(CLIP_KEEP)) fs.unlinkSync(path.join(CLIP_DIR, old.f));
  } catch { /* 清理失败不影响主流程 */ }
  return `/voice-clips/${cmdId}.wav`;
}

/** 音频质量统计：时长（秒）、RMS 电平、峰值（0-1 归一化，按第一声道计算） */
function audioStats(pcm, sampleRate, channels) {
  const frames = Math.floor(pcm.length / (2 * channels));
  if (!frames) return { duration: 0, rms: 0, peak: 0 };
  let sum = 0, peak = 0;
  for (let i = 0; i < frames; i++) {
    const v = pcm.readInt16LE(i * 2 * channels) / 32768;
    sum += v * v;
    const a = Math.abs(v);
    if (a > peak) peak = a;
  }
  return { duration: frames / sampleRate, rms: Math.sqrt(sum / frames), peak };
}

/* ---------------- LLM 解析（可选，Ollama） ---------------- */
let llmAvailable = false;
async function checkLlm() {
  if (LLM_BACKEND === "openai") {
    // OpenAI 兼容端点（llama.cpp server 等）：探 /models，命中即认为可用并预热
    try {
      const res = await fetch(`${OPENAI_BASE_URL}/models`, { signal: AbortSignal.timeout(3000) });
      const j = await res.json();
      const list = Array.isArray(j.data) ? j.data.map((m) => m.id ?? m.name) : [];
      llmAvailable = list.length === 0 || list.some((m) => String(m).includes(VOICE_MODEL));
      console.log(`[llm] OpenAI 兼容后端 ${llmAvailable ? `可用（${OPENAI_BASE_URL}，模型 ${VOICE_MODEL}）` : "已连接但模型不匹配"}`);
      if (llmAvailable) {
        console.log("[llm] 预热模型…");
        const t0 = Date.now();
        await fetch(`${OPENAI_BASE_URL}/chat/completions`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ model: VOICE_MODEL, messages: [{ role: "user", content: "ok" }], max_tokens: 4 }),
          signal: AbortSignal.timeout(180_000),
        });
        console.log(`[llm] 预热完成（${((Date.now() - t0) / 1000).toFixed(1)}s）`);
      }
    } catch {
      llmAvailable = false;
      console.log(`[llm] OpenAI 兼容后端不可达（${OPENAI_BASE_URL}），使用规则解析兜底`);
    }
    return;
  }
  try {
    const res = await fetch(`${OLLAMA_URL}/api/tags`, { signal: AbortSignal.timeout(3000) });
    const j = await res.json();
    llmAvailable = (j.models ?? []).some((m) => m.name.startsWith(VOICE_MODEL.split(":")[0]));
    console.log(`[llm] Ollama ${llmAvailable ? `可用，模型 ${VOICE_MODEL}` : `已连接但未找到模型 ${VOICE_MODEL}`}`);
    if (llmAvailable) {
      // 预热：把模型载入内存/显存，避免第一条语音指令等模型加载
      console.log("[llm] 预热模型…");
      const t0 = Date.now();
      await fetch(`${OLLAMA_URL}/api/chat`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ model: VOICE_MODEL, messages: [{ role: "user", content: "ok" }], stream: false, keep_alive: "30m" }),
        signal: AbortSignal.timeout(120_000),
      });
      console.log(`[llm] 预热完成（${((Date.now() - t0) / 1000).toFixed(1)}s）`);
    }
  } catch {
    llmAvailable = false;
    console.log(`[llm] Ollama 不可达（${OLLAMA_URL}），使用规则解析兜底`);
  }
}

const LLM_SCHEMA = {
  type: "object",
  properties: {
    action: { type: "string", enum: ["task", "estop", "unknown"] },
    type: { type: "string", enum: ["move", "inspection", "charging"] },
    target: { type: ["string", "null"] },
    mode: { type: "string", enum: ["immediate", "scheduled"] },
    scheduledAt: { type: ["number", "null"] },
    priority: { type: "string", enum: ["low", "normal", "high", "urgent"] },
    robot: { type: ["string", "null"] },
    confidence: { type: "number" },
  },
  required: ["action", "confidence"],
};

async function parseWithLlm(text, now) {
  const pointList = siteMap.points.map((p) => `${p.id}=${p.name}${p.kind === "charger" ? "(充电桩)" : ""}`).join("、");
  const robotList = [...robots.entries()].map(([id, r]) => `${id}=${r.name}`).join("、");
  const prompt = [
    `你是厂区四足机器人语音指令解析器。把中文口语指令解析成 JSON，只输出 JSON，不要解释。`,
    `当前时间戳(毫秒): ${now}`,
    `可用点位: ${pointList || "无"}`,
    `可用机器人: ${robotList || "无"}`,
    `规则:`,
    `- "停下/别动/急停"类 -> action=estop`,
    `- 任务类 -> action=task，type 取 move(去/到/移动)/inspection(巡检/检查/看看/转一圈)/charging(充电/回充/回桩)`,
    `- target 必须是点位 id（充电类默认选充电桩点位）；没提点位给 null`,
    `- 提到机器人名字或编号 -> robot 填对应 id；没提给 null`,
    `- 带时间（"X分钟后"/"X点后"/"下午3点"）-> mode=scheduled 且 scheduledAt 为毫秒时间戳；否则 immediate`,
    `- "下午/晚上"的时间必须换算成 24 小时制（下午3点 = 15:00）`,
    `- confidence 0-1，拿不准给低分`,
    `示例: "让仿真狗去仓库货架区巡检" -> {"action":"task","type":"inspection","target":"warehouse-racks","mode":"immediate","scheduledAt":null,"priority":"normal","robot":null,"confidence":0.92}`,
    `指令: "${text}"`,
  ].join("\n");

  if (LLM_BACKEND === "openai") {
    // OpenAI 兼容端点没有 ollama 的 format 约束，靠 prompt 收口 + 剥围栏
    const res = await fetch(`${OPENAI_BASE_URL}/chat/completions`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        model: VOICE_MODEL,
        messages: [{ role: "user", content: prompt }],
        temperature: 0.1,
        max_tokens: 220,
      }),
      signal: AbortSignal.timeout(60_000),
    });
    if (!res.ok) throw new Error(`openai 兼容端点 http ${res.status}`);
    const j = await res.json();
    let txt = j.choices?.[0]?.message?.content ?? "";
    txt = txt.replace(/^\s*```(?:json)?\s*/i, "").replace(/```\s*$/, "").trim();
    const m = txt.match(/\{[\s\S]*\}/);          // 从回复里抠第一个 JSON 对象
    if (!m) throw new Error(`LLM 未返回 JSON：${txt.slice(0, 80)}`);
    return JSON.parse(m[0]);
  }
  const res = await fetch(`${OLLAMA_URL}/api/chat`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      model: VOICE_MODEL,
      messages: [{ role: "user", content: prompt }],
      format: LLM_SCHEMA,
      stream: false,
      keep_alive: "30m",
      options: { temperature: 0.1 },
    }),
    signal: AbortSignal.timeout(60_000),
  });
  if (!res.ok) throw new Error(`ollama http ${res.status}`);
  const j = await res.json();
  return JSON.parse(j.message?.content ?? "{}");
}

/* ---------------- 规则解析（兜底） ---------------- */
function parseWithRules(text, now) {
  const t = text.replace(/[，。！？!?,.\s]+/g, "");
  // 急停
  if (/急停|停下|停止|别动|站住/.test(t)) {
    return { action: "estop", confidence: 0.95 };
  }
  // 点名机器人
  let robot = null;
  for (const [id, r] of robots) {
    if (t.toUpperCase().includes(id.toUpperCase()) || (r.name && t.includes(r.name))) { robot = id; break; }
  }
  // 定时
  // 中文数字 -> 数值（"两分钟""十一分钟"这类 ASR 常见输出；0.5B 模型也经常漏掉）
  const cnDigit = { "一": 1, "二": 2, "两": 2, "三": 3, "四": 4, "五": 5,
    "六": 6, "七": 7, "八": 8, "九": 9 };
  const cnNum = (str) => {
    if (/^\d+$/.test(str)) return Number(str);
    if (str === "十") return 10;
    if (str === "半") return 0.5;
    const m10 = str.match(/^十([一二三四五六七八九])?$/);
    if (m10) return 10 + (m10[1] ? cnDigit[m10[1]] : 0);
    const m1 = str.match(/^([一二两三四五六七八九])十$/);
    if (m1) return cnDigit[m1[1]] * 10;
    const m2 = str.match(/^([一二两三四五六七八九])十([一二三四五六七八九])$/);
    if (m2) return cnDigit[m2[1]] * 10 + cnDigit[m2[2]];
    if (cnDigit[str] !== undefined) return cnDigit[str];   // 单个数字："一分钟"
    return NaN;
  };
  let mode = "immediate";
  let scheduledAt = null;
  let m;
  if ((m = t.match(/([\d一二两三四五六七八九十]+)个?分钟?后/))) {
    const n = cnNum(m[1]);
    if (Number.isFinite(n) && n > 0) { mode = "scheduled"; scheduledAt = now + n * 60_000; }
  } else if ((m = t.match(/([\d一二两三四五六七八九十]+)个?(?:半)?小时后/)) || (m = t.match(/半小时后/))) {
    const n = m[1] ? cnNum(m[1]) : 0.5;
    if (Number.isFinite(n) && n > 0) { mode = "scheduled"; scheduledAt = now + n * 3600_000; }
  } else if ((m = t.match(/(下午|晚上|中午|上午|早上|凌晨)?([\d一二两三四五六七八九十]{1,3})[点:：](半|([\d一二两三四五六七八九十]{1,3})分?)?/))) {
    let hh = cnNum(m[2]);
    const period = m[1];
    if ((period === "下午" || period === "晚上") && hh < 12) hh += 12;
    if (period === "中午" && hh < 12) hh += 12;
    const mm = m[3] === "半" ? 30 : (m[4] ? (cnNum(m[4]) || 0) : 0);
    const d = new Date(now);
    d.setHours(hh, mm, 0, 0);
    if (d.getTime() <= now) d.setDate(d.getDate() + 1);
    mode = "scheduled"; scheduledAt = d.getTime();
  }
  // 类别
  let type = null;
  if (/充电|回充|回桩/.test(t)) type = "charging";
  else if (/巡检|巡查|检查|看看|转一圈|巡逻/.test(t)) type = "inspection";
  else if (/移动|过去|过来|前往|到/.test(t) || /去/.test(t)) type = "move";
  // 点位（充电桩优先匹配充电任务）
  let target = null;
  const chargers = siteMap.points.filter((p) => p.kind === "charger");
  if (type === "charging" && chargers.length > 0) target = chargers[0].id;
  for (const p of siteMap.points) {
    const short = p.name.split("-").pop();
    if (t.includes(p.name) || (short.length >= 2 && t.includes(short))) { target = p.id; break; }
  }
  if (!target) {
    for (const z of siteMap.zones) {
      if (t.includes(z.name)) { target = siteMap.points.find((p) => p.zone === z.id)?.id ?? null; break; }
    }
  }
  if (!type) return { action: "unknown", confidence: 0.3 };
  const confidence = (target ? 0.72 : 0.58) - (mode === "scheduled" && !scheduledAt ? 0.1 : 0);
  return { action: "task", type, target, mode, scheduledAt, priority: "normal", robot, confidence };
}

/* ---------------- 任务构建与校验 ---------------- */
function buildTask(parsed, text, now) {
  if (!parsed.type || !TASK_TYPES.includes(parsed.type)) return { err: "无法识别任务类别（移动/巡检/充电）" };
  let target = parsed.target ?? null;
  if (target && !siteMap.points.some((p) => p.id === target)) {
    // LLM 可能给了名字，尝试按名称解析
    target = siteMap.points.find((p) => p.name === target)?.id ?? null;
    if (!target) return { err: `点位「${parsed.target}」不在地图配置中` };
  }
  if (parsed.type === "charging" && !target) {
    target = siteMap.points.find((p) => p.kind === "charger")?.id ?? null;
    if (!target) return { err: "地图中没有充电桩点位" };
  }
  const mode = parsed.mode === "scheduled" ? "scheduled" : "immediate";
  const scheduledAt = mode === "scheduled" ? Number(parsed.scheduledAt) : null;
  if (mode === "scheduled" && (!Number.isFinite(scheduledAt) || scheduledAt <= now)) {
    return { err: "定时任务时间无效或已过点" };
  }
  const pointName = target ? siteMap.points.find((p) => p.id === target)?.name : null;
  const title = `${TYPE_LABELS[parsed.type]}${pointName ? ` · ${pointName}` : ""}`;
  return {
    parsed: {
      title,
      type: parsed.type,
      mode,
      target,
      priority: ["low", "normal", "high", "urgent"].includes(parsed.priority) ? parsed.priority : "normal",
      scheduledAt,
    },
  };
}

/* ---------------- 发布辅助 ---------------- */
function publishCommands() {
  client.publish(T.voiceCommands(), JSON.stringify(commands.slice(0, 50)), { retain: true, qos: 1 });
}

function tts(robotId, text, cmdId) {
  if (!policy.ttsReply || !robotId) return;
  client.publish(T.robotTts(robotId), JSON.stringify({ cmdId, ts: Date.now(), text }), { qos: 1 });
}

function raiseEvent(level, message) {
  client.publish(T.event(), JSON.stringify({
    alarmId: Date.now() % 1_000_000_000, ts: Date.now(), level, source: "语音网关", message,
  }), { qos: 1 });
}

function upsertCommand(cmd) {
  const i = commands.findIndex((c) => c.cmdId === cmd.cmdId);
  if (i >= 0) commands[i] = cmd; else commands.unshift(cmd);
  publishCommands();
}

/* ---------------- 生命周期 ---------------- */
function acceptCommand(cmd) {
  const now = Date.now();
  if (cmd.action === "estop") {
    if (cmd.robotId) {
      client.publish(T.robotCmd(cmd.robotId), JSON.stringify({
        cmdId: `CMD-${now.toString(36)}`, ts: now, action: "estop", task: null,
      }), { qos: 1 });
    }
    cmd.status = "accepted";
    upsertCommand(cmd);
    tts(cmd.robotId ?? cmd.heardBy, "已急停", cmd.cmdId);
    raiseEvent("critical", `语音急停：${cmd.robotId ?? cmd.heardBy}（「${cmd.transcript}」）`);
    return;
  }
  const id = tasks.reduce((m, t) => Math.max(m, t.id), 0) + 1;
  const task = {
    id,
    title: cmd.parsed.title,
    type: cmd.parsed.type,
    mode: cmd.parsed.mode,
    robotId: cmd.robotId,
    target: cmd.parsed.target,
    priority: cmd.parsed.priority,
    status: "published",
    scheduledAt: cmd.parsed.scheduledAt,
    description: `语音指令：${cmd.transcript}`,
    createdAt: now,
    updatedAt: now,
    source: "voice",
  };
  tasks = [task, ...tasks];
  client.publish(T.fleetTasks(), JSON.stringify(tasks), { retain: true, qos: 1 });
  // 即时任务立即下发 assign（与看板发布行为一致）
  if (task.mode === "immediate" && task.robotId) {
    client.publish(T.robotCmd(task.robotId), JSON.stringify({
      cmdId: `CMD-${now.toString(36)}`, ts: now, action: "assign", task,
    }), { qos: 1 });
  }
  cmd.status = "accepted";
  cmd.taskId = id;
  upsertCommand(cmd);
  const robotName = cmd.robotId ? (robots.get(cmd.robotId)?.name ?? cmd.robotId) : "机器人";
  const pointName = cmd.parsed.target ? siteMap.points.find((p) => p.id === cmd.parsed.target)?.name : null;
  const when = task.mode === "scheduled" ? `，计划 ${new Date(task.scheduledAt).toLocaleTimeString("zh-CN", { hour12: false })}` : "";
  tts(cmd.robotId ?? cmd.heardBy, `收到，${robotName}${TYPE_LABELS[task.type]}${pointName ?? ""}${when}`, cmd.cmdId);
  raiseEvent("info", `语音指令已发布为任务 #${id}：「${cmd.transcript}」→ ${task.title}${cmd.robotId ? ` @ ${cmd.robotId}` : ""}`);
}

// ---------------------------------------------------------------------------
// 定时任务调度器
// acceptCommand 对 scheduled 任务只入列不下发（"五分钟后去充电"这类指令），
// 这里每 5 s 扫描到期任务补发 assign。两个防呆：
//   1) dispatchedIds：机器人侧状态回写有延迟，防止同一任务重复下发；
//   2) 过期兜底：网关停机跨过 scheduledAt 再重启时，超过 30 分钟的旧任务
//      不再补跑（避免半夜重启把机器人突然开走），标记 expired。
// ---------------------------------------------------------------------------
const dispatchedIds = new Set();
setInterval(() => {
  const now = Date.now();
  let changed = false;
  for (const t of tasks) {
    if (t.mode !== "scheduled" || !t.scheduledAt || !t.robotId) continue;
    if (t.status !== "published" || dispatchedIds.has(t.id)) continue;
    if (Number(t.scheduledAt) > now) continue;
    if (now - Number(t.scheduledAt) > 30 * 60_000) {
      t.status = "expired";
      t.updatedAt = now;
      changed = true;
      raiseEvent("warning", `定时任务 #${t.id} 已过期未执行（超过 30 分钟）：${t.title}`);
      console.log(`[scheduler] 任务 #${t.id} 过期，跳过下发`);
      continue;
    }
    dispatchedIds.add(t.id);
    client.publish(T.robotCmd(t.robotId), JSON.stringify({
      cmdId: `CMD-${now.toString(36)}`, ts: now, action: "assign", task: t,
    }), { qos: 1 });
    t.dispatchedAt = now;
    t.updatedAt = now;
    changed = true;
    const pointName = t.target ? siteMap.points.find((p) => p.id === t.target)?.name : null;
    tts(t.robotId, `定时任务到点，开始${TYPE_LABELS[t.type] ?? "执行"}${pointName ?? ""}`);
    raiseEvent("info", `定时任务 #${t.id} 到点下发：${t.title} → ${t.robotId}`);
    console.log(`[scheduler] 任务 #${t.id} 到点下发（计划 ${new Date(t.scheduledAt).toLocaleTimeString("zh-CN", { hour12: false })}）`);
  }
  if (changed) {
    client.publish(T.fleetTasks(), JSON.stringify(tasks), { retain: true, qos: 1 });
  }
}, 5000);

async function handleTranscript({ text, heardBy, source, cmdId, audioFile = null }) {
  const now = Date.now();
  const cmd = {
    cmdId, heardBy, robotId: heardBy, ts: now, transcript: text,
    parsed: null, action: "task", confidence: 0, engine: "rule", source,
    status: "failed",
  };
  if (audioFile) cmd.audioFile = audioFile;

  // 1. 解析：LLM 优先，规则兜底；双路校验合并
  let parsed = null;
  if (llmAvailable) {
    try { parsed = await parseWithLlm(text, now); cmd.engine = "llm"; }
    catch (e) { console.log(`[llm:${LLM_BACKEND}] 解析失败，转规则兜底：${e.message}`); }
  }
  if (!parsed) parsed = parseWithRules(text, now);
  else {
    const rule = parseWithRules(text, now);
    // 急停关键词安全兜底：规则命中急停而 LLM 没识别时，以规则为准
    if (rule.action === "estop" && parsed.action !== "estop") parsed = rule;
    else if (parsed.action === "task") {
      // 小模型（0.5B 级）常见三类漏项，都用规则路补齐：
      // ① target 必须是真实点位：0.5B 会照抄 prompt 示例里的旧 id
      //   （实测输出 pt-shelf-n，地图里根本没有），假 id 直接丢弃
      if (siteMap.points.length && parsed.target &&
          !siteMap.points.some((p) => p.id === parsed.target)) {
        parsed.target = null;
      }
      // ①' 漏 target（"去充电桩充电"没给点位 id）
      if (!parsed.target && rule.target) parsed.target = rule.target;
      // ② 漏 robot（点名了但 LLM 没对上）
      if (!parsed.robot && rule.robot) parsed.robot = rule.robot;
      // ③ scheduledAt 缺失或偏差离谱（实测 0.5B 会把"5 分钟"算成 30 秒）。
      //    偏差超过 5 分钟或缺项时直接用规则值，规则解析对中文时间词更可靠。
      if (rule.action === "task" && rule.mode === "scheduled" && rule.scheduledAt) {
        const llmAt = Number(parsed.scheduledAt) || 0;
        const off = Math.abs(llmAt - rule.scheduledAt);
        if (parsed.mode !== "scheduled" || !llmAt || off > 5 * 60_000) {
          parsed.mode = "scheduled";
          parsed.scheduledAt = rule.scheduledAt;
          parsed.confidence = Math.min(Number(parsed.confidence) || 0.9, 0.8);
        }
      }
    }
  }

  if (parsed.action === "estop") {
    cmd.action = "estop";
    cmd.confidence = parsed.confidence ?? 0.9;
    if (parsed.robot) cmd.robotId = parsed.robot;
    if (policy.estopDirect) { acceptCommand(cmd); return; }
    cmd.status = "pending_confirm";
    upsertCommand(cmd);
    armPendingTimer(cmd.cmdId);
    return;
  }
  if (parsed.action !== "task") {
    cmd.status = "failed";
    cmd.reason = "未识别为有效指令";
    upsertCommand(cmd);
    tts(cmd.robotId, "没听清，请再说一次", cmd.cmdId);
    return;
  }

  cmd.confidence = Math.max(0, Math.min(1, Number(parsed.confidence) || 0.5));
  if (parsed.robot && robots.has(parsed.robot)) cmd.robotId = parsed.robot;
  const { parsed: built, err } = buildTask(parsed, text, now);
  if (err) {
    cmd.status = "failed";
    cmd.reason = err;
    upsertCommand(cmd);
    tts(cmd.robotId, `指令无效：${err}`, cmd.cmdId);
    raiseEvent("warning", `语音指令解析失败：「${text}」（${err}）`);
    return;
  }
  cmd.parsed = built;

  // 2. 确认策略
  const needConfirm = policy.requireConfirm[built.type] || cmd.confidence < policy.confidenceThreshold;
  if (needConfirm) {
    cmd.status = "pending_confirm";
    upsertCommand(cmd);
    armPendingTimer(cmd.cmdId);
    tts(cmd.robotId, `收到，${TYPE_LABELS[built.type]}任务等待确认`, cmd.cmdId);
  } else {
    acceptCommand(cmd);
  }
}

function armPendingTimer(cmdId) {
  clearTimeout(pendingTimers.get(cmdId));
  pendingTimers.set(cmdId, setTimeout(() => {
    const cmd = commands.find((c) => c.cmdId === cmdId);
    if (cmd && cmd.status === "pending_confirm") {
      cmd.status = "rejected";
      cmd.reason = "确认超时";
      upsertCommand(cmd);
    }
    pendingTimers.delete(cmdId);
  }, PENDING_TTL_MS));
}

function handleVoiceCmd(msg) {
  const cmd = commands.find((c) => c.cmdId === msg.cmdId);
  if (!cmd || cmd.status !== "pending_confirm") return;
  clearTimeout(pendingTimers.get(cmd.cmdId));
  pendingTimers.delete(cmd.cmdId);
  if (msg.action === "confirm") {
    acceptCommand(cmd);
  } else {
    cmd.status = "rejected";
    upsertCommand(cmd);
    tts(cmd.robotId ?? cmd.heardBy, "指令已取消", cmd.cmdId);
    raiseEvent("info", `语音指令已拒绝：「${cmd.transcript}」`);
  }
}

/* ---------------- MQTT 主流程 ---------------- */
const client = mqtt.connect(MQTT_URL);
client.on("connect", () => {
  console.log(`[mqtt] 已连接 ${MQTT_URL}`);
  client.subscribe([
    `${P}/robots/+/state`,
    `${P}/robots/+/audio`,
    T.siteMap(),
    T.fleetTasks(),
    T.voicePolicy(),
    T.voiceCmd(),
    T.voiceText(),
  ], { qos: 1 });
});

client.on("message", (topic, buf) => {
  let msg;
  try { msg = JSON.parse(buf.toString()); } catch { return; }
  try {
    if (topic === T.siteMap()) { siteMap = msg; return; }
    if (topic === T.fleetTasks()) { tasks = Array.isArray(msg) ? msg : []; return; }
    if (topic === T.voicePolicy()) {
      policy = { ...DEFAULT_POLICY, ...msg, requireConfirm: { ...DEFAULT_POLICY.requireConfirm, ...(msg.requireConfirm ?? {}) } };
      console.log(`[policy] 更新：${JSON.stringify(policy)}`);
      return;
    }
    if (topic === T.voiceCmd()) { handleVoiceCmd(msg); return; }
    if (topic === T.voiceText()) {
      // 测试注入：{ text, robotId? }
      if (!msg.text) return;
      handleTranscript({
        text: String(msg.text), heardBy: msg.robotId ?? [...robots.keys()][0] ?? "QB-01",
        source: "text", cmdId: `vc-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`,
      });
      return;
    }
    const seg = topic.split("/");
    if (seg[2] === "robots" && seg[4] === "state") {
      robots.set(msg.robotId, { name: msg.name, status: msg.status });
      return;
    }
    if (seg[2] === "robots" && seg[4] === "audio") {
      if (!msg.data) return;
      if (msg.format !== "pcm_s16le") {
        console.log(`[asr] 不支持的音频格式 ${msg.format}（当前仅支持 pcm_s16le）`);
        return;
      }
      const pcm = Buffer.from(msg.data, "base64");
      const sampleRate = msg.sampleRate || 16000;
      const channels = msg.channels || 1;
      const st = audioStats(pcm, sampleRate, channels);
      const cmdId = `vc-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`;
      const audioFile = saveClip(cmdId, pcm, sampleRate, channels);
      console.log(`[asr] ${msg.robotId} 收到音频 ${st.duration.toFixed(1)}s RMS=${st.rms.toFixed(4)} peak=${st.peak.toFixed(3)} → ${audioFile}`);
      // 静音/过短守卫：避免 ASR 对近静音输入产生幻觉文本（"I."、"Okay." 等）
      if (st.duration < 0.4 || st.rms < 0.002) {
        const reason = st.duration < 0.4 ? `音频过短（${st.duration.toFixed(1)}s）` : `音量过低（RMS=${st.rms.toFixed(4)}），疑似未采到声音`;
        console.log(`[asr] ${reason}，跳过识别`);
        upsertCommand({
          cmdId, heardBy: msg.robotId, robotId: msg.robotId, ts: Date.now(),
          transcript: "（未采到有效声音）", parsed: null, action: "task",
          confidence: 0, engine: "rule", source: "voice", status: "failed",
          reason, audioFile,
        });
        return;
      }
      const text = transcribePcm(pcm, sampleRate, channels);
      if (!text) {
        if (recognizer) {
          upsertCommand({
            cmdId, heardBy: msg.robotId, robotId: msg.robotId, ts: Date.now(),
            transcript: "（识别为空）", parsed: null, action: "task",
            confidence: 0, engine: "rule", source: "voice", status: "failed",
            reason: "ASR 未识别出文本", audioFile,
          });
        }
        console.log("[asr] 识别为空或 ASR 未启用");
        return;
      }
      console.log(`[asr] ${msg.robotId}: 「${text}」`);
      handleTranscript({ text, heardBy: msg.robotId, source: "voice", cmdId, audioFile });
    }
  } catch (e) {
    console.error(`[error] 处理 ${topic} 失败：`, e);
  }
});

/* ---------------- 启动 ---------------- */
await initAsr();
await checkLlm();
console.log(`[voice-gateway] 已启动（prefix=${P}）`);
