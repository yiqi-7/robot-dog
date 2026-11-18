// 联调验证脚本：向 {P}/site/map 发布 retained 地图配置
// 用法: node test-pub-map.mjs [default|test]
import mqtt from "mqtt";

const mode = process.argv[2] ?? "test";
const map = {
  version: 2,
  width: 600,
  height: 380,
  zones: [
    { id: "workshop-a", name: mode === "test" ? "车间A-联调" : "车间A", x: 35, y: 35, width: 230, height: 140 },
    { id: "workshop-b", name: "车间B", x: 35, y: 195, width: 230, height: 150 },
    { id: "warehouse", name: "仓库", x: 285, y: 35, width: 280, height: 125 },
    { id: "charging", name: "充电区", x: 285, y: 180, width: 130, height: 80 },
    { id: "power", name: "配电室", x: 430, y: 180, width: 135, height: 80 },
    { id: "control", name: "中控室", x: 285, y: 280, width: 280, height: 65 },
  ],
  points: [
    { id: "a-line-1", name: "车间A-1号产线", x: 90, y: 85, kind: "waypoint", zone: "workshop-a" },
    { id: "a-line-2", name: "车间A-2号产线", x: 195, y: 85, kind: "waypoint", zone: "workshop-a" },
    { id: "a-west-gate", name: "车间A-西门", x: 35, y: 125, kind: "waypoint", zone: "workshop-a" },
    { id: "b-assembly", name: "车间B-装配台", x: 130, y: 260, kind: "waypoint", zone: "workshop-b" },
    { id: "b-east-gate", name: "车间B-东门", x: 265, y: 250, kind: "waypoint", zone: "workshop-b" },
    { id: "warehouse-racks", name: "仓库-货架区", x: 380, y: 85, kind: "waypoint", zone: "warehouse" },
    { id: "warehouse-dock", name: "仓库-月台", x: 530, y: 135, kind: "waypoint", zone: "warehouse" },
    { id: "power-room", name: "配电室", x: 495, y: 220, kind: "waypoint", zone: "power" },
    { id: "boiler", name: "锅炉房", x: 450, y: 300, kind: "waypoint", zone: "control" },
    { id: "charger-1", name: "充电区-1号桩", x: 322.5, y: 236, kind: "charger", zone: "charging" },
    { id: "charger-2", name: "充电区-2号桩", x: 377.5, y: 236, kind: "charger", zone: "charging" },
    { id: "control-room", name: "中控室", x: 380, y: 310, kind: "waypoint", zone: "control" },
    { id: "north-gate", name: "厂区北门", x: 300, y: 12, kind: "waypoint" },
    { id: "ring-road", name: "厂区环路", x: 570, y: 180, kind: "waypoint" },
    ...(mode === "test"
      ? [{ id: "test-point", name: "联调测试点", x: 300, y: 190, kind: "waypoint" }]
      : []),
  ],
  updatedAt: Date.now(),
};

const client = mqtt.connect("mqtt://localhost:1883");
client.on("connect", () => {
  client.publish("qbot/v1/site/map", JSON.stringify(map), { retain: true, qos: 1 }, (err) => {
    if (err) console.error("publish failed:", err);
    else console.log(`published site/map (${mode}), points=${map.points.length}`);
    client.end();
    process.exit(err ? 1 : 0);
  });
});
client.on("error", (e) => { console.error("connect error:", e.message); process.exit(1); });
