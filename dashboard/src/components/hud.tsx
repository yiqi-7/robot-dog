import { cn } from "@/lib/utils";
import type { AlarmLevel, RobotStatus } from "@/types";
import { ALARM_LEVEL_LABELS, ROBOT_STATUS_LABELS } from "@/types";
import {
  TASK_MODE_LABELS,
  TASK_PRIORITY_LABELS,
  TASK_STATUS_LABELS,
  TASK_TYPE_LABELS,
  type TaskMode,
  type TaskPriority,
  type TaskStatus,
  type TaskType,
} from "@contracts/task";

/* ---------- LED ---------- */

export function Led({
  color,
  className,
}: {
  color: string;
  className?: string;
}) {
  return (
    <span
      className={cn("led shrink-0", className)}
      style={{ backgroundColor: color, color }}
    />
  );
}

/* ---------- 机器人状态 ---------- */

export const ROBOT_STATUS_COLORS: Record<RobotStatus, string> = {
  working: "#22d3ee",
  idle: "#8b9bb8",
  charging: "#34d399",
  fault: "#f87171",
};

export function RobotStatusChip({ status }: { status: RobotStatus }) {
  const c = ROBOT_STATUS_COLORS[status];
  return (
    <span className="status-chip font-hud" style={{ color: c, borderColor: `${c}55` }}>
      <Led color={c} />
      {ROBOT_STATUS_LABELS[status]}
    </span>
  );
}

/* ---------- 告警等级 ---------- */

export const ALARM_COLORS: Record<AlarmLevel, string> = {
  info: "#22d3ee",
  warning: "#fbbf24",
  critical: "#f87171",
};

export function AlarmLevelChip({ level }: { level: AlarmLevel }) {
  const c = ALARM_COLORS[level];
  return (
    <span className="status-chip font-hud" style={{ color: c, borderColor: `${c}55` }}>
      <Led color={c} />
      {ALARM_LEVEL_LABELS[level]}
    </span>
  );
}

/* ---------- 任务标签 ---------- */

const TYPE_COLORS: Record<TaskType, string> = {
  move: "#22d3ee",
  inspection: "#34d399",
  charging: "#fbbf24",
};

export function TaskTypeChip({ type }: { type: TaskType }) {
  const c = TYPE_COLORS[type];
  return (
    <span className="status-chip font-hud" style={{ color: c, borderColor: `${c}55` }}>
      {TASK_TYPE_LABELS[type]}
    </span>
  );
}

export function TaskModeChip({ mode }: { mode: TaskMode }) {
  const c = mode === "immediate" ? "#22d3ee" : "#c4b5fd";
  return (
    <span className="status-chip font-hud" style={{ color: c, borderColor: `${c}55` }}>
      {TASK_MODE_LABELS[mode]}
    </span>
  );
}

const PRIORITY_COLORS: Record<TaskPriority, string> = {
  low: "#8b9bb8",
  normal: "#22d3ee",
  high: "#fbbf24",
  urgent: "#f87171",
};

export function TaskPriorityChip({ priority }: { priority: TaskPriority }) {
  const c = PRIORITY_COLORS[priority];
  return (
    <span className="status-chip font-hud" style={{ color: c, borderColor: `${c}55` }}>
      <Led color={c} />
      {TASK_PRIORITY_LABELS[priority]}
    </span>
  );
}

const TASK_STATUS_COLORS: Record<TaskStatus, string> = {
  draft: "#8b9bb8",
  published: "#22d3ee",
  completed: "#34d399",
  revoked: "#64748b",
};

export function TaskStatusChip({ status }: { status: TaskStatus }) {
  const c = TASK_STATUS_COLORS[status];
  return (
    <span
      className={cn("status-chip font-hud", status === "revoked" && "line-through opacity-70")}
      style={{ color: c, borderColor: `${c}55` }}
    >
      {TASK_STATUS_LABELS[status]}
    </span>
  );
}

/* ---------- 电量 ---------- */

export function batteryColor(v: number) {
  if (v > 50) return "#34d399";
  if (v > 20) return "#fbbf24";
  return "#f87171";
}

export function BatteryBar({ value, className }: { value: number; className?: string }) {
  const c = batteryColor(value);
  return (
    <div className={cn("h-1.5 w-full bg-secondary", className)}>
      <div
        className="h-full transition-all duration-700"
        style={{ width: `${value}%`, backgroundColor: c, boxShadow: `0 0 6px ${c}66` }}
      />
    </div>
  );
}

export function BatteryArc({
  value,
  size = 116,
  label = "BATT",
}: {
  value: number;
  size?: number;
  label?: string;
}) {
  const stroke = 8;
  const r = (size - stroke) / 2;
  const c = 2 * Math.PI * r;
  const color = batteryColor(value);
  return (
    <div className="relative inline-flex items-center justify-center" style={{ width: size, height: size }}>
      <svg width={size} height={size} className="-rotate-90">
        <circle
          cx={size / 2}
          cy={size / 2}
          r={r}
          fill="none"
          stroke="hsl(var(--border))"
          strokeWidth={stroke}
        />
        <circle
          cx={size / 2}
          cy={size / 2}
          r={r}
          fill="none"
          stroke={color}
          strokeWidth={stroke}
          strokeDasharray={c}
          strokeDashoffset={c * (1 - value / 100)}
          strokeLinecap="butt"
          style={{ transition: "stroke-dashoffset 0.7s ease", filter: `drop-shadow(0 0 5px ${color}66)` }}
        />
      </svg>
      <div className="absolute inset-0 flex flex-col items-center justify-center">
        <span className="font-hud text-2xl font-semibold leading-none" style={{ color }}>
          {Math.round(value)}
          <span className="text-xs">%</span>
        </span>
        <span className="label-caps mt-1">{label}</span>
      </div>
    </div>
  );
}
