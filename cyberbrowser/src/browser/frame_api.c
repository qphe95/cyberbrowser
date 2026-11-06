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
#include "cyber_env.h"
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "dom_store.h"
#include "layout_api.h"
void timer_anim_wake(JSContextHandle ctx);
#include "dom_wrap.h"
#include "http_download.h"
#include "url_utils.h"
#include "html_media_extract.h"
#include "js_quickjs.h"
#include "platform.h"
#include "url_whatwg.h"

extern JSContextHandle g_js_context;
extern const char *g_cyber_start_url;
void timer_cancel_realm(JSContextHandle realm);
void timer_freeze_realm(JSContextHandle realm, bool frozen);
void timer_microtask_checkpoint(JSContextHandle ctx);

typedef enum { FRAME_EMPTY, FRAME_PENDING, FRAME_LOADED } FrameState;

/* One document of a frame's session history; a page in the back/forward
 * cache has its realm here (rooted) and its timers frozen. */
typedef struct FrameDoc {
    char url[2048];
    JSContextHandle ctx;
    NodeId doc;
} FrameDoc;
#define FRAME_HISTORY_MAX 32

typedef struct Frame {
    struct Frame *next;
    FrameDoc *hist;             /* its documents, oldest first */
    int hist_n, hist_i;         /* how many, and the current one */
    int traverse;               /* the navigation due goes this far through them */
    bool noopener;              /* a popup opened without an opener */
    uint32_t id;                /* its window's, for cross_window (the page's is 1) */
    GCValue factory;            /* its realm's cross_window maker (rooted when factory_made) */
    bool factory_made;
    NodeId iframe;              /* the <iframe> element, in the parent's document */
    JSContextHandle parent;     /* the parent's realm */
    JSContextHandle ctx;        /* the frame's realm (rooted), or null */
    NodeId doc;                 /* the frame's document */
    FrameState state;
    char url[2048];             /* the navigation's (or the loaded page's) URL */
    bool popup;                 /* window.open's: its element is in no document */
    bool hidden;                /* its element is not rendered, as of the parent's last layout here */
    double box_w, box_h;        /* its content box then */
    double zoom;                /* its zoom then */
    bool closing;               /* window.close(): gone at the next turn */
    bool view_transition;       /* the document it had opted into one to the next */
    char fallback_base[2048];   /* an about: frame: its base URL when it has no <base> */
    bool dead;                  /* its document is no longer fully active */
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

/* The window of a frame of realm ctx's document whose iframe's name is
 * name (WindowProperties' first case: a child navigable's target name); 0
 * for none. */
uint32_t frame_named_child(JSContextHandle ctx, const char *name, size_t len) {
    for (Frame *f = g_frames; f; f = f->next) {
        if (f->parent != ctx || !SAFE_VALID(f->ctx) || !dom_valid(f->iframe) || !dom_is_connected(f->iframe)) continue;
        const char *n = dom_get_attr_str(f->iframe, "name");
        if (n && strlen(n) == len && !memcmp(n, name, len)) return f->id;
    }
    return 0;
}


static JSContextHandle realm_of_document(NodeId doc) {
    for (Frame *f = g_frames; f; f = f->next) if (f->doc == doc && SAFE_VALID(f->ctx)) return f->ctx;
    if (doc == g_main_doc) return g_js_context;
    return GC_HANDLE_NULL;
}

/* Another realm than ctx whose document doc is (a frame's): true with it in *out. */
bool frame_other_realm_of_document(JSContextHandle ctx, NodeId doc, JSContextHandle *out) {
    for (Frame *f = g_frames; f; f = f->next)
        if (f->doc == doc && SAFE_VALID(f->ctx)) { if (f->ctx == ctx) return false; *out = f->ctx; return true; }
    return false;
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

/* ---- not fully active: the documents and globals of frames gone ---- */

static struct { NodeId *id; uint32_t n, cap; } g_dead_docs;
static uint32_t g_dead_any;

/* (kept, as event_path.c's: a symbol's atom is the runtime's; made anew for
 * another runtime, a document loaded in this process anew) */
static JSAtom inactive_atom(JSContextHandle ctx) {
    static JSAtom atom;
    static uint32_t serial;
    if (!atom || serial != JS_GetContextSerial(ctx)) {
        GCValue sym = JS_Eval(ctx, "Symbol.for('cyber.inactive')", 28, "<frame>", JS_EVAL_TYPE_GLOBAL);
        atom = JS_ValueToAtom(ctx, sym);
        serial = JS_GetContextSerial(ctx);
    }
    return atom;
}

static void dead_doc_add(NodeId doc) {
    if (!doc) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < g_dead_docs.n; i++)
        if (dom_valid(g_dead_docs.id[i])) g_dead_docs.id[k++] = g_dead_docs.id[i];
    g_dead_docs.n = k;
    if (k == g_dead_docs.cap) {
        g_dead_docs.cap = k ? k * 2 : 8;
        g_dead_docs.id = realloc(g_dead_docs.id, g_dead_docs.cap * sizeof(NodeId));
    }
    g_dead_docs.id[g_dead_docs.n++] = doc;
}

/* __cyberInactive(item, global): is item's document, or (not a node) its
 * realm's global, or the listener's global, not fully active? */
static GCValue js_inactive(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (!g_dead_any || argc < 2) return JS_FALSE;
    JSAtom a = inactive_atom(ctx);
    bool off = JS_IsObject(argv[1]) && JS_ToBool(ctx, JS_GetProperty(ctx, argv[1], a)) > 0;
    NodeId n = off || !JS_IsObject(argv[0]) ? DOM_NULL : dom_node_of(argv[0]);
    if (n) {
        NodeId d = dom_kind(n) == DOM_K_DOCUMENT ? n : dom_owner(n);
        for (uint32_t i = 0; i < g_dead_docs.n && !off; i++) off = g_dead_docs.id[i] == d;
    } else if (!off && JS_IsObject(argv[0])) {
        off = JS_ToBool(ctx, JS_GetProperty(ctx, argv[0], a)) > 0;
        GCValue fn = off ? JS_UNDEFINED : JS_GetPropertyStr(ctx, argv[0], "dispatchEvent");
        JSContextHandle realm = JS_IsFunction(ctx, fn) ? JS_GetFunctionRealmOf(ctx, fn) : NULL;
        if (realm) off = JS_ToBool(ctx, JS_GetProperty(ctx, JS_GetGlobalObject(realm), a)) > 0;
    }
    return JS_NewBool(ctx, off);
}

static void frame_index_define(JSContextHandle ctx);

/* (its frames with it) */
static void frame_mark_dead(Frame *f) {
    if (f->dead || !SAFE_VALID(f->ctx)) return;
    f->dead = true;
    g_dead_any++;
    dead_doc_add(f->doc);
    JSAtom a = inactive_atom(f->ctx);
    JS_SetProperty(f->ctx, JS_GetGlobalObject(f->ctx), a, JS_TRUE);
    for (Frame *c = g_frames; c; c = c->next)
        if (c != f && c->parent == f->ctx) frame_mark_dead(c);
}

/* A node left its tree: the frames of the iframes that went with it aren't
 * fully active now (their realms go at the next turn, frame_api_process: a
 * script of theirs may be what's running). */
void frame_api_on_disconnect(void) {
    for (Frame *f = g_frames; f; f = f->next)
        if (!f->dead && !f->popup && SAFE_VALID(f->ctx) && !dom_is_connected(f->iframe)) {
            frame_mark_dead(f);
            /* (window[i] has one less) */
            frame_index_define(f->parent);
        }
}

/* A cached page goes for good. */
static void frame_doc_drop(FrameDoc *d) {
    if (!SAFE_VALID(d->ctx)) { d->ctx = GC_HANDLE_NULL; return; }
    timer_freeze_realm(d->ctx, false);
    timer_cancel_realm(d->ctx);
    dom_realm_drop(d->ctx);
    gc_remove_root((GCHandle)d->ctx);
    d->ctx = GC_HANDLE_NULL;
}

static void frame_history_free(Frame *f) {
    for (int i = 0; i < f->hist_n; i++) frame_doc_drop(&f->hist[i]);
    free(f->hist);
    f->hist = NULL;
    f->hist_n = f->hist_i = 0;
}

/* The document just loaded takes its place: replacing the current one, or
 * after it, in place of what came after. */
static void frame_history_commit(Frame *f, const char *url, bool replace) {
    if (!f->hist) f->hist = (FrameDoc *)calloc(FRAME_HISTORY_MAX, sizeof(FrameDoc));
    if (!f->hist) return;
    if (f->hist_n == 0) replace = false;
    if (!replace) {
        for (int i = f->hist_i + 1; i < f->hist_n; i++) frame_doc_drop(&f->hist[i]);
        f->hist_i = f->hist_n ? f->hist_i + 1 : 0;
        if (f->hist_i == FRAME_HISTORY_MAX) {
            frame_doc_drop(&f->hist[0]);
            memmove(f->hist, f->hist + 1, (FRAME_HISTORY_MAX - 1) * sizeof(FrameDoc));
            f->hist_i--;
        }
        memset(&f->hist[f->hist_i], 0, sizeof(FrameDoc));
        f->hist_n = f->hist_i + 1;
    }
    snprintf(f->hist[f->hist_i].url, sizeof f->hist[f->hist_i].url, "%s", url);
}

static void frame_freeze_children(JSContextHandle ctx, bool frozen);

static void frame_discard_realm(Frame *f) {
    if (!SAFE_VALID(f->ctx)) return;
    frame_mark_dead(f);
    /* (its frames go with it) */
    for (Frame *c = g_frames; c; c = c->next)
        if (c != f && c->parent == f->ctx) frame_discard_realm(c);
    f->dead = false;
    if (f->factory_made) root_value(f->factory, false);
    f->factory_made = false;
    timer_cancel_realm(f->ctx);
    dom_realm_drop(f->ctx);
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
    if (f->doc) g_dom.realm[dom_slot(f->doc)] = dom_realm_index(ctx);
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
        Frame *f = strncmp(u, "about:", 6) && strncasecmp(u, "javascript:", 11) ? NULL : frame_of_realm(ctx);
        if (!f || !SAFE_VALID(f->parent)) return u;
        free(u);
        ctx = f->parent;
    }
    return strdup("about:blank");
}

/* A sandboxed frame (without allow-same-origin), or one in one: its realm's
 * origin is its own, like no other. */
static bool realm_opaque(JSContextHandle ctx) {
    for (int depth = 0; depth < 32; depth++) {
        Frame *f = frame_of_realm(ctx);
        if (!f || !SAFE_VALID(f->parent)) return false;
        if (!f->popup && dom_valid(f->iframe)) {
            const char *sb = dom_get_attr_str(f->iframe, "sandbox");
            if (sb) {
                bool allowed = false;
                for (const char *p = sb; *p; ) {
                    while (*p && isspace((unsigned char)*p)) p++;
                    const char *e = p;
                    while (*e && !isspace((unsigned char)*e)) e++;
                    if (e - p == 16 && !strncasecmp(p, "allow-same-origin", 17)) allowed = true;
                    if (e - p == 17 && !strncasecmp(p, "allow-same-origin", 17)) allowed = true;
                    p = e;
                }
                if (!allowed) return true;
            }
        }
        ctx = f->parent;
    }
    return false;
}

static bool realms_same_origin(JSContextHandle a, JSContextHandle b) {
    if (a == b) return true;
    if (realm_opaque(a) || realm_opaque(b)) return false;
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
static void frame_index_define(JSContextHandle ctx);

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

/* A new document in this process (page_engine_reset, the JS heap gone with
 * the last one's realms): no frames, no messages. */
static bool g_tab_nav_pending;
void frame_api_reset(void) {
    g_tab_nav_pending = false;
    while (g_frames) {
        Frame *f = g_frames;
        g_frames = f->next;
        free(f->hist);
        free(f);
    }
    while (g_messages) {
        Message *m = g_messages;
        g_messages = m->next;
        message_free(m);
    }
    g_messages_tail = &g_messages;
    g_next_id = 2;
    g_main_factory = JS_UNDEFINED;
    g_main_factory_made = false;
    g_processing = false;
    g_main_doc = DOM_NULL;
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

GCValue frame_window_of(JSContextHandle ctx, uint32_t id) {
    return window_seen_from(ctx, id);
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
    "(function (url, parent, top, frameElement, opener) {"
    "  var w = globalThis;"
    "  if (w.location) w.location.href = url;"
    "  if (document) { try { document.domain = w.location.hostname; } catch (e) {}"
    "                  try { document.baseURI = w.location.href; } catch (e) {} }"
    "  function ro(name, v) { Object.defineProperty(w, name, { value: v, configurable: true, writable: false }); }"
    "  ro('parent', opener ? w : parent); ro('top', opener ? w : top || parent); ro('frameElement', frameElement);"
    "  if (opener) {"
    /* (a popup's size is script's to set) */
    "    var sizeTo = function (x, y) {"
    "      forced.width = Math.max(1, Math.floor(Number(x)) || 1);"
    "      forced.height = Math.max(1, Math.floor(Number(y)) || 1);"
    "      w.setTimeout(function () { w.__cyberFrameResized(false); }, 0);"
    "    };"
    "    Object.defineProperty(w, 'resizeTo', { value: function resizeTo(x, y) { sizeTo(x, y); }, configurable: true, writable: true });"
    "    Object.defineProperty(w, 'resizeBy', { value: function resizeBy(x, y) { sizeTo(w.innerWidth + Number(x), w.innerHeight + Number(y)); }, configurable: true, writable: true });"
    "    Object.defineProperty(w, 'opener', { value: opener, configurable: true, writable: true });"
    "    Object.defineProperty(w, 'close', { value: function close() { w.__cyberFrameClose(); }, configurable: true, writable: true });"
    "  }"
    "  function px(v) { var n = parseFloat(v); return isFinite(n) ? n : NaN; }"
    "  var hidden = !!frameElement && !!frameElement.ownerDocument && frameElement.getClientRects && !frameElement.getClientRects().length;"
    "  var forced = {};"
    /* (its element's content box, as of the parent's last look: a style sheet
     * may size it.  What is on the element itself counts at once) */
    "  var box = {};"
    "  if (frameElement && !hidden) { box.width = frameElement.clientWidth; box.height = frameElement.clientHeight; }"
    "  function size(dim, attr, dflt) {"
    "    var f = frameElement, n = NaN;"
    "    if (hidden) return 0;"
    "    if (forced[dim] !== undefined) return forced[dim];"
    "    if (f) { n = px(f.style && f.style[dim]); if (isNaN(n)) n = px(f.getAttribute(attr)); }"
    "    if (!isNaN(n)) return Math.round(n);"
    "    if (box[dim] > 0) return box[dim];"
    "    return opener ? (dim === 'width' ? opener.innerWidth : opener.innerHeight) : dflt;"
    "  }"
    "  Object.defineProperty(w, 'innerWidth', { get: function () { return size('width', 'width', 300); }, configurable: true });"
    "  Object.defineProperty(w, 'innerHeight', { get: function () { return size('height', 'height', 150); }, configurable: true });"
    "  Object.defineProperty(w, 'outerWidth', { get: function () { return w.innerWidth; }, configurable: true });"
    "  Object.defineProperty(w, 'outerHeight', { get: function () { return w.innerHeight; }, configurable: true });"
    /* (its element's size changed: a resize at it, frame_api_resize_tick) */
    "  var lastW = w.innerWidth, lastH = w.innerHeight;"
    "  var zoom = 1, lastZ = 1, parentRatio = (parent && (function () { try { return parent.devicePixelRatio; } catch (e) { return 0; } })()) || 1;"
    "  Object.defineProperty(w, 'devicePixelRatio', { configurable: true, enumerable: true, get: function () { return parentRatio * zoom; } });"
    "  Object.defineProperty(w, '__cyberFrameResized', { configurable: true, value: function (nowHidden, boxW, boxH, z, quiet) {"
    "    if (nowHidden !== undefined) hidden = nowHidden;"
    "    if (z > 0) zoom = z;"
    "    if (boxW > 0) box.width = boxW / zoom;"
    "    if (boxH > 0) box.height = boxH / zoom;"
    /* (only its size, for a style asked after now: the resize comes in its turn) */
    "    if (quiet) return;"
    "    var a = w.innerWidth, b = w.innerHeight;"
    "    if (a === lastW && b === lastH && zoom === lastZ) return;"
    "    lastW = a; lastH = b; lastZ = zoom;"
    /* (CSS View Transitions: a new size ends the one running) */
    "    if (w.document && w.document.__cyberViewTransition) w.document.__cyberViewTransition.skipTransition();"
    "    w.dispatchEvent(new w.Event('resize'));"
    "  } });"
    /* (a frame's location navigates it; a fragment alone doesn't) */
    "  var loc = w.location, hd = loc && Object.getOwnPropertyDescriptor(loc, 'href'), nav = w.__cyberFrameNavigate;"
    "  if (hd && hd.set && hd.configurable && nav) {"
    "    var go = function (u) { if (!nav(String(u))) hd.set.call(loc, u); };"
    "    Object.defineProperty(loc, 'href', { get: hd.get, set: go, enumerable: true, configurable: true });"
    "    ['assign', 'replace'].forEach(function (k) {"
    "      Object.defineProperty(loc, k, { value: function (u) { go(u); }, enumerable: true, configurable: true, writable: true });"
    "    });"
    "    Object.defineProperty(loc, 'reload', { value: function reload() { nav(String(hd.get.call(loc)), true); }, enumerable: true, configurable: true, writable: true });"
    "  }"
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
    GCValue iframe = same && !f->popup ? dom_js(f->parent, f->iframe) : JS_NULL;
    GCValue setup = JS_Eval(ctx, frame_window_js, sizeof(frame_window_js) - 1, "<frame_window>",
                            JS_EVAL_TYPE_GLOBAL);
    /* (what the last document here opted into, for pagereveal) */
    JS_DefinePropertyValueStr(ctx, win, "__cyberOldOptIn", JS_NewBool(ctx, f->view_transition), JS_PROP_CONFIGURABLE);
    if (JS_IsFunction(ctx, setup)) {
        GCValue args[5] = { JS_NewString(ctx, f->url), parent_win, top, iframe, f->popup ? parent_win : JS_NULL };
        GCValue r = JS_Call(ctx, setup, JS_UNDEFINED, 5, args);
        if (JS_IsException(r)) JS_GetException(ctx);
    } else if (JS_IsException(setup)) {
        JS_GetException(ctx);
    }
    /* (no opener: no one to ask, nor to be seen by; the window's own size) */
    if (f->noopener) {
        JS_DefinePropertyValueStr(ctx, win, "opener", JS_NULL, JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
        GCValue fn = JS_GetPropertyStr(ctx, win, "__cyberFrameResized");
        GCValue a[3] = { JS_FALSE, JS_NewFloat64(ctx, 800), JS_NewFloat64(ctx, 600) };
        if (JS_IsFunction(ctx, fn) && JS_IsException(JS_Call(ctx, fn, JS_UNDEFINED, 3, a))) JS_GetException(ctx);
    }
    /* (no frameElement to measure across origins: the parent's layout says) */
    if (!same && !f->popup && dom_valid(f->iframe) && SAFE_VALID(f->parent)) {
        double bw = 0, bh = 0;
        layout_api_update(f->parent);
        if (layout_query_valid() && layout_query_content_size(f->iframe, &bw, &bh) && bw > 0 && bh > 0) {
            GCValue fn = JS_GetPropertyStr(ctx, win, "__cyberFrameResized");
            GCValue a[3] = { JS_FALSE, JS_NewFloat64(ctx, bw), JS_NewFloat64(ctx, bh) };
            if (JS_IsFunction(ctx, fn) && JS_IsException(JS_Call(ctx, fn, JS_UNDEFINED, 3, a))) JS_GetException(ctx);
        }
    }
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

/* (the %XX of a javascript: URL's script) */
static char *wurl_percent_decode(const char *s) {
    size_t n = strlen(s), k = 0;
    char *out = malloc(n + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '%' && i + 2 < n && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            char hex[3] = { s[i + 1], s[i + 2], 0 };
            out[k++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            out[k++] = s[i];
        }
    }
    out[k] = 0;
    return out;
}

/* Navigate f to f->url: a new realm and document with the page loaded. */
/* A popup's window of its first about:blank, which script has kept from
 * window.open: HTML has it stay the window through
 * the first navigation.  Here the document gets a realm of its own, so the old
 * window passes on what is asked of it and hears the load. */
static const char k_window_moved[] =
    "(function (id) {"
    /* (what the engine runs its events by stays: the rest is its frame's window, as it is now) */
    "  var own = /^(__cyber|addEventListener$|removeEventListener$|dispatchEvent$|onload$)/, me = window, cur = __cyberWindowById;"
    "  Reflect.ownKeys(me).forEach(function (n) {"
    "    if (typeof n !== 'string' || own.test(n)) return;"
    "    try {"
    "      Object.defineProperty(me, n, { configurable: true,"
    "        get: function () { var to = cur(id); if (!to) return undefined; var v = to[n]; return typeof v === 'function' && !/^[A-Z]/.test(n) ? v.bind(to) : v; },"
    "        set: function (v) { var to = cur(id); if (to) to[n] = v; } });"
    "    } catch (e) { /* (one it can't redefine) */ }"
    "  });"
    /* (and what is added there from now on) */
    "  try {"
    "    Object.setPrototypeOf(me, new Proxy({}, {"
    "      get: function (t, p) { var to = cur(id); if (!to) return undefined; var v = to[p]; return typeof v === 'function' ? v.bind(to) : v; },"
    "      has: function (t, p) { var to = cur(id); return !!to && p in to; },"
    "      set: function (t, p, v) { var to = cur(id); if (to) to[p] = v; return true; } }));"
    "  } catch (e) { /* (its prototype is fixed) */ }"
    "})";

static bool g_frame_initial_only;   /* frame_load: the initial about:blank, no navigation's load */

/* pagehide and pageshow, with whether the page is kept. */
static void frame_fire_transition(JSContextHandle ctx, const char *type, bool persisted) {
    char js[160];
    snprintf(js, sizeof js, "dispatchEvent(new PageTransitionEvent('%s', { persisted: %s }));", type, persisted ? "true" : "false");
    GCValue r = JS_Eval(ctx, js, strlen(js), "<page_transition>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) JS_GetException(ctx);
}

static void frame_freeze_children(JSContextHandle ctx, bool frozen) {
    timer_freeze_realm(ctx, frozen);
    for (Frame *c = g_frames; c; c = c->next)
        if (c->parent == ctx && SAFE_VALID(c->ctx)) frame_freeze_children(c->ctx, frozen);
}

static void frame_load(Frame *f) {
    JSContextHandle first_ctx = GC_HANDLE_NULL;
    GCValue first_win = JS_UNDEFINED;
    char url[2048];
    snprintf(url, sizeof(url), "%s", f->url);
    int traverse = f->traverse;
    f->traverse = 0;
    bool reload = false, replace_entry = false, keep = false;
    /* (the document going: its window hears) */
    if (SAFE_VALID(f->ctx) && !f->dead) {
        GCValue was = JS_GetGlobalObject(f->ctx);
        /* (pageswap: whether a view transition goes on, same-origin only) */
        char *from = realm_url(f->ctx);
        bool same = same_origin(from, url);
        if (!strncmp(from, "about:blank", 11) && strncmp(url, "about:blank", 11) &&
            (f->popup || !strncmp(url, "about:", 6) || same)) {
            first_ctx = f->ctx;
            first_win = was;
            gc_add_root((GCHandle)first_ctx);
            root_value(first_win, true);
        }
        free(from);
        char *from_now = realm_url(f->ctx);
        reload = !traverse && !strcmp(from_now, url);
        replace_entry = reload || !strncmp(from_now, "about:blank", 11);
        /* (a popup's page, with a document to come back to, stays: top-level
         * pages are the ones the back/forward cache is for) */
        keep = f->popup && !reload && !first_ctx && f->hist && strncmp(from_now, "about:", 6);
        free(from_now);
        GCValue swap = JS_GetPropertyStr(f->ctx, was, "__cyberPageSwap");
        GCValue to = JS_NewString(f->ctx, url), sr = JS_FALSE;
        GCValue sa[3] = { to, JS_NewBool(f->ctx, same), JS_NewString(f->ctx, traverse ? "traverse" : reload ? "reload" : "push") };
        if (JS_IsFunction(f->ctx, swap)) sr = JS_Call(f->ctx, swap, JS_UNDEFINED, 3, sa);
        if (JS_IsException(sr)) { JS_GetException(f->ctx); sr = JS_FALSE; }
        f->view_transition = same && JS_ToBool(f->ctx, sr) > 0;
        frame_fire(f->ctx, was, "beforeunload");
        frame_fire_transition(f->ctx, "pagehide", keep);
        if (!keep) frame_fire(f->ctx, was, "unload");
    }
    if (keep && SAFE_VALID(f->ctx) && f->hist_i < f->hist_n) {
        /* Into the cache: as it is, its timers waiting. */
        FrameDoc *d = &f->hist[f->hist_i];
        d->ctx = f->ctx;
        d->doc = f->doc;
        /* (the transition it started for this doesn't go in with it) */
        static const char skip_js[] = "if (window.__cyberOutboundVT) { __cyberOutboundVT.skipTransition(); window.__cyberOutboundVT = null; }";
        GCValue sk = JS_Eval(f->ctx, skip_js, sizeof skip_js - 1, "<outbound_vt>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(sk)) JS_GetException(f->ctx);
        timer_microtask_checkpoint(f->ctx);
        frame_freeze_children(f->ctx, true);
        f->ctx = GC_HANDLE_NULL;
        f->doc = DOM_NULL;
    } else {
        frame_discard_realm(f);
    }
    f->state = FRAME_LOADED;
    /* Through the history: from the cache if it is there. */
    if (traverse && f->hist && f->hist_i + traverse >= 0 && f->hist_i + traverse < f->hist_n) {
        f->hist_i += traverse;
        FrameDoc *d = &f->hist[f->hist_i];
        if (SAFE_VALID(d->ctx)) {
            f->ctx = d->ctx;
            f->doc = d->doc;
            d->ctx = GC_HANDLE_NULL;
            snprintf(f->url, sizeof f->url, "%s", d->url);
            frame_freeze_children(f->ctx, false);
            frame_fire_transition(f->ctx, "pageshow", true);
            timer_anim_wake(f->ctx);
            return;
        }
    }
    /* (what its base URL falls back on, as it is now) */
    if (!strncmp(url, "about:", 6)) {
        GCValue pd = JS_GetPropertyStr(f->parent, JS_GetGlobalObject(f->parent), "document");
        GCValue pb = JS_IsObject(pd) ? JS_GetPropertyStr(f->parent, pd, "baseURI") : JS_UNDEFINED;
        const char *pbs = JS_IsString(pb) ? JS_ToCString(f->parent, pb) : NULL;
        snprintf(f->fallback_base, sizeof(f->fallback_base), "%s", pbs ? pbs : "");
        if (pbs) JS_FreeCString(f->parent, pbs);
    }
    /* A frame document, asked for by the parent's origin (which the network
     * process lets through ORB: section 9). */
    char *parent_origin = realm_origin(f->parent);
    HttpRequestContext was;
    http_request_context_get(&was);
    http_request_context_set(HTTP_MODE_FRAME, parent_origin);
    free(parent_origin);
    HttpBuffer buf = {0};
    char err[256] = {0};
    bool blank = !strncmp(url, "about:", 6) || !strncasecmp(url, "javascript:", 11);         /* an empty document, from nowhere */
    /* (a data: URL: its body, its MIME type in its header) */
    bool data = !strncasecmp(url, "data:", 5);
    char data_mime[128] = "text/plain";
    bool fetched = false;
    if (data) {
        size_t n = 0;
        uint8_t *d = data_url_decode(url, &n);
        if (d) {
            buf.data = (char *)d;
            buf.size = n;
            fetched = true;
        }
        size_t k = 0;
        for (const char *p = url + 5; *p && *p != ';' && *p != ',' && k + 1 < sizeof data_mime; p++)
            data_mime[k++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
        if (k) data_mime[k] = 0;
    } else if (!blank) {
        fetched = http_get_to_memory(url, &buf, err, sizeof(err));
    }
    http_request_context_set(was.mode, was.initiator);
    /* The frame's document is where it ended, after redirects: its origin,
     * location and base URL are that URL's. */
    if (fetched && !data && http_last_url()[0]) {
        snprintf(url, sizeof(url), "%s", http_last_url());
        snprintf(f->url, sizeof(f->url), "%s", url);
    }
    if (!frame_new_realm(f)) {
        http_free_buffer(&buf);
        return;
    }
    if (!traverse) frame_history_commit(f, url, replace_entry);
    frame_setup_window(f);
    frame_index_define(f->parent);
    JSContextHandle ctx = f->ctx;
    char mime[128];
    response_mime(fetched ? buf.headers : NULL, mime, sizeof mime);
    if (data) snprintf(mime, sizeof mime, "%s", data_mime);
    if ((fetched || data) && mime[0]) {
        GCValue fd = dom_js(ctx, f->doc);
        if (JS_IsObject(fd)) JS_DefinePropertyValueStr(ctx, fd, "__cyber_ct", JS_NewString(ctx, mime), JS_PROP_CONFIGURABLE);
    }
    HtmlPageStream *ps = html_page_stream_begin_frame(ctx, url);
    if (ps && fetched && xml_mime(mime)) html_page_stream_set_xml(ps, mime);
    if (ps) {
        /* (about:srcdoc: the attribute's markup) */
        const char *srcdoc = !strcmp(url, "about:srcdoc") ? dom_get_attr_str(f->iframe, "srcdoc") : NULL;
        const char *js_out = NULL;
        size_t js_out_n = 0;
        if (!strncasecmp(url, "javascript:", 11)) {
            /* (a javascript: URL: what its script returns, as a string, is the document) */
            char *body = wurl_percent_decode(url + 11);
            GCValue v = JS_Eval(ctx, body ? body : "", body ? strlen(body) : 0, "<javascript-url>", JS_EVAL_TYPE_GLOBAL);
            free(body);
            if (JS_IsException(v)) JS_GetException(ctx);
            else if (JS_IsString(v)) js_out = JS_ToCStringLen(ctx, &js_out_n, v);
        }
        if (js_out) html_page_stream_feed(ps, js_out, js_out_n);
        else if (!strncasecmp(url, "javascript:", 11)) { /* (nothing to show) */ }
        else if (srcdoc && srcdoc[0]) html_page_stream_feed(ps, srcdoc, strlen(srcdoc));
        else if (fetched && buf.data && buf.size) html_page_stream_feed(ps, buf.data, buf.size);
        else if (!blank) platform_log(LOG_LEVEL_WARN, "frame", "could not fetch frame %s: %s", url, err);
        html_page_stream_finish(ps, NULL);
    }
    http_free_buffer(&buf);
    if (!SAFE_VALID(f->ctx)) return;    /* (a script removed the frame) */
    static const char lifecycle_js[] =
        "document.readyState = 'interactive';"
        "__cyberDispatchTrusted(document, new Event('DOMContentLoaded', { bubbles: true }));"
        "document.readyState = 'complete';"
        "if (window.__cyberPageReveal) __cyberPageReveal();"
        "window.dispatchEvent(new Event('load'));"
        "window.dispatchEvent(new PageTransitionEvent('pageshow', { persisted: false }));";
    GCValue r = JS_Eval(ctx, lifecycle_js, sizeof(lifecycle_js) - 1, "<frame_lifecycle>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) JS_GetException(ctx);
    if (first_ctx) {
        GCValue moved = JS_Eval(first_ctx, k_window_moved, sizeof k_window_moved - 1, "<window_moved>", JS_EVAL_TYPE_GLOBAL);
        GCValue now = JS_NewInt32(first_ctx, (int32_t)f->id);
        if (JS_IsFunction(first_ctx, moved) && JS_IsException(JS_Call(first_ctx, moved, JS_UNDEFINED, 1, &now))) JS_GetException(first_ctx);
        /* (it hears this one, gone as its realm is) */
        JS_SetProperty(first_ctx, first_win, inactive_atom(first_ctx), JS_FALSE);
        frame_fire(first_ctx, first_win, "load");
        JS_SetProperty(first_ctx, first_win, inactive_atom(first_ctx), JS_TRUE);
        root_value(first_win, false);
        gc_remove_root((GCHandle)first_ctx);
    }
    if (!g_frame_initial_only) frame_fire(f->parent, dom_js(f->parent, f->iframe), "load");
}

/* ---- window[i], window.length (HTML 7.2.3: the document-tree child
 * navigables), from the iframes of the realm's document in tree order ---- */

/* The frame of the i-th iframe with a browsing context in ctx's document
 * (or the count of them, with i < 0). */
static void frame_ensure(Frame *f);

static Frame *child_frame(JSContextHandle ctx, int i, int *count) {
    NodeId doc = dom_node_of(JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document"));
    int k = 0;
    if (dom_valid(doc)) {
        uint32_t r = dom_slot(doc);
        for (uint32_t s = r; s; s = dom_next_in_subtree(s, r)) {
            if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
            Frame *f = frame_of_iframe(dom_id_of(s));
            if (f && f->parent == ctx) frame_ensure(f);
            if (!f || f->parent != ctx || !SAFE_VALID(f->ctx)) continue;
            if (k++ == i) return f;
        }
    }
    if (count) *count = k;
    return NULL;
}

static GCValue js_window_index_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)this_val; (void)argc; (void)argv;
    Frame *f = child_frame(ctx, magic, NULL);
    return f ? window_seen_from(ctx, f->id) : JS_UNDEFINED;
}

static GCValue js_window_length_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    int n = 0;
    child_frame(ctx, -1, &n);
    return JS_NewInt32(ctx, n);
}

/* Index properties 0 .. count - 1 on ctx's window (each read live), and
 * its length; defined as frames come (never taken back: one past the count
 * reads undefined). */
static void frame_index_define(JSContextHandle ctx) {
    if (!SAFE_VALID(ctx)) return;
    GCValue g = JS_GetGlobalObject(ctx);
    int n = 0;
    child_frame(ctx, -1, &n);
    int32_t have = 0;
    GCValue hv = JS_GetPropertyStr(ctx, g, "__cyber_frame_indices");
    if (JS_IsNumber(hv)) JS_ToInt32(ctx, &have, hv);
    else {
        JSAtom la = JS_NewAtom(ctx, "length");
        JS_DefinePropertyGetSet(ctx, g, la, JS_NewCFunction(ctx, js_window_length_get, "length", 0), JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, la);
    }
    for (int i = have; i < n; i++) {
        JSAtom a = JS_NewAtomUInt32(ctx, (uint32_t)i);
        JS_DefinePropertyGetSet(ctx, g, a, JS_NewCFunctionMagic(ctx, js_window_index_get, "", 0, JS_CFUNC_generic_magic, i),
                                JS_UNDEFINED, JS_PROP_CONFIGURABLE);
        JS_FreeAtom(ctx, a);
    }
    /* (a frame gone: its index with it) */
    for (int i = n; i < have; i++) {
        JSAtom a = JS_NewAtomUInt32(ctx, (uint32_t)i);
        JS_DeleteProperty(ctx, g, a, 0);
        JS_FreeAtom(ctx, a);
    }
    if (n != have)
        JS_DefinePropertyValueStr(ctx, g, "__cyber_frame_indices", JS_NewInt32(ctx, n), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
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

/* Is the frame's parent an XML document still loading?  Its scripts run
 * once it is parsed whole (not as they are met, as an HTML document's do):
 * its frames wait for them, or their load events would come before the
 * handlers those scripts define. */
static bool parent_scripts_pending(const Frame *f) {
    if (!SAFE_VALID(f->parent)) return false;
    NodeId doc = dom_owner(f->iframe);
    if (!dom_valid(doc) || !(dom_flags(doc) & DOMF_XML_DOCUMENT)) return false;
    GCValue d = dom_js(f->parent, doc);
    const char *rs = JS_IsObject(d) ? JS_ToCString(f->parent, JS_GetPropertyStr(f->parent, d, "readyState")) : NULL;
    bool loading = rs && !strcmp(rs, "loading");
    if (rs) JS_FreeCString(f->parent, rs);
    return loading;
}

/* (an XML page's nodes going back in, as its parser would have: its iframes
 * out of the tree meanwhile aren't gone) */
static bool g_frames_held;
void frame_api_hold(bool hold) { g_frames_held = hold; }

/* A frame realm's size as its parent has it now (a class or sheet may have just
 * changed it), before its style is asked after; once per change to the page. */
void frame_api_sync_size(JSContextHandle ctx) {
    extern uint64_t g_dom_layout_requests;
    static uint64_t seen;
    static JSContextHandle seen_ctx;
    if (!g_frames || (seen == g_dom_layout_requests && seen_ctx == ctx)) return;
    for (Frame *f = g_frames; f; f = f->next) {
        if (f->ctx != ctx || f->dead || f->popup || f->state != FRAME_LOADED || !SAFE_VALID(f->parent)) continue;
        seen = g_dom_layout_requests;
        seen_ctx = ctx;
        layout_api_update(f->parent);
        if (!layout_query_valid()) return;
        double w = 0.0, h = 0.0;
        bool hidden = !layout_query_rendered(f->iframe);
        layout_query_content_size(f->iframe, &w, &h);
        double z = layout_query_zoom(f->iframe);
        GCValue fn = JS_GetPropertyStr(f->ctx, JS_GetGlobalObject(f->ctx), "__cyberFrameResized");
        if (!JS_IsFunction(f->ctx, fn)) return;
        GCValue args[5] = { JS_NewBool(f->ctx, hidden), JS_NewFloat64(f->ctx, w), JS_NewFloat64(f->ctx, h), JS_NewFloat64(f->ctx, z), JS_TRUE };
        if (JS_IsException(JS_Call(f->ctx, fn, JS_UNDEFINED, 5, args))) JS_GetException(f->ctx);
        return;
    }
}

/* The frames whose size changed, when anything in the page changed. */
int frame_api_resize_tick(void) {
    static uint64_t seen, seen_rendered;
    static unsigned long long last;
    extern uint64_t g_dom_layout_requests;
    uint64_t changes = g_dom_layout_requests;
    if (!g_frames || (seen == changes && seen_rendered == changes)) return 0;
    seen = changes;
    /* Whether each frame's element is still rendered, from its parent's layout
     * up to now.  Done first, and at most every 100 ms: the query layout holds
     * one document, and a frame's own, below, replaces it. */
    unsigned long long now = (unsigned long long)page_clock_now_ms();
    bool check = seen_rendered != changes && now - last >= 100;
    /* (too soon: a turn must still come for it, on an idle page too) */
    if (seen_rendered != changes && !check) timer_anim_wake(g_frames->parent);
    if (check) {
        last = now;
        seen_rendered = changes;
        for (Frame *f = g_frames; f; f = f->next) {
            if (f->dead || f->popup || !f->ctx || f->state != FRAME_LOADED) continue;
            layout_api_update(f->parent);
            if (layout_query_valid()) {
                f->hidden = !layout_query_rendered(f->iframe);
                f->box_w = f->box_h = 0.0;
                layout_query_content_size(f->iframe, &f->box_w, &f->box_h);
                f->zoom = layout_query_zoom(f->iframe);
            }
        }
    }
    for (Frame *f = g_frames; f; f = f->next) {
        if (f->dead || f->popup || !f->ctx || f->state != FRAME_LOADED) continue;
        /* (the page's frames lay out only the main document: a change in a
         * frame's own is seen here, and a transition it starts with it) */
        if (f->doc && dom_valid(f->doc) && (g_dom.flags[dom_slot(f->doc)] & DOMF_DOC_CHANGED)) {
            g_dom.flags[dom_slot(f->doc)] &= (uint16_t)~DOMF_DOC_CHANGED;
            layout_api_run(f->ctx);
        }
        GCValue fn = JS_GetPropertyStr(f->ctx, JS_GetGlobalObject(f->ctx), "__cyberFrameResized");
        if (!JS_IsFunction(f->ctx, fn)) continue;
        GCValue args[4] = { check ? JS_NewBool(f->ctx, f->hidden) : JS_UNDEFINED,
                            JS_NewFloat64(f->ctx, check ? f->box_w : 0.0), JS_NewFloat64(f->ctx, check ? f->box_h : 0.0) };
        args[3] = JS_NewFloat64(f->ctx, check ? f->zoom : 0.0);
        if (JS_IsException(JS_Call(f->ctx, fn, JS_UNDEFINED, 4, args))) JS_GetException(f->ctx);
    }
    return 0;
}

int frame_api_process(void) {
    if (g_processing || g_frames_held || (!g_frames && !g_messages)) return 0;
    g_processing = true;
    int done = deliver_messages();
    for (Frame **pp = &g_frames; *pp; ) {
        Frame *f = *pp;
        if (!dom_valid(f->iframe) || f->closing || (!f->popup && !dom_is_connected(f->iframe))) {
            /* the iframe left its document: its browsing context goes */
            frame_discard_realm(f);
            frame_history_free(f);
            *pp = f->next;
            free(f);
            continue;
        }
        if (f->state == FRAME_PENDING && parent_scripts_pending(f)) {
            pp = &f->next;
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

/* The window of the index-th child frame of realm ctx's document
 * (window[index]); 0 for none. */
uint32_t frame_indexed_child(JSContextHandle ctx, uint32_t index) {
    Frame *f = index <= 0x7fffffffu ? child_frame(ctx, (int)index, NULL) : NULL;
    return f ? f->id : 0;
}

/* A frame whose about:blank document isn't made yet gets it now, when its
 * window or document is asked for (HTML makes an iframe's initial
 * about:blank document as it is inserted; here the event loop does, unless
 * script looks first). */
static void frame_ensure(Frame *f) {
    if (!f || f->state != FRAME_PENDING || g_processing || SAFE_VALID(f->ctx)) return;
    if (strncmp(f->url, "about:", 6) != 0 || !dom_valid(f->iframe) || (!f->popup && !dom_is_connected(f->iframe))) return;
    if (parent_scripts_pending(f)) return;
    g_processing = true;
    /* (srcdoc comes in a task, as a navigation; until then the window asked
     * for is the initial about:blank's, which then goes on as srcdoc's) */
    if (!strncmp(f->url, "about:srcdoc", 12)) {
        char target[sizeof f->url];
        snprintf(target, sizeof target, "%s", f->url);
        snprintf(f->url, sizeof f->url, "about:blank");
        g_frame_initial_only = true;
        frame_load(f);
        g_frame_initial_only = false;
        snprintf(f->url, sizeof f->url, "%s", target);
        f->state = FRAME_PENDING;
    } else {
        frame_load(f);
    }
    g_processing = false;
}

bool frame_api_pending(void) {
    for (Frame *f = g_frames; f; f = f->next)
        if (f->state == FRAME_PENDING && !parent_scripts_pending(f)) return true;
    return g_messages != NULL;
}

/* ---- HTMLIFrameElement ---- */

static GCValue js_iframe_get_content_window(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    Frame *f = frame_of_iframe(dom_node_of(this_val));
    frame_ensure(f);
    return f && SAFE_VALID(f->ctx) && !f->dead ? window_seen_from(ctx, f->id) : JS_NULL;
}

static GCValue js_iframe_get_content_document(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    Frame *f = frame_of_iframe(dom_node_of(this_val));
    frame_ensure(f);
    if (!f || !SAFE_VALID(f->ctx) || f->dead || !realms_same_origin(ctx, f->ctx)) return JS_NULL;
    return JS_GetPropertyStr(f->ctx, JS_GetGlobalObject(f->ctx), "document");
}

static GCValue js_iframe_get_srcdoc(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    const char *v = dom_get_attr_str(dom_node_of(this_val), "srcdoc");
    return JS_NewString(ctx, v ? v : "");
}

static GCValue js_iframe_set_srcdoc(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    NodeId n = dom_node_of(this_val);
    GCValue set = JS_GetPropertyStr(ctx, this_val, "setAttribute");
    GCValue args[2] = { JS_NewString(ctx, "srcdoc"), JS_ToString(ctx, argc > 0 ? argv[0] : JS_UNDEFINED) };
    if (JS_IsException(args[1])) return args[1];
    GCValue r = JS_IsFunction(ctx, set) ? JS_Call(ctx, set, this_val, 2, args) : JS_UNDEFINED;
    if (JS_IsException(r)) return r;
    if (n && dom_is_connected(n)) frame_navigate(ctx, n, "about:srcdoc");
    return JS_UNDEFINED;
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
    /* (no browsing context in a document without one) */
    NodeId realm_doc = dom_node_of(JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document"));
    if (n && dom_is_connected(n) && dom_owner(n) == realm_doc) {
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
    static CssAtom iframe_tag, frame_tag;
    if (!iframe_tag) {
        css_atom_reserve(2);
        iframe_tag = css_atom_intern("iframe", 6);
        frame_tag = css_atom_intern("frame", 5);
    }
    if (!dom_valid(subtree)) return;
    /* (only in a document with a browsing context: the realm's own, not one
     * made by createHTMLDocument or DOMParser) */
    NodeId realm_doc = dom_node_of(JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document"));
    if (!realm_doc || dom_id_of(dom_tree_root_slot(dom_slot(subtree))) != realm_doc) return;
    for (NodeId n = subtree; n; ) {
        if (dom_kind(n) == DOM_K_ELEMENT && (dom_tag(n) == iframe_tag || dom_tag(n) == frame_tag) && !frame_of_iframe(n)) {
            const char *src = dom_get_attr_str(n, "src");
            if (dom_get_attr_str(n, "srcdoc")) {
                frame_navigate(ctx, n, "about:srcdoc");
            } else if (src && src[0]) {
                char *base = realm_url(ctx);
                char *abs = wurl_resolve(base, src);
                free(base);
                frame_navigate(ctx, n, abs ? abs : src);
                free(abs);
            } else {
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

/* A frame's location set: it navigates, unless only the fragment differs
 * (false: the location takes it as it is). */
static GCValue js_frame_navigate_to(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    Frame *f = frame_of_realm(ctx);
    const char *u = argc ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!f || !u) return JS_FALSE;
    char *base = realm_url(ctx);
    char *abs = wurl_resolve(base, u);
    const char *to = abs ? abs : u, *hash = strchr(to, '#');
    size_t n = hash ? (size_t)(hash - to) : strlen(to), bn = strcspn(base, "#");
    bool same = hash && n == bn && !strncmp(to, base, n);
    if (argc > 1 && JS_ToBool(ctx, argv[1])) same = false;     /* (a reload) */
    if (!same) {
        snprintf(f->url, sizeof(f->url), "%s", to);
        f->state = FRAME_PENDING;
    }
    free(abs);
    free(base);
    JS_FreeCString(ctx, u);
    return same ? JS_FALSE : JS_TRUE;
}

/* window.open: only an empty window, a browsing context with no element in
 * a document (a page with a URL for one gets none: as a blocked popup). */
static GCValue js_window_open(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    char *to = NULL;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        const char *u = JS_ToCString(ctx, argv[0]);
        if (!u) return JS_EXCEPTION;
        bool blank = !u[0] || !strcmp(u, "about:blank");
        /* (with a URL, only where popups are allowed: the tests' runs) */
        if (!blank) {
            if (!cyber_getenv("CYBER_ALLOW_POPUPS")) { JS_FreeCString(ctx, u); return JS_NULL; }
            char *base = realm_url(ctx);
            to = wurl_resolve(base, u);
            free(base);
        }
        JS_FreeCString(ctx, u);
    }
    bool noopener = false;
    if (argc > 2 && JS_IsString(argv[2])) {
        const char *feat = JS_ToCString(ctx, argv[2]);
        if (feat) {
            noopener = strstr(feat, "noopener") || strstr(feat, "noreferrer");
            JS_FreeCString(ctx, feat);
        }
    }
    GCValue g = JS_GetGlobalObject(ctx), doc = JS_GetPropertyStr(ctx, g, "document");
    GCValue create = JS_IsObject(doc) ? JS_GetPropertyStr(ctx, doc, "createElement") : JS_UNDEFINED;
    if (!JS_IsFunction(ctx, create)) return JS_NULL;
    GCValue tag = JS_NewString(ctx, "iframe");
    GCValue el = JS_Call(ctx, create, doc, 1, &tag);
    if (JS_IsException(el)) return el;
    NodeId n = dom_node_of(el);
    if (!n) return JS_NULL;
    /* (what keeps its element) */
    GCValue keep = JS_GetPropertyStr(ctx, g, "__cyberPopups");
    if (!JS_IsObject(keep)) {
        keep = JS_NewArray(ctx);
        JS_DefinePropertyValueStr(ctx, g, "__cyberPopups", keep, JS_PROP_CONFIGURABLE);
    }
    uint32_t have = 0;
    JS_ToUint32(ctx, &have, JS_GetPropertyStr(ctx, keep, "length"));
    JS_SetPropertyUint32(ctx, keep, have, el);
    frame_navigate(ctx, n, "about:blank");
    Frame *f = frame_of_iframe(n);
    if (!f) return JS_NULL;
    f->popup = true;
    f->noopener = noopener;
    frame_ensure(f);
    /* (its initial document first, as the opener can reach it at once) */
    if (to) {
        snprintf(f->url, sizeof(f->url), "%s", to);
        f->state = FRAME_PENDING;
        free(to);
    }
    if (noopener) return JS_NULL;
    return SAFE_VALID(f->ctx) ? window_seen_from(ctx, f->id) : JS_NULL;
}

/* Input into a frame's document, whatever its origin (the page gets no way
 * into it: numbers go in, nothing comes out).  No method: is there a window
 * there to take it? */
static GCValue js_input_into(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    NodeId n = argc > 0 && JS_IsObject(argv[0]) ? dom_node_of(argv[0]) : DOM_NULL;
    Frame *f = n ? frame_of_iframe(n) : NULL;
    if (f) frame_ensure(f);
    if (!f || f->dead || !SAFE_VALID(f->ctx)) return JS_FALSE;
    GCValue input = JS_GetPropertyStr(f->ctx, JS_GetGlobalObject(f->ctx), "__cyberInput");
    if (!JS_IsObject(input)) return JS_FALSE;
    if (argc < 2) return JS_TRUE;
    static const char *const methods[] = { "move", "down", "up", "wheel", "touchDown", "touchMove", "touchUp", NULL };
    const char *name = JS_ToCString(ctx, argv[1]);
    if (!name) return JS_EXCEPTION;
    GCValue fn = JS_UNDEFINED;
    for (int i = 0; methods[i]; i++) if (!strcmp(name, methods[i])) fn = JS_GetPropertyStr(f->ctx, input, methods[i]);
    JS_FreeCString(ctx, name);
    if (!JS_IsFunction(f->ctx, fn)) return JS_FALSE;
    GCValue args[4];
    int k = 0;
    for (int i = 2; i < argc && k < 4; i++) {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, argv[i])) return JS_EXCEPTION;
        args[k++] = JS_NewFloat64(f->ctx, d);
    }
    if (JS_IsException(JS_Call(f->ctx, fn, input, k, args))) JS_GetException(f->ctx);
    return JS_TRUE;
}

/* window.close() on a popup: closed now; its realm goes at the next turn. */
static GCValue js_frame_close(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    Frame *f = frame_of_realm(ctx);
    if (f && f->popup) {
        f->closing = true;
        JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "closed", JS_TRUE, JS_PROP_CONFIGURABLE);
    }
    return JS_UNDEFINED;
}

/* __cyberTabNavigate(url): the page itself goes there; false in a frame, or
 * with no tab process to do it. */
void (*g_tab_navigate_hook)(const char *url);
/* The page's script has sent it elsewhere: it won't finish loading. */
static bool g_tab_nav_pending;
bool frame_api_nav_pending(void) { return g_tab_nav_pending; }
static GCValue js_tab_navigate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (!g_tab_navigate_hook || ctx != g_js_context || argc < 1) return JS_FALSE;
    const char *u = JS_ToCString(ctx, argv[0]);
    if (!u) return JS_FALSE;
    g_tab_navigate_hook(u);
    g_tab_nav_pending = true;
    JS_FreeCString(ctx, u);
    return JS_TRUE;
}

/* __cyberWindowById(id): that frame's window as this realm sees it now. */
static GCValue js_window_by_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)ctx;
    int32_t id = 0;
    if (argc > 0) JS_ToInt32(ctx, &id, argv[0]);
    /* (as its parent would: the realm asking is one its frame has left) */
    Frame *f = id ? frame_of_window((uint32_t)id) : NULL;
    return f && SAFE_VALID(f->parent) ? window_seen_from(f->parent, (uint32_t)id) : JS_NULL;
}

/* __cyberFrameTraverse(delta): back or forward to another document, if this
 * window's frame has one there; the event loop runs it. */
static GCValue js_frame_traverse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    int32_t delta = 0;
    if (argc > 0) JS_ToInt32(ctx, &delta, argv[0]);
    Frame *f = frame_of_realm(ctx);
    if (!f || !f->hist || !delta) return JS_FALSE;
    int to = f->hist_i + delta;
    if (to < 0 || to >= f->hist_n) return JS_FALSE;
    snprintf(f->url, sizeof f->url, "%s", f->hist[to].url);
    f->traverse = delta;
    f->state = FRAME_PENDING;
    timer_anim_wake(ctx);
    return JS_TRUE;
}

void frame_api_install(JSContextHandle ctx, GCValue global) {
    JS_DefinePropertyValueStr(ctx, global, "__cyberTabNavigate", JS_NewCFunction(ctx, js_tab_navigate, "__cyberTabNavigate", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberWindowById", JS_NewCFunction(ctx, js_window_by_id, "__cyberWindowById", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberFrameTraverse", JS_NewCFunction(ctx, js_frame_traverse, "__cyberFrameTraverse", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberFrameClose", JS_NewCFunction(ctx, js_frame_close, "__cyberFrameClose", 0), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberFrameNavigate", JS_NewCFunction(ctx, js_frame_navigate_to, "__cyberFrameNavigate", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "__cyberInputInto", JS_NewCFunction(ctx, js_input_into, "__cyberInputInto", 6), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, global, "open", JS_NewCFunction(ctx, js_window_open, "open", 0),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    if (!g_main_doc) g_main_doc = g_dom_document;
    if (g_dom_document == g_main_doc && dom_valid(g_main_doc)) g_dom.realm[dom_slot(g_main_doc)] = dom_realm_index(ctx);
    dom_wrap_set_realm_resolver(realm_of_document);
    JS_DefinePropertyValueStr(ctx, global, "__cyberInactive", JS_NewCFunction(ctx, js_inactive, "__cyberInactive", 2),
                              JS_PROP_CONFIGURABLE);
    anim_api_init();
    static const struct { const char *name; JSCFunction *get; } getters[] = {
        { "contentWindow", js_iframe_get_content_window },
        { "contentDocument", js_iframe_get_content_document },
    };
    /* (<frame> has them too) */
    GCValue fctor = JS_GetPropertyStr(ctx, global, "HTMLFrameElement");
    GCValue fproto = JS_IsObject(fctor) ? JS_GetPropertyStr(ctx, fctor, "prototype") : JS_UNDEFINED;
    GCValue ctor = JS_GetPropertyStr(ctx, global, "HTMLIFrameElement");
    GCValue proto = JS_IsObject(ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
    if (!JS_IsObject(proto)) return;
    for (size_t i = 0; i < sizeof(getters) / sizeof(getters[0]); i++) {
        JSAtom a = JS_NewAtom(ctx, getters[i].name);
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction(ctx, getters[i].get, getters[i].name, 0),
                                JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        if (JS_IsObject(fproto))
            JS_DefinePropertyGetSet(ctx, fproto, a, JS_NewCFunction(ctx, getters[i].get, getters[i].name, 0),
                                    JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
    JSAtom src = JS_NewAtom(ctx, "src");
    JS_DefinePropertyGetSet(ctx, proto, src, JS_NewCFunction(ctx, js_iframe_get_src, "get src", 0),
                            JS_NewCFunction(ctx, js_iframe_set_src, "set src", 1),
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, src);
    JSAtom srcdoc = JS_NewAtom(ctx, "srcdoc");
    JS_DefinePropertyGetSet(ctx, proto, srcdoc, JS_NewCFunction(ctx, js_iframe_get_srcdoc, "get srcdoc", 0),
                            JS_NewCFunction(ctx, js_iframe_set_srcdoc, "set srcdoc", 1),
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, srcdoc);
}

/* The base URL an about:blank or about:srcdoc frame falls back on: its
 * parent's document's (HTML "fallback base URL"); NULL when not one. */
char *frame_api_fallback_base(JSContextHandle ctx) {
    Frame *f = frame_of_realm(ctx);
    return f && f->fallback_base[0] ? strdup(f->fallback_base) : NULL;
}
