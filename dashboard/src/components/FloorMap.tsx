import { useSim } from "@/sim/store";
import { facilityStatus, type Robot } from "@/types";
import { ROBOT_STATUS_COLORS } from "./hud";
import { cn } from "@/lib/utils";

const FACILITY_COLORS = {
  normal: "#34d399",
  warning: "#fbbf24",
  critical: "#f87171",
} as const;

function RobotMarker({
  robot,
  selected,
  dimmed,
  u,
  onSelect,
}: {
  robot: Robot;
  selected: boolean;
  dimmed: boolean;
  /** 视觉尺寸因子：地图 px / 1200，保证任意尺寸地图下标记在屏幕上等大 */
  u: number;
  onSelect?: (id: string) => void;
}) {
  const color = ROBOT_STATUS_COLORS[robot.status];
  return (
    <g
      transform={`translate(${robot.x} ${robot.y})`}
      onClick={() => onSelect?.(robot.id)}
      className={cn("cursor-pointer transition-opacity duration-500")}
      opacity={dimmed ? 0.2 : 1}
    >
      {robot.status !== "idle" && (
        <circle r={9 * u} fill="none" stroke={color} strokeWidth={1 * u} className="map-ping" />
      )}
      {selected && (
        <circle
          r={14 * u}
          fill="none"
          stroke="#fbbf24"
          strokeWidth={1.2 * u}
          strokeDasharray={`${3 * u} ${3 * u}`}
        />
      )}
      <circle r={5 * u} fill={color} stroke="#050810" strokeWidth={1.5 * u} />
      <line
        x1="0"
        y1="0"
        x2={Math.cos(robot.heading) * 12 * u}
        y2={Math.sin(robot.heading) * 12 * u}
        stroke={color}
        strokeWidth={1.5 * u}
      />
      <text
        y={-14 * u}
        textAnchor="middle"
        fill={selected ? "#fbbf24" : "#c9d6ea"}
        fontSize={11 * u}
        fontFamily="'JetBrains Mono', monospace"
        letterSpacing={1 * u}
      >
        {robot.id}
      </text>
    </g>
  );
}

export function FloorMap({
  focusId,
  onSelect,
  className,
}: {
  /** 设置后仅高亮该机器人，其余淡化 */
  focusId?: string;
  onSelect?: (id: string) => void;
  className?: string;
}) {
  const { robots, facilities, selectedId, selectRobot, siteMap } = useSim();
  const zones = siteMap.zones;
  const points = siteMap.points;
  const handleSelect = onSelect ?? selectRobot;
  // 线缆坐标为米，画布按 1px = 0.5m 换算；viewBox 与地图同比例
  const mapW = siteMap.width > 0 ? siteMap.width : 600;
  const mapH = siteMap.height > 0 ? siteMap.height : 380;
  const W = mapW * 2;
  const H = mapH * 2;
  // 视觉尺寸因子：设计基准为 600m 宽（1200px）。地图实际尺寸任意时，
  // 所有图标/文字/线宽乘 u，保证屏幕上的显示尺寸恒定、不随地图缩放。
  const u = W / 1200;

  return (
    <div className={cn("relative", className)}>
      <svg viewBox={`0 0 ${W} ${H}`} className="block h-auto w-full">
        <defs>
          <pattern id="map-grid" width={40 * u} height={40 * u} patternUnits="userSpaceOnUse">
            <path
              d={`M ${40 * u} 0 L 0 0 0 ${40 * u}`}
              fill="none"
              stroke="#1a2540"
              strokeWidth={0.5 * u}
              opacity="0.5"
            />
          </pattern>
        </defs>

        {/* 底图网格 */}
        <rect x="0" y="0" width={W} height={H} fill="#070b16" />
        <rect x="0" y="0" width={W} height={H} fill="url(#map-grid)" />

        {/* 围墙 */}
        <rect
          x={24 * u}
          y={24 * u}
          width={W - 48 * u}
          height={H - 48 * u}
          fill="none"
          stroke="#2a3a5f"
          strokeWidth={2 * u}
        />
        <text x={40 * u} y={48 * u} fill="#54637f" fontSize={11 * u} fontFamily="'JetBrains Mono', monospace" letterSpacing={2 * u}>
          SITE PERIMETER / 厂区围墙
        </text>
        <text x={W - 70 * u} y={H - 34 * u} textAnchor="end" fill="#54637f" fontSize={11 * u} fontFamily="'JetBrains Mono', monospace" letterSpacing={2 * u}>
          N ↑  SCALE 1:500
        </text>

        {/* 功能区 */}
        {zones.map((z) => (
          <g key={z.id}>
            <rect
              x={z.x * 2}
              y={z.y * 2}
              width={z.width * 2}
              height={z.height * 2}
              fill="#111a2b"
              fillOpacity="0.55"
              stroke="#1a2540"
              strokeWidth={1.5 * u}
            />
            <text
              x={z.x * 2 + 10 * u}
              y={z.y * 2 + 22 * u}
              fill="#8b9bb8"
              fontSize={13 * u}
              fontFamily="'JetBrains Mono', 'PingFang SC', sans-serif"
              letterSpacing={3 * u}
            >
              {z.name}
            </text>
          </g>
        ))}

        {/* 充电桩符号 */}
        {points.filter((p) => p.kind === "charger").map((p, i) => (
          <g key={p.id} transform={`translate(${p.x * 2} ${p.y * 2})`}>
            <rect x={-9 * u} y={-7 * u} width={18 * u} height={14 * u} fill="none" stroke="#34d399" strokeWidth={1.2 * u} />
            <text y={3 * u} textAnchor="middle" fill="#34d399" fontSize={9 * u} fontFamily="'JetBrains Mono', monospace">
              ⚡{i + 1}
            </text>
          </g>
        ))}

        {/* 数据驱动任务点位 */}
        {points.filter((p) => p.kind !== "charger").map((p) => (
          <g key={p.id} transform={`translate(${p.x * 2} ${p.y * 2})`} opacity={0.55}>
            <circle r={3 * u} fill="#60a5fa" stroke="#0b1220" strokeWidth={1 * u} />
            <text x={7 * u} y={3 * u} fill="#7ea7d8" fontSize={9 * u} fontFamily="'JetBrains Mono', sans-serif">{p.name}</text>
          </g>
        ))}

        {/* MQTT 实时设施点位 */}
        {facilities.map((f) => {
          const st = facilityStatus(f);
          const c = FACILITY_COLORS[st];
          return (
            <g key={f.id} transform={`translate(${f.mx} ${f.my})`} opacity={focusId ? 0.35 : 1}>
              <rect x={-5 * u} y={-5 * u} width={10 * u} height={10 * u} fill={`${c}22`} stroke={c} strokeWidth={1.2 * u} />
              <text
                x={10 * u}
                y={4 * u}
                fill={c}
                fontSize={10 * u}
                fontFamily="'JetBrains Mono', 'PingFang SC', sans-serif"
              >
                {f.name} {f.temp.toFixed(1)}°
              </text>
            </g>
          );
        })}

        {/* 机器人轨迹 */}
        {robots.map((r) => {
          if (r.trail.length < 2) return null;
          const dimmed = focusId != null && r.id !== focusId;
          return (
            <polyline
              key={`trail-${r.id}`}
              points={r.trail.map((p) => `${p.x},${p.y}`).join(" ")}
              fill="none"
              stroke={ROBOT_STATUS_COLORS[r.status]}
              strokeWidth={1.2 * u}
              strokeDasharray={`${5 * u} ${6 * u}`}
              className="flow-line"
              opacity={dimmed ? 0.08 : 0.35}
            />
          );
        })}

        {/* 机器人 */}
        {robots.map((r) => (
          <RobotMarker
            key={r.id}
            robot={r}
            selected={r.id === (focusId ?? selectedId)}
            dimmed={focusId != null && r.id !== focusId}
            u={u}
            onSelect={handleSelect}
          />
        ))}
      </svg>
    </div>
  );
}
