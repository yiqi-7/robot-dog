/**
 * 主题（MQTT topic）契约测试 —— node:test，不需要 broker、不 import 业务代码
 *
 * 【为什么这么测】"看板怎么拼主题"这件事在仓库里被写了三遍：
 *   src/mqtt/topics.ts（看板）、voice-gateway.js（语音网关）、C++ 桥接包的
 *   topic_prefix_ 拼接。三处一旦漂移，症状是"消息发出去了但没人收到"——最难查的
 *   一类问题，而且没有任何编译期提示。这里用【纯文本解析 + 文档反查】把三处锁在
 *   一起：既不 import 带副作用的模块（voice-gateway.js 顶层就 mqtt.connect），
 *   也不需要启动 broker。
 *
 * 只读的文件（本测试不修改任何源码）：
 *   ../MQTT-INTERFACE.md, ../VOICE-INTERFACE.md  —— 主题口径的权威来源
 *   src/config.ts                                —— 主题前缀默认值
 *   src/mqtt/topics.ts                           —— 看板侧构造函数 + 订阅通配
 *   voice-gateway.js                             —— 语音网关侧主题表 T
 *   ../src/quadruped_mqtt_bridge/src/*.cpp       —— C++ 侧 topic_prefix_ 拼接
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

/** 契约占位符 → 示例值（同一位置在文档里叫 robotId，在网关里叫 id） */
const SAMPLES = { robotId: "QB-01", id: "QB-01", facilityId: "f1", alarmId: "7" };

const HAS_WILDCARD = /[+#]/;

/** `qbot/v1/robots/+/state` 这类通配过滤是否命中具体主题（+ 单层 / # 多层） */
function topicMatch(filter, topic) {
  const f = filter.split("/");
  const t = topic.split("/");
  for (let i = 0; i < f.length; i += 1) {
    if (f[i] === "#") return true;
    if (t[i] === undefined) return false;
    if (f[i] !== "+" && f[i] !== t[i]) return false;
  }
  return f.length === t.length;
}

/** 把 JS 模板字符串（`${P}/robots/${id}/state`）填成具体主题 */
function fillTemplate(tpl, prefix) {
  return tpl.replace(/\$\{(\w+)\}/g, (all, name) =>
    name === "P" ? prefix : (SAMPLES[name] ?? `__unk__${name}__`));
}

/** 把文档模板（{P}/robots/{robotId}/state）填成具体主题 */
function fillDocTemplate(tpl, prefix) {
  return tpl
    .replace(/^\{P\}/, prefix)
    .replace(/\{(\w+)\}/g, (all, name) => SAMPLES[name] ?? `__unk__${name}__`);
}

/**
 * 解析形如 `  name: (id) => `${P}/robots/${id}/state`,` 的主题构造函数。
 * 不做 TS 编译：topics.ts 用 `@/` 别名，node 直接跑不了；而这里只需要模板字符串。
 */
function parseTopicBuilders(src, file) {
  const re = /^[ \t]{2}([A-Za-z_]\w*):\s*\(([^)]*)\)\s*=>\s*`([^`]+)`/gm;
  const map = new Map();
  let m;
  while ((m = re.exec(src)) !== null) {
    // 参数带类型标注（id: string / alarmId: number | string），只取变量名
    const args = m[2]
      .split(",")
      .map((s) => s.trim().split(":")[0].trim())
      .filter(Boolean);
    map.set(m[1], { args, template: m[3], file });
  }
  assert.ok(map.size > 0, `${file} 里没解析到任何 "name: (args) => 模板字符串" 形式的构造函数`);
  return map;
}

/** 构造函数 → 具体主题字符串 */
function concreteness(builders, prefix) {
  const out = new Map();
  for (const [name, { args, template }] of builders) {
    let filled = template;
    for (const a of args) {
      filled = filled.replace(
        new RegExp(`\\$\\{${a}\\}`, "g"),
        SAMPLES[a] ?? SAMPLES.id,
      );
    }
    out.set(name, fillTemplate(filled, prefix));
  }
  return out;
}

function sliceBetween(src, startMarker, endMarker, file) {
  const i = src.indexOf(startMarker);
  assert.ok(i >= 0, `${file} 里找不到 ${JSON.stringify(startMarker)}`);
  const j = src.indexOf(endMarker, i + startMarker.length);
  assert.ok(j > i, `${file} 里 ${JSON.stringify(startMarker)} 没有闭合`);
  return src.slice(i, j);
}

/** 看板主题前缀默认值（src/config.ts 的 topicPrefix 兜底值） */
function dashboardPrefix() {
  const m = readDash("src/config.ts").match(/topicPrefix:\s*pick\([^,]+,\s*"([^"]+)"\)/);
  assert.ok(m, "src/config.ts 里找不到 topicPrefix 的默认值");
  return m[1];
}

/** 语音网关主题前缀默认值（voice-gateway.js 的 TOPIC_PREFIX 兜底值） */
function gatewayPrefix() {
  const m = readDash("voice-gateway.js").match(/TOPIC_PREFIX\s*\|\|\s*"([^"]+)"/);
  assert.ok(m, "voice-gateway.js 里找不到 TOPIC_PREFIX 的默认值");
  return m[1];
}

/** 文档里出现过的全部 {P}/... 主题（含通配），填成具体形态 */
function documentedTopics(prefix) {
  const md =
    readDash("MQTT-INTERFACE.md") + "\n" + readDash("VOICE-INTERFACE.md");
  const set = new Set();
  for (const m of md.matchAll(/\{P\}\/[A-Za-z0-9/{}_+-]+/g)) {
    set.add(fillDocTemplate(m[0], prefix));
  }
  assert.ok(set.size > 10, `文档里只解析出 ${set.size} 个主题，解析规则可能失效`);
  return set;
}

/** 主题是否被文档声明过（精确命中，或被文档里的通配主题覆盖） */
function isDocumented(docs, topic) {
  if (docs.has(topic)) return true;
  for (const d of docs) {
    if (HAS_WILDCARD.test(d) && topicMatch(d, topic)) return true;
  }
  return false;
}

const PREFIX = dashboardPrefix();
const BUILDERS = parseTopicBuilders(readDash("src/mqtt/topics.ts"), "src/mqtt/topics.ts");
const TOPIC = concreteness(BUILDERS, PREFIX);
const DOCS = documentedTopics(PREFIX);

/** 看板必须知道的主题（来自 MQTT-INTERFACE.md 的主题总表） */
const REQUIRED_TOPICS = [
  "robotState",
  "robotCamera",
  "robotCmd",
  "robotAudio",
  "robotTts",
  "fleetTasks",
  "facilityState",
  "siteMap",
  "voiceCommands",
  "voiceCmd",
  "voicePolicy",
  "voiceText",
  "event",
  "alarmAck",
];

test("主题前缀默认值在看板与语音网关两侧一致", () => {
  assert.equal(PREFIX, gatewayPrefix(), "看板 src/config.ts 与 voice-gateway.js 的主题前缀默认值不一致");
  assert.ok(PREFIX.length > 0);
});

test("TOPICS 的构造结果与文档声明的主题逐条对得上", () => {
  for (const name of REQUIRED_TOPICS) {
    assert.ok(TOPIC.has(name), `src/mqtt/topics.ts 的 TOPICS 缺少构造 ${name}`);
  }
  for (const [name, topic] of TOPIC) {
    assert.ok(
      isDocumented(DOCS, topic),
      `TOPICS.${name}() 拼出 ${topic}，但 MQTT-INTERFACE.md / VOICE-INTERFACE.md 里没有这个主题`,
    );
  }
});

test("机器人主题符合 {P}/robots/{id}/xxx 的拼接规则", () => {
  // 提示里点名的那条规则，单独钉死（其余靠文档反查）
  assert.equal(TOPIC.get("robotState"), `${PREFIX}/robots/QB-01/state`);
  const robotTopics = ["robotState", "robotCamera", "robotCmd", "robotAudio", "robotTts"];
  for (const name of robotTopics) {
    const topic = TOPIC.get(name);
    assert.equal(topic.split("/").length, 5, `${name} 不是 5 段：${topic}`);
    assert.equal(topic.split("/")[2], "robots", `${name} 第 3 段应为 robots：${topic}`);
    assert.equal(topic.split("/")[3], "QB-01", `${name} 第 4 段应为机器人 id：${topic}`);
  }
  // 机器人 id 换了，主题必须跟着换（构造函数没写死示例值）
  const builders = BUILDERS.get("robotState");
  assert.deepEqual(builders.args, ["id"]);
});

test("TOPICS 无重复、无未替换的占位符", () => {
  const seen = new Map();
  for (const [name, topic] of TOPIC) {
    assert.ok(topic.startsWith(`${PREFIX}/`), `${name} 没带主题前缀：${topic}`);
    assert.ok(!topic.includes("${"), `${name} 残留未替换的模板占位符：${topic}`);
    assert.ok(!topic.includes("__unk__"), `${name} 有未知占位符：${topic}`);
    const prev = seen.get(topic);
    assert.equal(prev, undefined, `TOPICS.${name} 与 TOPICS.${prev} 拼出同一个主题 ${topic}`);
    seen.set(topic, name);
  }
});

test("SUBSCRIPTIONS 的通配主题能覆盖 TOPICS 里的具体主题", () => {
  const src = readDash("src/mqtt/topics.ts");
  const block = sliceBetween(src, "export const SUBSCRIPTIONS", "]", "src/mqtt/topics.ts");
  const subs = [...block.matchAll(/`([^`]+)`/g)].map((m) => fillTemplate(m[1], PREFIX));
  assert.ok(subs.length > 0, "SUBSCRIPTIONS 是空的");

  const covered = new Set();
  for (const sub of subs) {
    assert.ok(!sub.includes("__unk__"), `订阅主题里有未知占位符：${sub}`);
    for (const [name, topic] of TOPIC) {
      if (topicMatch(sub, topic)) covered.add(name);
    }
    assert.ok(
      [...TOPIC.values()].some((t) => topicMatch(sub, t)),
      `订阅 ${sub} 匹配不到 TOPICS 里任何主题（拼错了？）`,
    );
  }
  // 机器人遥测必须用通配订阅，否则看板只能看到一台机器人
  assert.ok(
    subs.includes(`${PREFIX}/robots/+/state`),
    `看板没有订阅 ${PREFIX}/robots/+/state（多机器人会漏）`,
  );
  for (const name of [
    "robotState",
    "robotCamera",
    "fleetTasks",
    "facilityState",
    "siteMap",
    "voiceCommands",
    "voicePolicy",
    "event",
  ]) {
    assert.ok(covered.has(name), `看板没有订阅 ${name}（${TOPIC.get(name)}）`);
  }
});

test("语音网关的主题表与看板契约逐条一致", () => {
  const gwPrefix = gatewayPrefix();
  const gwTopic = concreteness(
    parseTopicBuilders(readDash("voice-gateway.js"), "voice-gateway.js"),
    gwPrefix,
  );
  for (const [name, topic] of gwTopic) {
    assert.ok(TOPIC.has(name), `voice-gateway.js 的 T.${name} 在看板 TOPICS 里没有对应项`);
    assert.equal(
      topic,
      TOPIC.get(name),
      `voice-gateway.js 的 T.${name}() 与看板 TOPICS.${name}() 不一致`,
    );
  }
  // 网关自己读/写的那几条，少一条链路就断
  for (const name of [
    "robotState",
    "robotAudio",
    "robotCmd",
    "robotTts",
    "siteMap",
    "fleetTasks",
    "voiceCommands",
    "voiceCmd",
    "voicePolicy",
    "voiceText",
    "event",
  ]) {
    assert.ok(gwTopic.has(name), `voice-gateway.js 缺少主题构造 ${name}`);
  }
});

test("语音网关订阅了它依赖的全部输入主题", () => {
  const gw = readDash("voice-gateway.js");
  const gwPrefix = gatewayPrefix();
  const block = sliceBetween(gw, "client.subscribe([", "]", "voice-gateway.js");
  const gwTopic = concreteness(parseTopicBuilders(gw, "voice-gateway.js"), gwPrefix);

  // 订阅列表里有两种写法：直接写模板（`${P}/robots/+/state`）和引用主题表（T.siteMap()）
  const subs = [...block.matchAll(/`([^`]+)`/g)].map((m) => fillTemplate(m[1], gwPrefix));
  for (const m of block.matchAll(/\bT\.(\w+)\(\)/g)) {
    const t = gwTopic.get(m[1]);
    assert.ok(t, `网关订阅列表引用了不存在的主题构造 T.${m[1]}()`);
    subs.push(t);
  }
  assert.ok(subs.length >= 5, `网关只订阅了 ${subs.length} 个主题，明显偏少`);
  const matched = new Set();
  for (const sub of subs) {
    for (const [name, topic] of gwTopic) {
      if (topicMatch(sub, topic)) matched.add(name);
    }
    assert.ok(
      [...gwTopic.values()].some((t) => topicMatch(sub, t)),
      `网关订阅的 ${sub} 对不上它自己的任何主题构造（拼错了？）`,
    );
  }
  for (const name of [
    "robotState",
    "robotAudio",
    "siteMap",
    "fleetTasks",
    "voicePolicy",
    "voiceCmd",
    "voiceText",
  ]) {
    assert.ok(matched.has(name), `网关没有订阅 ${name}，相关功能会静默失效`);
  }
});

test("C++ 桥接包 topic_prefix_ 拼出的主题都在看板契约内", () => {
  const pkgSrc = path.join(REPO, "src/quadruped_mqtt_bridge/src");
  const files = fs.readdirSync(pkgSrc).filter((f) => f.endsWith(".cpp")).sort();
  assert.ok(files.length > 0, `找不到 C++ 源文件：${pkgSrc}`);

  // topic_prefix_ + "/robots/" + robot_id_ + "/state"  或  topic_prefix_ + "/event"
  const re = /topic_prefix_\s*\+\s*"([^"]+)"(?:\s*\+\s*robot_id_\s*\+\s*"([^"]*)")?/g;
  const byTopic = new Map([...TOPIC.entries()].map(([name, topic]) => [topic, name]));
  const found = new Map(); // 具体主题 -> 出处

  for (const f of files) {
    const src = fs.readFileSync(path.join(pkgSrc, f), "utf8");
    for (const m of src.matchAll(re)) {
      const topic = PREFIX + m[1] + (m[2] !== undefined ? `QB-01${m[2]}` : "");
      if (!found.has(topic)) found.set(topic, f);
      assert.ok(
        byTopic.has(topic),
        `${f} 里拼出的 MQTT 主题 ${topic} 在看板 TOPICS 里没有对应项（看板收不到）`,
      );
    }
  }

  assert.ok(found.size > 0, "C++ 侧一个 topic_prefix_ 主题都没解析到，解析规则失效");
  // 关键链路双向确认：这些主题 C++ 侧真的在发/收
  for (const name of [
    "robotState",
    "robotCamera",
    "robotCmd",
    "robotAudio",
    "robotTts",
    "siteMap",
    "fleetTasks",
    "event",
  ]) {
    assert.ok(
      found.has(TOPIC.get(name)),
      `C++ 侧没有发现 ${name} 主题（${TOPIC.get(name)}）`,
    );
  }
});
