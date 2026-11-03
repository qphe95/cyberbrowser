/*
 * idl_shape.c - the DOM's interfaces given the shapes WebIDL says they have
 * (WebIDL 3.7), over what the other files installed: one pass per realm,
 * driven by the table tools/gen_idl_table.js makes from WPT's IDL files
 * (idl_table.inc).
 *
 * For each interface the realm has: its interface object is not enumerable
 * on the global, its prototype property is fixed and its constructor
 * property not enumerable, its constructor's length is the IDL's; its
 * constants are read-only on both; its operations and attributes on the
 * prototype are enumerable, with their IDL lengths and getter names; and a
 * native operation or accessor of a node interface checks its this and its
 * argument count when called (JS_SetCFunctionChecks; the engine throws).
 * What the table names that the realm does not have is left out.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <quickjs.h>
#include "browser_api_impl_internal.h"
#include "dom_store.h"
#include "dom_wrap.h"

enum { IDL_OP, IDL_ATTR, IDL_RO_ATTR, IDL_CONST, IDL_STATIC_OP, IDL_STATIC_ATTR };

typedef struct IdlMember {
    const char *name;
    uint8_t kind;
    uint8_t required;           /* operations: the arguments they need */
    const char *value;          /* constants */
    uint8_t unscopable;         /* [Unscopable]: in the prototype's @@unscopables */
} IdlMember;

typedef struct IdlIface {
    const char *name, *parent;
    uint16_t first, count;      /* its members: k_idl_members[first ..] */
    int8_t ctor;                /* its constructor's length (-1: none) */
    uint8_t flags;              /* 1 callback interface, 2 no interface object, 4 exposed in workers */
} IdlIface;

#include "idl_table.inc"

#define NIFACE ((int)(sizeof k_idl_ifaces / sizeof k_idl_ifaces[0]))

/* ---- Brands: what a this must be (the node interfaces; 0 for the rest) ---- */

enum { B_NONE, B_NODE, B_ELEMENT, B_DOCUMENT, B_XMLDOCUMENT, B_FRAGMENT, B_SHADOW, B_CHARDATA, B_TEXT, B_CDATA,
       B_COMMENT, B_PI, B_DOCTYPE, B_ATTR };

static const struct { const char *name; int brand; } k_brands[] = {
    { "Node", B_NODE }, { "Element", B_ELEMENT }, { "Document", B_DOCUMENT }, { "XMLDocument", B_XMLDOCUMENT },
    { "DocumentFragment", B_FRAGMENT }, { "ShadowRoot", B_SHADOW }, { "CharacterData", B_CHARDATA },
    { "Text", B_TEXT }, { "CDATASection", B_CDATA }, { "Comment", B_COMMENT },
    { "ProcessingInstruction", B_PI }, { "DocumentType", B_DOCTYPE }, { "Attr", B_ATTR },
};

static int brand_of(const char *iface) {
    for (size_t i = 0; i < sizeof k_brands / sizeof k_brands[0]; i++)
        if (!strcmp(k_brands[i].name, iface)) return k_brands[i].brand;
    return B_NONE;
}

/* An Attr: an object with its own hidden fields (__a_name; dom_api.c) over
 * no store node. */
static bool is_attr(JSContextHandle ctx, GCValue v) {
    if (!JS_IsObject(v) || dom_node_of(v)) return false;
    JSAtom a = JS_NewAtom(ctx, "__a_name");
    int own = JS_GetOwnProperty(ctx, NULL, v, a);
    JS_FreeAtom(ctx, a);
    return own > 0;
}

static int brand_check(JSContextHandle ctx, GCValue v, int brand) {
    NodeId n = JS_IsObject(v) ? dom_node_of(v) : DOM_NULL;
    int k = dom_kind(n);
    switch (brand) {
    case B_NODE: return n || is_attr(ctx, v);
    case B_ELEMENT: return k == DOM_K_ELEMENT;
    case B_DOCUMENT: return k == DOM_K_DOCUMENT;
    case B_XMLDOCUMENT: return k == DOM_K_DOCUMENT && !dom_is_html_document(n);
    case B_FRAGMENT: return k == DOM_K_FRAGMENT;
    case B_SHADOW: return k == DOM_K_FRAGMENT && (dom_flags(n) & DOMF_SHADOW_ROOT);
    case B_CHARDATA: return dom_kind_is_chardata(k) && k != DOM_K_DOCTYPE;
    case B_TEXT: return k == DOM_K_TEXT || k == DOM_K_CDATA;
    case B_CDATA: return k == DOM_K_CDATA;
    case B_COMMENT: return k == DOM_K_COMMENT;
    case B_PI: return k == DOM_K_PI;
    case B_DOCTYPE: return k == DOM_K_DOCTYPE;
    case B_ATTR: return is_attr(ctx, v);
    default: return 1;
    }
}

/* ---- The pass ---- */

/* The functions given checks so far (a function on two prototypes, the
 * ChildNode methods say, gets the brand they share: none). */
typedef struct Checked { GCValue fn; int brand; } Checked;

static void set_checks(JSContextHandle ctx, Checked **seen, int *n, int *cap, GCValue fn, int required, int brand) {
    for (int i = 0; i < *n; i++) {
        if (!JS_StrictEq(ctx, (*seen)[i].fn, fn)) continue;
        if ((*seen)[i].brand != brand) {
            /* (a node interface and another: Node's, else none) */
            int b = (*seen)[i].brand != B_NONE && brand != B_NONE ? B_NODE : B_NONE;
            (*seen)[i].brand = b;
            JS_SetCFunctionChecks(fn, required, b);
        }
        return;
    }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        *seen = (Checked *)realloc(*seen, (size_t)*cap * sizeof(Checked));
    }
    (*seen)[*n].fn = fn;
    (*seen)[*n].brand = brand;
    (*n)++;
    JS_SetCFunctionChecks(fn, required, brand);
}

static void redefine_value(JSContextHandle ctx, GCValue obj, const char *name, GCValue v, int flags) {
    JS_DefinePropertyValueStr(ctx, obj, name, v, flags);
}

/* The member's property on obj, own: made what WebIDL says. */
static void shape_member(JSContextHandle ctx, GCValue obj, const IdlMember *m, int brand, Checked **seen, int *n,
                         int *cap) {
    JSAtom a = JS_NewAtom(ctx, m->name);
    JSPropertyDescriptor d;
    int has = JS_GetOwnProperty(ctx, &d, obj, a);
    if (has <= 0) {
        JS_FreeAtom(ctx, a);
        return;
    }
    if (m->kind == IDL_OP || m->kind == IDL_STATIC_OP) {
        if (!(d.flags & JS_PROP_GETSET) && JS_IsFunction(ctx, d.value)) {
            JS_DefinePropertyValue(ctx, obj, a, d.value, JS_PROP_WRITABLE | JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
            JS_DefinePropertyValueStr(ctx, d.value, "length", JS_NewInt32(ctx, m->required), JS_PROP_CONFIGURABLE);
            /* (an anonymous function: the operation's name) */
            size_t fl = 0;
            GCValue fname = JS_GetPropertyStr(ctx, d.value, "name");
            const char *fs = JS_IsString(fname) ? JS_ToCStringLen(ctx, &fl, fname) : NULL;
            if (fs) JS_FreeCString(ctx, fs);
            if (!fl) JS_DefinePropertyValueStr(ctx, d.value, "name", JS_NewString(ctx, m->name), JS_PROP_CONFIGURABLE);
            /* (a promise-returning operation rejects rather than throws: its own check) */
            bool promise = !strcmp(m->name, "exitFullscreen") || !strcmp(m->name, "requestFullscreen");
            set_checks(ctx, seen, n, cap, d.value, m->required, m->kind == IDL_OP && !promise ? brand : B_NONE);
        }
    } else if (d.flags & JS_PROP_GETSET) {
        char gname[96];
        snprintf(gname, sizeof gname, "get %s", m->name);
        if (JS_IsFunction(ctx, d.getter)) {
            JS_DefinePropertyValueStr(ctx, d.getter, "name", JS_NewString(ctx, gname), JS_PROP_CONFIGURABLE);
            JS_DefinePropertyValueStr(ctx, d.getter, "length", JS_NewInt32(ctx, 0), JS_PROP_CONFIGURABLE);
            if (m->kind != IDL_STATIC_ATTR) set_checks(ctx, seen, n, cap, d.getter, 0, brand);
        }
        if (JS_IsFunction(ctx, d.setter)) {
            snprintf(gname, sizeof gname, "set %s", m->name);
            JS_DefinePropertyValueStr(ctx, d.setter, "name", JS_NewString(ctx, gname), JS_PROP_CONFIGURABLE);
            JS_DefinePropertyValueStr(ctx, d.setter, "length", JS_NewInt32(ctx, 1), JS_PROP_CONFIGURABLE);
            if (m->kind != IDL_STATIC_ATTR) set_checks(ctx, seen, n, cap, d.setter, 0, brand);
        }
        JS_DefinePropertyGetSet(ctx, obj, a, d.getter, d.setter, JS_PROP_ENUMERABLE | JS_PROP_CONFIGURABLE);
    }
    JS_FreeAtom(ctx, a);
}

static void define_constant(JSContextHandle ctx, GCValue obj, const IdlMember *m) {
    double v = m->value ? strtod(m->value, NULL) : 0;
    GCValue val = v == (double)(int32_t)v ? JS_NewInt32(ctx, (int32_t)v) : JS_NewFloat64(ctx, v);
    redefine_value(ctx, obj, m->name, val, JS_PROP_ENUMERABLE);
}

/* The names of the interfaces the table has that workers do not
 * (worker_api.c), as an array. */
GCValue idl_window_only_names(JSContextHandle ctx) {
    GCValue a = JS_NewArray(ctx);
    uint32_t k = 0;
    for (int i = 0; i < NIFACE; i++)
        if (!(k_idl_ifaces[i].flags & 4)) JS_SetPropertyUint32(ctx, a, k++, JS_NewString(ctx, k_idl_ifaces[i].name));
    return a;
}

void idl_shape_apply(JSContextHandle ctx, GCValue global) {
    JS_SetBrandCheck(brand_check);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    bool win_other = JS_IsObject(win) && !JS_StrictEq(ctx, win, global);
    Checked *seen = NULL;
    int n = 0, cap = 0;
    for (int i = 0; i < NIFACE; i++) {
        const IdlIface *f = &k_idl_ifaces[i];
        if (f->flags & 2) continue;
        GCValue ctor = JS_GetPropertyStr(ctx, global, f->name);
        if (!JS_IsObject(ctor)) continue;
        /* (interface objects: writable, configurable, not enumerable) */
        redefine_value(ctx, global, f->name, ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        if (win_other) redefine_value(ctx, win, f->name, ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        const IdlMember *mem = &k_idl_members[f->first];
        for (int k = 0; k < f->count; k++)
            if (mem[k].kind == IDL_CONST) define_constant(ctx, ctor, &mem[k]);
        if (f->flags & 1) continue;     /* (a callback interface: its constants only) */
        if (f->ctor >= 0) redefine_value(ctx, ctor, "length", JS_NewInt32(ctx, f->ctor), JS_PROP_CONFIGURABLE);
        GCValue proto = JS_GetPropertyStr(ctx, ctor, "prototype");
        if (!JS_IsObject(proto)) continue;
        redefine_value(ctx, ctor, "prototype", proto, 0);
        redefine_value(ctx, proto, "constructor", ctor, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
        /* (an interface object's [[Prototype]]: its parent's interface object) */
        if (f->parent) {
            GCValue pc = JS_GetPropertyStr(ctx, global, f->parent);
            if (JS_IsObject(pc) && !JS_StrictEq(ctx, JS_GetPrototype(ctx, ctor), pc)) JS_SetPrototype(ctx, ctor, pc);
        }
        /* (its class string: the prototype's own @@toStringTag) */
        {
            GCValue sym = JS_GetPropertyStr(ctx, JS_GetPropertyStr(ctx, global, "Symbol"), "toStringTag");
            JSAtom ta = JS_ValueToAtom(ctx, sym);
            if (JS_GetOwnProperty(ctx, NULL, proto, ta) == 0)
                JS_DefinePropertyValue(ctx, proto, ta, JS_NewString(ctx, f->name), JS_PROP_CONFIGURABLE);
            JS_FreeAtom(ctx, ta);
        }
        /* ([Unscopable] members: the prototype's @@unscopables, a null-prototype object) */
        GCValue unscop = JS_UNDEFINED;
        for (int k = 0; k < f->count; k++) {
            if (!mem[k].unscopable) continue;
            if (JS_IsUndefined(unscop)) unscop = JS_NewObjectProto(ctx, JS_NULL);
            JS_DefinePropertyValueStr(ctx, unscop, mem[k].name, JS_TRUE, JS_PROP_C_W_E);
        }
        if (!JS_IsUndefined(unscop)) {
            GCValue sym = JS_GetPropertyStr(ctx, JS_GetPropertyStr(ctx, global, "Symbol"), "unscopables");
            JSAtom ua = JS_ValueToAtom(ctx, sym);
            JS_DefinePropertyValue(ctx, proto, ua, unscop, JS_PROP_CONFIGURABLE);
            JS_FreeAtom(ctx, ua);
        }
        int brand = brand_of(f->name);
        for (int k = 0; k < f->count; k++) {
            const IdlMember *m = &mem[k];
            if (m->kind == IDL_CONST) define_constant(ctx, proto, m);
            else shape_member(ctx, m->kind == IDL_STATIC_OP || m->kind == IDL_STATIC_ATTR ? ctor : proto, m, brand,
                              &seen, &n, &cap);
        }
    }
    free(seen);
}
