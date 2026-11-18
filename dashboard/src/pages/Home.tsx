import type { ReactNode } from "react";
import { format } from "date-fns";
import { Panel } from "@/components/Panel";
import { FloorMap } from "@/components/FloorMap";
import { CameraCard } from "@/components/CameraCard";
import { VoicePanel } from "@/components/VoicePanel";
import { RobotControls } from "@/components/RobotControls";
import {
  BatteryArc,
  BatteryBar,
  Led,
  ROBOT_STATUS_COLORS,
  RobotStatusChip,
  TaskModeChip,
  TaskPriorityChip,
  TaskTypeChip,
} from "@/components/hud";
import { useSim } from "@/sim/store";
import { ROBOT_STATUS_LABELS, type Robot } from "@/types";
import { trpc } from "@/providers/trpc";
import { cn } from "@/lib/utils";

function Stat({
  label,
  value,
  unit,
  color,
}: {
  label: string;
  value: ReactNode;
  unit?: string;
  color: string;
}) {
  return (
    <div className="panel scanlines flex items-center justify-between px-4 py-3">
      <span className="label-caps">{label}</span>
      <span className="font-hud text-2xl font-semibold tabular-nums" style={{ color }}>
        {value}
        {unit && <span className="ml-0.5 text-xs text-muted-foreground">{unit}</span>}
      </span>
    </div>
  );
}

function RobotRow({ robot, selected }: { robot: Robot; selected: boolean }) {
  const { selectRobot } = useSim();
  const c = ROBOT_STATUS_COLORS[robot.status];
  return (
    <button
      onClick={() => selectRobot(robot.id)}
      className={cn(
        "flex w-full items-center gap-2.5 border border-transparent px-2 py-2 text-left transition-colors hover:bg-accent/60",
        selected && "border-live/50 bg-accent/70",
      )}
    >
      <Led color={c} />
      <div className="min-w-0 flex-1">
        <div className="flex items-baseline gap-1.5">
          <span className="font-hud text-xs font-semibold text-foreground">{robot.id}</span>
          <span className="truncate text-xs text-muted-foreground">{robot.name}</span>
        </div>
        <BatteryBar value={robot.battery} className="mt-1.5" />
      </div>
      <span
        className="font-hud text-xs tabular-nums"
        style={{ color: robot.battery > 50 ? "#34d399" : robot.battery > 20 ? "#fbbf24" : "#f87171" }}
      >
        {Math.round(robot.battery)}%
      </span>
    </button>
  );
}

export default function Home() {
  const { robots, selectedRobot, alarms } = useSim();
  const pending = trpc.task.pending.useQuery(undefined, {
    refetchInterval: 15_000,
  });

  const online = robots.filter((r) => r.status !== "fault").length;
  const avgBattery = robots.reduce((s, r) => s + r.battery, 0) / robots.length;
  const unacked = alarms.filter((a) => !a.acked).length;
  const pendingTasks = pending.data ?? [];
  const r = selectedRobot;
  const speedMs = r.status === "working" ? r.speed * 0.18 : 0;

  return (
    <div className="flex flex-col gap-3">
      {/* 统计条 */}
      <div className="grid grid-cols-2 gap-3 lg:grid-cols-4">
        <Stat label="在线机器人" value={`${online}/${robots.length}`} color="#22d3ee" />
        <Stat label="平均电量" value={Math.round(avgBattery)} unit="%" color="#34d399" />
        <Stat label="待执行任务" value={pendingTasks.length} color="#fbbf24" />
        <Stat
          label="未处理告警"
          value={unacked}
          color={unacked > 0 ? "#f87171" : "#34d399"}
        />
      </div>

      <div className="grid gap-3 lg:grid-cols-[280px_minmax(0,1fr)_380px]">
        {/* 左：机器人队列 */}
        <Panel
          title="机器人队列 / FLEET"
          right={<span className="font-hud text-[10px] text-muted-foreground">{robots.length} 台</span>}
          bodyClassName="p-1"
          className="self-start"
        >
          <div className="flex flex-col">
            {robots.map((robot) => (
              <RobotRow key={robot.id} robot={robot} selected={robot.id === r.id} />
            ))}
          </div>
          <div className="mt-2 flex flex-wrap gap-x-3 gap-y-1 border-t border-border/60 px-2 py-2">
            {(Object.keys(ROBOT_STATUS_COLORS) as (keyof typeof ROBOT_STATUS_COLORS)[]).map(
              (s) => (
                <span key={s} className="flex items-center gap-1 text-[10px] text-muted-foreground">
                  <Led color={ROBOT_STATUS_COLORS[s]} />
                  {ROBOT_STATUS_LABELS[s]}
                </span>
              ),
            )}
          </div>
        </Panel>

        {/* 中：地图 + 待执行任务 */}
        <div className="flex min-w-0 flex-col gap-3">
          <Panel
            title="厂区平面图 / SITE MAP"
            scan
            right={
              <span className="font-hud text-[10px] text-live">实时位置 · 700ms 刷新</span>
            }
            bodyClassName="p-0"
          >
            <FloorMap />
          </Panel>

          <Panel
            title="待执行任务 / PENDING TASKS"
            right={
              <span className="font-hud text-[10px] text-warn">
                {pendingTasks.length} 项待调度
              </span>
            }
            bodyClassName="p-0"
          >
            {pendingTasks.length === 0 ? (
              <div className="border-dashed m-3 border border-border px-3 py-6 text-center text-xs text-muted-foreground">
                暂无待执行任务 — 可在「任务管理」中创建并发布
              </div>
            ) : (
              <ul className="divide-y divide-border/60">
                {pendingTasks.slice(0, 6).map((t) => (
                  <li key={t.id} className="flex items-center gap-3 px-3 py-2">
                    <TaskPriorityChip priority={t.priority} />
                    <div className="min-w-0 flex-1">
                      <div className="truncate text-sm text-foreground">{t.title}</div>
                      <div className="mt-0.5 flex items-center gap-2 text-[11px] text-muted-foreground">
                        <span className="font-hud">{t.robotId ?? "未指派"}</span>
                        <span>→</span>
                        <span className="truncate">{t.target ?? "—"}</span>
                        {t.scheduledAt && (
                          <span className="font-hud text-warn">
                            {format(t.scheduledAt, "MM-dd HH:mm")} 执行
                          </span>
                        )}
                      </div>
                    </div>
                    <TaskTypeChip type={t.type} />
                    <TaskModeChip mode={t.mode} />
                  </li>
                ))}
              </ul>
            )}
          </Panel>

          <VoicePanel />
        </div>

        {/* 右：选中机器人 + 摄像头 */}
        <div className="flex min-w-0 flex-col gap-3">
          <Panel
            title={`选中机器人 / ${r.id}`}
            right={<RobotStatusChip status={r.status} />}
          >
            <div className="flex items-center justify-between gap-2">
              <div>
                <div className="text-base font-semibold text-foreground">
                  {r.name}
                  <span className="ml-2 font-hud text-xs text-muted-foreground">{r.model}</span>
                </div>
                <div className="label-caps mt-1">UNIT TELEMETRY</div>
              </div>
              <BatteryArc value={r.battery} size={96} />
            </div>

            <div className="mt-3 grid grid-cols-2 gap-px border border-border/60 bg-border/40">
              {[
                {
                  label: "坐标 E / N",
                  value: `${(r.x * 0.5).toFixed(1)}m / ${(r.y * 0.5).toFixed(1)}m`,
                  color: "#22d3ee",
                },
                { label: "速度", value: `${speedMs.toFixed(1)} m/s`, color: "#e6edf7" },
                {
                  label: "电机温度",
                  value: `${r.motorTemp.toFixed(1)} °C`,
                  color: r.motorTemp > 70 ? "#f87171" : r.motorTemp > 55 ? "#fbbf24" : "#e6edf7",
                },
                {
                  label: "信号强度",
                  value: `${Math.round(r.signal)} dBm`,
                  color: r.signal < -68 ? "#fbbf24" : "#e6edf7",
                },
                { label: "累计运行", value: `${r.uptimeH.toFixed(1)} h`, color: "#e6edf7" },
                { label: "云台视角", value: `CAM0${r.cam + 1}`, color: "#e6edf7" },
              ].map((it) => (
                <div key={it.label} className="bg-card px-3 py-2">
                  <div className="label-caps">{it.label}</div>
                  <div
                    className="mt-1 font-hud text-sm font-medium tabular-nums"
                    style={{ color: it.color }}
                  >
                    {it.value}
                  </div>
                </div>
              ))}
            </div>

            <div className="mt-3 border-l-2 border-live bg-accent/40 px-3 py-2">
              <div className="label-caps">当前任务</div>
              <div className="mt-1 text-sm text-foreground">
                {r.currentTask ?? "空闲待命"}
              </div>
            </div>

            <RobotControls robot={r} className="mt-3" />
          </Panel>

          <Panel title="摄像头画面 / CAMERA" bodyClassName="p-2">
            <CameraCard robot={r} />
          </Panel>
        </div>
      </div>
    </div>
  );
}
