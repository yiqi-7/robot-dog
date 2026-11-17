#pragma once
// ===========================================================================
// site_map.hpp —— 站点定义（坐标一律用【仿真世界米制】）
//
// 【归属说明】这份表被两个节点共用，所以单独放一个头文件（不放 common.hpp，
// 也不允许 task_runner 自带一份拷贝——拷贝会漂移，看板地图和导航目标就对不上）：
//   · mqtt_bridge  发布 {P}/site/map（retained），看板据此渲染厂区图
//   · task_runner  把看板/语音下发的目标点 id 解析成仿真坐标去导航
// 【看板坐标换算不在这里】仿真世界 ⇄ 看板地图（缩放/翻转）只有 mqtt_bridge
// 用到（state 的 x/y 与 site/map 发布），公式与参数都留在 mqtt_bridge 节点里。
//
// 名称与看板内置地图保持一致，这样语音规则解析、任务下拉框都能直接匹配。
// ===========================================================================

#include <string>
#include <vector>

namespace qmb {

// 区域：与 worlds/factory.sdf 里的六个物理区域逐一对应（坐标 = 仿真世界米制）
//   车间A  西-南   x[-9.6,-1.3] y[-6.9,-0.6]
//   车间B  西-北   x[-9.6,-1.3] y[ 0.6, 6.9]
//   仓库   东-南   x[ 1.3, 9.6] y[-6.9,-0.6]
//   充电区 东北偏西 x[ 1.3, 4.6] y[ 0.6, 6.9]
//   配电室 东北下   x[ 4.6, 9.6] y[ 0.6, 3.4]
//   中控室 东北上   x[ 4.6, 9.6] y[ 3.6, 6.9]
struct SiteZone {
  const char *id;
  const char *name;
  double x, y, w, h;   // 仿真世界中的矩形（左上角 + 宽高）
};

struct SitePoint {
  const char *id;
  const char *name;
  double x, y;              // 仿真世界坐标
  const char *zone;         // 所属区域 id
  const char *kind;         // "waypoint" / "charger" / nullptr
};

inline const std::vector<SiteZone> kZones = {
  {"workshop-a", "车间A",  -9.6, -6.9, 8.3, 6.3},
  {"workshop-b", "车间B",  -9.6,  0.6, 8.3, 6.3},
  {"warehouse",  "仓库",    1.3, -6.9, 8.3, 6.3},
  {"charging",   "充电区",  1.3,  0.6, 3.3, 6.3},
  {"power",      "配电室",  4.6,  0.6, 5.0, 2.8},
  {"control",    "中控室",  4.6,  3.6, 5.0, 3.3},
};

// 点位：全部选在可通行地面（走廊或区域内空地），避开货架/变压器/操作台/充电桩
inline const std::vector<SitePoint> kPoints = {
  {"warehouse-racks", "仓库-货架区",   5.0, -3.5, "warehouse",  nullptr},
  {"warehouse-dock",  "仓库-月台",     8.5, -1.5, "warehouse",  nullptr},
  {"a-line-1",        "车间A-1号产线", -7.0, -3.7, "workshop-a", nullptr},
  {"a-line-2",        "车间A-2号产线", -3.5, -3.7, "workshop-a", nullptr},
  {"a-west-gate",     "车间A-西门",    -8.47, -1.16, "workshop-a", nullptr},
  {"b-assembly",      "车间B-装配台",  -7.5,  3.8, "workshop-b", nullptr},
  {"b-east-gate",     "车间B-东门",    -2.5,  3.0, "workshop-b", nullptr},
  {"power-room",      "配电室",         7.1,  2.6, "power",      nullptr},
  {"boiler",          "锅炉房",         5.17, 5.35, "control",    nullptr},
  {"charger-1",       "充电区-1号桩",   1.9,  4.9, "charging",   "charger"},
  {"charger-2",       "充电区-2号桩",   3.2,  4.9, "charging",   "charger"},
  {"control-room",    "中控室",         7.44, 5.87, "control",    nullptr},
  {"north-gate",      "厂区北门",       0.0,  6.4, nullptr,      nullptr},
  {"ring-road",       "厂区环路",       0.0, -6.3, nullptr,      nullptr},
};

// 点位查找：先按 id 精确匹配，再按名称互相包含做模糊匹配
//（看板给 id、语音/人给中文名，两种都要能接住；找不到返回 nullptr）
inline const SitePoint *findSitePoint(const std::string &target_id)
{
  if (target_id.empty()) return nullptr;
  for (const auto &p : kPoints) {
    if (target_id == p.id) return &p;
  }
  for (const auto &p : kPoints) {
    if (target_id.find(p.name) != std::string::npos ||
        std::string(p.name).find(target_id) != std::string::npos) {
      return &p;
    }
  }
  return nullptr;
}

}  // namespace qmb
