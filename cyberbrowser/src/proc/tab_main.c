/* tab_main.c - cb_tab.exe: a tab process (SANDBOX_DESIGN.md section 2.3).
 *
 * Started by the coordinator with its channels to the UI, the network process,
 * (unless its site is opaque) the storage service and the GPU process, the
 * viewport size and the URL (hex):
 *   cb_tab.exe --chan S E0 E1 --ctl S E0 E1 --net S E0 E1 [--store S E0 E1]
 *              --gpu S E0 E1 --size W H --tab N --url HEX
 * --ctl is the coordinator's channel to it, one way: a new draw channel
 * when the GPU process starts again.
 *
 * It has no network or files of its own: every request and document.cookie
 * goes to the network process, which adds the site's cookies, and
 * localStorage is loaded from and saved to the storage service.
 *
 * It loads the page with the page engine, then runs the page's event loop,
 * sends the part of each new frame around the viewport to the GPU process as
 * a display list (gpu_client.c), which draws it, and answers the
 * UI's input: resizes, scrolls and clicks.  A click on a link asks the UI to
 * navigate; every navigation gets a new tab process (the engine holds one
 * page per process). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#include "chan.h"
#include "bigmsg.h"
#include "proto.h"
#include "spawn.h"
#include "site.h"
#include "store_core.h"       /* STORE_QUOTA_BYTES */

#include "platform.h"
#include "quickjs.h"
#include "js_quickjs.h"
#include "det_clock.h"
#include "page_engine.h"
#include "dom_fetch.h"
#include "code_cache.h"
#include "gpu_client.h"
#include "http_download.h"
#include "image_cache.h"
#include "browser/storage_api.h"
#include "browser/webgpu_bridge.h"

unsigned long long timer_next_due_ms(void);
unsigned long long page_time_ms(void);

static Chan g_chan;                     /* side 1: the tab */
static Chan g_ctl;                      /* side 1: from the coordinator */
static bool g_have_ctl;
static uint32_t g_tab, g_width, g_height;
static uint32_t g_scroll, g_content_height;
static uint32_t g_scroll_seq;           /* the UI's last scroll (UtScroll.seq) */
static uint32_t g_frame_no;
static bool g_in_flight, g_need_paint;
/* Not shown (UT_VISIBLE): scripts and timers run, nothing is laid out or painted. */
static bool g_hidden;
void timer_set_raf_paused(bool paused);     /* (browser/timer_api.c) */
static char g_title[PROTO_TITLE_MAX];
/* The next document (UT_NAVIGATE): loaded in this process when the turn is
 * over, in place of this one.  Empty: none. */
static char g_next_url[PROTO_URL_MAX + 1];
static void navigate_request(const uint8_t *url, uint32_t len);

static void memory_report(const char *what) {
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof(pm));
    pm.cb = sizeof(pm);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm));
    /* (and when: since the process was created, and its CPU time so far) */
    FILETIME c, e, k, u, now;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ci = { .LowPart = c.dwLowDateTime, .HighPart = c.dwHighDateTime },
                   ni = { .LowPart = now.dwLowDateTime, .HighPart = now.dwHighDateTime },
                   ki = { .LowPart = k.dwLowDateTime, .HighPart = k.dwHighDateTime },
                   ui = { .LowPart = u.dwLowDateTime, .HighPart = u.dwHighDateTime };
    fprintf(stderr, "[tab %u] memory %s: private %.1f MB, working set %.1f MB (%.0f ms after the process was created "
            "at %.1f, %.0f ms cpu)\n", g_tab, what, pm.PrivateUsage / 1048576.0, pm.WorkingSetSize / 1048576.0,
            (ni.QuadPart - ci.QuadPart) / 1e4, (double)(ci.QuadPart % 1000000000ull) / 1e4,
            (ki.QuadPart + ui.QuadPart) / 1e4);
    fflush(stderr);
}

static void send_state(uint32_t state) {
    TuState s = { state };
    chan_send(&g_chan, TU_STATE, &s, sizeof(s), 1000);
}

/* ---- painting --------------------------------------------------------- */

static uint32_t frame_extent(const DisplayList *dl) {
    float bottom = 0;
    for (int i = 0; i < dl->count; i++) {
        float b = dl->cmds[i].y + dl->cmds[i].h;
        if (b > bottom) bottom = b;
    }
    if (bottom > 1e6f) bottom = 1e6f;
    return bottom > 0 ? (uint32_t)bottom : 0;
}

static void clamp_scroll(void) {
    uint32_t max = g_content_height > g_height ? g_content_height - g_height : 0;
    if (g_scroll > max) g_scroll = max;
}

/* The frame's display list moved up by dy, without what lies wholly
 * outside rows [0, h). */
static DisplayList g_shifted;
static int g_shifted_cap, g_shifted_mask_cap;

static void shift_frame(const DisplayList *dl, float dy, uint32_t h) {
    if (g_shifted_cap < dl->count) {
        g_shifted_cap = dl->count;
        g_shifted.cmds = (DisplayListCmd *)realloc(g_shifted.cmds, (size_t)g_shifted_cap * sizeof(DisplayListCmd));
    }
    if (g_shifted_mask_cap < dl->mask_count) {
        g_shifted_mask_cap = dl->mask_count;
        g_shifted.masks = (DisplayMask *)realloc(g_shifted.masks, (size_t)g_shifted_mask_cap * sizeof(DisplayMask));
    }
    int n = 0;
    for (int i = 0; i < dl->count; i++) {
        DisplayListCmd c = dl->cmds[i];
        c.y -= dy;
        bool bracket = c.type == DL_MASK_BEGIN || c.type == DL_MASK_END;
        if (!bracket && (c.y + c.h < 0 || c.y > (float)h)) continue;
        if (c.type == DL_BORDER) c.u.border.y -= dy;
        g_shifted.cmds[n++] = c;
    }
    for (int i = 0; i < dl->mask_count; i++) {
        DisplayMask m = dl->masks[i];
        m.y -= dy;
        m.iy -= dy;
        g_shifted.masks[i] = m;
    }
    g_shifted.count = n;
    g_shifted.capacity = g_shifted_cap;
    g_shifted.mask_count = dl->mask_count;
    g_shifted.mask_capacity = g_shifted_mask_cap;
}

/* The frame around the viewport (a layer, half a viewport more above and
 * below) to the GPU process, which draws it into this tab's content and
 * shows the viewport's part, moving it at once when the user scrolls; the
 * next goes when it has (GT_FRAME_DONE).  The UI hears the frame's size and
 * scroll position. */
static void paint(void) {
    PageFrame *f = page_frame();
    if (!f->valid) return;
    clamp_scroll();
    uint32_t margin = g_height / 2;
    uint32_t origin = g_scroll > margin ? g_scroll - margin : 0;
    uint32_t layer_h = g_scroll - origin + g_height + margin;
    if (g_content_height > origin && layer_h > g_content_height - origin) layer_h = g_content_height - origin;
    if (layer_h < g_height) layer_h = g_height;
    shift_frame(&f->dl, (float)origin, layer_h);
    GpuFrame hdr = { ++g_frame_no, g_width, layer_h, 0, 0, origin, g_height, g_scroll, g_scroll_seq, g_content_height };
    if (!gpu_client_send(&g_shifted, &hdr)) {
        fprintf(stderr, "[tab %u] the GPU process isn't answering\n", g_tab);
        g_need_paint = false;
        return;
    }
    TuFrame fr = { g_frame_no, 0, g_width, g_height, g_scroll, g_content_height };
    chan_send(&g_chan, TU_FRAME, &fr, sizeof(fr), 1000);
    g_in_flight = true;
    g_need_paint = false;
}

static void send_title(void) {
    const char *s = page_title();
    if (!s || strncmp(s, g_title, sizeof(g_title) - 1) == 0) return;
    snprintf(g_title, sizeof(g_title), "%s", s);
    chan_send(&g_chan, TU_TITLE, g_title, (uint32_t)strlen(g_title), 1000);
}

/* While the page loads (page_set_load_progress): every 100 ms, what is
 * parsed so far goes to the screen, as browsers paint a page before it has
 * all arrived.  Only the GPU process's "drawn" is taken meanwhile; the UI's
 * input waits for the load (no page script runs from here mid-parse). */
static bool g_loading;

/* While the page loads (waiting on the network, never in a script), a frame
 * of what is there so far, as a browser's rendering opportunities give one:
 * not before the <body> has something to show (browsers hold the first
 * paint), only when the DOM changed since the last one, and spaced so that
 * these frames take at most about a quarter of the load (the next waits
 * three times as long as the last took, 100 ms at least). */
static void load_progress(void) {
    static ULONGLONG last, gap = 100;
    static uint64_t painted;            /* the DOM version of the last frame */
    static bool busy, any;              /* (its layout may itself wait on the network) */
    ULONGLONG now = GetTickCount64();
    if (!g_loading || busy || g_hidden || now - last < gap) return;
    if (!page_first_paint_ready()) return;
    uint64_t version = page_dom_version();
    if (any && version == painted) return;
    busy = true;
    uint32_t done;
    int g;
    while ((g = gpu_client_poll(&done)) > 0)
        if (done == g_frame_no) g_in_flight = false;
    if (g < 0) g_in_flight = false;
    if (!g_in_flight) {
        page_request_layout();
        PageTurn t;
        page_turn(false, false, (int)g_width, (int)g_height, &t);
        if (t.new_frame) {
            g_content_height = frame_extent(&page_frame()->dl);
            clamp_scroll();
            paint();
            send_title();
        }
        ULONGLONG end = GetTickCount64();
        gap = 3 * (end - now) > 100 ? 3 * (end - now) : 100;
        last = end;
        painted = version;
        any = true;
    }
    busy = false;
}

/* ---- input ------------------------------------------------------------- */

/* Dispatch a click at the element, as a browser does; if nothing cancels
 * it and it is on a link, return "nav:<url>" (or "new:<url>" for
 * target=_blank). */
static const char CLICK_JS[] =
    "globalThis.__cyber_tab_click = function(el, x, y) {"
    "  if (!el) return '';"
    "  var ev;"
    "  try { ev = new MouseEvent('click', {bubbles: true, cancelable: true, clientX: x, clientY: y, button: 0}); }"
    "  catch (e) { ev = new Event('click', {bubbles: true, cancelable: true}); }"
    "  var go = true;"
    "  try { go = el.dispatchEvent(ev); } catch (e) {}"
    "  if (!go || ev.defaultPrevented) return '';"
    "  var a = el.closest ? el.closest('a[href]') : null;"
    "  if (!a) return '';"
    "  var h = a.getAttribute('href');"
    "  if (!h || h.charAt(0) === '#' || /^\\s*javascript:/i.test(h)) return '';"
    "  var u; try { u = new URL(h, document.baseURI).href; } catch (e) { return ''; }"
    "  return (a.getAttribute('target') === '_blank' ? 'new:' : 'nav:') + u;"
    "};";

static void click(int32_t x, int32_t y) {
    JSContextHandle ctx = page_engine_context();
    GCValue el = page_element_at((double)x, (double)y + g_scroll);
    GCValue fn = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cyber_tab_click");
    GCValue args[3] = { el, JS_NewInt32(ctx, x), JS_NewInt32(ctx, y) };
    GCValue r = JS_Call(ctx, fn, JS_UNDEFINED, 3, args);
    if (JS_IsException(r)) {
        JS_GetException(ctx);
        return;
    }
    const char *s = JS_ToCString(ctx, r);
    if (s && (strncmp(s, "nav:", 4) == 0 || strncmp(s, "new:", 4) == 0) && strlen(s + 4) <= PROTO_URL_MAX) {
        static uint8_t msg[sizeof(TuNavigate) + PROTO_URL_MAX];
        TuNavigate nav = { s[0] == 'n' && s[1] == 'e' };
        memcpy(msg, &nav, sizeof(nav));
        size_t n = strlen(s + 4);
        memcpy(msg + sizeof(nav), s + 4, n);
        chan_send(&g_chan, TU_NAVIGATE, msg, (uint32_t)(sizeof(nav) + n), 1000);
    }

}

static void handle(const ChanMsg *m) {
    switch (m->type) {
    case UT_RESIZE: {
        if (m->len != sizeof(UtSize)) return;
        UtSize sz;
        memcpy(&sz, m->data, sizeof(sz));
        if (sz.width == 0 || sz.height == 0 || sz.width > SURFACE_MAX_W || sz.height > SURFACE_MAX_H) return;
        if (sz.width != g_width || sz.height != g_height) {
            g_width = sz.width;
            g_height = sz.height;
            page_set_viewport((int)g_width, (int)g_height);
            page_request_layout();
        }
        return;
    }
    case UT_SCROLL: {
        if (m->len != sizeof(UtScroll)) return;
        UtScroll sc;
        memcpy(&sc, m->data, sizeof(sc));
        int64_t y = (int64_t)g_scroll + sc.dy;
        g_scroll = y < 0 ? 0 : (uint32_t)(y > 100000000 ? 100000000 : y);
        clamp_scroll();
        g_scroll_seq = sc.seq;
        g_need_paint = true;
        return;
    }
    case UT_VISIBLE: {
        if (m->len != sizeof(UtVisible)) return;
        UtVisible v;
        memcpy(&v, m->data, sizeof(v));
        bool hidden = !v.visible;
        if (hidden != g_hidden) {
            g_hidden = hidden;
            page_set_rendering(!hidden);
            if (!hidden) { page_request_layout(); g_need_paint = true; }
        }
        return;
    }
    case UT_NAVIGATE:
        navigate_request(m->data, m->len);
        return;
    case UT_CLICK: {
        if (m->len != sizeof(UtPoint)) return;
        UtPoint p;
        memcpy(&p, m->data, sizeof(p));
        if (p.x < 0 || p.y < 0 || p.x >= (int32_t)g_width || p.y >= (int32_t)g_height) return;
        click(p.x, p.y);
        return;
    }
    }
}

/* From the coordinator: a new GPU process (CT_GPU), or the answer to a
 * request for the web-GPU process (CT_WEBGPU, CT_WEBGPU_FAILED). */
static int g_webgpu_answer;             /* 0 waiting, 1 granted, -1 refused or ended */
static Chan g_webgpu;                   /* side 0: the tab */

static void ctl_message(const ChanMsg *m) {
    if ((m->type == CT_GPU || m->type == CT_WEBGPU) && m->len == sizeof(CtGpu)) {
        CtGpu g;
        memcpy(&g, m->data, sizeof(g));
        HANDLE s = (HANDLE)(uintptr_t)g.chan_section, e0 = (HANDLE)(uintptr_t)g.chan_ev0,
               e1 = (HANDLE)(uintptr_t)g.chan_ev1;
        if (m->type == CT_WEBGPU) {
            if (g_webgpu_answer == 0) g_webgpu_answer = chan_attach(&g_webgpu, s, e0, e1, 0) ? 1 : -1;
            return;
        }
        /* A new GPU process: everything goes again, and a frame now. */
        if (gpu_client_attach(s, e0, e1)) {
            fprintf(stderr, "[tab %u] drawing with a new GPU process\n", g_tab);
            g_in_flight = false;
            g_need_paint = true;
        }
    } else if (m->type == CT_WEBGPU_FAILED) {
        if (g_webgpu_answer > 0) fprintf(stderr, "[tab %u] the web-GPU process ended\n", g_tab);
        g_webgpu_answer = -1;
    }
}

/* ---- the site's web-GPU process (browser/webgpu_bridge.h) ----------------------- */

/* Asked of the coordinator (through the UI) once; its answer waited for. */
static bool webgpu_connect(void) {
    if (!g_have_ctl || !chan_send(&g_chan, TU_WANT_WEBGPU, NULL, 0, 1000)) return false;
    static ChanMsg m;
    unsigned long long until = GetTickCount64() + 15000;
    while (g_webgpu_answer == 0 && GetTickCount64() < until) {
        int r = chan_recv(&g_ctl, &m);
        if (r < 0) return false;
        if (r > 0) { ctl_message(&m); continue; }
        chan_wait(&g_ctl, 100);
    }
    fprintf(stderr, "[tab %u] web-GPU process: %s\n", g_tab, g_webgpu_answer > 0 ? "granted" : "none");
    return g_webgpu_answer > 0;
}

static bool webgpu_send(uint16_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    return big_send(&g_webgpu, type, a, alen, b, blen, NULL, 0, 10000);
}

/* Shaders compile and work finishes while the page waits: allow it 30 s,
 * unless the coordinator says the process ended. */
static bool webgpu_recv(uint16_t *type, const uint8_t **body, uint32_t *len) {
    static BigRx rx;
    static ChanMsg m;
    unsigned long long until = GetTickCount64() + 30000;
    for (;;) {
        while (g_have_ctl && chan_recv(&g_ctl, &m) > 0) ctl_message(&m);
        if (g_webgpu_answer < 0) return false;
        int r = big_recv(&g_webgpu, &rx);
        if (r < 0) return false;
        if (r > 0) {
            *type = rx.type;
            *body = rx.buf;
            *len = rx.total;
            return true;
        }
        if (GetTickCount64() >= until) return false;
        chan_wait(&g_webgpu, 100);
    }
}

static const WebgpuBridge g_webgpu_bridge = { webgpu_connect, webgpu_send, webgpu_recv };

static bool drain(void) {
    static ChanMsg m;
    bool any = false;
    int r;
    while ((r = chan_recv(&g_chan, &m)) > 0) {
        handle(&m);
        any = true;
    }
    if (r < 0) ExitProcess(2);          /* the UI broke the channel */
    while (g_have_ctl && (r = chan_recv(&g_ctl, &m)) > 0) {
        ctl_message(&m);
        any = true;
    }
    if (webgpu_canvas_dirty_take()) {
        g_need_paint = true;
        any = true;
    }
    uint32_t done;
    int g;
    while ((g = gpu_client_poll(&done)) > 0) {
        if (done == g_frame_no) g_in_flight = false;
        any = true;
    }
    if (g < 0) g_in_flight = false;     /* no GPU process: nothing to wait for */
    return any;
}

/* Sleep until the UI or the GPU process sends something, at most ms. */
static void wait_input(DWORD ms) {
    bool ready = !chan_arm(&g_chan) | !gpu_client_arm();
    if (g_have_ctl && !chan_arm(&g_ctl)) ready = true;
    HANDLE w[3] = { chan_event(&g_chan), gpu_client_event(), g_have_ctl ? chan_event(&g_ctl) : NULL };
    if (!ready) WaitForMultipleObjects(g_have_ctl ? 3 : 2, w, FALSE, ms);
    chan_disarm(&g_chan);
    gpu_client_disarm();
    if (g_have_ctl) chan_disarm(&g_ctl);
}

/* ---- the network process ------------------------------------------------- */

/* Requests come from several threads (the JS thread, script and image
 * fetchers).  Each sends its request whole under g_net_send, then waits for
 * its response, which the receiver thread files by id. */
typedef struct Pending {
    uint32_t id;
    bool done;
    uint8_t *msg;                       /* the NT_RESPONSE, whole */
    uint32_t len;
    struct Pending *next;
    struct Part *parts, **parts_end;    /* NT_PARTs not yet observed */
} Pending;

/* A piece of a document's body (NT_PART), for its request's observer. */
typedef struct Part {
    struct Part *next;
    char url[PROTO_URL_MAX + 1];        /* where it came from, after redirects */
    char type[128];                     /* its content type */
    uint32_t len;
    char data[];
} Part;

static DWORD g_main_thread;
static unsigned long long g_net_idle_ms;    /* the main thread waiting on the network process */
static unsigned long long g_start_tick;     /* when the process started (load timing in the log) */
static void load_progress(void);

static Chan g_net;                      /* side 0: the tab */
static CRITICAL_SECTION g_net_send, g_net_lock;
static CONDITION_VARIABLE g_net_cv;
static Pending *g_pending;
static volatile LONG g_net_id;
static volatile bool g_net_broken;
static char g_elsewhere[PROTO_URL_MAX + 1];     /* our document went to another site: there */

static DWORD WINAPI net_receiver(void *arg) {
    (void)arg;
    BigRx rx = { 0 };
    for (;;) {
        int r = big_recv(&g_net, &rx);
        if (r < 0) break;
        if (r == 0) { chan_wait(&g_net, 1000); continue; }
        if (rx.type == NT_PART && rx.total > 12) {
            uint32_t id, ulen, tlen;
            memcpy(&id, rx.buf, 4);
            memcpy(&ulen, rx.buf + 4, 4);
            if (ulen > PROTO_URL_MAX || ulen + 12 > rx.total) continue;
            memcpy(&tlen, rx.buf + 8 + ulen, 4);
            if (tlen >= sizeof(((Part *)0)->type) || 12 + ulen + tlen > rx.total) continue;
            uint32_t head = 12 + ulen + tlen;
            Part *part = (Part *)malloc(sizeof(Part) + rx.total - head);
            if (!part) continue;
            part->next = NULL;
            memcpy(part->url, rx.buf + 8, ulen);
            part->url[ulen] = 0;
            memcpy(part->type, rx.buf + 12 + ulen, tlen);
            part->type[tlen] = 0;
            part->len = rx.total - head;
            memcpy(part->data, rx.buf + head, part->len);
            EnterCriticalSection(&g_net_lock);
            Pending *p = g_pending;
            while (p && (p->id != id || p->done)) p = p->next;
            if (p) {
                *p->parts_end = part;
                p->parts_end = &part->next;
                WakeAllConditionVariable(&g_net_cv);
            }
            LeaveCriticalSection(&g_net_lock);
            if (!p) free(part);
            continue;
        }
        if (rx.type != NT_RESPONSE || rx.total < sizeof(NetResponse)) continue;
        uint32_t id;
        memcpy(&id, rx.buf, 4);
        EnterCriticalSection(&g_net_lock);
        for (Pending *p = g_pending; p; p = p->next) {
            if (p->id != id || p->done) continue;
            p->msg = rx.buf;            /* taken over */
            p->len = rx.total;
            p->done = true;
            rx.buf = NULL;
            rx.cap = 0;
            break;
        }
        WakeAllConditionVariable(&g_net_cv);
        LeaveCriticalSection(&g_net_lock);
    }
    fprintf(stderr, "[tab %u] the network channel broke\n", g_tab);
    EnterCriticalSection(&g_net_lock);
    g_net_broken = true;
    WakeAllConditionVariable(&g_net_cv);
    LeaveCriticalSection(&g_net_lock);
    return 0;
}

/* Send a message that expects a response, and wait for it (malloc'd). */
static uint8_t *net_call(uint16_t type, uint32_t id, const void *a, uint32_t alen, const void *b, uint32_t blen,
                         const void *c, uint32_t clen, uint32_t *len) {
    Pending p = { id, false, NULL, 0, NULL, NULL, NULL };
    p.parts_end = &p.parts;
    EnterCriticalSection(&g_net_lock);
    p.next = g_pending;
    g_pending = &p;
    LeaveCriticalSection(&g_net_lock);
    EnterCriticalSection(&g_net_send);
    bool sent = !g_net_broken && big_send(&g_net, type, a, alen, b, blen, c, clen, 30000);
    LeaveCriticalSection(&g_net_send);
    /* While it waits: the parts of a document go to this thread's observer
     * (the parser) as they come, and on the main thread during the load,
     * what is there is painted (load_progress), even while a script holds
     * the parser. */
    bool main = GetCurrentThreadId() == g_main_thread;
    EnterCriticalSection(&g_net_lock);
    while (sent && !g_net_broken) {
        Part *parts = p.parts;
        p.parts = NULL;
        p.parts_end = &p.parts;
        if (parts) {
            LeaveCriticalSection(&g_net_lock);
            for (Part *q = parts, *n; q; q = n) {
                n = q->next;
                /* (The document's URL: page_observe takes it from here.) */
                if (q->url[0]) http_set_last_url(q->url);
                http_set_last_content_type(q->type);
                http_body_observe(q->data, q->len);
                free(q);
            }
            EnterCriticalSection(&g_net_lock);
            continue;
        }
        if (p.done) break;
        if (main) {
            LeaveCriticalSection(&g_net_lock);
            load_progress();
            EnterCriticalSection(&g_net_lock);
            if (p.done || p.parts) continue;
        }
        ULONGLONG slept = main ? GetTickCount64() : 0;
        SleepConditionVariableCS(&g_net_cv, &g_net_lock, main ? 100 : INFINITE);
        if (main) g_net_idle_ms += GetTickCount64() - slept;
    }
    for (Pending **pp = &g_pending; *pp; pp = &(*pp)->next)
        if (*pp == &p) { *pp = p.next; break; }
    for (Part *q = p.parts, *n; q; q = n) {
        n = q->next;
        free(q);
    }
    LeaveCriticalSection(&g_net_lock);
    *len = p.len;
    return p.msg;
}

/* CYBER_NET_LOG=1: each request's start (from the tab's start), time and size. */
static bool net_log(void) {
    static int on = -1;
    if (on < 0) on = getenv("CYBER_NET_LOG") ? 1 : 0;
    return on;
}

static bool net_request(const char *url, const char *method, const char *body, size_t body_len,
                        const char **headers, size_t nheaders, HttpBuffer *out, int *status,
                        char *err, size_t err_len) {
    memset(out, 0, sizeof(*out));
    if (body_len > BIG_MAX / 2) { snprintf(err, err_len, "request body too large"); return false; }
    /* method, url, headers: NUL-terminated, after the fixed part */
    size_t hlen = 0;
    for (size_t i = 0; i < nheaders; i++) hlen += strlen(headers[i]) + 1;
    size_t mlen = strlen(method) + 1, ulen = strlen(url) + 1;
    HttpRequestContext rc;
    http_request_context_get(&rc);
    size_t ilen = strlen(rc.initiator) + 1;
    char *strs = (char *)malloc(mlen + ulen + hlen + ilen);
    if (!strs) return false;
    memcpy(strs, method, mlen);
    memcpy(strs + mlen, url, ulen);
    size_t o = mlen + ulen;
    for (size_t i = 0; i < nheaders; i++) {
        size_t n = strlen(headers[i]) + 1;
        memcpy(strs + o, headers[i], n);
        o += n;
    }
    memcpy(strs + o, rc.initiator, ilen);
    o += ilen;
    NetRequest q = { (uint32_t)InterlockedIncrement(&g_net_id), (uint32_t)mlen, (uint32_t)ulen, (uint32_t)hlen,
                     (uint32_t)nheaders, (uint32_t)body_len, (uint32_t)rc.mode, (uint32_t)ilen };
    uint32_t len = 0;
    ULONGLONG t0 = GetTickCount64();
    uint8_t *m = net_call(TN_REQUEST, q.id, &q, sizeof(q), strs, (uint32_t)o, body, (uint32_t)body_len, &len);
    free(strs);
    if (net_log())
        fprintf(stderr, "[net] %llu ms +%llu ms %s %u bytes %s %.120s\n", t0 - g_start_tick, GetTickCount64() - t0,
                GetCurrentThreadId() == g_main_thread ? "main" : "pool", len, method, url);
    NetResponse r;
    if (!m || len < sizeof(r)) {
        free(m);
        snprintf(err, err_len, "no response from the network process");
        return false;
    }
    memcpy(&r, m, sizeof(r));
    if ((uint64_t)sizeof(r) + r.url_len + r.headers_len + r.error_len + r.body_len != len) {
        free(m);
        snprintf(err, err_len, "malformed response from the network process");
        return false;
    }
    const char *final_url = (const char *)m + sizeof(r);
    const uint8_t *p = m + sizeof(r) + r.url_len;
    if (status) *status = r.status;
    if (r.ok == NET_ELSEWHERE && rc.mode == HTTP_MODE_NAVIGATE && r.url_len && r.url_len <= PROTO_URL_MAX) {
        /* Our document is another site's (a redirect went there): the UI
         * navigates there, in a tab of that site (main). */
        snprintf(g_elsewhere, sizeof(g_elsewhere), "%.*s", (int)r.url_len, final_url);
        snprintf(err, err_len, "another site's document: %s", g_elsewhere);
        free(m);
        return false;
    }
    if (r.ok != NET_OK) {
        snprintf(err, err_len, "%.*s", (int)r.error_len, (const char *)p + r.headers_len);
        free(m);
        return false;
    }
    /* Where it ended, after redirects: the document's URL (http_last_url). */
    if (r.url_len) {
        char *u = (char *)malloc((size_t)r.url_len + 1);
        if (u) {
            memcpy(u, final_url, r.url_len);
            u[r.url_len] = 0;
            http_set_last_url(u);
            free(u);
        }
    }
    out->headers = (char *)malloc((size_t)r.headers_len + 1);
    out->data = (char *)malloc((size_t)r.body_len + 1);
    if (!out->headers || !out->data) {
        free(out->headers); free(out->data); free(m);
        memset(out, 0, sizeof(*out));
        return false;
    }
    memcpy(out->headers, p, r.headers_len);
    out->headers[r.headers_len] = 0;
    out->headers_size = r.headers_len;
    memcpy(out->data, p + r.headers_len + r.error_len, r.body_len);
    out->data[r.body_len] = 0;
    out->size = r.body_len;
    free(m);
    return true;
}

static char *net_cookie_get(const char *url) {
    NetCookieGet g = { (uint32_t)InterlockedIncrement(&g_net_id) };
    uint32_t len = 0;
    uint8_t *m = net_call(TN_COOKIE_GET, g.id, &g, sizeof(g), url, (uint32_t)strlen(url), NULL, 0, &len);
    NetResponse r;
    char *s = NULL;
    if (m && len >= sizeof(r)) {
        memcpy(&r, m, sizeof(r));
        if (r.ok == NET_OK && (uint64_t)sizeof(r) + r.url_len + r.headers_len + r.error_len + r.body_len == len) {
            s = (char *)malloc((size_t)r.body_len + 1);
            if (s) {
                memcpy(s, m + sizeof(r) + r.url_len + r.headers_len + r.error_len, r.body_len);
                s[r.body_len] = 0;
            }
        }
    }
    free(m);
    return s;
}

static void net_cookie_set(const char *url, const char *line) {
    EnterCriticalSection(&g_net_send);
    if (!g_net_broken)
        big_send(&g_net, TN_COOKIE_SET, url, (uint32_t)strlen(url) + 1, line, (uint32_t)strlen(line), NULL, 0, 30000);
    LeaveCriticalSection(&g_net_send);
}

static const HttpRemote g_remote = { net_request, net_cookie_get, net_cookie_set };

/* ---- the storage service (the JS thread only) ---------------------------------- */

static Chan g_store;                    /* side 0: the tab */
static bool g_have_store;
static BigRx g_store_rx;
static uint32_t g_store_id;
static char g_site[SITE_MAX];            /* the page's site: its frames from others are partitioned */

/* UT_NAVIGATE: a page of this process's site, loaded here next; another
 * site's is refused (this process is its site's alone: SANDBOX_DESIGN.md). */
static void navigate_request(const uint8_t *url, uint32_t len) {
    char scheme[8], host[SITE_HOST_MAX + 1], site[SITE_MAX];
    if (len == 0 || len > PROTO_URL_MAX || !g_site[0] ||
        !site_url_host(url, len, scheme, sizeof(scheme), host, sizeof(host)) || !site_of(scheme, host, site) ||
        strcmp(site, g_site) != 0) {
        fprintf(stderr, "[tab %u] not loading another site's page here\n", g_tab);
        return;
    }
    memcpy(g_next_url, url, len);
    g_next_url[len] = 0;
}

static bool store_call(uint16_t type, const char *path, const void *data, uint32_t dlen,
                       char **out, size_t *out_len) {
    if (!g_have_store) return false;
    /* The service would refuse it (its quota), and a message past the quota
     * is past its channel's limit: it would drop the channel instead. */
    if (dlen > STORE_QUOTA_BYTES) return false;
    StoreRequest r = { ++g_store_id, 0, (uint32_t)strlen(path), dlen };
    if (!big_send(&g_store, type, &r, sizeof(r), path, r.path_len, data, dlen, 5000)) return false;
    for (;;) {
        int got = big_recv(&g_store, &g_store_rx);
        if (got < 0) { g_have_store = false; return false; }
        if (got == 0) { if (!chan_wait(&g_store, 10000)) return false; continue; }
        StoreResult res;
        if (g_store_rx.type != ST_RESULT || g_store_rx.total < sizeof(res)) continue;
        memcpy(&res, g_store_rx.buf, sizeof(res));
        if (res.id != r.id) continue;
        if (res.data_len != g_store_rx.total - sizeof(res)) return false;
        if (out && res.ok) {
            *out = (char *)malloc((size_t)res.data_len + 1);
            if (!*out) return false;
            memcpy(*out, g_store_rx.buf + sizeof(res), res.data_len);
            *out_len = res.data_len;
        }
        return res.ok != 0;
    }
}

/* A name as a path component: itself if it is one ([a-z0-9_.-], at most
 * 64), else h<a 64-bit hash>. */
static void component(const char *name, char *out, size_t cap) {
    bool ok = strlen(name) <= 64 && name[0] && name[0] != '.';
    for (const char *p = name; ok && *p; p++)
        ok = (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-';
    if (ok) {
        snprintf(out, cap, "%s", name);
        return;
    }
    uint64_t h = 1469598103934665603ull;
    for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
    snprintf(out, cap, "h%016llx", (unsigned long long)h);
}

/* An origin's localStorage file: local_storage/<scheme>_<host>_<port>,
 * under partitions/<its site> when its site isn't the page's (a frame from
 * another site keeps its storage under the page's site: section 9). */
static bool local_storage_path(const char *origin, char *out, size_t cap) {
    char scheme[8], host[SITE_HOST_MAX + 1], site[SITE_MAX];
    if (!site_url_host((const uint8_t *)origin, (uint32_t)strlen(origin), scheme, sizeof(scheme), host,
                       sizeof(host)) || !site_of(scheme, host, site))
        return false;
    int port = !strcmp(scheme, "https") ? 443 : 80;
    const char *a = strstr(origin, "://") + 3, *end = a + strcspn(a, "/?#");
    for (const char *p = end; p > a; p--)
        if (p[-1] == ':') { port = atoi(p); break; } else if (p[-1] < '0' || p[-1] > '9') break;
    char name[SITE_HOST_MAX + 32], file[80], part[80];
    snprintf(name, sizeof(name), "%s_%s_%d", scheme, host, port);
    component(name, file, sizeof(file));
    if (!strcmp(site, g_site)) {
        snprintf(out, cap, "local_storage/%s", file);
        return true;
    }
    for (char *p = site; *p; p++) if (*p == '+') *p = '_';          /* not in a path component */
    component(site, part, sizeof(part));
    snprintf(out, cap, "partitions/%s/local_storage/%s", part, file);
    return true;
}

static bool local_storage_load(const char *origin, char **data, size_t *len) {
    char path[256];
    return local_storage_path(origin, path, sizeof(path)) && store_call(TS_READ, path, NULL, 0, data, len);
}

static const StorageBackend g_storage = { local_storage_load };

/* The bytecode cache (code_cache.h) in the site's folder: cache/code/<key>,
 * which the storage service keeps within the site's cache budget. */
static bool code_cache_load(const char *key, uint8_t **data, size_t *len) {
    char path[64];
    snprintf(path, sizeof(path), "cache/code/%s", key);
    char *d = NULL;
    if (!store_call(TS_READ, path, NULL, 0, &d, len)) return false;
    *data = (uint8_t *)d;
    return true;
}

static void code_cache_save(const char *key, const uint8_t *data, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "cache/code/%s", key);
    if (len <= 0xffffffffu) store_call(TS_WRITE, path, data, (uint32_t)len, NULL, NULL);
}

static const CodeCacheStore g_code_cache = { code_cache_load, code_cache_save };

static void save_local_storage(void) {
    static ULONGLONG last;
    if (!g_have_store || GetTickCount64() - last < 500) return;
    last = GetTickCount64();
    char *origin, *data, path[256];
    size_t len;
    while (storage_local_take_changes(&origin, &data, &len)) {
        if (local_storage_path(origin, path, sizeof(path)) &&
            !store_call(TS_WRITE, path, data, (uint32_t)len, NULL, NULL))
            fprintf(stderr, "[tab %u] could not save localStorage of %s\n", g_tab, origin);
        free(origin);
        free(data);
    }
}

/* ---- main --------------------------------------------------------------- */

static char *unhex(const char *s) {
    size_t n = strlen(s) / 2;
    char *out = (char *)malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(s + 2 * i, "%2x", &v);
        out[i] = (char)v;
    }
    out[n] = 0;
    return out;
}

/* The CPU time the main thread and the whole process have used, in ms. */
static void cpu_ms(double *thread_ms, double *process_ms) {
    FILETIME c, e, k, u;
    *thread_ms = *process_ms = 0;
    if (GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u))
        *thread_ms = (double)((((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                              (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e4;
    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        *process_ms = (double)((((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                               (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e4;
}

int main(int argc, char **argv) {
    g_start_tick = GetTickCount64();
    det_clock_set_js_thread();
    HANDLE sec = NULL, e0 = NULL, e1 = NULL;
    HANDLE net[3] = { 0 }, store[3] = { 0 }, gpu[3] = { 0 }, ctl[3] = { 0 };
    const char *url_hex = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chan") && i + 3 < argc) {
            sec = spawn_handle_from_arg(argv[++i]);
            e0 = spawn_handle_from_arg(argv[++i]);
            e1 = spawn_handle_from_arg(argv[++i]);
        } else if ((!strcmp(argv[i], "--net") || !strcmp(argv[i], "--store") || !strcmp(argv[i], "--gpu") ||
                    !strcmp(argv[i], "--ctl")) && i + 3 < argc) {
            HANDLE *h = argv[i][2] == 'n' ? net : argv[i][2] == 's' ? store : argv[i][2] == 'g' ? gpu : ctl;
            for (int k = 0; k < 3; k++) h[k] = spawn_handle_from_arg(argv[++i]);
        } else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            g_width = (uint32_t)atoi(argv[++i]);
            g_height = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--tab") && i + 1 < argc) {
            g_tab = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--url") && i + 1 < argc) {
            url_hex = argv[++i];
        }
    }
    if (!sec || !net[0] || !gpu[0] || !url_hex || g_width == 0 || g_height == 0 ||
        g_width > SURFACE_MAX_W || g_height > SURFACE_MAX_H) {
        fprintf(stderr, "cb_tab: bad arguments\n");
        return 1;
    }
    if (!chan_attach(&g_chan, sec, e0, e1, 1)) { fprintf(stderr, "cb_tab: no channel\n"); return 1; }
    g_have_ctl = ctl[0] && chan_attach(&g_ctl, ctl[0], ctl[1], ctl[2], 1);
    if (!gpu_client_attach(gpu[0], gpu[1], gpu[2])) { fprintf(stderr, "cb_tab: no GPU channel\n"); return 1; }
    if (!chan_attach(&g_net, net[0], net[1], net[2], 0)) { fprintf(stderr, "cb_tab: no network channel\n"); return 1; }
    g_have_store = store[0] && chan_attach(&g_store, store[0], store[1], store[2], 0);
    InitializeCriticalSection(&g_net_send);
    InitializeCriticalSection(&g_net_lock);
    InitializeConditionVariable(&g_net_cv);
    CloseHandle(CreateThread(NULL, 0, net_receiver, NULL, 0, NULL));
    /* No network or files of our own. */
    http_set_remote(&g_remote);
    image_cache_allow_local_files(false);
    const char *url = unhex(url_hex);
    {
        char scheme[8], host[SITE_HOST_MAX + 1];
        if (!site_url_host((const uint8_t *)url, (uint32_t)strlen(url), scheme, sizeof(scheme), host, sizeof(host)) ||
            !site_of(scheme, host, g_site))
            g_site[0] = 0;
    }
    if (g_have_store && g_site[0]) storage_set_backend(&g_storage);
    if (g_have_store && g_site[0]) code_cache_set_store(&g_code_cache);
    if (g_have_ctl && g_site[0]) webgpu_bridge_set(&g_webgpu_bridge);
    fprintf(stderr, "[tab %u] %s (%ux%u)\n", g_tab, url, g_width, g_height);
    memory_report("at start");

    send_state(TAB_LOADING);
    /* No platform_init: it only starts Winsock, which the sandbox refuses. */
    if (!page_engine_init(NULL)) {
        send_state(TAB_LOAD_FAILED);
        return 1;
    }
    page_engine_load_fonts();
    /* (images arriving later lay out again; CYBER_FRAME_IMAGE_WAIT=1, for
     * reftests: a frame waits for its images, as cb_harness's do, so the one
     * a page says it is ready after has them) */
    page_set_frame_image_wait(getenv("CYBER_FRAME_IMAGE_WAIT") != NULL);
    memory_report("engine ready");
    bool reused = false;
load:
    if (reused) fprintf(stderr, "[tab %u] %s, in the same process\n", g_tab, url);
    page_prepare(url);
    page_set_viewport((int)g_width, (int)g_height);
    JSContextHandle ctx = page_engine_context();
    JS_Eval(ctx, CLICK_JS, sizeof(CLICK_JS) - 1, "<tab-click>", JS_EVAL_TYPE_GLOBAL);
    bool blank = strcmp(url, "about:blank") == 0;
    g_main_thread = GetCurrentThreadId();
    page_set_load_progress(load_progress);
    g_loading = true;
    bool loaded = blank || page_load();
    g_loading = false;
    page_set_load_progress(NULL);
    if (blank) {
        page_request_layout();
    } else if (!loaded && g_elsewhere[0]) {
        /* Another site's document (SANDBOX_DESIGN.md P10): not ours to run.
         * The UI navigates there, which ends this process. */
        fprintf(stderr, "[tab %u] the document is at %s, another site's: navigating there\n", g_tab, g_elsewhere);
        static uint8_t msg[sizeof(TuNavigate) + PROTO_URL_MAX];
        TuNavigate nav = { 0 };
        size_t n = strlen(g_elsewhere);
        memcpy(msg, &nav, sizeof(nav));
        memcpy(msg + sizeof(nav), g_elsewhere, n);
        chan_send(&g_chan, TU_NAVIGATE, msg, (uint32_t)(sizeof(nav) + n), 1000);
        for (;;) { drain(); if (g_next_url[0]) goto next; wait_input(INFINITE); }
    } else if (!loaded) {
        send_state(TAB_LOAD_FAILED);
        snprintf(g_title, sizeof(g_title), "Failed to load");
        chan_send(&g_chan, TU_TITLE, g_title, (uint32_t)strlen(g_title), 1000);
        for (;;) { drain(); if (g_next_url[0]) goto next; wait_input(INFINITE); }
    }
    send_state(TAB_LOADED);

    bool reported = false, settled = false;
    unsigned long long loaded_at = GetTickCount64();
    /* When the page's content last changed (a rebuilt frame; a playing
     * video's new image is not one): logged once it has not for 3 s, as the
     * time the load visibly settled. */
    unsigned long long content_at = loaded_at;
    unsigned rebuilds = 0;
    double content_cpu = 0;
    /* A watch page has loaded when its video starts playing: its first
     * frame shown (logged once). */
    bool video_started = false;
    double cpu_t, cpu_p;
    cpu_ms(&cpu_t, &cpu_p);
    unsigned frames;
    double frames_wall, frames_cpu;
    page_frame_stats(&frames, &frames_wall, &frames_cpu);
    fprintf(stderr, "[tab %u] load event at %llu ms (cpu: main thread %.0f ms, process %.0f ms; "
            "%u frames built, %.0f ms, %.0f ms cpu)\n",
            g_tab, loaded_at - g_start_tick, cpu_t, cpu_p, frames, frames_wall, frames_cpu);
    memory_report("at the load event");
    {
        DomFetchStats fs;
        dom_fetch_stats(&fs);
        fprintf(stderr, "[tab %u] script fetches: %llu prefetched, %llu waited for (%.0f ms), %llu fetched inline; "
                "main thread waited %llu ms on the network\n", g_tab,
                (unsigned long long)fs.queued, (unsigned long long)fs.waits, fs.wait_ms, (unsigned long long)fs.sync_fetches,
                g_net_idle_ms);
    }
    for (;;) {
        bool input = drain();
        if (g_next_url[0]) goto next;
        /* Animation frame callbacks run when a frame can follow them: not
         * while one is with the GPU process (nor while hidden), as a
         * browser's run when its compositor asks for the next frame.  So
         * what a page did before one is in the frame drawn after it. */
        timer_set_raf_paused(g_hidden || g_in_flight);
        PageTurn t;
        page_turn(true, false, (int)g_width, (int)g_height, &t);
        if (t.new_frame) {
            content_at = GetTickCount64();
            cpu_ms(&content_cpu, &cpu_p);
            rebuilds++;
            g_content_height = frame_extent(&page_frame()->dl);
            clamp_scroll();
            g_need_paint = true;
            send_title();
        }
        /* A playing video's new frame: the same display list, its image anew. */
        bool video = !g_hidden && page_refresh_videos();
        if (video) g_need_paint = true;
        if (video && !video_started) {
            cpu_ms(&cpu_t, &cpu_p);
            fprintf(stderr, "[tab %u] video playing at %llu ms (cpu: main thread %.0f ms, process %.0f ms)\n",
                    g_tab, GetTickCount64() - g_start_tick, cpu_t, cpu_p);
            video_started = true;
        }
        if (g_need_paint && !g_in_flight && !g_hidden) paint();
        if (!settled && GetTickCount64() - content_at > 3000) {
            page_frame_stats(&frames, &frames_wall, &frames_cpu);
            fprintf(stderr, "[tab %u] content settled at %llu ms (%u rebuilt frames after the load event; "
                    "%u frames built in all, %.0f ms, %.0f ms cpu; main thread cpu by then %.0f ms)\n",
                    g_tab, content_at - g_start_tick, rebuilds, frames, frames_wall, frames_cpu, content_cpu);
            settled = true;
        }
        if (!reported && GetTickCount64() - loaded_at > 3000) {
            memory_report("3 s after load");
            CodeCacheStats cs;
            code_cache_stats(&cs);
            if (cs.hits || cs.misses)
                fprintf(stderr, "[tab %u] bytecode cache: %d hits, %d misses, %d rejected, %d saved (reading %.0f ms)\n",
                        g_tab, cs.hits, cs.misses, cs.rejected, cs.saved, cs.read_ms + cs.hash_ms);
            reported = true;
        }
        save_local_storage();
        fflush(stdout);
        fflush(stderr);
        if (input || t.had_timers || t.had_images || t.new_frame || page_needs_layout()) continue;
        /* Idle: a script this page compiled goes into the bytecode cache
         * (one a turn: each is a full compile). */
        if (code_cache_flush(page_engine_context(), 1)) continue;
        /* Idle: sleep until input, the next timer, or (with a fetch or an
         * image in flight) a short while. */
        DWORD ms = 50;
        unsigned long long next = timer_next_due_ms(), now = page_time_ms();
        if (next != (unsigned long long)-1) ms = next > now ? (DWORD)(next - now < 50 ? next - now : 50) : 0;
        if (js_quickjs_network_pending() > 0 || image_cache_has_pending(page_image_cache())) ms = ms < 5 ? ms : 5;
        if (page_video_playing()) ms = ms < 8 ? ms : 8;         /* (its next frame) */
        if (ms) wait_input(ms);
    }

next:
    /* The next document (UT_NAVIGATE), in this process: what this one keeps
     * is saved, the engine made ready for another, and it loads. */
    save_local_storage();
    {
        static char next_url[PROTO_URL_MAX + 1];
        memcpy(next_url, g_next_url, sizeof(next_url));
        g_next_url[0] = 0;
        url = next_url;
    }
    if (!page_engine_reset()) {
        send_state(TAB_LOAD_FAILED);
        return 1;
    }
    g_scroll = 0;
    g_content_height = 0;
    g_elsewhere[0] = 0;
    g_title[0] = 0;
    g_start_tick = GetTickCount64();
    reused = true;
    send_state(TAB_LOADING);
    goto load;
}
