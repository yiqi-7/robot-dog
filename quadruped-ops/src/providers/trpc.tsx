/**
 * 任务存储（MQTT 持久化）+ tRPC 兼容垫片。
 *
 * 页面组件保持原有 `trpc.task.*.useQuery / useMutation` 调用方式不变；
 * 底层不再走 tRPC+MySQL，而是：
 *   - 任务列表 = MQTT `{P}/fleet/tasks`（retained 全量快照，JSON）
 *   - 发布/撤销即时任务时向 `{P}/robots/{robotId}/cmd` 下发 assign / cancel
 *   - 定时任务到点由调度方（mock 模式为内置模拟器）下发 assign 并执行
 */
import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { useSim } from "@/sim/store";
import { TOPICS } from "@/mqtt/topics";
import { taskFromMsg, taskToMsg, type TaskMsg } from "@/mqtt/wire";
import {
  type Task,
  type TaskMode,
  type TaskPriority,
  type TaskType,
} from "@contracts/task";

interface TaskInput {
  title: string;
  type: TaskType;
  mode: TaskMode;
  robotId?: string | null;
  target?: string | null;
  priority?: TaskPriority;
  scheduledAt?: Date | null;
  description?: string | null;
}

interface MutationOpts<TOut> {
  onSuccess?: (data: TOut) => void;
  onError?: (err: { message: string }) => void;
}

interface TaskStoreValue {
  tasks: Task[];
  create: (input: TaskInput) => Task;
  update: (input: { id: number } & Partial<TaskInput>) => Task;
  publish: (id: number) => Task;
  revoke: (id: number) => Task;
  remove: (id: number) => { ok: true };
}

const TaskContext = createContext<TaskStoreValue | null>(null);

const PRIORITY_RANK: Record<TaskPriority, number> = { urgent: 0, high: 1, normal: 2, low: 3 };

export function TaskProvider({ children }: { children: ReactNode }) {
  const { bus } = useSim();
  const [tasks, setTasks] = useState<Task[]>([]);
  const tasksRef = useRef<Task[]>([]);
  tasksRef.current = tasks;

  useEffect(() => {
    return bus.subscribe((topic, payload) => {
      if (topic !== TOPICS.fleetTasks()) return;
      setTasks((payload as TaskMsg[]).map(taskFromMsg));
    });
  }, [bus]);

  /** 全量写回（retained），由 Broker 回环驱动本地状态 */
  const writeAll = useCallback(
    (next: Task[]) => {
      bus.publish(TOPICS.fleetTasks(), next.map(taskToMsg), { retain: true });
    },
    [bus],
  );

  const mustGet = (id: number): Task => {
    const t = tasksRef.current.find((x) => x.id === id);
    if (!t) throw new Error("任务不存在");
    return t;
  };

  const create = useCallback((input: TaskInput): Task => {
    if (!input.title?.trim()) throw new Error("标题不能为空");
    if (input.mode === "scheduled" && !input.scheduledAt) throw new Error("定时任务必须设置执行时间");
    const now = new Date();
    const id = tasksRef.current.reduce((m, t) => Math.max(m, t.id), 0) + 1;
    const task: Task = {
      id,
      title: input.title.trim(),
      type: input.type,
      mode: input.mode,
      robotId: input.robotId ?? null,
      target: input.target ?? null,
      priority: input.priority ?? "normal",
      status: "draft",
      scheduledAt: input.mode === "scheduled" ? (input.scheduledAt ?? null) : null,
      description: input.description?.trim() || null,
      createdAt: now,
      updatedAt: now,
    };
    writeAll([task, ...tasksRef.current]);
    return task;
  }, [writeAll]);

  const update = useCallback(({ id, ...fields }: { id: number } & Partial<TaskInput>): Task => {
    const task = mustGet(id);
    if (task.status === "revoked" || task.status === "completed") {
      throw new Error("已撤销 / 已完成的任务不可修改");
    }
    const mode = fields.mode ?? task.mode;
    const scheduledAt = mode === "scheduled" ? (fields.scheduledAt ?? task.scheduledAt) : null;
    if (mode === "scheduled" && !scheduledAt) throw new Error("定时任务必须设置执行时间");
    const next: Task = {
      ...task,
      ...Object.fromEntries(Object.entries(fields).filter(([, v]) => v !== undefined)),
      scheduledAt,
      updatedAt: new Date(),
    } as Task;
    writeAll(tasksRef.current.map((t) => (t.id === id ? next : t)));
    return next;
  }, [writeAll]);

  const publish = useCallback((id: number): Task => {
    const task = mustGet(id);
    if (task.status !== "draft") throw new Error("仅草稿状态的任务可发布");
    if (task.mode === "scheduled" && !task.scheduledAt) throw new Error("定时任务必须设置执行时间后才能发布");
    const next: Task = { ...task, status: "published", updatedAt: new Date() };
    writeAll(tasksRef.current.map((t) => (t.id === id ? next : t)));
    // 即时任务立即向目标机器人下发 assign；定时任务由调度方到点下发
    if (next.mode === "immediate" && next.robotId) {
      bus.publish(TOPICS.robotCmd(next.robotId), {
        cmdId: `CMD-${Date.now().toString(36)}`,
        ts: Date.now(),
        action: "assign",
        task: taskToMsg(next),
      });
    }
    return next;
  }, [bus, writeAll]);

  const revoke = useCallback((id: number): Task => {
    const task = mustGet(id);
    if (task.status === "revoked" || task.status === "completed") throw new Error("该任务已结束，无法撤销");
    const next: Task = { ...task, status: "revoked", updatedAt: new Date() };
    writeAll(tasksRef.current.map((t) => (t.id === id ? next : t)));
    if (task.robotId) {
      bus.publish(TOPICS.robotCmd(task.robotId), {
        cmdId: `CMD-${Date.now().toString(36)}`,
        ts: Date.now(),
        action: "cancel",
        task: taskToMsg(task),
      });
    }
    return next;
  }, [bus, writeAll]);

  const remove = useCallback((id: number): { ok: true } => {
    const task = mustGet(id);
    if (task.status !== "draft") throw new Error("仅草稿可删除，已发布任务请先撤销");
    writeAll(tasksRef.current.filter((t) => t.id !== id));
    return { ok: true };
  }, [writeAll]);

  const value = useMemo<TaskStoreValue>(
    () => ({ tasks, create, update, publish, revoke, remove }),
    [tasks, create, update, publish, revoke, remove],
  );
  return <TaskContext.Provider value={value}>{children}</TaskContext.Provider>;
}

function useTaskStore() {
  const ctx = useContext(TaskContext);
  if (!ctx) throw new Error("TaskProvider 缺失");
  return ctx;
}

/* ---------------- tRPC 兼容垫片 ----------------
 * 复刻页面用到的最小 API 面：
 *   trpc.task.list.useQuery / trpc.task.pending.useQuery
 *   trpc.task.{create,update,publish,revoke,remove}.useMutation
 *   trpc.useUtils().task.{list,pending}.invalidate()
 * ----------------------------------------------- */

function useQueryShim<T>(data: T) {
  return useMemo(() => ({ data, isLoading: false, isError: false }), [data]);
}

function useMutationShim<TIn, TOut>(
  fn: (input: TIn) => TOut,
  opts?: MutationOpts<TOut>,
) {
  const [isPending, setIsPending] = useState(false);
  const optsRef = useRef(opts);
  optsRef.current = opts;
  const mutate = useCallback(
    (input: TIn) => {
      setIsPending(true);
      // 微任务异步，保持与真实 mutation 一致的回调时序
      Promise.resolve()
        .then(() => fn(input))
        .then((res) => optsRef.current?.onSuccess?.(res))
        .catch((e: unknown) =>
          optsRef.current?.onError?.({ message: e instanceof Error ? e.message : String(e) }),
        )
        .finally(() => setIsPending(false));
    },
    [fn],
  );
  return { mutate, isPending };
}

export const trpc = {
  useUtils() {
    return useMemo(
      () => ({ task: { list: { invalidate: () => {} }, pending: { invalidate: () => {} } } }),
      [],
    );
  },
  task: {
    list: {
      useQuery(_input?: undefined, _opts?: { refetchInterval?: number }) {
        const { tasks } = useTaskStore();
        return useQueryShim(tasks);
      },
    },
    pending: {
      useQuery(_input?: undefined, _opts?: { refetchInterval?: number }) {
        const { tasks } = useTaskStore();
        const pending = useMemo(
          () =>
            tasks
              .filter((t) => t.status === "published")
              .sort((a, b) => {
                const r = PRIORITY_RANK[a.priority] - PRIORITY_RANK[b.priority];
                if (r !== 0) return r;
                return (a.scheduledAt?.getTime() ?? 0) - (b.scheduledAt?.getTime() ?? 0);
              }),
          [tasks],
        );
        return useQueryShim(pending);
      },
    },
    create: {
      useMutation(opts?: MutationOpts<Task>) {
        const { create } = useTaskStore();
        return useMutationShim(create, opts);
      },
    },
    update: {
      useMutation(opts?: MutationOpts<Task>) {
        const { update } = useTaskStore();
        return useMutationShim(update, opts);
      },
    },
    publish: {
      useMutation(opts?: MutationOpts<Task>) {
        const { publish } = useTaskStore();
        return useMutationShim(({ id }: { id: number }) => publish(id), opts);
      },
    },
    revoke: {
      useMutation(opts?: MutationOpts<Task>) {
        const { revoke } = useTaskStore();
        return useMutationShim(({ id }: { id: number }) => revoke(id), opts);
      },
    },
    remove: {
      useMutation(opts?: MutationOpts<{ ok: true }>) {
        const { remove } = useTaskStore();
        return useMutationShim(({ id }: { id: number }) => remove(id), opts);
      },
    },
  },
};

/** 兼容旧引用（组件里仅用 trpc 对象） */
export type { Task };
