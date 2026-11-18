/**
 * 契约（contract）测试 —— 状态机 / 动作 / 错误码 / 站点表
 *
 * 【为什么这么测】这些取值在仓库里有三份副本：文档（MQTT-INTERFACE.md、
 * VOICE-INTERFACE.md）、前端契约（contracts/*.ts、src/mqtt/wire.ts）、以及
 * voice-gateway.js 与 C++ 里的硬编码。任何一份漂移，都会变成"任务卡在
 * pending_confirm 不动"或"下发 target 找不到点位"这类线上问题，而且没有编译期
 * 提示。这里全部用【读文件 + 纯文本断言】，不 import 带副作用的模块。
 */
import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const DASH = path.resolve(HERE, "..");
const REPO = path.resolve(DASH, "..");

const readDash = (rel) => fs.readFileSync(path.join(DASH, rel), "utf8");
const readRepo = (rel) => fs.readFileSync(path.join(REPO, rel), "utf8");

const TASK_TS = readDash("contracts/task.ts");
const WIRE_TS = readDash("src/mqtt/wire.ts");
const TYPES_TS = readDash("src/types/index.ts");
const GW = readDash("voice-gateway.js");
const MQTT_MD = readDash("MQTT-INTERFACE.md");
const VOICE_MD = readDash("VOICE-INTERFACE.md");

const sorted = (xs) => [...xs].sort();

/** 抽 `export const NAME = ["a", "b"] as const` 的字符串元素 */
function constArray(src, name, file) {
  const m = src.match(new RegExp(`export const ${name} = \\[([\\s\\S]*?)\\] as const`));
  assert.ok(m, `${file} 里找不到 export const ${name} = [...] as const`);
  return [...m[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]);
}

/** 抽 `export const NAME: Record<T, string> = { k: ... }` 的键名 */
function recordKeys(src, name, file) {
  const m = src.match(new RegExp(`(?:export )?const ${name}: Record<[^>]+> = \\{([\\s\\S]*?)\\n\\}`));
  assert.ok(m, `${file} 里找不到 ${name} 的映射表`);
  return [...m[1].matchAll(/^\s{2}(\w+):/gm)].map((x) => x[1]);
}

/** 抽形如 `field: "a" | "b" | "c"` 的字符串字面量联合，取包含 mustInclude 的那一处 */
function unionLiterals(src, field, mustInclude, file) {
  const all = [...src.matchAll(new RegExp(`\\b${field}:\\s*("(?:[^"]*)"(?:\\s*\\|\\s*"[^"]*")*)`, "g"))]
    .map((m) => [...m[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]));
  const hit = all.find((parts) => parts.includes(mustInclude));
  assert.ok(hit, `${file} 里找不到 ${field} 的字符串字面量联合（含 ${mustInclude}）`);
  return hit;
}

/** 抽 markdown 表格里 `| `field` | `a` 说明 / `b` 说明 |` 这一行的全部取值 */
function docRow(md, field, mustInclude, file) {
  const line = md
    .split("\n")
    .find((l) => l.startsWith("| `" + field + "` |") && l.includes(mustInclude));
  assert.ok(line, `${file} 里找不到字段 ${field} 的取值行（含 ${mustInclude}）`);
  return [...line.matchAll(/`([^`]+)`/g)].map((m) => m[1]).filter((t) => t !== field);
}

function sliceBetween(src, startMarker, endMarker, file) {
  const i = src.indexOf(startMarker);
  assert.ok(i >= 0, `${file} 里找不到 ${JSON.stringify(startMarker)}`);
  const j = src.indexOf(endMarker, i + startMarker.length);
  assert.ok(j > i, `${file} 里 ${JSON.stringify(startMarker)} 没有闭合`);
  return src.slice(i, j);
}

/* ---------------- 从契约文件里读出权威取值 ---------------- */

const TASK_TYPES = constArray(TASK_TS, "TASK_TYPES", "contracts/task.ts");
const TASK_MODES = constArray(TASK_TS, "TASK_MODES", "contracts/task.ts");
const TASK_PRIORITIES = constArray(TASK_TS, "TASK_PRIORITIES", "contracts/task.ts");
const TASK_STATUSES = constArray(TASK_TS, "TASK_STATUSES", "contracts/task.ts");
const SITE_POINTS = constArray(TASK_TS, "SITE_POINTS", "contracts/task.ts");

/** 语音指令状态（{P}/voice/commands 元素的 status 字段） */
const VOICE_STATUSES = unionLiterals(WIRE_TS, "status", "pending_confirm", "src/mqtt/wire.ts");

/* ---------------- 任务状态机 ---------------- */

test("任务状态 / 类型 / 模式 / 优先级集合与 MQTT-INTERFACE.md 一致", () => {
  assert.deepEqual(
    sorted(TASK_STATUSES),
    sorted(docRow(MQTT_MD, "status", "draft", "MQTT-INTERFACE.md")),
    "contracts/task.ts 的 TASK_STATUSES 与文档 §4.1 的状态取值不一致",
  );
  assert.deepEqual(
    sorted(TASK_TYPES),
    sorted(docRow(MQTT_MD, "type", "move", "MQTT-INTERFACE.md")),
    "TASK_TYPES 与文档 §4.1 的类型取值不一致",
  );
  assert.deepEqual(
    sorted(TASK_MODES),
    sorted(docRow(MQTT_MD, "mode", "immediate", "MQTT-INTERFACE.md")),
    "TASK_MODES 与文档 §4.1 的模式取值不一致",
  );
  assert.deepEqual(
    sorted(TASK_PRIORITIES),
    sorted(docRow(MQTT_MD, "priority", "low", "MQTT-INTERFACE.md")),
    "TASK_PRIORITIES 与文档 §4.1 的优先级取值不一致",
  );
  // 状态机约束（§4.3）：只有 draft / published / completed / revoked 四态，
  // 少了 draft 就发不出去，多了未知状态看板会渲染成空白
  assert.ok(TASK_STATUSES.includes("published"));
  assert.ok(TASK_STATUSES.includes("completed"));
});

test("看板的标签表覆盖且只覆盖契约取值", () => {
  assert.deepEqual(sorted(recordKeys(TASK_TS, "TASK_TYPE_LABELS", "contracts/task.ts")), sorted(TASK_TYPES));
  assert.deepEqual(sorted(recordKeys(TASK_TS, "TASK_MODE_LABELS", "contracts/task.ts")), sorted(TASK_MODES));
  assert.deepEqual(sorted(recordKeys(TASK_TS, "TASK_PRIORITY_LABELS", "contracts/task.ts")), sorted(TASK_PRIORITIES));
  assert.deepEqual(sorted(recordKeys(TASK_TS, "TASK_STATUS_LABELS", "contracts/task.ts")), sorted(TASK_STATUSES));
});

test("机器人状态 / 告警级别集合与文档一致", () => {
  assert.deepEqual(
    sorted(recordKeys(TYPES_TS, "ROBOT_STATUS_LABELS", "src/types/index.ts")),
    sorted(docRow(MQTT_MD, "status", "working", "MQTT-INTERFACE.md")),
    "RobotStatus 与文档 §3.1 的 status 取值不一致",
  );
  assert.deepEqual(
    sorted(recordKeys(TYPES_TS, "ALARM_LEVEL_LABELS", "src/types/index.ts")),
    sorted(docRow(MQTT_MD, "level", "info", "MQTT-INTERFACE.md")),
    "AlarmLevel 与文档 §5.2 的 level 取值不一致",
  );
});

/* ---------------- 指令 / 语音指令 ---------------- */

test("cmd 动作集合与文档 §3.3 表格一致，网关只发已知动作", () => {
  const cmdActions = unionLiterals(WIRE_TS, "action", "assign", "src/mqtt/wire.ts");
  const table = sliceBetween(MQTT_MD, "### 3.3", "\n---", "MQTT-INTERFACE.md");
  // 表头 `| `action` | 含义 | `task` |` 也以反引号开头，按"含义"排除掉
  const docActions = table
    .split("\n")
    .filter((l) => /^\| `[a-z]+` \|/.test(l) && !l.includes("含义"))
    .map((l) => /^\| `([a-z]+)` \|/.exec(l)[1]);
  assert.deepEqual(sorted(cmdActions), sorted(docActions), "CmdMsg.action 与文档 §3.3 不一致");

  // 网关真正下发给机器人的 cmd 动作：只取 T.robotCmd(...) 后面那段 payload。
  // 注意不能全文扫 `action: "..."`——解析结果里的 action 是 task/estop/unknown，
  // 与 cmd 动作（assign/cancel/estop/resume）是两个不同的枚举。
  const gwActions = new Set();
  for (const m of GW.matchAll(/T\.robotCmd\([^)]*\)[\s\S]{0,400}?action:\s*"([^"]+)"/g)) {
    gwActions.add(m[1]);
  }
  assert.ok(gwActions.size > 0, "voice-gateway.js 里找不到下发 cmd 的 payload，解析规则失效");
  for (const a of gwActions) {
    assert.ok(cmdActions.includes(a), `voice-gateway.js 下发了未在契约里的 cmd 动作 "${a}"`);
  }
  assert.ok(gwActions.has("assign") && gwActions.has("estop"), "网关应至少能下发 assign / estop");
});

test("语音指令状态集合与 VOICE-INTERFACE.md / 看板 UI 一致", () => {
  const docStatuses = docRow(VOICE_MD, "status", "pending_confirm", "VOICE-INTERFACE.md")
    .filter((t) => t !== "reason");
  assert.deepEqual(
    sorted(VOICE_STATUSES),
    sorted(docStatuses),
    "wire.ts 的 VoiceCommandMsg.status 与文档 §3 的状态集合不一致",
  );
  // 语音面板必须认识每一个线上状态，否则渲染出空白 chip
  assert.deepEqual(
    sorted(recordKeys(readDash("src/components/VoicePanel.tsx"), "STATUS_META", "src/components/VoicePanel.tsx")),
    sorted(VOICE_STATUSES),
    "VoicePanel 的 STATUS_META 与 VoiceCommandMsg.status 不一致",
  );
});

test("语音网关写入的 status 都落在任务/语音指令状态集合内", () => {
  const literals = new Set(
    [...GW.matchAll(/\bstatus(?::\s*|\s*=\s*)"([^"]+)"/g)].map((m) => m[1]),
  );
  assert.ok(literals.size >= 3, `网关里只找到 ${literals.size} 个 status 字面量，解析规则可能失效`);
  // 建任务时写的是任务状态；其余是语音指令自身状态
  assert.ok(
    literals.has("published"),
    "网关创建任务时应写 status=\"published\"（与看板发布行为一致）",
  );
  const allowed = new Set([...TASK_STATUSES, ...VOICE_STATUSES]);
  for (const s of literals) {
    assert.ok(
      allowed.has(s),
      `voice-gateway.js 里的 status="${s}" 既不属于任务状态 [${TASK_STATUSES}]，也不属于语音指令状态 [${VOICE_STATUSES}]`,
    );
  }
});

test("网关的 TASK_TYPES / TYPE_LABELS 与契约一致", () => {
  const m = GW.match(/const TASK_TYPES = \[([^\]]*)\]/);
  assert.ok(m, "voice-gateway.js 里找不到 const TASK_TYPES = [...]");
  const gwTypes = [...m[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]);
  assert.deepEqual(gwTypes, TASK_TYPES, "网关的 TASK_TYPES 与 contracts/task.ts 不一致");

  const labels = GW.match(/const TYPE_LABELS = \{([\s\S]*?)\n\};/);
  assert.ok(labels, "voice-gateway.js 里找不到 const TYPE_LABELS = {...}");
  const labelKeys = [...labels[1].matchAll(/(\w+):\s*"/g)].map((x) => x[1]);
  assert.deepEqual(sorted(labelKeys), sorted(gwTypes), "TYPE_LABELS 少了一种任务类型的播报文案");
});

test("LLM 结构化输出的枚举与契约一致", () => {
  const block = sliceBetween(GW, "const LLM_SCHEMA = {", "\n};", "voice-gateway.js");
  const enumOf = (key) => {
    const m = block.match(new RegExp(`${key}: \\{ type: "string", enum: \\[([^\\]]*)\\]`));
    assert.ok(m, `LLM_SCHEMA 里找不到 ${key} 的 enum`);
    return [...m[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]);
  };
  assert.deepEqual(enumOf("type"), TASK_TYPES, "LLM_SCHEMA.type 与 TASK_TYPES 不一致");
  assert.deepEqual(enumOf("mode"), TASK_MODES, "LLM_SCHEMA.mode 与 TASK_MODES 不一致");
  assert.deepEqual(enumOf("priority"), TASK_PRIORITIES, "LLM_SCHEMA.priority 与 TASK_PRIORITIES 不一致");
  const actions = enumOf("action");
  assert.ok(actions.includes("task") && actions.includes("estop"), "LLM_SCHEMA.action 必须能表达 task / estop");
  for (const a of actions) {
    assert.ok(["task", "estop", "unknown"].includes(a), `LLM_SCHEMA.action 出现未知取值 "${a}"`);
  }
});

/* ---------------- 错误码 ---------------- */

test("错误码集合唯一，且与 contracts/errors.ts 的导出名一一对应", () => {
  const errTs = readDash("contracts/errors.ts");
  const pairs = [...errTs.matchAll(/(\w+):\s*\(msg: string\)\s*=>\s*appError\((\d+)/g)]
    .map((m) => [m[1], Number(m[2])]);
  const map = Object.fromEntries(pairs);
  assert.deepEqual(map, {
    badRequest: 400,
    unauthorized: 401,
    forbidden: 403,
    notFound: 404,
    internal: 500,
  }, "contracts/errors.ts 的错误码集合发生了变化");
  const codes = pairs.map(([, c]) => c);
  assert.equal(new Set(codes).size, codes.length, "错误码有重复");
  // 统一错误形态：tag 固定 app_error，status/message 必填
  assert.match(errTs, /tag:\s*"app_error"/);
  assert.match(errTs, /status:\s*number/);
  assert.match(errTs, /message:\s*string/);
});

/* ---------------- 站点表（跨语言） ---------------- */

test("看板点位名称表与 C++ site_map.hpp 完全一致", () => {
  const hpp = readRepo(
    "src/quadruped_mqtt_bridge/include/quadruped_mqtt_bridge/site_map.hpp",
  );
  // {"id", "名称", x, y, "zone" 或 nullptr, "kind" 或 nullptr}
  const re = /\{\s*"([^"]+)",\s*"([^"]+)",\s*(-?[\d.]+),\s*(-?[\d.]+),\s*(?:"[^"]+"|nullptr),\s*(?:"([^"]+)"|nullptr)\s*\}/g;
  const cppPoints = [...hpp.matchAll(re)].map((m) => ({
    id: m[1],
    name: m[2],
    x: Number(m[3]),
    y: Number(m[4]),
    kind: m[5] ?? null,
  }));
  assert.ok(cppPoints.length > 0, "site_map.hpp 的 kPoints 一个点都没解析到");

  assert.equal(
    new Set(cppPoints.map((p) => p.id)).size,
    cppPoints.length,
    "site_map.hpp 的 kPoints 里有重复 id",
  );
  assert.equal(
    new Set(cppPoints.map((p) => p.name)).size,
    cppPoints.length,
    "site_map.hpp 的 kPoints 里有重复名称",
  );

  // 看板任务下拉框用【名称】当 target（MQTT-INTERFACE.md §4.1），
  // C++ 侧 findSitePoint 靠名称解析目标点：两份表必须完全一致
  assert.deepEqual(
    sorted(cppPoints.map((p) => p.name)),
    sorted(SITE_POINTS),
    "contracts/task.ts 的 SITE_POINTS 与 site_map.hpp 的 kPoints 名称集合不一致",
  );
  // 充电任务依赖 charger 点位
  assert.ok(
    cppPoints.some((p) => p.kind === "charger"),
    "site_map.hpp 里没有 kind=\"charger\" 的点位，语音/看板的充电任务会解析失败",
  );
});

test("网关上报的告警级别都在契约集合内", () => {
  const levels = new Set([...GW.matchAll(/raiseEvent\("(\w+)"/g)].map((m) => m[1]));
  assert.ok(levels.size > 0, "voice-gateway.js 里找不到 raiseEvent 调用");
  const known = recordKeys(TYPES_TS, "ALARM_LEVEL_LABELS", "src/types/index.ts");
  for (const l of levels) {
    assert.ok(known.includes(l), `网关上报了未定义的告警级别 "${l}"，看板会渲染成未知级别`);
  }
});
