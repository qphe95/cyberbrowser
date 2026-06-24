/*
 * CyberBrowser - minimal hardcoded YouTube loader
 *
 * Fetches https://www.youtube.com/, parses the HTML, runs the CSS layout
 * engine, builds a display list, renders a wireframe to a JPEG, and prints a
 * summary.  This is intended as a quick smoke-test executable for the
 * browser-emulator core.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "platform.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "browser_api_impl.h"
#include "js_quickjs.h"
#include "http_download.h"
#include "html_dom.h"
#include "css_parser.h"
#include "css_layout.h"
#include "display_list.h"
#include "text_shaper.h"
#include "image_cache.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "session_state.h"
#include "html_media_extract.h"

extern "C" int timer_process_due(JSContextHandle ctx);

#define LOG_TAG "cyberbrowser"

static JSRuntimeHandle g_rt;
static JSContextHandle g_ctx;
static GCValue g_global;

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

static char *fetch_youtube_homepage(size_t *out_size) {
    const char *url = "https://www.youtube.com/";
    printf("Fetching %s ...\n", url);

    HttpBuffer buffer = {0};
    char error[512] = {0};
    const char *headers[] = {
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.9"
    };

    bool ok = http_get_to_memory_with_headers(url, headers, 3, &buffer, error, sizeof(error));
    if (!ok || !buffer.data || buffer.size == 0) {
        printf("FATAL: Failed to fetch YouTube homepage: %s\n", error);
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
    FILE *f = fopen("youtube_loaded.html", "wb");
    if (!f) return;
    fwrite(html, 1, html_size, f);
    fclose(f);
    printf("Saved fetched HTML to youtube_loaded.html (%zu bytes)\n", html_size);
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
/* Session continuity: visitor tokens from homepage into JS context          */
/* ------------------------------------------------------------------------- */

static void js_escape_string(const char *in, char *out, size_t out_len) {
    size_t i = 0, j = 0;
    while (in[i] && j + 3 < out_len) {
        if (in[i] == '\\' || in[i] == '\'') {
            out[j++] = '\\';
        }
        out[j++] = in[i++];
    }
    out[j] = '\0';
}

static void inject_session_tokens(JSContextHandle ctx, GCValue global, const char *html) {
    char visitor[256] = {0};
    const char *cookies = platform_http_get_cookies();
    if (cookies && cookies[0]) {
        session_extract_cookie_value(cookies, "VISITOR_INFO1_LIVE", visitor, sizeof(visitor));
    }
    if (!visitor[0] && html && html[0]) {
        session_extract_json_field(html, "visitorData", visitor, sizeof(visitor));
    }
    if (!visitor[0]) {
        return;
    }

    session_set_visitor_data(visitor);

    char escaped[512] = {0};
    js_escape_string(visitor, escaped, sizeof(escaped));

    char script[2048];
    snprintf(script, sizeof(script),
        "window.ytcfg = window.ytcfg || {};"
        "window.ytcfg.set = function(k,v){ this[k]=v; return v; };"
        "window.ytcfg.get = function(k){ return this[k]; };"
        "window.ytcfg.set('VISITOR_DATA', '%s');"
        "window.ytcfg.set('CLIENT_VERSION', '2.20250122.04.00');"
        "document.cookie = 'VISITOR_INFO1_LIVE=%s; path=/; domain=.youtube.com';"
        "window.yt = window.yt || {};"
        "window.yt.config_ = window.yt.config_ || {};"
        "window.yt.config_.INNERTUBE_CLIENT_VERSION = '2.20250122.04.00';",
        escaped, escaped);

    JS_Eval(ctx, script, strlen(script), "<session>", JS_EVAL_TYPE_GLOBAL);
}

/* ------------------------------------------------------------------------- */
/* Phase 3 event loop helpers                                                */
/* ------------------------------------------------------------------------- */

static bool pump_timers_and_jobs(JSContextHandle ctx) {
    if (!ctx.valid()) return false;
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    bool did_work = false;
    int iterations = 0;
    while (iterations < 100) {
        int processed = timer_process_due(ctx);
        int jobs = 0;
        JSContextHandle pctx;
        int ret;
        while ((ret = JS_ExecutePendingJob(rt, &pctx)) > 0) {
            jobs++;
        }
        (void)ret;
        if (processed == 0 && jobs == 0) break;
        did_work = true;
        iterations++;
    }
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
    bool layout_ok = css_layout_run(&layout, doc, NULL,
                                    (double)WIREFRAME_WIDTH,
                                    (double)WIREFRAME_HEIGHT);
    if (!layout_ok) {
        printf("WARNING: css_layout_run() failed\n");
        return false;
    }
    printf("Layout boxes: %d\n", layout.tree.count);

    DisplayList dl;
    display_list_init(&dl);

    if (css_layout_build_display_list(&layout, &dl)) {
        printf("Display list commands: %d\n", dl.count);
        printf("Rendering screenshot to %s ...\n", out_path);
        if (render_display_list_to_jpg(&dl, out_path,
                                       WIREFRAME_WIDTH, WIREFRAME_HEIGHT)) {
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

static void print_captured_googlevideo_urls(JSContextHandle ctx) {
    char urls[JS_MAX_CAPTURED_URLS][JS_MAX_URL_LEN];
    int count = js_quickjs_get_captured_urls(urls, JS_MAX_CAPTURED_URLS);
    int gv_count = 0;
    for (int i = 0; i < count; i++) {
        if (strstr(urls[i], "googlevideo.com")) {
            printf("Captured googlevideo URL: %s\n", urls[i]);
            gv_count++;
        }
    }
    printf("Captured googlevideo.com URLs: %d\n", gv_count);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("========================================\n");
    printf("CyberBrowser - YouTube Loader\n");
    printf("========================================\n");

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

    if (!init_browser_context()) {
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }
    printf("init_browser_context ok\n");

    size_t html_size = 0;
    char *html = fetch_youtube_homepage(&html_size);
    if (!html) {
        cleanup_browser_context();
        platform_http_cleanup();
        platform_cleanup();
        return 1;
    }

    /* Extract VISITOR_INFO1_LIVE / visitorData and inject into JS so that
     * fetch() and XHR can send a consistent session with youtubei calls. */
    inject_session_tokens(g_ctx, g_global, html);

    /* Phase 3: execute page scripts, drain timers, dispatch lifecycle events,
     * and print any googlevideo.com URLs captured by the hooks. */
    {
        printf("Executing page scripts ...\n");
        js_quickjs_clear_captured_urls();
        JsExecResult js_result;
        bool exec_ok = html_execute_page_scripts(html, &js_result);
        if (exec_ok) {
            printf("Scripts executed: %d captured URLs\n", js_result.captured_url_count);
        } else {
            printf("WARNING: page script execution did not complete successfully\n");
        }
        pump_timers_and_jobs(g_ctx);

        // Reclaim handles allocated by script execution before dispatching
        // lifecycle events.
        JS_RunGC(JS_GetRuntime(g_ctx));

        printf("Dispatching DOMContentLoaded and load events ...\n");
        dispatch_page_lifecycle_events(g_ctx);
        pump_timers_and_jobs(g_ctx);

        print_captured_googlevideo_urls(g_ctx);

        // Upgrade any custom elements that were defined during script execution.
        // This lets Polymer run connectedCallback and stamp shadow-DOM content
        // on the existing server-rendered skeleton.
        {
            const char *upgrade_doc_js =
                "if (window.customElements && typeof window.customElements.upgrade === 'function' && document && document.documentElement) {"
                "  try { window.customElements.upgrade(document.documentElement); } catch(e) {}"
                "}";
            JS_Eval(g_ctx, upgrade_doc_js, strlen(upgrade_doc_js), "<upgrade_doc>", JS_EVAL_TYPE_GLOBAL);
        }
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
    while (loop_iterations < 100) {
        loop_iterations++;

        bool had_timers = pump_timers_and_jobs(g_ctx);
        bool had_images = image_cache_process_pending(image_cache);

        if (g_dom_needs_layout) {
            g_dom_needs_layout = 0;

            GCValue js_doc = get_global_document(g_ctx);
            HtmlDocument *new_doc = html_document_from_js_dom(g_ctx, js_doc);
            if (new_doc) {
                if (doc) html_document_free(doc);
                doc = new_doc;

                /* Print metadata from the rebuilt native document. */
                const char *title = get_title_text(doc);
                printf("Title: %s\n", title ? title : "(none)");
                printf("DOM nodes: %d\n", doc->array.count);
                print_body_snippet(doc);

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

    if (doc) html_document_free(doc);
    display_list_set_image_cache(NULL);
    image_cache_destroy(image_cache);
    free(html);

    printf("\nYouTube homepage loaded successfully.\n");

    cleanup_browser_context();
    platform_http_cleanup();
    platform_cleanup();
    return 0;
}
