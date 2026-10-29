/* webgpu_api.c - WebGL and WebGPU in a tab (SANDBOX_DESIGN.md section 5.7):
 * the natives browser/webgl_api.js and browser/webgpu_api.js use, over the
 * bridge to the site's web-GPU process the tab gives (webgpu_bridge.h).
 * Without one (the single-process browser) the APIs say they are
 * unavailable: getContext("webgl") is null, navigator.gpu.requestAdapter()
 * resolves to null.
 *
 * The natives reach the page only through those scripts' closures, never a
 * global.  A presented canvas comes back as pixels, put in the image cache
 * as "cyber-canvas:<node>" and drawn like an image (display_list.c); its
 * first frame or a new size lays the page out again, later frames only
 * repaint (webgpu_canvas_dirty_take). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "browser_api_impl_types.h"
#include "dom_store.h"
#include "dom_wrap.h"
#include "display_list.h"
#include "image_cache.h"
#include "platform.h"
#include "url_whatwg.h"
#include "webgpu_bridge.h"
#include "../ipc/proto.h"

extern const char *g_cyber_start_url;

static const char *webgl_gen_js =
#include "webgl_gen_js.inc"
;
static const char *webgl_api_js =
#include "webgl_api_js.inc"
;
static const char *webgpu_api_js =
#include "webgpu_api_js.inc"
;
static const char *webgpu_lazy_js =
#include "webgpu_lazy_js.inc"
;

static const WebgpuBridge *g_bridge;
static int g_state;                 /* 0 not connected, 1 connected, -1 none to be had */
static uint32_t g_seq;
static bool g_dirty;
static uint32_t g_lost[64];         /* contexts lost while waiting */
static int g_nlost;

void webgpu_bridge_set(const WebgpuBridge *b) { g_bridge = b; }

bool webgpu_canvas_dirty_take(void) {
    bool d = g_dirty;
    g_dirty = false;
    return d;
}

/* ---- canvases with a frame ------------------------------------------------------ */

typedef struct Shown { uint32_t node, w, h; } Shown;
static Shown g_shown[256];
static int g_nshown;

bool webgpu_canvas_has_frame(uint32_t node) {
    for (int i = 0; i < g_nshown; i++)
        if (g_shown[i].node == node) return true;
    return false;
}

static bool connected(void) {
    if (g_state == 0) g_state = g_bridge && g_bridge->connect() ? 1 : -1;
    return g_state > 0;
}

/* Wait for a reply of `type` to seq; its body copied (malloc'd).  Lost
 * contexts noted on the way.  NULL if the channel broke. */
static uint8_t *wait_reply(uint16_t type, uint32_t ctx, uint32_t seq, uint32_t *len) {
    for (;;) {
        uint16_t t;
        const uint8_t *b;
        uint32_t n;
        if (!g_bridge->recv(&t, &b, &n)) {
            g_state = -1;               /* the web-GPU process is gone: every context is lost */
            return NULL;
        }
        if (t == WT_LOST && n == sizeof(TwContext)) {
            TwContext x;
            memcpy(&x, b, sizeof(x));
            if (g_nlost < 64) g_lost[g_nlost++] = x.ctx;
            continue;
        }
        if (t != type || n < 8) continue;
        uint32_t c, s;
        memcpy(&c, b, 4);
        memcpy(&s, b + 4, 4);
        if (c != ctx || (type != WT_GL_CREATED && s != seq)) continue;
        uint8_t *copy = (uint8_t *)malloc(n ? n : 1);
        if (copy) memcpy(copy, b, n);
        *len = n;
        return copy;
    }
}

/* available(): whether there is a web-GPU process to be had. */
static GCValue n_available(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_NewBool(ctx, connected());
}

/* glCreate(ctx, version, w, h, flags): true, or why not. */
static GCValue n_gl_create(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    uint32_t v[5] = { 0 };
    for (int i = 0; i < 5 && i < argc; i++) JS_ToUint32(ctx, &v[i], argv[i]);
    if (!connected()) return JS_NewString(ctx, "no web-GPU process");
    TwGlCreate q = { v[0], v[1], v[2], v[3], v[4] };
    if (!g_bridge->send(TW_GL_CREATE, &q, sizeof(q), NULL, 0)) return JS_NewString(ctx, "the web-GPU process is gone");
    uint32_t len = 0;
    uint8_t *r = wait_reply(WT_GL_CREATED, q.ctx, 0, &len);
    if (!r) return JS_NewString(ctx, "the web-GPU process is gone");
    WtCreated c;
    memcpy(&c, r, sizeof(c));
    GCValue out = c.ok ? JS_TRUE : JS_NewStringLen(ctx, (const char *)r + sizeof(c), len - sizeof(c));
    free(r);
    return out;
}

/* The bytes of an ArrayBuffer or a view. */
static const uint8_t *bytes_of(JSContextHandle ctx, GCValue v, size_t *n) {
    size_t size = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, v);
    if (p) { *n = size; return p; }
    JS_GetException(ctx);
    size_t off = 0, len = 0, per = 0;
    GCValue buf = JS_GetTypedArrayBuffer(ctx, v, &off, &len, &per);
    if (JS_IsException(buf)) { JS_GetException(ctx); return NULL; }
    p = JS_GetArrayBuffer(ctx, &size, buf);
    if (!p || off + len > size) return NULL;
    *n = len;
    return p + off;
}

/* glRun(ctx, records, reply) / gpuRun(records, reply): the results (an
 * ArrayBuffer) if reply, else true; null if the web-GPU process is gone or
 * a record was malformed. */
static GCValue run(JSContextHandle ctx, uint16_t type, uint16_t reply_type, uint32_t cid, GCValue recs, bool reply) {
    size_t n = 0;
    const uint8_t *p = bytes_of(ctx, recs, &n);
    if (!p || !connected() || n > (64u << 20)) return JS_NULL;
    uint32_t seq = ++g_seq;
    bool sent;
    if (type == TW_GL_RUN) {
        TwGlRun q = { cid, seq, reply };
        sent = g_bridge->send(type, &q, sizeof(q), p, (uint32_t)n);
    } else {
        TwGpuRun q = { seq, reply };
        sent = g_bridge->send(type, &q, sizeof(q), p, (uint32_t)n);
    }
    if (!sent) { g_state = -1; return JS_NULL; }
    if (!reply) return JS_TRUE;
    uint32_t len = 0;
    uint8_t *r = wait_reply(reply_type, cid, seq, &len);
    if (!r) return JS_NULL;
    WtResult res;
    memcpy(&res, r, sizeof(res));
    GCValue out = res.ok ? JS_NewArrayBufferCopy(ctx, r + sizeof(res), len - sizeof(res)) : JS_NULL;
    free(r);
    return out;
}

static GCValue n_gl_run(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    uint32_t cid = 0;
    if (argc < 3) return JS_NULL;
    JS_ToUint32(ctx, &cid, argv[0]);
    return run(ctx, TW_GL_RUN, WT_GL_RESULT, cid, argv[1], JS_ToBool(ctx, argv[2]));
}

static GCValue n_gpu_run(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_NULL;
    return run(ctx, TW_GPU_RUN, WT_GPU_RESULT, 0, argv[0], JS_ToBool(ctx, argv[1]));
}

static GCValue n_resize(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    uint32_t v[3] = { 0 };
    for (int i = 0; i < 3 && i < argc; i++) JS_ToUint32(ctx, &v[i], argv[i]);
    TwGlResize q = { v[0], v[1], v[2] };
    if (connected()) g_bridge->send(TW_GL_RESIZE, &q, sizeof(q), NULL, 0);
    return JS_UNDEFINED;
}

static GCValue n_destroy(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    TwContext q = { 0 };
    if (argc > 0) JS_ToUint32(ctx, &q.ctx, argv[0]);
    if (connected()) g_bridge->send(TW_DESTROY, &q, sizeof(q), NULL, 0);
    return JS_UNDEFINED;
}

/* present(ctx, canvas): its pixels from the web-GPU process into the image
 * cache, for the canvas to be drawn with.  False if lost. */
static GCValue n_present(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 2 || !connected()) return JS_FALSE;
    uint32_t cid = 0;
    JS_ToUint32(ctx, &cid, argv[0]);
    NodeId node = dom_node_of(argv[1]);
    TwPresent q = { cid, ++g_seq };
    if (!node || !g_bridge->send(TW_PRESENT, &q, sizeof(q), NULL, 0)) return JS_FALSE;
    uint32_t len = 0;
    uint8_t *r = wait_reply(WT_FRAME, cid, q.seq, &len);
    if (!r) return JS_FALSE;
    WtFrame f;
    memcpy(&f, r, sizeof(f));
    ImageCache *cache = display_list_get_image_cache();
    if (cache && f.width && f.height && (uint64_t)f.width * f.height * 4 == len - sizeof(f)) {
        char key[48];
        snprintf(key, sizeof(key), "cyber-canvas:%u", (unsigned)node);
        image_cache_put_rgba(cache, key, (int)f.width, (int)f.height, r + sizeof(f));
        int i = 0;
        while (i < g_nshown && g_shown[i].node != node) i++;
        bool fresh = i == g_nshown;
        if (fresh && g_nshown < 256) g_nshown++;
        if (i < 256 && (fresh || g_shown[i].w != f.width || g_shown[i].h != f.height)) {
            g_shown[i].node = node;
            g_shown[i].w = f.width;
            g_shown[i].h = f.height;
            dom_request_layout();           /* a new canvas, or a new size: lay out */
        } else {
            g_dirty = true;                 /* the same: paint again */
        }
    }
    free(r);
    return JS_TRUE;
}

/* newId() / freeId(id): ids for WebGL contexts, WebGPU canvases and
 * WebGPU objects, one space for the tab (its realms share the web-GPU
 * process), below the process's 65536; 0 when none are left. */
#define MAX_IDS 65536
static uint32_t g_next_id = 1;
static uint32_t g_free_ids[MAX_IDS];
static uint32_t g_nfree;

static GCValue n_new_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    uint32_t id = g_nfree ? g_free_ids[--g_nfree] : g_next_id < MAX_IDS ? g_next_id++ : 0;
    return JS_NewUint32(ctx, id);
}

static GCValue n_free_id(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    uint32_t id = 0;
    if (argc > 0) JS_ToUint32(ctx, &id, argv[0]);
    if (id && id < g_next_id && g_nfree < MAX_IDS) g_free_ids[g_nfree++] = id;
    return JS_UNDEFINED;
}

/* lost(): the contexts lost since last asked (and -1 if all are: the
 * process is gone). */
static GCValue n_lost(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    GCValue a = JS_NewArray(ctx);
    uint32_t k = 0;
    for (int i = 0; i < g_nlost; i++) JS_SetPropertyUint32(ctx, a, k++, JS_NewUint32(ctx, g_lost[i]));
    if (g_state < 0 && g_bridge) JS_SetPropertyUint32(ctx, a, k++, JS_NewInt32(ctx, -1));
    g_nlost = 0;
    return a;
}

/* imageSource(element): an <img>'s or <video>'s current pixels, as
 * { width, height, data: ArrayBuffer (RGBA) }, or null. */
static GCValue n_image_source(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    NodeId node = dom_node_of(argv[0]);
    if (!node) return JS_NULL;
    const uint8_t *px = NULL;
    int w = 0, h = 0;
    uint64_t fid = 0;
    uint8_t *owned = NULL;
    if (!media_video_frame_for_node(node, &px, &w, &h, &fid)) {
        const char *src = dom_get_attr_str(node, "src");
        ImageCache *cache = display_list_get_image_cache();
        if (!src || !src[0] || !cache) return JS_NULL;
        char *url = wurl_resolve(g_cyber_start_url, src);
        int hd = image_cache_request(cache, url ? url : src, 0, 0);
        free(url);
        int ch = 0;
        uint8_t *p = NULL;
        if (hd < 0 || !image_cache_get(cache, hd, &w, &h, &ch, &p) || !p || ch != 4) return JS_NULL;
        px = p;
    }
    (void)owned;
    if (w <= 0 || h <= 0) return JS_NULL;
    GCValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "width", JS_NewInt32(ctx, w));
    JS_SetPropertyStr(ctx, o, "height", JS_NewInt32(ctx, h));
    JS_SetPropertyStr(ctx, o, "data", JS_NewArrayBufferCopy(ctx, px, (size_t)w * h * 4));
    return o;
}

static void report(JSContextHandle ctx, const char *what) {
    GCValue e = JS_GetException(ctx);
    const char *m = JS_ToCString(ctx, e);
    GCValue st = JS_IsObject(e) ? JS_GetPropertyStr(ctx, e, "stack") : JS_UNDEFINED;
    const char *s = JS_IsString(st) ? JS_ToCString(ctx, st) : NULL;
    platform_log(LOG_LEVEL_ERROR, "webgpu_api", "%s failed: %s %s", what, m ? m : "?", s ? s : "");
}

/* install(global, window, native): webgl_api.js and webgpu_api.js for a
 * realm, on its first use of either (webgpu_lazy.js). */
static GCValue n_install(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 3) return JS_UNDEFINED;
    GCValue gen = JS_Eval(ctx, webgl_gen_js, strlen(webgl_gen_js), "<webgl_gen>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(gen)) { report(ctx, "webgl_gen"); return JS_UNDEFINED; }
    GCValue fn = JS_Eval(ctx, webgl_api_js, strlen(webgl_api_js), "<webgl_api>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) { report(ctx, "webgl_api"); return JS_UNDEFINED; }
    GCValue args[4] = { argv[0], argv[1], argv[2], gen };
    if (JS_IsException(JS_Call(ctx, fn, JS_UNDEFINED, 4, args))) report(ctx, "webgl_api");
    fn = JS_Eval(ctx, webgpu_api_js, strlen(webgpu_api_js), "<webgpu_api>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) { report(ctx, "webgpu_api"); return JS_UNDEFINED; }
    if (JS_IsException(JS_Call(ctx, fn, JS_UNDEFINED, 3, args))) report(ctx, "webgpu_api");
    return JS_UNDEFINED;
}

void webgpu_api_install(JSContextHandle ctx, GCValue global, GCValue window) {
    GCValue nat = JS_NewObject(ctx);
    static const struct { const char *name; JSCFunction *fn; int n; } fns[] = {
        { "available", n_available, 0 }, { "glCreate", n_gl_create, 5 }, { "glRun", n_gl_run, 3 },
        { "gpuRun", n_gpu_run, 2 }, { "resize", n_resize, 3 }, { "destroy", n_destroy, 1 },
        { "present", n_present, 2 }, { "lost", n_lost, 0 }, { "imageSource", n_image_source, 1 },
        { "newId", n_new_id, 0 }, { "freeId", n_free_id, 1 },
    };
    for (size_t i = 0; i < sizeof(fns) / sizeof(fns[0]); i++)
        JS_SetPropertyStr(ctx, nat, fns[i].name, JS_NewCFunction(ctx, fns[i].fn, fns[i].name, fns[i].n));
    GCValue fn = JS_Eval(ctx, webgpu_lazy_js, strlen(webgpu_lazy_js), "<webgpu_lazy>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) { report(ctx, "webgpu_lazy"); return; }
    GCValue args[4] = { global, window, nat, JS_NewCFunction(ctx, n_install, "install", 3) };
    if (JS_IsException(JS_Call(ctx, fn, JS_UNDEFINED, 4, args))) report(ctx, "webgpu_lazy");
}
