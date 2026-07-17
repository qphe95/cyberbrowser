/*
 * CyberBrowser - minimal browser engine smoke-test
 *
 * Fetches a start page, parses the HTML, runs the CSS layout engine, builds a
 * display list, renders a wireframe to a JPEG, and prints a summary.  This is
 * intended as a quick smoke-test executable for the cyberbrowser core.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
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
#include "text_shaper.h"
#include "image_cache.h"
#include "cyber_profile.h"

/* stb_image_write implementation lives in cyber_profile.cpp (library side). */
#include "stb_image_write.h"
#include "session_state.h"
#include "html_media_extract.h"

extern "C" int timer_process_due(JSContextHandle ctx);
extern "C" int scheduler_process_tasks(JSContextHandle ctx);
extern "C" void timer_set_idle_deadline(unsigned long long deadline_ms);
unsigned long long timer_next_due_ms(void);
void platform_sleep_ms(unsigned int ms);

#define LOG_TAG "cyberbrowser"

/* Default page loaded by the smoke-test executable. */
static const char DEFAULT_START_URL[] = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
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
        return JSModuleDefHandle(GC_HANDLE_NULL);
    }

    HttpBuffer buffer = {0};
    char err[256] = {0};
    bool ok = http_get_to_memory(url, &buffer, err, sizeof(err));
    if (!ok || !buffer.data || buffer.size == 0) {
        platform_log(LOG_LEVEL_WARN, "module_loader", "Failed to fetch module %s: %s",
                     url, err[0] ? err : "unknown");
        if (buffer.data) free(buffer.data);
        JS_ThrowReferenceError(ctx, "could not fetch module '%s'", url);
        return JSModuleDefHandle(GC_HANDLE_NULL);
    }

    platform_log(LOG_LEVEL_INFO, "module_loader", "Loaded module %s (%zu bytes)", url, buffer.size);

    if (getenv("CYBER_DUMP_SCRIPTS")) {
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
        return JSModuleDefHandle(GC_HANDLE_NULL);
    }

    JSModuleDefHandle m = JS_VALUE_GET_MODULE_HANDLE(mv);
    if (!m.valid()) {
        platform_log(LOG_LEVEL_WARN, "module_loader", "Module %s did not produce a def", url);
        JS_ThrowReferenceError(ctx, "module '%s' produced no definition", url);
        return JSModuleDefHandle(GC_HANDLE_NULL);
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
        USHORT n = RtlCaptureStackBackTrace(0, 32, frames, NULL);
        fprintf(stderr, "[FATAL] backtrace (%d frames):\n", (int)n);
        for (USHORT i = 0; i < n; i++) fprintf(stderr, "  #%2d %p\n", (int)i, frames[i]);
        fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static JSRuntimeHandle g_rt;
static JSContextHandle g_ctx;
static GCValue g_global;

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
    if (!g_rt.valid()) {
        printf("FATAL: JS_NewRuntime() failed\n");
        return false;
    }

    /* Register the ES module loader so dynamic import() can fetch YouTube's
     * lazily-loaded component chunks over HTTP. */
    JS_SetModuleLoaderFunc(g_rt, cyber_module_normalize, cyber_module_loader, NULL);

    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx.valid()) {
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

    init_browser_api_impl(g_ctx, g_global);
    js_quickjs_setup_initial_dom();
    return true;
}

static void cleanup_browser_context(void) {
    extern JSRuntimeHandle g_js_runtime;
    extern JSContextHandle g_js_context;
    g_js_runtime = JSRuntimeHandle();
    g_js_context = JSContextHandle();
    if (g_rt.valid()) {
        JS_FreeRuntime(g_rt);
    }
}

static char *fetch_start_page(size_t *out_size) {
    const char *url = DEFAULT_START_URL;
    printf("Fetching %s ...\n", url);

    HttpBuffer buffer = {0};
    char error[512] = {0};
    const char *headers[] = {
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.9",
        "Cookie: CONSENT=YES+US.en+; PREF=tz=America.Los_Angeles&f4=4000000",
        "sec-ch-ua: \"Google Chrome\";v=\"125\", \"Chromium\";v=\"125\", \"Not.A/Brand\";v=\"24\"",
        "sec-ch-ua-mobile: ?0",
        "sec-ch-ua-platform: \"Windows\"",
        "sec-fetch-dest: document",
        "sec-fetch-mode: navigate",
        "sec-fetch-site: none",
        "sec-fetch-user: ?1",
        "upgrade-insecure-requests: 1"
    };

    bool ok = http_get_to_memory_with_headers(url, headers, sizeof(headers)/sizeof(headers[0]), &buffer, error, sizeof(error));
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

static bool pump_timers_and_jobs(JSContextHandle ctx) {
    if (!ctx.valid()) return false;
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    bool did_work = false;
    int iterations = 0;
    unsigned long long pump_start = platform_get_time_ms();
    while (iterations < 100) {
        // Give idle callbacks a real, per-iteration idle budget.
        unsigned long long idle_deadline = platform_get_time_ms() + 8;
        timer_set_idle_deadline(idle_deadline);

        int processed = timer_process_due(ctx);
        int sched = scheduler_process_tasks(ctx);
        int jobs = 0;
        JSContextHandle pctx;
        int ret;
        while ((ret = JS_ExecutePendingJob(rt, &pctx)) > 0) {
            jobs++;
        }
        (void)ret;
        if (processed == 0 && sched == 0 && jobs == 0) {
            /* Nothing immediately due.  Lifecycle callbacks (e.g. the monomer
             * "initialized" job set) are scheduled via setTimeout with a small
             * delay, and YouTube's app never reaches its "rendering" phase if
             * those never fire.  Wait for the earliest pending timer within a
             * bounded window instead of dropping it, so delayed callbacks run. */
            unsigned long long next = timer_next_due_ms();
            if (next != (unsigned long long)-1) {
                unsigned long long now = platform_get_time_ms();
                unsigned long long wait = next > now ? next - now : 0;
                unsigned long long elapsed = now - pump_start;
                if (wait <= 200 && elapsed + wait <= 3000) {
                    platform_sleep_ms((unsigned int)(wait ? wait : 1));
                    continue;
                }
            }
            break;
        }
        did_work = true;
        iterations++;
    }
    timer_set_idle_deadline(0);
    return did_work;
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
    if (!ctx.valid()) return JS_NULL;
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
    int ax0 = (int)floorf(cmd->u.glyph.u0 * (float)atlas_w);
    int ay0 = (int)floorf(cmd->u.glyph.v0 * (float)atlas_h);
    int ax1 = (int)floorf(cmd->u.glyph.u1 * (float)atlas_w);
    int ay1 = (int)floorf(cmd->u.glyph.v1 * (float)atlas_h);
    int gw = ax1 - ax0;
    int gh = ay1 - ay0;
    if (gw <= 0 || gh <= 0) return;

    int dx0 = (int)floorf(cmd->x);
    int dy0 = (int)floorf(cmd->y);
    for (int gy = 0; gy < gh; gy++) {
        for (int gx = 0; gx < gw; gx++) {
            int sx = ax0 + gx;
            int sy = ay0 + gy;
            if (sx < 0 || sx >= atlas_w || sy < 0 || sy >= atlas_h) continue;
            uint8_t a8 = atlas[sy * atlas_w + sx];
            if (a8 == 0) continue;
            int dx = dx0 + gx;
            int dy = dy0 + gy;
            if (dx < 0 || dx >= img_width || dy < 0 || dy >= img_height) continue;
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

static bool render_display_list_to_jpg(const DisplayList *dl, const char *path,
                                       int img_width, int img_height) {
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

    TextShaper *font = display_list_get_default_font();
    const uint8_t *atlas = font ? text_shaper_atlas_pixels(font) : NULL;
    int atlas_w = font ? text_shaper_atlas_width(font) : 0;
    int atlas_h = font ? text_shaper_atlas_height(font) : 0;

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
        } else if (cmd->type == DL_GLYPH && atlas) {
            draw_glyph(pixels, img_width, img_height, atlas, atlas_w, atlas_h, cmd);
        } else if (cmd->type == DL_IMAGE) {
            ImageCache *cache = display_list_get_image_cache();
            if (cache) draw_image(pixels, img_width, img_height, cache, cmd);
        }
    }

    /* stbi_write_jpg expects interleaved RGB. */
    int ok = stbi_write_jpg(path, img_width, img_height, 3, pixels, 95);
    free(pixels);
    return ok != 0;
}

static bool render_document_to_jpg(HtmlDocument *doc, ImageCache *image_cache,
                                   const char *out_path)
{
    if (!doc || !image_cache || !out_path) return false;

    printf("Running CSS layout (%dx%d) ...\n", WIREFRAME_WIDTH, WIREFRAME_HEIGHT);
    LayoutContext layout;
    memset(&layout, 0, sizeof(layout));
    layout.js_ctx = g_ctx;
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

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

#ifdef _WIN32
    SetUnhandledExceptionFilter(unhandled_exception_filter);
#endif

    setvbuf(stdout, NULL, _IONBF, 0);

    /* Fast JS-engine probe: eval a file and exit (CYBER_PROBE_JS=/path/to.js).
     * Optional CYBER_PROBE_NO_LAZY=1 forces full (non-lazy) parsing. */
    {
        const char *probe = getenv("CYBER_PROBE_JS");
        if (probe && probe[0]) {
            if (!platform_init()) { printf("FATAL: platform_init() failed\n"); return 1; }
            if (!platform_http_init()) { printf("FATAL: platform_http_init() failed\n"); return 1; }
            if (!init_browser_context()) { printf("FATAL: init_browser_context() failed\n"); return 1; }
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
            if (getenv("CYBER_PROBE_NO_LAZY")) pflags |= JS_EVAL_FLAG_NO_LAZY;
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
            if (!getenv("CYBER_PROBE_NO_PUMP")) {
                for (int pi = 0; pi < 50; pi++) {
                    if (!pump_timers_and_jobs(g_ctx)) break;
                }
            }
            free(pbuf);
            cleanup_browser_context();
            platform_http_cleanup();
            platform_cleanup();
            return JS_IsException(pres) ? 1 : 0;
        }
    }

    printf("========================================\n");
    printf("CyberBrowser\n");
    printf("========================================\n");

    CP_BEGIN("load-youtube");

    if (!platform_init()) {
        printf("FATAL: platform_init() failed\n");
        return 1;
    }
    printf("platform_init ok\n");
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

    /* Make the start URL available to the DOM / URL resolution code. */
    g_cyber_start_url = DEFAULT_START_URL;

    /* Reflect the start URL in window.location so the engine reports the
     * correct origin, host, and pathname instead of a hardcoded default.
     * Also sync document.domain and document.baseURI to the resolved URL. */
    {
        char loc_script[512];
        snprintf(loc_script, sizeof(loc_script),
                 "if (window.location) { window.location.href = %s%s%s; "
                 "if (document) { document.domain = window.location.hostname; "
                 "document.baseURI = window.location.href; } }",
                 "\"", DEFAULT_START_URL, "\"");
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
        const char *lhtml_path = getenv("CYBER_LAYOUT_HTML");
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
            html_document_free(ldoc);
            image_cache_destroy(lcache);
            cleanup_browser_context();
            platform_http_cleanup();
            platform_cleanup();
            return 0;
        }
    }

    size_t html_size = 0;
    char *html = NULL;
    {
        CP_SCOPE("fetch-start-page");
        html = fetch_start_page(&html_size);
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
        js_quickjs_clear_captured_urls();
        JsExecResult js_result;
        bool exec_ok = false;
        {
            CP_SCOPE("execute-page-scripts");
            exec_ok = html_execute_page_scripts(html, &js_result);
        }
        if (exec_ok) {
            printf("Scripts executed: %d captured URLs\n", js_result.captured_url_count);
        } else {
            printf("WARNING: page script execution did not complete successfully\n");
        }

        if (getenv("CYBER_HOOK_BOOT")) {
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
        pump_timers_and_jobs(g_ctx);

        /* Neutralise YouTube's error-logging paths before lifecycle timers fire.
         * The helper g.En("yt.logging.errors.log") creates empty objects along
         * the path if it is missing and then calls the result, which throws
         * "not a function" and aborts the app loader. */
        {
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
            JS_RunGC(JS_GetRuntime(g_ctx));
        }

        printf("Dispatching DOMContentLoaded and load events ...\n");
        {
            CP_SCOPE("lifecycle-events");
            dispatch_page_lifecycle_events(g_ctx);
        }
        {
            CP_SCOPE("pump-after-lifecycle");
            pump_timers_and_jobs(g_ctx);
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
             * normally fired by the app element's Polymer attached() callback.
             * When that callback is not delivered, the rendering phase (which
             * routes ytInitialData into the app) never runs.  Fire the signal
             * directly once the app has stamped. */
            {
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
                pump_timers_and_jobs(g_ctx);
            }
            }

        if (getenv("CYBER_DIAG_SIGNALS")) {
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
    ImageCache *image_cache = image_cache_create();
    display_list_set_image_cache(image_cache);

    const char *font_paths[] = {
        "cyberbrowser/third_party/fonts/Roboto-Regular.ttf",
        "third_party/fonts/Roboto-Regular.ttf",
        "../third_party/fonts/Roboto-Regular.ttf",
        "../../third_party/fonts/Roboto-Regular.ttf",
        NULL
    };
    for (int i = 0; font_paths[i]; i++) {
        if (display_list_set_default_font(font_paths[i], 16.0f)) break;
    }

    int loop_iterations = 0;
    CP_BEGIN("quiescence-loop");
    while (loop_iterations < 100) {
        loop_iterations++;

        bool had_timers = false;
        bool had_images = false;
        {
            CP_SCOPE("pump-timers");
            had_timers = pump_timers_and_jobs(g_ctx);
        }
        {
            CP_SCOPE("process-images");
            had_images = image_cache_process_pending(image_cache);
        }

        if (g_dom_needs_layout) {
            g_dom_needs_layout = 0;

            GCValue js_doc = get_global_document(g_ctx);

            HtmlDocument *new_doc = NULL;
            {
                CP_SCOPE("rebuild-doc");
                new_doc = html_document_from_js_dom(g_ctx, js_doc);
            }
            if (new_doc) {
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

                render_document_to_jpg(doc, image_cache, "youtube_screenshot.jpg");
            } else {
                printf("WARNING: failed to rebuild native document from JS DOM\n");
            }
        }

        if (!had_timers && !had_images && !g_dom_needs_layout &&
            !image_cache_has_pending(image_cache)) {
            break;
        }
    }
    CP_END("quiescence-loop");

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
    if (getenv("CYBER_DIAG_SHADY")) {
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

    if (getenv("CYBER_DIAG_BOOT")) {
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

    printf("\nStart page loaded successfully.\n");

    CP_END("load-youtube");
    if (cp_profile_enabled()) {
        cp_profile_flush("profile.json");
        cp_profile_write_flamegraph("flamegraph.png");
        printf("Profile written to profile.json + flamegraph.png\n");
    }

    cleanup_browser_context();
    platform_http_cleanup();
    platform_cleanup();
    return 0;
}
