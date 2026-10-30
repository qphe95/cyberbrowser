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

static void platform_api_run(JSContextHandle ctx, const char *src, const char *name,
                             GCValue global, GCValue window)
{
    GCValue fn = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) { platform_api_report(ctx); return; }
    GCValue args[2] = { global, window };
    if (JS_IsException(JS_Call(ctx, fn, JS_UNDEFINED, 2, args))) platform_api_report(ctx);
}

void platform_api_install(JSContextHandle ctx, GCValue global, GCValue window)
{
    platform_api_run(ctx, platform_api_js, "<platform_api>", global, window);
    platform_api_run(ctx, dom_events_js, "<dom_events>", global, window);
    platform_api_run(ctx, form_controls_js, "<form_controls>", global, window);
    platform_api_run(ctx, cssom_js, "<cssom>", global, window);
    platform_api_run(ctx, canvas_api_js, "<canvas_api>", global, window);
    webgpu_api_install(ctx, global, window);
}
