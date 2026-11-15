// 恢复机器人端推送的厂区地图（Broker 重启后 retained 丢失，用嗅探到的原样数据恢复）
import mqtt from "mqtt";

const payload = {"height":14.0,"points":[{"id":"pt-boiler-1","kind":"waypoint","name":"1号锅炉","x":3.5,"y":7.0,"zone":"boiler-room"},{"id":"pt-mill-2","kind":"waypoint","name":"2号机床","x":16.8,"y":6.5,"zone":"mill-area"},{"id":"pt-dry-1","kind":"waypoint","name":"1号烘干塔","x":9.5,"y":3.2,"zone":"drying"},{"id":"pt-air-1","kind":"waypoint","name":"空压机站","x":15.2,"y":10.8},{"id":"pt-shelf-n","kind":"waypoint","name":"货架区-北排","x":10.0,"y":8.3,"zone":"shelf-north"},{"id":"pt-shelf-s","kind":"waypoint","name":"货架区-南排","x":10.0,"y":5.8,"zone":"shelf-south"},{"id":"charger-1","kind":"charger","name":"充电区-1号桩","x":18.0,"y":12.0,"zone":"charging"}],"updatedAt":1789215408721,"version":1,"width":20.0,"zones":[{"height":4,"id":"boiler-room","name":"锅炉房","width":3.5,"x":0.0,"y":5.0},{"height":3,"id":"mill-area","name":"机加工区","width":3.5,"x":16.5,"y":5.0},{"height":2.5,"id":"shelf-north","name":"仓库货架区","width":11,"x":4.5,"y":8.0},{"height":2,"id":"shelf-south","name":"货架区南","width":9,"x":5.5,"y":3.5},{"height":3,"id":"charging","name":"充电区","width":3,"x":16.5,"y":10.5},{"height":3,"id":"drying","name":"烘干区","width":3,"x":8.0,"y":0.5}]};

const client = mqtt.connect("mqtt://localhost:1883");
client.on("connect", () => {
  client.publish("qbot/v1/site/map", JSON.stringify(payload), { retain: true, qos: 1 }, (err) => {
    console.log(err ? `failed: ${err}` : "robot site map restored (retained)");
    client.end();
    process.exit(err ? 1 : 0);
  });
});
client.on("error", (e) => { console.error("connect error:", e.message); process.exit(1); });
