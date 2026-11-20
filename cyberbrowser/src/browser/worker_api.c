/*
 * worker_api.c - dedicated workers (HTML 10.2), run on the page's thread.
 *
 * new Worker(url) (and new SharedWorker(url), its port the one it talks
 * through) gets a realm of its own in the page's runtime, set up as
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
    char **url, **src;          /* src: the script of a blob: URL */
    unsigned char *closed, *shared;
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

static int worker_new(JSContextHandle owner, GCValue obj, const char *url, bool shared) {
    if (W.n == W.cap) {
        int cap = W.cap ? W.cap * 2 : 8;
#define G(f) W.f = realloc(W.f, (size_t)cap * sizeof(*W.f))
        G(owner); G(ctx); G(obj); G(url); G(src); G(closed); G(shared);
#undef G
        W.cap = cap;
    }
    int w = W.n++;
    W.owner[w] = owner;
    W.ctx[w] = GC_HANDLE_NULL;
    W.obj[w] = obj;
    gc_add_root(GC_VALUE_GET_HANDLE(obj));
    W.url[w] = strdup(url);
    W.src[w] = NULL;
    W.closed[w] = 0;
    W.shared[w] = shared;
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

/* __cyberWorkerStart(target, url, shared): its row (target: the Worker, or a
 * SharedWorker's port, what its messages go to); the script starts as a task. */
static GCValue js_worker_start(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "bad worker");
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!url) return JS_EXCEPTION;
    int w = worker_new(ctx, argv[0], url, argc > 2 && JS_ToBool(ctx, argv[2]));
    JS_FreeCString(ctx, url);
    if (argc > 3 && JS_IsString(argv[3])) {
        const char *src = JS_ToCString(ctx, argv[3]);
        if (src) { W.src[w] = strdup(src); JS_FreeCString(ctx, src); }
    }
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
    "(function (g, url, windowOnly, shared) {\n"
    "  for (var i = 0; i < windowOnly.length; i++) delete g[windowOnly[i]];\n"
    "  delete g.document; delete g.Window; delete g.window; delete g.event;\n"
    /* (the window's interfaces made in script: none in a worker) */
    "  ['MediaError', 'AudioTrackList', 'AudioTrack', 'VideoTrackList', 'VideoTrack', 'TextTrackList', 'TextTrack', 'TextTrackCueList',\n"
    "   'TextTrackCue', 'TimeRanges', 'ValidityState', 'BarProp', 'HTMLOptionsCollection', 'HTMLFormControlsCollection', 'RadioNodeList',\n"
    "   'History', 'Location', 'Navigator', 'Option', 'CommandEvent', 'clientInformation', 'locationbar', 'menubar', 'personalbar',\n"
    "   'scrollbars', 'statusbar', 'toolbar', 'Storage', 'HTMLDocument', 'localStorage', 'sessionStorage', 'VTTCue',\n"
    "   'NavigationTransition', 'NavigationActivation', 'NavigationPrecommitController', 'NavigationCurrentEntryChangeEvent', 'SharedWorker',\n"
    "   'StorageEvent', 'External'].forEach(function (k) { try { delete g[k]; } catch (e) {} });\n"
    /* (SVG's interfaces: a document's, none in a worker) */
    "  Object.getOwnPropertyNames(g).forEach(function (k) { if (/^SVG[A-Z]/.test(k) || /^HTML[A-Z]/.test(k)) { try { delete g[k]; } catch (e) {} } });\n"
    "  ['DOMStringMap', 'SubmitEvent', 'FormDataEvent', 'CanvasRenderingContext2D', 'CustomElementRegistry', 'UserActivation', 'ToggleEvent', 'DataTransfer', 'DataTransferItemList', 'DataTransferItem', 'DragEvent', 'Navigation', 'NavigationHistoryEntry', 'NavigateEvent', 'NavigationDestination', 'PopStateEvent', 'HashChangeEvent', 'PageSwapEvent', 'PageRevealEvent', 'PageTransitionEvent', 'BeforeUnloadEvent', 'DOMParser', 'XMLSerializer', 'TrackEvent', 'ElementInternals', 'CustomStateSet', 'CloseWatcher', 'PluginArray', 'MimeTypeArray', 'Plugin', 'MimeType', 'Sanitizer', 'NotRestoredReasons', 'NotRestoredReasonDetails', 'VisibilityStateEntry', 'MathMLElement', 'SVGAnimatedString', 'InstallResultEvent'].forEach(function (k) { try { delete g[k]; } catch (e) {} });\n"
    /* (FontFaceSet in a worker: what it refuses; no faces here to wait for) */
    "  function fontError(spec) {\n"
    "    spec = String(spec);\n"
    "    return /var\\(/i.test(spec) || /^\\s*(initial|inherit|unset|revert|revert-layer|default)\\s*$/i.test(spec)\n"
    "      ? new DOMException(\"Could not parse the font list '\" + spec + \"'\", 'SyntaxError') : null;\n"
    "  }\n"
    "  var faces = new Set();\n"
    "  g.fonts = { status: 'loaded', ready: Promise.resolve(),\n"
    "    load: function (spec) { var e = fontError(spec); return e ? Promise.reject(e) : Promise.resolve([]); },\n"
    "    check: function (spec) { var e = fontError(spec); if (e) throw e; return true; },\n"
    /* (the faces added: waited for by ready) */
    "    add: function (face) { faces.add(face);\n"
    "      try { var fsrc = face && face[Symbol.for('cyber.ffsrc')]; if (fsrc && typeof g.__c2dFontScan === 'function') g.__c2dFontScan('@font-face { font-family: ' + JSON.stringify(String(face.family)) + '; src: ' + fsrc + ' }', String(g.location.href)); } catch (e) { }\n"
    "      var all = Array.from(faces).map(function (x) { return x && x.loaded ? x.loaded.catch(function () {}) : null; });\n"
    "      g.fonts.ready = Promise.all(all).then(function () { return g.fonts; }); return g.fonts; },\n"
    "    delete: function (face) { return faces.delete(face); }, has: function (face) { return faces.has(face); }, clear: function () { faces.clear(); },\n"
    "    forEach: function (cb, t) { faces.forEach(function (x) { cb.call(t, x, x, g.fonts); }); },\n"
    "    values: function () { return faces.values(); }, keys: function () { return faces.values(); }, entries: function () { return faces.entries(); },\n"
    "    addEventListener: function () {}, removeEventListener: function () {} };\n"
    "  Object.defineProperty(g.fonts, 'size', { get: function () { return faces.size; } });\n"
    "  g.fonts[Symbol.iterator] = function () { return faces.values(); };\n"
    "  g.fonts.ready = Promise.resolve(g.fonts);\n"
    "  function scope(name, parent) {\n"
    "    var c = function () { throw new TypeError('Illegal constructor'); };\n"
    "    Object.defineProperty(c, 'name', { value: name });\n"
    "    Object.defineProperty(c, 'prototype', { value: Object.create(parent.prototype), writable: false });\n"
    "    Object.setPrototypeOf(c, parent);\n"
    "    Object.defineProperty(c.prototype, 'constructor', { value: c, writable: true, configurable: true });\n"
    "    Object.defineProperty(c.prototype, Symbol.toStringTag, { value: name, configurable: true });\n"
    "    Object.defineProperty(g, name, { value: c, writable: true, configurable: true });\n"
    "    return c;\n"
    "  }\n"
    "  var WGS = scope('WorkerGlobalScope', g.EventTarget);\n"
    "  var GS = scope(shared ? 'SharedWorkerGlobalScope' : 'DedicatedWorkerGlobalScope', WGS);\n"
    "  Object.setPrototypeOf(g, GS.prototype);\n"
    "  var loc = new URL(url);\n"
    "  var wl = { href: loc.href, origin: loc.origin, protocol: loc.protocol, host: loc.host, hostname: loc.hostname,\n"
    "             port: loc.port, pathname: loc.pathname, search: loc.search, hash: loc.hash,\n"
    "             toString: function () { return loc.href; } };\n"
    "  Object.defineProperty(g, 'location', { value: wl, configurable: true });\n"
    "  Object.defineProperty(g, 'self', { value: g, writable: true, configurable: true });\n"
    "  if (!shared) g.postMessage = function postMessage(m) { g.__cyberWorkerOut(g.__cyberWire(m)); };\n"
    "  else delete g.postMessage;\n"
    "  g.importScripts = function importScripts() {\n"
    "    for (var i = 0; i < arguments.length; i++) g.__cyberImport(new URL(String(arguments[i]), url).href);\n"
    "  };\n"
    "  g.close = function close() {};\n"
    "  var handlers = {};\n"
    "  ['message', 'error', 'messageerror', 'connect'].forEach(function (t) {\n"
    "    Object.defineProperty(g, 'on' + t, { configurable: true, get: function () { return handlers[t] || null; },\n"
    "      set: function (f) { handlers[t] = typeof f === 'function' ? f : null; } });\n"
    "  });\n"
    "  var to = g;\n"
    "  if (shared) {\n"
    "    /* (its end of the port: a connect event brings it) */\n"
    "    class Port extends g.EventTarget {\n"
    "      postMessage(m) { g.__cyberWorkerOut(g.__cyberWire(m)); }\n"
    "      start() {}\n"
    "      close() {}\n"
    "    }\n"
    "    Object.defineProperty(Port.prototype, Symbol.toStringTag, { value: 'MessagePort', configurable: true });\n"
    "    to = new Port();\n"
    "    g.__cyberConnect = function () { g.dispatchEvent(new MessageEvent('connect', { source: to, ports: [to] })); };\n"
    "  }\n"
    "  /* (WorkerLocation and WorkerNavigator: interfaces, their members accessors on their prototypes) */\n"
    "  (function () {\n"
    "    var iface = function (name, members, src) {\n"
    "      var C = function () { throw new TypeError('Illegal constructor'); };\n"
    "      Object.defineProperty(C, 'name', { value: name });\n"
    "      var P = {};\n"
    "      Object.defineProperty(C, 'prototype', { value: P, writable: false });\n"
    "      Object.defineProperty(P, 'constructor', { value: C, writable: true, configurable: true });\n"
    "      Object.defineProperty(P, Symbol.toStringTag, { value: name, configurable: true });\n"
    "      var inst = Object.create(P), K = Symbol(name);\n"
    "      Object.defineProperty(inst, K, { value: src });\n"
    "      members.forEach(function (k) {\n"
    "        var get = { [k]: function () { if (!this || !this[K]) throw new TypeError('Illegal invocation'); var v = this[K][k]; return typeof v === 'function' ? v.call(this[K]) : v; } }[k];\n"
    "        Object.defineProperty(get, 'name', { value: 'get ' + k });\n"
    "        Object.defineProperty(P, k, { get: get, enumerable: true, configurable: true });\n"
    "      });\n"
    "      Object.defineProperty(g, name, { value: C, writable: true, configurable: true });\n"
    "      return { C: C, P: P, inst: inst, K: K };\n"
    "    };\n"
    "    try {\n"
    "      var lo = iface('WorkerLocation', ['href', 'origin', 'protocol', 'host', 'hostname', 'port', 'pathname', 'search', 'hash'], wl);\n"
    "      var lts = { toString: function () { if (!this || !this[lo.K]) throw new TypeError('Illegal invocation'); return this[lo.K].href; } }.toString;\n"
    "      Object.defineProperty(lo.P, 'toString', { value: lts, writable: true, enumerable: true, configurable: true });\n"
    "      wl = lo.inst;\n"
    "      Object.defineProperty(g, 'location', { value: wl, configurable: true });\n"
    "      var nv = g.navigator;\n"
    "      if (nv && typeof nv === 'object') {\n"
    "        var no = iface('WorkerNavigator', ['appCodeName', 'appName', 'appVersion', 'platform', 'product', 'userAgent', 'language', 'languages', 'onLine', 'hardwareConcurrency'], nv);\n"
    "        Object.getOwnPropertyNames(nv).concat(Object.getOwnPropertyNames(Object.getPrototypeOf(nv) || {})).forEach(function (k) {\n"
    "          if (k === 'constructor' || Object.prototype.hasOwnProperty.call(no.P, k)) return;\n"
    "          if (k === 'productSub' || k === 'vendor' || k === 'vendorSub') return;   /* (a Window's NavigatorID only) */\n"
    "          var d = Object.getOwnPropertyDescriptor(nv, k) || Object.getOwnPropertyDescriptor(Object.getPrototypeOf(nv), k);\n"
    "          if (!d) return;\n"
    "          if ('value' in d && typeof d.value !== 'function') Object.defineProperty(no.inst, k, { value: d.value, writable: true, configurable: true });\n"
    "          else if ('value' in d) Object.defineProperty(no.inst, k, { value: d.value.bind(nv), writable: true, configurable: true });\n"
    "          else Object.defineProperty(no.inst, k, { get: function () { return d.get.call(nv); }, configurable: true });\n"
    "        });\n"
    "        /* (languages: a frozen array, the same one each time) */\n"
    "        var langs = null, lget = { languages: function () { if (!this || !this[no.K]) throw new TypeError('Illegal invocation'); if (!langs) { var v = nv.languages; langs = Object.freeze(Array.prototype.slice.call(v || [])); } return langs; } }.languages;\n"
    "        Object.defineProperty(lget, 'name', { value: 'get languages' });\n"
    "        Object.defineProperty(no.P, 'languages', { get: lget, enumerable: true, configurable: true });\n"
    "        Object.defineProperty(g, 'navigator', { value: no.inst, writable: true, configurable: true });\n"
    "      }\n"
    "    } catch (e) { }\n"
    "  })();\n"
    "  /* (the scope interfaces' members on their prototypes, not the global: brand checked, with their arities) */\n"
    "  (function () {\n"
    "    var bad = function (t) { return t !== undefined && t !== null && t !== g; };\n"
    "    var move = function (proto, ops, attrs) {\n"
    "      Object.keys(ops).forEach(function (k) {\n"
    "        var d = Object.getOwnPropertyDescriptor(g, k);\n"
    "        if (!d || typeof d.value !== 'function') return;\n"
    "        var f = d.value, n = ops[k];\n"
    "        var w = { [k]: function () {\n"
    "          if (bad(this)) throw new TypeError('Illegal invocation');\n"
    "          if (arguments.length < n) throw new TypeError(\"Failed to execute '\" + k + \"': \" + n + ' argument required, but only ' + arguments.length + ' present.');\n"
    "          return f.apply(g, arguments);\n"
    "        } }[k];\n"
    "        Object.defineProperty(w, 'length', { value: n });\n"
    "        delete g[k];\n"
    "        Object.defineProperty(proto, k, { value: w, writable: true, enumerable: true, configurable: true });\n"
    "      });\n"
    "      attrs.forEach(function (k) {\n"
    "        var d = Object.getOwnPropertyDescriptor(g, k), held;\n"
    "        if (!d) {\n"
    "          if (k === 'isSecureContext') d = { value: /^(https:|wss:)/.test(String(url)) || /^https?:\\/\\/(127\\.0\\.0\\.1|localhost|\\[::1\\])(:|\\/)/.test(String(url)) };\n"
    "          else if (k === 'crossOriginIsolated') d = { value: false };\n"
    "          else if (k.slice(0, 2) !== 'on' && k !== 'name') return;\n"
    "          else d = { value: k === 'name' ? (g.__cyberWorkerName || '') : null, writable: true };\n"
    "        }\n"
    "        var gf = d.get, sf = d.set, isv = 'value' in d;\n"
    "        if (isv) held = d.value;\n"
    "        var get = { get: function () { if (bad(this)) throw new TypeError('Illegal invocation'); return gf ? gf.call(g) : held; } }.get;\n"
    "        Object.defineProperty(get, 'name', { value: 'get ' + k });\n"
    "        var set;\n"
    "        if (k.slice(0, 2) === 'on') set = { set: function (v) { if (bad(this)) throw new TypeError('Illegal invocation'); if (sf) sf.call(g, v); else held = typeof v === 'function' || (v && typeof v === 'object') ? v : null; } }.set;\n"
    "        /* ([Replaceable]: a set puts a plain value of that name on the global) */\n"
    "        else if (k === 'origin' || k === 'name') set = { set: function (v) { if (bad(this)) throw new TypeError('Illegal invocation'); Object.defineProperty(g, k, { value: v, writable: true, enumerable: true, configurable: true }); } }.set;\n"
    "        if (set) Object.defineProperty(set, 'name', { value: 'set ' + k });\n"
    "        delete g[k];\n"
    "        Object.defineProperty(proto, k, { get: get, set: set, enumerable: true, configurable: true });\n"
    "      });\n"
    "    };\n"
    "    try { if (Object.prototype.hasOwnProperty.call(g, Symbol.toStringTag)) delete g[Symbol.toStringTag]; } catch (e) { }\n"
    "    try {\n"
    "      move(WGS.prototype, { atob: 1, btoa: 1, clearInterval: 0, clearTimeout: 0, createImageBitmap: 1, queueMicrotask: 1, reportError: 1,\n"
    "                            setInterval: 1, setTimeout: 1, structuredClone: 1, importScripts: 0 },\n"
    "           ['self', 'location', 'navigator', 'origin', 'isSecureContext', 'crossOriginIsolated', 'onerror', 'onlanguagechange', 'onoffline', 'ononline',\n"
    "            'onrejectionhandled', 'onunhandledrejection']);\n"
    "      if (!shared) move(g, { postMessage: 1, close: 0, requestAnimationFrame: 1, cancelAnimationFrame: 1 }, ['name', 'onmessage', 'onmessageerror']);\n"
    "      else move(g, { close: 0 }, ['name', 'onconnect']);\n"
    "    } catch (e) { }\n"
    "  })();\n"
    "  g.__cyberDeliver = function (data) { to.dispatchEvent(new MessageEvent('message', { data: g.__cyberUnwire(data) })); };\n"
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
    extern bool g_idl_worker_realm;
    g_idl_worker_realm = true;   /* (idl_shape.c: a worker's realm being made) */
    init_browser_api_impl(ctx, JS_GetGlobalObject(ctx));
    g_idl_worker_realm = false;
    g_dom_document = main_doc;
    W.ctx[w] = ctx;
    GCValue g = JS_GetGlobalObject(ctx);
    GCValue setup = JS_Eval(ctx, k_worker_setup, sizeof k_worker_setup - 1, "<worker setup>", JS_EVAL_TYPE_GLOBAL);
    GCValue args[4] = { g, JS_NewString(ctx, W.url[w]), idl_window_only_names(ctx), JS_NewBool(ctx, W.shared[w]) };
    GCValue r = JS_IsException(setup) ? setup : JS_Call(ctx, setup, JS_UNDEFINED, 4, args);
    if (JS_IsException(r)) {
        JS_GetException(ctx);
        return;
    }
    /* (a blob: URL: its text came with the constructor) */
    bool ran = W.src[w] ? !JS_IsException(JS_Eval(ctx, W.src[w], strlen(W.src[w]), W.url[w], JS_EVAL_TYPE_GLOBAL)) : run_script(ctx, W.url[w]);
    if (!ran) {
        /* (an uncaught error: reported in the worker, as a window's would be) */
        GCValue exc = JS_GetException(ctx);
        GCValue report = JS_GetPropertyStr(ctx, g, "reportError");
        if (JS_IsFunction(ctx, report)) {
            GCValue rr = JS_Call(ctx, report, g, 1, &exc);
            if (JS_IsException(rr)) JS_GetException(ctx);
        }
    }
    if (W.shared[w]) {
        GCValue c = JS_GetPropertyStr(ctx, g, "__cyberConnect");
        if (JS_IsFunction(ctx, c)) {
            GCValue rr = JS_Call(ctx, c, g, 0, NULL);
            if (JS_IsException(rr)) JS_GetException(ctx);
        }
    }
}

static void deliver(JSContextHandle ctx, GCValue fn, GCValue this_val, int argc, GCValue *argv) {
    if (!JS_IsFunction(ctx, fn)) return;
    GCValue r = JS_Call(ctx, fn, this_val, argc, argv);
    if (JS_IsException(r)) JS_GetException(ctx);
}

/* The heap is about to go: the workers and what was queued for them with it. */
void worker_api_reset(void) {
    for (int i = 0; i < g_ntasks; i++) free(g_tasks[i].json);
    g_ntasks = 0;
    for (int w = 0; w < W.n; w++) { free(W.url[w]); free(W.src[w]); }
    W.n = 0;
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
    "      var abs = new URL(String(url), g.location.href).href;\n"
    "      Object.defineProperty(this, '__w', { value: start(this, abs, false, g.__cyberBlobText(abs)) });\n"
    "    }\n"
    "    postMessage(m) {\n"
    "      if (!(this instanceof Worker)) throw new TypeError(\"Failed to execute 'postMessage' on 'Worker': Illegal invocation\");\n"
    "      if (arguments.length < 1) throw new TypeError(\"Failed to execute 'postMessage' on 'Worker': 1 argument required, but only 0 present.\");\n"
    "      post(this.__w, g.__cyberWire(m));\n"
    "      var tl = arguments[1], tr = Array.isArray(tl) ? tl : (tl && typeof tl === 'object' && Array.isArray(tl.transfer) ? tl.transfer : null);\n"
    "      if (tr && g.ImageBitmap) for (var ti = 0; ti < tr.length; ti++) if (tr[ti] instanceof g.ImageBitmap) { try { g.__cyberDetachBitmap(tr[ti]); } catch (e) {} }\n"
    "    }\n"
    "    terminate() { if (!(this instanceof Worker)) throw new TypeError(\"Failed to execute 'terminate' on 'Worker': Illegal invocation\"); stop(this.__w); }\n"
    "  }\n"
    "  ['message', 'error', 'messageerror'].forEach(function (t) {\n"
    "    Object.defineProperty(Worker.prototype, 'on' + t, { configurable: true, enumerable: true,\n"
    "      get: function () { return (this.__h && this.__h[t]) || null; },\n"
    "      set: function (f) { var h = this.__h || (this.__h = {}); h[t] = typeof f === 'function' ? f : null; } });\n"
    "  });\n"
    "  Object.defineProperty(g, 'Worker', { value: Worker, writable: true, configurable: true });\n"
    "  class MessagePortOf extends g.EventTarget {\n"
    "    postMessage(m) { post(this.__w, g.__cyberWire(m)); }\n"
    "    start() {}\n"
    "    close() { stop(this.__w); }\n"
    "  }\n"
    "  Object.defineProperty(MessagePortOf.prototype, Symbol.toStringTag, { value: 'MessagePort', configurable: true });\n"
    "  Object.defineProperty(MessagePortOf.prototype, 'onmessage', { configurable: true, enumerable: true,\n"
    "    get: function () { return this.__om || null; },\n"
    "    set: function (f) { this.__om = typeof f === 'function' ? f : null; } });\n"
    "  class SharedWorker extends g.EventTarget {\n"
    "    constructor(url, options) {\n"
    "      if (arguments.length < 1) throw new TypeError(\"Failed to construct 'SharedWorker': 1 argument required.\");\n"
    "      super();\n"
    "      var port = new MessagePortOf();\n"
    "      Object.defineProperty(port, '__w', { value: start(port, new URL(String(url), g.location.href).href, true) });\n"
    "      Object.defineProperty(this, '__port', { value: port });\n"
    "    }\n"
    "  }\n"
    "  Object.defineProperty(SharedWorker.prototype, 'port', { configurable: true, enumerable: true,\n"
    "    get: { port: function () { if (!(this instanceof SharedWorker)) throw new TypeError('Illegal invocation'); return this.__port; } }.port });\n"
    "  Object.defineProperty(SharedWorker.prototype, 'onerror', { configurable: true, enumerable: true,\n"
    "    get: function () { if (!(this instanceof SharedWorker)) throw new TypeError('Illegal invocation'); return this.__oe || null; },\n"
    "    set: function (f) { if (!(this instanceof SharedWorker)) throw new TypeError('Illegal invocation'); this.__oe = typeof f === 'function' ? f : null; } });\n"
    "  Object.defineProperty(g, 'SharedWorker', { value: SharedWorker, writable: true, configurable: true });\n"
    "  g.__cyberWorkerDeliver = function (w, data) { w.dispatchEvent(new MessageEvent('message', { data: g.__cyberUnwire(data) })); };\n"
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
