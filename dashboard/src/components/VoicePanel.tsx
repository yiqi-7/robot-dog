import { useEffect, useRef, useState } from "react";
import { format } from "date-fns";
import { Play, Square } from "lucide-react";
import { toast } from "sonner";
import { Panel } from "@/components/Panel";
import { TaskTypeChip } from "@/components/hud";
import { Button } from "@/components/ui/button";
import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from "@/components/ui/dialog";
import { Label } from "@/components/ui/label";
import { Switch } from "@/components/ui/switch";
import { Slider } from "@/components/ui/slider";
import { useSim } from "@/sim/store";
import { TASK_TYPE_LABELS } from "@contracts/task";
import type { VoiceCommandMsg, VoicePolicyMsg } from "@/mqtt/wire";
import { cn } from "@/lib/utils";

const STATUS_META: Record<VoiceCommandMsg["status"], { label: string; color: string }> = {
  pending_confirm: { label: "待确认", color: "#fbbf24" },
  accepted: { label: "已执行", color: "#34d399" },
  rejected: { label: "已拒绝", color: "#64748b" },
  failed: { label: "失败", color: "#f87171" },
};

function StatusChip({ status }: { status: VoiceCommandMsg["status"] }) {
  const m = STATUS_META[status];
  return (
    <span
      className="border px-1.5 py-0.5 font-hud text-[10px]"
      style={{ color: m.color, borderColor: `${m.color}55`, backgroundColor: `${m.color}14` }}
    >
      {m.label}
    </span>
  );
}

/** 语音确认策略设置 */
function VoicePolicyDialog({ open, onOpenChange }: { open: boolean; onOpenChange: (v: boolean) => void }) {
  const { voicePolicy, setVoicePolicy } = useSim();
  const [local, setLocal] = useState<VoicePolicyMsg>(voicePolicy);

  useEffect(() => {
    if (open) setLocal(voicePolicy);
  }, [open, voicePolicy]);

  const setConfirm = (k: keyof VoicePolicyMsg["requireConfirm"], v: boolean) =>
    setLocal((p) => ({ ...p, requireConfirm: { ...p.requireConfirm, [k]: v } }));

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-md border-border bg-card">
        <DialogHeader>
          <DialogTitle className="font-hud tracking-wide">语音指令确认策略</DialogTitle>
        </DialogHeader>

        <div className="flex flex-col gap-4 py-1">
          <div>
            <div className="label-caps mb-2">按任务类别</div>
            <div className="flex flex-col gap-2.5">
              {(Object.keys(TASK_TYPE_LABELS) as (keyof typeof TASK_TYPE_LABELS)[]).map((k) => (
                <div key={k} className="flex items-center justify-between">
                  <Label className="text-sm text-foreground">{TASK_TYPE_LABELS[k]}任务需人工确认</Label>
                  <Switch checked={local.requireConfirm[k]} onCheckedChange={(v) => setConfirm(k, v)} />
                </div>
              ))}
            </div>
          </div>

          <div>
            <div className="mb-2 flex items-center justify-between">
              <Label className="text-sm text-foreground">置信度低于阈值时强制确认</Label>
              <span className="font-hud text-sm tabular-nums text-live">
                {(local.confidenceThreshold * 100).toFixed(0)}%
              </span>
            </div>
            <Slider
              value={[local.confidenceThreshold]}
              min={0.5}
              max={0.95}
              step={0.05}
              onValueChange={([v]) => setLocal((p) => ({ ...p, confidenceThreshold: v }))}
            />
          </div>

          <div className="flex items-center justify-between">
            <Label className="text-sm text-foreground">急停口令免确认直接执行</Label>
            <Switch
              checked={local.estopDirect}
              onCheckedChange={(v) => setLocal((p) => ({ ...p, estopDirect: v }))}
            />
          </div>
          <div className="flex items-center justify-between">
            <Label className="text-sm text-foreground">机器人 TTS 语音播报反馈</Label>
            <Switch
              checked={local.ttsReply}
              onCheckedChange={(v) => setLocal((p) => ({ ...p, ttsReply: v }))}
            />
          </div>
        </div>

        <DialogFooter>
          <Button variant="outline" onClick={() => onOpenChange(false)}>取消</Button>
          <Button
            onClick={() => {
              setVoicePolicy(local);
              toast.success("语音确认策略已保存（全端生效）");
              onOpenChange(false);
            }}
          >
            保存策略
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}

function VoiceRow({ cmd }: { cmd: VoiceCommandMsg }) {
  const { confirmVoice, siteMap, robots } = useSim();
  const [playing, setPlaying] = useState(false);
  const audioRef = useRef<HTMLAudioElement | null>(null);

  useEffect(() => () => audioRef.current?.pause(), []);

  const togglePlay = () => {
    if (!cmd.audioFile) return;
    if (playing) {
      audioRef.current?.pause();
      audioRef.current = null;
      setPlaying(false);
      return;
    }
    const a = new Audio(cmd.audioFile);
    a.onended = () => { setPlaying(false); audioRef.current = null; };
    a.onerror = () => { setPlaying(false); audioRef.current = null; toast.error("音频加载失败"); };
    audioRef.current = a;
    setPlaying(true);
    a.play().catch(() => { setPlaying(false); audioRef.current = null; toast.error("音频播放失败"); });
  };

  const pointName = cmd.parsed?.target
    ? (siteMap.points.find((p) => p.id === cmd.parsed!.target)?.name ?? cmd.parsed.target)
    : null;
  const robotName = cmd.robotId
    ? (robots.find((r) => r.id === cmd.robotId)?.name ?? cmd.robotId)
    : null;

  return (
    <li className="flex items-center gap-3 px-3 py-2">
      {cmd.audioFile && (
        <button
          onClick={togglePlay}
          title={playing ? "停止回放" : "回放语音片段"}
          className={cn(
            "flex h-6 w-6 shrink-0 items-center justify-center border transition-colors",
            playing
              ? "border-live/60 text-live"
              : "border-border text-muted-foreground hover:border-live/50 hover:text-live",
          )}
        >
          {playing ? <Square className="h-3 w-3" /> : <Play className="h-3 w-3" />}
        </button>
      )}
      <div className="min-w-0 flex-1">
        <div className="flex items-center gap-2">
          <span className="truncate text-sm text-foreground">「{cmd.transcript}」</span>
          <span className="shrink-0 font-hud text-[10px] text-muted-foreground">
            {cmd.engine === "llm" ? "LLM" : "规则"} · {(cmd.confidence * 100).toFixed(0)}%
          </span>
        </div>
        <div className="mt-0.5 flex flex-wrap items-center gap-x-2 gap-y-0.5 text-[11px] text-muted-foreground">
          <span className="font-hud">{format(new Date(cmd.ts), "HH:mm:ss")}</span>
          <span>拾音 {cmd.heardBy}</span>
          {cmd.action === "estop" ? (
            <span className="text-[#f87171]">急停</span>
          ) : cmd.parsed ? (
            <>
              <span>→</span>
              <span>{TASK_TYPE_LABELS[cmd.parsed.type]}{pointName ? ` @ ${pointName}` : ""}</span>
              {robotName && <span className="font-hud">{cmd.robotId} · {robotName}</span>}
              {cmd.parsed.mode === "scheduled" && cmd.parsed.scheduledAt && (
                <span className="font-hud text-warn">{format(new Date(cmd.parsed.scheduledAt), "MM-dd HH:mm")}</span>
              )}
            </>
          ) : null}
          {cmd.status === "failed" && cmd.reason && <span className="text-[#f87171]">{cmd.reason}</span>}
          {cmd.taskId != null && <span className="font-hud text-live">任务 #{cmd.taskId}</span>}
        </div>
      </div>
      {cmd.parsed && cmd.action === "task" && <TaskTypeChip type={cmd.parsed.type} />}
      <StatusChip status={cmd.status} />
      {cmd.status === "pending_confirm" && (
        <div className="flex shrink-0 gap-1.5">
          <Button size="sm" className="h-6 px-2 text-xs" onClick={() => confirmVoice(cmd.cmdId, true)}>
            确认
          </Button>
          <Button
            size="sm"
            variant="outline"
            className="h-6 px-2 text-xs"
            onClick={() => confirmVoice(cmd.cmdId, false)}
          >
            拒绝
          </Button>
        </div>
      )}
    </li>
  );
}

export function VoicePanel({ className }: { className?: string }) {
  const { voiceCommands, busStatus } = useSim();
  const [policyOpen, setPolicyOpen] = useState(false);
  const pendingCount = voiceCommands.filter((c) => c.status === "pending_confirm").length;

  return (
    <Panel
      title="语音指令 / VOICE COMMANDS"
      className={className}
      right={
        <div className="flex items-center gap-2">
          {pendingCount > 0 && (
            <span className="font-hud text-[10px] text-warn">{pendingCount} 条待确认</span>
          )}
          <button
            onClick={() => setPolicyOpen(true)}
            className={cn(
              "border border-border px-1.5 py-0.5 font-hud text-[10px] text-muted-foreground",
              "transition-colors hover:border-live/50 hover:text-live",
            )}
          >
            策略设置
          </button>
        </div>
      }
      bodyClassName="p-0"
    >
      {voiceCommands.length === 0 ? (
        <div className="border-dashed m-3 border border-border px-3 py-6 text-center text-xs text-muted-foreground">
          {busStatus === "connected"
            ? "暂无语音指令 — 对机器人说话即可发起（充电任务默认直通）"
            : "等待 Broker 连接…"}
        </div>
      ) : (
        <ul className="divide-y divide-border/60">
          {voiceCommands.slice(0, 8).map((c) => (
            <VoiceRow key={c.cmdId} cmd={c} />
          ))}
        </ul>
      )}
      <VoicePolicyDialog open={policyOpen} onOpenChange={setPolicyOpen} />
    </Panel>
  );
}
