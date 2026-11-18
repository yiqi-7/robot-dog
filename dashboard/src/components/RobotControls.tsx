import { useEffect, useRef, useState } from "react";
import { toast } from "sonner";
import { useSim } from "@/sim/store";
import type { Robot } from "@/types";
import { cn } from "@/lib/utils";

/**
 * 机器人控制按钮：紧急停止（两次点击确认）/ 故障复位。
 * 通过 {P}/robots/{id}/cmd 下发 estop / resume。
 */
export function RobotControls({ robot, className }: { robot: Robot; className?: string }) {
  const { sendRobotCmd } = useSim();
  const [arming, setArming] = useState(false);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => () => {
    if (timer.current) clearTimeout(timer.current);
  }, []);

  if (robot.id === "--") return null;

  const fireEstop = () => {
    if (!arming) {
      setArming(true);
      timer.current = setTimeout(() => setArming(false), 3000);
      return;
    }
    setArming(false);
    if (timer.current) clearTimeout(timer.current);
    sendRobotCmd(robot.id, "estop");
    toast.warning(`已向 ${robot.id} 下发紧急停止`);
  };

  const fireResume = () => {
    sendRobotCmd(robot.id, "resume");
    toast.success(`已向 ${robot.id} 下发复位指令`);
  };

  return (
    <div className={cn("flex gap-2", className)}>
      {robot.status === "fault" && (
        <button
          onClick={fireResume}
          className="flex-1 border border-live/50 bg-live/10 px-3 py-1.5 font-hud text-xs tracking-wider text-live transition-colors hover:bg-live/20"
        >
          复位恢复 / RESUME
        </button>
      )}
      <button
        onClick={fireEstop}
        className={cn(
          "flex-1 border px-3 py-1.5 font-hud text-xs tracking-wider transition-colors",
          arming
            ? "border-[#f87171] bg-[#f87171]/25 text-[#f87171]"
            : "border-[#f87171]/50 bg-[#f87171]/10 text-[#f87171]/90 hover:bg-[#f87171]/20",
        )}
      >
        {arming ? "再次点击确认急停！" : "紧急停止 / E-STOP"}
      </button>
    </div>
  );
}
