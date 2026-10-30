/*
 * dom_token_list.c - DOMTokenList (DOM Standard 7.1): element.classList,
 * relList (a, area, link, form) and iframe.sandbox.
 *
 * A list is its element and attribute; its tokens are the attribute's value
 * parsed when used (an ordered set split on ASCII white space), read from
 * the node store.  Changes are written like setAttribute (mutation records,
 * style invalidation; not a page-patched setAttribute).  list[i] is an
 * exotic property, so it is live without keeping index properties in step.
 * Each element keeps one list per attribute; the methods are on
 * DOMTokenList.prototype.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "css_style.h"

typedef struct TokenList {
    NodeId el;
    const char *attr;                   /* "class", "rel", "sandbox" */
    CssAtom atom;                       /* its atom, for reading it */
    const char *const *supported;       /* NULL: supports() throws */
} TokenList;

/* The GC cell the object's opaque handle points at. */
typedef struct TokenCell { TokenList *t; } TokenCell;

static JSClassID js_dom_token_list_class_id;

static TokenList *list_of(JSContextHandle ctx, GCValue obj) {
    GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_dom_token_list_class_id);
    if (h == GC_HANDLE_NULL) return NULL;
    TokenCell *c = (TokenCell *)gc_deref(h);
    return c ? c->t : NULL;
}

/* ---- the token set ---- */

typedef struct Span { const char *p; size_t n; } Span;
typedef struct Tokens { Span *s; int n, cap; Span inl[16]; } Tokens;

static bool ascii_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r'; }

static void tokens_push(Tokens *t, const char *p, size_t n) {
    for (int i = 0; i < t->n; i++)
        if (t->s[i].n == n && memcmp(t->s[i].p, p, n) == 0) return;      /* (a set) */
    if (t->n == t->cap) {
        int cap = t->cap * 2;
        Span *s = malloc((size_t)cap * sizeof(Span));
        memcpy(s, t->s, (size_t)t->n * sizeof(Span));
        if (t->s != t->inl) free(t->s);
        t->s = s;
        t->cap = cap;
    }
    t->s[t->n].p = p;
    t->s[t->n++].n = n;
}

/* The attribute's tokens; the spans point into the attribute value, which
 * stays put until the attribute changes. */
static void tokens_parse(const TokenList *l, Tokens *t) {
    t->s = t->inl;
    t->n = 0;
    t->cap = 16;
    const char *v = l->el ? dom_get_attr(l->el, l->atom, NULL) : NULL;
    if (!v) return;
    for (const char *p = v; *p; ) {
        while (*p && ascii_ws(*p)) p++;
        const char *b = p;
        while (*p && !ascii_ws(*p)) p++;
        if (p > b) tokens_push(t, b, (size_t)(p - b));
    }
}

static void tokens_free(Tokens *t) { if (t->s != t->inl) free(t->s); }

static int tokens_find(const Tokens *t, const char *p, size_t n) {
    for (int i = 0; i < t->n; i++)
        if (t->s[i].n == n && memcmp(t->s[i].p, p, n) == 0) return i;
    return -1;
}

/* The update steps: the attribute becomes the serialized set (unless it is
 * absent and the set empty).  The spans may point into the old value, so
 * the new one is built first. */
static void tokens_update(JSContextHandle ctx, const TokenList *l, const Tokens *t) {
    if (!l->el || (!dom_get_attr(l->el, l->atom, NULL) && !t->n)) return;
    size_t len = 0;
    for (int i = 0; i < t->n; i++) len += t->s[i].n + 1;
    char *v = malloc(len + 1), *o = v;
    for (int i = 0; i < t->n; i++) {
        if (i) *o++ = ' ';
        memcpy(o, t->s[i].p, t->s[i].n);
        o += t->s[i].n;
    }
    *o = '\0';
    dom_element_set_attr(ctx, l->el, l->attr, v);
    free(v);
}

/* A token argument (String(arg)) checked for add/remove/toggle/replace:
 * empty is a SyntaxError, white space an InvalidCharacterError.  NULL with
 * an exception pending. */
static const char *token_arg(JSContextHandle ctx, GCValue arg, size_t *n) {
    const char *s = JS_ToCStringLen(ctx, n, arg);
    if (!s) return NULL;
    const char *err = !*n ? "SyntaxError" : NULL;
    for (size_t i = 0; !err && i < *n; i++) if (ascii_ws(s[i])) err = "InvalidCharacterError";
    if (err) {
        JS_FreeCString(ctx, s);
        throw_dom_exception(ctx, err, !strcmp(err, "SyntaxError") ? "The token must not be empty."
                                                                   : "The token must not contain white space.");
        return NULL;
    }
    return s;
}

#define THIS_LIST(l) \
    TokenList *l = list_of(ctx, this_val); \
    if (!l) return JS_ThrowTypeError(ctx, "Illegal invocation")

/* ---- DOMTokenList.prototype ---- */

static GCValue js_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_LIST(l);
    Tokens t;
    tokens_parse(l, &t);
    int n = t.n;
    tokens_free(&t);
    return JS_NewInt32(ctx, n);
}

static GCValue js_get_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_LIST(l);
    const char *v = l->el ? dom_get_attr(l->el, l->atom, NULL) : NULL;
    return JS_NewString(ctx, v ? v : "");
}

static GCValue js_set_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_LIST(l);
    const char *v = JS_ToCString(ctx, argc ? argv[0] : JS_UNDEFINED);
    if (!v) return JS_EXCEPTION;
    if (l->el) dom_element_set_attr(ctx, l->el, l->attr, v);
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}

static GCValue js_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_LIST(l);
    uint32_t i = 0;
    if (JS_ToUint32(ctx, &i, argc ? argv[0] : JS_UNDEFINED)) return JS_EXCEPTION;
    Tokens t;
    tokens_parse(l, &t);
    GCValue r = i < (uint32_t)t.n ? JS_NewStringLen(ctx, t.s[i].p, t.s[i].n) : JS_NULL;
    tokens_free(&t);
    return r;
}

static GCValue js_contains(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_LIST(l);
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, argc ? argv[0] : JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    Tokens t;
    tokens_parse(l, &t);
    bool r = tokens_find(&t, s, n) >= 0;
    tokens_free(&t);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

/* add(...tokens) and remove(...tokens): every token is checked first. */
static GCValue add_remove(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, bool add) {
    THIS_LIST(l);
    const char *args[16], **a = argc <= 16 ? args : malloc((size_t)argc * sizeof(char *));
    size_t lens[16], *ln = argc <= 16 ? lens : malloc((size_t)argc * sizeof(size_t));
    int got = 0;
    for (; got < argc; got++)
        if (!(a[got] = token_arg(ctx, argv[got], &ln[got]))) break;
    GCValue r = JS_UNDEFINED;
    if (got < argc) r = JS_EXCEPTION;
    else {
        Tokens t;
        tokens_parse(l, &t);
        for (int i = 0; i < argc; i++) {
            int at = tokens_find(&t, a[i], ln[i]);
            if (add && at < 0) tokens_push(&t, a[i], ln[i]);
            else if (!add && at >= 0) { memmove(&t.s[at], &t.s[at + 1], (size_t)(t.n - at - 1) * sizeof(Span)); t.n--; }
        }
        tokens_update(ctx, l, &t);
        tokens_free(&t);
    }
    for (int i = 0; i < got; i++) JS_FreeCString(ctx, a[i]);
    if (a != args) free((void *)a);
    if (ln != lens) free(ln);
    return r;
}

static GCValue js_add(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return add_remove(ctx, this_val, argc, argv, true);
}

static GCValue js_remove(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return add_remove(ctx, this_val, argc, argv, false);
}

static GCValue js_toggle(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_LIST(l);
    size_t n = 0;
    const char *s = token_arg(ctx, argc ? argv[0] : JS_UNDEFINED, &n);
    if (!s) return JS_EXCEPTION;
    int force = argc > 1 && !JS_IsUndefined(argv[1]) ? JS_ToBool(ctx, argv[1]) : -1;
    Tokens t;
    tokens_parse(l, &t);
    int at = tokens_find(&t, s, n);
    bool r;
    if (at >= 0) {
        r = force == 1;
        if (!r) { memmove(&t.s[at], &t.s[at + 1], (size_t)(t.n - at - 1) * sizeof(Span)); t.n--; tokens_update(ctx, l, &t); }
    } else {
        r = force != 0;
        if (r) { tokens_push(&t, s, n); tokens_update(ctx, l, &t); }
    }
    tokens_free(&t);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

static GCValue js_replace(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_LIST(l);
    size_t n = 0, nn = 0;
    const char *s = JS_ToCStringLen(ctx, &n, argc ? argv[0] : JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    const char *ns = JS_ToCStringLen(ctx, &nn, argc > 1 ? argv[1] : JS_UNDEFINED);
    if (!ns) { JS_FreeCString(ctx, s); return JS_EXCEPTION; }
    /* both checked for empty, then both for white space */
    const char *err = !n || !nn ? "SyntaxError" : NULL;
    for (size_t i = 0; !err && i < n; i++) if (ascii_ws(s[i])) err = "InvalidCharacterError";
    for (size_t i = 0; !err && i < nn; i++) if (ascii_ws(ns[i])) err = "InvalidCharacterError";
    if (err) {
        JS_FreeCString(ctx, s);
        JS_FreeCString(ctx, ns);
        return throw_dom_exception(ctx, err, !strcmp(err, "SyntaxError") ? "The token must not be empty."
                                                                         : "The token must not contain white space.");
    }
    Tokens t;
    tokens_parse(l, &t);
    int at = tokens_find(&t, s, n);
    if (at >= 0) {
        /* the first of token and newToken becomes newToken; the other goes */
        int other = tokens_find(&t, ns, nn), first = other >= 0 && other < at ? other : at;
        int gone = first == at ? other : at;
        t.s[first].p = ns;
        t.s[first].n = nn;
        if (gone >= 0 && gone != first) { memmove(&t.s[gone], &t.s[gone + 1], (size_t)(t.n - gone - 1) * sizeof(Span)); t.n--; }
        tokens_update(ctx, l, &t);
    }
    tokens_free(&t);
    JS_FreeCString(ctx, s);
    JS_FreeCString(ctx, ns);
    return JS_NewBool(ctx, at >= 0);
}

static GCValue js_supports(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_LIST(l);
    if (!l->supported) return JS_ThrowTypeError(ctx, "'%s' has no supported tokens", l->attr);
    const char *s = JS_ToCString(ctx, argc ? argv[0] : JS_UNDEFINED);
    if (!s) return JS_EXCEPTION;
    bool r = false;
    for (int i = 0; l->supported[i] && !r; i++) r = strcasecmp(l->supported[i], s) == 0;
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, r);
}

/* list[i]: the i-th token, live.  (An index atom reads back as its
 * canonical string: digits, no leading zero, below 2^32 - 1.) */
static bool atom_index(JSContextHandle ctx, JSAtom prop, uint32_t *i) {
    const char *s = JS_AtomToCString(ctx, prop);
    if (!s) return false;
    bool ok = s[0] >= '0' && s[0] <= '9' && (s[0] != '0' || !s[1]);
    uint64_t v = 0;
    for (const char *p = s; ok && *p; p++) {
        ok = *p >= '0' && *p <= '9';
        v = v * 10 + (uint64_t)(*p - '0');
        if (v >= 0xffffffffu) ok = false;
    }
    JS_FreeCString(ctx, s);
    if (ok) *i = (uint32_t)v;
    return ok;
}

static int js_get_own(JSContextHandle ctx, JSPropertyDescriptor *desc, GCValue obj, JSAtom prop) {
    uint32_t i;
    TokenList *l = list_of(ctx, obj);
    if (!l || !atom_index(ctx, prop, &i)) return 0;
    Tokens t;
    tokens_parse(l, &t);
    bool in = i < (uint32_t)t.n;
    if (in && desc) {
        desc->flags = JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE;
        desc->value = JS_NewStringLen(ctx, t.s[i].p, t.s[i].n);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    tokens_free(&t);
    return in;
}

static JSClassExoticMethods js_dom_token_list_exotic = {
    .get_own_property = js_get_own,
};

static void js_dom_token_list_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    GCHandle h = JS_GetOpaqueHandle(val, js_dom_token_list_class_id);
    if (h == GC_HANDLE_NULL) return;
    TokenCell *c = (TokenCell *)gc_deref(h);
    if (c) { free(c->t); c->t = NULL; }
}

static void js_dom_token_list_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle h = JS_GetOpaqueHandle(val, js_dom_token_list_class_id);
    if (h != GC_HANDLE_NULL) mark_func(rt, h);
}

static JSClassDef js_dom_token_list_class_def = {
    .class_name = "DOMTokenList",
    .finalizer = js_dom_token_list_finalizer,
    .gc_mark = js_dom_token_list_mark,
    .exotic = &js_dom_token_list_exotic,
};

static const JSCFunctionListEntry g_methods[] = {
    JS_CFUNC_DEF("item", 1, js_item),
    JS_CFUNC_DEF("contains", 1, js_contains),
    JS_CFUNC_DEF("add", 0, js_add),
    JS_CFUNC_DEF("remove", 0, js_remove),
    JS_CFUNC_DEF("toggle", 1, js_toggle),
    JS_CFUNC_DEF("replace", 2, js_replace),
    JS_CFUNC_DEF("supports", 1, js_supports),
    JS_CFUNC_DEF("toString", 0, js_get_value),
};

/* ---- the elements' lists ---- */

static const char *const REL_LINK[] = { "alternate", "dns-prefetch", "expect", "icon", "manifest", "modulepreload",
                                        "next", "pingback", "preconnect", "prefetch", "preload", "search",
                                        "stylesheet", NULL };
static const char *const REL_NAV[] = { "noreferrer", "noopener", "opener", NULL };
static const char *const SANDBOX[] = {
    "allow-downloads", "allow-forms", "allow-modals", "allow-orientation-lock", "allow-pointer-lock",
    "allow-popups", "allow-popups-to-escape-sandbox", "allow-presentation", "allow-same-origin",
    "allow-scripts", "allow-top-navigation", "allow-top-navigation-by-user-activation",
    "allow-top-navigation-to-custom-protocols", "allow-storage-access-by-user-activation", NULL };

static const struct { const char *iface, *prop, *attr; const char *const *supported; } g_lists[] = {
    { "Element", "classList", "class", NULL },
    { "HTMLLinkElement", "relList", "rel", REL_LINK },
    { "HTMLAnchorElement", "relList", "rel", REL_NAV },
    { "HTMLAreaElement", "relList", "rel", REL_NAV },
    { "HTMLFormElement", "relList", "rel", REL_NAV },
    { "HTMLIFrameElement", "sandbox", "sandbox", SANDBOX },
};

/* The element's list for g_lists[magic], made once and kept on it under a
 * hidden property. */
static GCValue js_list_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)argc; (void)argv;
    NodeId el = dom_node_of(this_val);
    if (!el) return JS_ThrowTypeError(ctx, "Illegal invocation");
    static JSAtom keys[sizeof g_lists / sizeof g_lists[0]];
    if (!keys[magic]) {
        char key[32];
        snprintf(key, sizeof key, "__tokenList_%s", g_lists[magic].attr);
        keys[magic] = JS_NewAtom(ctx, key);
    }
    GCValue existing = JS_GetProperty(ctx, this_val, keys[magic]);
    if (JS_IsObject(existing)) return existing;
    GCValue proto = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "DOMTokenList");
    proto = JS_IsObject(proto) ? JS_GetPropertyStr(ctx, proto, "prototype") : JS_UNDEFINED;
    GCValue obj = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, js_dom_token_list_class_id)
                                     : JS_NewObjectClass(ctx, js_dom_token_list_class_id);
    if (JS_IsException(obj)) return obj;
    GCHandle h = gc_allocz(sizeof(TokenCell), JS_GC_OBJ_TYPE_DATA);
    TokenList *l = calloc(1, sizeof *l);
    if (h == GC_HANDLE_NULL || !l) { free(l); return JS_ThrowOutOfMemory(ctx); }
    l->el = el;
    l->attr = g_lists[magic].attr;
    css_atom_reserve(1);
    l->atom = css_atom_intern(l->attr, strlen(l->attr));
    l->supported = g_lists[magic].supported;
    ((TokenCell *)gc_deref(h))->t = l;
    JS_SetOpaqueHandle(obj, h);
    JS_DefinePropertyValue(ctx, this_val, keys[magic], obj, 0);     /* (not enumerable) */
    return obj;
}

/* list = v: [PutForwards=value]. */
static GCValue js_list_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    NodeId el = dom_node_of(this_val);
    if (!el) return JS_ThrowTypeError(ctx, "Illegal invocation");
    const char *v = JS_ToCString(ctx, argc ? argv[0] : JS_UNDEFINED);
    if (!v) return JS_EXCEPTION;
    dom_element_set_attr(ctx, el, g_lists[magic].attr, v);
    JS_FreeCString(ctx, v);
    return JS_UNDEFINED;
}

static GCValue js_illegal(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* The DOMTokenList interface, and the lists' accessors on the element
 * interfaces that exist now (run after platform_api.js defines them). */
void dom_token_list_install(JSContextHandle ctx, GCValue global, GCValue window) {
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    if (!js_dom_token_list_class_id) JS_NewClassID(&js_dom_token_list_class_id);
    if (!JS_IsRegisteredClass(rt, js_dom_token_list_class_id))
        JS_NewClass(rt, js_dom_token_list_class_id, &js_dom_token_list_class_def);
    GCValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, g_methods, (int)(sizeof g_methods / sizeof g_methods[0]));
    JSAtom a = JS_NewAtom(ctx, "length");
    JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction(ctx, js_length, "get length", 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
    a = JS_NewAtom(ctx, "value");
    JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction(ctx, js_get_value, "get value", 0),
                            JS_NewCFunction(ctx, js_set_value, "set value", 1), JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
    GCValue sym = JS_Eval(ctx, "Symbol", 6, "<symbol>", JS_EVAL_TYPE_GLOBAL);
    GCValue iter = JS_GetPropertyStr(ctx, sym, "iterator"), tag = JS_GetPropertyStr(ctx, sym, "toStringTag");
    JSAtom ia = JS_ValueToAtom(ctx, iter), ta = JS_ValueToAtom(ctx, tag);
    /* iterable with an indexed getter (WebIDL 3.7.9): Array.prototype's
     * keys, values, entries and forEach, over length and list[i] */
    GCValue ap = JS_GetPropertyStr(ctx, JS_GetPropertyStr(ctx, global, "Array"), "prototype");
    static const char *const from_array[] = { "keys", "values", "entries", "forEach" };
    for (size_t i = 0; i < 4; i++)
        JS_DefinePropertyValueStr(ctx, proto, from_array[i], JS_GetPropertyStr(ctx, ap, from_array[i]),
                                  JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_DefinePropertyValue(ctx, proto, ia, JS_GetPropertyStr(ctx, ap, "values"), JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValue(ctx, proto, ta, JS_NewString(ctx, "DOMTokenList"), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, ia);
    JS_FreeAtom(ctx, ta);
    GCValue ctor = JS_NewCFunction2(ctx, js_illegal, "DOMTokenList", 0, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, ctor, proto);
    JS_DefinePropertyValueStr(ctx, global, "DOMTokenList", ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    if (JS_IsObject(window) && !JS_StrictEq(ctx, window, global))
        JS_DefinePropertyValueStr(ctx, window, "DOMTokenList", ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    for (size_t i = 0; i < sizeof g_lists / sizeof g_lists[0]; i++) {
        GCValue c = JS_GetPropertyStr(ctx, global, g_lists[i].iface);
        GCValue p = JS_IsObject(c) ? JS_GetPropertyStr(ctx, c, "prototype") : JS_UNDEFINED;
        if (!JS_IsObject(p)) continue;
        a = JS_NewAtom(ctx, g_lists[i].prop);
        char gn[48], sn[48];
        snprintf(gn, sizeof gn, "get %s", g_lists[i].prop);
        snprintf(sn, sizeof sn, "set %s", g_lists[i].prop);
        JS_DefinePropertyGetSet(ctx, p, a, JS_NewCFunctionMagic(ctx, js_list_get, gn, 0, JS_CFUNC_generic_magic, (int)i),
                                JS_NewCFunctionMagic(ctx, js_list_set, sn, 1, JS_CFUNC_generic_magic, (int)i),
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
}
