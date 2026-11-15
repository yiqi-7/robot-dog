import { useMemo, useState } from "react";
import { format } from "date-fns";
import {
  CartesianGrid,
  Line,
  LineChart,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";
import { Panel } from "@/components/Panel";
import { AlarmLevelChip, Led } from "@/components/hud";
import { useSim } from "@/sim/store";
import {
  FACILITY_STATUS_LABELS,
  facilityStatus,
  type AlarmLevel,
  type Facility,
} from "@/types";
import { cn } from "@/lib/utils";

const FACILITY_STATUS_COLORS = {
  normal: "#34d399",
  warning: "#fbbf24",
  critical: "#f87171",
} as const;

const LINE_COLORS = ["#22d3ee", "#fbbf24", "#34d399", "#f87171"];

function FacilityCard({ f }: { f: Facility }) {
  const st = facilityStatus(f);
  const c = FACILITY_STATUS_COLORS[st];
  // 阈值标尺：显示当前温度在 [base-6, critAt+4] 区间的位置
  const lo = f.base - 6;
  const hi = f.critAt + 4;
  const pct = Math.min(100, Math.max(0, ((f.temp - lo) / (hi - lo)) * 100));
  const warnPct = ((f.warnAt - lo) / (hi - lo)) * 100;
  const critPct = ((f.critAt - lo) / (hi - lo)) * 100;

  return (
    <div
      className="panel scanlines p-3"
      style={{ borderLeftWidth: 3, borderLeftColor: c }}
    >
      <div className="flex items-center justify-between">
        <span className="text-sm font-medium text-foreground">{f.name}</span>
        <span className="flex items-center gap-1.5 font-hud text-[10px]" style={{ color: c }}>
          <Led color={c} />
          {FACILITY_STATUS_LABELS[st]}
        </span>
      </div>
      <div className="mt-0.5 text-[11px] text-muted-foreground">{f.zone}</div>
      <div className="mt-2 flex items-baseline gap-1">
        <span className="font-hud text-2xl font-semibold tabular-nums" style={{ color: c }}>
          {f.temp.toFixed(1)}
        </span>
        <span className="font-hud text-xs text-muted-foreground">°C</span>
      </div>
      <div className="relative mt-2 h-1 bg-secondary">
        <div
          className="absolute inset-y-0 left-0 transition-all duration-700"
          style={{ width: `${pct}%`, backgroundColor: c }}
        />
        <span className="absolute -top-0.5 h-2 w-px bg-[#fbbf24]" style={{ left: `${warnPct}%` }} />
        <span className="absolute -top-0.5 h-2 w-px bg-[#f87171]" style={{ left: `${critPct}%` }} />
      </div>
      <div className="mt-1.5 flex justify-between font-hud text-[9px] text-muted-foreground">
        <span>预警 {f.warnAt}°</span>
        <span>临界 {f.critAt}°</span>
      </div>
    </div>
  );
}

export default function Alarms() {
  const { facilities, temps, alarms, ackAlarm, ackAll } = useSim();
  const [selected, setSelected] = useState<string[]>(["f1", "f5", "f7"]);
  const [levelFilter, setLevelFilter] = useState<"all" | AlarmLevel>("all");
  const [onlyUnacked, setOnlyUnacked] = useState(false);

  const toggleFacility = (id: string) =>
    setSelected((prev) => {
      if (prev.includes(id)) return prev.filter((x) => x !== id);
      if (prev.length >= 4) return [...prev.slice(1), id];
      return [...prev, id];
    });

  const filtered = useMemo(
    () =>
      alarms.filter(
        (a) =>
          (levelFilter === "all" || a.level === levelFilter) &&
          (!onlyUnacked || !a.acked),
      ),
    [alarms, levelFilter, onlyUnacked],
  );
  const unacked = alarms.filter((a) => !a.acked).length;

  const selectedFacilities = selected
    .map((id) => facilities.find((f) => f.id === id))
    .filter((f): f is Facility => f != null);

  return (
    <div className="flex flex-col gap-3">
      {/* 厂区设施状态 */}
      <Panel
        title="厂区设施 / FACILITIES"
        right={
          <span className="font-hud text-[10px] text-muted-foreground">
            {facilities.filter((f) => facilityStatus(f) !== "normal").length} 项异常
          </span>
        }
      >
        <div className="grid grid-cols-2 gap-3 md:grid-cols-3 xl:grid-cols-4">
          {facilities.map((f) => (
            <FacilityCard key={f.id} f={f} />
          ))}
        </div>
      </Panel>

      {/* 温控曲线 */}
      <Panel
        title="温控曲线 / TEMPERATURE · 24H"
        right={
          <div className="flex flex-wrap items-center gap-1.5">
            {facilities.map((f) => {
              const idx = selected.indexOf(f.id);
              const active = idx >= 0;
              const c = active ? LINE_COLORS[idx % LINE_COLORS.length] : undefined;
              return (
                <button
                  key={f.id}
                  onClick={() => toggleFacility(f.id)}
                  className={cn(
                    "border px-2 py-0.5 font-hud text-[10px] tracking-wider transition-colors",
                    active
                      ? "border-current"
                      : "border-border text-muted-foreground hover:text-foreground",
                  )}
                  style={c ? { color: c } : undefined}
                  title={`基准 ${f.base}°C`}
                >
                  {f.name}
                </button>
              );
            })}
            <span key="hint" className="font-hud text-[9px] text-muted-foreground">最多 4 条</span>
          </div>
        }
      >
        <div className="h-[260px] w-full">
          <ResponsiveContainer width="100%" height="100%">
            <LineChart data={temps} margin={{ top: 8, right: 12, bottom: 0, left: -8 }}>
              <CartesianGrid stroke="#1a2540" strokeDasharray="3 6" vertical={false} />
              <XAxis
                dataKey="label"
                tick={{ fill: "#8b9bb8", fontSize: 10, fontFamily: "JetBrains Mono" }}
                tickLine={false}
                axisLine={{ stroke: "#1a2540" }}
                interval={7}
              />
              <YAxis
                tick={{ fill: "#8b9bb8", fontSize: 10, fontFamily: "JetBrains Mono" }}
                tickLine={false}
                axisLine={false}
                width={52}
                tickFormatter={(v: number) => `${v}°`}
              />
              <Tooltip
                contentStyle={{
                  background: "#0c1220",
                  border: "1px solid #1a2540",
                  borderRadius: 2,
                  fontFamily: "JetBrains Mono",
                  fontSize: 11,
                }}
                labelStyle={{ color: "#8b9bb8" }}
                formatter={(value: number | string, name: string) => {
                  const f = facilities.find((x) => x.id === name);
                  return [`${value}°C`, f?.name ?? name];
                }}
              />
              {selectedFacilities.map((f, i) => (
                <ReferenceLine
                  key={`ref-${f.id}`}
                  y={f.warnAt}
                  stroke={LINE_COLORS[i % LINE_COLORS.length]}
                  strokeDasharray="4 6"
                  strokeOpacity={0.35}
                />
              ))}
              {selectedFacilities.map((f, i) => (
                <Line
                  key={f.id}
                  type="monotone"
                  dataKey={f.id}
                  stroke={LINE_COLORS[i % LINE_COLORS.length]}
                  strokeWidth={1.6}
                  dot={false}
                  isAnimationActive={false}
                />
              ))}
            </LineChart>
          </ResponsiveContainer>
        </div>
        <div className="mt-1 font-hud text-[10px] text-muted-foreground">
          虚线为对应设施的预警阈值 · 每 30 分钟一格 · 实时滚动更新
        </div>
      </Panel>

      {/* 告警列表 */}
      <Panel
        title="告警列表 / ALARM LIST"
        right={
          <div className="flex items-center gap-2">
            <div className="flex border border-input">
              {(["all", "critical", "warning", "info"] as const).map((l) => (
                <button
                  key={l}
                  onClick={() => setLevelFilter(l)}
                  className={cn(
                    "px-2.5 py-1 font-hud text-[11px] transition-colors",
                    levelFilter === l
                      ? "bg-[hsl(var(--live)/0.15)] text-live"
                      : "text-muted-foreground hover:text-foreground",
                  )}
                >
                  {l === "all" ? "全部" : l === "critical" ? "严重" : l === "warning" ? "警告" : "提示"}
                </button>
              ))}
            </div>
            <button
              onClick={() => setOnlyUnacked((v) => !v)}
              className={cn(
                "border px-2.5 py-1 font-hud text-[11px] transition-colors",
                onlyUnacked
                  ? "border-crit/60 text-crit"
                  : "border-border text-muted-foreground hover:text-foreground",
              )}
            >
              仅未处理 {unacked > 0 && `(${unacked})`}
            </button>
            <button
              onClick={ackAll}
              className="border border-border px-2.5 py-1 font-hud text-[11px] text-muted-foreground transition-colors hover:text-foreground"
            >
              全部确认
            </button>
          </div>
        }
        bodyClassName="p-0"
      >
        <ul className="divide-y divide-border/40">
          {filtered.map((a) => (
            <li
              key={a.id}
              className={cn(
                "flex items-start gap-3 border-l-[3px] px-3 py-2.5 transition-colors",
                a.acked && "opacity-50",
              )}
              style={{
                borderLeftColor:
                  a.level === "critical" ? "#f87171" : a.level === "warning" ? "#fbbf24" : "#22d3ee",
              }}
            >
              <AlarmLevelChip level={a.level} />
              <div className="min-w-0 flex-1">
                <div className="flex flex-wrap items-baseline gap-x-2">
                  <span className="font-hud text-xs font-semibold text-foreground">{a.source}</span>
                  <span className="font-hud text-[10px] tabular-nums text-muted-foreground">
                    {format(a.time, "MM-dd HH:mm:ss")}
                  </span>
                </div>
                <div className="mt-0.5 text-sm text-foreground/85">{a.message}</div>
              </div>
              {a.acked ? (
                <span className="font-hud text-[11px] text-muted-foreground">已确认</span>
              ) : (
                <button
                  onClick={() => ackAlarm(a.id)}
                  className="h-7 shrink-0 border border-border px-2.5 font-hud text-[11px] text-warn transition-colors hover:bg-[hsl(var(--warn)/0.1)]"
                >
                  确认
                </button>
              )}
            </li>
          ))}
          {filtered.length === 0 && (
            <li className="px-3 py-10 text-center text-xs text-muted-foreground">
              当前筛选条件下暂无告警
            </li>
          )}
        </ul>
      </Panel>
    </div>
  );
}
