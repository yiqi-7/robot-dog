// 任务相关的共享常量与类型（前后端通用）

export const TASK_TYPES = ["move", "inspection", "charging"] as const;
export const TASK_MODES = ["immediate", "scheduled"] as const;
export const TASK_PRIORITIES = ["low", "normal", "high", "urgent"] as const;
export const TASK_STATUSES = [
  "draft",
  "published",
  "completed",
  "revoked",
] as const;

export type TaskType = (typeof TASK_TYPES)[number];
export type TaskMode = (typeof TASK_MODES)[number];
export type TaskPriority = (typeof TASK_PRIORITIES)[number];
export type TaskStatus = (typeof TASK_STATUSES)[number];

export const TASK_TYPE_LABELS: Record<TaskType, string> = {
  move: "移动",
  inspection: "巡检",
  charging: "充电",
};

export const TASK_MODE_LABELS: Record<TaskMode, string> = {
  immediate: "即时",
  scheduled: "定时",
};

export const TASK_PRIORITY_LABELS: Record<TaskPriority, string> = {
  low: "低",
  normal: "普通",
  high: "高",
  urgent: "紧急",
};

export const TASK_STATUS_LABELS: Record<TaskStatus, string> = {
  draft: "草稿",
  published: "已发布",
  completed: "已完成",
  revoked: "已撤销",
};

// 厂区点位（移动/巡检目标、充电桩）
export const SITE_POINTS = [
  "车间A-1号产线",
  "车间A-2号产线",
  "车间A-西门",
  "车间B-装配台",
  "车间B-东门",
  "仓库-货架区",
  "仓库-月台",
  "配电室",
  "锅炉房",
  "充电区-1号桩",
  "充电区-2号桩",
  "中控室",
  "厂区北门",
  "厂区环路",
] as const;

/** 任务对象（前端模型，日期为 Date；线上 JSON 形态见 src/mqtt/wire.ts TaskMsg） */
export interface Task {
  id: number;
  title: string;
  type: TaskType;
  mode: TaskMode;
  robotId: string | null;
  target: string | null;
  priority: TaskPriority;
  status: TaskStatus;
  scheduledAt: Date | null;
  description: string | null;
  createdAt: Date;
  updatedAt: Date;
  /** 任务来源：manual 看板手工 / voice 语音指令；缺省视为 manual */
  source?: "manual" | "voice";
}
