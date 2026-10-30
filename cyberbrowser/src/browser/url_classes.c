/*
 * url_classes.c - URL, URLSearchParams (WHATWG URL Standard 6) and the
 * HTMLHyperlinkElementUtils of <a> and <area> (HTML 4.6.3), over the C URL
 * parser (url_whatwg.c).
 *
 *   - a URL holds its parsed record; each attribute serializes the part it
 *     names when read.  Its searchParams object is made when first read,
 *     and kept in step by the href and search setters;
 *   - a URLSearchParams holds its name-value list; a change runs the
 *     "update steps" (its URL's query becomes the serialized list);
 *   - <a>/<area> attributes parse the href attribute against the document
 *     base URL, which is cached while the document URL and the first
 *     <base href> stay the same.
 * Strings are converted as USVStrings: a lone surrogate becomes U+FFFD.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "dom_query.h"
#include "url_whatwg.h"

/* ---- strings ---- */

/* String(v) as UTF-8 with lone surrogates (QuickJS writes them as 3-byte
 * ED A0..BF sequences) replaced by U+FFFD.  malloc'd; NULL with an
 * exception pending. */
static char *usv_dup(JSContextHandle ctx, GCValue v, size_t *len) {
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s) return NULL;
    char *out = malloc(n + 1);
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xED && i + 2 < n && ((unsigned char)s[i + 1] & 0xE0) == 0xA0) {
            out[o++] = (char)0xEF; out[o++] = (char)0xBF; out[o++] = (char)0xBD;
            i += 3;
            continue;
        }
        out[o++] = s[i++];
    }
    out[o] = '\0';
    JS_FreeCString(ctx, s);
    if (len) *len = o;
    return out;
}

static GCValue need_args(JSContextHandle ctx, int argc, int n, const char *op, const char *iface) {
    if (argc >= n) return JS_UNDEFINED;
    return JS_ThrowTypeError(ctx, "Failed to execute '%s' on '%s': %d argument%s required, but only %d present.",
                             op, iface, n, n > 1 ? "s" : "", argc);
}
#define NEED(n, op, iface) do { if (argc < (n)) return need_args(ctx, argc, n, op, iface); } while (0)

/* ---- the classes ---- */

typedef struct Pair { char *name, *value; size_t nl, vl; } Pair;
typedef struct Params { Pair *p; int n, cap; } Params;
typedef struct UrlRec { WUrl u; } UrlRec;
typedef struct ParamsIter { int kind, index; } ParamsIter;     /* kind: 0 keys, 1 values, 2 entries */

/* The GC cell an object's opaque handle points at. */
typedef struct Cell { void *data; } Cell;

static JSClassID g_url_class, g_params_class, g_iter_class;

static void *cell_of(JSContextHandle ctx, GCValue obj, JSClassID id) {
    GCHandle h = JS_GetOpaqueHandle2(ctx, obj, id);
    if (h == GC_HANDLE_NULL) return NULL;
    Cell *c = (Cell *)gc_deref(h);
    return c ? c->data : NULL;
}

static void params_clear(Params *p) {
    for (int i = 0; i < p->n; i++) { free(p->p[i].name); free(p->p[i].value); }
    p->n = 0;
}

static void finalize(GCValue val, JSClassID id) {
    GCHandle h = JS_GetOpaqueHandle(val, id);
    if (h == GC_HANDLE_NULL) return;
    Cell *c = (Cell *)gc_deref(h);
    if (!c || !c->data) return;
    if (id == g_url_class) wurl_free(&((UrlRec *)c->data)->u);
    else if (id == g_params_class) { params_clear(c->data); free(((Params *)c->data)->p); }
    free(c->data);
    c->data = NULL;
}
static void url_finalizer(JSRuntimeHandle rt, GCValue v) { (void)rt; finalize(v, g_url_class); }
static void params_finalizer(JSRuntimeHandle rt, GCValue v) { (void)rt; finalize(v, g_params_class); }
static void iter_finalizer(JSRuntimeHandle rt, GCValue v) { (void)rt; finalize(v, g_iter_class); }
static void url_mark(JSRuntimeHandle rt, GCValue v, JS_MarkFunc *m) { GCHandle h = JS_GetOpaqueHandle(v, g_url_class); if (h != GC_HANDLE_NULL) m(rt, h); }
static void params_mark(JSRuntimeHandle rt, GCValue v, JS_MarkFunc *m) { GCHandle h = JS_GetOpaqueHandle(v, g_params_class); if (h != GC_HANDLE_NULL) m(rt, h); }
static void iter_mark(JSRuntimeHandle rt, GCValue v, JS_MarkFunc *m) { GCHandle h = JS_GetOpaqueHandle(v, g_iter_class); if (h != GC_HANDLE_NULL) m(rt, h); }

static JSClassDef g_url_def = { .class_name = "URL", .finalizer = url_finalizer, .gc_mark = url_mark };
static JSClassDef g_params_def = { .class_name = "URLSearchParams", .finalizer = params_finalizer, .gc_mark = params_mark };
static JSClassDef g_iter_def = { .class_name = "URLSearchParams Iterator", .finalizer = iter_finalizer, .gc_mark = iter_mark };

/* A new object of class id with this prototype (or the global
 * constructor's), owning data. */
static GCValue new_obj(JSContextHandle ctx, GCValue proto, JSClassID id, void *data) {
    GCValue obj = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, id) : JS_NewObjectClass(ctx, id);
    if (JS_IsException(obj)) { free(data); return obj; }
    GCHandle h = gc_allocz(sizeof(Cell), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) { free(data); return JS_ThrowOutOfMemory(ctx); }
    ((Cell *)gc_deref(h))->data = data;
    JS_SetOpaqueHandle(obj, h);
    return obj;
}

static GCValue global_proto(JSContextHandle ctx, const char *ctor) {
    GCValue c = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), ctor);
    return JS_IsObject(c) ? JS_GetPropertyStr(ctx, c, "prototype") : JS_UNDEFINED;
}

/* For a constructor: new.target's prototype (subclasses), else the class's. */
static GCValue ctor_proto(JSContextHandle ctx, GCValue new_target, const char *name) {
    GCValue p = JS_IsObject(new_target) ? JS_GetPropertyStr(ctx, new_target, "prototype") : JS_UNDEFINED;
    return JS_IsObject(p) ? p : global_proto(ctx, name);
}

/* ---- application/x-www-form-urlencoded ---- */

static void params_push(Params *p, char *name, size_t nl, char *value, size_t vl) {
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->p = realloc(p->p, (size_t)p->cap * sizeof(Pair));
    }
    p->p[p->n++] = (Pair){ name, value, nl, vl };
}

static void params_parse(Params *p, const char *s, size_t n) {
    params_clear(p);
    for (size_t i = 0; i < n; ) {
        size_t e = i;
        while (e < n && s[e] != '&') e++;
        if (e > i) {
            size_t eq = i;
            while (eq < e && s[eq] != '=') eq++;
            size_t nl = 0, vl = 0;
            char *name = wurl_form_decode(s + i, eq - i, &nl);
            char *value = eq < e ? wurl_form_decode(s + eq + 1, e - eq - 1, &vl) : strdup("");
            params_push(p, name ? name : strdup(""), nl, value ? value : strdup(""), vl);
        }
        i = e + 1;
    }
}

static char *params_serialize(const Params *p) {
    size_t cap = 64, len = 0;
    char *out = malloc(cap);
    out[0] = '\0';
    for (int i = 0; i < p->n; i++) {
        char *n = wurl_form_encode(p->p[i].name, p->p[i].nl), *v = wurl_form_encode(p->p[i].value, p->p[i].vl);
        size_t need = len + (n ? strlen(n) : 0) + (v ? strlen(v) : 0) + 3;
        if (need > cap) { while (need > cap) cap *= 2; out = realloc(out, cap); }
        len += (size_t)sprintf(out + len, "%s%s=%s", i ? "&" : "", n ? n : "", v ? v : "");
        free(n);
        free(v);
    }
    return out;
}

/* The URL's query as the list sees it. */
static void params_from_url(Params *p, const WUrl *u) {
    if (u->query) params_parse(p, u->query, strlen(u->query));
    else params_clear(p);
}

/* ---- URL ---- */

static GCValue url_sp(JSContextHandle ctx, GCValue url_obj, UrlRec *r, bool make);

/* After the query changed through URL: its searchParams' list follows. */
static void url_query_changed(JSContextHandle ctx, GCValue obj, UrlRec *r) {
    GCValue sp = JS_GetPropertyStr(ctx, obj, "__sp");
    Params *p = JS_IsObject(sp) ? cell_of(ctx, sp, g_params_class) : NULL;
    if (p) params_from_url(p, &r->u);
}

static GCValue js_url_ctor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    NEED(1, "constructor", "URL");
    if (JS_IsUndefined(new_target)) return JS_ThrowTypeError(ctx, "Failed to construct 'URL': Please use the 'new' operator");
    size_t n = 0, bn = 0;
    char *in = usv_dup(ctx, argv[0], &n);
    if (!in) return JS_EXCEPTION;
    WUrl base, u;
    bool has_base = argc > 1 && !JS_IsUndefined(argv[1]);
    if (has_base) {
        char *b = usv_dup(ctx, argv[1], &bn);
        if (!b) { free(in); return JS_EXCEPTION; }
        int bad = wurl_parse(b, bn, NULL, &base);
        free(b);
        if (bad) { free(in); return JS_ThrowTypeError(ctx, "Failed to construct 'URL': Invalid base URL"); }
    }
    int bad = wurl_parse(in, n, has_base ? &base : NULL, &u);
    if (has_base) wurl_free(&base);
    free(in);
    if (bad) return JS_ThrowTypeError(ctx, "Failed to construct 'URL': Invalid URL");
    UrlRec *r = malloc(sizeof *r);
    r->u = u;
    return new_obj(ctx, ctor_proto(ctx, new_target, "URL"), g_url_class, r);
}

/* URL.canParse(url, base) (magic 0) and URL.parse (magic 1). */
static GCValue js_url_static_parse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)this_val;
    NEED(1, magic ? "parse" : "canParse", "URL");
    if (magic) {
        GCValue ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "URL");
        GCValue r = JS_CallConstructor(ctx, ctor, argc > 1 && !JS_IsUndefined(argv[1]) ? 2 : 1, argv);
        if (JS_IsException(r)) { JS_GetException(ctx); return JS_NULL; }
        return r;
    }
    size_t n = 0, bn = 0;
    WUrl base, u;
    bool has_base = argc > 1 && !JS_IsUndefined(argv[1]);
    if (has_base) {
        char *b = usv_dup(ctx, argv[1], &bn);
        if (!b) return JS_EXCEPTION;
        int bad = wurl_parse(b, bn, NULL, &base);
        free(b);
        if (bad) return JS_FALSE;
    }
    char *in = usv_dup(ctx, argv[0], &n);
    if (!in) { if (has_base) wurl_free(&base); return JS_EXCEPTION; }
    int bad = wurl_parse(in, n, has_base ? &base : NULL, &u);
    free(in);
    if (has_base) wurl_free(&base);
    if (!bad) wurl_free(&u);
    return JS_NewBool(ctx, !bad);
}

/* The parts, in the order of the getters' magic. */
enum { P_HREF, P_ORIGIN, P_PROTOCOL, P_USERNAME, P_PASSWORD, P_HOST, P_HOSTNAME, P_PORT, P_PATHNAME, P_SEARCH, P_HASH };
static const WUrlSetter g_setter[] = { WURL_SET_HREF, -1, WURL_SET_PROTOCOL, WURL_SET_USERNAME, WURL_SET_PASSWORD,
                                       WURL_SET_HOST, WURL_SET_HOSTNAME, WURL_SET_PORT, WURL_SET_PATHNAME,
                                       WURL_SET_SEARCH, WURL_SET_HASH };
static const char *const g_part_names[] = { "href", "origin", "protocol", "username", "password", "host",
                                            "hostname", "port", "pathname", "search", "hash" };

static GCValue url_part(JSContextHandle ctx, const WUrl *u, int part) {
    char *s;
    switch (part) {
    case P_HREF: s = wurl_href(u); break;
    case P_ORIGIN: s = wurl_origin(u); break;
    case P_PROTOCOL: s = wurl_protocol(u); break;
    case P_USERNAME: return JS_NewString(ctx, u->username ? u->username : "");
    case P_PASSWORD: return JS_NewString(ctx, u->password ? u->password : "");
    case P_HOST: s = wurl_host(u); break;
    case P_HOSTNAME: s = wurl_hostname(u); break;
    case P_PORT: s = wurl_port(u); break;
    case P_PATHNAME: s = wurl_pathname(u); break;
    case P_SEARCH: s = wurl_search(u); break;
    default: s = wurl_hash(u); break;
    }
    GCValue v = JS_NewString(ctx, s ? s : "");
    free(s);
    return v;
}

#define THIS_URL(r) \
    UrlRec *r = cell_of(ctx, this_val, g_url_class); \
    if (!r) return JS_ThrowTypeError(ctx, "Illegal invocation")

static GCValue js_url_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)argc; (void)argv;
    THIS_URL(r);
    return url_part(ctx, &r->u, magic);
}

static GCValue js_url_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    THIS_URL(r);
    size_t n = 0;
    char *v = usv_dup(ctx, argc ? argv[0] : JS_UNDEFINED, &n);
    if (!v) return JS_EXCEPTION;
    if (magic == P_HREF) {
        WUrl u;
        int bad = wurl_parse(v, n, NULL, &u);
        free(v);
        if (bad) return JS_ThrowTypeError(ctx, "Failed to set the 'href' property on 'URL': Invalid URL");
        wurl_free(&r->u);
        r->u = u;
    } else {
        wurl_set(&r->u, g_setter[magic], v, n);
        free(v);
    }
    if (magic == P_HREF || magic == P_SEARCH) url_query_changed(ctx, this_val, r);
    return JS_UNDEFINED;
}

static GCValue js_url_search_params(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_URL(r);
    return url_sp(ctx, this_val, r, true);
}

/* The URL's URLSearchParams, made on first use (kept under a hidden
 * property, and holding its URL under one). */
static GCValue url_sp(JSContextHandle ctx, GCValue url_obj, UrlRec *r, bool make) {
    GCValue sp = JS_GetPropertyStr(ctx, url_obj, "__sp");
    if (JS_IsObject(sp) || !make) return sp;
    Params *p = calloc(1, sizeof *p);
    params_from_url(p, &r->u);
    sp = new_obj(ctx, global_proto(ctx, "URLSearchParams"), g_params_class, p);
    if (JS_IsException(sp)) return sp;
    JS_DefinePropertyValueStr(ctx, sp, "__url", url_obj, 0);
    JS_DefinePropertyValueStr(ctx, url_obj, "__sp", sp, 0);
    return sp;
}

/* ---- URLSearchParams ---- */

#define THIS_PARAMS(p) \
    Params *p = cell_of(ctx, this_val, g_params_class); \
    if (!p) return JS_ThrowTypeError(ctx, "Illegal invocation")

/* The update steps: the linked URL's query becomes the list ("" makes it
 * null, as the search setter does). */
static void params_update(JSContextHandle ctx, GCValue obj, const Params *p) {
    GCValue url = JS_GetPropertyStr(ctx, obj, "__url");
    UrlRec *r = JS_IsObject(url) ? cell_of(ctx, url, g_url_class) : NULL;
    if (!r) return;
    char *s = params_serialize(p);
    wurl_set(&r->u, WURL_SET_SEARCH, s, strlen(s));
    free(s);
}

/* Array.from(v) through the global (sequence initializers are rare). */
static GCValue array_from(JSContextHandle ctx, GCValue v) {
    GCValue from = JS_GetPropertyStr(ctx, JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "Array"), "from");
    return JS_Call(ctx, from, JS_UNDEFINED, 1, &v);
}

static GCValue js_params_ctor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (JS_IsUndefined(new_target)) return JS_ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': Please use the 'new' operator");
    Params *p = calloc(1, sizeof *p);
    GCValue init = argc ? argv[0] : JS_UNDEFINED;
    if (JS_IsObject(init)) {
        GCValue sym = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "Symbol");
        JSAtom ia = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "iterator"));
        GCValue it = JS_GetProperty(ctx, init, ia);
        JS_FreeAtom(ctx, ia);
        if (JS_IsException(it)) goto fail;
        if (!JS_IsUndefined(it) && !JS_IsNull(it)) {
            if (!JS_IsFunction(ctx, it)) {
                JS_ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': The object must have a callable @@iterator property.");
                goto fail;
            }
            GCValue pairs = array_from(ctx, init);
            if (JS_IsException(pairs)) goto fail;
            uint32_t n = 0;
            JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, pairs, "length"));
            for (uint32_t i = 0; i < n; i++) {
                GCValue pair = JS_GetPropertyUint32(ctx, pairs, i);
                if (!JS_IsObject(pair)) {
                    JS_ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': The provided value cannot be converted to a sequence.");
                    goto fail;
                }
                GCValue arr = array_from(ctx, pair);
                if (JS_IsException(arr)) goto fail;
                uint32_t len = 0;
                JS_ToUint32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
                if (len != 2) {
                    JS_ThrowTypeError(ctx, "Failed to construct 'URLSearchParams': Sequence initializer must only contain pair elements");
                    goto fail;
                }
                size_t nl = 0, vl = 0;
                char *nm = usv_dup(ctx, JS_GetPropertyUint32(ctx, arr, 0), &nl);
                char *vv = nm ? usv_dup(ctx, JS_GetPropertyUint32(ctx, arr, 1), &vl) : NULL;
                if (!vv) { free(nm); goto fail; }
                params_push(p, nm, nl, vv, vl);
            }
        } else {
            /* record<USVString, USVString>: own enumerable keys, a later
             * duplicate (after conversion) replacing the value in place */
            JSPropertyEnum *tab = NULL;
            uint32_t n = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &n, init, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_ENUM_ONLY) < 0)
                goto fail;
            for (uint32_t i = 0; i < n; i++) {
                GCValue key = JS_AtomToValue(ctx, tab[i].atom);
                size_t nl = 0, vl = 0;
                char *nm = usv_dup(ctx, key, &nl);
                if (!nm) { JS_FreePropertyEnum(ctx, tab, n); goto fail; }
                char *vv = usv_dup(ctx, JS_GetProperty(ctx, init, tab[i].atom), &vl);
                if (!vv) { free(nm); JS_FreePropertyEnum(ctx, tab, n); goto fail; }
                int k = 0;
                while (k < p->n && !(p->p[k].nl == nl && memcmp(p->p[k].name, nm, nl) == 0)) k++;
                if (k < p->n) { free(nm); free(p->p[k].value); p->p[k].value = vv; p->p[k].vl = vl; }
                else params_push(p, nm, nl, vv, vl);
            }
            JS_FreePropertyEnum(ctx, tab, n);
        }
    } else if (!JS_IsUndefined(init)) {
        size_t n = 0;
        char *s = usv_dup(ctx, init, &n);
        if (!s) goto fail;
        size_t skip = n && s[0] == '?';
        params_parse(p, s + skip, n - skip);
        free(s);
    }
    return new_obj(ctx, ctor_proto(ctx, new_target, "URLSearchParams"), g_params_class, p);
fail:
    params_clear(p);
    free(p->p);
    free(p);
    return JS_EXCEPTION;
}

static GCValue js_params_size(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_PARAMS(p);
    return JS_NewInt32(ctx, p->n);
}

static GCValue js_params_append(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)magic;
    THIS_PARAMS(p);
    NEED(2, "append", "URLSearchParams");
    size_t nl = 0, vl = 0;
    char *n = usv_dup(ctx, argv[0], &nl);
    if (!n) return JS_EXCEPTION;
    char *v = usv_dup(ctx, argv[1], &vl);
    if (!v) { free(n); return JS_EXCEPTION; }
    params_push(p, n, nl, v, vl);
    params_update(ctx, this_val, p);
    return JS_UNDEFINED;
}

/* delete(name, value) (magic 0), has(name, value) (magic 1). */
static GCValue js_params_delete_has(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    THIS_PARAMS(p);
    NEED(1, magic ? "has" : "delete", "URLSearchParams");
    size_t nl = 0, vl = 0;
    char *n = usv_dup(ctx, argv[0], &nl), *v = NULL;
    if (!n) return JS_EXCEPTION;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !(v = usv_dup(ctx, argv[1], &vl))) { free(n); return JS_EXCEPTION; }
    bool found = false;
    int o = 0;
    for (int i = 0; i < p->n; i++) {
        bool hit = p->p[i].nl == nl && memcmp(p->p[i].name, n, nl) == 0 &&
                   (!v || (p->p[i].vl == vl && memcmp(p->p[i].value, v, vl) == 0));
        found |= hit;
        if (hit && !magic) { free(p->p[i].name); free(p->p[i].value); continue; }
        p->p[o++] = p->p[i];
    }
    p->n = o;
    free(n);
    free(v);
    if (magic) return JS_NewBool(ctx, found);
    params_update(ctx, this_val, p);
    return JS_UNDEFINED;
}

/* get(name) (magic 0), getAll(name) (magic 1). */
static GCValue js_params_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    THIS_PARAMS(p);
    NEED(1, magic ? "getAll" : "get", "URLSearchParams");
    size_t nl = 0;
    char *n = usv_dup(ctx, argv[0], &nl);
    if (!n) return JS_EXCEPTION;
    GCValue r = magic ? JS_NewArray(ctx) : JS_NULL;
    uint32_t k = 0;
    for (int i = 0; i < p->n; i++) {
        if (p->p[i].nl != nl || memcmp(p->p[i].name, n, nl)) continue;
        GCValue v = JS_NewStringLen(ctx, p->p[i].value, p->p[i].vl);
        if (!magic) { r = v; break; }
        JS_SetPropertyUint32(ctx, r, k++, v);
    }
    free(n);
    return r;
}

static GCValue js_params_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)magic;
    THIS_PARAMS(p);
    NEED(2, "set", "URLSearchParams");
    size_t nl = 0, vl = 0;
    char *n = usv_dup(ctx, argv[0], &nl);
    if (!n) return JS_EXCEPTION;
    char *v = usv_dup(ctx, argv[1], &vl);
    if (!v) { free(n); return JS_EXCEPTION; }
    int o = 0, first = -1;
    for (int i = 0; i < p->n; i++) {
        if (p->p[i].nl == nl && memcmp(p->p[i].name, n, nl) == 0) {
            if (first >= 0) { free(p->p[i].name); free(p->p[i].value); continue; }
            first = o;
        }
        p->p[o++] = p->p[i];
    }
    p->n = o;
    if (first >= 0) { free(n); free(p->p[first].value); p->p[first].value = v; p->p[first].vl = vl; }
    else params_push(p, n, nl, v, vl);
    params_update(ctx, this_val, p);
    return JS_UNDEFINED;
}

/* sort(): stable, by the names' UTF-16 code units.  UTF-8 byte order is
 * code point order, which differs from UTF-16 order only above U+FFFF
 * against U+E000..U+FFFF; compared as UTF-16 here. */
static int utf16_cmp(const char *a, size_t an, const char *b, size_t bn) {
    size_t i = 0, j = 0;
    while (i < an && j < bn) {
        uint32_t ca, cb;
        int la, lb;
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[j];
        la = x < 0x80 ? 1 : x < 0xE0 ? 2 : x < 0xF0 ? 3 : 4;
        lb = y < 0x80 ? 1 : y < 0xE0 ? 2 : y < 0xF0 ? 3 : 4;
        ca = la == 1 ? x : la == 2 ? x & 0x1F : la == 3 ? x & 0x0F : x & 0x07;
        cb = lb == 1 ? y : lb == 2 ? y & 0x1F : lb == 3 ? y & 0x0F : y & 0x07;
        for (int k = 1; k < la && i + (size_t)k < an; k++) ca = ca << 6 | ((unsigned char)a[i + (size_t)k] & 0x3F);
        for (int k = 1; k < lb && j + (size_t)k < bn; k++) cb = cb << 6 | ((unsigned char)b[j + (size_t)k] & 0x3F);
        /* the first UTF-16 unit of each: a supplementary character's high surrogate */
        uint32_t ua = ca > 0xFFFF ? 0xD800 + ((ca - 0x10000) >> 10) : ca;
        uint32_t ub = cb > 0xFFFF ? 0xD800 + ((cb - 0x10000) >> 10) : cb;
        if (ua != ub) return ua < ub ? -1 : 1;
        if (ca != cb) return ca < cb ? -1 : 1;
        i += (size_t)la;
        j += (size_t)lb;
    }
    return i < an ? 1 : j < bn ? -1 : 0;
}

static GCValue js_params_sort(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)magic;
    (void)argc; (void)argv;
    THIS_PARAMS(p);
    for (int i = 1; i < p->n; i++) {      /* insertion sort: stable */
        Pair x = p->p[i];
        int j = i;
        while (j > 0 && utf16_cmp(p->p[j - 1].name, p->p[j - 1].nl, x.name, x.nl) > 0) { p->p[j] = p->p[j - 1]; j--; }
        p->p[j] = x;
    }
    params_update(ctx, this_val, p);
    return JS_UNDEFINED;
}

static GCValue js_params_for_each(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)magic;
    THIS_PARAMS(p);
    NEED(1, "forEach", "URLSearchParams");
    if (!JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Failed to execute 'forEach' on 'URLSearchParams': The callback provided as parameter 1 is not a function.");
    for (int i = 0; i < p->n; i++) {      /* (the list may change under the callback) */
        GCValue args[3] = { JS_NewStringLen(ctx, p->p[i].value, p->p[i].vl),
                            JS_NewStringLen(ctx, p->p[i].name, p->p[i].nl), this_val };
        GCValue r = JS_Call(ctx, argv[0], argc > 1 ? argv[1] : JS_UNDEFINED, 3, args);
        if (JS_IsException(r)) return r;
    }
    return JS_UNDEFINED;
}

static GCValue js_params_to_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)magic;
    (void)argc; (void)argv;
    THIS_PARAMS(p);
    char *s = params_serialize(p);
    GCValue v = JS_NewString(ctx, s);
    free(s);
    return v;
}

/* entries() (magic 2), keys() (0), values() (1): iterators over the live list. */
static GCValue js_params_iter(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)argc; (void)argv;
    THIS_PARAMS(p);
    (void)p;
    ParamsIter *it = calloc(1, sizeof *it);
    it->kind = magic;
    GCValue proto = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__URLSearchParamsIteratorProto");
    GCValue obj = new_obj(ctx, proto, g_iter_class, it);
    if (!JS_IsException(obj)) JS_DefinePropertyValueStr(ctx, obj, "__p", this_val, 0);
    return obj;
}

static GCValue js_iter_next(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    ParamsIter *it = cell_of(ctx, this_val, g_iter_class);
    if (!it) return JS_ThrowTypeError(ctx, "Illegal invocation");
    Params *p = cell_of(ctx, JS_GetPropertyStr(ctx, this_val, "__p"), g_params_class);
    GCValue r = JS_NewObject(ctx);
    if (!p || it->index >= p->n) {
        JS_SetPropertyStr(ctx, r, "value", JS_UNDEFINED);
        JS_SetPropertyStr(ctx, r, "done", JS_TRUE);
        return r;
    }
    Pair *x = &p->p[it->index++];
    GCValue v;
    if (it->kind == 0) v = JS_NewStringLen(ctx, x->name, x->nl);
    else if (it->kind == 1) v = JS_NewStringLen(ctx, x->value, x->vl);
    else {
        v = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, v, 0, JS_NewStringLen(ctx, x->name, x->nl));
        JS_SetPropertyUint32(ctx, v, 1, JS_NewStringLen(ctx, x->value, x->vl));
    }
    JS_SetPropertyStr(ctx, r, "value", v);
    JS_SetPropertyStr(ctx, r, "done", JS_FALSE);
    return r;
}

/* ---- the document base URL and <a>/<area> ---- */

/* The document's URL (location.href). */
static char *document_url(JSContextHandle ctx) {
    GCValue loc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "location");
    GCValue h = JS_IsObject(loc) ? JS_GetPropertyStr(ctx, loc, "href") : JS_UNDEFINED;
    const char *s = JS_IsString(h) ? JS_ToCString(ctx, h) : NULL;
    char *r = strdup(s ? s : "about:blank");
    if (s) JS_FreeCString(ctx, s);
    return r;
}

/* The document base URL: the first <base href> parsed against the document
 * URL, else the document URL.  Cached while both strings stay the same. */
static struct { char *doc, *base; WUrl u; bool ok; } g_base;

static const WUrl *base_url(JSContextHandle ctx, NodeId el) {
    char *doc = document_url(ctx);
    const char *base = NULL;
    NodeId d = el ? dom_owner(el) : DOM_NULL;
    if (d) {
        DomNodeList l = dom_query_by_tag(d, "base", 4);
        for (uint32_t i = 0; i < l.n && !base; i++) base = dom_attr(l.ids[i], "href");
        dom_node_list_free(&l);
    }
    if (g_base.ok && !strcmp(g_base.doc, doc) && ((!base && !g_base.base) || (base && g_base.base && !strcmp(base, g_base.base)))) {
        free(doc);
        return &g_base.u;
    }
    if (g_base.ok) wurl_free(&g_base.u);
    free(g_base.doc);
    free(g_base.base);
    g_base.doc = doc;
    g_base.base = base ? strdup(base) : NULL;
    WUrl du;
    if (wurl_parse(doc, strlen(doc), NULL, &du) && wurl_parse("about:blank", 11, NULL, &du)) { g_base.ok = false; return NULL; }
    if (base && !wurl_parse(base, strlen(base), &du, &g_base.u)) wurl_free(&du);
    else g_base.u = du;
    g_base.ok = true;
    return &g_base.u;
}

/* The element's href attribute parsed (false when absent or unparsable). */
static bool link_url(JSContextHandle ctx, NodeId el, WUrl *out, const char **raw) {
    const char *h = el ? dom_attr(el, "href") : NULL;
    if (raw) *raw = h;
    if (!h) return false;
    const WUrl *b = base_url(ctx, el);
    return !wurl_parse(h, strlen(h), b, out);
}

static GCValue js_link_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)argc; (void)argv;
    NodeId el = dom_node_of(this_val);
    WUrl u;
    const char *raw = NULL;
    if (!link_url(ctx, el, &u, &raw)) {
        if (magic == P_HREF) return JS_NewString(ctx, raw ? raw : "");
        return JS_NewString(ctx, magic == P_PROTOCOL ? ":" : "");
    }
    GCValue v = url_part(ctx, &u, magic);
    wurl_free(&u);
    return v;
}

static GCValue js_link_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    NodeId el = dom_node_of(this_val);
    if (!el) return JS_ThrowTypeError(ctx, "Illegal invocation");
    size_t n = 0;
    char *v = usv_dup(ctx, argc ? argv[0] : JS_UNDEFINED, &n);
    if (!v) return JS_EXCEPTION;
    if (magic == P_HREF) {
        dom_element_set_attr(ctx, el, "href", v);
    } else {
        WUrl u;
        if (link_url(ctx, el, &u, NULL)) {
            wurl_set(&u, g_setter[magic], v, n);
            char *h = wurl_href(&u);
            if (h) dom_element_set_attr(ctx, el, "href", h);
            free(h);
            wurl_free(&u);
        }
    }
    free(v);
    return JS_UNDEFINED;
}

static GCValue js_base_uri(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (!n) n = g_dom_document;
    const WUrl *b = base_url(ctx, n);
    char *h = b ? wurl_href(b) : NULL;
    GCValue v = JS_NewString(ctx, h ? h : "about:blank");
    free(h);
    return v;
}

/* document.URL and documentURI: the document's URL. */
static GCValue js_document_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    char *u = document_url(ctx);
    GCValue v = JS_NewString(ctx, u);
    free(u);
    return v;
}

static GCValue js_ignore(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_UNDEFINED;
}

/* ---- installing ---- */

static void accessor(JSContextHandle ctx, GCValue obj, const char *name, JSCFunctionMagic *get, JSCFunctionMagic *set,
                     int magic) {
    char gn[48], sn[48];
    snprintf(gn, sizeof gn, "get %s", name);
    snprintf(sn, sizeof sn, "set %s", name);
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, obj, a, JS_NewCFunctionMagic(ctx, get, gn, 0, JS_CFUNC_generic_magic, magic),
                            set ? JS_NewCFunctionMagic(ctx, set, sn, 1, JS_CFUNC_generic_magic, magic) : JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

static void method(JSContextHandle ctx, GCValue obj, const char *name, JSCFunctionMagic *fn, int len, int magic) {
    JS_DefinePropertyValueStr(ctx, obj, name, JS_NewCFunctionMagic(ctx, fn, name, len, JS_CFUNC_generic_magic, magic),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}

static void define_global(JSContextHandle ctx, GCValue global, const char *name, GCValue v) {
    JS_DefinePropertyValueStr(ctx, global, name, v, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    if (JS_IsObject(win) && !JS_StrictEq(ctx, win, global))
        JS_DefinePropertyValueStr(ctx, win, name, v, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}

static void to_string_tag(JSContextHandle ctx, GCValue obj, const char *tag) {
    GCValue sym = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "Symbol");
    JSAtom a = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "toStringTag"));
    JS_DefinePropertyValue(ctx, obj, a, JS_NewString(ctx, tag), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, a);
}

void url_classes_install(JSContextHandle ctx, GCValue global) {
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    JSClassID *ids[3] = { &g_url_class, &g_params_class, &g_iter_class };
    JSClassDef *defs[3] = { &g_url_def, &g_params_def, &g_iter_def };
    for (int i = 0; i < 3; i++) {
        if (!*ids[i]) JS_NewClassID(ids[i]);
        if (!JS_IsRegisteredClass(rt, *ids[i])) JS_NewClass(rt, *ids[i], defs[i]);
    }

    /* URL */
    GCValue up = JS_NewObject(ctx);
    for (int k = P_HREF; k <= P_HASH; k++)
        accessor(ctx, up, g_part_names[k], js_url_get, k == P_ORIGIN ? NULL : js_url_set, k);
    JSAtom a = JS_NewAtom(ctx, "searchParams");
    JS_DefinePropertyGetSet(ctx, up, a, JS_NewCFunction(ctx, js_url_search_params, "get searchParams", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
    method(ctx, up, "toString", js_url_get, 0, P_HREF);
    method(ctx, up, "toJSON", js_url_get, 0, P_HREF);
    to_string_tag(ctx, up, "URL");
    GCValue uc = JS_NewCFunction2(ctx, js_url_ctor, "URL", 1, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, uc, up);
    method(ctx, uc, "canParse", js_url_static_parse, 1, 0);
    method(ctx, uc, "parse", js_url_static_parse, 1, 1);
    JS_DefinePropertyValueStr(ctx, uc, "createObjectURL", JS_NewCFunction(ctx, js_url_create_object_url, "createObjectURL", 1),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, uc, "revokeObjectURL", JS_NewCFunction(ctx, js_url_revoke_object_url, "revokeObjectURL", 1),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    define_global(ctx, global, "URL", uc);
    if (JS_IsUndefined(JS_GetPropertyStr(ctx, global, "webkitURL")))
        JS_DefinePropertyValueStr(ctx, global, "webkitURL", uc, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);

    /* URLSearchParams */
    GCValue pp = JS_NewObject(ctx);
    a = JS_NewAtom(ctx, "size");
    JS_DefinePropertyGetSet(ctx, pp, a, JS_NewCFunction(ctx, js_params_size, "get size", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
    method(ctx, pp, "append", js_params_append, 2, 0);
    method(ctx, pp, "delete", js_params_delete_has, 1, 0);
    method(ctx, pp, "get", js_params_get, 1, 0);
    method(ctx, pp, "getAll", js_params_get, 1, 1);
    method(ctx, pp, "has", js_params_delete_has, 1, 1);
    method(ctx, pp, "set", js_params_set, 2, 0);
    method(ctx, pp, "sort", js_params_sort, 0, 0);
    method(ctx, pp, "forEach", js_params_for_each, 1, 0);
    method(ctx, pp, "entries", js_params_iter, 0, 2);
    method(ctx, pp, "keys", js_params_iter, 0, 0);
    method(ctx, pp, "values", js_params_iter, 0, 1);
    method(ctx, pp, "toString", js_params_to_string, 0, 0);
    GCValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    a = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "iterator"));
    JS_DefinePropertyValue(ctx, pp, a, JS_GetPropertyStr(ctx, pp, "entries"), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, a);
    to_string_tag(ctx, pp, "URLSearchParams");
    GCValue pc = JS_NewCFunction2(ctx, js_params_ctor, "URLSearchParams", 0, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, pc, pp);
    define_global(ctx, global, "URLSearchParams", pc);

    /* URLSearchParams Iterator: inherits %IteratorPrototype% */
    static const char k_iter_proto[] = "Object.getPrototypeOf(Object.getPrototypeOf([][Symbol.iterator]()))";
    GCValue arr_iter = JS_Eval(ctx, k_iter_proto, strlen(k_iter_proto), "<url>", JS_EVAL_TYPE_GLOBAL);
    GCValue ip = JS_IsObject(arr_iter) ? JS_NewObjectProto(ctx, arr_iter) : JS_NewObject(ctx);
    JS_DefinePropertyValueStr(ctx, ip, "next", JS_NewCFunction(ctx, js_iter_next, "next", 0),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    to_string_tag(ctx, ip, "URLSearchParams Iterator");
    JS_DefinePropertyValueStr(ctx, global, "__URLSearchParamsIteratorProto", ip, 0);
}

/* <a>/<area>'s HTMLHyperlinkElementUtils and document.baseURI, once the
 * element interfaces exist (platform_api_install). */
void url_hyperlinks_install(JSContextHandle ctx, GCValue global) {
    static const char *const ifaces[] = { "HTMLAnchorElement", "HTMLAreaElement" };
    for (int i = 0; i < 2; i++) {
        GCValue p = global_proto(ctx, ifaces[i]);
        if (!JS_IsObject(p)) continue;
        for (int k = P_HREF; k <= P_HASH; k++)
            accessor(ctx, p, g_part_names[k], js_link_get, k == P_ORIGIN ? NULL : js_link_set, k);
        method(ctx, p, "toString", js_link_get, 0, P_HREF);
    }
    /* node.baseURI; the document's own baseURI (which embedder code
     * assigns) becomes the same accessor, with a setter that ignores it */
    GCValue targets[2] = { global_proto(ctx, "Node"), JS_GetPropertyStr(ctx, global, "document") };
    for (int i = 0; i < 2; i++) {
        if (!JS_IsObject(targets[i])) continue;
        JSAtom a = JS_NewAtom(ctx, "baseURI");
        JS_DefinePropertyGetSet(ctx, targets[i], a, JS_NewCFunction(ctx, js_base_uri, "get baseURI", 0),
                                JS_NewCFunction(ctx, js_ignore, "set baseURI", 1), JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
    /* the page's document: URL and documentURI (other documents keep their own) */
    GCValue doc = targets[1];
    static const char *const doc_urls[] = { "URL", "documentURI" };
    for (int i = 0; i < 2 && JS_IsObject(doc); i++) {
        JSAtom a = JS_NewAtom(ctx, doc_urls[i]);
        char gn[32];
        snprintf(gn, sizeof gn, "get %s", doc_urls[i]);
        JS_DefinePropertyGetSet(ctx, doc, a, JS_NewCFunction(ctx, js_document_url, gn, 0), JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
}
