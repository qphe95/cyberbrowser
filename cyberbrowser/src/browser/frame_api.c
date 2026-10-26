/* Same-origin <iframe> (HTML section 4.8.5 and 7.3, "nested browsing
 * contexts"): each frame's document lives in a realm of its own, a JS
 * context of the one runtime with the whole browser API installed, whose
 * global object is the frame's window.
 *
 * Setting an iframe's src queues a navigation; the event loop runs it
 * (frame_api_process, from timer_process_due): the page is fetched,
 * parsed into the frame's document with its scripts run in the frame's
 * realm (html_page_stream_begin_frame), DOMContentLoaded and load fire in
 * the frame, then load fires on the iframe element.  A frame whose iframe
 * leaves its document is discarded: its timers are cleared and its realm
 * unrooted.
 *
 * Cross-origin frames are not loaded (their contentWindow stays null).
 * Nodes are wrapped in their document's realm (dom_wrap_set_realm_resolver),
 * so a parent reaching into a frame gets the frame's objects. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "dom_store.h"
#include "dom_wrap.h"
#include "http_download.h"
#include "html_media_extract.h"
#include "js_quickjs.h"
#include "platform.h"
#include "url_whatwg.h"

extern JSContextHandle g_js_context;
extern const char *g_cyber_start_url;
void timer_cancel_realm(JSContextHandle realm);

typedef enum { FRAME_EMPTY, FRAME_PENDING, FRAME_LOADED } FrameState;

typedef struct Frame {
    struct Frame *next;
    NodeId iframe;              /* the <iframe> element, in the parent's document */
    JSContextHandle parent;     /* the parent's realm */
    JSContextHandle ctx;        /* the frame's realm (rooted), or null */
    NodeId doc;                 /* the frame's document */
    FrameState state;
    char url[2048];             /* the navigation's (or the loaded page's) URL */
} Frame;

static Frame *g_frames;
static bool g_processing;
static NodeId g_main_doc;       /* the page's document (g_dom_document changes while a frame's realm is made) */

static Frame *frame_of_iframe(NodeId iframe) {
    for (Frame *f = g_frames; f; f = f->next) if (f->iframe == iframe) return f;
    return NULL;
}

static JSContextHandle realm_of_document(NodeId doc) {
    for (Frame *f = g_frames; f; f = f->next) if (f->doc == doc && SAFE_VALID(f->ctx)) return f->ctx;
    if (doc == g_main_doc) return g_js_context;
    return GC_HANDLE_NULL;
}

/* The URL a realm's document has (its location), for resolving. */
static char *realm_url(JSContextHandle ctx) {
    GCValue loc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "location");
    GCValue href = JS_IsObject(loc) ? JS_GetPropertyStr(ctx, loc, "href") : JS_UNDEFINED;
    const char *s = JS_IsString(href) ? JS_ToCString(ctx, href) : NULL;
    char *out = strdup(s && s[0] ? s : (g_cyber_start_url ? g_cyber_start_url : "about:blank"));
    return out;
}

static bool same_origin(const char *a, const char *b) {
    WUrl ua, ub;
    if (wurl_parse(a, strlen(a), NULL, &ua) != 0) return false;
    if (wurl_parse(b, strlen(b), NULL, &ub) != 0) { wurl_free(&ua); return false; }
    char *oa = wurl_origin(&ua), *ob = wurl_origin(&ub);
    bool same = oa && ob && strcmp(oa, ob) == 0 && strcmp(oa, "null") != 0;
    free(oa); free(ob);
    wurl_free(&ua); wurl_free(&ub);
    return same;
}

static void frame_discard_realm(Frame *f) {
    if (!SAFE_VALID(f->ctx)) return;
    timer_cancel_realm(f->ctx);
    gc_remove_root((GCHandle)f->ctx);
    f->ctx = GC_HANDLE_NULL;
    f->doc = DOM_NULL;
}

static JSContextHandle frame_new_realm(Frame *f) {
    JSRuntimeHandle rt = JS_GetRuntime(f->parent);
    JSContextHandle ctx = JS_NewContext(rt);
    if (!SAFE_VALID(ctx)) return GC_HANDLE_NULL;
    gc_add_root((GCHandle)ctx);
    JS_AddIntrinsicBaseObjects(ctx);
    JS_AddIntrinsicEval(ctx);
    JS_AddIntrinsicRegExp(ctx);
    JS_AddIntrinsicJSON(ctx);
    JS_AddIntrinsicPromise(ctx);
    JS_AddIntrinsicMapSet(ctx);
    JS_AddIntrinsicTypedArrays(ctx);
    JS_AddIntrinsicProxy(ctx);
    JS_AddIntrinsicWeakRef(ctx);
    /* The API installs a new document as the main one: keep the page's. */
    NodeId main_doc = g_dom_document;
    init_browser_api_impl(ctx, JS_GetGlobalObject(ctx));
    f->doc = g_dom_document;
    g_dom_document = main_doc;
    f->ctx = ctx;
    return ctx;
}

static const char frame_window_js[] =
    "(function (url, parent, top, frameElement) {"
    "  var w = globalThis;"
    "  if (w.location) w.location.href = url;"
    "  if (document) { try { document.domain = w.location.hostname; } catch (e) {}"
    "                  try { document.baseURI = w.location.href; } catch (e) {} }"
    "  function ro(name, v) { Object.defineProperty(w, name, { value: v, configurable: true, writable: false }); }"
    "  ro('parent', parent); ro('top', top || parent); ro('frameElement', frameElement);"
    "  function px(v) { var n = parseFloat(v); return isFinite(n) ? n : NaN; }"
    "  function size(dim, attr, dflt) {"
    "    var f = frameElement, n = NaN;"
    "    if (f) { n = px(f.style && f.style[dim]); if (isNaN(n)) n = px(f.getAttribute(attr)); }"
    "    return isNaN(n) ? dflt : Math.round(n);"
    "  }"
    "  Object.defineProperty(w, 'innerWidth', { get: function () { return size('width', 'width', 300); }, configurable: true });"
    "  Object.defineProperty(w, 'innerHeight', { get: function () { return size('height', 'height', 150); }, configurable: true });"
    "  Object.defineProperty(w, 'outerWidth', { get: function () { return w.innerWidth; }, configurable: true });"
    "  Object.defineProperty(w, 'outerHeight', { get: function () { return w.innerHeight; }, configurable: true });"
    "})";

/* The frame's window: its location, its place among windows (parent, top,
 * frameElement) and a viewport the iframe's size. */
static void frame_setup_window(Frame *f) {
    JSContextHandle ctx = f->ctx;
    GCValue win = JS_GetGlobalObject(ctx);
    GCValue parent_win = JS_GetGlobalObject(f->parent);
    GCValue top = JS_GetPropertyStr(f->parent, parent_win, "top");
    GCValue iframe = dom_js(f->parent, f->iframe);
    GCValue setup = JS_Eval(ctx, frame_window_js, sizeof(frame_window_js) - 1, "<frame_window>",
                            JS_EVAL_TYPE_GLOBAL);
    if (JS_IsFunction(ctx, setup)) {
        GCValue args[4] = { JS_NewString(ctx, f->url), parent_win, top, iframe };
        GCValue r = JS_Call(ctx, setup, JS_UNDEFINED, 4, args);
        if (JS_IsException(r)) JS_GetException(ctx);
    } else if (JS_IsException(setup)) {
        JS_GetException(ctx);
    }
    (void)win;
}

static void frame_fire(JSContextHandle ctx, GCValue target, const char *type) {
    GCValue ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "Event");
    if (!JS_IsFunction(ctx, ctor) || !JS_IsObject(target)) return;
    GCValue tv = JS_NewString(ctx, type);
    GCValue ev = JS_CallConstructor(ctx, ctor, 1, &tv);
    if (!JS_IsObject(ev)) { JS_GetException(ctx); return; }
    js_event_target_dispatchEvent(ctx, target, 1, &ev);
}

/* Navigate f to f->url: a new realm and document with the page loaded. */
static void frame_load(Frame *f) {
    char url[2048];
    snprintf(url, sizeof(url), "%s", f->url);
    frame_discard_realm(f);
    f->state = FRAME_LOADED;
    char *parent_url = realm_url(f->parent);
    bool allowed = same_origin(parent_url, url);
    free(parent_url);
    if (!allowed) {
        platform_log(LOG_LEVEL_INFO, "frame", "not loading cross-origin frame %s", url);
        frame_fire(f->parent, dom_js(f->parent, f->iframe), "load");
        return;
    }
    HttpBuffer buf = {0};
    char err[256] = {0};
    bool fetched = http_get_to_memory(url, &buf, err, sizeof(err));
    if (!frame_new_realm(f)) {
        http_free_buffer(&buf);
        return;
    }
    frame_setup_window(f);
    JSContextHandle ctx = f->ctx;
    HtmlPageStream *ps = html_page_stream_begin_frame(ctx, url);
    if (ps) {
        if (fetched && buf.data && buf.size) html_page_stream_feed(ps, buf.data, buf.size);
        else platform_log(LOG_LEVEL_WARN, "frame", "could not fetch frame %s: %s", url, err);
        html_page_stream_finish(ps, NULL);
    }
    http_free_buffer(&buf);
    if (!SAFE_VALID(f->ctx)) return;    /* (a script removed the frame) */
    static const char lifecycle_js[] =
        "document.readyState = 'interactive';"
        "document.dispatchEvent(new Event('DOMContentLoaded', { bubbles: true }));"
        "document.readyState = 'complete';"
        "window.dispatchEvent(new Event('load'));";
    GCValue r = JS_Eval(ctx, lifecycle_js, sizeof(lifecycle_js) - 1, "<frame_lifecycle>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) JS_GetException(ctx);
    frame_fire(f->parent, dom_js(f->parent, f->iframe), "load");
}

/* Queue a navigation of iframe to url (resolved), run by the event loop. */
static void frame_navigate(JSContextHandle parent, NodeId iframe, const char *url) {
    Frame *f = frame_of_iframe(iframe);
    if (!f) {
        f = (Frame *)calloc(1, sizeof(*f));
        if (!f) return;
        f->iframe = iframe;
        f->next = g_frames;
        g_frames = f;
    }
    f->parent = parent;
    snprintf(f->url, sizeof(f->url), "%s", url);
    f->state = FRAME_PENDING;
}

int frame_api_process(void) {
    if (g_processing || !g_frames) return 0;
    g_processing = true;
    int done = 0;
    for (Frame **pp = &g_frames; *pp; ) {
        Frame *f = *pp;
        if (!dom_valid(f->iframe) || !dom_is_connected(f->iframe)) {
            /* the iframe left its document: its browsing context goes */
            frame_discard_realm(f);
            *pp = f->next;
            free(f);
            continue;
        }
        if (f->state == FRAME_PENDING) {
            frame_load(f);
            done++;
            pp = &g_frames;         /* (scripts may have changed the list) */
            continue;
        }
        pp = &f->next;
    }
    g_processing = false;
    return done;
}

bool frame_api_pending(void) {
    for (Frame *f = g_frames; f; f = f->next) if (f->state == FRAME_PENDING) return true;
    return false;
}

/* ---- HTMLIFrameElement ---- */

static GCValue js_iframe_get_content_window(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    Frame *f = frame_of_iframe(dom_node_of(this_val));
    (void)ctx;
    return f && SAFE_VALID(f->ctx) ? JS_GetGlobalObject(f->ctx) : JS_NULL;
}

static GCValue js_iframe_get_content_document(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    Frame *f = frame_of_iframe(dom_node_of(this_val));
    (void)ctx;
    if (!f || !SAFE_VALID(f->ctx)) return JS_NULL;
    return JS_GetPropertyStr(f->ctx, JS_GetGlobalObject(f->ctx), "document");
}

static GCValue js_iframe_get_src(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    const char *src = dom_get_attr_str(n, "src");
    if (!src) return JS_NewString(ctx, "");
    char *base = realm_url(ctx);
    char *abs = wurl_resolve(base, src);
    free(base);
    GCValue r = JS_NewString(ctx, abs ? abs : src);
    free(abs);
    return r;
}

static GCValue js_iframe_set_src(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    NodeId n = dom_node_of(this_val);
    GCValue v = argc > 0 ? argv[0] : JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, v);
    if (!s) return JS_EXCEPTION;
    GCValue set = JS_GetPropertyStr(ctx, this_val, "setAttribute");
    GCValue args[2] = { JS_NewString(ctx, "src"), JS_NewString(ctx, s) };
    if (JS_IsFunction(ctx, set)) {
        GCValue r = JS_Call(ctx, set, this_val, 2, args);
        if (JS_IsException(r)) return r;
    }
    if (n && dom_is_connected(n)) {
        char *base = realm_url(ctx);
        char *abs = wurl_resolve(base, s);
        free(base);
        frame_navigate(ctx, n, abs ? abs : s);
        free(abs);
    }
    return JS_UNDEFINED;
}

void frame_api_install(JSContextHandle ctx, GCValue global) {
    if (!g_main_doc) g_main_doc = g_dom_document;
    dom_wrap_set_realm_resolver(realm_of_document);
    GCValue ctor = JS_GetPropertyStr(ctx, global, "HTMLIFrameElement");
    GCValue proto = JS_IsObject(ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
    if (!JS_IsObject(proto)) return;
    static const struct { const char *name; JSCFunction *get; } getters[] = {
        { "contentWindow", js_iframe_get_content_window },
        { "contentDocument", js_iframe_get_content_document },
    };
    for (size_t i = 0; i < sizeof(getters) / sizeof(getters[0]); i++) {
        JSAtom a = JS_NewAtom(ctx, getters[i].name);
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction(ctx, getters[i].get, getters[i].name, 0),
                                JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
    JSAtom src = JS_NewAtom(ctx, "src");
    JS_DefinePropertyGetSet(ctx, proto, src, JS_NewCFunction(ctx, js_iframe_get_src, "get src", 0),
                            JS_NewCFunction(ctx, js_iframe_set_src, "set src", 1),
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, src);
}
