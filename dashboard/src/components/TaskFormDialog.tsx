import { useEffect, useState } from "react";
import { toast } from "sonner";
import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import { Label } from "@/components/ui/label";
import { Textarea } from "@/components/ui/textarea";
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from "@/components/ui/select";
import { trpc } from "@/providers/trpc";
import { useSim } from "@/sim/store";
import {
  TASK_MODE_LABELS,
  TASK_MODES,
  TASK_PRIORITIES,
  TASK_PRIORITY_LABELS,
  TASK_TYPES,
  TASK_TYPE_LABELS,
  type TaskMode,
  type TaskPriority,
  type TaskType,
} from "@contracts/task";
import type { Task } from "@contracts/task";

function toLocalInput(d: Date | null | undefined) {
  if (!d) return "";
  const p = (n: number) => String(n).padStart(2, "0");
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}`;
}

export function TaskFormDialog({
  open,
  onOpenChange,
  task,
}: {
  open: boolean;
  onOpenChange: (v: boolean) => void;
  /** 传入则为编辑，否则为新建 */
  task: Task | null;
}) {
  const { robots, siteMap } = useSim();
  const utils = trpc.useUtils();

  const [title, setTitle] = useState("");
  const [type, setType] = useState<TaskType>("inspection");
  const [mode, setMode] = useState<TaskMode>("immediate");
  const [robotId, setRobotId] = useState<string>("");
  const [target, setTarget] = useState<string>("");
  const [priority, setPriority] = useState<TaskPriority>("normal");
  const [scheduledLocal, setScheduledLocal] = useState("");
  const [description, setDescription] = useState("");

  useEffect(() => {
    if (!open) return;
    setTitle(task?.title ?? "");
    setType(task?.type ?? "inspection");
    setMode(task?.mode ?? "immediate");
    setRobotId(task?.robotId ?? "");
    setTarget(task?.target ?? "");
    setPriority(task?.priority ?? "normal");
    setScheduledLocal(toLocalInput(task?.scheduledAt));
    setDescription(task?.description ?? "");
  }, [open, task]);

  const invalidate = () => {
    utils.task.list.invalidate();
    utils.task.pending.invalidate();
  };

  const create = trpc.task.create.useMutation({
    onSuccess: () => {
      toast.success("任务已创建（草稿），发布后生效");
      invalidate();
      onOpenChange(false);
    },
    onError: (e) => toast.error(e.message),
  });

  const update = trpc.task.update.useMutation({
    onSuccess: () => {
      toast.success("任务已更新");
      invalidate();
      onOpenChange(false);
    },
    onError: (e) => toast.error(e.message),
  });

  const busy = create.isPending || update.isPending;

  const submit = () => {
    if (!title.trim()) {
      toast.error("请填写任务标题");
      return;
    }
    if (mode === "scheduled" && !scheduledLocal) {
      toast.error("定时任务必须设置执行时间");
      return;
    }
    const payload = {
      title: title.trim(),
      type,
      mode,
      robotId: robotId || null,
      target: target || null,
      priority,
      scheduledAt: mode === "scheduled" ? new Date(scheduledLocal) : null,
      description: description.trim() || null,
    };
    if (task) {
      update.mutate({ id: task.id, ...payload });
    } else {
      create.mutate(payload);
    }
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="border-border bg-[#0c1220] sm:max-w-[560px]">
        <DialogHeader>
          <DialogTitle className="font-hud tracking-[0.15em]">
            {task ? `修改任务 #${task.id}` : "新建任务"}
          </DialogTitle>
        </DialogHeader>

        <div className="grid gap-4">
          <div className="grid gap-1.5">
            <Label className="label-caps">任务标题</Label>
            <Input
              value={title}
              onChange={(e) => setTitle(e.target.value)}
              placeholder="例如：车间A 夜间巡检"
              className="border-input bg-background/60 font-hud"
              maxLength={60}
            />
          </div>

          <div className="grid grid-cols-2 gap-3">
            <div className="grid gap-1.5">
              <Label className="label-caps">任务类别</Label>
              <Select value={type} onValueChange={(v) => setType(v as TaskType)}>
                <SelectTrigger className="border-input bg-background/60 font-hud">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  {TASK_TYPES.map((t) => (
                    <SelectItem key={t} value={t}>
                      {TASK_TYPE_LABELS[t]}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
            <div className="grid gap-1.5">
              <Label className="label-caps">任务模式</Label>
              <div className="flex border border-input">
                {TASK_MODES.map((m) => (
                  <button
                    key={m}
                    type="button"
                    onClick={() => setMode(m)}
                    className={`flex-1 px-3 py-1.5 font-hud text-sm transition-colors ${
                      mode === m
                        ? "bg-[hsl(var(--live)/0.15)] text-live"
                        : "text-muted-foreground hover:text-foreground"
                    }`}
                  >
                    {TASK_MODE_LABELS[m]}
                  </button>
                ))}
              </div>
            </div>
          </div>

          {mode === "scheduled" && (
            <div className="grid gap-1.5">
              <Label className="label-caps">计划执行时间</Label>
              <Input
                type="datetime-local"
                value={scheduledLocal}
                onChange={(e) => setScheduledLocal(e.target.value)}
                className="border-input bg-background/60 font-hud [color-scheme:dark]"
              />
            </div>
          )}

          <div className="grid grid-cols-2 gap-3">
            <div className="grid gap-1.5">
              <Label className="label-caps">执行机器人</Label>
              <Select value={robotId || "none"} onValueChange={(v) => setRobotId(v === "none" ? "" : v)}>
                <SelectTrigger className="border-input bg-background/60 font-hud">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="none">未指派</SelectItem>
                  {robots.map((r) => (
                    <SelectItem key={r.id} value={r.id}>
                      {r.id} · {r.name}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
            <div className="grid gap-1.5">
              <Label className="label-caps">目标点位</Label>
              <Select value={target || "none"} onValueChange={(v) => setTarget(v === "none" ? "" : v)}>
                <SelectTrigger className="border-input bg-background/60 font-hud">
                  <SelectValue />
                </SelectTrigger>
                <SelectContent>
                  <SelectItem value="none">未指定</SelectItem>
                  {siteMap.points.map((p) => (
                    <SelectItem key={p.id} value={p.id}>
                      {p.name}
                    </SelectItem>
                  ))}
                </SelectContent>
              </Select>
            </div>
          </div>

          <div className="grid gap-1.5">
            <Label className="label-caps">优先级</Label>
            <div className="flex border border-input">
              {TASK_PRIORITIES.map((p) => (
                <button
                  key={p}
                  type="button"
                  onClick={() => setPriority(p)}
                  className={`flex-1 px-3 py-1.5 font-hud text-sm transition-colors ${
                    priority === p
                      ? "bg-[hsl(var(--live)/0.15)] text-live"
                      : "text-muted-foreground hover:text-foreground"
                  }`}
                >
                  {TASK_PRIORITY_LABELS[p]}
                </button>
              ))}
            </div>
          </div>

          <div className="grid gap-1.5">
            <Label className="label-caps">任务说明</Label>
            <Textarea
              value={description}
              onChange={(e) => setDescription(e.target.value)}
              placeholder="可选：执行要求、注意事项…"
              className="min-h-[72px] border-input bg-background/60"
              maxLength={300}
            />
          </div>
        </div>

        <DialogFooter>
          <Button variant="outline" onClick={() => onOpenChange(false)} disabled={busy}>
            取消
          </Button>
          <Button
            onClick={submit}
            disabled={busy}
            className="bg-[hsl(var(--live))] text-[hsl(var(--primary-foreground))] hover:bg-[hsl(var(--live)/0.85)]"
          >
            {busy ? "提交中…" : task ? "保存修改" : "创建任务"}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
