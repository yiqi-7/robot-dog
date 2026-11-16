/*
 * gz_window_minimize.c —— 把 Gazebo Sim 的 GUI 窗口最小化到任务栏
 *
 * 背景：ros2 launch 启动 gz sim 时，Gazebo 窗口会直接弹到前台挡住桌面。
 *       需求是：窗口只进任务栏，用户点了才显示。
 *
 * 做法：向窗口发送 ICCCM 标准的 WM_CHANGE_STATE 客户端消息（IconicState），
 *       这是窗口管理器认可的"最小化"请求，mutter(GNOME) 与 KDE 都支持。
 *       只依赖 libX11，不需要安装 xdotool/wmctrl，也不需要 root。
 *
 * 用法：gz_window_minimize [窗口标题子串] [等待秒数]
 *       默认标题 "Gazebo Sim"，默认最多等 30 秒（每 0.5 秒找一次）。
 *
 * 说明：Gazebo 是 XWayland 窗口，仍然走 X11 协议，Wayland 会话下同样有效。
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_WINS 32
#define MAX_DEPTH 6

/* 忽略 X11 协议错误：遍历/查询期间窗口可能刚好被销毁（比如仿真重启），
   此时报 BadWindow 属正常现象，不应该让工具崩溃退出 */
static int ignore_x_errors(Display *dpy, XErrorEvent *err)
{
    (void)dpy; (void)err;
    return 0;
}

/* 递归收集所有标题包含 needle 的窗口（外层 mutter frame 与内层客户端都会命中） */
static int collect(Display *dpy, Window root, const char *needle, int depth,
                   Window *out, int max, int cnt)
{
    if (depth > MAX_DEPTH || cnt >= max) return cnt;

    Window parent, *children = NULL;
    unsigned int n = 0;
    if (!XQueryTree(dpy, root, &root, &parent, &children, &n)) return cnt;

    for (unsigned int i = 0; i < n && cnt < max; i++)
    {
        char *name = NULL;
        int from_fetch = 0;
        Atom net_name = XInternAtom(dpy, "_NET_WM_NAME", False);
        Atom utf8     = XInternAtom(dpy, "UTF8_STRING", False);
        Atom type; int fmt; unsigned long nitems, after; unsigned char *data = NULL;

        if (XGetWindowProperty(dpy, children[i], net_name, 0, 256, False, utf8,
                               &type, &fmt, &nitems, &after, &data) == Success && data)
            name = (char *)data;
        else if (XFetchName(dpy, children[i], &name) && name)
            from_fetch = 1;

        if (name && strstr(name, needle))
            out[cnt++] = children[i];

        if (data) XFree(data);
        else if (from_fetch && name) XFree(name);

        cnt = collect(dpy, children[i], needle, depth + 1, out, max, cnt);
    }
    if (children) XFree(children);
    return cnt;
}

/* 窗口是否已经处于 HIDDEN（已最小化）状态 */
static int already_hidden(Display *dpy, Window w)
{
    Atom st     = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom hidden = XInternAtom(dpy, "_NET_WM_STATE_HIDDEN", False);
    Atom type; int fmt; unsigned long nitems, after; unsigned char *data = NULL;
    int ret = 0;

    if (XGetWindowProperty(dpy, w, st, 0, 32, False, XA_ATOM,
                           &type, &fmt, &nitems, &after, &data) == Success && data)
    {
        for (unsigned long i = 0; i < nitems; i++)
            if (((Atom *)data)[i] == hidden) ret = 1;
        XFree(data);
    }
    return ret;
}

/* 打印窗口的 _NET_WM_STATE，用于确认是否真的进入 HIDDEN（最小化）状态 */
static void print_state(Display *dpy, Window w)
{
    Atom st = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom type; int fmt; unsigned long nitems, after; unsigned char *data = NULL;

    printf("  窗口 0x%lx 状态:", w);
    if (XGetWindowProperty(dpy, w, st, 0, 32, False, XA_ATOM,
                           &type, &fmt, &nitems, &after, &data) == Success && data)
    {
        for (unsigned long i = 0; i < nitems; i++)
        {
            char *nm = XGetAtomName(dpy, ((Atom *)data)[i]);
            if (nm) { printf(" %s", nm); XFree(nm); }
        }
        XFree(data);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *needle = (argc > 1) ? argv[1] : "Gazebo Sim";
    int wait_sec = (argc > 2) ? atoi(argv[2]) : 30;

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "无法连接 X 显示\n"); return 1; }
    XSetErrorHandler(ignore_x_errors);

    Window wins[MAX_WINS];
    int cnt = 0;
    for (int i = 0; i < wait_sec * 2 && cnt == 0; i++)
    {
        cnt = collect(dpy, DefaultRootWindow(dpy), needle, 0, wins, MAX_WINS, 0);
        if (cnt == 0) usleep(500 * 1000);
    }
    if (cnt == 0)
    {
        fprintf(stderr, "未找到标题含 \"%s\" 的窗口\n", needle);
        XCloseDisplay(dpy);
        return 2;
    }

    Atom wm_change = XInternAtom(dpy, "WM_CHANGE_STATE", False);
    int sent = 0;
    /* 内外层都发一遍：不同窗口管理器认的层级不一样 */
    for (int i = 0; i < cnt; i++)
    {
        if (already_hidden(dpy, wins[i])) continue;   /* 已经是任务栏状态，不用再动 */
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.xclient.type         = ClientMessage;
        ev.xclient.window       = wins[i];
        ev.xclient.message_type = wm_change;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = IconicState;   /* 3 = 最小化 */
        XSendEvent(dpy, DefaultRootWindow(dpy), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &ev);
        XFlush(dpy);
        sent++;
    }
    usleep(800 * 1000);                          /* 等窗口管理器处理完 */
    printf("匹配 %d 个窗口，对其中 %d 个发送了最小化请求\n", cnt, sent);
    for (int i = 0; i < cnt; i++) print_state(dpy, wins[i]);
    XCloseDisplay(dpy);
    return 0;
}