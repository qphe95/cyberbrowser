/*
 * HTMLImageElement loading behaviour on top of the image cache.
 *
 * - naturalWidth / naturalHeight / complete / currentSrc getters read the
 *   image cache (and start the fetch, as reading them in a browser would
 *   only after the fetch began — harmless and it gets bytes moving early);
 * - decode() returns a promise (resolved immediately: decoding is not on
 *   the critical path of script);
 * - image_api_scan() starts fetches for every connected <img> with a
 *   source, so images load whether or not they are laid out (display:none,
 *   off-screen), like a browser's image loader;
 * - image_api_notify() runs on the main thread when the cache reports that
 *   an image was fetched (or failed): every connected <img> showing that URL
 *   gets a `load` (or `error`) event, once per URL.  Pages gate visibility on
 *   it (YouTube adds ytCoreImageLoaded in its load listener).
 */
#include "image_api.h"

#include <stdlib.h>
#include <string.h>

#include "dom_store.h"
#include "dom_wrap.h"
#include "display_list.h"
#include "image_cache.h"
#include "url_whatwg.h"
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "css_style.h"
#include <stdio.h>

extern const char *g_cyber_start_url;
GCValue js_event_target_dispatchEvent(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

/* Resolved URL an <img> node shows (srcset candidate, else src). */
static char *img_url_of(NodeId id)
{
    const char *src = dom_get_attr_str(id, "src");
    const char *srcset = dom_get_attr_str(id, "srcset");
    char *picked = srcset && srcset[0] ? display_list_pick_srcset(srcset, 0.0f) : NULL;
    if (picked) src = picked;
    char *url = NULL;
    if (src && src[0]) {
        const char *base = (g_cyber_start_url && g_cyber_start_url[0]) ? g_cyber_start_url : NULL;
        url = wurl_resolve(base, src);
        if (!url) url = strdup(src);
    }
    free(picked);
    return url;
}

static bool is_img(NodeId id)
{
    static CssAtom img_atom = CSS_ATOM_NONE;
    if (img_atom == CSS_ATOM_NONE) img_atom = css_atom_find("img", 3);
    return dom_kind(id) == DOM_K_ELEMENT && img_atom != CSS_ATOM_NONE && dom_tag(id) == img_atom;
}

static GCValue img_natural(JSContextHandle ctx, GCValue this_val, bool height)
{
    NodeId id = dom_node_of(this_val);
    ImageCache *ic = display_list_get_image_cache();
    if (!id || !ic) return JS_NewInt32(ctx, 0);
    char *url = img_url_of(id);
    int w = 0, h = 0;
    if (url) image_cache_natural_size(ic, url, &w, &h);
    free(url);
    return JS_NewInt32(ctx, height ? h : w);
}

static GCValue js_img_natural_width(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    return img_natural(ctx, this_val, false);
}

static GCValue js_img_natural_height(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    return img_natural(ctx, this_val, true);
}

static GCValue js_img_complete(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    NodeId id = dom_node_of(this_val);
    if (!id) return JS_TRUE;
    char *url = img_url_of(id);
    if (!url) return JS_TRUE;               /* no source: complete */
    ImageCache *ic = display_list_get_image_cache();
    int st = ic ? image_cache_status(ic, url) : 0;
    free(url);
    return JS_NewBool(ctx, st != 0);
}

static GCValue js_img_current_src(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    NodeId id = dom_node_of(this_val);
    char *url = id ? img_url_of(id) : NULL;
    GCValue v = JS_NewString(ctx, url ? url : "");
    free(url);
    return v;
}

static void define_getter(JSContextHandle ctx, GCValue proto, const char *name, JSCFunction *fn)
{
    char gname[64];
    snprintf(gname, sizeof(gname), "get %s", name);
    GCValue getter = JS_NewCFunction(ctx, fn, gname, 0);
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, atom, getter, JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
}

GCValue js_element_set_attribute(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv);

/* src: reflects the attribute; reads give the resolved URL. */
static GCValue js_img_get_src(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    NodeId id = dom_node_of(this_val);
    const char *src = id ? dom_get_attr_str(id, "src") : NULL;
    if (!src || !src[0]) return JS_NewString(ctx, "");
    const char *base = (g_cyber_start_url && g_cyber_start_url[0]) ? g_cyber_start_url : NULL;
    char *url = wurl_resolve(base, src);
    GCValue v = JS_NewString(ctx, url ? url : src);
    free(url);
    return v;
}

static GCValue js_img_get_srcset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)argc; (void)argv;
    NodeId id = dom_node_of(this_val);
    const char *v = id ? dom_get_attr_str(id, "srcset") : NULL;
    return JS_NewString(ctx, v ? v : "");
}

static GCValue img_set_attr(JSContextHandle ctx, GCValue this_val, const char *name, GCValue value)
{
    GCValue args[2] = { JS_NewString(ctx, name), value };
    js_element_set_attribute(ctx, this_val, 2, args);
    /* Start the fetch right away (as the browser's image loader does). */
    NodeId id = dom_node_of(this_val);
    ImageCache *ic = display_list_get_image_cache();
    if (id && ic && dom_is_connected(id)) {
        char *url = img_url_of(id);
        if (url) {
            int w, h;
            image_cache_natural_size(ic, url, &w, &h);
            free(url);
        }
    }
    return JS_UNDEFINED;
}

static GCValue js_img_set_src(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    return img_set_attr(ctx, this_val, "src", argc > 0 ? argv[0] : JS_UNDEFINED);
}

static GCValue js_img_set_srcset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    return img_set_attr(ctx, this_val, "srcset", argc > 0 ? argv[0] : JS_UNDEFINED);
}

static void define_accessor(JSContextHandle ctx, GCValue proto, const char *name,
                            JSCFunction *get, JSCFunction *set)
{
    char gname[64], sname[64];
    snprintf(gname, sizeof(gname), "get %s", name);
    snprintf(sname, sizeof(sname), "set %s", name);
    JSAtom atom = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, atom, JS_NewCFunction(ctx, get, gname, 0),
                            JS_NewCFunction(ctx, set, sname, 1),
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
}

void image_api_install(JSContextHandle ctx, GCValue img_proto)
{
    if (!JS_IsObject(img_proto)) return;
    define_accessor(ctx, img_proto, "src", js_img_get_src, js_img_set_src);
    define_accessor(ctx, img_proto, "srcset", js_img_get_srcset, js_img_set_srcset);
    define_getter(ctx, img_proto, "naturalWidth", js_img_natural_width);
    define_getter(ctx, img_proto, "naturalHeight", js_img_natural_height);
    define_getter(ctx, img_proto, "complete", js_img_complete);
    define_getter(ctx, img_proto, "currentSrc", js_img_current_src);
    /* decode(): resolved promise; `new Image(w, h)`. */
    static const char *js =
        "(function(P){"
        "  if (!P.decode) Object.defineProperty(P, 'decode', {configurable:true, writable:true,"
        "    value: function(){ return Promise.resolve(); }});"
        "  if (typeof Image === 'undefined') {"
        "    var I = function Image(w, h){ var e = document.createElement('img');"
        "      if (w !== undefined) e.setAttribute('width', String(w));"
        "      if (h !== undefined) e.setAttribute('height', String(h)); return e; };"
        "    I.prototype = P; globalThis.Image = I;"
        "  }"
        "})";
    GCValue fn = JS_Eval(ctx, js, strlen(js), "<image_api>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsFunction(ctx, fn)) JS_Call(ctx, fn, JS_UNDEFINED, 1, &img_proto);
}

void image_api_scan(JSContextHandle ctx)
{
    (void)ctx;
    ImageCache *ic = display_list_get_image_cache();
    if (!ic) return;
    for (uint32_t s = 1; s < g_dom.count; s++) {
        NodeId id = dom_id_of(s);
        if (!dom_valid(id) || !is_img(id) || !dom_is_connected(id)) continue;
        char *url = img_url_of(id);
        if (url) {
            int w, h;
            image_cache_natural_size(ic, url, &w, &h);   /* starts the fetch */
            free(url);
        }
    }
}

static void fire(JSContextHandle ctx, GCValue el, const char *type)
{
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue ctor = JS_GetPropertyStr(ctx, global, "Event");
    if (!JS_IsFunction(ctx, ctor)) return;
    GCValue tv = JS_NewString(ctx, type);
    GCValue ev = JS_CallConstructor(ctx, ctor, 1, &tv);
    if (JS_IsException(ev) || !JS_IsObject(ev)) return;
    js_event_target_dispatchEvent(ctx, el, 1, &ev);
}

void image_api_notify(JSContextHandle ctx, const char *url)
{
    ImageCache *ic = display_list_get_image_cache();
    if (!ic || !url) return;
    int st = image_cache_status(ic, url);
    if (st == 0) return;                    /* still in flight */
    const char *type = st > 0 ? "load" : "error";
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("CYBER_DEBUG_IMG") ? 1 : 0;
    int nimg = 0, nmatch = 0, nfired = 0;
    for (uint32_t s = 1; s < g_dom.count; s++) {
        NodeId id = dom_id_of(s);
        if (!dom_valid(id) || !is_img(id)) continue;
        nimg++;
        char *u = img_url_of(id);
        if (dbg == 1 && u) fprintf(stderr, "[IMG] cand w=%d c=%d %.100s\n", dom_has_wrapper(id), dom_is_connected(id), u);
        if (!dom_has_wrapper(id) || !dom_is_connected(id)) { free(u); continue; }
        bool match = u && strcmp(u, url) == 0;
        free(u);
        if (!match) continue;
        nmatch++;
        GCValue el = dom_js_existing(id);
        if (!JS_IsObject(el)) continue;
        /* Once per URL per element. */
        GCValue fired = JS_GetPropertyStr(ctx, el, "__cyber_img_fired");
        if (JS_IsString(fired)) {
            const char *f = JS_ToCString(ctx, fired);
            bool same = f && strcmp(f, url) == 0;
            if (same) continue;
        }
        JS_DefinePropertyValueStr(ctx, el, "__cyber_img_fired", JS_NewString(ctx, url),
                                  JS_PROP_CONFIGURABLE | JS_PROP_WRITABLE);
        fire(ctx, el, type);
        nfired++;
    }
    if (dbg) fprintf(stderr, "[IMG] %s %.100s: imgs=%d match=%d fired=%d\n", type, url, nimg, nmatch, nfired);
    if (dbg == 1) dbg = 2;
}
