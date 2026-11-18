import { useMemo, useRef } from "react";
import { useSim } from "@/sim/store";
import type { Robot } from "@/types";
import { STREAMING } from "@/config";
import { useWhep } from "@/components/useWhep";
import camWorkshop from "@/assets/cam/workshop.png";
import camWarehouse from "@/assets/cam/warehouse.png";
import camCharging from "@/assets/cam/charging.png";
import camCorridor from "@/assets/cam/corridor.png";

export const CAM_IMAGES = [camWorkshop, camWarehouse, camCharging, camCorridor];

export const CAM_NAMES = [
  "车间内窥视角",
  "仓库巷道视角",
  "充电区视角",
  "外围环路视角",
];

function Corner({ className }: { className: string }) {
  return (
    <span
      className={`pointer-events-none absolute h-4 w-4 border-[hsl(var(--live))] opacity-80 ${className}`}
    />
  );
}

/**
 * 摄像头画面：三级显示策略
 *  1. WebRTC 实时流（MediaMTX，机器人 RTSP 推流 → 看板 WHEP 拉流）
 *  2. MQTT 图像帧（旧链路兜底，6s 内有新帧）
 *  3. 静态底图（无推流时占位）
 */
export function CameraCard({ robot, className }: { robot: Robot; className?: string }) {
  const { now, frames } = useSim();
  const videoRef = useRef<HTMLVideoElement | null>(null);
  const whepUrl = useMemo(
    () => `${STREAMING.whepBase}/${STREAMING.camStreamPath(robot.id)}/whep`,
    [robot.id],
  );
  const whepState = useWhep(whepUrl, videoRef, STREAMING.retryMs);

  const ts = now.toLocaleTimeString("zh-CN", { hour12: false });
  const frame = frames[robot.id];
  const frameLive = frame != null && now.getTime() - frame.ts < 6000;

  const mode: "webrtc" | "mqtt" | "static" =
    whepState === "live" ? "webrtc" : frameLive ? "mqtt" : "static";
  const modeLabel =
    mode === "webrtc"
      ? "WEBRTC 实时流"
      : mode === "mqtt"
        ? "图像帧兼容模式（未推流）"
        : whepState === "connecting"
          ? "WEBRTC 建连中…"
          : "等待 RTSP 推流 · STANDBY";

  return (
    <div className={`scanlines relative overflow-hidden border border-border bg-black ${className ?? ""}`}>
      {/* WebRTC 视频层（live 时显示，覆盖底图） */}
      <video
        ref={videoRef}
        muted
        autoPlay
        playsInline
        className={`aspect-video w-full object-cover ${mode === "webrtc" ? "opacity-95" : "hidden"}`}
      />
      {/* 兜底图像层：MQTT 帧或静态底图 */}
      {mode !== "webrtc" && (
        <img
          src={mode === "mqtt" ? frame!.url : CAM_IMAGES[robot.cam]}
          alt={`${robot.name} 摄像头画面`}
          className="aspect-video w-full object-cover opacity-90"
          draggable={false}
        />
      )}
      {/* 四角取景框 */}
      <Corner className="left-2 top-2 border-l-2 border-t-2" />
      <Corner className="right-2 top-2 border-r-2 border-t-2" />
      <Corner className="bottom-2 left-2 border-b-2 border-l-2" />
      <Corner className="bottom-2 right-2 border-b-2 border-r-2" />

      {/* 顶部信息条 */}
      <div className="absolute inset-x-0 top-0 flex items-center justify-between bg-gradient-to-b from-black/70 to-transparent px-3 pb-4 pt-2">
        <div className="flex items-center gap-2">
          <span className="rec-dot inline-block h-2 w-2 rounded-full bg-[#f87171]" />
          <span className="font-hud text-[11px] tracking-[0.15em] text-white/90">
            {robot.id}-CAM0{robot.cam + 1} · {CAM_NAMES[robot.cam]}
          </span>
        </div>
        <span className="font-hud text-[11px] tracking-wider text-white/70">{ts}</span>
      </div>

      {/* 底部信息条 */}
      <div className="absolute inset-x-0 bottom-0 flex items-center justify-between bg-gradient-to-t from-black/70 to-transparent px-3 pb-2 pt-4">
        <span className="font-hud text-[10px] tracking-[0.15em] text-white/60">
          SIG {Math.round(robot.signal)}dBm · 1080P
        </span>
        <span
          className={`font-hud text-[10px] tracking-[0.15em] ${
            mode === "webrtc" ? "text-[#34d399]" : "text-[#fbbf24]"
          }`}
        >
          {modeLabel}
        </span>
      </div>
    </div>
  );
}
