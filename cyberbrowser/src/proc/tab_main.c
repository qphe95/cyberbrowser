/* tab_main.c - cb_tab.exe: a tab process (SANDBOX_DESIGN.md section 2.3).
 *
 * Started by the coordinator with its channel to the UI, its surface, the
 * viewport size and the URL (hex):
 *   cb_tab.exe --chan S E0 E1 --surface H --size W H --tab N --url HEX
 *
 * It loads the page with the page engine, then runs the page's event loop,
 * paints the visible part of each new frame into its surface, and answers the
 * UI's input: resizes, scrolls and clicks.  A click on a link asks the UI to
 * navigate; every navigation gets a new tab process (the engine holds one
 * page per process). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <psapi.h>

#include "chan.h"
#include "proto.h"
#include "spawn.h"

#include "platform.h"
#include "quickjs.h"
#include "js_quickjs.h"
#include "det_clock.h"
#include "page_engine.h"
#include "raster_cpu.h"

unsigned long long timer_next_due_ms(void);
unsigned long long page_time_ms(void);

static Chan g_chan;                     /* side 1: the tab */
static uint8_t *g_surface;
static uint32_t g_tab, g_width, g_height;
static uint32_t g_scroll, g_content_height;
static uint32_t g_frame_no, g_buffer;
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

/* The frame's display list moved up by the scroll offset, without what
 * lies wholly outside the viewport. */
static DisplayList g_shifted;
static int g_shifted_cap, g_shifted_mask_cap;

static void shift_frame(const DisplayList *dl, float dy) {
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
        if (!bracket && (c.y + c.h < 0 || c.y > (float)g_height)) continue;
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

static void paint(void) {
    PageFrame *f = page_frame();
    if (!f->valid) return;
    static uint8_t *rgb;
    static size_t rgb_bytes;
    size_t need = (size_t)g_width * g_height * 3;
    if (rgb_bytes < need) {
        free(rgb);
        rgb = (uint8_t *)malloc(need);
        rgb_bytes = rgb ? need : 0;
        if (!rgb) return;
    }
    clamp_scroll();
    shift_frame(&f->dl, (float)g_scroll);
    raster_display_list(&g_shifted, rgb, (int)g_width, (int)g_height);
    uint8_t *out = g_surface + SURFACE_HEADER + (size_t)g_buffer * SURFACE_BUFFER_BYTES;
    const uint8_t *in = rgb;
    for (size_t i = 0, n = (size_t)g_width * g_height; i < n; i++, in += 3, out += 4) {
        out[0] = in[2];
        out[1] = in[1];
        out[2] = in[0];
        out[3] = 255;
    }
    TuFrame fr = { ++g_frame_no, g_buffer, g_width, g_height, g_scroll, g_content_height };
    chan_send(&g_chan, TU_FRAME, &fr, sizeof(fr), 1000);
    g_buffer ^= 1;
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
    case UT_FRAME_DONE: {
        if (m->len != sizeof(UtFrameDone)) return;
        UtFrameDone d;
        memcpy(&d, m->data, sizeof(d));
        if (d.frame == g_frame_no) g_in_flight = false;
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
    return any;
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
    HANDLE sec = NULL, e0 = NULL, e1 = NULL, surface = NULL;
    const char *url_hex = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chan") && i + 3 < argc) {
            sec = spawn_handle_from_arg(argv[++i]);
            e0 = spawn_handle_from_arg(argv[++i]);
            e1 = spawn_handle_from_arg(argv[++i]);
        } else if (!strcmp(argv[i], "--surface") && i + 1 < argc) {
            surface = spawn_handle_from_arg(argv[++i]);
        } else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            g_width = (uint32_t)atoi(argv[++i]);
            g_height = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--tab") && i + 1 < argc) {
            g_tab = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--url") && i + 1 < argc) {
            url_hex = argv[++i];
        }
    }
    if (!sec || !surface || !url_hex || g_width == 0 || g_height == 0 ||
        g_width > SURFACE_MAX_W || g_height > SURFACE_MAX_H) {
        fprintf(stderr, "cb_tab: bad arguments\n");
        return 1;
    }
    if (!chan_attach(&g_chan, sec, e0, e1, 1)) { fprintf(stderr, "cb_tab: no channel\n"); return 1; }
    g_surface = (uint8_t *)MapViewOfFile(surface, FILE_MAP_WRITE, 0, 0, SURFACE_BYTES);
    if (!g_surface) { fprintf(stderr, "cb_tab: no surface\n"); return 1; }
    const char *url = unhex(url_hex);
    fprintf(stderr, "[tab %u] %s (%ux%u)\n", g_tab, url, g_width, g_height);
    memory_report("at start");

    send_state(TAB_LOADING);
    if (!platform_init() || !platform_http_init() || !page_engine_init(NULL)) {
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
        for (;;) { drain(); chan_wait(&g_chan, INFINITE); }
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
        fflush(stdout);
        fflush(stderr);
        if (input || t.had_timers || t.had_images || t.new_frame || page_needs_layout()) continue;
        /* Idle: sleep until input, the next timer, or (with a fetch or an
         * image in flight) a short while. */
        DWORD ms = 50;
        unsigned long long next = timer_next_due_ms(), now = page_time_ms();
        if (next != (unsigned long long)-1) ms = next > now ? (DWORD)(next - now < 50 ? next - now : 50) : 0;
        if (js_quickjs_network_pending() > 0 || image_cache_has_pending(page_image_cache())) ms = ms < 5 ? ms : 5;
        if (ms) chan_wait(&g_chan, ms);
    }
}
