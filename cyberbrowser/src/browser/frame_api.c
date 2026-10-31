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
 * Frames of other origins load too, in the page's process
 * (SANDBOX_DESIGN.md section 9), behind the same-origin policy: across
 * origins a window is a stand-in made in the realm that looks
 * (cross_window), with postMessage, location (to navigate), parent, top,
 * frames, length and closed, and nothing else; contentDocument is null and
 * frameElement too.  Messages are queued and delivered by the event loop,
 * as a JSON clone, with the sender's origin and its stand-in as source.
 * Their requests say they are frame documents (HTTP_MODE_FRAME), and their
 * localStorage is their own origin's (storage_api.c), which the tab keeps
 * under the page's site.
 *
 * Nodes are wrapped in their document's realm (dom_wrap_set_realm_resolver),
 * so a parent reaching into a same-origin frame gets the frame's objects. */
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
#include "xml_parse.h"

extern JSContextHandle g_js_context;
extern const char *g_cyber_start_url;
void timer_cancel_realm(JSContextHandle realm);

typedef enum { FRAME_EMPTY, FRAME_PENDING, FRAME_LOADED } FrameState;

typedef struct Frame {
    struct Frame *next;
    uint32_t id;                /* its window's, for cross_window (the page's is 1) */
    GCValue factory;            /* its realm's cross_window maker (rooted when factory_made) */
    bool factory_made;
    NodeId iframe;              /* the <iframe> element, in the parent's document */
    JSContextHandle parent;     /* the parent's realm */
    JSContextHandle ctx;        /* the frame's realm (rooted), or null */
    NodeId doc;                 /* the frame's document */
    FrameState state;
    char url[2048];             /* the navigation's (or the loaded page's) URL */
} Frame;

static Frame *g_frames;
static uint32_t g_next_id = 2;
static GCValue g_main_factory;      /* the page realm's cross_window maker */
static bool g_main_factory_made;
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

static void root_value(GCValue v, bool add) {
    if (!GC_IS_REFERENCE(v)) return;
    GCHandle h = GC_VALUE_GET_HANDLE(v);
    if (h == GC_HANDLE_NULL) return;
    if (add) gc_add_root(h); else gc_remove_root(h);
}

static void frame_discard_realm(Frame *f) {
    if (!SAFE_VALID(f->ctx)) return;
    if (f->factory_made) root_value(f->factory, false);
    f->factory_made = false;
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

/* ---- windows across origins ---- */

#define PAGE_WINDOW 1u

static Frame *frame_of_realm(JSContextHandle ctx) {
    for (Frame *f = g_frames; f; f = f->next) if (SAFE_VALID(f->ctx) && f->ctx == ctx) return f;
    return NULL;
}

static uint32_t window_id(JSContextHandle ctx) {
    if (ctx == g_js_context) return PAGE_WINDOW;
    Frame *f = frame_of_realm(ctx);
    return f ? f->id : 0;
}

static JSContextHandle window_realm(uint32_t id) {
    if (id == PAGE_WINDOW) return g_js_context;
    for (Frame *f = g_frames; f; f = f->next) if (f->id == id) return SAFE_VALID(f->ctx) ? f->ctx : GC_HANDLE_NULL;
    return GC_HANDLE_NULL;
}

static Frame *frame_of_window(uint32_t id) {
    for (Frame *f = g_frames; f; f = f->next) if (f->id == id) return f;
    return NULL;
}

/* The URL a realm's origin comes from: an about:blank (or about:srcdoc)
 * frame has its creator's (HTML 7.1.5), here its parent's. */
static char *origin_url(JSContextHandle ctx) {
    for (int depth = 0; depth < 32; depth++) {
        char *u = realm_url(ctx);
        Frame *f = strncmp(u, "about:", 6) ? NULL : frame_of_realm(ctx);
        if (!f || !SAFE_VALID(f->parent)) return u;
        free(u);
        ctx = f->parent;
    }
    return strdup("about:blank");
}

static bool realms_same_origin(JSContextHandle a, JSContextHandle b) {
    char *ua = origin_url(a), *ub = origin_url(b);
    bool same = same_origin(ua, ub);
    free(ua);
    free(ub);
    return same;
}

static char *realm_origin(JSContextHandle ctx) {
    char *u = origin_url(ctx);
    WUrl w;
    char *o = NULL;
    if (wurl_parse(u, strlen(u), NULL, &w) == 0) {
        o = wurl_origin(&w);
        wurl_free(&w);
    }
    free(u);
    return o ? o : strdup("null");
}

static GCValue window_seen_from(JSContextHandle ctx, uint32_t id);

/* What a realm makes its stand-ins with.  op(kind, id, a, b) is the native
 * side (cross_op); it reaches the factory only, never a global. */
static const char cross_window_js[] =
    "(function (op) {"
    "  var made = new Map();"
    "  function security() {"
    "    var E = typeof DOMException === 'function' ? DOMException : Error;"
    "    return new E('Blocked a frame from accessing a cross-origin frame.', 'SecurityError');"
    "  }"
    "  return function (id) {"
    "    var w = made.get(id);"
    "    if (w) return w;"
    "    w = {};"
    "    var loc = {};"
    "    function go(u) { op(1, id, String(u)); }"
    "    Object.defineProperty(loc, 'href', { set: go, get: function () { throw security(); }, enumerable: true });"
    "    Object.defineProperty(loc, 'replace', { value: go, enumerable: true });"
    "    Object.freeze(loc);"
    "    function self() { return w; }"
    "    function nothing() {}"
    "    Object.defineProperties(w, {"
    "      postMessage: { value: function postMessage(message, options) {"
    "        var to = typeof options === 'object' && options !== null ? options.targetOrigin : options;"
    "        if (to === undefined) to = '/';"
    "        op(0, id, JSON.stringify(message === undefined ? null : message), String(to));"
    "      }, enumerable: true },"
    "      location: { get: function () { return loc; }, set: go, enumerable: true },"
    "      window: { get: self, enumerable: true }, self: { get: self, enumerable: true },"
    "      frames: { get: self, enumerable: true },"
    "      parent: { get: function () { return op(2, id); }, enumerable: true },"
    "      top: { get: function () { return op(3, id); }, enumerable: true },"
    "      length: { get: function () { return op(4, id); }, enumerable: true },"
    "      closed: { get: function () { return op(5, id); }, enumerable: true },"
    "      opener: { value: null, enumerable: true },"
    "      close: { value: nothing }, focus: { value: nothing }, blur: { value: nothing }"
    "    });"
    "    Object.freeze(w);"
    "    made.set(id, w);"
    "    return w;"
    "  };"
    "})";

/* A message on its way to a window: delivered by frame_api_process. */
typedef struct Message {
    struct Message *next;
    uint32_t to, from;
    char *json, *target_origin, *origin;
} Message;

static Message *g_messages, **g_messages_tail = &g_messages;

static void message_free(Message *m) {
    free(m->json);
    free(m->target_origin);
    free(m->origin);
    free(m);
}

static GCValue cross_op(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    int32_t kind = 0, id = 0;
    if (argc < 2 || JS_ToInt32(ctx, &kind, argv[0]) || JS_ToInt32(ctx, &id, argv[1])) return JS_UNDEFINED;
    JSContextHandle target = window_realm((uint32_t)id);
    Frame *tf = frame_of_window((uint32_t)id);
    switch (kind) {
    case 0: {                                   /* postMessage(json, targetOrigin) */
        if (argc < 4 || !SAFE_VALID(target)) return JS_UNDEFINED;
        const char *json = JS_ToCString(ctx, argv[2]), *to = JS_ToCString(ctx, argv[3]);
        Message *m = (Message *)calloc(1, sizeof(*m));
        if (m && json && to) {
            m->to = (uint32_t)id;
            m->from = window_id(ctx);
            m->json = strdup(json);
            m->target_origin = strdup(to);
            m->origin = realm_origin(ctx);
            *g_messages_tail = m;
            g_messages_tail = &m->next;
        } else {
            free(m);
        }
        if (json) JS_FreeCString(ctx, json);
        if (to) JS_FreeCString(ctx, to);
        return JS_UNDEFINED;
    }
    case 1: {                                   /* location = url: a frame navigates */
        if (argc < 3 || !tf || !SAFE_VALID(tf->parent)) return JS_UNDEFINED;
        const char *u = JS_ToCString(ctx, argv[2]);
        if (!u) return JS_EXCEPTION;
        char *base = realm_url(ctx);
        char *abs = wurl_resolve(base, u);
        free(base);
        snprintf(tf->url, sizeof(tf->url), "%s", abs ? abs : u);
        tf->state = FRAME_PENDING;
        free(abs);
        JS_FreeCString(ctx, u);
        return JS_UNDEFINED;
    }
    case 2:                                     /* parent */
    case 3: {                                   /* top */
        if (!tf) return window_seen_from(ctx, PAGE_WINDOW);         /* the page is its own */
        if (kind == 3) return window_seen_from(ctx, PAGE_WINDOW);
        return window_seen_from(ctx, window_id(tf->parent));
    }
    case 4: {                                   /* length: its frames */
        int n = 0;
        for (Frame *f = g_frames; f; f = f->next) if (SAFE_VALID(target) && f->parent == target) n++;
        return JS_NewInt32(ctx, n);
    }
    case 5:                                     /* closed */
        return SAFE_VALID(target) ? JS_FALSE : JS_TRUE;
    }
    return JS_UNDEFINED;
}

/* Window id as realm ctx may see it: itself if same-origin, else a
 * stand-in made in ctx. */
static GCValue window_seen_from(JSContextHandle ctx, uint32_t id) {
    JSContextHandle target = window_realm(id);
    if (!id || !SAFE_VALID(target)) return JS_NULL;
    if (realms_same_origin(ctx, target)) return JS_GetGlobalObject(target);
    GCValue *factory;
    bool *made;
    Frame *own = frame_of_realm(ctx);
    if (ctx == g_js_context) { factory = &g_main_factory; made = &g_main_factory_made; }
    else if (own) { factory = &own->factory; made = &own->factory_made; }
    else return JS_NULL;
    if (!*made) {
        GCValue mk = JS_Eval(ctx, cross_window_js, sizeof(cross_window_js) - 1, "<cross_window>", JS_EVAL_TYPE_GLOBAL);
        if (!JS_IsFunction(ctx, mk)) { JS_GetException(ctx); return JS_NULL; }
        GCValue op = JS_NewCFunction(ctx, cross_op, "op", 4);
        GCValue f = JS_Call(ctx, mk, JS_UNDEFINED, 1, &op);
        if (!JS_IsFunction(ctx, f)) { JS_GetException(ctx); return JS_NULL; }
        *factory = f;
        root_value(f, true);
        *made = true;
    }
    GCValue arg = JS_NewInt32(ctx, (int32_t)id);
    GCValue w = JS_Call(ctx, *factory, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(w)) { JS_GetException(ctx); return JS_NULL; }
    return w;
}

static const char deliver_js[] =
    "(function (json, origin, source) {"
    "  var data = null;"
    "  try { data = JSON.parse(json); } catch (e) {}"
    "  var ME = typeof MessageEvent === 'function' ? MessageEvent : null;"
    "  var ev = ME ? new ME('message', { data: data, origin: origin, source: source }) : new Event('message');"
    "  if (!ME) { ev.data = data; ev.origin = origin; ev.source = source; }"
    "  dispatchEvent(ev);"
    "})";

/* Deliver the messages queued so far; a message to a window that is gone,
 * or whose origin isn't the one the sender named, is dropped. */
static int deliver_messages(void) {
    Message *list = g_messages;
    g_messages = NULL;
    g_messages_tail = &g_messages;
    int n = 0;
    while (list) {
        Message *m = list;
        list = m->next;
        JSContextHandle ctx = window_realm(m->to);
        if (SAFE_VALID(ctx)) {
            char *origin = realm_origin(ctx);
            bool to_ok = !strcmp(m->target_origin, "*") ||
                         (!strcmp(m->target_origin, "/") ? !strcmp(origin, m->origin)
                                                         : !strcmp(m->target_origin, origin));
            free(origin);
            if (to_ok) {
                GCValue fn = JS_Eval(ctx, deliver_js, sizeof(deliver_js) - 1, "<message>", JS_EVAL_TYPE_GLOBAL);
                if (JS_IsFunction(ctx, fn)) {
                    GCValue args[3] = { JS_NewString(ctx, m->json), JS_NewString(ctx, m->origin),
                                        window_seen_from(ctx, m->from) };
                    GCValue r = JS_Call(ctx, fn, JS_UNDEFINED, 3, args);
                    if (JS_IsException(r)) JS_GetException(ctx);
                } else if (JS_IsException(fn)) {
                    JS_GetException(ctx);
                }
                n++;
            }
        }
        message_free(m);
    }
    return n;
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
 * frameElement: stand-ins and null across origins) and a viewport the
 * iframe's size. */
static void frame_setup_window(Frame *f) {
    JSContextHandle ctx = f->ctx;
    GCValue win = JS_GetGlobalObject(ctx);
    GCValue loc = JS_GetPropertyStr(ctx, win, "location");
    if (JS_IsObject(loc)) JS_SetPropertyStr(ctx, loc, "href", JS_NewString(ctx, f->url));
    bool same = realms_same_origin(ctx, f->parent);
    GCValue parent_win = window_seen_from(ctx, window_id(f->parent));
    GCValue top = window_seen_from(ctx, PAGE_WINDOW);
    GCValue iframe = same ? dom_js(f->parent, f->iframe) : JS_NULL;
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

/* The MIME type of a response's Content-Type (lower-cased; "" if none). */
static void response_mime(const char *headers, char *out, size_t cap) {
    out[0] = 0;
    if (!headers) return;
    for (const char *p = headers; *p; p++) {
        if ((p == headers || p[-1] == '\n') && !strncasecmp(p, "content-type:", 13)) {
            p += 13;
            while (*p == ' ' || *p == '\t') p++;
            size_t n = 0;
            while (p[n] && p[n] != ';' && p[n] != '\r' && p[n] != '\n' && p[n] != ' ' && n + 1 < cap) {
                out[n] = (char)(p[n] >= 'A' && p[n] <= 'Z' ? p[n] + 32 : p[n]);
                n++;
            }
            out[n] = 0;
            return;
        }
    }
}

/* Is it an XML MIME type (text/xml, application/xml, ...+xml)? */
static bool xml_mime(const char *m) {
    size_t n = strlen(m);
    return !strcmp(m, "text/xml") || !strcmp(m, "application/xml") || (n > 4 && !strcmp(m + n - 4, "+xml"));
}

/* The frame's document as the XML document these bytes make (its realm's
 * document emptied first; a parsererror root if not well-formed). */
static void frame_load_xml(JSContextHandle ctx, const char *data, size_t n, const char *mime) {
    GCValue doc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document");
    NodeId d = dom_node_of(doc);
    if (dom_kind(d) != DOM_K_DOCUMENT) return;
    while (dom_first_child(d)) {
        NodeId c = dom_first_child(d);
        dom_unlink(c);
    }
    g_dom.flags[dom_slot(d)] |= DOMF_XML_DOCUMENT;
    /* (its own documentElement, head and body were the skeleton's) */
    static const char *const own[] = { "documentElement", "head", "body", "contentType" };
    for (size_t i = 0; i < sizeof own / sizeof own[0]; i++) {
        JSAtom a = JS_NewAtom(ctx, own[i]);
        JS_DeleteProperty(ctx, doc, a, 0);
        JS_FreeAtom(ctx, a);
    }
    JS_DefinePropertyValueStr(ctx, doc, "contentType", JS_NewString(ctx, mime), JS_PROP_CONFIGURABLE);
    char err[256];
    if (!dom_parse_xml(data ? data : "", data ? n : 0, d, err, sizeof err)) {
        while (dom_first_child(d)) dom_unlink(dom_first_child(d));
        static const char ns[] = "http://www.mozilla.org/newlayout/xml/parsererror.xml";
        NodeId pe = dom_create_element_str("parsererror", 11, d);
        dom_set_element_name(pe, dom_ns_of(ns, sizeof ns - 1), NULL, 0, "parsererror", 11);
        dom_link_append(pe, dom_create_chardata(DOM_K_TEXT, err, strlen(err), d));
        dom_link_append(d, pe);
    }
}

/* Navigate f to f->url: a new realm and document with the page loaded. */
static void frame_load(Frame *f) {
    char url[2048];
    snprintf(url, sizeof(url), "%s", f->url);
    frame_discard_realm(f);
    f->state = FRAME_LOADED;
    /* A frame document, asked for by the parent's origin (which the network
     * process lets through ORB: section 9). */
    char *parent_origin = realm_origin(f->parent);
    HttpRequestContext was;
    http_request_context_get(&was);
    http_request_context_set(HTTP_MODE_FRAME, parent_origin);
    free(parent_origin);
    HttpBuffer buf = {0};
    char err[256] = {0};
    bool blank = !strncmp(url, "about:", 6);         /* an empty document, from nowhere */
    bool fetched = !blank && http_get_to_memory(url, &buf, err, sizeof(err));
    http_request_context_set(was.mode, was.initiator);
    /* The frame's document is where it ended, after redirects: its origin,
     * location and base URL are that URL's. */
    if (fetched && http_last_url()[0]) {
        snprintf(url, sizeof(url), "%s", http_last_url());
        snprintf(f->url, sizeof(f->url), "%s", url);
    }
    if (!frame_new_realm(f)) {
        http_free_buffer(&buf);
        return;
    }
    frame_setup_window(f);
    JSContextHandle ctx = f->ctx;
    char mime[128];
    response_mime(fetched ? buf.headers : NULL, mime, sizeof mime);
    HtmlPageStream *ps = NULL;
    if (fetched && xml_mime(mime)) frame_load_xml(ctx, buf.data, buf.size, mime);
    else ps = html_page_stream_begin_frame(ctx, url);
    if (ps) {
        if (fetched && buf.data && buf.size) html_page_stream_feed(ps, buf.data, buf.size);
        else if (!blank) platform_log(LOG_LEVEL_WARN, "frame", "could not fetch frame %s: %s", url, err);
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
        f->id = g_next_id++;
        f->factory = JS_UNDEFINED;
        f->next = g_frames;
        g_frames = f;
    }
    f->parent = parent;
    snprintf(f->url, sizeof(f->url), "%s", url);
    f->state = FRAME_PENDING;
}

int frame_api_process(void) {
    if (g_processing || (!g_frames && !g_messages)) return 0;
    g_processing = true;
    int done = deliver_messages();
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
    return g_messages != NULL;
}

/* ---- HTMLIFrameElement ---- */

static GCValue js_iframe_get_content_window(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    Frame *f = frame_of_iframe(dom_node_of(this_val));
    return f && SAFE_VALID(f->ctx) ? window_seen_from(ctx, f->id) : JS_NULL;
}

static GCValue js_iframe_get_content_document(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    Frame *f = frame_of_iframe(dom_node_of(this_val));
    if (!f || !SAFE_VALID(f->ctx) || !realms_same_origin(ctx, f->ctx)) return JS_NULL;
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

bool g_frame_blank_now;

/* A subtree was inserted into a connected tree (parsed, or by script):
 * each iframe in it without a browsing context yet navigates to its src, or
 * to about:blank (at once when g_frame_blank_now; srcdoc aside). */
void frame_api_on_connect(JSContextHandle ctx, NodeId subtree) {
    static CssAtom iframe_tag;
    if (!iframe_tag) iframe_tag = css_atom_intern("iframe", 6);
    if (!dom_valid(subtree)) return;
    /* (only in a document with a browsing context: the realm's own, not one
     * made by createHTMLDocument or DOMParser) */
    NodeId realm_doc = dom_node_of(JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document"));
    if (!realm_doc || dom_id_of(dom_tree_root_slot(dom_slot(subtree))) != realm_doc) return;
    for (NodeId n = subtree; n; ) {
        if (dom_kind(n) == DOM_K_ELEMENT && dom_tag(n) == iframe_tag && !frame_of_iframe(n)) {
            const char *src = dom_get_attr_str(n, "src");
            if (src && src[0]) {
                char *base = realm_url(ctx);
                char *abs = wurl_resolve(base, src);
                free(base);
                frame_navigate(ctx, n, abs ? abs : src);
                free(abs);
            } else if (!dom_get_attr_str(n, "srcdoc")) {
                frame_navigate(ctx, n, "about:blank");
                Frame *f = frame_of_iframe(n);
                if (g_frame_blank_now && f && f->state == FRAME_PENDING) frame_load(f);
            }
        }
        /* Document order within the subtree. */
        if (dom_first_child(n)) { n = dom_first_child(n); continue; }
        while (n && n != subtree && !dom_next_sibling(n)) n = dom_parent(n);
        if (!n || n == subtree) break;
        n = dom_next_sibling(n);
    }
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
