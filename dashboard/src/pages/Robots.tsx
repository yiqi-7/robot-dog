import { useMemo } from "react";
import { format } from "date-fns";
import { Panel } from "@/components/Panel";
import { FloorMap } from "@/components/FloorMap";
import { CameraCard } from "@/components/CameraCard";
import { RobotControls } from "@/components/RobotControls";
import {
  AlarmLevelChip,
  BatteryArc,
  BatteryBar,
  Led,
  ROBOT_STATUS_COLORS,
  RobotStatusChip,
  TaskStatusChip,
  TaskTypeChip,
} from "@/components/hud";
import { useSim } from "@/sim/store";
import { trpc } from "@/providers/trpc";
import { cn } from "@/lib/utils";
import { TASK_MODE_LABELS } from "@contracts/task";

export default function Robots() {
  const { robots, selectedRobot: r, selectedId, selectRobot, alarms } = useSim();
  const allTasks = trpc.task.list.useQuery(undefined, { refetchInterval: 15_000 });

  const robotTasks = useMemo(
    () => (allTasks.data ?? []).filter((t) => t.robotId === r.id).slice(0, 8),
    [allTasks.data, r.id],
  );
  const robotAlarms = useMemo(
    () => alarms.filter((a) => a.source === r.id || a.source === r.name).slice(0, 6),
    [alarms, r.id, r.name],
  );

  return (
    <div className="grid gap-3 xl:grid-cols-[360px_minmax(0,1fr)]">
      {/* 左：机器人列表 */}
      <Panel
        title="机器人列表 / ROBOT LIST"
        right={<span className="font-hud text-[10px] text-muted-foreground">点击选择</span>}
        bodyClassName="p-0"
        className="self-start"
      >
        <ul className="divide-y divide-border/60">
          {robots.map((robot) => {
            const c = ROBOT_STATUS_COLORS[robot.status];
            const active = robot.id === selectedId;
            return (
              <li key={robot.id}>
                <button
                  onClick={() => selectRobot(robot.id)}
                  className={cn(
                    "w-full border-l-2 px-3 py-2.5 text-left transition-colors hover:bg-accent/50",
                    active ? "border-live bg-accent/60" : "border-transparent",
                  )}
                >
                  <div className="flex items-center justify-between gap-2">
                    <div className="flex items-center gap-2">
                      <Led color={c} />
                      <span className="font-hud text-sm font-semibold text-foreground">
                        {robot.id}
                      </span>
                      <span className="text-sm text-muted-foreground">{robot.name}</span>
                    </div>
                    <RobotStatusChip status={robot.status} />
                  </div>
                  <div className="mt-2 flex items-center gap-3">
                    <BatteryBar value={robot.battery} className="flex-1" />
                    <span className="font-hud text-xs tabular-nums text-muted-foreground">
                      {Math.round(robot.battery)}%
                    </span>
                    <span className="font-hud text-[11px] tabular-nums text-muted-foreground">
                      E{(robot.x * 0.5).toFixed(0)} N{(robot.y * 0.5).toFixed(0)}
                    </span>
                  </div>
                  <div className="mt-1 truncate text-[11px] text-muted-foreground">
                    {robot.currentTask ?? "空闲待命"} · {robot.model}
                  </div>
                </button>
              </li>
            );
          })}
        </ul>
      </Panel>

      {/* 右：选中机器人详情 */}
      <div className="flex min-w-0 flex-col gap-3">
        <Panel
          title={`机器人详情 / ${r.id} ${r.name}`}
          right={<RobotStatusChip status={r.status} />}
        >
          <div className="grid gap-4 md:grid-cols-[auto_minmax(0,1fr)]">
            <BatteryArc value={r.battery} size={128} />
            <div className="grid grid-cols-2 gap-px border border-border/60 bg-border/40 lg:grid-cols-3">
              {[
                { label: "型号", value: r.model, color: "#e6edf7" },
                {
                  label: "坐标 E / N",
                  value: `${(r.x * 0.5).toFixed(1)}m / ${(r.y * 0.5).toFixed(1)}m`,
                  color: "#22d3ee",
                },
                {
                  label: "速度",
                  value: `${(r.status === "working" ? r.speed * 0.18 : 0).toFixed(1)} m/s`,
                  color: "#e6edf7",
                },
                {
                  label: "电机温度",
                  value: `${r.motorTemp.toFixed(1)} °C`,
                  color: r.motorTemp > 70 ? "#f87171" : r.motorTemp > 55 ? "#fbbf24" : "#e6edf7",
                },
                { label: "信号强度", value: `${Math.round(r.signal)} dBm`, color: "#e6edf7" },
                { label: "累计运行", value: `${r.uptimeH.toFixed(1)} h`, color: "#e6edf7" },
              ].map((it) => (
                <div key={it.label} className="bg-card px-3 py-2.5">
                  <div className="label-caps">{it.label}</div>
                  <div
                    className="mt-1 truncate font-hud text-sm font-medium tabular-nums"
                    style={{ color: it.color }}
                  >
                    {it.value}
                  </div>
                </div>
              ))}
            </div>
          </div>

          <RobotControls robot={r} className="mt-4" />
        </Panel>

        <div className="grid gap-3 lg:grid-cols-2">
          <Panel title="摄像头画面 / CAMERA" bodyClassName="p-2">
            <CameraCard robot={r} />
          </Panel>
          <Panel title="位置追踪 / TRACKING" scan bodyClassName="p-0">
            <FloorMap focusId={r.id} />
          </Panel>
        </div>

        <div className="grid gap-3 lg:grid-cols-2">
          <Panel
            title="关联任务 / RELATED TASKS"
            bodyClassName="p-0"
            right={<span className="font-hud text-[10px] text-muted-foreground">{robotTasks.length} 条</span>}
          >
            {robotTasks.length === 0 ? (
              <div className="m-3 border border-dashed border-border px-3 py-5 text-center text-xs text-muted-foreground">
                该机器人暂无关联任务
              </div>
            ) : (
              <ul className="divide-y divide-border/60">
                {robotTasks.map((t) => (
                  <li key={t.id} className="flex items-center gap-2 px-3 py-2">
                    <TaskTypeChip type={t.type} />
                    <div className="min-w-0 flex-1">
                      <div className="truncate text-sm text-foreground">{t.title}</div>
                      <div className="mt-0.5 font-hud text-[10px] text-muted-foreground">
                        {TASK_MODE_LABELS[t.mode]}
                        {t.scheduledAt && ` · ${format(t.scheduledAt, "MM-dd HH:mm")}`}
                        {" · "}
                        {format(t.createdAt, "MM-dd HH:mm")} 创建
                      </div>
                    </div>
                    <TaskStatusChip status={t.status} />
                  </li>
                ))}
              </ul>
            )}
          </Panel>

          <Panel
            title="相关告警 / ALARMS"
            bodyClassName="p-0"
            right={<span className="font-hud text-[10px] text-muted-foreground">{robotAlarms.length} 条</span>}
          >
            {robotAlarms.length === 0 ? (
              <div className="m-3 border border-dashed border-border px-3 py-5 text-center text-xs text-muted-foreground">
                该机器人暂无告警记录
              </div>
            ) : (
              <ul className="divide-y divide-border/60">
                {robotAlarms.map((a) => (
                  <li key={a.id} className="flex items-start gap-2 px-3 py-2">
                    <AlarmLevelChip level={a.level} />
                    <div className="min-w-0 flex-1">
                      <div className="text-sm text-foreground">{a.message}</div>
                      <div className="mt-0.5 font-hud text-[10px] text-muted-foreground">
                        {format(a.time, "MM-dd HH:mm:ss")}
                        {a.acked && " · 已确认"}
                      </div>
                    </div>
                  </li>
                ))}
              </ul>
            )}
          </Panel>
        </div>
      </div>
    </div>
  );
}
