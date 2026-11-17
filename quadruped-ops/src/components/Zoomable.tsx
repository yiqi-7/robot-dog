import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { cn } from "@/lib/utils";

/**
 * 双击放大容器
 *
 * 交互：双击内容区 -> 放大到接近全屏；再次双击 / 点遮罩 / 点「恢复」/ 按 Esc -> 还原。
 *
 * 两个关键设计：
 *  1. 始终是同一个 DOM 实例（不复制、不 portal 重建），只切换定位方式与尺寸，
 *     因此 WebRTC <video> 的播放状态不会被打断（复制实例会重新挂载导致断流）。
 *  2. 放大时外层持有原尺寸占位（rect 宽高），避免元素脱离文档流后
 *     让同行/同列的其他卡片塌陷重排。
 */
export function Zoomable({
  children,
  className,
  label,
}: {
  /** 渲染函数：参数为当前是否放大，供内部切换尺寸/裁切策略 */
  children: (expanded: boolean) => ReactNode;
  className?: string;
  /** 放大态右上角提示里的名称，如「厂区平面图」 */
  label?: string;
}) {
  const [expanded, setExpanded] = useState(false);
  const [rect, setRect] = useState<{ w: number; h: number } | null>(null);
  const holder = useRef<HTMLDivElement | null>(null);

  const toggle = useCallback(() => {
    setExpanded((prev) => {
      // 放大前记录原尺寸，作为占位用
      if (!prev && holder.current) {
        const r = holder.current.getBoundingClientRect();
        setRect({ w: r.width, h: r.height });
      }
      return !prev;
    });
  }, []);

  // 放大态：Esc 退出 + 锁定背景滚动
  useEffect(() => {
    if (!expanded) return;
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") setExpanded(false);
    };
    window.addEventListener("keydown", onKey);
    const prevOverflow = document.body.style.overflow;
    document.body.style.overflow = "hidden";
    return () => {
      window.removeEventListener("keydown", onKey);
      document.body.style.overflow = prevOverflow;
    };
  }, [expanded]);

  return (
    <>
      {/* 放大态遮罩：点击空白处同样还原 */}
      {expanded && (
        <div
          className="fixed inset-0 z-[110] bg-black/80 backdrop-blur-sm"
          onClick={() => setExpanded(false)}
        />
      )}

      {/* 占位层：放大时保持原始尺寸，防止周边布局塌陷 */}
      <div
        ref={holder}
        className={cn("relative", !expanded && className)}
        style={expanded && rect ? { width: rect.w, height: rect.h } : undefined}
      >
        <div
          onDoubleClick={(e) => {
            e.preventDefault();
            e.stopPropagation();
            toggle();
          }}
          title={expanded ? "双击恢复原始大小" : "双击放大"}
          className={cn(
            expanded
              ? "fixed inset-3 z-[120] flex items-center justify-center overflow-hidden border border-border bg-[#050810] p-2 shadow-2xl"
              : "cursor-zoom-in",
          )}
        >
          {children(expanded)}

          {/* 放大态提示条 + 退出按钮 */}
          {expanded && (
            <div className="absolute right-2 top-2 z-[130] flex items-center gap-2">
              <span className="pointer-events-none border border-border bg-[#0a0f1c]/95 px-2 py-1 font-hud text-[10px] tracking-[0.15em] text-muted-foreground">
                {label ? `${label} · ` : ""}双击恢复原大小 / ESC 退出
              </span>
              <button
                type="button"
                onClick={(e) => {
                  e.stopPropagation();
                  setExpanded(false);
                }}
                className="border border-border bg-[#0a0f1c]/95 px-2 py-1 font-hud text-[10px] tracking-[0.15em] text-foreground transition-colors hover:text-live"
              >
                恢复 ✕
              </button>
            </div>
          )}
        </div>
      </div>
    </>
  );
}
