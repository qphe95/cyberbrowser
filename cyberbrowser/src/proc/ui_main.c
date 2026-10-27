/* ui_main.c - cb_ui.exe: the UI process (SANDBOX_DESIGN.md section 2.2).
 *
 * Started by the coordinator with its channel (and start URLs, hex):
 *   cb_ui.exe --chan S E0 E1 [HEX ...]
 *
 * Owns the window: the tab strip, the address bar and the content area, and
 * the tab table (which tabs exist, which is active, each one's URL and
 * title).  Each tab paints into a surface the UI maps read-only; the UI shows
 * the active tab's latest frame and sends it the user's input.  Following a
 * link or typing a URL replaces the tab's process with a new one.
 *
 * Keys: Ctrl+T new tab, Ctrl+W close tab, Ctrl+L address bar, Ctrl+Tab next
 * tab, Page Up/Down, arrows and Space scroll.
 *
 * CYBER_UI_SCRIPT=<file>: run commands from a file, one per line, for tests:
 *   open URL | go URL | waitload [MS] | wait MS | click X Y | scroll DY |
 *   select I | close | resize W H | shot FILE.bmp | log TEXT | quit */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <windowsx.h>

#include "chan.h"
#include "proto.h"
#include "spawn.h"

#define MAX_TABS 60
#define STRIP_H 34
#define ADDR_H 34
#define CONTENT_Y (STRIP_H + ADDR_H)
#define TAB_MAX_W 220
#define NEW_TAB_W 34

enum { ST_STARTING, ST_LOADING, ST_LOADED, ST_FAILED, ST_CRASHED };

typedef struct Tab {
    uint32_t id;                /* of its current process */
    char url[PROTO_URL_MAX + 1];
    char title[PROTO_TITLE_MAX + 1];
    int state;
    bool started;               /* channel and surface attached */
    Chan chan;                  /* side 0: the UI */
    HANDLE surface_h;
    const uint8_t *surface;
    bool has_frame;
    TuFrame frame;
    DWORD last_frame_tick;
} Tab;

static Chan g_coord;            /* side 1: the UI */
static Tab g_tabs[MAX_TABS];
static int g_ntabs, g_active = -1;
static uint32_t g_next_id = 1;
static HWND g_wnd, g_addr;
static WNDPROC g_addr_proc;
static HFONT g_font;
static int g_cw = 1280, g_ch = 900;     /* client size */

static void ui_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static int content_w(void) { int w = g_cw; return w < 1 ? 1 : w > SURFACE_MAX_W ? SURFACE_MAX_W : w; }
static int content_h(void) { int h = g_ch - CONTENT_Y; return h < 1 ? 1 : h > SURFACE_MAX_H ? SURFACE_MAX_H : h; }

static wchar_t *wide(const char *s, wchar_t *buf, int cap) {
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, cap)) buf[0] = 0;
    buf[cap - 1] = 0;
    return buf;
}

/* ---- tabs ---------------------------------------------------------------- */

static void release(Tab *t) {
    if (t->started) {
        chan_close(&t->chan);
        if (t->surface) UnmapViewOfFile(t->surface);
        if (t->surface_h) CloseHandle(t->surface_h);
    }
    t->started = false;
    t->surface = NULL;
    t->surface_h = NULL;
    t->has_frame = false;
}

static void start_process(Tab *t) {
    static uint8_t msg[sizeof(UcOpenTab) + PROTO_URL_MAX];
    t->id = g_next_id++;
    t->state = ST_STARTING;
    t->has_frame = false;
    t->title[0] = 0;
    UcOpenTab o = { t->id, (uint32_t)content_w(), (uint32_t)content_h() };
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

static void update_chrome(void) {
    wchar_t buf[PROTO_URL_MAX + 64];
    if (g_active >= 0) {
        Tab *t = &g_tabs[g_active];
        if (GetFocus() != g_addr) SetWindowTextW(g_addr, wide(t->url, buf, PROTO_URL_MAX + 1));
        wchar_t title[PROTO_TITLE_MAX + 1];
        wide(t->title[0] ? t->title : t->url, title, PROTO_TITLE_MAX + 1);
        _snwprintf(buf, PROTO_URL_MAX + 64, L"%ls - CyberBrowser", title);
        buf[PROTO_URL_MAX + 63] = 0;
        SetWindowTextW(g_wnd, buf);
    } else {
        SetWindowTextW(g_addr, L"");
        SetWindowTextW(g_wnd, L"CyberBrowser");
    }
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void navigate(Tab *t, const char *url) {
    end_process(t);
    snprintf(t->url, sizeof(t->url), "%s", url);
    start_process(t);
    update_chrome();
}

static void open_tab(const char *url) {
    if (g_ntabs >= MAX_TABS) return;
    Tab *t = &g_tabs[g_ntabs];
    memset(t, 0, sizeof(*t));
    snprintf(t->url, sizeof(t->url), "%s", url);
    g_active = g_ntabs++;
    start_process(t);
    update_chrome();
}

static void close_tab(int i) {
    if (i < 0 || i >= g_ntabs) return;
    end_process(&g_tabs[i]);
    memmove(&g_tabs[i], &g_tabs[i + 1], (size_t)(g_ntabs - i - 1) * sizeof(Tab));
    g_ntabs--;
    if (g_ntabs == 0) { DestroyWindow(g_wnd); return; }
    if (g_active >= g_ntabs) g_active = g_ntabs - 1;
    else if (g_active > i) g_active--;
    update_chrome();
}

static Tab *tab_by_id(uint32_t id) {
    for (int i = 0; i < g_ntabs; i++)
        if (g_tabs[i].id == id) return &g_tabs[i];
    return NULL;
}

static void resize_tabs(void) {
    UtSize s = { (uint32_t)content_w(), (uint32_t)content_h() };
    for (int i = 0; i < g_ntabs; i++)
        if (g_tabs[i].started) chan_send(&g_tabs[i].chan, UT_RESIZE, &s, sizeof(s), 100);
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

/* ---- messages from the coordinator and the tabs ----------------------------- */

static void from_coord(const ChanMsg *m) {
    switch (m->type) {
    case CU_TAB_STARTED: {
        if (m->len != sizeof(CuTabStarted)) return;
        CuTabStarted s;
        memcpy(&s, m->data, sizeof(s));
        HANDLE hs = (HANDLE)(uintptr_t)s.chan_section, h0 = (HANDLE)(uintptr_t)s.chan_ev0,
               h1 = (HANDLE)(uintptr_t)s.chan_ev1, hsurf = (HANDLE)(uintptr_t)s.surface;
        Tab *t = tab_by_id(s.tab);
        if (!t || s.surface_bytes != SURFACE_BYTES) {
            /* A tab closed (or navigated) since it was asked for: drop its
             * handles and end its process. */
            CloseHandle(hs); CloseHandle(h0); CloseHandle(h1); CloseHandle(hsurf);
            UcTab c = { s.tab };
            chan_send(&g_coord, UC_CLOSE_TAB, &c, sizeof(c), 1000);
            return;
        }
        if (!chan_attach(&t->chan, hs, h0, h1, 0)) { t->state = ST_FAILED; return; }
        t->surface_h = hsurf;
        t->surface = (const uint8_t *)MapViewOfFile(hsurf, FILE_MAP_READ, 0, 0, SURFACE_BYTES);
        t->started = true;
        ui_log("[ui] tab %u started", t->id);
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
        update_chrome();
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
        update_chrome();
        return;
    }
    }
}

static void from_tab(Tab *t, const ChanMsg *m) {
    switch (m->type) {
    case TU_FRAME: {
        if (m->len != sizeof(TuFrame)) return;
        TuFrame f;
        memcpy(&f, m->data, sizeof(f));
        if (f.buffer > 1 || f.width == 0 || f.height == 0 || f.width > SURFACE_MAX_W || f.height > SURFACE_MAX_H) return;
        t->frame = f;
        t->has_frame = true;
        t->last_frame_tick = GetTickCount();
        /* From now on only f.buffer is read: the tab may reuse the other. */
        UtFrameDone d = { f.frame };
        chan_send(&t->chan, UT_FRAME_DONE, &d, sizeof(d), 100);
        if (t == &g_tabs[g_active]) {
            RECT r = { 0, CONTENT_Y, g_cw, g_ch };
            InvalidateRect(g_wnd, &r, FALSE);
        }
        return;
    }
    case TU_TITLE:
        if (m->len > PROTO_TITLE_MAX) return;
        memcpy(t->title, m->data, m->len);
        t->title[m->len] = 0;
        update_chrome();
        return;
    case TU_STATE: {
        if (m->len != sizeof(TuState)) return;
        TuState s;
        memcpy(&s, m->data, sizeof(s));
        t->state = s.state == TAB_LOADED ? ST_LOADED : s.state == TAB_LOAD_FAILED ? ST_FAILED : ST_LOADING;
        InvalidateRect(g_wnd, NULL, FALSE);
        return;
    }
    case TU_NAVIGATE: {
        if (m->len <= sizeof(TuNavigate) || m->len > sizeof(TuNavigate) + PROTO_URL_MAX) return;
        TuNavigate n;
        memcpy(&n, m->data, sizeof(n));
        char url[PROTO_URL_MAX + 1];
        uint32_t ulen = m->len - (uint32_t)sizeof(n);
        memcpy(url, m->data + sizeof(n), ulen);
        url[ulen] = 0;
        ui_log("[ui] tab %u navigates to %s%s", t->id, url, n.new_tab ? " (new tab)" : "");
        if (n.new_tab) open_tab(url);
        else navigate(t, url);
        return;
    }
    }
}

static void drain_all(void) {
    static ChanMsg m;
    int r;
    while ((r = chan_recv(&g_coord, &m)) > 0) from_coord(&m);
    if (r < 0) ExitProcess(3);
    for (int i = 0; i < g_ntabs; i++) {
        Tab *t = &g_tabs[i];
        while (t->started && (r = chan_recv(&t->chan, &m)) > 0) from_tab(t, &m);
        if (t->started && r < 0) {
            ui_log("[ui] tab %u broke its channel", t->id);
            end_process(t);
            t->state = ST_CRASHED;
            snprintf(t->title, sizeof(t->title), "Crashed");
            update_chrome();
        }
    }
}

/* ---- painting ----------------------------------------------------------------- */

static RECT tab_rect(int i) {
    int avail = g_cw - NEW_TAB_W - 8;
    int w = g_ntabs ? avail / g_ntabs : TAB_MAX_W;
    if (w > TAB_MAX_W) w = TAB_MAX_W;
    RECT r = { 4 + i * w, 4, 4 + (i + 1) * w - 2, STRIP_H };
    return r;
}

static RECT close_rect(RECT tr) {
    RECT r = { tr.right - 22, tr.top + 7, tr.right - 6, tr.top + 23 };
    return r;
}

static RECT new_tab_rect(void) {
    RECT last = g_ntabs ? tab_rect(g_ntabs - 1) : (RECT){ 0, 4, 2, STRIP_H };
    RECT r = { last.right + 4, 6, last.right + 4 + NEW_TAB_W - 6, STRIP_H - 4 };
    return r;
}

static void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void paint_to(HDC dc, int w, int h) {
    RECT all = { 0, 0, w, h };
    fill(dc, (RECT){ 0, 0, w, CONTENT_Y }, RGB(222, 225, 230));
    SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < g_ntabs; i++) {
        RECT r = tab_rect(i);
        fill(dc, r, i == g_active ? RGB(255, 255, 255) : RGB(232, 234, 237));
        RECT tr = { r.left + 10, r.top, r.right - 26, r.bottom };
        Tab *t = &g_tabs[i];
        wchar_t buf[PROTO_TITLE_MAX + 16];
        const char *label = t->title[0] ? t->title : t->state <= ST_LOADING ? "Loading..." : t->url;
        wide(label, buf, PROTO_TITLE_MAX + 1);
        SetTextColor(dc, RGB(32, 33, 36));
        DrawTextW(dc, buf, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        DrawTextW(dc, L"\x00d7", -1, (RECT[]){ close_rect(r) }, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    RECT nt = new_tab_rect();
    DrawTextW(dc, L"+", -1, &nt, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    RECT content = { 0, CONTENT_Y, w, h };
    Tab *t = g_active >= 0 ? &g_tabs[g_active] : NULL;
    if (t && t->started && t->has_frame && t->surface) {
        const TuFrame *f = &t->frame;
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = (LONG)f->width;
        bi.bmiHeader.biHeight = -(LONG)f->height;      /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        const uint8_t *px = t->surface + SURFACE_HEADER + (size_t)f->buffer * SURFACE_BUFFER_BYTES;
        SetDIBitsToDevice(dc, 0, CONTENT_Y, f->width, f->height, 0, 0, 0, f->height, px, &bi, DIB_RGB_COLORS);
        /* What the frame doesn't cover (the window grew since). */
        if ((int)f->width < w) fill(dc, (RECT){ (int)f->width, CONTENT_Y, w, h }, RGB(255, 255, 255));
        if (CONTENT_Y + (int)f->height < h) fill(dc, (RECT){ 0, CONTENT_Y + (int)f->height, w, h }, RGB(255, 255, 255));
    } else {
        fill(dc, content, RGB(255, 255, 255));
        const wchar_t *msg = !t ? L"" : t->state == ST_FAILED ? L"This page could not be loaded."
                           : t->state == ST_CRASHED ? L"This tab's process ended." : L"Loading...";
        SetTextColor(dc, RGB(95, 99, 104));
        DrawTextW(dc, msg, -1, &content, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    (void)all;
}

static void paint_window(void) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(g_wnd, &ps);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, g_cw, g_ch);
    HGDIOBJ old = SelectObject(mem, bmp);
    paint_to(mem, g_cw, g_ch);
    BitBlt(dc, 0, 0, g_cw, g_ch, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(g_wnd, &ps);
}

/* The window as the user sees it, with the address bar, saved as a BMP. */
static bool save_shot(const char *path) {
    HDC wdc = GetDC(g_wnd);
    HDC mem = CreateCompatibleDC(wdc);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = g_cw;
    bi.bmiHeader.biHeight = g_ch;              /* bottom-up, as BMP files are */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    paint_to(mem, g_cw, g_ch);
    /* The address bar is a child window: draw it too. */
    RECT ar;
    GetWindowRect(g_addr, &ar);
    MapWindowPoints(NULL, g_wnd, (POINT *)&ar, 2);
    SetViewportOrgEx(mem, ar.left, ar.top, NULL);
    SendMessageW(g_addr, WM_PRINT, (WPARAM)mem, PRF_CLIENT | PRF_NONCLIENT | PRF_ERASEBKGND);
    SetViewportOrgEx(mem, 0, 0, NULL);
    GdiFlush();
    int stride = ((g_cw * 3) + 3) & ~3;
    BITMAPFILEHEADER fh;
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + (DWORD)stride * g_ch;
    FILE *f = fopen(path, "wb");
    bool ok = false;
    if (f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        fwrite(bits, (size_t)stride, (size_t)g_ch, f);
        fclose(f);
        ok = true;
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(g_wnd, wdc);
    return ok;
}

/* ---- input ------------------------------------------------------------------- */

static void send_active(uint16_t type, const void *p, uint32_t n) {
    if (g_active < 0) return;
    Tab *t = &g_tabs[g_active];
    if (t->started) chan_send(&t->chan, type, p, n, 100);
}

static void scroll(int dy) {
    UtScroll s = { dy };
    send_active(UT_SCROLL, &s, sizeof(s));
}

static void select_tab(int i) {
    if (i < 0 || i >= g_ntabs) return;
    g_active = i;
    update_chrome();
}

static void mouse_down(int x, int y) {
    POINT p = { x, y };
    if (y < STRIP_H) {
        for (int i = 0; i < g_ntabs; i++) {
            RECT r = tab_rect(i);
            if (!PtInRect(&r, p)) continue;
            RECT c = close_rect(r);
            if (PtInRect(&c, p)) close_tab(i);
            else select_tab(i);
            return;
        }
        RECT nt = new_tab_rect();
        if (PtInRect(&nt, p)) { open_tab("about:blank"); SetFocus(g_addr); }
        return;
    }
    if (y >= CONTENT_Y) {
        SetFocus(g_wnd);
        UtPoint pt = { x, y - CONTENT_Y };
        send_active(UT_CLICK, &pt, sizeof(pt));
    }
}

static bool shortcut(WPARAM key) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (ctrl && key == 'T') { open_tab("about:blank"); SetFocus(g_addr); return true; }
    if (ctrl && key == 'W') { close_tab(g_active); return true; }
    if (ctrl && key == 'L') { SetFocus(g_addr); SendMessageW(g_addr, EM_SETSEL, 0, -1); return true; }
    if (ctrl && key == VK_TAB && g_ntabs) { select_tab((g_active + 1) % g_ntabs); return true; }
    return false;
}

static LRESULT CALLBACK addr_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && shortcut(wp)) return 0;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        wchar_t wbuf[PROTO_URL_MAX];
        char in[PROTO_URL_MAX], url[PROTO_URL_MAX + 16];
        GetWindowTextW(g_addr, wbuf, PROTO_URL_MAX);
        WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, in, sizeof(in), NULL, NULL);
        typed_url(in, url, sizeof(url));
        if (g_active < 0) open_tab(url);
        else navigate(&g_tabs[g_active], url);
        SetFocus(g_wnd);
        return 0;
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    return CallWindowProcW(g_addr_proc, w, msg, wp, lp);
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        paint_window();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        g_cw = LOWORD(lp);
        g_ch = HIWORD(lp);
        if (g_addr) MoveWindow(g_addr, 10, STRIP_H + 5, g_cw - 20, ADDR_H - 10, TRUE);
        resize_tabs();
        InvalidateRect(w, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL:
        scroll(-GET_WHEEL_DELTA_WPARAM(wp) * 100 / WHEEL_DELTA);
        return 0;
    case WM_KEYDOWN:
        if (shortcut(wp)) return 0;
        switch (wp) {
        case VK_NEXT: case VK_SPACE: scroll(content_h() * 9 / 10); return 0;
        case VK_PRIOR: scroll(-content_h() * 9 / 10); return 0;
        case VK_DOWN: scroll(40); return 0;
        case VK_UP: scroll(-40); return 0;
        case VK_HOME: scroll(-100000000); return 0;
        case VK_END: scroll(100000000); return 0;
        }
        break;
    case WM_DESTROY:
        chan_send(&g_coord, UC_QUIT, NULL, 0, 1000);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
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
    if (sscanf(g_cmd, "open %4095s", arg) == 1) { open_tab(arg); return true; }
    if (sscanf(g_cmd, "go %4095s", arg) == 1) { if (g_active >= 0) navigate(&g_tabs[g_active], arg); return true; }
    if (sscanf(g_cmd, "wait %d", &a) == 1) return now - g_cmd_start >= (DWORD)a;
    if (!strncmp(g_cmd, "waitload", 8)) {
        int limit = 60000;
        sscanf(g_cmd, "waitload %d", &limit);
        Tab *t = g_active >= 0 ? &g_tabs[g_active] : NULL;
        /* Loaded, painted, and no new frame for a second. */
        bool done = t && (t->state == ST_FAILED || t->state == ST_CRASHED ||
                          (t->state == ST_LOADED && t->has_frame && now - t->last_frame_tick > 1000));
        if (!done && now - g_cmd_start > (DWORD)limit) { ui_log("[script] waitload timed out"); return true; }
        return done;
    }
    if (sscanf(g_cmd, "click %d %d", &a, &b) == 2) { UtPoint p = { a, b }; send_active(UT_CLICK, &p, sizeof(p)); return true; }
    if (sscanf(g_cmd, "scroll %d", &a) == 1) { scroll(a); return true; }
    if (sscanf(g_cmd, "select %d", &a) == 1) { select_tab(a); return true; }
    if (!strncmp(g_cmd, "close", 5)) { close_tab(g_active); return true; }
    if (sscanf(g_cmd, "resize %d %d", &a, &b) == 2) {
        RECT r = { 0, 0, a, b };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(g_wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
        return true;
    }
    if (sscanf(g_cmd, "shot %4095s", arg) == 1) {
        drain_all();
        ui_log("[script] shot %s: %s", arg, save_shot(arg) ? "saved" : "FAILED");
        return true;
    }
    if (!strncmp(g_cmd, "log ", 4)) { ui_log("[script] %s", g_cmd + 4); return true; }
    if (!strncmp(g_cmd, "quit", 4)) { DestroyWindow(g_wnd); return true; }
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
    RECT r = { 0, 0, g_cw, g_ch };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowW(L"CyberBrowser", L"CyberBrowser", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                          NULL, NULL, wc.hInstance, NULL);
    g_addr = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                           10, STRIP_H + 5, g_cw - 20, ADDR_H - 10, g_wnd, NULL, wc.hInstance, NULL);
    SendMessageW(g_addr, WM_SETFONT, (WPARAM)g_font, TRUE);
    g_addr_proc = (WNDPROC)SetWindowLongPtrW(g_addr, GWLP_WNDPROC, (LONG_PTR)addr_proc);
    ShowWindow(g_wnd, SW_SHOW);

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
        open_tab(url);
    }
    const char *script = getenv("CYBER_UI_SCRIPT");
    if (script && *script && !(g_script = fopen(script, "r"))) ui_log("[ui] cannot open script %s", script);
    if (g_ntabs == 0 && !g_script) { open_tab("about:blank"); SetFocus(g_addr); }

    for (;;) {
        /* Sleep until a window message or a message on any channel. */
        HANDLE waits[MAX_TABS + 1];
        int n = 0;
        bool ready = !chan_arm(&g_coord);
        waits[n++] = chan_event(&g_coord);
        for (int i = 0; i < g_ntabs; i++) {
            if (!g_tabs[i].started) continue;
            if (!chan_arm(&g_tabs[i].chan)) ready = true;
            waits[n++] = chan_event(&g_tabs[i].chan);
        }
        if (!ready) MsgWaitForMultipleObjectsEx((DWORD)n, waits, g_script ? 15 : INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        chan_disarm(&g_coord);
        for (int i = 0; i < g_ntabs; i++)
            if (g_tabs[i].started) chan_disarm(&g_tabs[i].chan);
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        drain_all();
        script_step();
    }
}
