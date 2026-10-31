/* The parts of the web platform written in JS: NodeList, HTMLCollection
 * and the User Timing marks and measures (browser/platform_api.js); DOM
 * events (browser/dom_events.js); the form controls' IDL attributes
 * (browser/form_controls.js); <canvas> and its 2D context
 * (browser/canvas_api.js), then its WebGL and WebGPU contexts
 * (browser/webgpu_api.c). */
#include <string.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "platform.h"

static const char *platform_api_js =
#include "platform_api_js.inc"
;

static const char *dom_events_js =
#include "dom_events_js.inc"
;

static const char *form_controls_js =
#include "form_controls_js.inc"
;

static const char *cssom_js =
#include "cssom_js.inc"
;

static const char *canvas_api_js =
#include "canvas_api_js.inc"
;

static void platform_api_report(JSContextHandle ctx)
{
    GCValue e = JS_GetException(ctx);
    const char *m = JS_ToCString(ctx, e);
    GCValue st = JS_IsObject(e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
    const char *sm = JS_IsString(st) ? JS_ToCString(ctx, st) : NULL;
    platform_log(LOG_LEVEL_ERROR, "platform_api", "platform API setup failed: %s %s", m ? m : "?", sm ? sm : "");
}

/* A built-in script (the engine's own, compiled in), run in ctx: compiled
 * the first time in this process, its bytecode kept and read into every
 * runtime after that (a same-site navigation makes a new one,
 * page_engine_reset) rather than parsed again. */
GCValue builtin_eval(JSContextHandle ctx, const char *src, const char *name)
{
    static struct { const char *src; uint8_t *bc; size_t len; } cache[16];
    static int n;
    size_t slen = strlen(src);
    for (int i = 0; i < n; i++) {
        if (cache[i].src != src) continue;
        GCValue fn = JS_ReadObjectWithSource(ctx, cache[i].bc, cache[i].len, JS_READ_OBJ_BYTECODE, src, slen);
        if (JS_IsException(fn)) break;
        return JS_EvalFunction(ctx, fn);
    }
    GCValue fn = JS_Eval(ctx, src, slen, name, JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY | JS_EVAL_FLAG_NO_LAZY);
    if (JS_IsException(fn)) return fn;
    bool known = false;
    for (int i = 0; i < n; i++) known |= cache[i].src == src;
    if (!known && n < (int)(sizeof(cache) / sizeof(cache[0]))) {
        size_t len = 0;
        uint8_t *bc = JS_WriteObject(ctx, &len, fn, JS_WRITE_OBJ_BYTECODE | JS_WRITE_OBJ_SHARE_SOURCE);
        if (bc) {
            cache[n].src = src;
            cache[n].bc = bc;
            cache[n].len = len;
            n++;
        } else if (JS_HasException(ctx)) {
            JS_GetException(ctx);
        }
    }
    return JS_EvalFunction(ctx, fn);
}

static void platform_api_run(JSContextHandle ctx, const char *src, const char *name,
                             GCValue global, GCValue window)
{
    GCValue fn = builtin_eval(ctx, src, name);
    if (JS_IsException(fn)) { platform_api_report(ctx); return; }
    GCValue args[2] = { global, window };
    if (JS_IsException(JS_Call(ctx, fn, JS_UNDEFINED, 2, args))) platform_api_report(ctx);
}

void platform_api_install(JSContextHandle ctx, GCValue global, GCValue window)
{
    platform_api_run(ctx, platform_api_js, "<platform_api>", global, window);
    traversal_install(ctx, global);
    document_api_install(ctx, global);
    node_namespaces_install(ctx, global);
    event_path_install(ctx, global);
    platform_api_run(ctx, dom_events_js, "<dom_events>", global, window);
    platform_api_run(ctx, form_controls_js, "<form_controls>", global, window);
    platform_api_run(ctx, cssom_js, "<cssom>", global, window);
    platform_api_run(ctx, canvas_api_js, "<canvas_api>", global, window);
    dom_token_list_install(ctx, global, window);
    url_hyperlinks_install(ctx, global);
    webgpu_api_install(ctx, global, window);
}
