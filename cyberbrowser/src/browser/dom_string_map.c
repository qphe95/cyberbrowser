/*
 * dom_string_map.c - element.dataset, a DOMStringMap (HTML 3.2.6.6): its
 * properties are the element's data-* attributes, by their names in camel
 * case (data-date-of-birth is dateOfBirth).
 *
 * A map is an object of its class over its element (the NodeId in its
 * opaque cell); its properties are exotic, read from and written to the
 * element's attributes through setAttribute and removeAttribute.  Each
 * element keeps its one map (a hidden __cyber_dataset).
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "dom_js.h"
#include "dom_store.h"

typedef struct MapCell { NodeId el; } MapCell;

static JSClassID g_map_class;

static NodeId map_element(JSContextHandle ctx, GCValue obj) {
    GCHandle h = JS_IsObject(obj) ? JS_GetOpaqueHandle2(ctx, obj, g_map_class) : GC_HANDLE_NULL;
    MapCell *c = h != GC_HANDLE_NULL ? (MapCell *)gc_deref(h) : NULL;
    return c && dom_valid(c->el) ? c->el : DOM_NULL;
}

static void map_mark(JSRuntimeHandle rt, GCValue v, JS_MarkFunc *mark) {
    GCHandle h = JS_GetOpaqueHandle(v, g_map_class);
    if (h != GC_HANDLE_NULL) mark(rt, h);
}

/* ---- names ---- */

static bool lower(char c) { return c >= 'a' && c <= 'z'; }
static bool upper(char c) { return c >= 'A' && c <= 'Z'; }

/* An attribute name's property name (malloc'd), or NULL when it is no
 * data-* attribute a property shows (one with an upper-case letter). */
static char *prop_of_attr(const char *an) {
    if (strncmp(an, "data-", 5) != 0) return NULL;
    const char *rest = an + 5;
    for (const char *p = rest; *p; p++) if (upper(*p)) return NULL;
    size_t n = strlen(rest);
    char *out = malloc(n + 1), *o = out;
    for (size_t i = 0; i < n; i++) {
        if (rest[i] == '-' && i + 1 < n && lower(rest[i + 1])) {
            *o++ = (char)(rest[i + 1] - 32);
            i++;
        } else {
            *o++ = rest[i];
        }
    }
    *o = 0;
    return out;
}

/* A property name's attribute name (malloc'd): data- and each upper-case
 * letter as '-' and its lower case. */
static char *attr_of_prop(const char *p) {
    size_t n = strlen(p), k = 0;
    char *out = malloc(5 + 2 * n + 1);
    memcpy(out, "data-", 5);
    k = 5;
    for (size_t i = 0; i < n; i++) {
        if (upper(p[i])) { out[k++] = '-'; out[k++] = (char)(p[i] + 32); }
        else out[k++] = p[i];
    }
    out[k] = 0;
    return out;
}

/* The index of the attribute a property name names, or -1. */
static int find(NodeId el, const char *prop) {
    int count = dom_attr_count(el);
    for (int i = 0; i < count; i++) {
        const DomAttr *a = dom_attr_at(el, i);
        if (a->ns) continue;
        char *pn = prop_of_attr(dom_attr_qualified_name(a));
        bool eq = pn && !strcmp(pn, prop);
        free(pn);
        if (eq) return i;
    }
    return -1;
}

static const char *prop_cstr(JSContextHandle ctx, JSAtom prop) {
    GCValue key = JS_AtomToValue(ctx, prop);
    return JS_IsString(key) ? JS_ToCString(ctx, key) : NULL;
}

/* ---- the exotic methods ([LegacyOverrideBuiltIns]: the names win over
 * the prototype's) ---- */

static int js_map_get_own(JSContextHandle ctx, JSPropertyDescriptor *desc, GCValue obj, JSAtom prop) {
    NodeId el = map_element(ctx, obj);
    if (!el) return 0;
    const char *p = prop_cstr(ctx, prop);
    if (!p) return 0;
    int i = find(el, p);
    JS_FreeCString(ctx, p);
    if (i < 0) return 0;
    if (desc) {
        const DomAttr *a = dom_attr_at(el, i);
        desc->flags = JS_PROP_C_W_E;
        desc->value = JS_NewStringLen(ctx, a->value.s ? a->value.s : "", a->value.len);
        desc->getter = JS_UNDEFINED;
        desc->setter = JS_UNDEFINED;
    }
    return 1;
}

static int js_map_own_names(JSContextHandle ctx, JSPropertyEnum **ptab, uint32_t *plen, GCValue obj) {
    NodeId el = map_element(ctx, obj);
    int count = el ? dom_attr_count(el) : 0;
    GCHandle h = gc_allocz(sizeof(JSPropertyEnum) * (size_t)(count + 1), JS_GC_OBJ_TYPE_DATA);
    JSPropertyEnum *tab = h != GC_HANDLE_NULL ? (JSPropertyEnum *)gc_deref(h) : NULL;
    if (!tab) return -1;
    uint32_t k = 0;
    for (int i = 0; i < count; i++) {
        const DomAttr *a = dom_attr_at(el, i);
        char *pn = a->ns ? NULL : prop_of_attr(dom_attr_qualified_name(a));
        if (pn) tab[k++].atom = JS_NewAtom(ctx, pn);
        free(pn);
    }
    *ptab = tab;
    *plen = k;
    return 0;
}

/* (the named setter: a property name with '-' and a lower-case letter is a
 * SyntaxError; an attribute name that isn't one, InvalidCharacterError) */
static int js_map_define(JSContextHandle ctx, GCValue obj, JSAtom prop, GCValue val, GCValue getter, GCValue setter,
                         int flags) {
    NodeId el = map_element(ctx, obj);
    const char *p = el ? prop_cstr(ctx, prop) : NULL;
    if (!p) return JS_DefineProperty(ctx, obj, prop, val, getter, setter, flags | JS_PROP_NO_EXOTIC);
    if ((flags & (JS_PROP_HAS_GET | JS_PROP_HAS_SET)) || !(flags & JS_PROP_HAS_VALUE)) {
        JS_FreeCString(ctx, p);
        return JS_RefuseProperty(ctx, flags, "A DOMStringMap's properties are data properties.");
    }
    for (const char *q = p; *q; q++)
        if (q[0] == '-' && lower(q[1])) {
            JS_FreeCString(ctx, p);
            throw_dom_exception(ctx, "SyntaxError", "'-' followed by a lower-case letter is not allowed in a dataset name.");
            return -1;
        }
    char *an = attr_of_prop(p);
    JS_FreeCString(ctx, p);
    GCValue args[2] = { JS_NewString(ctx, an), val };
    free(an);
    GCValue r = js_element_set_attribute(ctx, dom_js(ctx, el), 2, args);
    return JS_IsException(r) ? -1 : 1;
}

static int js_map_delete(JSContextHandle ctx, GCValue obj, JSAtom prop) {
    NodeId el = map_element(ctx, obj);
    const char *p = el ? prop_cstr(ctx, prop) : NULL;
    if (!p) return 1;
    bool have = find(el, p) >= 0;
    char *an = have ? attr_of_prop(p) : NULL;
    JS_FreeCString(ctx, p);
    if (!an) return 1;
    GCValue arg = JS_NewString(ctx, an);
    free(an);
    GCValue r = js_element_remove_attribute(ctx, dom_js(ctx, el), 1, &arg);
    return JS_IsException(r) ? -1 : 1;
}

static JSClassExoticMethods g_map_exotic = {
    .get_own_property = js_map_get_own,
    .get_own_property_names = js_map_own_names,
    .delete_property = js_map_delete,
    .define_own_property = js_map_define,
};

static JSClassDef g_map_def = {
    .class_name = "DOMStringMap",
    .gc_mark = map_mark,
    .exotic = &g_map_exotic,
};

/* element.dataset ([SameObject]) */
GCValue js_element_get_dataset(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId el = dom_node_of(this_val);
    if (dom_kind(el) != DOM_K_ELEMENT) return JS_ThrowTypeError(ctx, "Illegal invocation");
    GCValue have = JS_GetPropertyStr(ctx, this_val, "__cyber_dataset");
    if (JS_IsObject(have)) return have;
    GCValue ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "DOMStringMap");
    GCValue proto = JS_IsObject(ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
    GCValue map = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, g_map_class) : JS_NewObjectClass(ctx, g_map_class);
    GCHandle h = JS_IsException(map) ? GC_HANDLE_NULL : gc_allocz(sizeof(MapCell), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) return JS_IsException(map) ? map : JS_ThrowOutOfMemory(ctx);
    ((MapCell *)gc_deref(h))->el = el;
    JS_SetOpaqueHandle(map, h);
    JS_DefinePropertyValueStr(ctx, this_val, "__cyber_dataset", map, JS_PROP_CONFIGURABLE);
    return map;
}

static GCValue js_illegal(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

/* The DOMStringMap interface on the global. */
void dom_string_map_install(JSContextHandle ctx, GCValue global) {
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    if (!g_map_class) JS_NewClassID(&g_map_class);
    if (!JS_IsRegisteredClass(rt, g_map_class)) JS_NewClass(rt, g_map_class, &g_map_def);
    GCValue proto = JS_NewObject(ctx);
    GCValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSAtom tag = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "toStringTag"));
    JS_DefinePropertyValue(ctx, proto, tag, JS_NewString(ctx, "DOMStringMap"), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tag);
    GCValue c = JS_NewCFunction2(ctx, js_illegal, "DOMStringMap", 0, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, c, proto);
    JS_DefinePropertyValueStr(ctx, global, "DOMStringMap", c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
}
