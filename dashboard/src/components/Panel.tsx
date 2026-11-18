import type { ReactNode } from "react";
import { cn } from "@/lib/utils";

export function Panel({
  title,
  right,
  className,
  bodyClassName,
  scan = false,
  children,
}: {
  title?: ReactNode;
  right?: ReactNode;
  className?: string;
  bodyClassName?: string;
  scan?: boolean;
  children: ReactNode;
}) {
  return (
    <section className={cn("panel", scan && "scanlines", className)}>
      {title != null && (
        <header className="panel-hd">
          <span className="panel-title">{title}</span>
          {right != null && (
            <div className="flex min-w-0 items-center gap-2">{right}</div>
          )}
        </header>
      )}
      <div className={cn("p-3", bodyClassName)}>{children}</div>
    </section>
  );
}
