# -*- coding: utf-8 -*-
"""仓库东侧补扫路线：只覆盖 factory_full 地图里未扫开的区域。

从热力图看，未知格集中在：
  1) 仓库南缘 y[-6.5,-5.0]  x[4.5,9.5]   （南墙附近的一整条）
  2) 仓库中部 y[-4.5,-4.0]  x[6.0,9.5]   （货架之间的夹缝）
  3) 东墙边   x[8.5,9.5]     y[-3.5,-1.0]
  4) 仓库北缘 y[-1.0,-1.5]  x[5.5,8.5]

路线设计原则：
  · 沿"已被扫开的空地"走主通道，保证每一步都可达
  · 在每个未扫区的【边缘】停一下让雷达扫进去，而不是钻进未知区
  · 每步都先用 Nav2 规划器验证可达，不可达就跳过（避免卡死）
"""
import io
import math
import re

P = "/run/media/wby/shuju/Document/robot-dog/maps/factory_full.pgm"

d = open(P, "rb").read()
m = re.search(rb"P5\s+(\d+)\s+(\d+)\s+(\d+)", d)
w, h = int(m.group(1)), int(m.group(2))
px = d[m.end() + 1:]
res, ox, oy = 0.05, -10.067, -6.933


def val(x, y):
    c = int((x - ox) / res)
    r = h - 1 - int((y - oy) / res)
    if not (0 <= c < w and 0 <= r < h):
        return None
    return px[r * w + c]


def is_free(x, y):
    v = val(x, y)
    return v is not None and v >= 250


def clearance(x, y, maxr=1.0):
    """到最近不可走格的距离"""
    for i in range(1, int(maxr / res) + 1):
        rad = i * res
        for k in range(16):
            a = 2 * math.pi * k / 16
            v = val(x + rad * math.cos(a), y + rad * math.sin(a))
            if v is None or v < 250:
                return rad
    return maxr


def unknown_ratio(x, y, half=0.5):
    """以 (x,y) 为中心 1m×1m 范围内的未知格比例"""
    unk = tot = 0
    for i in range(int(1.0 / res)):
        for j in range(int(1.0 / res)):
            v = val(x - half + i * res, y - half + j * res)
            if v is None:
                continue
            tot += 1
            if 50 < v < 250:
                unk += 1
    return unk / tot if tot else 1.0


# 候选点：先人工列出想覆盖的位置，再自动吸附到附近"已扫开的空地"
candidates = [
    # 仓库南缘（南墙附近未扫带）
    (4.8, -5.2, "仓库西南角"),
    (6.0, -5.0, "仓库南缘1"),
    (7.2, -5.0, "仓库南缘2"),
    (8.4, -5.0, "仓库东南角"),
    # 仓库中部夹缝
    (6.0, -4.0, "仓库中部夹缝1"),
    (7.2, -4.0, "仓库中部夹缝2"),
    (8.4, -4.0, "仓库中部夹缝3"),
    # 东墙边
    (8.8, -3.0, "东墙中段"),
    (9.0, -2.0, "东墙南段"),
    (9.0, -1.0, "仓库月台内"),
    # 仓库北缘
    (6.0, -1.2, "仓库北缘1"),
    (7.2, -1.2, "仓库北缘2"),
    (8.2, -1.2, "仓库北缘3"),
]


def snap(x, y, max_try=60):
    """从 (x,y) 螺旋搜索最近的"已扫开且净空足够"的空地"""
    if is_free(x, y) and clearance(x, y) >= 0.45:
        return x, y
    for i in range(1, max_try + 1):
        r = i * 0.1
        for k in range(16):
            a = 2 * math.pi * k / 16
            nx, ny = x + r * math.cos(a), y + r * math.sin(a)
            if is_free(nx, ny) and clearance(nx, ny) >= 0.45:
                return round(nx, 2), round(ny, 2)
    return None


print("=== 补扫路线（已吸附到可通行空地）===")
route = []
for x, y, name in candidates:
    s = snap(x, y)
    if s is None:
        print("  %-14s (%4.1f,%4.1f)  ❌ 附近无可用空地，跳过" % (name, x, y))
        continue
    sx, sy = s
    moved = math.hypot(sx - x, sy - y)
    ur = unknown_ratio(sx, sy)
    cl = clearance(sx, sy)
    flag = "↔" if moved > 0.05 else "="
    print("  %-14s (%4.1f,%4.1f) → (%5.2f,%5.2f) %s 净空 %.2fm 邻域未知 %.0f%%" % (
        name, x, y, sx, sy, flag, cl, ur * 100))
    route.append((sx, sy, name))

# 输出 YAML
lines = [
    "# 仓库东侧补扫路线（针对 factory_full 的未知区）",
    "# 由 tools/make_rescan_route.py 生成：候选点自动吸附到最近的已扫开空地，",
    "# 保证每一步在 Nav2 代价地图里都可达，避免走进未知区卡死。",
    "route:",
]
for x, y, name in route:
    lines.append('  - {x: %.2f, y: %.2f, name: "%s"}' % (x, y, name))

out = "/run/media/wby/shuju/Document/robot-dog/tools/rescan_warehouse.yaml"
io.open(out, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
print("\n已写出 %s（%d 个点）" % (out, len(route)))
