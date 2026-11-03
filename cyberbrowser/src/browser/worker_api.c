/*
 * worker_api.c - dedicated workers (HTML 10.2), run on the page's thread.
 *
 * new Worker(url) gets a realm of its own in the page's runtime, set up as
 * a window's is and then made a worker's: the DOM interfaces not exposed to
 * workers (idl_table.inc) and the document go, and its global is a
 * DedicatedWorkerGlobalScope with postMessage, importScripts and a
 * WorkerLocation.  Its script runs, and the messages both ways are
 * delivered, as tasks of the event loop (worker_api_tick); a message is
 * cloned through JSON.
 *
 * The workers are rows of flat arrays; the tasks a queue of them.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_store.h"
#include "http_download.h"

extern NodeId g_dom_document;

enum { WT_START, WT_TO_WORKER, WT_TO_OWNER };

typedef struct WorkerTask {
    int kind, w;
    char *json;                 /* a message, as JSON ("null" for undefined) */
} WorkerTask;

static struct {
    int n, cap;
    JSContextHandle *owner, *ctx;   /* the realm that made it, its own (0 until started) */
    GCValue *obj;               /* the Worker object (rooted) */
    char **url;
    unsigned char *closed;
} W;

static WorkerTask *g_tasks;
static int g_ntasks, g_captasks;

static void task_push(int kind, int w, char *json) {
    if (g_ntasks == g_captasks) {
        g_captasks = g_captasks ? g_captasks * 2 : 16;
        g_tasks = (WorkerTask *)realloc(g_tasks, (size_t)g_captasks * sizeof(WorkerTask));
    }
    g_tasks[g_ntasks++] = (WorkerTask){ kind, w, json };
}

static int worker_new(JSContextHandle owner, GCValue obj, const char *url) {
    if (W.n == W.cap) {
        int cap = W.cap ? W.cap * 2 : 8;
#define G(f) W.f = realloc(W.f, (size_t)cap * sizeof(*W.f))
        G(owner); G(ctx); G(obj); G(url); G(closed);
#undef G
        W.cap = cap;
    }
    int w = W.n++;
    W.owner[w] = owner;
    W.ctx[w] = GC_HANDLE_NULL;
    W.obj[w] = obj;
    gc_add_root(GC_VALUE_GET_HANDLE(obj));
    W.url[w] = strdup(url);
    W.closed[w] = 0;
    return w;
}

/* The worker a realm is (-1: a window's). */
static int worker_of(JSContextHandle ctx) {
    for (int w = 0; w < W.n; w++)
        if (W.ctx[w] == ctx) return w;
    return -1;
}

static char *json_of(JSContextHandle ctx, GCValue v) {
    GCValue s = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(s)) {
        JS_GetException(ctx);
        return strdup("null");
    }
    if (!JS_IsString(s)) return strdup("null");
    const char *c = JS_ToCString(ctx, s);
    char *r = strdup(c ? c : "null");
    if (c) JS_FreeCString(ctx, c);
    return r;
}

/* ---- the natives ---- */

/* __cyberWorkerStart(worker, url): its row; the script starts as a task. */
static GCValue js_worker_start(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "bad worker");
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!url) return JS_EXCEPTION;
    int w = worker_new(ctx, argv[0], url);
    JS_FreeCString(ctx, url);
    task_push(WT_START, w, NULL);
    return JS_NewInt32(ctx, w);
}

/* __cyberWorkerPost(row, message): to the worker, as a task. */
static GCValue js_worker_post(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    int32_t w = -1;
    if (argc < 2 || JS_ToInt32(ctx, &w, argv[0]) || w < 0 || w >= W.n) return JS_UNDEFINED;
    if (!W.closed[w]) task_push(WT_TO_WORKER, w, json_of(ctx, argv[1]));
    return JS_UNDEFINED;
}

static GCValue js_worker_terminate(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    int32_t w = -1;
    if (argc > 0 && JS_ToInt32(ctx, &w, argv[0]) == 0 && w >= 0 && w < W.n) W.closed[w] = 1;
    return JS_UNDEFINED;
}

/* (in a worker) __cyberWorkerOut(message): to its Worker object, as a task. */
static GCValue js_worker_out(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    int w = worker_of(ctx);
    if (w >= 0 && !W.closed[w]) task_push(WT_TO_OWNER, w, json_of(ctx, argc > 0 ? argv[0] : JS_UNDEFINED));
    return JS_UNDEFINED;
}

/* A script fetched and run in realm ctx (importScripts, the worker's own);
 * false with an exception thrown. */
static bool run_script(JSContextHandle ctx, const char *url) {
    HttpBuffer buf = { 0 };
    char err[256] = "";
    if (!http_get_to_memory(url, &buf, err, sizeof err) || !buf.data) {
        http_free_buffer(&buf);
        throw_dom_exception(ctx, "NetworkError", "The script could not be loaded.");
        return false;
    }
    GCValue r = JS_Eval(ctx, buf.data, buf.size, url, JS_EVAL_TYPE_GLOBAL);
    http_free_buffer(&buf);
    return !JS_IsException(r);
}

/* (in a worker) __cyberImport(url): importScripts' one script. */
static GCValue js_worker_import(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!url) return JS_EXCEPTION;
    bool ok = run_script(ctx, url);
    JS_FreeCString(ctx, url);
    return ok ? JS_UNDEFINED : JS_EXCEPTION;
}

/* ---- realms ---- */

/* What a worker's global becomes (window's made a worker's). */
static const char k_worker_setup[] =
    "(function (g, url, windowOnly) {\n"
    "  for (var i = 0; i < windowOnly.length; i++) delete g[windowOnly[i]];\n"
    "  delete g.document;\n"
    "  function scope(name, parent) {\n"
    "    var c = function () { throw new TypeError('Illegal constructor'); };\n"
    "    Object.defineProperty(c, 'name', { value: name });\n"
    "    c.prototype = Object.create(parent.prototype);\n"
    "    Object.defineProperty(c.prototype, 'constructor', { value: c, writable: true, configurable: true });\n"
    "    Object.defineProperty(c.prototype, Symbol.toStringTag, { value: name, configurable: true });\n"
    "    Object.defineProperty(g, name, { value: c, writable: true, configurable: true });\n"
    "    return c;\n"
    "  }\n"
    "  var WGS = scope('WorkerGlobalScope', g.EventTarget);\n"
    "  var DWGS = scope('DedicatedWorkerGlobalScope', WGS);\n"
    "  Object.setPrototypeOf(g, DWGS.prototype);\n"
    "  var loc = new URL(url);\n"
    "  var wl = { href: loc.href, origin: loc.origin, protocol: loc.protocol, host: loc.host, hostname: loc.hostname,\n"
    "             port: loc.port, pathname: loc.pathname, search: loc.search, hash: loc.hash,\n"
    "             toString: function () { return loc.href; } };\n"
    "  Object.defineProperty(g, 'location', { value: wl, configurable: true });\n"
    "  Object.defineProperty(g, 'self', { value: g, writable: true, configurable: true });\n"
    "  g.postMessage = function postMessage(m) { g.__cyberWorkerOut(m); };\n"
    "  g.importScripts = function importScripts() {\n"
    "    for (var i = 0; i < arguments.length; i++) g.__cyberImport(new URL(String(arguments[i]), url).href);\n"
    "  };\n"
    "  g.close = function close() {};\n"
    "  var handlers = {};\n"
    "  ['message', 'error', 'messageerror'].forEach(function (t) {\n"
    "    Object.defineProperty(g, 'on' + t, { configurable: true, get: function () { return handlers[t] || null; },\n"
    "      set: function (f) { if (handlers[t]) g.removeEventListener(t, handlers[t]);\n"
    "        handlers[t] = typeof f === 'function' ? f : null; if (handlers[t]) g.addEventListener(t, handlers[t]); } });\n"
    "  });\n"
    "  g.__cyberDeliver = function (data) { g.dispatchEvent(new MessageEvent('message', { data: data })); };\n"
    "})";

/* The DOM interfaces a worker does not have (idl_shape.c's table). */
GCValue idl_window_only_names(JSContextHandle ctx);

static void worker_start(int w) {
    JSContextHandle owner = W.owner[w];
    JSRuntimeHandle rt = JS_GetRuntime(owner);
    JSContextHandle ctx = JS_NewContext(rt);
    if (!SAFE_VALID(ctx)) return;
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
    /* (the API installs a new document as the main one: keep the page's) */
    NodeId main_doc = g_dom_document;
    init_browser_api_impl(ctx, JS_GetGlobalObject(ctx));
    g_dom_document = main_doc;
    W.ctx[w] = ctx;
    GCValue g = JS_GetGlobalObject(ctx);
    GCValue setup = JS_Eval(ctx, k_worker_setup, sizeof k_worker_setup - 1, "<worker setup>", JS_EVAL_TYPE_GLOBAL);
    GCValue args[3] = { g, JS_NewString(ctx, W.url[w]), idl_window_only_names(ctx) };
    GCValue r = JS_IsException(setup) ? setup : JS_Call(ctx, setup, JS_UNDEFINED, 3, args);
    if (JS_IsException(r)) {
        JS_GetException(ctx);
        return;
    }
    if (!run_script(ctx, W.url[w])) {
        /* (an uncaught error: reported in the worker, as a window's would be) */
        GCValue exc = JS_GetException(ctx);
        GCValue report = JS_GetPropertyStr(ctx, g, "reportError");
        if (JS_IsFunction(ctx, report)) {
            GCValue rr = JS_Call(ctx, report, g, 1, &exc);
            if (JS_IsException(rr)) JS_GetException(ctx);
        }
    }
}

static void deliver(JSContextHandle ctx, GCValue fn, GCValue this_val, int argc, GCValue *argv) {
    if (!JS_IsFunction(ctx, fn)) return;
    GCValue r = JS_Call(ctx, fn, this_val, argc, argv);
    if (JS_IsException(r)) JS_GetException(ctx);
}

int worker_api_tick(JSContextHandle ctx) {
    (void)ctx;
    if (!g_ntasks) return 0;
    /* (taken first: running them queues more, for the next turn) */
    int n = g_ntasks;
    WorkerTask *t = (WorkerTask *)malloc((size_t)n * sizeof(WorkerTask));
    if (!t) return 0;
    memcpy(t, g_tasks, (size_t)n * sizeof(WorkerTask));
    g_ntasks = 0;
    for (int i = 0; i < n; i++) {
        int w = t[i].w;
        if (W.closed[w]) { free(t[i].json); continue; }
        if (t[i].kind == WT_START) {
            worker_start(w);
        } else if (t[i].kind == WT_TO_WORKER && SAFE_VALID(W.ctx[w])) {
            JSContextHandle wc = W.ctx[w];
            GCValue data = JS_ParseJSON(wc, t[i].json, strlen(t[i].json), "<message>");
            if (JS_IsException(data)) { JS_GetException(wc); data = JS_NULL; }
            GCValue g = JS_GetGlobalObject(wc);
            deliver(wc, JS_GetPropertyStr(wc, g, "__cyberDeliver"), g, 1, &data);
            timer_microtask_checkpoint(wc);
        } else if (t[i].kind == WT_TO_OWNER) {
            JSContextHandle oc = W.owner[w];
            GCValue data = JS_ParseJSON(oc, t[i].json, strlen(t[i].json), "<message>");
            if (JS_IsException(data)) { JS_GetException(oc); data = JS_NULL; }
            GCValue args[2] = { W.obj[w], data };
            deliver(oc, JS_GetPropertyStr(oc, JS_GetGlobalObject(oc), "__cyberWorkerDeliver"), JS_UNDEFINED, 2, args);
            timer_microtask_checkpoint(oc);
        }
        free(t[i].json);
    }
    free(t);
    return n;
}

/* The Worker interface of a window realm (and the natives of a worker's). */
static const char k_worker_class[] =
    "(function (g) {\n"
    "  var start = g.__cyberWorkerStart, post = g.__cyberWorkerPost, stop = g.__cyberWorkerTerminate;\n"
    "  class Worker extends g.EventTarget {\n"
    "    constructor(url, options) {\n"
    "      if (arguments.length < 1) throw new TypeError(\"Failed to construct 'Worker': 1 argument required.\");\n"
    "      super();\n"
    "      Object.defineProperty(this, '__w', { value: start(this, new URL(String(url), g.location.href).href) });\n"
    "    }\n"
    "    postMessage(m) { post(this.__w, m); }\n"
    "    terminate() { stop(this.__w); }\n"
    "  }\n"
    "  ['message', 'error', 'messageerror'].forEach(function (t) {\n"
    "    Object.defineProperty(Worker.prototype, 'on' + t, { configurable: true, enumerable: true,\n"
    "      get: function () { return (this.__h && this.__h[t]) || null; },\n"
    "      set: function (f) { var h = this.__h || (this.__h = {}); if (h[t]) this.removeEventListener(t, h[t]);\n"
    "        h[t] = typeof f === 'function' ? f : null; if (h[t]) this.addEventListener(t, h[t]); } });\n"
    "  });\n"
    "  Object.defineProperty(g, 'Worker', { value: Worker, writable: true, configurable: true });\n"
    "  g.__cyberWorkerDeliver = function (w, data) { w.dispatchEvent(new MessageEvent('message', { data: data })); };\n"
    "})";

void worker_api_install(JSContextHandle ctx, GCValue global) {
    static const struct { const char *name; JSCFunction *fn; int len; } natives[] = {
        { "__cyberWorkerStart", js_worker_start, 2 }, { "__cyberWorkerPost", js_worker_post, 2 },
        { "__cyberWorkerTerminate", js_worker_terminate, 1 }, { "__cyberWorkerOut", js_worker_out, 1 },
        { "__cyberImport", js_worker_import, 1 },
    };
    for (size_t i = 0; i < sizeof natives / sizeof natives[0]; i++)
        JS_DefinePropertyValueStr(ctx, global, natives[i].name, JS_NewCFunction(ctx, natives[i].fn, natives[i].name, natives[i].len),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue fn = JS_Eval(ctx, k_worker_class, sizeof k_worker_class - 1, "<worker>", JS_EVAL_TYPE_GLOBAL);
    GCValue r = JS_IsException(fn) ? fn : JS_Call(ctx, fn, JS_UNDEFINED, 1, &global);
    if (JS_IsException(r)) JS_GetException(ctx);
}
