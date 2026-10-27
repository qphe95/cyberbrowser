/*
 * CyberBrowser - the single-process harness
 *
 * Loads a start page with the page engine (page_engine.h), waits until it
 * settles, saves a screenshot and the final DOM, and prints a summary.  The
 * replay benchmark (tests/jit/replay.sh) and the tests run it; the probe
 * modes (CYBER_PROBE_JS, CYBER_LAYOUT_HTML) run a script or lay out a local
 * file instead.
 */
#include "cyber_env.h"
#include "det_clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

#include "platform.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "browser_api_impl.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_internal.h"
#include "js_quickjs.h"
#include "http_download.h"
#include "url_utils.h"
#include "html_dom.h"
#include "css_parser.h"
#include "css_layout.h"
#include "display_list.h"
#include "font_registry.h"
#include "text_shaper.h"
#include "image_cache.h"
#include "browser/image_api.h"
#include "browser/layout_api.h"
#include "cyber_profile.h"
#include "cyber_sampler.h"
#include "dom_store.h"

#include "session_state.h"
#include "html_media_extract.h"
#include "dom_fetch.h"
#include "url_whatwg.h"

#include "page_engine.h"
#include "raster_cpu.h"

void platform_sleep_ms(unsigned int ms);


/* Default page loaded by the smoke-test executable.  Can be overridden by
 * argv[1] or the CYBER_START_URL environment variable. */
static const char DEFAULT_START_URL[] = "https://en.wikipedia.org/wiki/Main_Page";

#ifdef _WIN32
static LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    fprintf(stderr, "[FATAL] Unhandled exception 0x%08X at %p\n",
            (unsigned int)code, ep->ExceptionRecord->ExceptionAddress);
    if (code == EXCEPTION_ACCESS_VIOLATION) fprintf(stderr, "[FATAL] Access violation\n");
    if (code == EXCEPTION_STACK_OVERFLOW) fprintf(stderr, "[FATAL] Stack overflow\n");
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION) fprintf(stderr, "[FATAL] Illegal instruction\n");
    {
        void *frames[32];
        HMODULE self = GetModuleHandleA(NULL);
        fprintf(stderr, "[FATAL] module base=%p\n", (void *)self);
        USHORT n = RtlCaptureStackBackTrace(0, 32, frames, NULL);
        fprintf(stderr, "[FATAL] backtrace (%d frames):\n", (int)n);
        for (USHORT i = 0; i < n; i++) fprintf(stderr, "  #%2d %p\n", (int)i, frames[i]);
        fflush(stdout);
        fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

/* The page's JS context (page_engine_context). */
static JSContextHandle g_ctx = GC_HANDLE_NULL;

static void save_html(const char *html, size_t html_size) {
    FILE *f = fopen("page_loaded.html", "wb");
    if (!f) return;
    fwrite(html, 1, html_size, f);
    fclose(f);
    printf("Saved fetched HTML to page_loaded.html (%zu bytes)\n", html_size);
}

/* The viewport the page is laid out in, and the screenshot's size. */
#define WIREFRAME_WIDTH  1024
#define WIREFRAME_HEIGHT 2400

/* Paint a frame and save it as a JPEG. */
static void frame_save_jpg(PageFrame *f, const char *out_path) {
    if (!f->valid) return;
    printf("Display list commands: %d\n", f->dl.count);
    printf("Rendering screenshot to %s ...\n", out_path);
    bool raster_ok;
    {
        CP_SCOPE("raster-jpg");
        raster_ok = render_display_list_to_jpg(&f->dl, out_path, WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
    }
    if (raster_ok) printf("Saved screenshot: %s (%dx%d)\n", out_path, WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
    else printf("WARNING: failed to write %s\n", out_path);
}

/* Lay out a document, paint it and save it (CYBER_LAYOUT_HTML). */
static void render_document_to_jpg(HtmlDocument *doc, ImageCache *image_cache, const char *out_path) {
    PageFrame f = {0};
    page_frame_build(&f, doc, image_cache, WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
    frame_save_jpg(&f, out_path);
    page_frame_free(&f);
}

/* CYBER_TIMELINE=1: the milestones of a run, in ms since process creation
 * (what the process spends outside load-page shows too), with the CPU time
 * and the CPU cycles this (the JS) thread used so far.  Cycles have no
 * scheduler-tick granularity: tests/jit/replay.sh compares them.
 *
 * CYBER_SPIN_PCT=<p> (calibration, JS_PERF_DESIGN.md section 4): before
 * the "page settled" mark, spin until the thread has used p% more
 * cycles, so a known slowdown can be checked against the measurement. */
/* CYBER_MEM=2: a JS heap object's count and bytes. */
static void mem_type_cb(GCHandle h, void *d)
{
    unsigned long long *tc = (unsigned long long *)d;
    tc[0]++;
    tc[1] += gc_header_total_size(gc_header_from_handle(h));
}

static void tl_mark(const char *what) {
    static int on = -1;
    /* CYBER_SAMPLE_LOAD=1: profile the page load only, as the replays time
     * it (not the screenshot and dumps after it). */
    if (strcmp(what, "page settled") == 0 && cyber_getenv("CYBER_SAMPLE_LOAD")) cyber_sampler_stop();
    if (on < 0) on = cyber_getenv("CYBER_TIMELINE") ? 1 : 0;
    if (!on) return;
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    const char *spin = cyber_getenv("CYBER_SPIN_PCT");
    if (spin && strcmp(what, "page settled") == 0) {
        ULONG64 until = cycles + (ULONG64)((double)cycles * atof(spin) / 100.0), now_c = cycles;
        volatile unsigned x = 0;
        while (now_c < until) {
            for (int i = 0; i < 10000; i++) x += i;
            QueryThreadCycleTime(GetCurrentThread(), &now_c);
        }
        cycles = now_c;
    }
    FILETIME c, e, k, u, now; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); GetSystemTimeAsFileTime(&now);
    unsigned long long a = ((unsigned long long)c.dwHighDateTime << 32) | c.dwLowDateTime, b = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    FILETIME tc, te, tk, tu; GetThreadTimes(GetCurrentThread(), &tc, &te, &tk, &tu);
    unsigned long long cpu = (((unsigned long long)tk.dwHighDateTime << 32) | tk.dwLowDateTime) +
                             (((unsigned long long)tu.dwHighDateTime << 32) | tu.dwLowDateTime);
    /* Under CYBER_DET_TIME, also the calls made so far: the work the page
     * did, the same at every run that took the same path. */
    char calls[48] = "";
    if (det_clock_on()) snprintf(calls, sizeof(calls), ", %llu calls", (unsigned long long)g_det_calls);
    fprintf(stderr, "[timeline] %-28s %8.1f ms  (thread cpu %8.1f ms, %9.1f Mcycles%s)\n", what, (b - a) / 1e4,
            cpu / 1e4, cycles / 1e6, calls);
    /* CYBER_MEM=1: where the memory is at this mark: the process's private
     * bytes and working set (and their peaks), the JS heap's committed
     * regions and the bytes allocated in them, and the C heaps' committed
     * and allocated bytes. */
    if (cyber_getenv("CYBER_MEM")) {
        PROCESS_MEMORY_COUNTERS_EX pm;
        GCStats gs;
        HANDLE heaps[64];
        unsigned long long hc = 0, ha = 0;
        memset(&pm, 0, sizeof(pm));
        pm.cb = sizeof(pm);
        K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof(pm));
        gc_get_stats(&gs);
        DWORD nh = GetProcessHeaps(64, heaps);
        for (DWORD i = 0; i < nh && i < 64; i++) {
            HEAP_SUMMARY hs;
            memset(&hs, 0, sizeof(hs));
            hs.cb = sizeof(hs);
            if (HeapSummary(heaps[i], 0, &hs)) {
                hc += hs.cbCommitted;
                ha += hs.cbAllocated;
            }
        }
        fprintf(stderr, "[memory] %-30s private %6.1f MB (peak %6.1f), working set %6.1f MB (peak %6.1f); "
                "JS heap %6.1f MB committed, %6.1f MB in regions; C heaps %6.1f MB committed, %6.1f MB allocated (%lu heaps)\n",
                what, pm.PrivateUsage / 1048576.0, pm.PeakPagefileUsage / 1048576.0, pm.WorkingSetSize / 1048576.0,
                pm.PeakWorkingSetSize / 1048576.0, gs.heap_committed / 1048576.0, gs.heap_used / 1048576.0,
                hc / 1048576.0, ha / 1048576.0, (unsigned long)nh);
        /* CYBER_MEM=2 at "page settled": the C heaps' blocks by size class,
         * and the largest ones. */
        if (atoi(cyber_getenv("CYBER_MEM")) >= 2 && (strcmp(what, "page settled") == 0 || strcmp(what, "idle collection done") == 0)) {
            unsigned long long cls_n[40] = { 0 }, cls_b[40] = { 0 }, big[16] = { 0 };
            for (DWORD i = 0; i < nh && i < 64; i++) {
                PROCESS_HEAP_ENTRY e;
                memset(&e, 0, sizeof(e));
                if (!HeapLock(heaps[i])) continue;
                while (HeapWalk(heaps[i], &e)) {
                    if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
                    unsigned long long sz = e.cbData;
                    int c = 0;
                    while (c < 39 && (1ull << (c + 1)) <= sz) c++;
                    cls_n[c]++;
                    cls_b[c] += sz;
                    for (int k = 0; k < 16; k++)
                        if (sz > big[k]) { memmove(big + k + 1, big + k, (15 - k) * sizeof(big[0])); big[k] = sz; break; }
                }
                HeapUnlock(heaps[i]);
            }
            for (int c = 0; c < 40; c++)
                if (cls_n[c])
                    fprintf(stderr, "[memory] blocks %10llu..%-10llu %9llu blocks %8.1f MB\n",
                            1ull << c, (2ull << c) - 1, cls_n[c], cls_b[c] / 1048576.0);
            /* the JS heap's objects by type (JS_GC_OBJ_TYPE_*) */
            for (int t = 0; t < 24; t++) {
                unsigned long long tc[2] = { 0, 0 };
                gc_for_each_object_of_type((JSGCObjectTypeEnum)t, mem_type_cb, tc);
                if (tc[0])
                    fprintf(stderr, "[memory] JS heap type %2d: %9llu objects %8.1f MB\n", t, tc[0], tc[1] / 1048576.0);
            }
            fprintf(stderr, "[memory] largest blocks:");
            for (int k = 0; k < 16 && big[k]; k++) fprintf(stderr, " %.1fMB", big[k] / 1048576.0);
            fprintf(stderr, "\n");
            /* CYBER_MEM_SIZES=lo-hi: the commonest exact sizes in [lo, hi],
             * with the first bytes of a few blocks of each. */
            const char *rs = cyber_getenv("CYBER_MEM_SIZES");
            if (rs) {
                unsigned long long lo = strtoull(rs, NULL, 10), hi = strchr(rs, '-') ? strtoull(strchr(rs, '-') + 1, NULL, 10) : lo;
                enum { NS = 4096 };
                static unsigned long long sz_of[NS], n_of[NS];
                static void *ex_of[NS][3];
                int ns = 0;
                for (DWORD i = 0; i < nh && i < 64; i++) {
                    PROCESS_HEAP_ENTRY e;
                    memset(&e, 0, sizeof(e));
                    if (!HeapLock(heaps[i])) continue;
                    while (HeapWalk(heaps[i], &e)) {
                        if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY) || e.cbData < lo || e.cbData > hi) continue;
                        int k = 0;
                        while (k < ns && sz_of[k] != e.cbData) k++;
                        if (k == ns) { if (ns == NS) continue; sz_of[ns] = e.cbData; n_of[ns] = 0; ns++; }
                        if (n_of[k] < 3) ex_of[k][n_of[k]] = e.lpData;
                        n_of[k]++;
                    }
                    HeapUnlock(heaps[i]);
                }
                for (int r = 0; r < 8; r++) {
                    int best = -1;
                    for (int k = 0; k < ns; k++) if (n_of[k] && (best < 0 || n_of[k] > n_of[best])) best = k;
                    if (best < 0) break;
                    fprintf(stderr, "[memory] size %llu: %llu blocks\n", sz_of[best], n_of[best]);
                    for (int x = 0; x < 3 && x < (int)n_of[best]; x++) {
                        const unsigned char *p = ex_of[best][x];
                        fprintf(stderr, "[memory]   ");
                        for (int j = 0; j < 48; j++) fprintf(stderr, "%02x", p[j]);
                        fprintf(stderr, "  ");
                        for (int j = 0; j < 48; j++) fputc(p[j] >= 32 && p[j] < 127 ? p[j] : '.', stderr);
                        fprintf(stderr, "\n");
                    }
                    n_of[best] = 0;
                }
            }
        }
    }
    fflush(stderr);
}

/* The loaded page, before it settles: save it for inspection. */
static void save_loaded_html(void) {
    char *serialized = page_serialize();
    if (serialized) {
        save_html(serialized, strlen(serialized));
        free(serialized);
    }
}

int main(int argc, char *argv[]) {
    det_clock_set_js_thread();     /* this thread runs the page */
    tl_mark("main");
    /* Diagnostics on stderr and progress on stdout are buffered (unbuffered,
     * MinGW's printf writes a few bytes per system call: half a second of a
     * YouTube load for stdout's 1.5 KB) and flushed at the top of every
     * event-loop turn, by the crash handler and at exit: a process killed
     * from outside loses at most one turn. */
    setvbuf(stderr, NULL, _IOFBF, 1 << 16);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    /* Start URL: argv[1] wins, then CYBER_START_URL, then the compiled-in
     * default (Wikipedia).  The YouTube-specific shims below only run when the
     * resolved URL is a YouTube page. */
    const char *start_url = DEFAULT_START_URL;
    if (argc > 1 && argv[1] && argv[1][0]) {
        start_url = argv[1];
    } else {
        const char *env_url = cyber_getenv("CYBER_START_URL");
        if (env_url && env_url[0]) start_url = env_url;
    }

#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_exception_filter);
#endif

    /* Fast JS-engine probe: eval a file and exit (CYBER_PROBE_JS=/path/to.js).
     * Optional CYBER_PROBE_NO_LAZY=1 forces full (non-lazy) parsing. */
    {
        const char *probe = cyber_getenv("CYBER_PROBE_JS");
        if (probe && probe[0]) {
            if (!platform_init()) { printf("FATAL: platform_init() failed\n"); return 1; }
            cyber_sampler_start();
            if (!platform_http_init()) { printf("FATAL: platform_http_init() failed\n"); return 1; }
            if (!page_engine_init(NULL)) { printf("FATAL: init_browser_context() failed\n"); return 1; }
            g_ctx = page_engine_context();
            page_engine_load_fonts();   /* layout queries measure text */
            FILE *pf = fopen(probe, "rb");
            if (!pf) { printf("FATAL: cannot open probe %s\n", probe); return 1; }
            fseek(pf, 0, SEEK_END);
            long psz = ftell(pf);
            fseek(pf, 0, SEEK_SET);
            char *pbuf = (char *)malloc((size_t)psz + 1);
            fread(pbuf, 1, (size_t)psz, pf);
            fclose(pf);
            pbuf[psz] = '\0';
            int pflags = JS_EVAL_TYPE_GLOBAL;
            if (cyber_getenv("CYBER_PROBE_NO_LAZY")) pflags |= JS_EVAL_FLAG_NO_LAZY;
            GCValue pres = JS_Eval(g_ctx, pbuf, (size_t)psz, "<probe>", pflags);
            if (JS_IsException(pres)) {
                GCValue exc = JS_GetException(g_ctx);
                const char *es = JS_ToCString(g_ctx, exc);
                printf("PROBE EXCEPTION: %s\n", es ? es : "?");
                GCValue stack = JS_GetPropertyStr(g_ctx, exc, "stack");
                const char *ss = JS_ToCString(g_ctx, stack);
                if (ss) printf("PROBE STACK:\n%s\n", ss);
            } else {
                printf("PROBE OK\n");
            }
            /* Pump timers/jobs so async continuations and setTimeout callbacks
             * run before exit (set CYBER_PROBE_NO_PUMP=1 to skip). */
            if (!cyber_getenv("CYBER_PROBE_NO_PUMP")) {
                for (int pi = 0; pi < 50; pi++) {
                    if (!page_pump(true) && js_quickjs_network_pending() == 0) break;
                }
            }
            free(pbuf);
            page_engine_cleanup();
            platform_http_cleanup();
            platform_cleanup();
            cyber_sampler_stop();
            return JS_IsException(pres) ? 1 : 0;
        }
    }

    printf("========================================\n");
    printf("CyberBrowser\n");
    printf("Start URL: %s\n", start_url);
    printf("========================================\n");

    CP_BEGIN("load-page");
    tl_mark("load-page start");

    if (!platform_init()) {
        printf("FATAL: platform_init() failed\n");
        return 1;
    }
    printf("platform_init ok\n");
    cyber_sampler_start();
    cyber_heapcheck_start();
    if (!platform_http_init()) {
        printf("FATAL: platform_http_init() failed\n");
        platform_cleanup();
        return 1;
    }

    {
        CP_SCOPE("init-browser-context");
        const PageHooks hooks = { tl_mark, save_loaded_html };
        if (!page_engine_init(&hooks)) {
            platform_http_cleanup();
            platform_cleanup();
            return 1;
        }
    }
    printf("init_browser_context ok\n");
    g_ctx = page_engine_context();

    /* Fonts before any script runs: layout queries from scripts
     * (offsetHeight, getBoundingClientRect) measure text with them. */
    page_engine_load_fonts();

    page_prepare(start_url);

    /* Layout probe: render a local HTML file through the production layout
     * pipeline and exit (CYBER_LAYOUT_HTML=/path/to.html). */
    {
        const char *lhtml_path = cyber_getenv("CYBER_LAYOUT_HTML");
        if (lhtml_path && lhtml_path[0]) {
            FILE *lf = fopen(lhtml_path, "rb");
            if (!lf) { printf("FATAL: cannot open %s\n", lhtml_path); return 1; }
            fseek(lf, 0, SEEK_END);
            long lsz = ftell(lf);
            fseek(lf, 0, SEEK_SET);
            char *lbuf = (char *)malloc((size_t)lsz + 1);
            fread(lbuf, 1, (size_t)lsz, lf);
            fclose(lf);
            lbuf[lsz] = '\0';
            HtmlDocument *ldoc = html_parse(lbuf, (size_t)lsz);
            free(lbuf);
            if (!ldoc) { printf("FATAL: html_parse failed\n"); return 1; }
            ImageCache *lcache = image_cache_create();
            display_list_set_image_cache(lcache);
            render_document_to_jpg(ldoc, lcache, "layout_probe.jpg");
            /* Layout requested images (natural sizes) and paint requested
             * pixels: once they are in, lay out and paint again. */
            for (int pass = 0; pass < 3 && image_cache_has_pending(lcache); pass++) {
                image_cache_wait_pending(lcache);
                render_document_to_jpg(ldoc, lcache, "layout_probe.jpg");
            }
            html_document_free(ldoc);
            image_cache_destroy(lcache);
            page_engine_cleanup();
            platform_http_cleanup();
            platform_cleanup();
            return 0;
        }
    }

    /* Fetch, parse and run the page (page_loaded.html is saved once it has
     * run, by save_loaded_html). */
    if (!page_load()) {
        page_engine_cleanup();
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }

    /* Phase 4: quiescence loop.  Pump timers/jobs, process completed async
     * image loads, and rebuild the native HtmlDocument from the JS DOM whenever
     * a mutation invalidates it.  Exit when nothing is left to do. */
    int loop_iterations = 0;
    /* Pages with recurring timers (setInterval, polling) never go quiet, so
     * bound the settle phase by wall-clock time too (CYBER_SETTLE_MS,
     * default 15 s). */
    unsigned long long settle_budget_ms = 15000;
    {
        const char *sm = cyber_getenv("CYBER_SETTLE_MS");
        if (sm && *sm) settle_budget_ms = strtoull(sm, NULL, 10);
    }
    unsigned long long settle_start_ms = platform_get_time_ms();
    bool settle_final = false;
    /* Recurring timers keep the loop busy long after the page stops
     * changing.  Settle once nothing that renders has changed and no image
     * has arrived for CYBER_QUIET_MS (default 1500 ms; 0 waits for the full
     * budget). */
    unsigned long long quiet_ms = 1500;
    {
        const char *qm = cyber_getenv("CYBER_QUIET_MS");
        if (qm && *qm) quiet_ms = strtoull(qm, NULL, 10);
    }
    uint64_t last_stamp = dom_change_stamp();
    /* The quiet window is page time: virtual time can cross it. */
    unsigned long long last_change_ms = page_time_ms();
    unsigned long long settle_start_page = last_change_ms;
    /* CYBER_RUN_UNTIL=<global>: a page that runs on its own for long (a
     * benchmark) keeps the loop going, with no turn limit and no quiet
     * window, until that global of the page is truthy (or the settle
     * budget runs out). */
    const char *run_until = cyber_getenv("CYBER_RUN_UNTIL");
    if (run_until && !*run_until) run_until = NULL;
    CP_BEGIN("quiescence-loop");
    tl_mark("quiescence start");
    while (loop_iterations < 100 || run_until) {
        loop_iterations++;
        if (run_until) {
            GCValue g = JS_GetGlobalObject(g_ctx);
            GCValue v = JS_GetPropertyStr(g_ctx, g, run_until);
            if (JS_ToBool(g_ctx, v)) {
                printf("%s set after %d iterations (%llu ms real).\n", run_until, loop_iterations,
                       platform_get_time_ms() - settle_start_ms);
                break;
            }
        }

        PageTurn turn;
        page_turn(!settle_final, WIREFRAME_WIDTH, WIREFRAME_HEIGHT, &turn);
        bool had_timers = turn.had_timers;
        bool had_images = turn.had_images;


        if (!had_timers && !had_images && !page_needs_layout() &&
            !image_cache_has_pending(page_image_cache()) && js_quickjs_network_pending() == 0) {
            if (!run_until)
                break;
            platform_sleep_ms(1);   /* idle until a timer is due */
        }
        {
            unsigned long long now = page_time_ms();
            uint64_t stamp = dom_change_stamp();
            if (cyber_getenv("CYBER_SETTLE_LOG")) {
                static uint64_t ps, pa, pw, pl;
                fprintf(stderr, "[settle] t=%llu changes=%llu (connected %llu structure %llu style %llu layoutreq %llu) net=%d images=%d timers=%d\n",
                        now - settle_start_page, (unsigned long long)(stamp - last_stamp),
                        (unsigned long long)(g_dom.connected_version - pa), (unsigned long long)(g_dom.structure_version - ps),
                        (unsigned long long)(JS_watched_write_epoch - pw),
                        (unsigned long long)(g_dom_layout_requests - pl),
                        js_quickjs_network_pending(), had_images, had_timers);
                ps = g_dom.structure_version; pa = g_dom.connected_version;
                pw = JS_watched_write_epoch; pl = g_dom_layout_requests;
            }
            if (stamp != last_stamp || had_images || js_quickjs_network_pending() > 0) {
                last_stamp = stamp;
                last_change_ms = now;
            } else if (quiet_ms && !run_until && !page_needs_layout() && now - last_change_ms >= quiet_ms) {
                printf("Page quiet for %llu ms after %d iterations (%llu ms page time, %llu ms real); "
                       "pending timers left running.\n",
                       now - last_change_ms, loop_iterations, now - settle_start_page,
                       platform_get_time_ms() - settle_start_ms);
                break;
            }
        }
        if (!had_timers && !had_images && !page_needs_layout()) {
            /* Only image fetches/decodes in flight: sleep until one makes
             * progress instead of spinning the settle loop. */
            page_wait_for_work(last_change_ms, quiet_ms);
        }
        if (settle_final || platform_get_time_ms() - settle_start_ms > settle_budget_ms) {
            if (page_needs_layout() && !settle_final) {
                settle_final = true;    /* one more turn: lay out, no timers */
                continue;
            }
            printf("Settle budget (%llu ms) reached after %d iterations; pending timers left running.\n",
                   settle_budget_ms, loop_iterations);
            break;
        }
    }
    CP_END("quiescence-loop");
    /* The page load is over: what follows is the harness's (the screenshot,
     * dumps).  tests/jit/replay.sh times the load to here. */
    tl_mark("page settled");
    /* The page is idle: collect now, in the background, rather than at the
     * next 128 MB of allocation (half the JS heap is garbage by then, and
     * the free pages go back to the OS). */
    if (gc_is_background()) {
        gc_request_cycle(true);
        /* CYBER_MEM: the memory once that collection is over */
        if (cyber_getenv("CYBER_MEM") && gc_wait_idle(5000))
            tl_mark("idle collection done");
    }
    frame_save_jpg(page_frame(), "wikipedia_screenshot.jpg");
    page_frame_free(page_frame());

    /* CYBER_POSTLOAD_JS=/path/to.js: evaluate a script against the settled
     * page (inspect live state; its console output goes to stderr). */
    {
        const char *post = cyber_getenv("CYBER_POSTLOAD_JS");
        FILE *pf = post && post[0] ? fopen(post, "rb") : NULL;
        if (pf) {
            fseek(pf, 0, SEEK_END);
            long psz = ftell(pf);
            fseek(pf, 0, SEEK_SET);
            char *pbuf = (char *)malloc((size_t)psz + 1);
            if (pbuf && fread(pbuf, 1, (size_t)psz, pf) == (size_t)psz) {
                pbuf[psz] = '\0';
                GCValue r = JS_Eval(g_ctx, pbuf, (size_t)psz, "<postload>", JS_EVAL_TYPE_GLOBAL);
                if (JS_IsException(r)) {
                    const char *es = JS_ToCString(g_ctx, JS_GetException(g_ctx));
                    fprintf(stderr, "[postload] exception: %s\n", es ? es : "?");
                }
                page_pump(true);
            }
            free(pbuf);
            fclose(pf);
        }
    }

    tl_mark("before page_final");
    /* Save the final mutated JS DOM for inspection. */
    {
        char *serialized = page_serialize();
        if (serialized) {
            FILE *f = fopen("page_final.html", "wb");
            if (f) {
                fwrite(serialized, 1, strlen(serialized), f);
                fclose(f);
                printf("Saved final JS DOM to page_final.html (%zu bytes)\n", strlen(serialized));
            }
            free(serialized);
        }
    }

    /* ShadyDOM bookkeeping diagnostic (CYBER_DIAG_SHADY=1): compare the
     * logical (shady) light children of ytd-app against the physical tree and
     * inspect the masthead slot's assigned nodes. */
    if (cyber_getenv("CYBER_DIAG_SHADY")) {
        const char *shady_diag_js =
            "(function(){"
            "  try {"
            "    var app = document.querySelector('ytd-app');"
            "    var SD = window.ShadyDOM;"
            "    var out = [];"
            "    out.push('app=' + !!app + ' SD=' + !!SD + ' wrap=' + (SD ? typeof SD.wrap : 'n/a'));"
            "    if (app && SD && SD.wrap) {"
            "      var wapp = SD.wrap(app);"
            "      var cn = wapp.childNodes;"
            "      out.push('logicalKids=' + (cn ? cn.length : -1));"
            "      var names = [];"
            "      if (cn) for (var i = 0; i < cn.length; i++) names.push(cn[i].tagName || cn[i].nodeName);"
            "      out.push('logical=[' + names.join(',') + ']');"
            "      var mc = app.querySelector('#masthead-container');"
            "      out.push('mc=' + !!mc);"
            "      if (mc) {"
            "        var slot = mc.firstElementChild;"
            "        out.push('slotTag=' + (slot ? slot.tagName : 'none'));"
            "        if (slot && typeof slot.assignedNodes === 'function') {"
            "          var an = slot.assignedNodes();"
            "          out.push('assigned=' + an.length);"
            "          var anames = [];"
            "          for (var j = 0; j < an.length; j++) anames.push(an[j].tagName || an[j].nodeName);"
            "          out.push('assignedTo=[' + anames.join(',') + ']');"
            "        }"
            "      }"
            "    }"
            "    console.error('[SHADY-DIAG] ' + out.join(' '));"
            "    var mh0 = app.querySelector('ytd-masthead');"
            "    console.error('[SHADY-DIAG] preFlush physKids=' + app.childNodes.length + ' mastInPhys=' + !!mh0);"
            "    if (SD && typeof SD.flush === 'function') { SD.flush(); }"
            "    var mh1 = app.querySelector('ytd-masthead');"
            "    console.error('[SHADY-DIAG] postFlush physKids=' + app.childNodes.length + ' mastInPhys=' + !!mh1);"
            "    var mh2 = document.querySelector('ytd-masthead');"
            "    console.error('[SHADY-DIAG] mastAnywhere=' + !!mh2 + (mh2 && mh2.parentNode ? ' parent=' + (mh2.parentNode.tagName || mh2.parentNode.nodeName) : ''));"
            "    var root = app.__CE_shadowRoot;"
            "    var ikeys = [];"
            "    if (root && root.i) for (var k in root.i) ikeys.push(k + '=' + root.i[k].length);"
            "    console.error('[SHADY-DEEP] root=' + !!root + ' rootG=' + (root && root.g ? root.g.length : -1) + ' slotmap=[' + ikeys.join(',') + ']');"
            "    var mh3 = null, cn3 = wapp.childNodes;"
            "    for (var q = 0; q < cn3.length; q++) if (cn3[q].tagName === 'YTD-MASTHEAD') mh3 = cn3[q];"
            "    console.error('[SHADY-DEEP] mh=' + !!mh3"
            "      + ' shady=' + (mh3 && mh3.__shady ? 'yes' : 'no')"
            "      + ' assignedSlot=' + (mh3 && mh3.__shady && mh3.__shady.assignedSlot ? mh3.__shady.assignedSlot.tagName : 'none')"
            "      + ' slotAttr=' + (mh3 ? mh3.getAttribute('slot') : 'n/a'));"
            "    var mc3 = app.querySelector('#masthead-container');"
            "    var slot3 = mc3 ? mc3.querySelector('slot') : null;"
            "    console.error('[SHADY-DEEP] slotEl=' + !!slot3"
            "      + ' shady=' + (slot3 && slot3.__shady ? 'yes' : 'no')"
            "      + ' assignedLen=' + (slot3 && slot3.__shady && slot3.__shady.assignedNodes ? slot3.__shady.assignedNodes.length : -1));"
            "    var rootSlots = 0;"
            "    try { rootSlots = root.querySelectorAll('slot').length; } catch(e2) { rootSlots = -2; }"
            "    var appSlots = 0;"
            "    try { appSlots = app.querySelectorAll('slot').length; } catch(e3) { appSlots = -2; }"
            "    console.error('[SHADY-DEEP2] rootKids=' + (root ? root.childNodes.length : -1)"
            "      + ' rootSlots=' + rootSlots"
            "      + ' appSlots=' + appSlots"
            "      + ' rootCtor=' + (root && root.constructor ? root.constructor.name : '?')"
            "      + ' appendNative=' + (root ? /native/.test(root.appendChild + '') : '?')"
            "      + ' slotParent=' + (slot3 && slot3.parentNode ? (slot3.parentNode.tagName || slot3.parentNode.nodeName) : 'null'));"
            "    var dfc = Object.getOwnPropertyDescriptor(Node.prototype, 'firstChild');"
            "    console.error('[SHADY-CFG] firstChild configurable=' + (dfc ? dfc.configurable : 'nodesc')"
            "      + ' hasGet=' + (dfc ? !!dfc.get : '-')"
            "      + ' SD.inUse=' + window.ShadyDOM.inUse"
            "      + ' SD.preferPerformance=' + window.ShadyDOM.preferPerformance"
            "      + ' SD.force=' + window.ShadyDOM.force);"
            "    if (rootSlots > 0) {"
            "      var rslot = root.querySelectorAll('slot')[0];"
            "      console.error('[SHADY-DEEP3] rootSlot shady=' + (rslot.__shady ? 'yes' : 'no')"
            "        + ' slotName=' + rslot.getAttribute('name')"
            "        + ' assignedLen=' + (rslot.__shady && rslot.__shady.assignedNodes ? rslot.__shady.assignedNodes.length : -1)"
            "        + ' sameAsPhys=' + (rslot === slot3));"
            "    }"
            "    console.error('[SHADY-LIVE] rootHasShadyInsert=' + (root ? typeof root.__shady_insertBefore : 'n/a')"
            "      + ' rootShady=' + (root && root.__shady ? 'yes' : 'no')"
            "      + ' protoAppendName=' + (Node.prototype.appendChild && Node.prototype.appendChild.name));"
            "  } catch(e) { console.error('[SHADY-DIAG] err: ' + (e && e.message)); }"
            "})();";
        JS_Eval(g_ctx, shady_diag_js, strlen(shady_diag_js), "<shady_diag>", JS_EVAL_TYPE_GLOBAL);
    }

    if (cyber_getenv("CYBER_DIAG_BOOT")) {
        const char *diag_js =
            "(function(){"
            "  var app = document.querySelector('ytd-app');"
            "  var mh = app && app.querySelector('ytd-masthead');"
            "  var L = window.default_kevlar_base;"
            "  var inst = null;"
            "  try { inst = L && L.LU && L.LU(); } catch(e) { console.error('[BOOT-DIAG] LU() threw: ' + (e && e.message)); }"
            "  var sigs = '?';"
            "  try { sigs = (inst && inst.signals) ? inst.signals.join(',') : 'none'; } catch(e) { sigs = 'err:' + e.message; }"
            "  (function(){"
            "    try {"
            "      var L2 = window.default_kevlar_base;"
            "      console.error('[CFG] _.C=' + (L2 ? typeof L2.C : 'n/a')"
            "        + ' _.YG=' + (L2 ? typeof L2.YG : 'n/a')"
            "        + ' _.mQ=' + (L2 ? typeof L2.mQ : 'n/a')"
            "        + ' _.D9=' + (L2 ? typeof L2.D9 : 'n/a'));"
            "      if (L2 && typeof L2.YG === 'function') {"
            "        try { console.error('[CFG] YG(EXPERIMENT_FLAGS) type=' + typeof L2.YG('EXPERIMENT_FLAGS', {})); }"
            "        catch(e) { console.error('[CFG] YG threw: ' + e.message); }"
            "      }"
            "      if (L2 && typeof L2.C === 'function') {"
            "        try { console.error('[CFG] C(web_monomer_web_component_wrapper_handle_errors)=' + L2.C('web_monomer_web_component_wrapper_handle_errors')); }"
            "        catch(e) { console.error('[CFG] C threw: ' + e.message); }"
            "      }"
            "    } catch(e) { console.error('[CFG] err: ' + e.message); }"
            "  })();"
            "  (function(){"
            "    try {"
            "      var arr = window.__cyber_all_errors || [];"
            "      for (var i = 0; i < arr.length; i++) {"
            "        console.error('[ERR' + i + '] ' + arr[i].msg + ' || ' + arr[i].stack);"
            "      }"
            "      if (!arr.length) console.error('[ERR] none');"
            "    } catch(e) { console.error('[ERR] read err: ' + e.message); }"
            "  })();"
            "  (function(){"
            "    try {"
            "      var fe = window.__cyber_first_real_error;"
            "      if (fe) console.error('[REAL-ERR] msg=' + fe.msg + ' | stack=' + String(fe.stack).split('\\n').slice(0,6).join(' | '));"
            "      else console.error('[REAL-ERR] none captured');"
            "    } catch(e) { console.error('[REAL-ERR] read err: ' + e.message); }"
            "  })();"
            "  (function(){"
            "    try {"
            "      if (inst) {"
            "        var before = inst.signals ? inst.signals.length : -1;"
            "        inst.processSignal('ci');"
            "        var after = inst.signals ? inst.signals.length : -1;"
            "        var idx = inst.signals ? inst.signals.indexOf('ci') : -2;"
            "        console.error('[D-CI] before=' + before + ' after=' + after + ' ciIdx=' + idx);"
            "      }"
            "    } catch(e) { console.error('[D-CI] err: ' + (e && e.message)); }"
            "  })();"
            "  (function(){"
            "    try { console.error('[D] div.__shady_attachShadow=' + typeof document.createElement('div').__shady_attachShadow); } catch(e) { console.error('[D] shady_attachShadow err: ' + e.message); }"
            "    try {"
            "      var sd = window.ShadyDOM;"
            "      console.error('[D] WeakMap.prototype.get=' + (window.WeakMap ? typeof WeakMap.prototype.get : 'N/A')"
            "        + ' set=' + (window.WeakMap ? typeof WeakMap.prototype.set : 'N/A'));"
            "      var fresh = new WeakMap(); var k={}; fresh.set(k, 9); console.error('[D] fresh wm.get=' + fresh.get(k));"
            "      console.error('[D] SD.wrap=' + (sd ? typeof sd.wrap : 'N/A'));"
            "      if (sd && sd.wrap) {"
            "        var w = sd.wrap(document.createElement('div'));"
            "        console.error('[D] wrap res=' + typeof w + ' wrap.attachShadow=' + (w ? typeof w.attachShadow : 'nul'));"
            "      }"
            "    } catch(e) { console.error('[D] wrap err: ' + e.message + ' | ' + (e.stack ? String(e.stack).split('\\n').slice(0,3).join('|') : '')); }"
            "  })();"
            "  console.error('[BOOT-DIAG] getInitialData=' + typeof window.getInitialData"
            "    + ' getInitialCommand=' + typeof window.getInitialCommand"
            "    + ' appChildren=' + (app ? app.childElementCount : -1)"
            "    + ' appShadow=' + (app && app.shadowRoot ? 'yes' : 'no')"
            "    + ' app__CE_sr=' + (app && app.__CE_shadowRoot ? 'yes' : 'no')"
            "    + ' app_sr_children=' + (app && app.__CE_shadowRoot && app.__CE_shadowRoot.childNodes ? app.__CE_shadowRoot.childNodes.length : -1)"
            "    + ' masthead=' + (mh ? 'found' : 'MISSING') + ' mhChildren=' + (mh ? mh.childElementCount : -1)"
            "    + ' mhShadow=' + (mh && mh.shadowRoot ? 'yes' : 'no')"
            "    + ' mh__CE_sr=' + (mh && mh.__CE_shadowRoot ? 'yes' : 'no')"
            "    + ' du-app=' + (app && app.hasAttribute('disable-upgrade'))"
            "    + ' du-mh=' + (mh && mh.hasAttribute('disable-upgrade'))"
            "    + ' signals=[' + sigs + ']');"
            "})();";
        GCValue diag_res = JS_Eval(g_ctx, diag_js, strlen(diag_js), "<boot_diag>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(diag_res)) {
            GCValue exc = JS_GetException(g_ctx);
            const char *es = JS_ToCString(g_ctx, exc);
            fprintf(stderr, "[BOOT-DIAG] eval EXCEPTION: %s\n", es ? es : "?");
        }
    }

    page_close();

    {
        uint64_t lq_queries = 0, lq_layouts = 0, lq_ms = 0;
        layout_api_stats(&lq_queries, &lq_layouts, &lq_ms);
        printf("Script layout queries: %llu (forced layouts: %llu, %llu ms, %llu ms CPU, %llu inside changed regions; "
               "kept: %llu; cached: %llu; styling hidden subtrees: %llu; restyled on demand: %llu)\n",
               (unsigned long long)lq_queries, (unsigned long long)lq_layouts,
               (unsigned long long)lq_ms, (unsigned long long)layout_api_cpu_ms(),
               (unsigned long long)layout_api_region_layouts(),
               (unsigned long long)layout_api_kept(), (unsigned long long)layout_api_cheap_hits(),
               (unsigned long long)layout_api_style_all_layouts(), (unsigned long long)layout_api_ondemand_kept());
    }
    {
        DomFetchStats fs;
        dom_fetch_stats(&fs);
        printf("Prefetched scripts and sheets: %llu (%llu ready when needed, %llu waited for: %.0f ms; %llu fetched unprefetched)\n",
               (unsigned long long)fs.queued, (unsigned long long)fs.hits, (unsigned long long)fs.waits,
               fs.wait_ms, (unsigned long long)fs.sync_fetches);
    }
    /* CYBER_LAYOUT_BENCH=<n>: after the load, n rounds of each scenario (a
     * DOM change, then a query of the changed element, as scripts do), with
     * each layout's phase times; the scenarios mimic what pages do between
     * frames and while scrolling (appending items to a list). */
    {
        const char *bn = getenv("CYBER_LAYOUT_BENCH");
        int n = bn ? atoi(bn) : 0;
        if (n > 0) {
            /* A visible list the benchmark makes at the top of the body (30
             * items), so each change is laid out and asked about. */
            static const char *setup =
                "(function(){var r=document.createElement('div');r.id='layout-bench';"
                "r.style.cssText='display:block;width:600px';"
                "for(var i=0;i<30;i++){var d=document.createElement('div');d.className='bench-item';"
                "d.textContent='Bench item '+i+' with some text to wrap across a line or two';r.appendChild(d);}"
                "document.body.insertBefore(r,document.body.firstChild);void r.offsetHeight;})();";
            static const struct { const char *label, *js; } scen[] = {
                { "append (infinite scroll)",
                  "(function(n){var r=document.getElementById('layout-bench');var made=[];"
                  "for(var i=0;i<n;i++){var d=document.createElement('div');d.className='bench-item';"
                  "d.textContent='Appended item '+i+' with some text';r.appendChild(d);made.push(d);void d.offsetHeight;}"
                  "for(var k=0;k<made.length;k++)made[k].remove();void r.offsetHeight;})" },
                { "inline style change",
                  "(function(n){var r=document.getElementById('layout-bench');"
                  "for(var i=0;i<n;i++){var el=r.children[i%r.children.length];el.style.paddingTop=(1+i%3)+'px';void el.offsetHeight;}})" },
                { "text change",
                  "(function(n){var r=document.getElementById('layout-bench');"
                  "for(var i=0;i<n;i++){var el=r.children[i%r.children.length];el.textContent='Changed item '+i+(i%2?' with a longer text that wraps':'');void el.offsetWidth;}})" },
            };
            {
                GCValue r = JS_Eval(g_ctx, setup, strlen(setup), "<layout-bench>", JS_EVAL_TYPE_GLOBAL);
                if (JS_IsException(r)) JS_GetException(g_ctx);
            }
            for (size_t k = 0; k < sizeof(scen) / sizeof(scen[0]); k++) {
                char js[2048];
                snprintf(js, sizeof(js), "%s(%d);", scen[k].js, n);
                uint64_t kept0, built0, kept1, built1, fs0, fb0, fs1, fb1;
                css_layout_tree_stats(&kept0, &built0);
                css_layout_flow_stats(&fs0, &fb0);
                css_layout_bench_record(true);
                double t0 = (double)platform_get_time_ms();
                GCValue r = JS_Eval(g_ctx, js, strlen(js), "<layout-bench>", JS_EVAL_TYPE_GLOBAL);
                double t1 = (double)platform_get_time_ms();
                css_layout_bench_record(false);
                if (JS_IsException(r)) JS_GetException(g_ctx);
                css_layout_tree_stats(&kept1, &built1);
                css_layout_flow_stats(&fs1, &fb1);
                fprintf(stderr, "[layout-bench] %s: %d rounds in %.0f ms; trees kept %llu, built %llu; "
                        "%llu subtrees reused (%llu boxes)\n", scen[k].label, n, t1 - t0,
                        (unsigned long long)(kept1 - kept0), (unsigned long long)(built1 - built0),
                        (unsigned long long)(fs1 - fs0), (unsigned long long)(fb1 - fb0));
                css_layout_bench_report(scen[k].label);
            }
        }
    }
    printf("\nStart page loaded successfully.\n");

    CP_END("load-page");
    tl_mark("load-page end");
    cyber_sampler_stop();
    if (cp_profile_enabled()) {
        cp_profile_flush("profile.json");
        cp_profile_write_flamegraph("flamegraph.png");
        printf("Profile written to profile.json + flamegraph.png\n");
    }

    if (cp_profile_enabled() || getenv("CYBER_DEBUG_NET")) http_net_log_stats();
    http_net_close_idle();
    tl_mark("before cleanup");
    page_engine_finish();
    platform_http_cleanup();
    platform_cleanup();
    tl_mark("after cleanup");
    return 0;
}
