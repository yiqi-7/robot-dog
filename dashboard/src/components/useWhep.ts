import { useEffect, useState } from "react";
import type { RefObject } from "react";

export type WhepState = "connecting" | "live" | "offline";

/**
 * WHEP 拉流 Hook（MediaMTX WebRTC）。
 *
 * - url 指向 `http://<host>:8889/<path>/whep`；无人推流时 MediaMTX 返回 404，按 retryMs 自动重试
 * - 断流（connectionState failed）后同样自动重试
 * - 组件卸载 / url 变化时关闭旧连接
 * - iceServers 传入 STUN 地址可提升公网/NAT 场景的建连成功率；局域网留空即可
 */
export function useWhep(
  url: string | null,
  videoRef: RefObject<HTMLVideoElement | null>,
  retryMs = 5000,
  iceServers: string[] = [],
): WhepState {
  const [state, setState] = useState<WhepState>("offline");

  useEffect(() => {
    if (!url) {
      setState("offline");
      return;
    }
    let stopped = false;
    let pc: RTCPeerConnection | null = null;
    let retryTimer: number | undefined;
    let resourceUrl: string | null = null;

    const teardown = () => {
      if (resourceUrl) {
        // WHEP 资源删除（尽力而为，MediaMTX 也会在连接关闭后自动回收）
        fetch(resourceUrl, { method: "DELETE" }).catch(() => {});
        resourceUrl = null;
      }
      pc?.close();
      pc = null;
      if (videoRef.current) videoRef.current.srcObject = null;
    };

    const scheduleRetry = () => {
      if (stopped) return;
      setState("offline");
      teardown();
      retryTimer = window.setTimeout(connect, retryMs);
    };

    const connect = async () => {
      if (stopped) return;
      setState("connecting");
      try {
        pc = new RTCPeerConnection({
          iceServers: iceServers.map((u) => ({ urls: u })),
        });
        pc.addTransceiver("video", { direction: "recvonly" });
        pc.addTransceiver("audio", { direction: "recvonly" });

        pc.ontrack = (ev) => {
          const v = videoRef.current;
          if (v && ev.streams[0]) {
            v.srcObject = ev.streams[0];
            v.play().catch(() => {});
            setState("live");
          }
        };
        pc.onconnectionstatechange = () => {
          if (!pc) return;
          if (pc.connectionState === "connected") setState("live");
          if (pc.connectionState === "failed" || pc.connectionState === "closed") scheduleRetry();
        };

        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        // 等待 ICE 候选收集完成（非 trickle 一次性 POST；局域网主机候选，通常瞬间完成）
        await new Promise<void>((resolve) => {
          if (!pc || pc.iceGatheringState === "complete") return resolve();
          const onChange = () => {
            if (pc?.iceGatheringState === "complete") {
              pc.removeEventListener("icegatheringstatechange", onChange);
              resolve();
            }
          };
          pc.addEventListener("icegatheringstatechange", onChange);
          window.setTimeout(resolve, 2000); // 兜底
        });
        if (stopped || !pc?.localDescription) return;

        const res = await fetch(url, {
          method: "POST",
          headers: { "Content-Type": "application/sdp" },
          body: pc.localDescription.sdp,
        });
        if (!res.ok) throw new Error(`WHEP http ${res.status}`);
        const loc = res.headers.get("location");
        resourceUrl = loc ? new URL(loc, url).toString() : null;
        const sdp = await res.text();
        await pc.setRemoteDescription({ type: "answer", sdp });
      } catch {
        // 404 = 无人推流；网络错误 = MediaMTX 未启动等 —— 统一重试
        scheduleRetry();
      }
    };

    connect();
    return () => {
      stopped = true;
      window.clearTimeout(retryTimer);
      teardown();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [url]);

  return state;
}
