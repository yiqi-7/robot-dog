export type SiteMapZone = { id: string; name: string; x: number; y: number; w: number; h: number };
export type SiteMapPoint = { id: string; name: string; x: number; y: number; zone?: string; kind?: "waypoint" | "charger" };

import type { SiteMapMsg } from "@/mqtt/wire";

export const SITE_MAP_ZONES: SiteMapZone[] = [
  { id: "workshop-a", name: "车间A", x: 70, y: 70, w: 460, h: 280 },
  { id: "workshop-b", name: "车间B", x: 70, y: 390, w: 460, h: 300 },
  { id: "warehouse", name: "仓库", x: 570, y: 70, w: 560, h: 250 },
  { id: "charging", name: "充电区", x: 570, y: 360, w: 260, h: 160 },
  { id: "power", name: "配电室", x: 860, y: 360, w: 270, h: 160 },
  { id: "control", name: "中控室", x: 570, y: 560, w: 560, h: 130 },
];

export const SITE_MAP_POINTS: SiteMapPoint[] = [
  { id: "a-line-1", name: "车间A-1号产线", x: 180, y: 170, zone: "workshop-a" },
  { id: "a-line-2", name: "车间A-2号产线", x: 390, y: 170, zone: "workshop-a" },
  { id: "a-west-gate", name: "车间A-西门", x: 70, y: 250, zone: "workshop-a" },
  { id: "b-assembly", name: "车间B-装配台", x: 260, y: 520, zone: "workshop-b" },
  { id: "b-east-gate", name: "车间B-东门", x: 530, y: 500, zone: "workshop-b" },
  { id: "warehouse-racks", name: "仓库-货架区", x: 760, y: 170, zone: "warehouse" },
  { id: "warehouse-dock", name: "仓库-月台", x: 1060, y: 270, zone: "warehouse" },
  { id: "power-room", name: "配电室", x: 990, y: 440, zone: "power" },
  { id: "boiler", name: "锅炉房", x: 900, y: 600, zone: "control" },
  { id: "charger-1", name: "充电区-1号桩", x: 645, y: 472, zone: "charging", kind: "charger" },
  { id: "charger-2", name: "充电区-2号桩", x: 755, y: 472, zone: "charging", kind: "charger" },
  { id: "control-room", name: "中控室", x: 760, y: 620, zone: "control" },
  { id: "north-gate", name: "厂区北门", x: 600, y: 24 },
  { id: "ring-road", name: "厂区环路", x: 1140, y: 360 },
];

/**
 * 看板内置默认地图（米制，与 {P}/site/map 线缆格式一致）。
 * 机器人端/调度服务未推送地图配置时用它渲染；收到 retained 配置后整体替换。
 */
export const DEFAULT_SITE_MAP: SiteMapMsg = {
  version: 1,
  width: 600,
  height: 380,
  zones: SITE_MAP_ZONES.map((z) => ({ id: z.id, name: z.name, x: z.x / 2, y: z.y / 2, width: z.w / 2, height: z.h / 2 })),
  points: SITE_MAP_POINTS.map((p) => ({ id: p.id, name: p.name, x: p.x / 2, y: p.y / 2, kind: p.kind, zone: p.zone })),
  updatedAt: 0,
};
