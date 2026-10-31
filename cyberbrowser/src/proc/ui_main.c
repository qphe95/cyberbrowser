/* ui_main.c - cb_ui.exe: the UI process (SANDBOX_DESIGN.md sections 2.2, 7.4).
 *
 * Started by the coordinator with its channel (and start URLs, hex):
 *   cb_ui.exe --chan S E0 E1 [HEX ...]
 * It gets its channel to the GPU process from the coordinator (CU_GPU), and
 * again if that restarts.
 *
 * Owns the windows: each has a tab strip, an address bar and a content area,
 * and the tab table says which tabs each window has and which is active
 * (each tab's URL and title too).  A window's content area is a child window
 * the GPU process presents its active tab's frames into (UG_WINDOW); until it
 * has (GU_SHOWN), it stays hidden and the UI shows "Loading..." or what went
 * wrong.  The UI sends the active tab the user's input.  Following a link or
 * typing a URL replaces the tab's process with a new one.  What a tab sends
 * is read by ui_core.c (proved); a tab opens a new tab only for a click the
 * user gave it.
 *
 * Dragging a tab moves it: along its strip, onto another window's strip, or
 * out of every strip into a window of its own.  The tab's process doesn't
 * notice beyond a new size: the GPU process presents the content it already
 * has in the other window.  Scrolling goes to the tab and to the GPU process
 * at once, which moves the content it has before the tab draws again.
 *
 * Keys: Ctrl+T new tab, Ctrl+N new window, Ctrl+W close tab, Ctrl+L address
 * bar, Ctrl+Tab next tab, Page Up/Down, arrows and Space scroll.
 *
 * CYBER_UI_SCRIPT=<file>: run commands from a file, one per line, for tests:
 *   open URL | go URL | waitload [MS [QUIET]] (logs the load's timing) | wait MS | click X Y | scroll DY |
 *   select I | close | resize W H | shot FILE.bmp | log TEXT | quit |
 *   detach (the active tab into a new window) | attach W (into window W) |
 *   dragout | dragto W (the same, dragged with the mouse handlers) |
 *   window W (later commands act on window W) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <windowsx.h>

#include "chan.h"
#include "bigmsg.h"
#include "proto.h"
#include "spawn.h"
#include "ui_core.h"

#define MAX_TABS 60
#define MAX_WINS 8
#define STRIP_H 34
#define ADDR_H 34
#define CONTENT_Y (STRIP_H + ADDR_H)
#define TAB_MAX_W 220
#define NEW_TAB_W 34
#define DRAG_START 8            /* px the mouse moves before a press on a tab becomes a drag */

enum { ST_STARTING, ST_LOADING, ST_LOADED, ST_FAILED, ST_CRASHED };

typedef struct Tab {
    bool used;
    int win;                    /* the window it is in */
    uint32_t id;                /* of its current process */
    char url[PROTO_URL_MAX + 1];
    char title[PROTO_TITLE_MAX + 1];
    int state;
    bool started;               /* channel attached */
    Chan chan;                  /* side 0: the UI */
    bool has_frame;             /* it sent one to the GPU process */
    bool shown;                 /* the GPU process presented one in its window */
    uint32_t scroll_seq;
    TuFrame frame;
    DWORD last_frame_tick;
    double start_ms, frame_ms, loaded_ms, last_frame_ms;  /* load timing for scripts: process started, first and last frames, loaded */
    unsigned frames;
    bool vis_told, visible;     /* what its process was last told (UT_VISIBLE) */
    UiGesture gesture;          /* its process's last click, for opening a tab */
} Tab;

typedef struct Win {
    bool used;
    HWND wnd, addr, content;
    int cw, ch;                 /* client size */
    int order[MAX_TABS];        /* its tabs (indices into g_tabs), left to right */
    int n, active;              /* active: index into order, -1 none */
    UgWindow told;              /* what the GPU process was last told */
} Win;

static Chan g_coord;            /* side 1: the UI */
static Tab g_tabs[MAX_TABS];
static Win g_wins[MAX_WINS];
static int g_focus;             /* the window script commands act on */
static uint32_t g_next_id = 1;
static Chan g_gpu;              /* side 0: the UI */
static bool g_have_gpu, g_gpu_before;
static BigRx g_gpu_rx;
static WNDPROC g_addr_proc;
static HFONT g_font;
static bool g_quitting;

static void ui_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static int content_w(const Win *w) { int v = w->cw; return v < 1 ? 1 : v > SURFACE_MAX_W ? SURFACE_MAX_W : v; }
static int content_h(const Win *w) {
    int v = w->ch - CONTENT_Y;
    return v < 1 ? 1 : v > SURFACE_MAX_H ? SURFACE_MAX_H : v;
}

static wchar_t *wide(const char *s, wchar_t *buf, int cap) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, cap)) buf[0] = 0;
    buf[cap - 1] = 0;
    return buf;
}

static int win_of_hwnd(HWND h) {
    for (int i = 0; i < MAX_WINS; i++)
        if (g_wins[i].used && (g_wins[i].wnd == h || g_wins[i].addr == h || g_wins[i].content == h)) return i;
    return -1;
}

static Tab *active_tab(const Win *w) {
    return w->active >= 0 && w->active < w->n ? &g_tabs[w->order[w->active]] : NULL;
}

/* Each tab's process told whether it is shown (its window's active tab, the
 * window not minimized): a hidden one does not render. */
static void tell_visibility(void) {
    for (int i = 0; i < MAX_TABS; i++) {
        Tab *t = &g_tabs[i];
        if (!t->used || !t->started || !g_wins[t->win].used) continue;
        bool v = active_tab(&g_wins[t->win]) == t && !IsIconic(g_wins[t->win].wnd);
        if (t->vis_told && t->visible == v) continue;
        UtVisible m = { v ? 1u : 0u };
        if (chan_send(&t->chan, UT_VISIBLE, &m, sizeof(m), 100)) { t->vis_told = true; t->visible = v; }
    }
}

/* ---- tabs ---------------------------------------------------------------- */

static void release(Tab *t) {
    if (t->started) chan_close(&t->chan);
    t->started = false;
    t->has_frame = false;
    t->shown = false;
}

/* Tell the GPU process what window w's content window shows (its active tab
 * once that has a frame), and show the content window once that's presented. */
static void show_content(Win *w) {
    if (!w->used) return;
    Tab *t = active_tab(w);
    UgWindow u = { (uint64_t)(uintptr_t)w->content, t && t->started && t->has_frame ? t->id : 0,
                   (uint32_t)content_w(w), (uint32_t)content_h(w) };
    if (g_have_gpu && memcmp(&u, &w->told, sizeof(u)) != 0 &&
        big_send(&g_gpu, UG_WINDOW, &u, sizeof(u), NULL, 0, NULL, 0, 1000))
        w->told = u;
    bool visible = u.tab && t->shown;
    if (visible != (IsWindowVisible(w->content) != 0)) ShowWindow(w->content, visible ? SW_SHOWNA : SW_HIDE);
}

/* Milliseconds on the performance counter (load timing). */
static double ui_now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void start_process(Tab *t) {
    static uint8_t msg[sizeof(UcOpenTab) + PROTO_URL_MAX];
    Win *w = &g_wins[t->win];
    t->start_ms = ui_now_ms();
    t->frame_ms = t->loaded_ms = t->last_frame_ms = 0;
    t->frames = 0;
    t->vis_told = false;
    t->id = g_next_id++;
    t->state = ST_STARTING;
    t->has_frame = false;
    t->shown = false;
    t->title[0] = 0;
    t->scroll_seq = 0;
    memset(&t->gesture, 0, sizeof(t->gesture));
    UcOpenTab o = { t->id, (uint32_t)content_w(w), (uint32_t)content_h(w) };
    size_t n = strlen(t->url);
    memcpy(msg, &o, sizeof(o));
    memcpy(msg + sizeof(o), t->url, n);
    chan_send(&g_coord, UC_OPEN_TAB, msg, (uint32_t)(sizeof(o) + n), 1000);
}

static void end_process(Tab *t) {
    if (t->id) {
        UcTab c = { t->id };
        chan_send(&g_coord, UC_CLOSE_TAB, &c, sizeof(c), 1000);
    }
    release(t);
    t->id = 0;
}

static void update_chrome(Win *w) {
    if (!w->used) return;
    wchar_t buf[PROTO_URL_MAX + 64];
    Tab *t = active_tab(w);
    if (t) {
        if (GetFocus() != w->addr) SetWindowTextW(w->addr, wide(t->url, buf, PROTO_URL_MAX + 1));
        wchar_t title[PROTO_TITLE_MAX + 1];
        wide(t->title[0] ? t->title : t->url, title, PROTO_TITLE_MAX + 1);
        _snwprintf(buf, PROTO_URL_MAX + 64, L"%ls - CyberBrowser", title);
        buf[PROTO_URL_MAX + 63] = 0;
        SetWindowTextW(w->wnd, buf);
    } else {
        SetWindowTextW(w->addr, L"");
        SetWindowTextW(w->wnd, L"CyberBrowser");
    }
    show_content(w);
    InvalidateRect(w->wnd, NULL, FALSE);
}

static void update_tab(Tab *t) { update_chrome(&g_wins[t->win]); }

static void navigate(Tab *t, const char *url) {
    end_process(t);
    snprintf(t->url, sizeof(t->url), "%s", url);
    start_process(t);
    update_tab(t);
}

static int new_tab(void) {
    for (int i = 0; i < MAX_TABS; i++)
        if (!g_tabs[i].used) {
            memset(&g_tabs[i], 0, sizeof(Tab));
            g_tabs[i].used = true;
            return i;
        }
    return -1;
}

/* Put tab ti into window wi at position pos (clamped), active. */
static void insert_tab(int wi, int ti, int pos) {
    Win *w = &g_wins[wi];
    if (pos < 0 || pos > w->n) pos = w->n;
    memmove(&w->order[pos + 1], &w->order[pos], (size_t)(w->n - pos) * sizeof(int));
    w->order[pos] = ti;
    w->n++;
    w->active = pos;
    g_tabs[ti].win = wi;
}

/* Take tab ti out of its window's strip (its process goes on). */
static void remove_tab(int ti) {
    Win *w = &g_wins[g_tabs[ti].win];
    int pos = -1;
    for (int i = 0; i < w->n; i++)
        if (w->order[i] == ti) pos = i;
    if (pos < 0) return;
    memmove(&w->order[pos], &w->order[pos + 1], (size_t)(w->n - pos - 1) * sizeof(int));
    w->n--;
    if (w->active > pos || w->active >= w->n) w->active--;
    if (w->active < 0 && w->n) w->active = 0;
}

static void open_tab_in(int wi, const char *url) {
    int ti = new_tab();
    if (ti < 0 || !g_wins[wi].used) return;
    snprintf(g_tabs[ti].url, sizeof(g_tabs[ti].url), "%s", url);
    insert_tab(wi, ti, g_wins[wi].n);
    start_process(&g_tabs[ti]);
    update_chrome(&g_wins[wi]);
}

static void close_window(int wi);

static void close_tab(int wi, int pos) {
    Win *w = &g_wins[wi];
    if (pos < 0 || pos >= w->n) return;
    int ti = w->order[pos];
    end_process(&g_tabs[ti]);
    remove_tab(ti);
    g_tabs[ti].used = false;
    if (w->n == 0) {
        close_window(wi);
        return;
    }
    update_chrome(w);
}

static Tab *tab_by_id(uint32_t id) {
    for (int i = 0; i < MAX_TABS; i++)
        if (g_tabs[i].used && g_tabs[i].id == id) return &g_tabs[i];
    return NULL;
}

/* Tell tab t its content size (that of its window). */
static void resize_tab(Tab *t) {
    if (!t->started) return;
    Win *w = &g_wins[t->win];
    UtSize s = { (uint32_t)content_w(w), (uint32_t)content_h(w) };
    chan_send(&t->chan, UT_RESIZE, &s, sizeof(s), 100);
}

/* What the user typed: a URL, or a host to put https:// in front of. */
static void typed_url(const char *in, char *out, size_t cap) {
    while (*in == ' ') in++;
    if (!strncmp(in, "http://", 7) || !strncmp(in, "https://", 8) || !strcmp(in, "about:blank"))
        snprintf(out, cap, "%s", in);
    else
        snprintf(out, cap, "https://%s", in);
    size_t n = strlen(out);
    while (n && out[n - 1] == ' ') out[--n] = 0;
}

/* ---- windows -------------------------------------------------------------- */

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp);
static LRESULT CALLBACK addr_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp);

/* A window's content window (content_proc): hidden until a frame is presented. */
static HWND make_content(Win *w) {
    return CreateWindowW(L"CyberContent", NULL, WS_CHILD | WS_CLIPSIBLINGS, 0, CONTENT_Y, content_w(w),
                         content_h(w), w->wnd, NULL, GetModuleHandleW(NULL), NULL);
}

/* A new window, client size cw x ch, at (x, y) on the screen (CW_USEDEFAULT:
 * where Windows likes).  -1 if there are too many. */
static int new_window(int x, int y, int cw, int ch) {
    int wi = -1;
    for (int i = 0; i < MAX_WINS && wi < 0; i++)
        if (!g_wins[i].used) wi = i;
    if (wi < 0) return -1;
    Win *w = &g_wins[wi];
    memset(w, 0, sizeof(*w));
    w->used = true;
    w->active = -1;
    w->cw = cw;
    w->ch = ch;
    RECT r = { 0, 0, cw, ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HINSTANCE hi = GetModuleHandleW(NULL);
    w->wnd = CreateWindowW(L"CyberBrowser", L"CyberBrowser", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    if (!w->wnd) {
        w->used = false;
        return -1;
    }
    w->content = make_content(w);
    w->addr = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 10, STRIP_H + 5,
                            cw - 20, ADDR_H - 10, w->wnd, NULL, hi, NULL);
    SendMessageW(w->addr, WM_SETFONT, (WPARAM)g_font, TRUE);
    g_addr_proc = (WNDPROC)SetWindowLongPtrW(w->addr, GWLP_WNDPROC, (LONG_PTR)addr_proc);
    /* Channels keep being read while Windows runs a move or resize loop. */
    SetTimer(w->wnd, 1, 15, NULL);
    ShowWindow(w->wnd, SW_SHOW);
    return wi;
}

/* Window wi goes, with whatever tabs are left in it.  The last one ends the
 * browser. */
static void close_window(int wi) {
    Win *w = &g_wins[wi];
    if (!w->used) return;
    while (w->n) {
        int ti = w->order[w->n - 1];
        end_process(&g_tabs[ti]);
        g_tabs[ti].used = false;
        w->n--;
    }
    UgWindow gone = { (uint64_t)(uintptr_t)w->content, 0, 0, 0 };
    if (g_have_gpu) big_send(&g_gpu, UG_WINDOW, &gone, sizeof(gone), NULL, 0, NULL, 0, 1000);
    w->used = false;
    HWND h = w->wnd;
    w->wnd = NULL;
    DestroyWindow(h);
    for (int i = 0; i < MAX_WINS; i++)
        if (g_wins[i].used) {
            if (g_focus == wi) g_focus = i;
            return;
        }
    if (!g_quitting) {
        g_quitting = true;
        chan_send(&g_coord, UC_QUIT, NULL, 0, 1000);
        PostQuitMessage(0);
    }
}

/* Move tab ti into window wi at pos: its process carries on, the GPU
 * process presents what it has there, and the tab hears its new size. */
static void move_tab(int ti, int wi, int pos) {
    Tab *t = &g_tabs[ti];
    int from = t->win;
    remove_tab(ti);
    insert_tab(wi, ti, pos);
    t->shown = false;
    resize_tab(t);
    ui_log("[ui] tab %u moved to window %d", t->id, wi);
    if (g_wins[from].n == 0 && from != wi) close_window(from);
    else update_chrome(&g_wins[from]);
    update_chrome(&g_wins[wi]);
}

/* Tab ti into a window of its own at screen point (x, y). */
static void detach_tab(int ti, int x, int y) {
    Win *src = &g_wins[g_tabs[ti].win];
    /* Its title bar on the screen the drop was on. */
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(MonitorFromPoint((POINT){ x, y }, MONITOR_DEFAULTTONEAREST), &mi)) {
        if (x > mi.rcWork.right - 200) x = mi.rcWork.right - 200;
        if (y > mi.rcWork.bottom - 100) y = mi.rcWork.bottom - 100;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
    }
    int wi = new_window(x, y, src->cw, src->ch);
    if (wi < 0) return;
    move_tab(ti, wi, 0);
    g_focus = wi;
}

/* ---- messages from the coordinator and the tabs ----------------------------- */

static void from_coord(const ChanMsg *m) {
    switch (m->type) {
    case CU_TAB_STARTED: {
        if (m->len != sizeof(CuTabStarted)) return;
        CuTabStarted s;
        memcpy(&s, m->data, sizeof(s));
        HANDLE hs = (HANDLE)(uintptr_t)s.chan_section, h0 = (HANDLE)(uintptr_t)s.chan_ev0,
               h1 = (HANDLE)(uintptr_t)s.chan_ev1;
        Tab *t = tab_by_id(s.tab);
        if (!t) {
            /* A tab closed (or navigated) since it was asked for: drop its
             * handles and end its process. */
            CloseHandle(hs); CloseHandle(h0); CloseHandle(h1);
            UcTab c = { s.tab };
            chan_send(&g_coord, UC_CLOSE_TAB, &c, sizeof(c), 1000);
            return;
        }
        if (!chan_attach(&t->chan, hs, h0, h1, 0)) { t->state = ST_FAILED; return; }
        t->started = true;
        ui_log("[ui] tab %u started", t->id);
        resize_tab(t);          /* in case its window changed size since it was asked for */
        return;
    }
    case CU_TAB_EXITED: {
        if (m->len != sizeof(CuTabExited)) return;
        CuTabExited x;
        memcpy(&x, m->data, sizeof(x));
        Tab *t = tab_by_id(x.tab);
        if (!t) return;
        ui_log("[ui] tab %u exited with %u", x.tab, x.exit_code);
        release(t);
        t->id = 0;
        t->state = ST_CRASHED;
        snprintf(t->title, sizeof(t->title), "Crashed (%u)", x.exit_code);
        update_tab(t);
        return;
    }
    case CU_GPU: {
        if (m->len != sizeof(CuGpu)) return;
        CuGpu g;
        memcpy(&g, m->data, sizeof(g));
        if (g_have_gpu) chan_close(&g_gpu);
        big_free(&g_gpu_rx);
        memset(&g_gpu_rx, 0, sizeof(g_gpu_rx));
        g_have_gpu = chan_attach(&g_gpu, (HANDLE)(uintptr_t)g.chan_section, (HANDLE)(uintptr_t)g.chan_ev0,
                                 (HANDLE)(uintptr_t)g.chan_ev1, 0);
        for (int i = 0; i < MAX_TABS; i++) g_tabs[i].shown = false;
        for (int i = 0; i < MAX_WINS; i++) {
            Win *w = &g_wins[i];
            if (!w->used) continue;
            memset(&w->told, 0, sizeof(w->told));
            /* A new GPU process (not the first): new content windows too.
             * The old ones stay tied to the old process's swap chains, and
             * another can't be made on them (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR). */
            if (g_gpu_before) {
                DestroyWindow(w->content);
                w->content = make_content(w);
            }
        }
        g_gpu_before = true;
        ui_log("[ui] %s the GPU process", g_have_gpu ? "connected to" : "could not connect to");
        for (int i = 0; i < MAX_WINS; i++) show_content(&g_wins[i]);
        return;
    }
    case CU_TAB_FAILED: {
        if (m->len != sizeof(UcTab)) return;
        UcTab x;
        memcpy(&x, m->data, sizeof(x));
        Tab *t = tab_by_id(x.tab);
        if (!t) return;
        t->id = 0;
        t->state = ST_FAILED;
        snprintf(t->title, sizeof(t->title), "Could not open");
        update_tab(t);
        return;
    }
    }
}

/* A tab's message, as ui_tab_message (proved) reads it. */
static void from_tab(Tab *t, const ChanMsg *m) {
    static UiTabMsg u;
    switch (ui_tab_message(m->type, m->data, m->len, &u)) {
    case TU_FRAME:
        t->frame = u.frame;
        t->last_frame_tick = GetTickCount();
        t->last_frame_ms = ui_now_ms();
        t->frames++;
        if (!t->frame_ms) t->frame_ms = t->last_frame_ms;
        if (!t->has_frame) {
            t->has_frame = true;
            show_content(&g_wins[t->win]);
        }
        return;
    case TU_TITLE:
        memcpy(t->title, u.text, u.text_len + 1);
        update_tab(t);
        return;
    case TU_STATE:
        t->state = u.state == TAB_LOADED ? ST_LOADED : u.state == TAB_LOAD_FAILED ? ST_FAILED : ST_LOADING;
        if (t->state == ST_LOADED && !t->loaded_ms) t->loaded_ms = ui_now_ms();
        InvalidateRect(g_wins[t->win].wnd, NULL, FALSE);
        return;
    case TU_WANT_WEBGPU: {
        /* For this tab only: the id is the channel's, not the tab's word. */
        UcTab w = { t->id };
        chan_send(&g_coord, UC_WEBGPU, &w, sizeof(w), 1000);
        return;
    }
    case TU_NAVIGATE:
        /* A new tab only for a click the user gave this tab (ui_core.h). */
        if (u.new_tab && !ui_may_open_tab(&t->gesture, GetTickCount())) {
            ui_log("[ui] tab %u may not open a tab now (no click): %s", t->id, u.text);
            return;
        }
        ui_log("[ui] tab %u navigates to %s%s", t->id, u.text, u.new_tab ? " (new tab)" : "");
        if (u.new_tab) open_tab_in(t->win, u.text);
        else navigate(t, u.text);
        return;
    default:
        return;
    }
}

static void from_gpu(void) {
    if (g_gpu_rx.type == GU_SHOWN && g_gpu_rx.total == sizeof(GuShown)) {
        GuShown x;
        memcpy(&x, g_gpu_rx.buf, sizeof(x));
        Tab *t = tab_by_id(x.tab);
        /* Shown in the window it is in now (not one it was dragged out of). */
        if (t && g_wins[t->win].content == (HWND)(uintptr_t)x.hwnd) {
            t->shown = true;
            show_content(&g_wins[t->win]);
        }
    }
}

static void drain_gpu(void) {
    int r;
    while (g_have_gpu && (r = big_recv(&g_gpu, &g_gpu_rx)) > 0) from_gpu();
    if (g_have_gpu && r < 0) {
        ui_log("[ui] the GPU process broke its channel");
        chan_close(&g_gpu);
        g_have_gpu = false;
    }
}

static void drain_all(void) {
    static ChanMsg m;
    int r;
    while ((r = chan_recv(&g_coord, &m)) > 0) from_coord(&m);
    if (r < 0) ExitProcess(3);
    drain_gpu();
    for (int i = 0; i < MAX_TABS; i++) {
        Tab *t = &g_tabs[i];
        while (t->used && t->started && (r = chan_recv(&t->chan, &m)) > 0) from_tab(t, &m);
        if (t->used && t->started && r < 0) {
            ui_log("[ui] tab %u broke its channel", t->id);
            end_process(t);
            t->state = ST_CRASHED;
            snprintf(t->title, sizeof(t->title), "Crashed");
            update_tab(t);
        }
    }
}

/* ---- painting ----------------------------------------------------------------- */

static RECT tab_rect(const Win *w, int i) {
    int avail = w->cw - NEW_TAB_W - 8;
    int tw = w->n ? avail / w->n : TAB_MAX_W;
    if (tw > TAB_MAX_W) tw = TAB_MAX_W;
    RECT r = { 4 + i * tw, 4, 4 + (i + 1) * tw - 2, STRIP_H };
    return r;
}

static RECT close_rect(RECT tr) {
    RECT r = { tr.right - 22, tr.top + 7, tr.right - 6, tr.top + 23 };
    return r;
}

static RECT new_tab_rect(const Win *w) {
    RECT last = w->n ? tab_rect(w, w->n - 1) : (RECT){ 0, 4, 2, STRIP_H };
    RECT r = { last.right + 4, 6, last.right + 4 + NEW_TAB_W - 6, STRIP_H - 4 };
    return r;
}

/* The strip position a tab dropped at client x would take. */
static int drop_pos(const Win *w, int x) {
    for (int i = 0; i < w->n; i++) {
        RECT r = tab_rect(w, i);
        if (x < (r.left + r.right) / 2) return i;
    }
    return w->n;
}

static void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void paint_to(const Win *w, HDC dc, int cw, int ch) {
    fill(dc, (RECT){ 0, 0, cw, CONTENT_Y }, RGB(222, 225, 230));
    SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < w->n; i++) {
        RECT r = tab_rect(w, i);
        fill(dc, r, i == w->active ? RGB(255, 255, 255) : RGB(232, 234, 237));
        RECT tr = { r.left + 10, r.top, r.right - 26, r.bottom };
        const Tab *t = &g_tabs[w->order[i]];
        wchar_t buf[PROTO_TITLE_MAX + 16];
        const char *label = t->title[0] ? t->title : t->state <= ST_LOADING ? "Loading..." : t->url;
        wide(label, buf, PROTO_TITLE_MAX + 1);
        SetTextColor(dc, RGB(32, 33, 36));
        DrawTextW(dc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        DrawTextW(dc, L"\x00d7", -1, (RECT[]){ close_rect(r) }, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    RECT nt = new_tab_rect(w);
    DrawTextW(dc, L"+", -1, &nt, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    /* Under the content window, when it is shown: what to show until then. */
    RECT content = { 0, CONTENT_Y, cw, ch };
    const Tab *t = active_tab(w);
    fill(dc, content, RGB(255, 255, 255));
    if (!(t && t->started && t->shown)) {
        const wchar_t *msg = !t ? L"" : t->state == ST_FAILED ? L"This page could not be loaded."
                           : t->state == ST_CRASHED ? L"This tab's process ended." : L"Loading...";
        SetTextColor(dc, RGB(95, 99, 104));
        DrawTextW(dc, msg, -1, &content, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
}

static void paint_window(Win *w) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w->wnd, &ps);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w->cw, w->ch);
    HGDIOBJ old = SelectObject(mem, bmp);
    paint_to(w, mem, w->cw, w->ch);
    BitBlt(dc, 0, 0, w->cw, w->ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(w->wnd, &ps);
}

/* A content window's pixels, from the GPU process (BGRX, top-down, into a
 * malloc'd *px): false if it shows nothing. */
static bool content_pixels(const Win *w, uint8_t **px, uint32_t *pw, uint32_t *ph) {
    UgShot q = { (uint64_t)(uintptr_t)w->content };
    if (!g_have_gpu || !big_send(&g_gpu, UG_SHOT, &q, sizeof(q), NULL, 0, NULL, 0, 1000)) return false;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 10000) {
        int r = big_recv(&g_gpu, &g_gpu_rx);
        if (r < 0) return false;
        if (r == 0) {
            chan_wait(&g_gpu, 50);
            continue;
        }
        if (g_gpu_rx.type != GU_SHOT) {
            from_gpu();
            continue;
        }
        GuShot s;
        if (g_gpu_rx.total < sizeof(s)) return false;
        memcpy(&s, g_gpu_rx.buf, sizeof(s));
        if (!s.width || !s.height || s.width > SURFACE_MAX_W || s.height > SURFACE_MAX_H ||
            g_gpu_rx.total - sizeof(s) != (size_t)s.width * s.height * 4)
            return false;
        *px = (uint8_t *)malloc((size_t)s.width * s.height * 4);
        if (!*px) return false;
        memcpy(*px, g_gpu_rx.buf + sizeof(s), (size_t)s.width * s.height * 4);
        *pw = s.width;
        *ph = s.height;
        return true;
    }
    return false;
}

/* Window w as the user sees it, with the address bar, saved as a BMP. */
static bool save_shot(const Win *w, const char *path) {
    HDC wdc = GetDC(w->wnd);
    HDC mem = CreateCompatibleDC(wdc);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w->cw;
    bi.bmiHeader.biHeight = w->ch;             /* bottom-up, as BMP files are */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    paint_to(w, mem, w->cw, w->ch);
    /* The content, as the GPU process drew it. */
    uint8_t *px = NULL;
    uint32_t pw = 0, ph = 0;
    if (IsWindowVisible(w->content) && content_pixels(w, &px, &pw, &ph)) {
        BITMAPINFO pi;
        memset(&pi, 0, sizeof(pi));
        pi.bmiHeader.biSize = sizeof(pi.bmiHeader);
        pi.bmiHeader.biWidth = (LONG)pw;
        pi.bmiHeader.biHeight = -(LONG)ph;
        pi.bmiHeader.biPlanes = 1;
        pi.bmiHeader.biBitCount = 32;
        pi.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(mem, 0, CONTENT_Y, pw, ph, 0, 0, 0, ph, px, &pi, DIB_RGB_COLORS);
        free(px);
    }
    /* The address bar is a child window: draw it too. */
    RECT ar;
    GetWindowRect(w->addr, &ar);
    MapWindowPoints(NULL, w->wnd, (POINT *)&ar, 2);
    SetViewportOrgEx(mem, ar.left, ar.top, NULL);
    SendMessageW(w->addr, WM_PRINT, (WPARAM)mem, PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
    SetViewportOrgEx(mem, 0, 0, NULL);
    GdiFlush();
    int stride = ((w->cw * 3) + 3) & ~3;
    BITMAPFILEHEADER fh;
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + (DWORD)stride * w->ch;
    FILE *f = fopen(path, "wb");
    bool ok = false;
    if (f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        fwrite(bits, (size_t)stride, (size_t)w->ch, f);
        fclose(f);
        ok = true;
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(w->wnd, wdc);
    return ok;
}

/* ---- input ------------------------------------------------------------------- */

static void send_active(Win *w, uint16_t type, const void *p, uint32_t n) {
    Tab *t = active_tab(w);
    if (t && t->started && chan_send(&t->chan, type, p, n, 100) && type == UT_CLICK)
        ui_gesture(&t->gesture, GetTickCount());    /* lets it open one tab */
}

/* To the tab and, so the content moves at once, to the GPU process. */
static void scroll(Win *w, int dy) {
    Tab *t = active_tab(w);
    if (!t || !t->started) return;
    UtScroll s = { dy, ++t->scroll_seq };
    chan_send(&t->chan, UT_SCROLL, &s, sizeof(s), 100);
    UgScroll g = { t->id, s.seq, dy };
    if (g_have_gpu) big_send(&g_gpu, UG_SCROLL, &g, sizeof(g), NULL, 0, NULL, 0, 100);
}

static void select_tab(Win *w, int i) {
    if (i < 0 || i >= w->n) return;
    w->active = i;
    update_chrome(w);
}

/* The content window: the GPU process presents into it.  Mouse input goes
 * through it to the main window, which sends it to the tab. */
static LRESULT CALLBACK content_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        ValidateRect(w, NULL);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

/* A press on a tab: it may become a drag (DRAG_START px away). */
static struct {
    bool down, dragging;
    int tab;                    /* g_tabs index */
    POINT at;                   /* where it began, client */
} g_drag;

static void mouse_down(int wi, int x, int y) {
    Win *w = &g_wins[wi];
    POINT p = { x, y };
    if (y < STRIP_H) {
        for (int i = 0; i < w->n; i++) {
            RECT r = tab_rect(w, i);
            if (!PtInRect(&r, p)) continue;
            RECT c = close_rect(r);
            if (PtInRect(&c, p)) {
                close_tab(wi, i);
                return;
            }
            select_tab(w, i);
            g_drag.down = true;
            g_drag.dragging = false;
            g_drag.tab = w->order[i];
            g_drag.at = p;
            SetCapture(w->wnd);
            return;
        }
        RECT nt = new_tab_rect(w);
        if (PtInRect(&nt, p)) { open_tab_in(wi, "about:blank"); SetFocus(w->addr); }
        return;
    }
    if (y >= CONTENT_Y) {
        SetFocus(w->wnd);
        UtPoint pt = { x, y - CONTENT_Y };
        send_active(w, UT_CLICK, &pt, sizeof(pt));
    }
}

static void mouse_move(int wi, int x, int y) {
    if (!g_drag.down) return;
    (void)wi;
    if (!g_drag.dragging && abs(x - g_drag.at.x) + abs(y - g_drag.at.y) >= DRAG_START) {
        g_drag.dragging = true;
        SetCursor(LoadCursor(NULL, IDC_SIZEALL));
    }
}

/* The drag ends where the mouse is released: along a strip, onto another
 * window's strip, or anywhere else, into a window of its own. */
static void mouse_up(int wi, int x, int y) {
    if (!g_drag.down) return;
    bool dragging = g_drag.dragging;
    int ti = g_drag.tab;
    g_drag.down = g_drag.dragging = false;
    ReleaseCapture();
    if (!dragging || !g_tabs[ti].used) return;
    POINT sp = { x, y };
    ClientToScreen(g_wins[wi].wnd, &sp);
    HWND under = WindowFromPoint(sp);
    int target = under ? win_of_hwnd(GetAncestor(under, GA_ROOT)) : -1;    if (target >= 0) {
        POINT cp = sp;
        ScreenToClient(g_wins[target].wnd, &cp);
        if (cp.y >= 0 && cp.y < STRIP_H) {
            Win *tw = &g_wins[target];
            int pos = drop_pos(tw, cp.x);
            if (target == g_tabs[ti].win) {
                /* Along its own strip. */
                int cur = -1;
                for (int i = 0; i < tw->n; i++)
                    if (tw->order[i] == ti) cur = i;
                if (cur >= 0 && pos > cur) pos--;
                remove_tab(ti);
                insert_tab(target, ti, pos);
                update_chrome(tw);
            } else {
                move_tab(ti, target, pos);
            }
            return;
        }
    }
    /* Out of every strip: a window of its own (unless it is alone in one,
     * which then just moves). */
    Win *src = &g_wins[g_tabs[ti].win];
    if (src->n == 1) {
        SetWindowPos(src->wnd, NULL, sp.x - 40, sp.y - 10, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        return;
    }
    detach_tab(ti, sp.x - 40, sp.y - 10);
}

static bool shortcut(int wi, WPARAM key) {
    Win *w = &g_wins[wi];
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (ctrl && key == 'T') { open_tab_in(wi, "about:blank"); SetFocus(w->addr); return true; }
    if (ctrl && key == 'N') {
        int nw = new_window(CW_USEDEFAULT, CW_USEDEFAULT, w->cw, w->ch);
        if (nw >= 0) { open_tab_in(nw, "about:blank"); SetFocus(g_wins[nw].addr); }
        return true;
    }
    if (ctrl && key == 'W') { close_tab(wi, w->active); return true; }
    if (ctrl && key == 'L') { SetFocus(w->addr); SendMessageW(w->addr, EM_SETSEL, 0, -1); return true; }
    if (ctrl && key == VK_TAB && w->n) { select_tab(w, (w->active + 1) % w->n); return true; }
    return false;
}

static LRESULT CALLBACK addr_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    int wi = win_of_hwnd(GetParent(h));
    if (wi >= 0 && msg == WM_KEYDOWN && shortcut(wi, wp)) return 0;
    if (wi >= 0 && msg == WM_KEYDOWN && wp == VK_RETURN) {
        Win *w = &g_wins[wi];
        wchar_t wbuf[PROTO_URL_MAX];
        char in[PROTO_URL_MAX], url[PROTO_URL_MAX + 16];
        GetWindowTextW(h, wbuf, PROTO_URL_MAX);
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, in, sizeof(in), NULL, NULL);
        typed_url(in, url, sizeof(url));
        Tab *t = active_tab(w);
        if (!t) open_tab_in(wi, url);
        else navigate(t, url);
        SetFocus(w->wnd);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    return CallWindowProcW(g_addr_proc, h, msg, wp, lp);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    int wi = win_of_hwnd(h);
    if (wi < 0) return DefWindowProcW(h, msg, wp, lp);
    Win *w = &g_wins[wi];
    switch (msg) {
    case WM_PAINT:
        paint_window(w);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) g_focus = wi;
        break;
    case WM_TIMER:
        drain_all();            /* also during Windows' move and resize loops */
        return 0;
    case WM_SIZE:
        w->cw = LOWORD(lp);
        w->ch = HIWORD(lp);
        if (w->addr) MoveWindow(w->addr, 10, STRIP_H + 5, w->cw - 20, ADDR_H - 10, TRUE);
        if (w->content) MoveWindow(w->content, 0, CONTENT_Y, content_w(w), content_h(w), FALSE);
        for (int i = 0; i < w->n; i++) resize_tab(&g_tabs[w->order[i]]);
        show_content(w);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        mouse_down(wi, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE:
        mouse_move(wi, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        mouse_up(wi, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_CAPTURECHANGED:
        g_drag.down = g_drag.dragging = false;
        return 0;
    case WM_MOUSEWHEEL:
        scroll(w, -GET_WHEEL_DELTA_WPARAM(wp) * 100 / WHEEL_DELTA);
        return 0;
    case WM_KEYDOWN:
        if (shortcut(wi, wp)) return 0;
        switch (wp) {
        case VK_NEXT: case VK_SPACE: scroll(w, content_h(w) * 9 / 10); return 0;
        case VK_PRIOR: scroll(w, -content_h(w) * 9 / 10); return 0;
        case VK_DOWN: scroll(w, 40); return 0;
        case VK_UP: scroll(w, -40); return 0;
        case VK_HOME: scroll(w, -100000000); return 0;
        case VK_END: scroll(w, 100000000); return 0;
        }
        break;
    case WM_CLOSE:
        close_window(wi);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- the test script ------------------------------------------------------------ */

static FILE *g_script;
static char g_cmd[PROTO_URL_MAX + 64];
static bool g_cmd_pending;
static DWORD g_cmd_start;

/* Run (or continue) the current command; true when it is done. */
static bool script_command(void) {
    char arg[PROTO_URL_MAX + 64] = "";
    int a = 0, b = 0;
    DWORD now = GetTickCount();
    if (!g_wins[g_focus].used) return true;
    Win *w = &g_wins[g_focus];
    Tab *t = active_tab(w);
    if (sscanf(g_cmd, "open %4095s", arg) == 1) { open_tab_in(g_focus, arg); return true; }
    if (sscanf(g_cmd, "go %4095s", arg) == 1) { if (t) navigate(t, arg); return true; }
    if (sscanf(g_cmd, "wait %d", &a) == 1) return now - g_cmd_start >= (DWORD)a;
    /* waitfile <path> <ms>: until the file exists (a test server says it is done), or ms. */
    if (sscanf(g_cmd, "waitfile %4095s %d", arg, &a) == 2)
        return GetFileAttributesA(arg) != INVALID_FILE_ATTRIBUTES || now - g_cmd_start >= (DWORD)a;
    if (!strncmp(g_cmd, "waitload", 8)) {
        int limit = 60000, quiet = 1000;
        sscanf(g_cmd, "waitload %d %d", &limit, &quiet);
        /* Loaded, painted, and no new frame for QUIET ms (default a second). */
        bool done = t && (t->state == ST_FAILED || t->state == ST_CRASHED ||
                          (t->state == ST_LOADED && t->has_frame && now - t->last_frame_tick > (DWORD)quiet));
        bool timed_out = !done && now - g_cmd_start > (DWORD)limit;
        /* From the tab process's start: the first frame, the load event, and
         * the last frame before it went quiet (settled: what the eye sees). */
        if ((done || timed_out) && t)
            ui_log("[script] load timing: first frame %.0f ms, loaded %.0f ms, %s %.0f ms (%u frames)",
                   t->frame_ms ? t->frame_ms - t->start_ms : -1.0, t->loaded_ms ? t->loaded_ms - t->start_ms : -1.0,
                   done ? "settled" : "still painting at timeout, last frame", t->last_frame_ms ? t->last_frame_ms - t->start_ms : -1.0,
                   t->frames);
        if (timed_out) { ui_log("[script] waitload timed out"); return true; }
        return done;
    }
    if (sscanf(g_cmd, "click %d %d", &a, &b) == 2) { UtPoint p = { a, b }; send_active(w, UT_CLICK, &p, sizeof(p)); return true; }
    if (sscanf(g_cmd, "scroll %d", &a) == 1) { scroll(w, a); return true; }
    if (sscanf(g_cmd, "select %d", &a) == 1) { select_tab(w, a); return true; }
    if (!strncmp(g_cmd, "detach", 6)) {
        if (t) {
            RECT r;
            GetWindowRect(w->wnd, &r);
            detach_tab((int)(t - g_tabs), r.left + 60, r.top + 60);
            ui_log("[script] now in window %d", g_focus);
        }
        return true;
    }
    if (sscanf(g_cmd, "attach %d", &a) == 1) {
        if (t && a >= 0 && a < MAX_WINS && g_wins[a].used && a != g_focus) {
            move_tab((int)(t - g_tabs), a, g_wins[a].n);
            g_focus = a;
        }
        return true;
    }
    if (!strncmp(g_cmd, "dragout", 7) || sscanf(g_cmd, "dragto %d", &a) == 1) {
        /* The mouse's way: press on the active tab, move, release below the
         * window (dragout) or at the end of window a's strip (dragto). */
        if (!t || (g_cmd[4] == 't' && (a < 0 || a >= MAX_WINS || !g_wins[a].used))) return true;
        int wi = g_focus;
        RECT r = tab_rect(w, w->active);
        POINT from = { (r.left + r.right) / 2, STRIP_H / 2 }, to;
        if (g_cmd[4] == 'o') {
            to.x = from.x;
            to.y = w->ch + 150;
        } else {
            RECT nt = new_tab_rect(&g_wins[a]);
            to.x = nt.right + 10;
            to.y = STRIP_H / 2;
            ClientToScreen(g_wins[a].wnd, &to);
            ScreenToClient(w->wnd, &to);
        }
        /* Where a user would drop it, the strip is in sight: on top of other
         * programs' windows for the drop. */
        HWND above = g_cmd[4] == 't' ? g_wins[a].wnd : NULL;
        UINT keep = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
        if (above) SetWindowPos(above, HWND_TOPMOST, 0, 0, 0, 0, keep);
        mouse_down(wi, from.x, from.y);
        mouse_move(wi, to.x, to.y);
        mouse_up(wi, to.x, to.y);
        if (above) SetWindowPos(above, HWND_NOTOPMOST, 0, 0, 0, 0, keep);
        ui_log("[script] now in window %d", g_tabs[t - g_tabs].win);
        g_focus = g_tabs[t - g_tabs].win;
        return true;
    }
    if (sscanf(g_cmd, "window %d", &a) == 1) { if (a >= 0 && a < MAX_WINS && g_wins[a].used) g_focus = a; return true; }
    if (!strncmp(g_cmd, "close", 5)) { close_tab(g_focus, w->active); return true; }
    if (sscanf(g_cmd, "resize %d %d", &a, &b) == 2) {
        RECT r = { 0, 0, a, b };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(w->wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        return true;
    }
    if (sscanf(g_cmd, "shot %4095s", arg) == 1) {
        drain_all();
        ui_log("[script] shot %s: %s", arg, save_shot(w, arg) ? "saved" : "FAILED");
        return true;
    }
    if (!strncmp(g_cmd, "log ", 4)) { ui_log("[script] %s", g_cmd + 4); return true; }
    if (!strncmp(g_cmd, "quit", 4)) {
        for (int i = MAX_WINS - 1; i >= 0; i--) close_window(i);
        return true;
    }
    if (g_cmd[0]) ui_log("[script] unknown command: %s", g_cmd);
    return true;
}

static void script_step(void) {
    while (g_script) {
        if (!g_cmd_pending) {
            if (!fgets(g_cmd, sizeof(g_cmd), g_script)) { fclose(g_script); g_script = NULL; return; }
            g_cmd[strcspn(g_cmd, "\r\n")] = 0;
            if (g_cmd[0] == '#') continue;
            g_cmd_pending = true;
            g_cmd_start = GetTickCount();
            ui_log("[script] %s", g_cmd);
        }
        if (!script_command()) return;
        g_cmd_pending = false;
    }
}

/* ---- main --------------------------------------------------------------------- */

int main(int argc, char **argv) {
    SetProcessDPIAware();
    if (argc < 5 || strcmp(argv[1], "--chan")) { fprintf(stderr, "cb_ui: bad arguments\n"); return 1; }
    if (!chan_attach(&g_coord, spawn_handle_from_arg(argv[2]), spawn_handle_from_arg(argv[3]),
                     spawn_handle_from_arg(argv[4]), 1)) {
        fprintf(stderr, "cb_ui: no channel\n");
        return 1;
    }

    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"CyberBrowser";
    RegisterClassW(&wc);
    WNDCLASSW cc;
    memset(&cc, 0, sizeof(cc));
    cc.lpfnWndProc = content_proc;
    cc.hInstance = wc.hInstance;
    cc.hCursor = LoadCursor(NULL, IDC_ARROW);
    cc.lpszClassName = L"CyberContent";
    RegisterClassW(&cc);
    g_focus = new_window(CW_USEDEFAULT, CW_USEDEFAULT, 1280, 900);
    if (g_focus < 0) return 1;

    for (int i = 5; i < argc; i++) {
        char url[PROTO_URL_MAX + 1];
        size_t n = strlen(argv[i]) / 2;
        if (n > PROTO_URL_MAX) continue;
        for (size_t k = 0; k < n; k++) {
            unsigned v = 0;
            sscanf(argv[i] + 2 * k, "%2x", &v);
            url[k] = (char)v;
        }
        url[n] = 0;
        open_tab_in(g_focus, url);
    }
    const char *script = getenv("CYBER_UI_SCRIPT");
    if (script && *script && !(g_script = fopen(script, "r"))) ui_log("[ui] cannot open script %s", script);
    if (g_wins[g_focus].n == 0 && !g_script) { open_tab_in(g_focus, "about:blank"); SetFocus(g_wins[g_focus].addr); }

    for (;;) {
        /* Sleep until a window message or a message on any channel. */
        HANDLE waits[MAX_TABS + 2];
        int n = 0;
        bool ready = !chan_arm(&g_coord);
        waits[n++] = chan_event(&g_coord);
        if (g_have_gpu) {
            if (!chan_arm(&g_gpu)) ready = true;
            waits[n++] = chan_event(&g_gpu);
        }
        for (int i = 0; i < MAX_TABS; i++) {
            if (!g_tabs[i].used || !g_tabs[i].started) continue;
            if (!chan_arm(&g_tabs[i].chan)) ready = true;
            waits[n++] = chan_event(&g_tabs[i].chan);
        }
        if (!ready) MsgWaitForMultipleObjectsEx((DWORD)n, waits, g_script ? 15 : INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        chan_disarm(&g_coord);
        if (g_have_gpu) chan_disarm(&g_gpu);
        for (int i = 0; i < MAX_TABS; i++)
            if (g_tabs[i].used && g_tabs[i].started) chan_disarm(&g_tabs[i].chan);
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        drain_all();
        script_step();
        tell_visibility();
    }
}
