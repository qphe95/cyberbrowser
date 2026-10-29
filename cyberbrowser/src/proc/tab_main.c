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

#include "platform.h"
#include "quickjs.h"
#include "js_quickjs.h"
#include "det_clock.h"
#include "page_engine.h"
#include "gpu_client.h"
#include "http_download.h"
#include "image_cache.h"
#include "browser/storage_api.h"

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
static char g_title[PROTO_TITLE_MAX];

static void memory_report(const char *what) {
    PROCESS_MEMORY_COUNTERS_EX pm;
    memset(&pm, 0, sizeof(pm));
    pm.cb = sizeof(pm);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm));
    fprintf(stderr, "[tab %u] memory %s: private %.1f MB, working set %.1f MB\n", g_tab, what,
            pm.PrivateUsage / 1048576.0, pm.WorkingSetSize / 1048576.0);
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
        if (m.type != CT_GPU || m.len != sizeof(CtGpu)) continue;
        CtGpu g;
        memcpy(&g, m.data, sizeof(g));
        /* A new GPU process: everything goes again, and a frame now. */
        if (gpu_client_attach((HANDLE)(uintptr_t)g.chan_section, (HANDLE)(uintptr_t)g.chan_ev0,
                              (HANDLE)(uintptr_t)g.chan_ev1)) {
            fprintf(stderr, "[tab %u] drawing with a new GPU process\n", g_tab);
            g_in_flight = false;
            g_need_paint = true;
        }
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
} Pending;

static Chan g_net;                      /* side 0: the tab */
static CRITICAL_SECTION g_net_send, g_net_lock;
static CONDITION_VARIABLE g_net_cv;
static Pending *g_pending;
static volatile LONG g_net_id;
static volatile bool g_net_broken;

static DWORD WINAPI net_receiver(void *arg) {
    (void)arg;
    BigRx rx = { 0 };
    for (;;) {
        int r = big_recv(&g_net, &rx);
        if (r < 0) break;
        if (r == 0) { chan_wait(&g_net, 1000); continue; }
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
    Pending p = { id, false, NULL, 0, NULL };
    EnterCriticalSection(&g_net_lock);
    p.next = g_pending;
    g_pending = &p;
    LeaveCriticalSection(&g_net_lock);
    EnterCriticalSection(&g_net_send);
    bool sent = !g_net_broken && big_send(&g_net, type, a, alen, b, blen, c, clen, 30000);
    LeaveCriticalSection(&g_net_send);
    EnterCriticalSection(&g_net_lock);
    while (sent && !p.done && !g_net_broken) SleepConditionVariableCS(&g_net_cv, &g_net_lock, INFINITE);
    for (Pending **pp = &g_pending; *pp; pp = &(*pp)->next)
        if (*pp == &p) { *pp = p.next; break; }
    LeaveCriticalSection(&g_net_lock);
    *len = p.len;
    return p.msg;
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
    char *strs = (char *)malloc(mlen + ulen + hlen + 1);
    if (!strs) return false;
    memcpy(strs, method, mlen);
    memcpy(strs + mlen, url, ulen);
    size_t o = mlen + ulen;
    for (size_t i = 0; i < nheaders; i++) {
        size_t n = strlen(headers[i]) + 1;
        memcpy(strs + o, headers[i], n);
        o += n;
    }
    NetRequest q = { (uint32_t)InterlockedIncrement(&g_net_id), (uint32_t)mlen, (uint32_t)ulen, (uint32_t)hlen,
                     (uint32_t)nheaders, (uint32_t)body_len };
    uint32_t len = 0;
    uint8_t *m = net_call(TN_REQUEST, q.id, &q, sizeof(q), strs, (uint32_t)o, body, (uint32_t)body_len, &len);
    free(strs);
    NetResponse r;
    if (!m || len < sizeof(r)) {
        free(m);
        snprintf(err, err_len, "no response from the network process");
        return false;
    }
    memcpy(&r, m, sizeof(r));
    if ((uint64_t)sizeof(r) + r.headers_len + r.error_len + r.body_len != len) {
        free(m);
        snprintf(err, err_len, "malformed response from the network process");
        return false;
    }
    const uint8_t *p = m + sizeof(r);
    if (status) *status = r.status;
    if (!r.ok) {
        snprintf(err, err_len, "%.*s", (int)r.error_len, (const char *)p + r.headers_len);
        free(m);
        return false;
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
        if (r.ok && (uint64_t)sizeof(r) + r.headers_len + r.error_len + r.body_len == len) {
            s = (char *)malloc((size_t)r.body_len + 1);
            if (s) {
                memcpy(s, m + sizeof(r) + r.headers_len + r.error_len, r.body_len);
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
static char g_local_path[80];           /* local_storage/<origin> */

static bool store_call(uint16_t type, const char *path, const void *data, uint32_t dlen,
                       char **out, size_t *out_len) {
    if (!g_have_store) return false;
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

static bool local_storage_load(char **data, size_t *len) {
    return g_local_path[0] && store_call(TS_READ, g_local_path, NULL, 0, data, len);
}

static const StorageBackend g_storage = { local_storage_load };

/* The page origin's localStorage file: local_storage/<scheme>_<host>_<port>,
 * or a hash of that if it is too long for a path component. */
static void local_storage_path(const char *url) {
    char scheme[8], host[SITE_HOST_MAX + 1];
    if (!site_url_host((const uint8_t *)url, (uint32_t)strlen(url), scheme, sizeof(scheme), host, sizeof(host)))
        return;
    int port = !strcmp(scheme, "https") ? 443 : 80;
    const char *a = strstr(url, "://") + 3, *end = a + strcspn(a, "/?#");
    for (const char *p = end; p > a; p--)
        if (p[-1] == ':') { port = atoi(p); break; } else if (p[-1] < '0' || p[-1] > '9') break;
    char origin[SITE_HOST_MAX + 32];
    snprintf(origin, sizeof(origin), "%s_%s_%d", scheme, host, port);
    if (strlen(origin) > 64) {
        uint64_t h = 1469598103934665603ull;
        for (const char *p = origin; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ull;
        snprintf(origin, sizeof(origin), "h%016llx", (unsigned long long)h);
    }
    snprintf(g_local_path, sizeof(g_local_path), "local_storage/%s", origin);
}

static void save_local_storage(void) {
    static ULONGLONG last;
    if (!g_have_store || GetTickCount64() - last < 500) return;
    last = GetTickCount64();
    char *data;
    size_t len;
    if (!storage_local_take_changes(&data, &len)) return;
    if (!store_call(TS_WRITE, g_local_path, data, (uint32_t)len, NULL, NULL))
        fprintf(stderr, "[tab %u] could not save localStorage\n", g_tab);
    free(data);
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

int main(int argc, char **argv) {
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
    if (g_have_store) {
        local_storage_path(url);
        storage_set_backend(&g_storage);
    }
    fprintf(stderr, "[tab %u] %s (%ux%u)\n", g_tab, url, g_width, g_height);
    memory_report("at start");

    send_state(TAB_LOADING);
    /* No platform_init: it only starts Winsock, which the sandbox refuses. */
    if (!page_engine_init(NULL)) {
        send_state(TAB_LOAD_FAILED);
        return 1;
    }
    page_engine_load_fonts();
    memory_report("engine ready");
    page_prepare(url);
    JSContextHandle ctx = page_engine_context();
    JS_Eval(ctx, CLICK_JS, sizeof(CLICK_JS) - 1, "<tab-click>", JS_EVAL_TYPE_GLOBAL);
    if (strcmp(url, "about:blank") == 0) {
        page_request_layout();
    } else if (!page_load()) {
        send_state(TAB_LOAD_FAILED);
        snprintf(g_title, sizeof(g_title), "Failed to load");
        chan_send(&g_chan, TU_TITLE, g_title, (uint32_t)strlen(g_title), 1000);
        for (;;) { drain(); wait_input(INFINITE); }
    }
    send_state(TAB_LOADED);

    bool reported = false;
    unsigned long long loaded_at = GetTickCount64();
    for (;;) {
        bool input = drain();
        PageTurn t;
        page_turn(true, false, (int)g_width, (int)g_height, &t);
        if (t.new_frame) {
            g_content_height = frame_extent(&page_frame()->dl);
            clamp_scroll();
            g_need_paint = true;
            send_title();
        }
        if (g_need_paint && !g_in_flight) paint();
        if (!reported && GetTickCount64() - loaded_at > 3000) {
            memory_report("3 s after load");
            reported = true;
        }
        save_local_storage();
        fflush(stdout);
        fflush(stderr);
        if (input || t.had_timers || t.had_images || t.new_frame || page_needs_layout()) continue;
        /* Idle: sleep until input, the next timer, or (with a fetch or an
         * image in flight) a short while. */
        DWORD ms = 50;
        unsigned long long next = timer_next_due_ms(), now = page_time_ms();
        if (next != (unsigned long long)-1) ms = next > now ? (DWORD)(next - now < 50 ? next - now : 50) : 0;
        if (js_quickjs_network_pending() > 0 || image_cache_has_pending(page_image_cache())) ms = ms < 5 ? ms : 5;
        if (ms) wait_input(ms);
    }
}
