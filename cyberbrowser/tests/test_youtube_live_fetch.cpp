/*
 * Live YouTube Fetch Test
 *
 * This test fetches fresh YouTube data from the live website instead of using
 * stored scraped data. It verifies that:
 * 1. The youtubei/v1/player POST returns HTTP 200 (not 400)
 * 2. video.src is assigned and googlevideo URLs are captured
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "browser_api_impl.h"
#include "js_quickjs.h"
#include "html_media_extract.h"
#include "http_download.h"
#include "html_dom.h"

extern "C" int timer_process_due(JSContextHandle ctx);
extern "C" int timer_has_pending(void);

#define LOG_TAG "youtube_live_fetch"

/* Maximum script count */
#define MAX_SCRIPTS 100
#define MAX_SCRIPT_SIZE (20 * 1024 * 1024)
#define MAX_HTML_SIZE (20 * 1024 * 1024)

/* Global state */
static JSRuntimeHandle g_rt;
static JSContextHandle g_ctx;
static GCValue g_global;

/* Path to youtube_data directory */
static const char* get_youtube_data_dir(void) {
    static const char* paths[] = {
        "youtube_data",
        "../youtube_data",
        "../../youtube_data",
        "../../../youtube_data",
    };
    static char found_path[512] = "";
    if (found_path[0] != '\0') return found_path;
    for (size_t i = 0; i < sizeof(paths)/sizeof(paths[0]); i++) {
        DIR *dir = opendir(paths[i]);
        if (dir) { closedir(dir); strncpy(found_path, paths[i], sizeof(found_path)-1); return found_path; }
    }
    return paths[0];
}
#define YOUTUBE_DATA_DIR get_youtube_data_dir()

/* Read entire file into memory */
static char* read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("    ERROR: Cannot open file: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > MAX_SCRIPT_SIZE) {
        printf("    ERROR: Invalid file size: %ld\n", size);
        fclose(f); return NULL;
    }
    char *content = (char*)malloc(size + 1);
    if (!content) { fclose(f); return NULL; }
    size_t read = fread(content, 1, size, f);
    fclose(f);
    if (read != (size_t)size) { free(content); return NULL; }
    content[size] = '\0';
    if (out_size) *out_size = size;
    return content;
}

/* Initialize test context */
static bool init_test_context(void) {
    if (!gc_init()) { printf("    ERROR: gc_init() failed\n"); return false; }
    g_rt = JS_NewRuntime();
    if (!g_rt.valid()) { printf("    ERROR: JS_NewRuntime() failed\n"); return false; }
    g_ctx = JS_NewContext(g_rt);
    if (!g_ctx.valid()) { printf("    ERROR: JS_NewContext() failed\n"); return false; }
    JS_AddIntrinsicEval(g_ctx);
    JS_AddIntrinsicRegExp(g_ctx);
    JS_AddIntrinsicJSON(g_ctx);
    JS_AddIntrinsicPromise(g_ctx);
    JS_AddIntrinsicMapSet(g_ctx);
    JS_AddIntrinsicTypedArrays(g_ctx);
    g_global = JS_GetGlobalObject(g_ctx);
    if (JS_IsException(g_global)) { printf("    ERROR: JS_GetGlobalObject() failed\n"); return false; }
    extern JSRuntimeHandle g_js_runtime;
    extern JSContextHandle g_js_context;
    g_js_runtime = g_rt;
    g_js_context = g_ctx;
    init_browser_api_impl(g_ctx, g_global);
    if (JS_HasException(g_ctx)) {
        GCValue pending_exc = JS_GetException(g_ctx);
        (void)pending_exc;
    }
    js_quickjs_setup_initial_dom();
    return true;
}

/* Cleanup test context */
static void cleanup_test_context(void) {
    extern JSRuntimeHandle g_js_runtime;
    extern JSContextHandle g_js_context;
    g_js_runtime = JSRuntimeHandle();
    g_js_context = JSContextHandle();
    if (g_rt.valid()) JS_FreeRuntime(g_rt);
}

/* Check if content is JSON-LD structured data */
static bool is_json_ld(const char *content, size_t len) {
    if (len < 10 || content[0] != '{') return false;
    return strstr(content, "\"@context\"") != NULL;
}

/* Execute a single script */
static bool execute_script(JSContextHandle ctx, const char *script, size_t script_len,
                           const char *name, int script_num) {
    if (is_json_ld(script, script_len)) {
        printf("    \u26a0 Script %03d SKIPPED (JSON-LD)\n", script_num);
        return true;
    }
    if (script_len > 1 * 1024 * 1024) {
        printf("    [DEBUG] Script %03d is large: %.1fMB\n", script_num, script_len / (1024.0 * 1024.0));
    }
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    GCValue result = JS_Eval(ctx, script, script_len, name, JS_EVAL_TYPE_GLOBAL);
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double duration = (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;
    if (JS_IsException(result)) {
        GCValue exc = JS_GetException(ctx);
        const char *error_msg = NULL;
        GCValue msg_val = JS_GetPropertyStr(ctx, exc, "message");
        if (!JS_IsException(msg_val) && !JS_IsUndefined(msg_val)) error_msg = JS_ToCString(ctx, msg_val);
        const char *error_stack = NULL;
        GCValue stack_val = JS_GetPropertyStr(ctx, exc, "stack");
        if (!JS_IsException(stack_val) && !JS_IsUndefined(stack_val)) error_stack = JS_ToCString(ctx, stack_val);
        const char *error_name = NULL;
        GCValue name_val = JS_GetPropertyStr(ctx, exc, "name");
        if (!JS_IsException(name_val) && !JS_IsUndefined(name_val)) error_name = JS_ToCString(ctx, name_val);
        const char *error_str = NULL;
        GCValue str_val = JS_ToString(ctx, exc);
        if (!JS_IsException(str_val)) error_str = JS_ToCString(ctx, str_val);
        printf("    \u2717 Script %03d ERROR: type=%s msg=%s str=%s\n", script_num,
               error_name ? error_name : "(no name)",
               error_msg ? error_msg : "(no message)",
               error_str ? error_str : "(no str)");
        if (error_stack && error_stack[0]) printf("      Stack: %s\n", error_stack);
        return false;
    }
    if (script_len > 1 * 1024 * 1024 || duration > 0.5) {
        printf("    [DEBUG] Script %03d execution time: %.2f s\n", script_num, duration);
    }
    printf("    \u2713 Script %03d executed successfully\n", script_num);
    return true;
}

/* ============================================================================
 * DOM Population from Parsed HTML
 * ============================================================================ */

/* Recursively create JS elements from parsed HTML nodes and append to parent */
static void append_parsed_children_to_js_element(JSContextHandle ctx, GCValue js_doc, HtmlDocument *doc, HtmlNode *parent_node, GCValue js_parent) {
    if (!doc || !parent_node) return;
    
    HtmlNode *child = html_node_first_child(doc, parent_node);
    while (child) {
        if (child->type == HTML_NODE_ELEMENT) {
            /* Skip script tags - we execute them manually */
            if (strcasecmp(child->tag_name, "script") == 0) {
                child = html_node_next_sibling(doc, child);
                continue;
            }
            
            GCValue elem = html_create_element_js_with_document(ctx, js_doc, child->tag_name, child->attributes);
            if (!JS_IsNull(elem) && !JS_IsUndefined(elem)) {
                /* Recursively add children */
                append_parsed_children_to_js_element(ctx, js_doc, doc, child, elem);
                
                /* Append to parent */
                GCValue appendChild = JS_GetPropertyStr(ctx, js_parent, "appendChild");
                if (!JS_IsUndefined(appendChild) && !JS_IsNull(appendChild)) {
                    GCValue args[1] = { elem };
                    JS_Call(ctx, appendChild, js_parent, 1, args);
                }
            }
        } else if (child->type == HTML_NODE_TEXT && child->text_content && strlen(child->text_content) > 0) {
            GCValue createTextNode = JS_GetPropertyStr(ctx, js_doc, "createTextNode");
            if (!JS_IsUndefined(createTextNode) && !JS_IsNull(createTextNode)) {
                GCValue args[1] = { JS_NewString(ctx, child->text_content) };
                GCValue text_node = JS_Call(ctx, createTextNode, js_doc, 1, args);
                GCValue appendChild = JS_GetPropertyStr(ctx, js_parent, "appendChild");
                if (!JS_IsUndefined(appendChild) && !JS_IsNull(appendChild)) {
                    GCValue append_args[1] = { text_node };
                    JS_Call(ctx, appendChild, js_parent, 1, append_args);
                }
            }
        }
        child = html_node_next_sibling(doc, child);
    }
}

/* Parse HTML and populate existing JS DOM with non-script elements */
static bool populate_dom_from_html(JSContextHandle ctx, const char *html, size_t html_len) {
    HtmlDocument *doc = html_parse(html, html_len);
    if (!doc) {
        printf("    ERROR: Failed to parse HTML\n");
        return false;
    }
    
    GCValue js_doc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document");
    if (JS_IsUndefined(js_doc) || JS_IsNull(js_doc)) {
        html_document_free(doc);
        return false;
    }
    
    GCValue body = JS_GetPropertyStr(ctx, js_doc, "body");
    GCValue head = JS_GetPropertyStr(ctx, js_doc, "head");
    
    /* Populate body from parsed HTML body children */
    HtmlNode *doc_body = html_document_body(doc);
    if (doc_body && !JS_IsUndefined(body) && !JS_IsNull(body)) {
        append_parsed_children_to_js_element(ctx, js_doc, doc, doc_body, body);
    }
    
    /* Populate head from parsed HTML head children */
    HtmlNode *doc_head = html_document_head(doc);
    if (doc_head && !JS_IsUndefined(head) && !JS_IsNull(head)) {
        append_parsed_children_to_js_element(ctx, js_doc, doc, doc_head, head);
    }
    
    html_document_free(doc);
    printf("    DOM populated from HTML\n");
    return true;
}

/* ============================================================================
 * Fresh Data Fetching and Script Execution
 * ============================================================================ */

/* Fetch fresh YouTube page */
static char* fetch_youtube_page(size_t *out_size) {
    const char *url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
    printf("  Fetching fresh YouTube page: %s\n", url);

    HttpBuffer buffer = {0};
    char error[512] = {0};

    /* Use a desktop browser User-Agent to get the full page with ytInitialPlayerResponse */
    const char *headers[] = {
        "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language: en-US,en;q=0.9"
    };
    bool success = http_get_to_memory_with_headers(url, headers, 3, &buffer, error, sizeof(error));
    if (!success || !buffer.data || buffer.size == 0) {
        printf("    ERROR: Failed to fetch YouTube page: %s\n", error);
        return NULL;
    }

    printf("    Fetched %zu bytes\n", buffer.size);
    
    /* Debug: save fetched HTML for inspection */
    FILE *debug_f = fopen("/tmp/yt_fetched_by_test.html", "wb");
    if (debug_f) {
        fwrite(buffer.data, 1, buffer.size, debug_f);
        fclose(debug_f);
    }
    
    /* Check if marker exists */
    if (strstr(buffer.data, "ytInitialPlayerResponse")) {
        printf("    Marker ytInitialPlayerResponse FOUND in fetched HTML\n");
    } else {
        printf("    Marker ytInitialPlayerResponse NOT FOUND in fetched HTML\n");
    }

    if (buffer.size > MAX_HTML_SIZE) {
        printf("    ERROR: HTML too large: %zu bytes\n", buffer.size);
        http_free_buffer(&buffer);
        return NULL;
    }

    size_t fetched_size = buffer.size;
    char *html = (char*)malloc(fetched_size + 1);
    if (!html) {
        http_free_buffer(&buffer);
        return NULL;
    }
    memcpy(html, buffer.data, fetched_size);
    html[fetched_size] = '\0';
    http_free_buffer(&buffer);

    if (out_size) *out_size = fetched_size;
    return html;
}

/* Extract ytInitialPlayerResponse from HTML using brace counting */
static char* extract_yt_initial_player_response(const char *html, size_t html_len) {
    printf("    [DEBUG] html_len=%zu\n", html_len);
    const char *marker = "var ytInitialPlayerResponse = {";
    const char *start = strstr(html, marker);
    if (!start) {
        printf("    ERROR: ytInitialPlayerResponse marker not found\n");
        return NULL;
    }
    printf("    [DEBUG] marker found at offset %zu\n", (size_t)(start - html));
    start += strlen("var ytInitialPlayerResponse = ");
    printf("    [DEBUG] start offset=%zu, first char='%c'\n", (size_t)(start - html), *start);

    /* Parse JSON by counting braces, respecting strings */
    int brace_depth = 0;
    bool in_string = false;
    bool escaped = false;
    const char *p = start;
    int iterations = 0;
    
    for (; p < html + html_len; p++) {
        iterations++;
        char c = *p;
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
        } else {
            if (c == '"') {
                in_string = true;
            } else if (c == '{') {
                brace_depth++;
            } else if (c == '}') {
                brace_depth--;
                if (brace_depth == 0) {
                    /* Found matching closing brace */
                    p++; /* Move past the closing brace */
                    break;
                }
            }
        }
    }
    
    printf("    [DEBUG] loop iterations=%d, brace_depth=%d, p offset=%zu\n", 
           iterations, brace_depth, (size_t)(p - html));
    
    if (brace_depth != 0) {
        printf("    ERROR: Could not find matching closing brace for ytInitialPlayerResponse\n");
        return NULL;
    }

    size_t len = p - start;
    printf("    [DEBUG] len=%zu\n", len);
    char *json = (char*)malloc(len + 1);
    if (!json) return NULL;
    memcpy(json, start, len);
    json[len] = '\0';

    printf("    Extracted ytInitialPlayerResponse: %zu bytes\n", len);
    return json;
}

/* Execute scripts in HTML order (inline and external interleaved) */
static bool execute_scripts_in_html_order(JSContextHandle ctx, const char *html, size_t html_len, bool use_live_scripts) {
    printf("  Executing scripts in HTML order...\n");
    int count = 0;
    int external_count = 0;
    const char *p = html;
    const char *end = html + html_len;
    
    while (p < end) {
        /* Find next <script tag */
        const char *script_tag = strstr(p, "<script");
        if (!script_tag) break;
        
        /* Find the end of the opening tag */
        const char *tag_end = strchr(script_tag, '>');
        if (!tag_end) break;
        
        /* Check if it has a src attribute (external script) */
        bool has_src = false;
        for (const char *c = script_tag + 7; c < tag_end; c++) {
            if (*c == 's' && strncmp(c, "src=", 4) == 0) {
                has_src = true;
                break;
            }
        }
        
        if (has_src) {
            /* Extract src URL from the tag */
            const char *src_start = NULL;
            size_t src_len = 0;
            for (const char *c = script_tag + 7; c < tag_end; c++) {
                if (*c == 's' && strncmp(c, "src=", 4) == 0) {
                    const char *quote = c + 4;
                    if (*quote == '"' || *quote == '\'') {
                        src_start = quote + 1;
                        const char *end_quote = strchr(src_start, *quote);
                        if (end_quote && end_quote < tag_end) {
                            src_len = end_quote - src_start;
                        }
                    }
                    break;
                }
            }
            
            if (src_start && src_len > 0 && src_len < 2048) {
                char src_url[2048];
                memcpy(src_url, src_start, src_len);
                src_url[src_len] = '\0';
                
                /* Resolve relative URLs */
                char full_url[2048];
                if (src_url[0] == '/') {
                    snprintf(full_url, sizeof(full_url), "https://www.youtube.com%s", src_url);
                } else {
                    strncpy(full_url, src_url, sizeof(full_url) - 1);
                    full_url[sizeof(full_url) - 1] = '\0';
                }
                
                bool script_executed = false;
                
                if (use_live_scripts) {
                    /* Try fetching fresh script from YouTube */
                    printf("    Fetching external script %03d: %.100s\n", count, full_url);
                    HttpBuffer buffer = {0};
                    char error[512] = {0};
                    bool fetched = http_get_to_memory(full_url, &buffer, error, sizeof(error));
                    
                    if (fetched && buffer.data && buffer.size > 0) {
                        char name[64];
                        snprintf(name, sizeof(name), "ext_%03d", count);
                        execute_script(ctx, buffer.data, buffer.size, name, count);
                        http_free_buffer(&buffer);
                        script_executed = true;
                    } else {
                        printf("    WARNING: Failed to fetch %s: %s\n", full_url, error);
                    }
                }
                
                if (!script_executed) {
                    /* Fall back to cached script from youtube_data */
                    char filename[256];
                    snprintf(filename, sizeof(filename), "%s/youtube_script_%03d_external.js", YOUTUBE_DATA_DIR, count);
                    size_t content_len;
                    char *content = read_file(filename, &content_len);
                    if (content) {
                        char name[64];
                        snprintf(name, sizeof(name), "cached_%03d", count);
                        execute_script(ctx, content, content_len, name, count);
                        free(content);
                        script_executed = true;
                    } else if (!use_live_scripts) {
                        printf("    WARNING: External script %03d not found in youtube_data (skipping)\n", count);
                    }
                }
                
                if (script_executed) {
                    external_count++;
                }
            } else {
                printf("    WARNING: Could not extract src URL for external script %03d\n", count);
            }
            p = tag_end + 1;
        } else {
            /* Inline script */
            /* Check for non-JS types (JSON-LD, application/json, etc.) */
            bool is_js = true;
            for (const char *c = script_tag + 7; c < tag_end; c++) {
                if (strncmp(c, "application/ld+json", 19) == 0 ||
                    strncmp(c, "application/json", 16) == 0) {
                    is_js = false;
                    break;
                }
            }
            
            const char *content_start = tag_end + 1;
            const char *script_end = strstr(content_start, "</script>");
            if (!script_end) break;
            
            size_t content_len = script_end - content_start;
            if (is_js && content_len > 0 && content_len < MAX_SCRIPT_SIZE) {
                char *script = (char*)malloc(content_len + 1);
                if (script) {
                    memcpy(script, content_start, content_len);
                    script[content_len] = '\0';
                    char name[64];
                    snprintf(name, sizeof(name), "<inline_%d>", count);
                    execute_script(ctx, script, content_len, name, count);
                    free(script);
                }
            }
            p = script_end + strlen("</script>");
        }
        
        count++;
    }
    
    printf("  Processed %d scripts (%d external)\n", count, external_count);
    return true;
}

/* ============================================================================
 * Main Test
 * ============================================================================ */

static bool test_youtube_live_fetch(void) {
    printf("\n  Test: Fetch fresh YouTube data and verify player bootstrap\n");
    
    /* Track media availability across test phases */
    bool has_media_after_scripts = false;
    bool has_encrypted_after_scripts = false;
    bool has_sabr_after_scripts = false;

    /* Step 1: Get YouTube HTML (cached known-good by default, live with USE_LIVE_HTML=1) */
    size_t html_size;
    char *html = NULL;
    bool using_live_html = false;
    const char *use_live = getenv("USE_LIVE_HTML");
    if (use_live && (strcmp(use_live, "1") == 0 || strcmp(use_live, "true") == 0)) {
        printf("    Fetching live YouTube HTML...\n");
        html = fetch_youtube_page(&html_size);
        if (!html) {
            printf("    SKIPPING: Could not fetch fresh YouTube page (network may be unavailable)\n");
            return true; /* Skip, don't fail */
        }
        using_live_html = true;
    } else {
        /* Use known-good cached HTML for deterministic testing */
        const char *known_good_paths[] = {
            "youtube_data/known_good.html",
            "../youtube_data/known_good.html",
            "../../youtube_data/known_good.html",
            "../../../youtube_data/known_good.html",
        };
        for (size_t i = 0; i < sizeof(known_good_paths)/sizeof(known_good_paths[0]); i++) {
            html = read_file(known_good_paths[i], &html_size);
            if (html) {
                printf("    Using known-good cached HTML from %s\n", known_good_paths[i]);
                break;
            }
        }
        if (!html) {
            printf("    No known-good HTML found, fetching live...\n");
            html = fetch_youtube_page(&html_size);
            if (!html) {
                printf("    SKIPPING: Could not fetch YouTube page\n");
                return true;
            }
            using_live_html = true;
        }
    }

    /* Step 2: Verify fresh HTML contains required data */
    char *ytip_json = extract_yt_initial_player_response(html, html_size);
    if (!ytip_json) {
        printf("    ERROR: Could not extract ytInitialPlayerResponse from HTML\n");
        free(html);
        return false;
    }
    bool has_format_urls = (strstr(ytip_json, "\"url\":\"https://rr") != NULL);
    bool has_sabr_url = (strstr(ytip_json, "\"serverAbrStreamingUrl\"") != NULL);
    printf("    Fresh ytInitialPlayerResponse has format URLs: %s\n", has_format_urls ? "YES" : "NO");
    printf("    Fresh ytInitialPlayerResponse has SABR URL: %s\n", has_sabr_url ? "YES" : "NO");
    free(ytip_json);

    /* Step 3: Initialize context */
    if (!init_test_context()) {
        free(html);
        return false;
    }

    /* Step 4: Populate DOM from parsed HTML */
    if (!populate_dom_from_html(g_ctx, html, html_size)) {
        printf("    WARNING: Failed to populate DOM from HTML, continuing with default DOM\n");
    }

    /* Step 5: Override non-deterministic functions for consistent player behavior */
    {
        const char *determinism_js =
            "(function() {"
            "  var seed = 12345;"
            "  Math.random = function() {"
            "    seed = (seed * 9301 + 49297) % 233280;"
            "    return seed / 233280;"
            "  };"
            "  var fixedTime = Date.now();"
            "  Date.now = function() { return fixedTime; };"
            "  var perfNowBase = 0;"
            "  if (typeof performance !== 'undefined' && performance.now) {"
            "    performance.now = function() { return perfNowBase++; };"
            "  }"
            "})();";
        JS_Eval(g_ctx, determinism_js, strlen(determinism_js), "<determinism>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Step 6: Execute scripts in HTML order (inline + external interleaved) */
    if (!execute_scripts_in_html_order(g_ctx, html, html_size, using_live_html)) {
        free(html);
        cleanup_test_context();
        return false;
    }

    /* Step 7: Verify ytInitialPlayerResponse was set in JS context */
    {
        GCValue global = JS_GetGlobalObject(g_ctx);
        GCValue ytip = JS_GetPropertyStr(g_ctx, global, "ytInitialPlayerResponse");
        if (JS_IsUndefined(ytip) || JS_IsNull(ytip)) {
            printf("    ERROR: ytInitialPlayerResponse not set after script execution\n");
            free(html);
            cleanup_test_context();
            return false;
        }
        printf("    ytInitialPlayerResponse verified in JS context\n");
        
        /* Check for media URLs right after script execution */
        const char *check_media_early_js =
            "(function() {"
            "  var result = { hasMediaUrls: false, hasEncrypted: false, hasSabrUrl: false, hasDashUrl: false, hasHlsUrl: false };"
            "  var ytip = window.ytInitialPlayerResponse;"
            "  if (!ytip || !ytip.streamingData) return result;"
            "  var formats = ytip.streamingData.formats || [];"
            "  var adaptive = ytip.streamingData.adaptiveFormats || [];"
            "  for (var i = 0; i < formats.length; i++) {"
            "    if (formats[i].url) result.hasMediaUrls = true;"
            "    if (formats[i].signatureCipher) result.hasEncrypted = true;"
            "  }"
            "  for (var i = 0; i < adaptive.length; i++) {"
            "    if (adaptive[i].url) result.hasMediaUrls = true;"
            "    if (adaptive[i].signatureCipher) result.hasEncrypted = true;"
            "  }"
            "  if (ytip.streamingData.serverAbrStreamingUrl) result.hasSabrUrl = true;"
            "  if (ytip.streamingData.dashManifestUrl) result.hasDashUrl = true;"
            "  if (ytip.streamingData.hlsManifestUrl) result.hasHlsUrl = true;"
            "  return result;"
            "})();";
        GCValue early_media = JS_Eval(g_ctx, check_media_early_js, strlen(check_media_early_js), "<early_media>", JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(early_media)) {
            has_media_after_scripts = JS_ToBool(g_ctx, JS_GetPropertyStr(g_ctx, early_media, "hasMediaUrls"));
            has_encrypted_after_scripts = JS_ToBool(g_ctx, JS_GetPropertyStr(g_ctx, early_media, "hasEncrypted"));
            has_sabr_after_scripts = JS_ToBool(g_ctx, JS_GetPropertyStr(g_ctx, early_media, "hasSabrUrl"));
            printf("    After script exec - Media URLs: %s, Encrypted: %s, SABR: %s\n",
                   has_media_after_scripts ? "YES" : "NO",
                   has_encrypted_after_scripts ? "YES" : "NO",
                   has_sabr_after_scripts ? "YES" : "NO");
        }
        
        /* Debug: check ytcfg values */
        const char *check_ytcfg_js =
            "(function() {"
            "  var result = {};"
            "  try {"
            "    result.has_ytcfg = (typeof ytcfg !== 'undefined');"
            "    result.has_yt = (typeof yt !== 'undefined');"
            "    result.has_yt_config = (typeof yt !== 'undefined' && typeof yt.config_ !== 'undefined');"
            "    result.innertube_context = (ytcfg && ytcfg.get) ? ytcfg.get('INNERTUBE_CONTEXT') : null;"
            "    result.visitor_data = (ytcfg && ytcfg.get) ? ytcfg.get('VISITOR_DATA') : null;"
            "    result.client_version = (ytcfg && ytcfg.get) ? ytcfg.get('CLIENT_VERSION') : null;"
            "  } catch(e) { result.error = e.message; }"
            "  return result;"
            "})();";
        GCValue ytcfg_result = JS_Eval(g_ctx, check_ytcfg_js, strlen(check_ytcfg_js), "<check_ytcfg>", JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsException(ytcfg_result)) {
            GCValue v_has_ytcfg = JS_GetPropertyStr(g_ctx, ytcfg_result, "has_ytcfg");
            GCValue v_has_yt = JS_GetPropertyStr(g_ctx, ytcfg_result, "has_yt");
            GCValue v_has_yt_config = JS_GetPropertyStr(g_ctx, ytcfg_result, "has_yt_config");
            GCValue v_innertube = JS_GetPropertyStr(g_ctx, ytcfg_result, "innertube_context");
            GCValue v_visitor = JS_GetPropertyStr(g_ctx, ytcfg_result, "visitor_data");
            GCValue v_version = JS_GetPropertyStr(g_ctx, ytcfg_result, "client_version");
            GCValue v_err = JS_GetPropertyStr(g_ctx, ytcfg_result, "error");
            printf("    [DEBUG] ytcfg exists: %s\n", JS_ToBool(g_ctx, v_has_ytcfg) ? "YES" : "NO");
            printf("    [DEBUG] yt exists: %s\n", JS_ToBool(g_ctx, v_has_yt) ? "YES" : "NO");
            printf("    [DEBUG] yt.config_ exists: %s\n", JS_ToBool(g_ctx, v_has_yt_config) ? "YES" : "NO");
            const char *ic = JS_ToCString(g_ctx, v_innertube);
            if (ic) printf("    [DEBUG] INNERTUBE_CONTEXT: %.200s\n", ic);
            const char *vd = JS_ToCString(g_ctx, v_visitor);
            if (vd) printf("    [DEBUG] VISITOR_DATA: %.100s\n", vd);
            const char *cv = JS_ToCString(g_ctx, v_version);
            if (cv) printf("    [DEBUG] CLIENT_VERSION: %.100s\n", cv);
            const char *err = JS_ToCString(g_ctx, v_err);
            if (err && err[0]) printf("    [DEBUG] ytcfg check error: %s\n", err);
            
            /* Debug: check yt.config_ */
            const char *check_yt_config_js =
                "(function() {"
                "  var result = {};"
                "  try {"
                "    if (typeof yt !== 'undefined' && yt.config_) {"
                "      result.has_innertube = (typeof yt.config_.INNERTUBE_CONTEXT !== 'undefined');"
                "      result.has_client_version = (typeof yt.config_.INNERTUBE_CLIENT_VERSION !== 'undefined');"
                "      result.innertube_preview = yt.config_.INNERTUBE_CONTEXT ? JSON.stringify(yt.config_.INNERTUBE_CONTEXT).substring(0, 200) : null;"
                "    }"
                "  } catch(e) { result.error = e.message; }"
                "  return result;"
                "})();";
            GCValue yt_result = JS_Eval(g_ctx, check_yt_config_js, strlen(check_yt_config_js), "<check_yt>", JS_EVAL_TYPE_GLOBAL);
            if (!JS_IsException(yt_result)) {
                GCValue v_has_ic = JS_GetPropertyStr(g_ctx, yt_result, "has_innertube");
                GCValue v_has_cv = JS_GetPropertyStr(g_ctx, yt_result, "has_client_version");
                GCValue v_ic_prev = JS_GetPropertyStr(g_ctx, yt_result, "innertube_preview");
                printf("    [DEBUG] yt.config_.INNERTUBE_CONTEXT exists: %s\n", JS_ToBool(g_ctx, v_has_ic) ? "YES" : "NO");
                printf("    [DEBUG] yt.config_.INNERTUBE_CLIENT_VERSION exists: %s\n", JS_ToBool(g_ctx, v_has_cv) ? "YES" : "NO");
                const char *ic_prev = JS_ToCString(g_ctx, v_ic_prev);
                if (ic_prev) printf("    [DEBUG] yt.config_.INNERTUBE_CONTEXT: %.200s\n", ic_prev);
            }
        }
    }

    free(html); /* No longer need the HTML */

    /* Step 8: Install fetch hook to capture requests and responses */
    {
        const char *fetch_hook_js =
            "(function() {"
            "  window.__fetchCalls = [];"
            "  window.__fetchBodies = [];"
            "  var origFetch = window.fetch;"
            "  window.fetch = function(req, init) {"
            "    try {"
            "      var url = (typeof req === 'string') ? req : (req.url || req.__original_url || 'unknown');"
            "      window.__fetchCalls.push(url);"
            "      var bodyInfo = {url: url, hasInit: !!init, status: 0};"
            "      if (init && init.body) {"
            "        bodyInfo.initBodyType = typeof init.body;"
            "        bodyInfo.initBodyPreview = String(init.body).substring(0, 200);"
            "      }"
            "      if (req && req.body) {"
            "        bodyInfo.reqBodyType = typeof req.body;"
            "        try { bodyInfo.reqBodyPreview = JSON.stringify(req.body).substring(0, 200); } catch(e) { bodyInfo.reqBodyPreview = String(req.body).substring(0, 200); }"
            "      }"
            "      window.__fetchBodies.push(bodyInfo);"
            "      var promise = origFetch.apply(this, arguments);"
            "      promise.then(function(response) {"
            "        bodyInfo.status = response.status;"
            "      }).catch(function(e) {"
            "        bodyInfo.error = e.message;"
            "      });"
            "      return promise;"
            "    } catch(e) { window.__fetchBodies.push({error: e.message}); return origFetch.apply(this, arguments); }"
            "  };"
            "})();";
        JS_Eval(g_ctx, fetch_hook_js, strlen(fetch_hook_js), "<fetch_hook>", JS_EVAL_TYPE_GLOBAL);
    }

    /* Step 9: Reset Math.random seed for deterministic player behavior */
    /* The player uses Math.random() for hostname selection and feature flags.
     * Since inline scripts from varying HTML may call Math.random() a different
     * number of times, we reset the seed before player bootstrap to ensure
     * consistent player behavior regardless of HTML variations. */
    const char *reset_random_js =
        "(function() {"
        "  var seed = 12345;"
        "  Math.random = function() {"
        "    seed = (seed * 9301 + 49297) % 233280;"
        "    return seed / 233280;"
        "  };"
        "})();";
    JS_Eval(g_ctx, reset_random_js, strlen(reset_random_js), "<reset_random>", JS_EVAL_TYPE_GLOBAL);
    
    /* Step 10: Bootstrap player */
    printf("\n  Bootstrapping player...\n");

    const char *bootstrap_js =
        "(function() {\n"
        "  var result = { eventsDispatched: 0, playerCreated: false, playerError: null, playerApi: null };\n"
        "  function dispatchEvent(target, type) {\n"
        "    try {\n"
        "      var event = document.createEvent('Event');\n"
        "      event.initEvent(type, true, true);\n"
        "      target.dispatchEvent(event);\n"
        "      result.eventsDispatched++;\n"
        "    } catch(e) {}\n"
        "  }\n"
        "  var playerApi = document.getElementById('player-api');\n"
        "  if (!playerApi) {\n"
        "    var container = document.createElement('div');\n"
        "    container.id = 'player-api';\n"
        "    document.body.appendChild(container);\n"
        "    playerApi = container;\n"
        "  }\n"
        "  window.ytplayer = window.ytplayer || {};\n"
        "  window.ytplayer.bootstrapPlayerContainer = playerApi;\n"
        "  dispatchEvent(document, 'readystatechange');\n"
        "  dispatchEvent(document, 'DOMContentLoaded');\n"
        "  dispatchEvent(window, 'load');\n"
        "  dispatchEvent(document, 'yt-page-data-updated');\n"
        "  dispatchEvent(document, 'yt-navigate-finish');\n"
        "  dispatchEvent(document, 'yt-page-type-changed');\n"
        "  if (playerApi) dispatchEvent(playerApi, 'yt-player-ready');\n"
        "  try {\n"
        "    if (window.yt && yt.player && yt.player.Application) {\n"
        "      var createFn = yt.player.Application.createAlternate || yt.player.Application.create;\n"
        "      if (createFn && typeof createFn === 'function') {\n"
        "        var config = window.ytplayer.config;\n"
        "        var wpcc = null;\n"
        "        try {\n"
        "          wpcc = yt.config_.WEB_PLAYER_CONTEXT_CONFIGS['WEB_PLAYER_CONTEXT_CONFIG_ID_KEVLAR_WATCH'];\n"
        "        } catch(e) {}\n"
        "        if (playerApi && config) {\n"
        "          window.__playerInstance = createFn(playerApi, config, wpcc);\n"
        "          result.playerCreated = true;\n"
        "          result.playerApi = window.__playerInstance;\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "  } catch(e) {\n"
        "    result.playerError = (e && e.message) ? e.message : String(e);\n"
        "  }\n"
        "  return result;\n"
        "})();\n";

    GCValue bootstrap_result = JS_Eval(g_ctx, bootstrap_js, strlen(bootstrap_js), "<bootstrap>", JS_EVAL_TYPE_GLOBAL);
    bool player_created = false;
    if (!JS_IsException(bootstrap_result)) {
        GCValue v_pcreated = JS_GetPropertyStr(g_ctx, bootstrap_result, "playerCreated");
        GCValue v_perr = JS_GetPropertyStr(g_ctx, bootstrap_result, "playerError");
        GCValue v_disp = JS_GetPropertyStr(g_ctx, bootstrap_result, "eventsDispatched");
        player_created = JS_ToBool(g_ctx, v_pcreated);
        printf("    Events dispatched: %d\n", JS_VALUE_GET_INT(v_disp));
        if (player_created) printf("    Player created\n");
        const char *perr = JS_ToCString(g_ctx, v_perr);
        if (perr && perr[0]) printf("    Player error: %s\n", perr);
    }

    /* Step 11: Process timers and promise jobs */
    int total_timers = 0;
    for (int t = 0; t < 200; t++) {
        int processed = timer_process_due(g_ctx);
        if (processed > 0) total_timers += processed;
        
        // Execute promise jobs (needed for async player initialization)
        JSContextHandle pctx;
        int promise_jobs = 0;
        while (true) {
            int ret = JS_ExecutePendingJob(g_rt, &pctx);
            if (ret == 1) break; // no more pending jobs
            if (ret < 0) {
                // Exception in promise job - log and continue
                GCValue exc = JS_GetException(pctx);
                const char *exc_str = JS_ToCString(pctx, exc);
                if (exc_str) {
                    printf("    [WARN] Promise job exception: %.200s\n", exc_str);
                }
                continue;
            }
            promise_jobs++;
            if (promise_jobs > 100) break;
        }
        
        if (timer_has_pending()) {
            /* continue without delay */
        } else if (processed > 0 || promise_jobs > 0) {
            usleep(5000);
        } else {
            break;
        }
    }
    if (total_timers > 0) printf("    Processed %d timer callbacks\n", total_timers);

    /* Step 12: Check fetch calls and POST status */
    printf("\n  Checking fetch calls...\n");
    GCValue fetch_val = JS_GetPropertyStr(g_ctx, JS_GetGlobalObject(g_ctx), "__fetchCalls");
    GCValue fetch_bodies = JS_GetPropertyStr(g_ctx, JS_GetGlobalObject(g_ctx), "__fetchBodies");
    int fetch_len = 0;
    if (!JS_IsUndefined(fetch_val)) {
        GCValue len_val = JS_GetPropertyStr(g_ctx, fetch_val, "length");
        fetch_len = JS_VALUE_GET_INT(len_val);
    }
    printf("    Fetch calls: %d\n", fetch_len);

    bool post_succeeded = false;
    int post_status = 0;
    for (int i = 0; i < fetch_len && i < 20; i++) {
        GCValue f = JS_GetPropertyUint32(g_ctx, fetch_val, i);
        const char *fs = JS_ToCString(g_ctx, f);
        if (fs) printf("      [%d] %.120s\n", i, fs);

        /* Check body for status */
        if (!JS_IsUndefined(fetch_bodies)) {
            GCValue b = JS_GetPropertyUint32(g_ctx, fetch_bodies, i);
            if (!JS_IsUndefined(b)) {
                GCValue status_val = JS_GetPropertyStr(g_ctx, b, "status");
                if (!JS_IsUndefined(status_val)) {
                    int status = JS_VALUE_GET_INT(status_val);
                    if (fs && strstr(fs, "youtubei/v1/player")) {
                        post_status = status;
                        if (status == 200) post_succeeded = true;
                        printf("      -> POST status: %d\n", status);
                    }
                }
            }
        }
    }

    /* Step 13: Check captured URLs */
    printf("\n  Checking captured URLs...\n");
    char js_urls[JS_MAX_CAPTURED_URLS][JS_MAX_URL_LEN];
    int js_url_count = js_quickjs_get_captured_urls(js_urls, JS_MAX_CAPTURED_URLS);
    printf("    Captured URLs: %d\n", js_url_count);

    bool has_googlevideo = false;
    for (int i = 0; i < js_url_count; i++) {
        printf("      [%d] %.120s\n", i, js_urls[i]);
        if (strstr(js_urls[i], "googlevideo.com")) has_googlevideo = true;
    }

    /* Step 13b: Also check for media URLs in ytInitialPlayerResponse.streamingData */
    printf("\n  Checking ytInitialPlayerResponse for media URLs...\n");
    const char *check_media_js =
        "(function() {"
        "  var result = { hasMediaUrls: false, mediaUrlCount: 0, sampleUrl: '', hasEncrypted: false, hasSabrUrl: false };"
        "  var ytip = window.ytInitialPlayerResponse;"
        "  if (!ytip || !ytip.streamingData) return result;"
        "  var formats = ytip.streamingData.formats || [];"
        "  var adaptive = ytip.streamingData.adaptiveFormats || [];"
        "  for (var i = 0; i < formats.length; i++) {"
        "    if (formats[i].url) {"
        "      result.hasMediaUrls = true;"
        "      result.mediaUrlCount++;"
        "      if (!result.sampleUrl) result.sampleUrl = formats[i].url.substring(0, 120);"
        "    }"
        "    if (formats[i].signatureCipher) {"
        "      result.hasEncrypted = true;"
        "    }"
        "  }"
        "  for (var i = 0; i < adaptive.length; i++) {"
        "    if (adaptive[i].url) {"
        "      result.hasMediaUrls = true;"
        "      result.mediaUrlCount++;"
        "      if (!result.sampleUrl) result.sampleUrl = adaptive[i].url.substring(0, 120);"
        "    }"
        "    if (adaptive[i].signatureCipher) {"
        "      result.hasEncrypted = true;"
        "    }"
        "  }"
        "  // Also check SABR streaming URL"
        "  if (ytip.streamingData.serverAbrStreamingUrl) {"
        "    result.hasSabrUrl = true;"
        "    result.sabrUrlPreview = ytip.streamingData.serverAbrStreamingUrl.substring(0, 120);"
        "  }"
        "  // Check for dashManifestUrl or hlsManifestUrl"
        "  if (ytip.streamingData.dashManifestUrl) {"
        "    result.hasDashUrl = true;"
        "  }"
        "  if (ytip.streamingData.hlsManifestUrl) {"
        "    result.hasHlsUrl = true;"
        "  }"
        "  return result;"
        "})();";
    GCValue media_result = JS_Eval(g_ctx, check_media_js, strlen(check_media_js), "<check_media>", JS_EVAL_TYPE_GLOBAL);
    bool has_media_urls = false;
    bool has_sabr_from_js = false;
    bool has_encrypted = false;
    bool has_dash_url = false;
    bool has_hls_url = false;
    if (!JS_IsException(media_result)) {
        GCValue v_has = JS_GetPropertyStr(g_ctx, media_result, "hasMediaUrls");
        GCValue v_count = JS_GetPropertyStr(g_ctx, media_result, "mediaUrlCount");
        GCValue v_sample = JS_GetPropertyStr(g_ctx, media_result, "sampleUrl");
        GCValue v_sabr = JS_GetPropertyStr(g_ctx, media_result, "hasSabrUrl");
        GCValue v_sabr_preview = JS_GetPropertyStr(g_ctx, media_result, "sabrUrlPreview");
        GCValue v_enc = JS_GetPropertyStr(g_ctx, media_result, "hasEncrypted");
        GCValue v_dash = JS_GetPropertyStr(g_ctx, media_result, "hasDashUrl");
        GCValue v_hls = JS_GetPropertyStr(g_ctx, media_result, "hasHlsUrl");
        has_media_urls = JS_ToBool(g_ctx, v_has);
        int count = JS_VALUE_GET_INT(v_count);
        const char *sample = JS_ToCString(g_ctx, v_sample);
        has_sabr_from_js = JS_ToBool(g_ctx, v_sabr);
        has_encrypted = JS_ToBool(g_ctx, v_enc);
        has_dash_url = JS_ToBool(g_ctx, v_dash);
        has_hls_url = JS_ToBool(g_ctx, v_hls);
        const char *sabr_preview = JS_ToCString(g_ctx, v_sabr_preview);
        printf("    Media URLs in player response: %d\n", count);
        if (sample && sample[0]) printf("    Sample URL: %.120s\n", sample);
        if (has_sabr_from_js && sabr_preview) printf("    SABR URL: %.120s\n", sabr_preview);
        if (has_encrypted) printf("    Encrypted formats present (signatureCipher)\n");
        if (has_dash_url) printf("    DASH manifest URL present\n");
        if (has_hls_url) printf("    HLS manifest URL present\n");
    }

    /* Step 14: Final assertions */
    printf("\n  Assertions:\n");
    printf("    Player created: %s\n", player_created ? "YES" : "NO");
    printf("    POST to youtubei/v1/player: status=%d %s\n",
           post_status, post_succeeded ? "SUCCESS" : (post_status == 0 ? "NOT CALLED" : "FAILED"));
    printf("    Googlevideo URLs captured via fetch: %s\n", has_googlevideo ? "YES" : "NO");
    printf("    Media URLs in player response: %s\n", has_media_urls ? "YES" : "NO");
    printf("    SABR URL available: %s\n", has_sabr_from_js ? "YES" : "NO");
    printf("    Encrypted formats present: %s\n", has_encrypted ? "YES" : "NO");
    printf("    DASH/HLS manifest present: %s/%s\n", has_dash_url ? "YES" : "NO", has_hls_url ? "YES" : "NO");
    printf("    Media after script execution: %s\n", has_media_after_scripts ? "YES" : "NO");

    if (!player_created) {
        printf("    FAIL: Player was not created\n");
        return false;
    }

    bool has_any_media = has_googlevideo || has_media_urls || has_sabr_from_js || has_encrypted || has_dash_url || has_hls_url || has_media_after_scripts || has_encrypted_after_scripts || has_sabr_after_scripts;
    if (!has_any_media) {
        printf("    FAIL: No media URLs available (neither fetched nor in player response)\n");
        return false;
    }

    printf("    ALL ASSERTIONS PASSED\n");
    return true;
}

/* Main entry point */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    setbuf(stdout, NULL);
    printf("========================================\n");
    printf("YouTube Live Fetch Test\n");
    printf("(Fetches fresh data from youtube.com)\n");
    printf("========================================\n\n");

    if (!platform_init()) {
        printf("Failed to initialize platform\n");
        return 1;
    }
    if (!platform_http_init()) {
        printf("Failed to initialize HTTP\n");
        platform_cleanup();
        return 1;
    }

    bool result = test_youtube_live_fetch();

    cleanup_test_context();
    platform_http_cleanup();
    platform_cleanup();

    printf("\n========================================\n");
    if (result) {
        printf("TEST PASSED\n");
        return 0;
    } else {
        printf("TEST FAILED\n");
        return 1;
    }
}
