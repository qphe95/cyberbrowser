/*
 * page_engine.c - the page engine (page_engine.h): the JS runtime and the
 * browser APIs, loading a page and running its scripts and event loop, and
 * laying it out into frames.
 */
#include "cyber_env.h"
#include "det_clock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

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

int timer_process_due(JSContextHandle ctx);
int scheduler_process_tasks(JSContextHandle ctx);
void timer_set_idle_deadline(unsigned long long deadline_ms);
unsigned long long timer_next_due_ms(void);
void platform_sleep_ms(unsigned int ms);

#include "page_engine.h"

extern const char *g_cyber_start_url;

static PageHooks g_hooks;

static void page_mark(const char *what) {
    if (g_hooks.mark) g_hooks.mark(what);
}

/* ============================================================================
 * ES module loader
 *
 * YouTube's modern app lazy-loads page-specific component modules via dynamic
 * import() of chunk URLs.  QuickJS calls the host module normalize/loader
 * hooks registered here to resolve and fetch those chunks.  Without them
 * every import() fails with "could not load module", so watch-page components
 * (ytd-watch-flexy, yt-formatted-string, ...) are never registered.
 * ============================================================================ */

/* Resolve a module specifier against the page origin into an absolute URL.
 * Handles absolute URLs, scheme-relative (//host), root-absolute (/path), and
 * relative (path) specifiers.  Returns a malloc'd string (caller frees). */
static char *cyber_module_resolve(const char *base, const char *specifier)
{
    if (!specifier || !specifier[0]) return NULL;

    /* Absolute URL (http://, https://, or any scheme:) — use as-is. */
    if (url_has_scheme(specifier)) return strdup(specifier);

    /* "./x", "../x" and "/x" resolve against the importing module's URL. */
    if (specifier[0] == '.' || (specifier[0] == '/' && specifier[1] != '/')) {
        const char *b = (base && url_has_scheme(base)) ? base : g_cyber_start_url;
        char *abs = b ? wurl_resolve(b, specifier) : NULL;
        if (abs) return abs;
    }

    /* Determine the origin from the base URL. */
    char origin[512] = "";
    const char *src = (base && base[0]) ? base : g_cyber_start_url;
    if (src) {
        const char *scheme = strstr(src, "://");
        if (scheme) {
            const char *host = scheme + 3;
            const char *slash = strchr(host, '/');
            size_t origin_len = slash ? (size_t)(slash - src) : strlen(src);
            if (origin_len >= sizeof(origin)) origin_len = sizeof(origin) - 1;
            memcpy(origin, src, origin_len);
            origin[origin_len] = '\0';
        }
    }
    if (!origin[0]) {
        /* No origin discoverable; fall back to the specifier verbatim. */
        return strdup(specifier);
    }

    /* Scheme-relative (//host/...) → prepend "https:". */
    if (specifier[0] == '/' && specifier[1] == '/') {
        size_t n = strlen(specifier) + 8;
        char *out = (char *)malloc(n);
        if (out) snprintf(out, n, "https:%s", specifier);
        return out;
    }
    /* Root-absolute (/path) → origin + path. */
    if (specifier[0] == '/') {
        size_t n = strlen(origin) + strlen(specifier) + 1;
        char *out = (char *)malloc(n);
        if (out) snprintf(out, n, "%s%s", origin, specifier);
        return out;
    }
    /* Relative (path) → origin + "/" + path.  (Coarse but adequate for
     * chunk specifiers that are already effectively absolute.) */
    size_t n = strlen(origin) + strlen(specifier) + 2;
    char *out = (char *)malloc(n);
    if (out) snprintf(out, n, "%s/%s", origin, specifier);
    return out;
}

/* JSModuleNormalizeFunc: return the canonical (absolute URL) module name as a
 * GCValue string. */
static GCValue cyber_module_normalize(JSContextHandle ctx, const char *module_base_name,
                                      const char *module_name, void *opaque)
{
    (void)opaque;
    char *resolved = cyber_module_resolve(module_base_name, module_name);
    if (!resolved) {
        JS_ThrowTypeError(ctx, "could not normalize module '%s'", module_name);
        return JS_EXCEPTION;
    }
    GCValue ret = JS_NewString(ctx, resolved);
    free(resolved);
    return ret;
}

/* JSModuleLoaderFunc: fetch the module source over HTTP and compile it as an
 * ES module.  Returns the module definition handle (or null on failure). */
static JSModuleDefHandle cyber_module_loader(JSContextHandle ctx, const char *module_name,
                                             void *opaque)
{
    (void)opaque;
    const char *url = module_name;

    /* Only http(s) URLs can be fetched; bare names can't be loaded. */
    if (!url_is_network_url(url)) {
        JS_ThrowReferenceError(ctx, "module '%s' is not a network URL", url);
        return (JSModuleDefHandle)(GC_HANDLE_NULL);
    }

    HttpBuffer buffer = {0};
    char err[256] = {0};
    bool ok = http_get_to_memory(url, &buffer, err, sizeof(err));
    if (!ok || !buffer.data || buffer.size == 0) {
        platform_log(LOG_LEVEL_WARN, "module_loader", "Failed to fetch module %s: %s",
                     url, err[0] ? err : "unknown");
        if (buffer.data) free(buffer.data);
        JS_ThrowReferenceError(ctx, "could not fetch module '%s'", url);
        return (JSModuleDefHandle)(GC_HANDLE_NULL);
    }

    platform_log(LOG_LEVEL_INFO, "module_loader", "Loaded module %s (%zu bytes)", url, buffer.size);

    if (CYBER_ENV(CYBER_T_DUMP_SCRIPTS)) {
        static int module_dump_idx = 0;
        char dump_name[64];
        snprintf(dump_name, sizeof(dump_name), "exec_module_%d.js", module_dump_idx++);
        FILE *df = fopen(dump_name, "wb");
        if (df) {
            fwrite(buffer.data, 1, buffer.size, df);
            fclose(df);
        }
        platform_log(LOG_LEVEL_INFO, "module_loader", "Dumped module to %s url=%s", dump_name, url);
    }

    /* Compile the source as a module (only: the importer links and evaluates
     * the whole graph).  JS_Eval registers it and returns a JS_TAG_MODULE
     * value whose handle we extract and return. */
    GCValue mv = JS_Eval(ctx, buffer.data, buffer.size, url,
                         JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY |
                         (cyber_getenv("CYBER_LAZY") && cyber_getenv("CYBER_LAZY")[0] == '0' ? JS_EVAL_FLAG_NO_LAZY : 0));
    free(buffer.data);
    if (JS_IsException(mv)) {
        GCValue exc = JS_GetException(ctx);
        const char *es = JS_ToCString(ctx, exc);
        platform_log(LOG_LEVEL_WARN, "module_loader", "Module eval error for %s: %s",
                     url, es ? es : "?");
        if (es) JS_FreeCString(ctx, es);
        JS_ThrowReferenceError(ctx, "could not compile module '%s'", url);
        return (JSModuleDefHandle)(GC_HANDLE_NULL);
    }

    JSModuleDefHandle m = JS_VALUE_GET_MODULE_HANDLE(mv);
    if (SAFE_VALID(m)) js_quickjs_set_module_url(ctx, m, url);
    if (!SAFE_VALID(m)) {
        platform_log(LOG_LEVEL_WARN, "module_loader", "Module %s did not produce a def", url);
        JS_ThrowReferenceError(ctx, "module '%s' produced no definition", url);
        return (JSModuleDefHandle)(GC_HANDLE_NULL);
    }
    return m;
}

static JSRuntimeHandle g_rt = GC_HANDLE_NULL;
static JSContextHandle g_ctx = GC_HANDLE_NULL;
static GCValue g_global = GC_UNDEFINED_INIT;

/* Interrupt handler for the bounded customElements.upgrade pass. */
struct upgrade_timeout_state { struct timespec start; double limit; };
static int upgrade_timeout_handler(JSRuntimeHandle rt, void *opaque) {
    (void)rt;
    struct upgrade_timeout_state *uts = (struct upgrade_timeout_state *)opaque;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - uts->start.tv_sec) +
                     (now.tv_nsec - uts->start.tv_nsec) / 1e9;
    return elapsed > uts->limit ? 1 : 0;
}

static bool init_browser_context(void) {
    if (!gc_init()) {
        printf("FATAL: gc_init() failed\n");
        return false;
    }

    g_rt = JS_NewRuntime();
    if (!SAFE_VALID(g_rt)) {
        printf("FATAL: JS_NewRuntime() failed\n");
        return false;
    }
    /* QuickJS defaults to a 1 MB JS stack (about 760 nested calls here);
     * real pages (YouTube's Polymer) recurse deeper than that. */
    JS_SetMaxStackSize(g_rt, js_stack_budget());

    /* Register the ES module loader so dynamic import() can fetch YouTube's
     * lazily-loaded component chunks over HTTP. */
    JS_SetModuleLoaderFunc(g_rt, cyber_module_normalize, cyber_module_loader, NULL);

    g_ctx = JS_NewContext(g_rt);
    if (!SAFE_VALID(g_ctx)) {
        printf("FATAL: JS_NewContext() failed\n");
        JS_FreeRuntime(g_rt);
        return false;
    }

    JS_AddIntrinsicBaseObjects(g_ctx);
    JS_AddIntrinsicEval(g_ctx);
    JS_AddIntrinsicRegExp(g_ctx);
    JS_AddIntrinsicJSON(g_ctx);
    JS_AddIntrinsicPromise(g_ctx);
    JS_AddIntrinsicMapSet(g_ctx);
    JS_AddIntrinsicTypedArrays(g_ctx);
    JS_AddIntrinsicProxy(g_ctx);
    JS_AddIntrinsicWeakRef(g_ctx);

    g_global = JS_GetGlobalObject(g_ctx);
    if (JS_IsException(g_global)) {
        printf("FATAL: JS_GetGlobalObject() failed\n");
        JS_FreeRuntime(g_rt);
        return false;
    }

    extern JSRuntimeHandle g_js_runtime;
    extern JSContextHandle g_js_context;
    g_js_runtime = g_rt;
    g_js_context = g_ctx;
    JS_EnableScriptPrecompile(g_ctx);

    init_browser_api_impl(g_ctx, g_global);
    js_quickjs_setup_initial_dom();
    return true;
}

static void cleanup_browser_context(void) {
    extern JSRuntimeHandle g_js_runtime;
    extern JSContextHandle g_js_context;
    g_js_runtime = GC_HANDLE_NULL;
    g_js_context = GC_HANDLE_NULL;
    if (SAFE_VALID(g_rt)) {
        if (getenv("QJS_GC_CENSUS")) {
            extern void gc_census_dump(void);
            gc_census_dump();
        }
        JS_FreeRuntime(g_rt);
    }
}

/* At the end of a successful run: the OS reclaims the heap at exit, and
 * freeing a YouTube-sized runtime object by object took 0.3 s.  Only the
 * GC thread is stopped, so nothing runs during process exit.
 * CYBER_FULL_CLEANUP=1 tears everything down (leak and teardown checks). */
static void finish_browser_context(void) {
    if (cyber_getenv("CYBER_FULL_CLEANUP")) {
        cleanup_browser_context();
        return;
    }
    if (SAFE_VALID(g_rt) && getenv("QJS_GC_CENSUS")) {
        extern void gc_census_dump(void);
        gc_census_dump();
    }
    extern void gc_core_stop_thread(void);
    gc_core_stop_thread();
}

typedef struct PageObserver {
    HtmlPreloadScan *scan;
    HtmlPageStream *page;
} PageObserver;

/* The page as it arrives: first what it will fetch, then parse and run. */
static void page_observe(void *user, const char *data, size_t len) {
    PageObserver *o = (PageObserver *)user;
    if (o->scan) html_preload_scan_feed(o->scan, data, len);
    if (o->page) html_page_stream_feed(o->page, data, len);
}

static char *fetch_start_page(const char *url, size_t *out_size, HtmlPageStream *page) {
    printf("Fetching %s ...\n", url);

    HttpBuffer buffer = {0};
    char error[512] = {0};
    /* Google's consent cookie only makes sense for Google properties; sending
     * it to other sites is pointless, so include it just for YouTube. */
    const bool send_google_consent = strstr(url, "youtube.") != NULL;
    const char *headers[16];
    int nheaders = 0;
    headers[nheaders++] = "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36";
    headers[nheaders++] = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8";
    headers[nheaders++] = "Accept-Language: en-US,en;q=0.9";
    if (send_google_consent)
        headers[nheaders++] = "Cookie: CONSENT=YES+US.en+; PREF=tz=America.Los_Angeles&f4=4000000";
    headers[nheaders++] = "sec-ch-ua: \"Google Chrome\";v=\"125\", \"Chromium\";v=\"125\", \"Not.A/Brand\";v=\"24\"";
    headers[nheaders++] = "sec-ch-ua-mobile: ?0";
    headers[nheaders++] = "sec-ch-ua-platform: \"Windows\"";
    headers[nheaders++] = "sec-fetch-dest: document";
    headers[nheaders++] = "sec-fetch-mode: navigate";
    headers[nheaders++] = "sec-fetch-site: none";
    headers[nheaders++] = "sec-fetch-user: ?1";
    headers[nheaders++] = "upgrade-insecure-requests: 1";

    /* Scan the page as it arrives: its scripts and stylesheets start
     * downloading while the rest of it does (CYBER_PRELOAD_SCAN=0: once it
     * is all here, as before). */
    const char *ps = cyber_getenv("CYBER_PRELOAD_SCAN");
    PageObserver obs = { ps && ps[0] == '0' ? NULL : html_preload_scan_begin(), page };
    bool ok = obs.scan || obs.page
        ? http_get_to_memory_observed(url, headers, nheaders, &buffer, error, sizeof(error), page_observe, &obs)
        : http_get_to_memory_with_headers(url, headers, nheaders, &buffer, error, sizeof(error));
    html_preload_scan_end(obs.scan);
    if (!ok || !buffer.data || buffer.size == 0) {
        printf("FATAL: Failed to fetch start page: %s\n", error);
        return NULL;
    }

    char *html = (char *)malloc(buffer.size + 1);
    if (!html) {
        http_free_buffer(&buffer);
        return NULL;
    }
    memcpy(html, buffer.data, buffer.size);
    html[buffer.size] = '\0';
    if (out_size) *out_size = buffer.size;
    http_free_buffer(&buffer);
    return html;
}

static const char *get_title_text(HtmlDocument *doc) {
    HtmlNode *title = html_document_get_element_by_tag(doc, "title");
    if (title) {
        HtmlNode *child = html_node_first_child(doc, title);
        if (child && child->type == HTML_NODE_TEXT && child->text_content && child->text_content[0]) {
            return child->text_content;
        }
        if (title->text_content && title->text_content[0]) {
            return title->text_content;
        }
    }
    return NULL;
}

static void print_body_snippet(HtmlDocument *doc) {
    HtmlNode *body = html_document_body(doc);
    if (!body) return;

    HtmlNode *child = html_node_first_child(doc, body);
    int chars = 0;
    printf("Body snippet: \"");
    while (child && chars < 200) {
        if (child->type == HTML_NODE_TEXT && child->text_content) {
            const char *p = child->text_content;
            while (*p && chars < 200) {
                if (isprint((unsigned char)*p) && *p != '\n' && *p != '\r') {
                    putchar(*p);
                    chars++;
                } else if (*p == ' ' || *p == '\t') {
                    putchar(' ');
                    chars++;
                }
                p++;
            }
        }
        child = html_node_next_sibling(doc, child);
    }
    printf("\"\n");
}

/* ------------------------------------------------------------------------- */
/* Phase 3 event loop helpers                                                */
/* ------------------------------------------------------------------------- */

/* Virtual time (CYBER_REAL_TIME=1: off).  When the page is idle, waiting
 * only for its own timers, the event loop advances the page clock to the
 * next one instead of sleeping (as headless Chrome's virtual time does).
 * Never while a fetch or an image is in flight: page timeouts that race the
 * network must not fire early. */
static ImageCache *g_loop_images;

static bool virtual_time_idle(void) {
    static int real = -1;
    if (real < 0) real = cyber_getenv("CYBER_REAL_TIME") ? 1 : 0;
    if (real) return false;
    if (js_quickjs_network_pending() > 0) return false;
    /* Images being fetched or decoded; finished ones are delivered at the
     * top of each event-loop turn. */
    if (g_loop_images && image_cache_has_work(g_loop_images)) return false;
    return true;
}

/* CYBER_LOOP_LOG=1: log every real wait of the event loop (what it waits
 * for, the timer delay, what the image cache has in flight), with totals
 * at exit. */
static unsigned long long g_loop_wait_ms[2];   /* sleeps, network waits */
static void loop_log_total(void) {
    fprintf(stderr, "[loop] waited %llu ms sleeping (images in flight), %llu ms on the network\n",
            g_loop_wait_ms[0], g_loop_wait_ms[1]);
}
static void loop_log_wait(const char *what, unsigned long long wait) {
    static int on = -1;
    if (on < 0) {
        on = cyber_getenv("CYBER_LOOP_LOG") ? 1 : 0;
        if (on) atexit(loop_log_total);
    }
    if (!on || !what) return;
    int working = 0, undelivered = 0;
    image_cache_pending_counts(g_loop_images, &working, &undelivered);
    g_loop_wait_ms[what[0] == 'n'] += wait ? wait : 1;
    fprintf(stderr, "[loop] %s %llu ms at page time %llu: images working %d, undelivered %d, fetches %d\n",
            what, wait, page_time_ms(), working, undelivered, js_quickjs_network_pending());
}

/* CYBER_DET_TIME (det_clock.h): at the top of each event-loop turn, wait
 * until no fetch, stylesheet or image is in flight, so the page receives
 * each at the same point of its run every time.  The waits are real time,
 * which the deterministic clock does not see. */
static void det_drain_in_flight(void) {
    if (!det_clock_on()) return;
    bool net = js_quickjs_net_wait_all(2000);
    bool sheets = dom_fetch_wait_all(2000);
    bool images = !g_loop_images || image_cache_wait_pending_ms(g_loop_images, 2000);
    if (!net || !sheets || !images)
        fprintf(stderr, "[det] still in flight after 2 s:%s%s%s\n", net ? "" : " fetches",
                sheets ? "" : " prefetches", images ? "" : " images");
}

/* One event-loop drain: due timers, scheduler tasks and promise jobs, again
 * until nothing is due.  With `wait`, also wait (bounded) for timers due
 * soon and for fetches in flight. */
static bool pump_event_loop(JSContextHandle ctx, bool wait) {
    if (!SAFE_VALID(ctx)) return false;
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    bool did_work = false;
    int iterations = 0;
    /* Budgets in page time: with virtual time they bound what a pump skips
     * ahead exactly as they bounded what it slept. */
    unsigned long long pump_start = page_time_ms();
    while (iterations < 100) {
        // Give idle callbacks a real, per-iteration idle budget.
        unsigned long long idle_deadline = page_time_ms() + 8;
        timer_set_idle_deadline(idle_deadline);

        /* Top of an event-loop turn: a tick boundary, and where the
         * buffered diagnostics go out. */
        det_drain_in_flight();
        gc_tick();
        fflush(stdout);
        fflush(stderr);
        /* Images that finished loading: their load events are tasks of
         * this turn (before the timers, which are due later). */
        int images = g_loop_images && image_cache_process_pending(g_loop_images) ? 1 : 0;
        int processed = timer_process_due(ctx);
        int sched = scheduler_process_tasks(ctx);
        int jobs = 0;
        JSContextHandle pctx = GC_HANDLE_NULL;
        int ret;
        while ((ret = JS_ExecutePendingJob(rt, &pctx)) > 0) {
            jobs++;
        }
        (void)ret;
        if (processed == 0 && sched == 0 && jobs == 0 && images == 0) {
            if (!wait) break;
            /* Nothing immediately due.  Lifecycle callbacks (e.g. the monomer
             * "initialized" job set) are scheduled via setTimeout with a small
             * delay, and YouTube's app never reaches its "rendering" phase if
             * those never fire.  Wait for the earliest pending timer within a
             * bounded window instead of dropping it, so delayed callbacks run. */
            unsigned long long next = timer_next_due_ms();
            if (next != (unsigned long long)-1) {
                unsigned long long now = page_time_ms();
                unsigned long long wait = next > now ? next - now : 0;
                unsigned long long elapsed = now - pump_start;
                if (wait <= 200 && elapsed + wait <= 3000) {
                    loop_log_wait(js_quickjs_network_pending() > 0 ? "net" :
                                  virtual_time_idle() ? NULL : "sleep", wait);
                    if (js_quickjs_network_pending() > 0)
                        js_quickjs_net_wait((unsigned int)(wait ? wait : 1));
                    else if (virtual_time_idle())
                        page_time_advance(wait ? wait : 1);
                    else if (g_loop_images)
                        /* Until the timer is due or an image finishes (the
                         * page is then idle and time can skip ahead). */
                        image_cache_wait_progress(g_loop_images, (int)(wait ? wait : 1));
                    else
                        platform_sleep_ms((unsigned int)(wait ? wait : 1));
                    continue;
                }
            }
            /* A fetch() in flight: wait for its response like for a timer. */
            if (js_quickjs_network_pending() > 0 &&
                page_time_ms() - pump_start <= 3000) {
                loop_log_wait("net (no timer)", 50);
                js_quickjs_net_wait(50);
                continue;
            }
            break;
        }
        did_work = true;
        iterations++;
    }
    timer_set_idle_deadline(0);
    return did_work;
}

static bool pump_timers_and_jobs(JSContextHandle ctx) {
    return pump_event_loop(ctx, true);
}

/* Between the boot steps (scripts, DOMContentLoaded/load, element upgrade)
 * a browser does not hold the page for timers that are not due yet: they
 * fire later, in the settle loop.  CYBER_PREBOOT_WAIT=1 waits for them
 * here as well (the old behaviour). */
static bool pump_due_work(JSContextHandle ctx) {
    static int wait = -1;
    if (wait < 0) wait = cyber_getenv("CYBER_PREBOOT_WAIT") ? 1 : 0;
    return pump_event_loop(ctx, wait != 0);
}

static void dispatch_page_lifecycle_events(JSContextHandle ctx) {
    const char *lifecycle_js =
        "document.readyState = 'interactive';"
        "var dcl = new Event('DOMContentLoaded', { bubbles: true });"
        "document.dispatchEvent(dcl);"
        "document.readyState = 'complete';"
        "var loadEvt = new Event('load');"
        "window.dispatchEvent(loadEvt);"
        "document.dispatchEvent(loadEvt);";
    GCValue result = JS_Eval(ctx, lifecycle_js, strlen(lifecycle_js),
                             "<lifecycle>", JS_EVAL_TYPE_GLOBAL);
    (void)result;
}

static GCValue get_global_document(JSContextHandle ctx) {
    if (!SAFE_VALID(ctx)) return JS_NULL;
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue doc = JS_GetPropertyStr(ctx, global, "document");
    return doc;
}

static void on_image_progress(const char *url, void *user_data)
{
    (void)user_data;
    image_api_notify(g_ctx, url);
    dom_request_relayout();
}

/* Fonts come from the OS font folders and the page's @font-face rules
 * (font_registry.c); index the system fonts up front. */
static void register_font_table(void)
{
    font_registry_init();
    if (!display_list_get_default_font())
        printf("WARNING: no system font found; text will not be measured or drawn\n");
}

/* CYBER_LAYOUT_DUMP=1 (or a path): write the final layout's boxes to
 * layout_dump.txt (or that path).  Off by default: 40 ms a write. */
static const char *layout_dump_path(void) {
    const char *e = cyber_getenv("CYBER_LAYOUT_DUMP");
    if (!e || !e[0] || strcmp(e, "0") == 0) return NULL;
    return strcmp(e, "1") == 0 ? "layout_dump.txt" : e;
}

/* ------------------------------------------------------------------------- */
/* Frames                                                                    */
/* ------------------------------------------------------------------------- */

void page_frame_free(PageFrame *f) {
    if (!f->valid) return;
    display_list_free(&f->dl);
    css_layout_tree_free(&f->layout);
    f->valid = false;
}

void page_frame_build(PageFrame *f, HtmlDocument *doc, ImageCache *images, int width, int height) {
    page_frame_free(f);
    printf("Running CSS layout (%dx%d) ...\n", width, height);
    memset(&f->layout, 0, sizeof(f->layout));
    f->layout.js_ctx = g_ctx;
    f->layout.dump_path = layout_dump_path();
    bool ok;
    {
        CP_SCOPE("css-layout");
        ok = css_layout_run(&f->layout, doc, NULL, (double)width, (double)height);
    }
    if (!ok) {
        printf("WARNING: css_layout_run() failed\n");
        css_layout_tree_free(&f->layout);
        return;
    }
    printf("Layout boxes: %d\n", f->layout.tree.count);
    display_list_init(&f->dl);
    {
        CP_SCOPE("build-display-list");
        ok = css_layout_build_display_list(&f->layout, &f->dl);
    }
    if (!ok) {
        printf("WARNING: css_layout_build_display_list() failed\n");
        display_list_free(&f->dl);
        css_layout_tree_free(&f->layout);
        return;
    }
    f->valid = true;
    /* Images first requested while building the list (mask images,
     * backgrounds) finish loading here, within a bound (a slow CDN must not
     * hold the frame): taken one turn at a time, each arrival laid the page
     * out again.  One that arrives later requests a layout
     * (on_image_progress), and the next turn builds the frame again. */
    if (!image_cache_wait_pending_ms(images, 1500))
        printf("Images still loading; painting without them for now\n");
}

/* Before the page's first script: engine polyfills, diagnostics and the
 * CYBER_PRELUDE_JS hook. */
static void prepare_page_scripts(void) {
    {
        const char *bind_polyfill =
            "(function(){"
            "  var orig = Function.prototype.bind;"
            "  Function.prototype.bind = function(){"
            "    var b = orig.apply(this, arguments);"
            "    try { b.__cyber_bound_target = this; } catch(e) {}"
            "    return b;"
            "  };"
            "})();";
        JS_Eval(g_ctx, bind_polyfill, strlen(bind_polyfill), "<bind_polyfill>", JS_EVAL_TYPE_GLOBAL);
    }
    {
        const char *pre_diag = "console.error('[PRE-DIAG] Element.matches=' + typeof Element.prototype.matches);";
        JS_Eval(g_ctx, pre_diag, strlen(pre_diag), "<pre_diag>", JS_EVAL_TYPE_GLOBAL);
    }
    /* Developer hook: CYBER_PRELUDE_JS=<file> is evaluated in the page
     * realm before any page script (instrumentation, polyfill tests). */
    {
        const char *prelude = cyber_getenv("CYBER_PRELUDE_JS");
        if (prelude && *prelude) {
            FILE *pf = fopen(prelude, "rb");
            if (pf) {
                fseek(pf, 0, SEEK_END);
                long psz = ftell(pf);
                fseek(pf, 0, SEEK_SET);
                char *pbuf = psz >= 0 ? (char *)malloc((size_t)psz + 1) : NULL;
                if (pbuf && fread(pbuf, 1, (size_t)psz, pf) == (size_t)psz) {
                    pbuf[psz] = 0;
                    GCValue pr = JS_Eval(g_ctx, pbuf, (size_t)psz, "<prelude>", JS_EVAL_TYPE_GLOBAL);
                    if (JS_IsException(pr)) {
                        GCValue exc = JS_GetException(g_ctx);
                        const char *es = JS_ToCString(g_ctx, exc);
                        fprintf(stderr, "[PRELUDE] exception: %s\n", es ? es : "?");
                    }
                }
                free(pbuf);
                fclose(pf);
            } else {
                fprintf(stderr, "[PRELUDE] cannot open %s\n", prelude);
            }
        }
    }
    js_quickjs_clear_captured_urls();
}

/* ------------------------------------------------------------------------- */
/* The page                                                                  */
/* ------------------------------------------------------------------------- */

static char *g_html;                /* the page as fetched */
static HtmlDocument *g_doc;         /* the native document of the JS DOM */
static PageFrame g_frame;           /* g_doc laid out */
static bool g_page_is_youtube;

bool page_engine_init(const PageHooks *hooks) {
    if (hooks) g_hooks = *hooks;
    return init_browser_context();
}

void page_engine_load_fonts(void) {
    register_font_table();
}

JSContextHandle page_engine_context(void) {
    return g_ctx;
}

void page_engine_finish(void) {
    finish_browser_context();
}

void page_engine_cleanup(void) {
    cleanup_browser_context();
}

bool page_pump(bool wait) {
    return pump_event_loop(g_ctx, wait);
}

ImageCache *page_image_cache(void) {
    return g_loop_images;
}

PageFrame *page_frame(void) {
    return &g_frame;
}

char *page_serialize(void) {
    GCValue js_doc = get_global_document(g_ctx);
    GCValue doc_elem = JS_GetPropertyStr(g_ctx, js_doc, "documentElement");
    return html_serialize_js_node(g_ctx, doc_elem);
}

void page_prepare(const char *url) {
    const char *start_url = url;
    /* Make the start URL available to the DOM / URL resolution code. */
    g_cyber_start_url = start_url;
    g_page_is_youtube = strstr(start_url, "youtube.") != NULL;

    /* The image cache exists from the start so scripts reading
     * img.naturalWidth / img.complete start fetches early. */
    ImageCache *image_cache = image_cache_create();
    g_loop_images = image_cache;
    display_list_set_image_cache(image_cache);
    /* Image fetched/failed: fire load/error on its <img> elements and re-render. */
    image_cache_set_notify(image_cache, on_image_progress, NULL);

    /* Reflect the start URL in window.location so the engine reports the
     * correct origin, host, and pathname instead of a hardcoded default.
     * Also sync document.domain and document.baseURI to the resolved URL. */
    {
        char loc_script[1024];
        snprintf(loc_script, sizeof(loc_script),
                 "if (window.location) { window.location.href = %s%s%s; "
                 "if (document) { document.domain = window.location.hostname; "
                 "document.baseURI = window.location.href; } }",
                 "\"", start_url, "\"");
        JS_Eval(g_ctx, loc_script, strlen(loc_script), "<set_location>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Enable dynamic <script src> loading before the initial page scripts run.
     * YouTube lazy-loads modules (including player modules and the masthead
     * bootstrap) by creating script elements from JS.  Parser-inserted scripts
     * are marked so they are not executed twice. */
    dom_enable_dynamic_script_loading();
}

bool page_load(void) {
    const char *start_url = g_cyber_start_url;
    const bool page_is_youtube = g_page_is_youtube;
    /* Parse the page into the document as it arrives and run each script
     * where the parser reaches it (HTML's model).  CYBER_STREAM_PARSE=0: parse
     * the whole page, then run every script (the old path). */
    bool scripts_prepared = false;
    HtmlPageStream *page_stream = NULL;
    {
        const char *sp = cyber_getenv("CYBER_STREAM_PARSE");
        if (!(sp && sp[0] == '0')) {
            prepare_page_scripts();
            scripts_prepared = true;
            page_stream = html_page_stream_begin();
        }
    }
    size_t html_size = 0;
    char *html = NULL;
    {
        CP_SCOPE("fetch-start-page");
        html = fetch_start_page(start_url, &html_size, page_stream);
    }
    if (!html) return false;
    g_html = html;

    /* Phase 3: execute page scripts, drain timers, dispatch lifecycle events,
     * and upgrade any custom elements defined during script execution. */
    {
        printf("Executing page scripts ...\n");
        if (!scripts_prepared) prepare_page_scripts();
        JsExecResult js_result;
        bool exec_ok = false;
        if (page_stream) {
            exec_ok = html_page_stream_finish(page_stream, &js_result);
            page_stream = NULL;
        } else {
            CP_SCOPE("execute-page-scripts");
            exec_ok = html_execute_page_scripts(html, &js_result);
        }
        if (exec_ok) {
            printf("Scripts executed: %d captured URLs\n", js_result.captured_url_count);
        } else {
            printf("WARNING: page script execution did not complete successfully\n");
        }

        if (cyber_getenv("CYBER_HOOK_BOOT")) {
            /* Instrument the kevlar boot chain: log ytsignals, the consumption
             * of window.getInitialData, and ytd-app's connectedCallback. */
            const char *hook_js =
                "(function(){"
                "  try {"
                "    var L = window.default_kevlar_base;"
                "    var app0 = document.querySelector('ytd-app');"
                "    console.error('[SIG] hook-time: dkb=' + typeof L + ' LU=' + (L ? typeof L.LU : '-')"
                "      + ' app=' + (app0 ? 'found' : 'NULL')"
                "      + ' gid=' + typeof window.getInitialData);"
                "    if (L) {"
                "      ['jJ','JC'].forEach(function(nm){"
                "        if (typeof L[nm] === 'function') {"
                "          var orig = L[nm];"
                "          L[nm] = function(e) {"
                "            try { console.error('[YTERR:' + nm + '] ' + (e && e.message ? e.message : e) + ' | ' + (e && e.stack ? String(e.stack).split('\\n').slice(0,4).join(' | ') : '')); } catch(x) {}"
                "            try { return orig.apply(this, arguments); } catch(x2) { console.error('[YTERR:' + nm + '] orig threw: ' + (x2 && x2.message)); }"
                "          };"
                "        }"
                "      });"
                "    }"
                "    if (L && L.LU) {"
                "      var inst = L.LU();"
                "      var ps = inst.processSignal;"
                "      inst.processSignal = function(s) {"
                "        console.error('[SIG] processSignal ' + s);"
                "        try { return ps.apply(this, arguments); } catch(e) { console.error('[SIG] ' + s + ' threw: ' + (e && e.message)); throw e; }"
                "      };"
                "    }"
                "    var gid = window.getInitialData;"
                "    Object.defineProperty(window, 'getInitialData', {"
                "      configurable: true,"
                "      get: function() { return gid; },"
                "      set: function(v) { console.error('[SIG] getInitialData set to ' + typeof v); gid = v; }"
                "    });"
                "    var app = document.querySelector('ytd-app');"
                "    if (app) {"
                "      var ccb = app.connectedCallback;"
                "      if (typeof ccb === 'function') {"
                "        app.connectedCallback = function() {"
                "          console.error('[SIG] ytd-app connectedCallback');"
                "          try { return ccb.apply(this, arguments); } catch(e) { console.error('[SIG] ytd-app ccb threw: ' + (e && e.message)); throw e; }"
                "        };"
                "      } else console.error('[SIG] ytd-app has no connectedCallback fn at hook time');"
                "    }"
                "  } catch(e) { console.error('[SIG] hook install failed: ' + (e && e.message)); }"
                "})();";
            GCValue hook_res = JS_Eval(g_ctx, hook_js, strlen(hook_js), "<boot_hook>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(hook_res)) {
                GCValue exc = JS_GetException(g_ctx);
                const char *es = JS_ToCString(g_ctx, exc);
                fprintf(stderr, "[SIG] hook eval EXCEPTION: %s\n", es ? es : "?");
            } else {
                fprintf(stderr, "[SIG] hook eval ok\n");
            }
        }
        pump_due_work(g_ctx);

        /* Neutralise YouTube's error-logging paths before lifecycle timers fire.
         * The helper g.En("yt.logging.errors.log") creates empty objects along
         * the path if it is missing and then calls the result, which throws
         * "not a function" and aborts the app loader.  YouTube-only. */
        if (page_is_youtube) {
            const char *neutralise_log_js2 =
                "(function(){"
                "  try {"
                "    var yt = window.yt || (window.yt = {});"
                "    var logging = yt.logging || (yt.logging = {});"
                "    var errors = logging.errors || (logging.errors = {});"
                "    if (typeof errors.log !== 'function') { errors.log = function(){}; console.error('[NEUTRAL-EARLY] yt.logging.errors.log'); }"
                "    if (typeof errors.warn !== 'function') { errors.warn = function(){}; }"
                "  } catch(e) {}"
                "  if (window.ytcsi && typeof window.ytcsi.tick === 'function') { window.ytcsi.tick = function(){}; console.error('[NEUTRAL-EARLY] ytcsi.tick'); }"
                "})();";
            JS_Eval(g_ctx, neutralise_log_js2, strlen(neutralise_log_js2), "<neutralise_log_early>", JS_EVAL_TYPE_GLOBAL);
        }

        // Reclaim handles allocated by script execution before dispatching
        // lifecycle events.
        {
            CP_SCOPE("run-gc");
            if (gc_is_background()) gc_request_cycle(true);
            else JS_RunGC(JS_GetRuntime(g_ctx));
        }

        printf("Dispatching DOMContentLoaded and load events ...\n");
        page_mark("scripts done");
        {
            CP_SCOPE("lifecycle-events");
            dispatch_page_lifecycle_events(g_ctx);
        }
        {
            CP_SCOPE("pump-after-lifecycle");
            pump_due_work(g_ctx);
        }

        // Upgrade any custom elements that were defined during script execution.
        // This lets Polymer run connectedCallback and stamp shadow-DOM content
        // on the existing server-rendered skeleton.
        {
            CP_SCOPE("upgrade-custom-elements");
            fprintf(stderr, "[UPGRADE-DOC] invoking customElements.upgrade\n");
            fflush(stderr);

            const char *upgrade_doc_js =
                "if (window.customElements && typeof window.customElements.upgrade === 'function' && document) {"
                "  var app = document.querySelector('ytd-app');"
                "  try { window.customElements.upgrade(app || document.documentElement); } catch(e) {}"
                "} else {"
                "  var log = (typeof __bgmdwnldr_log !== 'undefined') ? __bgmdwnldr_log : null;"
                "  if (log) try { log('[UPGRADE-DOC] no upgrade function'); } catch(x) {}"
                "}";
            {
                /* Bound the upgrade pass: it can hard-loop on pathological DOMs;
                 * on timeout, log the stack to find the loop. */
                struct upgrade_timeout_state uts;
                clock_gettime(CLOCK_MONOTONIC, &uts.start);
                uts.limit = 60.0;
                JS_SetInterruptHandler(JS_GetRuntime(g_ctx), upgrade_timeout_handler, &uts);
                GCValue ures = JS_Eval(g_ctx, upgrade_doc_js, strlen(upgrade_doc_js), "<upgrade_doc>", JS_EVAL_TYPE_GLOBAL);
                JS_SetInterruptHandler(JS_GetRuntime(g_ctx), NULL, NULL);
                if (JS_IsException(ures)) {
                    GCValue exc = JS_GetException(g_ctx);
                    const char *es = JS_ToCString(g_ctx, exc);
                    fprintf(stderr, "[UPGRADE-DOC] eval EXCEPTION: %s\n", es ? es : "?");
                    GCValue stk = JS_GetPropertyStr(g_ctx, exc, "stack");
                    const char *ss = JS_ToCString(g_ctx, stk);
                    if (ss) fprintf(stderr, "[UPGRADE-DOC] stack:\n%s\n", ss);
                    fflush(stderr);
                }
            }
            fprintf(stderr, "[UPGRADE-DOC] done\n");
            fflush(stderr);

            /* Everything from here to the end of this scope is YouTube-app
             * bootstrapping: neutralising yt.logging.errors, triggering the
             * 'script-load-dpj' app loader, re-upgrading ytd-app, and firing
             * the ytsignals "ci" signal.  Skip it entirely for non-YouTube
             * pages. */
            if (page_is_youtube) {
            /* YouTube's error-logging wrappers (_.JC) catch exceptions and forward
             * them to yt.logging.errors.log.  In the emulator the logging path
             * itself throws ("not a function"), so real errors get lost and
             * timers/appLoad abort.  Neutralise the global loggers so wrapped
             * callbacks fail gracefully. */
            {
                const char *neutralise_log_js =
                    "(function(){"
                    "  var ytle = window.yt && window.yt.logging && window.yt.logging.errors;"
                    "  if (ytle) {"
                    "    if (typeof ytle.log === 'function') { ytle.log = function(){}; console.error('[NEUTRAL] yt.logging.errors.log'); }"
                    "    if (typeof ytle.warn === 'function') { ytle.warn = function(){}; }"
                    "  }"
                    "  if (window.ytcsi && typeof window.ytcsi.tick === 'function') {"
                    "    window.ytcsi.tick = function(){}; console.error('[NEUTRAL] ytcsi.tick');"
                    "  }"
                    "})();";
                JS_Eval(g_ctx, neutralise_log_js, strlen(neutralise_log_js), "<neutralise_log>", JS_EVAL_TYPE_GLOBAL);
            }

            /* YouTube's app loader normally fires on a 'script-load-dpj' event
             * or ytsignals callback.  In our emulator those signals are missing,
             * so the app stays in its disable-upgrade skeleton state.  Manually
             * run the loader to remove disable-upgrade and let ytd-app stamp. */
            const char *app_load_js =
                "console.error('[APP-LOAD] typeof=' + typeof appLoad + ' window=' + typeof window.appLoad + ' schedule=' + typeof scheduleAppLoad);"
                "var cp = window.yt && window.yt.player && window.yt.player.Application && (window.yt.player.Application.createAlternate || window.yt.player.Application.create);"
                "console.error('[APP-LOAD] createPlayer typeof=' + typeof cp);"
                "var __cyber_old_yterr = window.yterr; window.yterr = false;"
                "if (typeof appLoad === 'function') { try { appLoad(); console.error('[APP-LOAD] triggered'); } catch(e) { console.error('[APP-LOAD] err', e.message, e.stack); } }"
                "else if (window.appLoad && typeof window.appLoad === 'function') { try { window.appLoad(); console.error('[APP-LOAD] triggered via window'); } catch(e) { console.error('[APP-LOAD] err', e.message, e.stack); } }"
                "else { console.error('[APP-LOAD] not found'); }"
                "window.yterr = __cyber_old_yterr;";
            JS_Eval(g_ctx, app_load_js, strlen(app_load_js), "<app_load>", JS_EVAL_TYPE_GLOBAL);

            /* Re-upgrade now that disable-upgrade has been removed. */
            {
                const char *reupgrade_js =
                    "if (window.customElements && typeof window.customElements.upgrade === 'function') {"
                    "  var app = document.querySelector('ytd-app');"
                    "  try { window.customElements.upgrade(app || document.documentElement); } catch(e) {}"
                    "}";
                JS_Eval(g_ctx, reupgrade_js, strlen(reupgrade_js), "<reupgrade>", JS_EVAL_TYPE_GLOBAL);
            }

            /* The app's rendering phase is gated on the ytsignals "ci" signal,
             * fired by the app element's Polymer attached() callback, which is
             * delivered now.  Firing it from here as well came too early: when
             * it worked (it usually threw), the watch page stopped half built
             * (~2,200 nodes).  CYBER_FIRE_CI=1 still fires it. */
            if (cyber_getenv("CYBER_FIRE_CI")) {
                const char *fire_ci_js =
                    "(function(){"
                    "  try {"
                    "    var L = window.default_kevlar_base;"
                    "    if (L && L.LU) { L.LU().processSignal('ci'); console.error('[CI] fired via dkb'); }"
                    "    else if (window.ytsignals && typeof window.ytsignals.getInstance === 'function') {"
                    "      var inst = window.ytsignals.getInstance();"
                    "      if (inst && typeof inst.processSignal === 'function') { inst.processSignal('ci'); console.error('[CI] fired via ytsignals'); }"
                    "      else console.error('[CI] no processSignal');"
                    "    } else console.error('[CI] no signals API');"
                    "  } catch(e) { console.error('[CI] fire failed: ' + (e && e.message)); }"
                    "})();";
                JS_Eval(g_ctx, fire_ci_js, strlen(fire_ci_js), "<fire_ci>", JS_EVAL_TYPE_GLOBAL);
                pump_due_work(g_ctx);
            }
            }
            }

        if (cyber_getenv("CYBER_DIAG_SIGNALS")) {
            const char *diag_js =
                "(function(){"
                "  var pm = document.querySelector('ytd-page-manager');"
                "  console.error('[DIAG] pm children=' + (pm ? pm.childElementCount : -1));"
                "  var ytdapp = document.querySelector('ytd-app');"
                "  console.error('[DIAG] ytd-app inst=' + (ytdapp && ytdapp.inst ? 'yes' : 'no'));"
                "  if (ytdapp) { try { console.error('[DIAG] ytd-app ownProps=' + Object.getOwnPropertyNames(ytdapp).filter(function(n){return n[0]!='_';}).slice(0,40).join(',')); } catch(e){console.error('[DIAG] ownProps err');} }"
                "  if (ytdapp) { console.error('[DIAG] polymerController=' + (ytdapp.polymerController ? 'yes' : 'no') + ' controllerProxy=' + (ytdapp.controllerProxy ? 'yes' : 'no')); }"
                "  var sig = (window.ytsignals && window.ytsignals.getInstance) ? window.ytsignals.getInstance() : null;"
                "  console.error('[DIAG] ytsignals inst=' + (sig ? 'yes' : 'no'));"
                "  try { console.error('[DIAG] Promise native=' + (function(){ var s = ''+Promise.resolve; return s.indexOf('native') >= 0; })() + ' Promise.name=' + Promise.name + ' mm?=' + (window.default_kevlar_base && window.default_kevlar_base.mm ? 'y' : '?')); } catch(e) { console.error('[DIAG] promise check err'); }"
                "  try { var rid = window.requestAnimationFrame(function(){}); console.error('[DIAG] RAF src=' + (''+window.requestAnimationFrame).slice(0,90) + ' returnedId=' + rid); } catch(e) { console.error('[DIAG] RAF check err ' + e.message); }"
                "  try { console.error('[DIAG] Promise str=' + (''+Promise).slice(0,120)); console.error('[DIAG] Promise===mm:' + (window.default_kevlar_base && window.default_kevlar_base.mm ? (Promise === window.default_kevlar_base.mm) : 'n/a')); } catch(e) {}"
                "  if (sig && sig.processSignal) { sig.processSignal('ci'); console.error('[DIAG] fired ci via ytsignals'); }"
                "})();";
            JS_Eval(g_ctx, diag_js, strlen(diag_js), "<diag_signals>", JS_EVAL_TYPE_GLOBAL);
            pump_timers_and_jobs(g_ctx);
            const char *diag2_js =
                "(function(){"
                "  var pm = document.querySelector('ytd-page-manager');"
                "  console.error('[DIAG] after ci: pm children=' + (pm ? pm.childElementCount : -1));"
                "  var w = document.querySelectorAll('ytd-watch-flexy');"
                "  console.error('[DIAG] ytd-watch-flexy count=' + w.length);"
                "})();";
            JS_Eval(g_ctx, diag2_js, strlen(diag2_js), "<diag_signals2>", JS_EVAL_TYPE_GLOBAL);
            pump_timers_and_jobs(g_ctx);
            const char *diag3_js =
                "(function(){"
                "  var sig = (window.ytsignals && window.ytsignals.getInstance) ? window.ytsignals.getInstance() : null;"
                "  if (sig && sig.processSignal) { sig.processSignal('eocs'); sig.processSignal('eor'); console.error('[DIAG] fired eocs+eor via ytsignals'); }"
                "})();";
            JS_Eval(g_ctx, diag3_js, strlen(diag3_js), "<diag_signals3>", JS_EVAL_TYPE_GLOBAL);
            pump_timers_and_jobs(g_ctx);
            const char *diag4_js =
                "(function(){"
                "  console.error('[DIAG4] loadInitialData=' + typeof window.loadInitialData + ' getInitialData=' + typeof window.getInitialData + ' getDataPromise=' + typeof window.getDataPromise);"
                "  var app = document.querySelector('ytd-app');"
                "  var pc = app && app.polymerController;"
                "  console.error('[DIAG4] pc.data=' + (pc && pc.data ? 'yes' : 'no') + ' pc.root=' + (pc && pc.root ? 'yes' : 'no') + ' pc.loadData=' + (pc ? typeof pc.loadData : 'n/a'));"
                "  console.error('[DIAG4] pm pc=' + (pm && pm.polymerController ? 'yes' : 'no') + ' pm.isConnected=' + (pm && pm.isConnected) + ' pm.shadowRoot=' + (pm && pm.shadowRoot ? 'yes' : 'no') + ' pm.__shady=' + (pm && pm.__shady ? 'yes' : 'no'));"
                "  var allpm = document.querySelectorAll('ytd-page-manager');"
                "  console.error('[DIAG4] pm count=' + allpm.length);"
                "  var realpm = allpm[0];"
                "  if (realpm) { var pp = window.__cyber_eventComposedPath ? window.__cyber_eventComposedPath(realpm) : []; console.error('[DIAG4] pm path len=' + pp.length + ' tags=' + pp.map(function(n){return n.tagName||('#'+n.nodeType);}).slice(0,12).join('>')); }"
                "  var app2 = document.querySelector('ytd-app');"
                "  if (app2) { var la = app2['__listeners_attached']; console.error('[DIAG4] app listeners_attached=' + (la ? la.length : 'none')); }"
                "  for (var pmi = 0; pmi < allpm.length && pmi < 6; pmi++) { var pX = allpm[pmi]; console.error('[DIAG4] pm['+pmi+'] parent=' + (pX.parentNode ? pX.parentNode.tagName : 'null') + ' conn=' + pX.isConnected + ' pc=' + (pX.polymerController?'yes':'no') + ' kids=' + pX.childElementCount); }"
                "  console.error('[DIAG4] pm parent=' + (pm && pm.parentNode && pm.parentNode.tagName) + ' rootNode=' + (pm && pm.getRootNode && pm.getRootNode().nodeType));"
                "  var pm = document.querySelector('ytd-page-manager');"
                "  if (pm && pm.pagePool) {"
                "    try { var r = pm.preparePage('watch'); console.error('[DIAG4] preparePage ran'); } catch(e){ console.error('[DIAG4] preparePage threw: ' + (e && e.message)); }"
                "    var w = pm.pagePool.pageNameToElement.get('watch');"
                "    console.error('[DIAG4] after preparePage: watch el=' + (w ? 'yes' : 'no') + ' isAttached=' + (w && w.isAttached));"
                "  }"
                "})();";
            GCValue d4 = JS_Eval(g_ctx, diag4_js, strlen(diag4_js), "<diag_signals4>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(d4)) {
                GCValue exc = JS_GetException(g_ctx);
                const char *es = JS_ToCString(g_ctx, exc);
                fprintf(stderr, "[DIAG4] eval exception: %s\n", es ? es : "?");
                fflush(stderr);
            }
            pump_timers_and_jobs(g_ctx);
        }

        // ytInitialData injection removed: the page is expected to fetch its
        // own initial data through standard fetch/XHR now.
    }

    /* The JS DOM populated by html_execute_page_scripts is now the source of
     * truth.  Mark it dirty so the quiescence loop performs an initial layout. */
    dom_request_layout();

    if (g_hooks.loaded) g_hooks.loaded();
    image_api_scan(g_ctx);
    return true;
}

void page_close(void) {
    page_frame_free(&g_frame);
    if (g_doc) html_document_free(g_doc);
    g_doc = NULL;
    display_list_set_image_cache(NULL);
    image_cache_destroy(g_loop_images);
    g_loop_images = NULL;
    free(g_html);
    g_html = NULL;
}

void page_turn(bool run_timers, bool wait, int width, int height, PageTurn *out) {
    bool had_timers = false;
    bool had_images = false;
    out->new_frame = false;
    {
        CP_SCOPE("pump-timers");
        if (run_timers) had_timers = pump_event_loop(g_ctx, wait);
    }
    {
        CP_SCOPE("process-images");
        had_images = image_cache_process_pending(g_loop_images);
    }
    if (g_dom_needs_layout) {
        g_dom_needs_layout = 0;

        GCValue js_doc = get_global_document(g_ctx);
        image_api_scan(g_ctx);

        HtmlDocument *new_doc = NULL;
        {
            CP_SCOPE("rebuild-doc");
            new_doc = html_document_from_js_dom(g_ctx, js_doc);
        }
        if (new_doc) {
            page_frame_free(&g_frame);   /* it refers into the old document */
            if (g_doc) html_document_free(g_doc);
            g_doc = new_doc;

            /* Print metadata from the rebuilt native document. */
            const char *title = get_title_text(g_doc);
            printf("Title: %s\n", title ? title : "(none)");
            printf("DOM nodes: %d\n", g_doc->array.count);
            print_body_snippet(g_doc);

            /* CSS is applied by the layout engine (css_layout_run) which
             * has its own complete stylesheet collection + selector matching
             * pipeline.  No separate CSS application step is needed here. */

            page_frame_build(&g_frame, g_doc, g_loop_images, width, height);
            out->new_frame = g_frame.valid;
        } else {
            printf("WARNING: failed to rebuild native document from JS DOM\n");
        }
    }
    out->had_timers = had_timers;
    out->had_images = had_images;
}

void page_request_layout(void) {
    dom_request_layout();
}

GCValue dom_element_at(JSContextHandle ctx, LayoutContext *layout, double x, double y);

GCValue page_element_at(double x, double y) {
    if (!g_frame.valid) return JS_NULL;
    return dom_element_at(g_ctx, &g_frame.layout, x, y);
}

const char *page_title(void) {
    return g_doc ? get_title_text(g_doc) : NULL;
}

bool page_needs_layout(void) {
    return g_dom_needs_layout != 0;
}

void page_wait_for_work(unsigned long long quiet_from, unsigned long long quiet_ms) {
    CP_SCOPE("wait-images");
    if (js_quickjs_network_pending() > 0) js_quickjs_net_wait(50);
    else if (virtual_time_idle()) {
        /* Nothing in flight: skip ahead to the next timer, or to the end
         * of the quiet window. */
        unsigned long long now = page_time_ms();
        unsigned long long target = quiet_ms ? quiet_from + quiet_ms : now + 100;
        unsigned long long next = timer_next_due_ms();
        if (next != (unsigned long long)-1 && next < target) target = next;
        page_time_advance(target > now ? target - now : 1);
    } else image_cache_wait_progress(g_loop_images, 100);
}
