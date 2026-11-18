// parseWithRules 离线单测（ESM）：抽取网关里的规则路，喂典型口语指令验证。
// 用法: node test_rules.mjs
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const code = fs.readFileSync(path.join(here, "voice-gateway.js"), "utf8");
const start = code.indexOf("function parseWithRules");
const end = code.indexOf("function buildTask");
if (start < 0 || end < 0) {
  console.error("没截到 parseWithRules");
  process.exit(1);
}
const fn = code.slice(start, end);

const sandbox = [
  "const siteMap = { points: [",
  '  { id: "warehouse-racks", name: "仓库-货架区", kind: "poi", zone: "warehouse" },',
  '  { id: "warehouse-dock", name: "仓库-月台", kind: "poi", zone: "warehouse" },',
  '  { id: "charger-1", name: "充电区-1号桩", kind: "charger", zone: "charge" },',
  '  { id: "control-room", name: "中控室", kind: "poi", zone: "office" }],',
  '  zones: [{ id: "warehouse", name: "仓库" }, { id: "charge", name: "充电区" }] };',
  'const robots = new Map([["robot-01", { name: "仿真狗" }]]);',
  "const now = Date.now();",
  fn,
  "return parseWithRules(text, now);",
].join("\n");

const parse = new Function("text", sandbox);

const cases = [
  ["一分钟后去充电桩充电", "task", "charging", "charger-1", "scheduled", 60],
  ["两分钟后让仿真狗去仓库货架区巡检", "task", "inspection", "warehouse-racks", "scheduled", 120],
  ["三分钟后回充", "task", "charging", "charger-1", "scheduled", 180],
  ["半小时后去中控室", "task", "move", "control-room", "scheduled", 1800],
  ["五分钟后去充电桩充电", "task", "charging", "charger-1", "scheduled", 300],
  ["十五分钟后巡检仓库", "task", "inspection", "warehouse-racks", "scheduled", 900],
  ["二十分钟后去月台", "task", "move", "warehouse-dock", "scheduled", 1200],
  ["下午3点半去中控室", "task", "move", "control-room", "scheduled", null],
  ["三点半去中控室", "task", "move", "control-room", "scheduled", null],
  ["去仓库货架区巡检", "task", "inspection", "warehouse-racks", "immediate", null],
  ["紧急停止", "estop", null, null, null, null],
  ["让仿真狗五分钟后充电", "task", "charging", "charger-1", "scheduled", 300],
];

let pass = 0;
for (const [text, action, type, target, mode, delta] of cases) {
  const r = parse(text);
  const at = r.scheduledAt;
  // estop 分支只返回 action/confidence，type/target/mode 是 undefined，不参与比较
  let ok = r.action === action &&
    (action !== "task" || (r.type === type && r.target === target && r.mode === mode));
  if (delta !== null && ok) {
    const off = Math.round((at - Date.now()) / 1000);
    ok = Math.abs(off - delta) <= 2;
  }
  if (delta === null && mode === "scheduled" && ok) ok = Number.isFinite(at) && at > Date.now();
  const atStr = at ? new Date(at).toTimeString().slice(0, 8) : "-";
  console.log(`  ${ok ? "PASS" : "FAIL"}  ${text}`);
  if (!ok) {
    console.log(`        实际: action=${r.action} type=${r.type} target=${r.target} mode=${r.mode} @${atStr}`);
  }
  if (ok) pass++;
}
console.log(`\n${pass}/${cases.length} 通过`);
process.exit(pass === cases.length ? 0 : 1);
