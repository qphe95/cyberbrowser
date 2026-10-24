/*
 * CyberBrowser - minimal browser engine smoke-test
 *
 * Fetches a start page, parses the HTML, runs the CSS layout engine, builds a
 * display list, renders a wireframe to a JPEG, and prints a summary.  This is
 * intended as a quick smoke-test executable for the cyberbrowser core.
 */
#include "cyber_env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
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

/* stb_image_write implementation lives in cyber_profile.c (library side). */
#include "stb_image_write.h"
#include "session_state.h"
#include "html_media_extract.h"
#include "dom_fetch.h"

int timer_process_due(JSContextHandle ctx);
int scheduler_process_tasks(JSContextHandle ctx);
void timer_set_idle_deadline(unsigned long long deadline_ms);
unsigned long long timer_next_due_ms(void);
void platform_sleep_ms(unsigned int ms);

#define LOG_TAG "cyberbrowser"

/* Default page loaded by the smoke-test executable.  Can be overridden by
 * argv[1] or the CYBER_START_URL environment variable. */
static const char DEFAULT_START_URL[] = "https://en.wikipedia.org/wiki/Main_Page";
extern const char *g_cyber_start_url;

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

    /* Compile the source as a module.  JS_Eval with JS_EVAL_TYPE_MODULE parses
     * and registers the module, returning a JS_TAG_MODULE value whose handle
     * we extract and return. */
    GCValue mv = JS_Eval(ctx, buffer.data, buffer.size, url, JS_EVAL_TYPE_MODULE);
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
    if (!SAFE_VALID(m)) {
        platform_log(LOG_LEVEL_WARN, "module_loader", "Module %s did not produce a def", url);
        JS_ThrowReferenceError(ctx, "module '%s' produced no definition", url);
        return (JSModuleDefHandle)(GC_HANDLE_NULL);
    }
    return m;
}

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

static JSRuntimeHandle g_rt = GC_HANDLE_NULL;
static JSContextHandle g_ctx = GC_HANDLE_NULL;
static GCValue g_global = GC_UNDEFINED_INIT;

/* Interrupt handler for the bounded customElements.upgrade pass. */
struct upgrade_timeout_state { struct timespec start; double limit; };
int upgrade_timeout_handler(JSRuntimeHandle rt, void *opaque) {
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

static void save_html(const char *html, size_t html_size) {
    FILE *f = fopen("page_loaded.html", "wb");
    if (!f) return;
    fwrite(html, 1, html_size, f);
    fclose(f);
    printf("Saved fetched HTML to page_loaded.html (%zu bytes)\n", html_size);
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

static const char *str_case_find(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0) return haystack;
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, n) == 0) return p;
    }
    return NULL;
}

static void extract_title_from_html(const char *html, size_t html_len, char *out, size_t out_len) {
    (void)html_len;
    out[0] = '\0';
    const char *start = str_case_find(html, "<title>");
    if (!start) start = str_case_find(html, "<title ");
    if (!start) return;
    start = strchr(start, '>');
    if (!start) return;
    start++;
    const char *end = str_case_find(start, "</title>");
    if (!end) return;
    size_t len = (size_t)(end - start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    /* trim whitespace */
    char *p = out;
    char *q = out + strlen(out);
    while (q > p && isspace((unsigned char)q[-1])) *(--q) = '\0';
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != out) memmove(out, p, strlen(p) + 1);
}

static int count_substrings(const char *haystack, const char *needle) {
    int count = 0;
    const char *p = haystack;
    size_t n = strlen(needle);
    if (n == 0) return 0;
    while ((p = strstr(p, needle)) != NULL) {
        count++;
        p += n;
    }
    return count;
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
        "window.dispatchEvent(dcl);"
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

/* ------------------------------------------------------------------------- */
/* Simple software rasterizer for the display list -> JPEG wireframe         */
/* ------------------------------------------------------------------------- */

#define WIREFRAME_WIDTH  1024
#define WIREFRAME_HEIGHT 2400

typedef struct {
    uint8_t r, g, b;
} RGB;

static inline void set_pixel(RGB *pixels, int width, int height, int x, int y, RGB c) {
    if (x < 0 || y < 0 || x >= width || y >= height) return;
    pixels[y * width + x] = c;
}

static inline RGB blend_over(RGB dst, float sr, float sg, float sb, float sa) {
    if (sa <= 0.0f) return dst;
    if (sa >= 1.0f) {
        RGB r = { (uint8_t)(sr * 255.0f + 0.5f),
                  (uint8_t)(sg * 255.0f + 0.5f),
                  (uint8_t)(sb * 255.0f + 0.5f) };
        return r;
    }
    RGB out;
    out.r = (uint8_t)(dst.r * (1.0f - sa) + sr * 255.0f * sa + 0.5f);
    out.g = (uint8_t)(dst.g * (1.0f - sa) + sg * 255.0f * sa + 0.5f);
    out.b = (uint8_t)(dst.b * (1.0f - sa) + sb * 255.0f * sa + 0.5f);
    return out;
}

static void fill_rect(RGB *pixels, int width, int height,
                      int x0, int y0, int x1, int y1,
                      float r, float g, float b, float a) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= width) x1 = width - 1;
    if (y1 >= height) y1 = height - 1;
    for (int y = y0; y <= y1; y++) {
        RGB *row = &pixels[y * width];
        for (int x = x0; x <= x1; x++) {
            row[x] = blend_over(row[x], r, g, b, a);
        }
    }
}

static void draw_hline(RGB *pixels, int width, int height,
                       int x0, int x1, int y,
                       float r, float g, float b, float a) {
    if (y < 0 || y >= height) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= width) x1 = width - 1;
    RGB *row = &pixels[y * width];
    for (int x = x0; x <= x1; x++) {
        row[x] = blend_over(row[x], r, g, b, a);
    }
}

static void draw_vline(RGB *pixels, int width, int height,
                       int x, int y0, int y1,
                       float r, float g, float b, float a) {
    if (x < 0 || x >= width) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= height) y1 = height - 1;
    for (int y = y0; y <= y1; y++) {
        RGB *p = &pixels[y * width + x];
        *p = blend_over(*p, r, g, b, a);
    }
}

static void draw_rect_outline(RGB *pixels, int width, int height,
                              int x0, int y0, int x1, int y1,
                              float r, float g, float b, float a) {
    draw_hline(pixels, width, height, x0, x1, y0, r, g, b, a);
    draw_hline(pixels, width, height, x0, x1, y1, r, g, b, a);
    draw_vline(pixels, width, height, x0, y0, y1, r, g, b, a);
    draw_vline(pixels, width, height, x1, y0, y1, r, g, b, a);
}

static void draw_glyph(RGB *pixels, int img_width, int img_height,
                       const uint8_t *atlas, int atlas_w, int atlas_h,
                       const DisplayListCmd *cmd)
{
    /* Resample the atlas region into the destination rect (cmd->x/y/w/h) so
     * glyphs render at the scaled font size, not the 16px atlas size.
     * Sampling at pixel centers keeps the nearest-neighbor lookup stable. */
    if (cmd->w <= 0.0f || cmd->h <= 0.0f) return;
    int dx0 = (int)floorf(cmd->x);
    int dy0 = (int)floorf(cmd->y);
    int dx1 = (int)ceilf(cmd->x + cmd->w);
    int dy1 = (int)ceilf(cmd->y + cmd->h);
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > img_width) dx1 = img_width;
    if (dy1 > img_height) dy1 = img_height;
    if (dx0 >= dx1 || dy0 >= dy1) return;

    for (int dy = dy0; dy < dy1; dy++) {
        float v = (dy + 0.5f - cmd->y) / cmd->h;
        float av = cmd->u.glyph.v0 + v * (cmd->u.glyph.v1 - cmd->u.glyph.v0);
        int sy = (int)floorf(av * (float)atlas_h);
        if (sy < 0) sy = 0;
        if (sy >= atlas_h) sy = atlas_h - 1;
        for (int dx = dx0; dx < dx1; dx++) {
            float u = (dx + 0.5f - cmd->x) / cmd->w;
            float au = cmd->u.glyph.u0 + u * (cmd->u.glyph.u1 - cmd->u.glyph.u0);
            int sx = (int)floorf(au * (float)atlas_w);
            if (sx < 0) sx = 0;
            if (sx >= atlas_w) sx = atlas_w - 1;
            uint8_t a8 = atlas[sy * atlas_w + sx];
            if (a8 == 0) continue;
            float alpha = (a8 / 255.0f) * cmd->a;
            pixels[dy * img_width + dx] = blend_over(pixels[dy * img_width + dx],
                                                      cmd->r, cmd->g, cmd->b, alpha);
        }
    }
}

static void draw_image(RGB *pixels, int img_width, int img_height,
                       ImageCache *cache, const DisplayListCmd *cmd)
{
    int src_w = 0, src_h = 0, src_ch = 0;
    uint8_t *src = NULL;
    if (!image_cache_get(cache, cmd->u.image.image_handle,
                         &src_w, &src_h, &src_ch, &src)) {
        return;
    }
    if (src_w <= 0 || src_h <= 0) return;

    int dx0 = (int)floorf(cmd->x);
    int dy0 = (int)floorf(cmd->y);
    int dx1 = (int)floorf(cmd->x + cmd->w);
    int dy1 = (int)floorf(cmd->y + cmd->h);
    if (dx0 < 0) dx0 = 0;
    if (dy0 < 0) dy0 = 0;
    if (dx1 > img_width) dx1 = img_width;
    if (dy1 > img_height) dy1 = img_height;
    if (dx0 >= dx1 || dy0 >= dy1) return;

    for (int dy = dy0; dy < dy1; dy++) {
        float v = (cmd->h > 0.0f) ? ((float)(dy - dy0) / cmd->h) : 0.0f;
        int sy = (int)floorf(v * (float)src_h);
        if (sy < 0) sy = 0;
        if (sy >= src_h) sy = src_h - 1;
        for (int dx = dx0; dx < dx1; dx++) {
            float u = (cmd->w > 0.0f) ? ((float)(dx - dx0) / cmd->w) : 0.0f;
            int sx = (int)floorf(u * (float)src_w);
            if (sx < 0) sx = 0;
            if (sx >= src_w) sx = src_w - 1;
            int si = (sy * src_w + sx) * 4;
            uint8_t a = src[si + 3];
            if (a == 0) continue;
            pixels[dy * img_width + dx] = blend_over(pixels[dy * img_width + dx],
                                                     src[si] / 255.0f,
                                                     src[si + 1] / 255.0f,
                                                     src[si + 2] / 255.0f,
                                                     a / 255.0f);
        }
    }
}

/* Coverage of a rounded rectangle at (px, py): inside unless in a corner's
 * quadrant and outside its ellipse (about one pixel of anti-aliasing). */
static float rrect_alpha(float x, float y, float w, float h, const float *rx, const float *ry,
                         float px, float py)
{
    float lx = px - x, ly = py - y;
    if (w <= 0.0f || h <= 0.0f || lx < 0.0f || ly < 0.0f || lx > w || ly > h) return 0.0f;
    int c = -1;
    float cx = 0, cy = 0;
    if (lx < rx[0] && ly < ry[0]) { c = 0; cx = rx[0]; cy = ry[0]; }
    else if (lx > w - rx[1] && ly < ry[1]) { c = 1; cx = w - rx[1]; cy = ry[1]; }
    else if (lx > w - rx[2] && ly > h - ry[2]) { c = 2; cx = w - rx[2]; cy = h - ry[2]; }
    else if (lx < rx[3] && ly > h - ry[3]) { c = 3; cx = rx[3]; cy = h - ry[3]; }
    if (c < 0 || rx[c] <= 0.0f || ry[c] <= 0.0f) return 1.0f;
    float nx = (lx - cx) / rx[c], ny = (ly - cy) / ry[c];
    float d = sqrtf(nx * nx + ny * ny);
    float r = rx[c] < ry[c] ? rx[c] : ry[c];
    float a = 0.5f - (d - 1.0f) * r;
    return a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
}

/* Mask alpha at pixel centre (px, py): a linear gradient along the CSS
 * angle through the box, or an image's alpha (src: w x h x ch, NULL while
 * not decoded = transparent). */
static float mask_alpha_at(const DisplayMask *m, float px, float py,
                           const uint8_t *src, int sw, int sh, int sch)
{
    if (m->kind == 2) {
        float rad = m->angle_deg * 3.14159265f / 180.0f;
        float dx = sinf(rad), dy = -cosf(rad);
        float line = fabsf(m->w * dx) + fabsf(m->h * dy);
        if (line <= 0.0f) return 1.0f;
        float t = ((px - (m->x + m->w * 0.5f)) * dx + (py - (m->y + m->h * 0.5f)) * dy) / line + 0.5f;
        if (t <= m->stop_pos[0]) return m->stop_alpha[0];
        for (int k = 1; k < m->nstops; k++) {
            if (t <= m->stop_pos[k]) {
                float span = m->stop_pos[k] - m->stop_pos[k - 1];
                float f = span > 0.0f ? (t - m->stop_pos[k - 1]) / span : 1.0f;
                return m->stop_alpha[k - 1] + (m->stop_alpha[k] - m->stop_alpha[k - 1]) * f;
            }
        }
        return m->stop_alpha[m->nstops - 1];
    }
    if (m->kind == 3)
        return rrect_alpha(m->x, m->y, m->w, m->h, m->rx, m->ry, px, py);
    if (m->kind == 4) {
        /* Border ring: the rounded box minus its inset by the border
         * widths (inner radii shrink by the adjacent widths). */
        const float *b = m->border;   /* top, right, bottom, left */
        float irx[4] = { m->rx[0] - b[3], m->rx[1] - b[1], m->rx[2] - b[1], m->rx[3] - b[3] };
        float iry[4] = { m->ry[0] - b[0], m->ry[1] - b[0], m->ry[2] - b[2], m->ry[3] - b[2] };
        for (int k = 0; k < 4; k++) { if (irx[k] < 0.0f) irx[k] = 0.0f; if (iry[k] < 0.0f) iry[k] = 0.0f; }
        float outer = rrect_alpha(m->x, m->y, m->w, m->h, m->rx, m->ry, px, py);
        float inner = rrect_alpha(m->x + b[3], m->y + b[0], m->w - b[1] - b[3], m->h - b[0] - b[2], irx, iry, px, py);
        return outer * (1.0f - inner);
    }
    if (!src || sw <= 0 || sh <= 0 || m->iw <= 0.0f || m->ih <= 0.0f) return 0.0f;
    float u = (px - m->ix) / m->iw, v = (py - m->iy) / m->ih;
    if (m->repeat) { u -= floorf(u); v -= floorf(v); }
    else if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) return 0.0f;
    int sx = (int)(u * (float)sw), sy = (int)(v * (float)sh);
    if (sx >= sw) sx = sw - 1;
    if (sy >= sh) sy = sh - 1;
    if (sch < 4) return 1.0f;
    return (float)src[((size_t)sy * sw + sx) * sch + 3] / 255.0f;
}

typedef struct {
    int x0, y0, x1, y1;
    RGB *saved;
    int mask;
} MaskLayer;

static bool render_display_list_to_jpg(const DisplayList *dl, const char *path,
                                       int img_width, int img_height) {
    MaskLayer layers[64];
    int nlayers = 0;
    size_t pixel_count = (size_t)img_width * (size_t)img_height;
    RGB *pixels = (RGB *)calloc(pixel_count, sizeof(RGB));
    if (!pixels) {
        printf("FATAL: out of memory allocating %dx%d image buffer\n", img_width, img_height);
        return false;
    }

    /* White background. */
    for (size_t i = 0; i < pixel_count; i++) {
        pixels[i].r = 255;
        pixels[i].g = 255;
        pixels[i].b = 255;
    }

    /* Per-slot atlases (font_registry.h), resolved when a glyph first
     * references the slot. */
    TextShaper *slot_fonts[FONT_SLOTS_MAX] = {0};
    const uint8_t *slot_atlas[FONT_SLOTS_MAX] = {0};
    int slot_atlas_w[FONT_SLOTS_MAX] = {0};
    int slot_atlas_h[FONT_SLOTS_MAX] = {0};

    for (int i = 0; i < dl->count; i++) {
        const DisplayListCmd *cmd = &dl->cmds[i];
        int x0 = (int)floorf(cmd->x);
        int y0 = (int)floorf(cmd->y);
        int x1 = (int)floorf(cmd->x + cmd->w);
        int y1 = (int)floorf(cmd->y + cmd->h);

        if (cmd->type == DL_RECT) {
            fill_rect(pixels, img_width, img_height,
                      x0, y0, x1, y1,
                      cmd->r, cmd->g, cmd->b, cmd->a);
        } else if (cmd->type == DL_BORDER) {
            float thickness = cmd->u.border.thickness;
            if (thickness <= 0) thickness = 1.0f;
            int t = (int)ceilf(thickness);
            for (int offset = 0; offset < t; offset++) {
                draw_rect_outline(pixels, img_width, img_height,
                                  x0 + offset, y0 + offset,
                                  x1 - offset, y1 - offset,
                                  cmd->r, cmd->g, cmd->b, cmd->a);
            }
        } else if (cmd->type == DL_GLYPH) {
            int slot = cmd->u.glyph.font_slot;
            if (slot < FONT_SLOTS_MAX) {
                if (!slot_fonts[slot]) {
                    slot_fonts[slot] = font_registry_slot(slot);
                    slot_atlas[slot] = slot_fonts[slot]
                        ? text_shaper_atlas_pixels(slot_fonts[slot]) : NULL;
                    slot_atlas_w[slot] = slot_fonts[slot]
                        ? text_shaper_atlas_width(slot_fonts[slot]) : 0;
                    slot_atlas_h[slot] = slot_fonts[slot]
                        ? text_shaper_atlas_height(slot_fonts[slot]) : 0;
                }
                if (slot_atlas[slot]) {
                    draw_glyph(pixels, img_width, img_height,
                               slot_atlas[slot], slot_atlas_w[slot],
                               slot_atlas_h[slot], cmd);
                }
            }
        } else if (cmd->type == DL_IMAGE) {
            ImageCache *cache = display_list_get_image_cache();
            if (cache) draw_image(pixels, img_width, img_height, cache, cmd);
        } else if (cmd->type == DL_SHADOW) {
            /* Blurred rounded rectangle: coverage from the signed distance
             * to its edge through a Gaussian (sigma = blur / 2), minus the
             * part under the box that casts it. */
            const DisplayMask *m = &dl->masks[cmd->u.mask_index];
            int sx0 = (int)floorf(cmd->x), sy0 = (int)floorf(cmd->y);
            int sx1 = (int)ceilf(cmd->x + cmd->w), sy1 = (int)ceilf(cmd->y + cmd->h);
            if (sx0 < 0) sx0 = 0;
            if (sy0 < 0) sy0 = 0;
            if (sx1 > img_width) sx1 = img_width;
            if (sy1 > img_height) sy1 = img_height;
            float sigma = m->blur * 0.5f;
            float hw = m->w * 0.5f, hh = m->h * 0.5f, cx = m->x + hw, cy = m->y + hh;
            for (int y = sy0; y < sy1; y++) {
                for (int x = sx0; x < sx1; x++) {
                    float px = (float)x + 0.5f, py = (float)y + 0.5f;
                    int c = px < cx ? (py < cy ? 0 : 3) : (py < cy ? 1 : 2);
                    float r = m->rx[c] < m->ry[c] ? m->rx[c] : m->ry[c];
                    if (r > hw) r = hw;
                    if (r > hh) r = hh;
                    float qx = fabsf(px - cx) - hw + r, qy = fabsf(py - cy) - hh + r;
                    float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
                    float d = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0.0f ? qx : 0.0f) : (qy < 0.0f ? qy : 0.0f)) - r;
                    float cov = sigma > 0.01f ? 0.5f * erfcf(d / (sigma * 1.41421356f))
                                              : (d <= -0.5f ? 1.0f : d >= 0.5f ? 0.0f : 0.5f - d);
                    cov *= 1.0f - rrect_alpha(m->ix, m->iy, m->iw, m->ih, m->ex_rx, m->ex_ry, px, py);
                    float a = cov * cmd->a;
                    if (a <= 0.002f) continue;
                    RGB *p = &pixels[(size_t)y * img_width + x];
                    p->r = (uint8_t)(p->r + ((float)(cmd->r * 255.0f) - p->r) * a + 0.5f);
                    p->g = (uint8_t)(p->g + ((float)(cmd->g * 255.0f) - p->g) * a + 0.5f);
                    p->b = (uint8_t)(p->b + ((float)(cmd->b * 255.0f) - p->b) * a + 0.5f);
                }
            }
        } else if (cmd->type == DL_MASK_BEGIN) {
            /* Remember what is under the masked box; MASK_END blends the
             * box's painting back over it by the mask's alpha. */
            const DisplayMask *m = &dl->masks[cmd->u.mask_index];
            MaskLayer *L = nlayers < 64 ? &layers[nlayers] : NULL;
            if (!L) continue;
            L->x0 = (int)floorf(m->x); L->y0 = (int)floorf(m->y);
            L->x1 = (int)ceilf(m->x + m->w); L->y1 = (int)ceilf(m->y + m->h);
            if (L->x0 < 0) L->x0 = 0;
            if (L->y0 < 0) L->y0 = 0;
            if (L->x1 > img_width) L->x1 = img_width;
            if (L->y1 > img_height) L->y1 = img_height;
            L->mask = cmd->u.mask_index;
            L->saved = NULL;
            if (L->x1 > L->x0 && L->y1 > L->y0) {
                int w = L->x1 - L->x0;
                L->saved = (RGB *)malloc((size_t)w * (size_t)(L->y1 - L->y0) * sizeof(RGB));
                for (int y = L->y0; L->saved && y < L->y1; y++)
                    memcpy(L->saved + (size_t)(y - L->y0) * w, pixels + (size_t)y * img_width + L->x0,
                           (size_t)w * sizeof(RGB));
            }
            nlayers++;
        } else if (cmd->type == DL_MASK_END) {
            if (nlayers == 0) continue;
            MaskLayer *L = &layers[--nlayers];
            const DisplayMask *m = &dl->masks[L->mask];
            int sw = 0, sh = 0, sch = 0;
            uint8_t *src = NULL;
            ImageCache *cache = display_list_get_image_cache();
            if (m->kind == 1 && cache && !image_cache_get(cache, m->image_handle, &sw, &sh, &sch, &src)) src = NULL;
            int w = L->x1 - L->x0;
            for (int y = L->y0; L->saved && y < L->y1; y++) {
                for (int x = L->x0; x < L->x1; x++) {
                    float a = mask_alpha_at(m, (float)x + 0.5f, (float)y + 0.5f, src, sw, sh, sch);
                    if (a >= 0.999f) continue;
                    RGB *cur = &pixels[(size_t)y * img_width + x];
                    const RGB *old = &L->saved[(size_t)(y - L->y0) * w + (x - L->x0)];
                    if (a < 0.0f) a = 0.0f;
                    cur->r = (uint8_t)(old->r + (cur->r - old->r) * a + 0.5f);
                    cur->g = (uint8_t)(old->g + (cur->g - old->g) * a + 0.5f);
                    cur->b = (uint8_t)(old->b + (cur->b - old->b) * a + 0.5f);
                }
            }
            free(L->saved);
        }
    }
    while (nlayers > 0) free(layers[--nlayers].saved);

    /* stbi_write_jpg expects interleaved RGB. */
    int ok = stbi_write_jpg(path, img_width, img_height, 3, pixels, 95);
    free(pixels);
    return ok != 0;
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

/* The screenshot of the settled page.  The settle loop lays out each
 * rebuilt document and builds its display list, which requests the images
 * it shows (their arrival keeps the loop going), and the last one is
 * rasterized once, after the loop: painting and encoding every
 * intermediate document cost 0.1 s a run for a file the next one
 * replaced. */
typedef struct PendingShot {
    bool valid;
    LayoutContext layout;
    DisplayList dl;
} PendingShot;

static void pending_shot_free(PendingShot *s) {
    if (!s->valid) return;
    display_list_free(&s->dl);
    css_layout_tree_free(&s->layout);
    s->valid = false;
}

/* Lay out `doc` and build its display list (it must outlive the shot). */
static void pending_shot_build(PendingShot *s, HtmlDocument *doc, ImageCache *image_cache) {
    pending_shot_free(s);
    printf("Running CSS layout (%dx%d) ...\n", WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
    memset(&s->layout, 0, sizeof(s->layout));
    s->layout.js_ctx = g_ctx;
    s->layout.dump_path = layout_dump_path();
    bool ok;
    {
        CP_SCOPE("css-layout");
        ok = css_layout_run(&s->layout, doc, NULL, (double)WIREFRAME_WIDTH, (double)WIREFRAME_HEIGHT);
    }
    if (!ok) {
        printf("WARNING: css_layout_run() failed\n");
        css_layout_tree_free(&s->layout);
        return;
    }
    printf("Layout boxes: %d\n", s->layout.tree.count);
    display_list_init(&s->dl);
    {
        CP_SCOPE("build-display-list");
        ok = css_layout_build_display_list(&s->layout, &s->dl);
    }
    if (!ok) {
        printf("WARNING: css_layout_build_display_list() failed\n");
        display_list_free(&s->dl);
        css_layout_tree_free(&s->layout);
        return;
    }
    s->valid = true;
    /* Images first requested while building the list (mask images,
     * backgrounds) finish loading here, within a bound (a slow CDN must not
     * hold the screenshot): taken one settle turn at a time, each arrival
     * laid the page out again.  One that arrives later requests a layout
     * (on_image_progress), and the settle loop builds the shot again. */
    if (!image_cache_wait_pending_ms(image_cache, 1500))
        printf("Images still loading; painting without them for now\n");
}

/* Paint and save the last shot. */
static void pending_shot_save(PendingShot *s, const char *out_path) {
    if (!s->valid) return;
    printf("Display list commands: %d\n", s->dl.count);
    printf("Rendering screenshot to %s ...\n", out_path);
    bool raster_ok;
    {
        CP_SCOPE("raster-jpg");
        raster_ok = render_display_list_to_jpg(&s->dl, out_path, WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
    }
    if (raster_ok) printf("Saved screenshot: %s (%dx%d)\n", out_path, WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
    else printf("WARNING: failed to write %s\n", out_path);
}

static bool render_document_to_jpg(HtmlDocument *doc, ImageCache *image_cache,
                                   const char *out_path)
{
    if (!doc || !image_cache || !out_path) return false;

    printf("Running CSS layout (%dx%d) ...\n", WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
    LayoutContext layout;
    memset(&layout, 0, sizeof(layout));
    layout.js_ctx = g_ctx;
    layout.dump_path = layout_dump_path();
    bool layout_ok = false;
    {
        CP_SCOPE("css-layout");
        layout_ok = css_layout_run(&layout, doc, NULL,
                                   (double)WIREFRAME_WIDTH,
                                   (double)WIREFRAME_HEIGHT);
    }
    if (!layout_ok) {
        printf("WARNING: css_layout_run() failed\n");
        return false;
    }
    printf("Layout boxes: %d\n", layout.tree.count);

    DisplayList dl;
    display_list_init(&dl);

    bool dl_ok = false;
    {
        CP_SCOPE("build-display-list");
        dl_ok = css_layout_build_display_list(&layout, &dl);
    }
    if (dl_ok) {
        /* Images first requested while building the list (mask images,
         * backgrounds) finish decoding before the raster pass reads them,
         * within a bound: a slow download (a rate-limited CDN) must not
         * hold the screenshot.  One that arrives later requests a layout
         * (on_image_progress), and the settle loop paints again. */
        if (!image_cache_wait_pending_ms(image_cache, 1500))
            printf("Images still loading; painting without them for now\n");
        printf("Display list commands: %d\n", dl.count);
        printf("Rendering screenshot to %s ...\n", out_path);
        bool raster_ok = false;
        {
            CP_SCOPE("raster-jpg");
            raster_ok = render_display_list_to_jpg(&dl, out_path,
                                                   WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
        }
        if (raster_ok) {
            printf("Saved screenshot: %s (%dx%d)\n", out_path,
                   WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
        } else {
            printf("WARNING: failed to write %s\n", out_path);
        }
    } else {
        printf("WARNING: css_layout_build_display_list() failed\n");
    }

    display_list_free(&dl);
    css_layout_tree_free(&layout);
    return true;
}

/* CYBER_TIMELINE=1: the milestones of a run, in ms since process creation
 * (what the process spends outside load-page shows too). */
static void tl_mark(const char *what) {
    static int on = -1;
    if (on < 0) on = cyber_getenv("CYBER_TIMELINE") ? 1 : 0;
    if (!on) return;
    FILETIME c, e, k, u, now; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u); GetSystemTimeAsFileTime(&now);
    unsigned long long a = ((unsigned long long)c.dwHighDateTime << 32) | c.dwLowDateTime, b = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    fprintf(stderr, "[timeline] %-28s %8.1f ms\n", what, (b - a) / 1e4); fflush(stderr);
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

int main(int argc, char *argv[]) {
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
    const bool page_is_youtube = strstr(start_url, "youtube.") != NULL;

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
            if (!init_browser_context()) { printf("FATAL: init_browser_context() failed\n"); return 1; }
            register_font_table();   /* layout queries measure text */
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
                    if (!pump_timers_and_jobs(g_ctx) && js_quickjs_network_pending() == 0) break;
                }
            }
            free(pbuf);
            cleanup_browser_context();
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
        if (!init_browser_context()) {
            platform_http_cleanup();
            platform_cleanup();
            return 1;
        }
    }
    printf("init_browser_context ok\n");

    /* Fonts before any script runs: layout queries from scripts
     * (offsetHeight, getBoundingClientRect) measure text with them. */
    register_font_table();

    /* Make the start URL available to the DOM / URL resolution code. */
    g_cyber_start_url = start_url;

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
            cleanup_browser_context();
            platform_http_cleanup();
            platform_cleanup();
            return 0;
        }
    }

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
    if (!html) {
        cleanup_browser_context();
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }

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
        tl_mark("scripts done");
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

    /* Save the serialized JS DOM so it can be inspected post-run. */
    {
        GCValue js_doc = get_global_document(g_ctx);
        GCValue doc_elem = JS_GetPropertyStr(g_ctx, js_doc, "documentElement");
        char *serialized = html_serialize_js_node(g_ctx, doc_elem);
        if (serialized) {
            save_html(serialized, strlen(serialized));
            free(serialized);
        }
    }

    /* Phase 4: quiescence loop.  Pump timers/jobs, process completed async
     * image loads, and rebuild the native HtmlDocument from the JS DOM whenever
     * a mutation invalidates it.  Exit when nothing is left to do. */
    HtmlDocument *doc = NULL;
    PendingShot shot = {0};
    image_api_scan(g_ctx);

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
    CP_BEGIN("quiescence-loop");
    tl_mark("quiescence start");
    while (loop_iterations < 100) {
        loop_iterations++;

        bool had_timers = false;
        bool had_images = false;
        {
            CP_SCOPE("pump-timers");
            if (!settle_final) had_timers = pump_timers_and_jobs(g_ctx);
        }
        {
            CP_SCOPE("process-images");
            had_images = image_cache_process_pending(image_cache);
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
                pending_shot_free(&shot);   /* it refers into the old document */
                if (doc) html_document_free(doc);
                doc = new_doc;

                /* Print metadata from the rebuilt native document. */
                const char *title = get_title_text(doc);
                printf("Title: %s\n", title ? title : "(none)");
                printf("DOM nodes: %d\n", doc->array.count);
                print_body_snippet(doc);

                /* CSS is applied by the layout engine (css_layout_run) which
                 * has its own complete stylesheet collection + selector matching
                 * pipeline.  No separate CSS application step is needed here. */

                pending_shot_build(&shot, doc, image_cache);
            } else {
                printf("WARNING: failed to rebuild native document from JS DOM\n");
            }
        }

        if (!had_timers && !had_images && !g_dom_needs_layout &&
            !image_cache_has_pending(image_cache) && js_quickjs_network_pending() == 0) {
            break;
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
            } else if (quiet_ms && !g_dom_needs_layout && now - last_change_ms >= quiet_ms) {
                printf("Page quiet for %llu ms after %d iterations (%llu ms page time, %llu ms real); "
                       "pending timers left running.\n",
                       now - last_change_ms, loop_iterations, now - settle_start_page,
                       platform_get_time_ms() - settle_start_ms);
                break;
            }
        }
        if (!had_timers && !had_images && !g_dom_needs_layout) {
            /* Only image fetches/decodes in flight: sleep until one makes
             * progress instead of spinning the settle loop. */
            CP_SCOPE("wait-images");
            if (js_quickjs_network_pending() > 0) js_quickjs_net_wait(50);
            else if (virtual_time_idle()) {
                /* Nothing in flight: skip ahead to the next timer, or to the
                 * end of the quiet window. */
                unsigned long long now = page_time_ms();
                unsigned long long target = quiet_ms ? last_change_ms + quiet_ms : now + 100;
                unsigned long long next = timer_next_due_ms();
                if (next != (unsigned long long)-1 && next < target) target = next;
                page_time_advance(target > now ? target - now : 1);
            } else image_cache_wait_progress(image_cache, 100);
        }
        if (settle_final || platform_get_time_ms() - settle_start_ms > settle_budget_ms) {
            if (g_dom_needs_layout && !settle_final) {
                settle_final = true;    /* one more turn: lay out, no timers */
                continue;
            }
            printf("Settle budget (%llu ms) reached after %d iterations; pending timers left running.\n",
                   settle_budget_ms, loop_iterations);
            break;
        }
    }
    CP_END("quiescence-loop");
    pending_shot_save(&shot, "wikipedia_screenshot.jpg");
    pending_shot_free(&shot);

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
                pump_timers_and_jobs(g_ctx);
            }
            free(pbuf);
            fclose(pf);
        }
    }

    tl_mark("before page_final");
    /* Save the final mutated JS DOM for inspection. */
    {
        GCValue js_doc = get_global_document(g_ctx);
        GCValue doc_elem = JS_GetPropertyStr(g_ctx, js_doc, "documentElement");
        char *serialized = html_serialize_js_node(g_ctx, doc_elem);
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

    if (doc) html_document_free(doc);
    display_list_set_image_cache(NULL);
    image_cache_destroy(image_cache);
    free(html);

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
    finish_browser_context();
    platform_http_cleanup();
    platform_cleanup();
    tl_mark("after cleanup");
    return 0;
}
