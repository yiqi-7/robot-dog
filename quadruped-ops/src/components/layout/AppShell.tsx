import type { ReactNode } from "react";
import { NavLink } from "react-router";
import { useSim } from "@/sim/store";
import { ALARM_COLORS } from "@/components/hud";
import { cn } from "@/lib/utils";

function RadarLogo() {
  return (
    <svg width="30" height="30" viewBox="0 0 32 32" className="shrink-0">
      <circle cx="16" cy="16" r="14" fill="none" stroke="#22d3ee" strokeWidth="1.2" opacity="0.5" />
      <circle cx="16" cy="16" r="9" fill="none" stroke="#22d3ee" strokeWidth="0.8" opacity="0.3" />
      <circle cx="16" cy="16" r="4" fill="none" stroke="#22d3ee" strokeWidth="0.8" opacity="0.3" />
      <line x1="16" y1="16" x2="16" y2="2" stroke="#22d3ee" strokeWidth="1.5" className="radar-sweep" />
      <circle cx="16" cy="16" r="2" fill="#22d3ee" />
      <circle cx="22" cy="10" r="1.5" fill="#fbbf24" />
      <circle cx="9" cy="20" r="1.5" fill="#34d399" />
    </svg>
  );
}

const NAV = [
  { to: "/", label: "总览", en: "OVERVIEW" },
  { to: "/robots", label: "机器人", en: "ROBOTS" },
  { to: "/tasks", label: "任务管理", en: "TASKS" },
  { to: "/alarms", label: "告警中心", en: "ALARMS" },
];

function AlertTicker() {
  const { alarms } = useSim();
  const items = alarms.slice(0, 10);
  const doubled = [...items, ...items];
  return (
    <div className="flex items-stretch border-b border-border bg-[#0a0f1c]">
      <div className="flex shrink-0 items-center gap-2 border-r border-border px-3">
        <span className="led bg-[#f87171] text-[#f87171]" />
        <span className="label-caps">Alert Feed</span>
      </div>
      <div className="relative flex-1 overflow-hidden">
        <div className="ticker-track flex w-max items-center gap-8 px-4 py-1.5">
          {doubled.map((a, i) => (
            <span key={`${a.id}-${i}`} className="flex items-center gap-2 whitespace-nowrap font-hud text-[11px]">
              <span className="text-muted-foreground">
                {a.time.toLocaleTimeString("zh-CN", { hour12: false, hour: "2-digit", minute: "2-digit" })}
              </span>
              <span style={{ color: ALARM_COLORS[a.level] }}>
                [{a.level === "critical" ? "严重" : a.level === "warning" ? "警告" : "提示"}]
              </span>
              <span className="text-foreground/85">
                {a.source}：{a.message}
              </span>
            </span>
          ))}
        </div>
      </div>
    </div>
  );
}

export function AppShell({ children }: { children: ReactNode }) {
  const { now, robots, busStatus } = useSim();
  const online = robots.filter((r) => r.status !== "fault").length;

  return (
    <div className="flex min-h-full flex-col">
      {/* 顶栏 */}
      <header className="flex h-14 items-center justify-between gap-4 border-b border-border bg-[#0a0f1c]/90 px-4">
        <div className="flex items-center gap-3">
          <RadarLogo />
          <div>
            <div className="font-hud text-sm font-semibold tracking-[0.25em] text-foreground">
              厂区监控运维看板
            </div>
            <div className="label-caps" style={{ fontSize: 9 }}>
              Quadruped Ops Console · 厂区一期
            </div>
          </div>
        </div>

        <nav className="hidden items-center gap-1 md:flex">
          {NAV.map((n) => (
            <NavLink
              key={n.to}
              to={n.to}
              end={n.to === "/"}
              className={({ isActive }) =>
                cn(
                  "border-b-2 px-3 py-1.5 font-hud text-xs tracking-[0.2em] transition-colors",
                  isActive
                    ? "border-[hsl(var(--live))] text-live text-glow-live"
                    : "border-transparent text-muted-foreground hover:text-foreground",
                )
              }
            >
              {n.label}
            </NavLink>
          ))}
        </nav>

        <div className="flex items-center gap-4">
          <div className="hidden items-center gap-2 sm:flex">
            <span className="label-caps">在线</span>
            <span className="font-hud text-sm text-live">
              {online}
              <span className="text-muted-foreground">/{robots.length}</span>
            </span>
          </div>
          <div className="text-right">
            <div className="font-hud text-sm tabular-nums text-foreground">
              {now.toLocaleTimeString("zh-CN", { hour12: false })}
            </div>
            <div className="font-hud text-[10px] text-muted-foreground">
              {now.toLocaleDateString("zh-CN", { month: "2-digit", day: "2-digit", weekday: "short" })}
            </div>
          </div>
          <span className="flex items-center gap-1.5 border border-live px-2 py-0.5 font-hud text-[10px] tracking-[0.2em] text-live">
            <span className="led bg-[#22d3ee] text-[#22d3ee]" />
            LIVE
          </span>
        </div>
      </header>

      <AlertTicker />

      {/* 移动端导航 */}
      <nav className="flex items-center gap-1 border-b border-border bg-[#0a0f1c] px-2 py-1 md:hidden">
        {NAV.map((n) => (
          <NavLink
            key={n.to}
            to={n.to}
            end={n.to === "/"}
            className={({ isActive }) =>
              cn(
                "rounded-sm px-2 py-1 font-hud text-xs",
                isActive ? "bg-accent text-live" : "text-muted-foreground",
              )
            }
          >
            {n.label}
          </NavLink>
        ))}
      </nav>

      <main className="flex-1 p-3">{children}</main>

      <footer className="flex items-center justify-between border-t border-border px-4 py-2">
        <span className="label-caps">Quadruped Ops Console v1.0</span>
        <span className="label-caps">
          数据链路：MQTT ·{" "}
          {busStatus === "mock"
            ? "内置模拟"
            : busStatus === "connected"
              ? "Broker 已连接"
              : busStatus === "connecting"
                ? "连接中…"
                : "已断线"}
        </span>
      </footer>
    </div>
  );
}
