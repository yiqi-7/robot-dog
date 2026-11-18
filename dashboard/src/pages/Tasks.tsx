import { useMemo, useState } from "react";
import { format } from "date-fns";
import { toast } from "sonner";
import { Panel } from "@/components/Panel";
import { TaskFormDialog } from "@/components/TaskFormDialog";
import {
  TaskModeChip,
  TaskPriorityChip,
  TaskStatusChip,
  TaskTypeChip,
} from "@/components/hud";
import { Button } from "@/components/ui/button";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { trpc } from "@/providers/trpc";
import { useSim } from "@/sim/store";
import { cn } from "@/lib/utils";
import {
  TASK_MODE_LABELS,
  TASK_MODES,
  TASK_STATUSES,
  TASK_STATUS_LABELS,
  TASK_TYPES,
  TASK_TYPE_LABELS,
  type TaskMode,
  type TaskStatus,
  type TaskType,
} from "@contracts/task";
import type { Task } from "@contracts/task";

function RowActions({
  task,
  onEdit,
}: {
  task: Task;
  onEdit: (t: Task) => void;
}) {
  const utils = trpc.useUtils();
  const [confirming, setConfirming] = useState(false);

  const invalidate = () => {
    utils.task.list.invalidate();
    utils.task.pending.invalidate();
  };
  const publish = trpc.task.publish.useMutation({
    onSuccess: () => {
      toast.success(`任务 #${task.id} 已发布`);
      invalidate();
    },
    onError: (e) => toast.error(e.message),
  });
  const revoke = trpc.task.revoke.useMutation({
    onSuccess: () => {
      toast.success(`任务 #${task.id} 已撤销`);
      setConfirming(false);
      invalidate();
    },
    onError: (e) => toast.error(e.message),
  });
  const remove = trpc.task.remove.useMutation({
    onSuccess: () => {
      toast.success(`任务 #${task.id} 已删除`);
      invalidate();
    },
    onError: (e) => toast.error(e.message),
  });

  const busy = publish.isPending || revoke.isPending || remove.isPending;
  const btn =
    "h-7 border border-border px-2 font-hud text-[11px] tracking-wider transition-colors disabled:opacity-40";

  if (task.status === "revoked" || task.status === "completed") {
    return <span className="font-hud text-[11px] text-muted-foreground">—</span>;
  }

  return (
    <div className="flex items-center justify-end gap-1.5">
      {task.status === "draft" && (
        <button
          className={cn(btn, "border-live/60 text-live hover:bg-[hsl(var(--live)/0.12)]")}
          disabled={busy}
          onClick={() => publish.mutate({ id: task.id })}
        >
          发布
        </button>
      )}
      <button
        className={cn(btn, "text-muted-foreground hover:text-foreground")}
        disabled={busy}
        onClick={() => onEdit(task)}
      >
        修改
      </button>
      {confirming ? (
        <>
          <button
            className={cn(btn, "border-crit/60 text-crit hover:bg-[hsl(var(--crit)/0.12)]")}
            disabled={busy}
            onClick={() => revoke.mutate({ id: task.id })}
          >
            确认撤销
          </button>
          <button
            className={cn(btn, "text-muted-foreground")}
            onClick={() => setConfirming(false)}
          >
            取消
          </button>
        </>
      ) : (
        <button
          className={cn(btn, "text-warn hover:bg-[hsl(var(--warn)/0.1)]")}
          disabled={busy}
          onClick={() => setConfirming(true)}
        >
          撤销
        </button>
      )}
      {task.status === "draft" && (
        <button
          className={cn(btn, "text-muted-foreground hover:text-crit")}
          disabled={busy}
          onClick={() => remove.mutate({ id: task.id })}
        >
          删除
        </button>
      )}
    </div>
  );
}

export default function Tasks() {
  const list = trpc.task.list.useQuery(undefined, { refetchInterval: 10_000 });
  const { siteMap } = useSim();
  const [modeFilter, setModeFilter] = useState<"all" | TaskMode>("all");
  const [typeFilter, setTypeFilter] = useState<"all" | TaskType>("all");
  const [statusFilter, setStatusFilter] = useState<"all" | TaskStatus>("all");
  const [dialogOpen, setDialogOpen] = useState(false);
  const [editing, setEditing] = useState<Task | null>(null);

  /** 目标点位 ID -> 名称（旧任务存的是中文名，原样显示） */
  const pointName = useMemo(() => {
    const m = new Map(siteMap.points.map((p) => [p.id, p.name]));
    return (target: string | null) => (target ? (m.get(target) ?? target) : null);
  }, [siteMap]);

  const tasks = useMemo(() => {
    return (list.data ?? []).filter(
      (t) =>
        (modeFilter === "all" || t.mode === modeFilter) &&
        (typeFilter === "all" || t.type === typeFilter) &&
        (statusFilter === "all" || t.status === statusFilter),
    );
  }, [list.data, modeFilter, typeFilter, statusFilter]);

  const counts = useMemo(() => {
    const all = list.data ?? [];
    return {
      all: all.length,
      published: all.filter((t) => t.status === "published").length,
      draft: all.filter((t) => t.status === "draft").length,
    };
  }, [list.data]);

  return (
    <div className="flex flex-col gap-3">
      <Panel
        title="任务管理 / TASK MANAGEMENT"
        right={
          <div className="flex items-center gap-3">
            <span className="font-hud text-[10px] text-muted-foreground">
              共 {counts.all} 条 · 已发布 {counts.published} · 草稿 {counts.draft}
            </span>
            <Button
              size="sm"
              onClick={() => {
                setEditing(null);
                setDialogOpen(true);
              }}
              className="h-7 bg-[hsl(var(--live))] px-3 font-hud text-xs text-[hsl(var(--primary-foreground))] hover:bg-[hsl(var(--live)/0.85)]"
            >
              + 新建任务
            </Button>
          </div>
        }
        bodyClassName="p-0"
      >
        {/* 筛选条 */}
        <div className="flex flex-wrap items-center gap-3 border-b border-border/60 px-3 py-2">
          <div className="flex border border-input">
            {(["all", ...TASK_MODES] as const).map((m) => (
              <button
                key={m}
                onClick={() => setModeFilter(m)}
                className={cn(
                  "px-3 py-1 font-hud text-[11px] tracking-wider transition-colors",
                  modeFilter === m
                    ? "bg-[hsl(var(--live)/0.15)] text-live"
                    : "text-muted-foreground hover:text-foreground",
                )}
              >
                {m === "all" ? "全部模式" : TASK_MODE_LABELS[m]}
              </button>
            ))}
          </div>
          <Select value={typeFilter} onValueChange={(v) => setTypeFilter(v as typeof typeFilter)}>
            <SelectTrigger className="h-7 w-[130px] border-input bg-background/60 font-hud text-xs">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="all">全部类别</SelectItem>
              {TASK_TYPES.map((t) => (
                <SelectItem key={t} value={t}>
                  {TASK_TYPE_LABELS[t]}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <Select
            value={statusFilter}
            onValueChange={(v) => setStatusFilter(v as typeof statusFilter)}
          >
            <SelectTrigger className="h-7 w-[130px] border-input bg-background/60 font-hud text-xs">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="all">全部状态</SelectItem>
              {TASK_STATUSES.map((s) => (
                <SelectItem key={s} value={s}>
                  {TASK_STATUS_LABELS[s]}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <span className="ml-auto font-hud text-[10px] text-muted-foreground">
            数据链路：MQTT fleet/tasks · 实时推送
          </span>
        </div>

        {/* 任务表 */}
        <div className="overflow-x-auto">
          <table className="w-full min-w-[960px] text-sm">
            <thead>
              <tr className="border-b border-border/60 text-left">
                {["ID", "任务", "类别", "模式 / 计划时间", "机器人", "目标点位", "优先级", "状态", "操作"].map(
                  (h) => (
                    <th key={h} className="label-caps px-3 py-2 font-normal last:text-right">
                      {h}
                    </th>
                  ),
                )}
              </tr>
            </thead>
            <tbody className="divide-y divide-border/40">
              {tasks.map((t) => (
                <tr key={t.id} className="transition-colors hover:bg-accent/40">
                  <td className="px-3 py-2.5 font-hud text-xs text-muted-foreground">
                    #{t.id}
                  </td>
                  <td className="max-w-[240px] px-3 py-2.5">
                    <div className="truncate text-foreground" title={t.title}>
                      {t.title}
                      {t.source === "voice" && (
                        <span className="ml-1.5 border border-live/40 bg-live/10 px-1 py-0.5 align-middle font-hud text-[9px] text-live">
                          语音
                        </span>
                      )}
                    </div>
                    {t.description && (
                      <div className="mt-0.5 truncate text-[11px] text-muted-foreground" title={t.description}>
                        {t.description}
                      </div>
                    )}
                  </td>
                  <td className="px-3 py-2.5">
                    <TaskTypeChip type={t.type} />
                  </td>
                  <td className="px-3 py-2.5">
                    <div className="flex flex-col gap-1">
                      <TaskModeChip mode={t.mode} />
                      {t.scheduledAt && (
                        <span className="font-hud text-[11px] tabular-nums text-warn">
                          {format(t.scheduledAt, "MM-dd HH:mm")}
                        </span>
                      )}
                    </div>
                  </td>
                  <td className="px-3 py-2.5 font-hud text-xs text-foreground">
                    {t.robotId ?? <span className="text-muted-foreground">未指派</span>}
                  </td>
                  <td className="px-3 py-2.5 text-xs text-foreground">
                    {pointName(t.target) ?? <span className="text-muted-foreground">—</span>}
                  </td>
                  <td className="px-3 py-2.5">
                    <TaskPriorityChip priority={t.priority} />
                  </td>
                  <td className="px-3 py-2.5">
                    <TaskStatusChip status={t.status} />
                  </td>
                  <td className="px-3 py-2.5">
                    <RowActions
                      task={t}
                      onEdit={(task) => {
                        setEditing(task);
                        setDialogOpen(true);
                      }}
                    />
                  </td>
                </tr>
              ))}
              {tasks.length === 0 && (
                <tr>
                  <td colSpan={9} className="px-3 py-10 text-center text-xs text-muted-foreground">
                    {list.isLoading ? "加载中…" : "没有符合条件的任务，点击右上角「新建任务」创建"}
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </div>
      </Panel>

      <TaskFormDialog open={dialogOpen} onOpenChange={setDialogOpen} task={editing} />
    </div>
  );
}
