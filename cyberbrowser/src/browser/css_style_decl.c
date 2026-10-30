/*
 * css_style_decl.c - CSSStyleDeclaration (CSSOM 6.7) for element.style:
 * an ordered list of declarations (property, value, !important), read and
 * written through the property table (css_props.h).
 *
 *   - setProperty / the IDL attributes check a value against its property's
 *     grammar and keep its canonical serialization; a shorthand sets its
 *     longhands, and reads back from them;
 *   - cssText serializes the block, shorthands where all their longhands
 *     are there ("serialize a CSS declaration block");
 *   - every change is written back to the owner element's style attribute
 *     (CSSOM "update style attribute"), which the layout mirror reads, and
 *     marks it for layout.
 *
 * The IDL attributes (camelCase, dashed, webkitCased, cssFloat) are magic
 * accessors on CSSStyleDeclaration.prototype, one entry list for all realms.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_internal.h"
#include "css_parser.h"
#include "css_props.h"
#include "dom_js.h"
#include "css_style_decl.h"
#include "computed_style.h"

typedef struct StyleDecl {
    const CssPropDef *prop;     /* NULL for a custom property */
    char *name;                 /* custom property name (--x) */
    char *value;
    bool important;
} StyleDecl;

typedef struct StyleBlock {
    StyleDecl *d;
    int n, cap;
    NodeId owner;
    bool syncing;               /* applying the style attribute: no write-back */
    ComputedStyle *cs;          /* getComputedStyle's (read-only): its values */
} StyleBlock;

/* The GC cell the object's opaque handle points at. */
typedef struct StyleCell { StyleBlock *b; } StyleCell;

/* The realm a computed style's reads lay out in (set by each JS entry). */
static JSContextHandle g_ctx_for_get;

JSClassID js_css_style_decl_class_id;

static StyleBlock *block_of(JSContextHandle ctx, GCValue obj) {
    GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_css_style_decl_class_id);
    if (h == GC_HANDLE_NULL) return NULL;
    StyleCell *c = (StyleCell *)gc_deref(h);
    return c ? c->b : NULL;
}

static void block_clear(StyleBlock *b) {
    for (int i = 0; i < b->n; i++) { free(b->d[i].name); free(b->d[i].value); }
    b->n = 0;
}

static void css_style_decl_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    GCHandle h = JS_GetOpaqueHandle(val, js_css_style_decl_class_id);
    if (h == GC_HANDLE_NULL) return;
    StyleCell *c = (StyleCell *)gc_deref(h);
    if (!c || !c->b) return;
    block_clear(c->b);
    computed_style_free(c->b->cs);
    free(c->b->d);
    free(c->b);
    c->b = NULL;
}

static void css_style_decl_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle h = JS_GetOpaqueHandle(val, js_css_style_decl_class_id);
    if (h != GC_HANDLE_NULL) mark_func(rt, h);
}

JSClassDef js_css_style_decl_class_def = {
    .class_name = "CSSStyleDeclaration",
    .finalizer = css_style_decl_finalizer,
    .gc_mark = css_style_decl_mark,
};

/* ---- the block ---- */

static StyleDecl *find_decl(StyleBlock *b, const CssPropDef *p, const char *custom) {
    for (int i = 0; i < b->n; i++) {
        if (p ? b->d[i].prop == p : (b->d[i].name && strcmp(b->d[i].name, custom) == 0)) return &b->d[i];
    }
    return NULL;
}

/* Set one longhand (or custom property): updated in place, else appended. */
static void put_decl(StyleBlock *b, const CssPropDef *p, const char *custom, const char *value, bool important) {
    StyleDecl *d = find_decl(b, p, custom);
    if (d) {
        free(d->value);
        d->value = strdup(value);
        d->important = important;
        return;
    }
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 8;
        StyleDecl *nd = realloc(b->d, (size_t)cap * sizeof(StyleDecl));
        if (!nd) return;
        b->d = nd;
        b->cap = cap;
    }
    d = &b->d[b->n++];
    d->prop = p;
    d->name = custom ? strdup(custom) : NULL;
    d->value = strdup(value);
    d->important = important;
}

static bool remove_decl(StyleBlock *b, const CssPropDef *p, const char *custom) {
    StyleDecl *d = find_decl(b, p, custom);
    if (!d) return false;
    int i = (int)(d - b->d);
    free(d->name);
    free(d->value);
    memmove(&b->d[i], &b->d[i + 1], (size_t)(b->n - i - 1) * sizeof(StyleDecl));
    b->n--;
    return true;
}

static const CssPropDef *lookup(const char *name, size_t len) {
    return css_prop_find(name, len);
}

/* The value of a shorthand from the block's longhands ("" when it has not
 * all of them, or they differ in importance); *important their priority. */
static char *shorthand_value(StyleBlock *b, const CssPropDef *p, bool *important) {
    const char **names;
    int n = css_prop_longhands(p, &names);
    const char *vals[64];
    int imp = -1;
    for (int i = 0; i < n && i < 64; i++) {
        const CssPropDef *lp = css_prop_find(names[i], strlen(names[i]));
        StyleDecl *d = lp ? find_decl(b, lp, NULL) : NULL;
        if (!d) return NULL;
        if (imp >= 0 && imp != d->important) return NULL;
        imp = d->important;
        vals[i] = d->value;
    }
    if (important) *important = imp == 1;
    return css_prop_shorthand_serialize(p, vals);
}

/* CSSOM "serialize a CSS declaration block". */
static char *block_text(StyleBlock *b) {
    size_t cap = 64, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    out[0] = '\0';
    bool *done = calloc((size_t)(b->n ? b->n : 1), sizeof(bool));
#define APPEND(s) do { size_t l_ = strlen(s); \
        if (len + l_ + 1 > cap) { while (len + l_ + 1 > cap) cap *= 2; out = realloc(out, cap); } \
        memcpy(out + len, s, l_ + 1); len += l_; } while (0)
    for (int i = 0; i < b->n; i++) {
        if (done[i]) continue;
        StyleDecl *d = &b->d[i];
        const char *name = d->prop ? d->prop->name : d->name;
        const char *value = d->value;
        char *sh_value = NULL;
        bool important = d->important;
        if (d->prop) {
            /* the first shorthand (in the table) that has this longhand and
             * whose longhands are all here */
            const CssPropDef *const *shs;
            int nsh = css_prop_shorthands_of(d->prop, &shs);
            for (int k = 0; k < nsh && !sh_value; k++) {
                const CssPropDef *s = shs[k];
                const char **names;
                int n = css_prop_longhands(s, &names);
                bool all_undone = true;
                for (int j = 0; j < n && all_undone; j++) {
                    const CssPropDef *lp = css_prop_find(names[j], strlen(names[j]));
                    StyleDecl *ld = lp ? find_decl(b, lp, NULL) : NULL;
                    if (!ld || done[ld - b->d]) all_undone = false;
                }
                if (!all_undone) continue;
                bool imp = false;
                char *v = shorthand_value(b, s, &imp);
                if (!v) continue;
                sh_value = v;
                name = s->name;
                value = v;
                important = imp;
                for (int j = 0; j < n; j++) {
                    const CssPropDef *lp = css_prop_find(names[j], strlen(names[j]));
                    StyleDecl *ld = find_decl(b, lp, NULL);
                    if (ld) done[ld - b->d] = true;
                }
            }
        }
        done[i] = true;
        if (len) APPEND(" ");
        APPEND(name);
        APPEND(": ");
        APPEND(value);
        if (important) APPEND(" !important");
        APPEND(";");
        free(sh_value);
    }
#undef APPEND
    free(done);
    return out;
}

/* After a change: the owner's style attribute and its layout. */
static void block_changed(StyleBlock *b) {
    if (b->syncing) return;
    /* a rule's declarations (CSSStyleRule.style): its sheet renders anew */
    if (!b->owner && !b->cs) { dom_request_relayout(); return; }
    if (!b->owner || !dom_valid(b->owner)) return;
    char *text = block_text(b);
    if (text) {
        if (text[0] || dom_get_attr_str(b->owner, "style")) dom_set_attr_str(b->owner, "style", text);
        free(text);
    }
    dom_mark_dirty(b->owner, DOMF_DIRTY_ATTR);
    dom_request_layout();
}

/* setProperty's steps: false when nothing was set (unknown property,
 * invalid value or priority). */
static bool block_set(StyleBlock *b, const char *prop, size_t plen, const char *value, const char *priority) {
    bool important = false;
    if (priority && priority[0]) {
        if (strcasecmp(priority, "important") != 0) return false;
        important = true;
    }
    if (css_prop_is_custom(prop, plen)) {
        char *name = strndup(prop, plen);
        if (!value[0]) remove_decl(b, NULL, name);
        else {
            /* the value as written, without leading/trailing white space */
            const char *v = value;
            while (*v && isspace((unsigned char)*v)) v++;
            size_t vl = strlen(v);
            while (vl && isspace((unsigned char)v[vl - 1])) vl--;
            char *tv = strndup(v, vl);
            put_decl(b, NULL, name, tv, important);
            free(tv);
        }
        free(name);
        return true;
    }
    const CssPropDef *p = lookup(prop, plen);
    if (!p) return false;
    if (!value[0]) {
        /* removeProperty */
        if (p->longhands) {
            const char **names;
            int n = css_prop_longhands(p, &names);
            for (int i = 0; i < n; i++) remove_decl(b, css_prop_find(names[i], strlen(names[i])), NULL);
        } else remove_decl(b, p, NULL);
        return true;
    }
    if (p->longhands) {
        char **vals = NULL;
        int n = css_prop_expand(p, value, &vals);
        if (n < 0) return false;
        const char **names;
        css_prop_longhands(p, &names);
        for (int i = 0; i < n; i++) {
            const CssPropDef *lp = css_prop_find(names[i], strlen(names[i]));
            if (lp) put_decl(b, lp, NULL, vals[i], important);
            free(vals[i]);
        }
        free(vals);
        return true;
    }
    char *v = css_prop_parse(p, value);
    if (!v) return false;
    put_decl(b, p, NULL, v, important);
    free(v);
    return true;
}

static char *block_get(StyleBlock *b, const char *prop, size_t plen, bool *important) {
    if (important) *important = false;
    if (b->cs) return computed_style_value(g_ctx_for_get, b->cs, prop, plen);
    if (css_prop_is_custom(prop, plen)) {
        char *name = strndup(prop, plen);
        StyleDecl *d = find_decl(b, NULL, name);
        free(name);
        if (!d) return strdup("");
        if (important) *important = d->important;
        return strdup(d->value);
    }
    const CssPropDef *p = lookup(prop, plen);
    if (!p) return strdup("");
    if (p->longhands) {
        char *v = shorthand_value(b, p, important);
        return v ? v : strdup("");
    }
    StyleDecl *d = find_decl(b, p, NULL);
    if (!d) return strdup("");
    if (important) *important = d->important;
    return strdup(d->value);
}

/* Replace the block with the declarations of text (cssText / the style
 * attribute); invalid ones are dropped. */
static void block_set_text(StyleBlock *b, const char *text) {
    block_clear(b);
    int count = 0;
    CssDeclaration *decls = css_parse_inline_style(text ? text : "", &count);
    for (int i = 0; decls && i < count; i++) {
        if (!decls[i].property || !decls[i].value) continue;
        block_set(b, decls[i].property, strlen(decls[i].property), decls[i].value,
                  decls[i].important ? "important" : "");
    }
    if (decls) css_declarations_free(decls, count);
}

/* ---- C API (css_style_decl.h) ---- */

GCValue css_style_decl_new_computed(JSContextHandle ctx, NodeId el) {
    GCValue obj = css_style_decl_new(ctx, DOM_NULL, NULL);
    StyleBlock *b = block_of(ctx, obj);
    if (b) b->cs = computed_style_new(el);
    return obj;
}

GCValue css_style_decl_new(JSContextHandle ctx, NodeId owner, const char *text) {
    GCValue proto = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__CSSStyleDeclarationProto");
    GCValue obj = JS_IsObject(proto) ? JS_NewObjectProtoClass(ctx, proto, js_css_style_decl_class_id)
                                     : JS_NewObjectClass(ctx, js_css_style_decl_class_id);
    if (JS_IsException(obj)) return obj;
    GCHandle h = gc_allocz(sizeof(StyleCell), JS_GC_OBJ_TYPE_DATA);
    StyleBlock *b = calloc(1, sizeof(StyleBlock));
    if (h == GC_HANDLE_NULL || !b) { free(b); return obj; }
    ((StyleCell *)gc_deref(h))->b = b;
    JS_SetOpaqueHandle(obj, h);
    b->owner = owner;
    if (text && text[0]) {
        b->syncing = true;
        block_set_text(b, text);
        b->syncing = false;
    }
    return obj;
}

bool css_style_decl_is(JSContextHandle ctx, GCValue obj) { return block_of(ctx, obj) != NULL; }

void css_style_decl_set_owner(JSContextHandle ctx, GCValue obj, NodeId owner) {
    StyleBlock *b = block_of(ctx, obj);
    if (b) b->owner = owner;
}

char *css_style_decl_text(JSContextHandle ctx, GCValue obj) {
    StyleBlock *b = block_of(ctx, obj);
    return b ? block_text(b) : NULL;
}

void css_style_decl_sync_from_attr(JSContextHandle ctx, GCValue obj, const char *text) {
    StyleBlock *b = block_of(ctx, obj);
    if (!b) return;
    b->syncing = true;
    block_set_text(b, text);
    b->syncing = false;
    if (b->owner) { dom_mark_dirty(b->owner, DOMF_DIRTY_ATTR); dom_request_layout(); }
}

int css_style_decl_count(JSContextHandle ctx, GCValue obj) {
    StyleBlock *b = block_of(ctx, obj);
    return b ? b->n : 0;
}

bool css_style_decl_at(JSContextHandle ctx, GCValue obj, int i, const char **name, const char **value, bool *important) {
    StyleBlock *b = block_of(ctx, obj);
    if (!b || i < 0 || i >= b->n) return false;
    StyleDecl *d = &b->d[i];
    if (name) *name = d->prop ? d->prop->name : d->name;
    if (value) *value = d->value;
    if (important) *important = d->important;
    return true;
}

bool css_style_decl_set(JSContextHandle ctx, GCValue obj, const char *prop, const char *value, const char *priority) {
    StyleBlock *b = block_of(ctx, obj);
    if (!b || !prop || !value) return false;
    bool ok = block_set(b, prop, strlen(prop), value, priority);
    if (ok) block_changed(b);
    return ok;
}

/* ---- JS ---- */

static StyleBlock *this_block(JSContextHandle ctx, GCValue this_val) {
    StyleBlock *b = block_of(ctx, this_val);
    if (!b) JS_ThrowTypeError(ctx, "Illegal invocation");
    g_ctx_for_get = ctx;
    return b;
}

/* For a write: a computed style is read-only. */
static StyleBlock *writable_block(JSContextHandle ctx, GCValue this_val) {
    StyleBlock *b = this_block(ctx, this_val);
    if (b && b->cs) {
        throw_dom_exception(ctx, "NoModificationAllowedError", "These styles are computed, and therefore read-only.");
        return NULL;
    }
    return b;
}

static GCValue js_set_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    StyleBlock *b = writable_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "Failed to execute 'setProperty': 2 arguments required");
    size_t pl = 0;
    const char *prop = JS_ToCStringLen(ctx, &pl, argv[0]);
    /* [LegacyNullToEmptyString] value */
    const char *value = JS_IsNull(argv[1]) ? "" : JS_ToCString(ctx, argv[1]);
    const char *prio = argc > 2 && !JS_IsUndefined(argv[2]) ? JS_ToCString(ctx, argv[2]) : "";
    if (!prop || !value || !prio) return JS_EXCEPTION;
    b = block_of(ctx, this_val);    /* (conversions may run script) */
    if (b && block_set(b, prop, pl, value, prio)) block_changed(b);
    return JS_UNDEFINED;
}

static GCValue js_remove_property(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    StyleBlock *b = writable_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    size_t pl = 0;
    const char *prop = argc > 0 ? JS_ToCStringLen(ctx, &pl, argv[0]) : "";
    if (!prop) return JS_EXCEPTION;
    b = block_of(ctx, this_val);
    if (!b) return JS_NewString(ctx, "");
    char *old = block_get(b, prop, pl, NULL);
    bool had = old && old[0];
    if (had && block_set(b, prop, pl, "", "")) block_changed(b);
    GCValue r = JS_NewString(ctx, old ? old : "");
    free(old);
    return r;
}

static GCValue js_get_property_value(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    StyleBlock *b = this_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    size_t pl = 0;
    const char *prop = argc > 0 ? JS_ToCStringLen(ctx, &pl, argv[0]) : "";
    if (!prop) return JS_EXCEPTION;
    b = block_of(ctx, this_val);
    char *v = b ? block_get(b, prop, pl, NULL) : NULL;
    GCValue r = JS_NewString(ctx, v ? v : "");
    free(v);
    return r;
}

static GCValue js_get_property_priority(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    StyleBlock *b = this_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    size_t pl = 0;
    const char *prop = argc > 0 ? JS_ToCStringLen(ctx, &pl, argv[0]) : "";
    if (!prop) return JS_EXCEPTION;
    b = block_of(ctx, this_val);
    bool imp = false;
    char *v = b ? block_get(b, prop, pl, &imp) : NULL;
    bool set = v && v[0];
    free(v);
    return JS_NewString(ctx, set && imp ? "important" : "");
}

static GCValue js_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    StyleBlock *b = this_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    uint32_t i = 0;
    if (argc > 0 && JS_ToUint32(ctx, &i, argv[0]) < 0) return JS_EXCEPTION;
    b = block_of(ctx, this_val);
    if (b && b->cs) {
        const char *nm = computed_style_item(b->cs, (int)i);
        return JS_NewString(ctx, nm ? nm : "");
    }
    if (!b || i >= (uint32_t)b->n) return JS_NewString(ctx, "");
    StyleDecl *d = &b->d[i];
    return JS_NewString(ctx, d->prop ? d->prop->name : d->name);
}

static GCValue js_get_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    StyleBlock *b = this_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    return JS_NewInt32(ctx, b->cs ? computed_style_length(b->cs) : b->n);
}

static GCValue js_get_css_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    StyleBlock *b = this_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    if (b->cs) return JS_NewString(ctx, "");
    char *t = block_text(b);
    GCValue r = JS_NewString(ctx, t ? t : "");
    free(t);
    return r;
}

static GCValue js_set_css_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    StyleBlock *b = writable_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    const char *text = argc > 0 && !JS_IsNull(argv[0]) ? JS_ToCString(ctx, argv[0]) : "";
    if (!text) return JS_EXCEPTION;
    b = block_of(ctx, this_val);
    if (!b) return JS_UNDEFINED;
    block_set_text(b, text);
    block_changed(b);
    return JS_UNDEFINED;
}

static GCValue js_get_parent_rule(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    return JS_NULL;
}

/* The IDL attributes: magic = index into g_attr_props. */
static const CssPropDef **g_attr_props;
static JSCFunctionListEntry *g_attr_entries;
static int g_attr_count;

static GCValue js_attr_get(JSContextHandle ctx, GCValue this_val, int magic) {
    StyleBlock *b = this_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    const CssPropDef *p = g_attr_props[magic];
    char *v = block_get(b, p->name, strlen(p->name), NULL);
    GCValue r = JS_NewString(ctx, v ? v : "");
    free(v);
    return r;
}

static GCValue js_attr_set(JSContextHandle ctx, GCValue this_val, GCValue val, int magic) {
    StyleBlock *b = writable_block(ctx, this_val);
    if (!b) return JS_EXCEPTION;
    const char *value = JS_IsNull(val) ? "" : JS_ToCString(ctx, val);
    if (!value) return JS_EXCEPTION;
    b = block_of(ctx, this_val);
    const CssPropDef *p = g_attr_props[magic];
    if (b && block_set(b, p->name, strlen(p->name), value, "")) block_changed(b);
    return JS_UNDEFINED;
}

static void add_attr(int *cap, const char *name, const CssPropDef *p) {
    if (g_attr_count == *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        g_attr_props = realloc(g_attr_props, (size_t)*cap * sizeof(*g_attr_props));
        g_attr_entries = realloc(g_attr_entries, (size_t)*cap * sizeof(*g_attr_entries));
    }
    JSCFunctionListEntry e = JS_CGETSET_MAGIC_DEF(name, js_attr_get, js_attr_set, g_attr_count);
    e.prop_flags = JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE;
    g_attr_props[g_attr_count] = p;
    g_attr_entries[g_attr_count] = e;
    g_attr_count++;
}

static void build_attrs(void) {
    if (g_attr_entries) return;
    int cap = 0;
    for (int i = 0; i < css_prop_count(); i++) {
        const CssPropDef *p = css_prop_at(i);
        char *camel = css_prop_camel(p->name);
        add_attr(&cap, camel, p);                   /* (kept: the entries point at the names) */
        if (strchr(p->name, '-')) add_attr(&cap, p->name, p);
        if (strcmp(p->name, "float") == 0) add_attr(&cap, "cssFloat", p);
    }
    /* aliases: -webkit-x as itself, webkitX and WebkitX; grid-gap, gridGap */
    const CssPropDef *p;
    const char *alias;
    for (int i = 0; (alias = css_prop_alias_at(i, &p)); i++) {
        if (!p) continue;
        add_attr(&cap, alias, p);
        bool webkit = strncmp(alias, "-webkit-", 8) == 0;
        char *camel = css_prop_camel(webkit ? alias + 1 : alias);
        add_attr(&cap, camel, p);
        if (webkit) {
            char *upper = strdup(camel);
            upper[0] = (char)toupper((unsigned char)upper[0]);
            add_attr(&cap, upper, p);
        }
    }
}

/* __cyberStyleDecl(text): a declaration block of no element (a CSS rule's). */
static GCValue js_new_style_decl(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    const char *text = argc > 0 && JS_IsString(argv[0]) ? JS_ToCString(ctx, argv[0]) : "";
    return css_style_decl_new(ctx, DOM_NULL, text ? text : "");
}

/* __cyberRelayout(): a style sheet changed through the CSSOM. */
static GCValue js_relayout(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    dom_request_relayout();
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry g_methods[] = {
    JS_CFUNC_DEF("setProperty", 2, js_set_property),
    JS_CFUNC_DEF("removeProperty", 1, js_remove_property),
    JS_CFUNC_DEF("getPropertyValue", 1, js_get_property_value),
    JS_CFUNC_DEF("getPropertyPriority", 1, js_get_property_priority),
    JS_CFUNC_DEF("item", 1, js_item),
};

void css_style_decl_install(JSContextHandle ctx, GCValue proto) {
    JSRuntimeHandle rt = JS_GetRuntime(ctx);
    if (!js_css_style_decl_class_id) JS_NewClassID(&js_css_style_decl_class_id);
    if (!JS_IsRegisteredClass(rt, js_css_style_decl_class_id))
        JS_NewClass(rt, js_css_style_decl_class_id, &js_css_style_decl_class_def);
    build_attrs();
    JS_SetPropertyFunctionList(ctx, proto, g_methods, (int)(sizeof g_methods / sizeof g_methods[0]));
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberStyleDecl",
                              JS_NewCFunction(ctx, js_new_style_decl, "__cyberStyleDecl", 1), JS_PROP_CONFIGURABLE);
    JS_DefinePropertyValueStr(ctx, JS_GetGlobalObject(ctx), "__cyberRelayout",
                              JS_NewCFunction(ctx, js_relayout, "__cyberRelayout", 0), JS_PROP_CONFIGURABLE);
    JS_SetPropertyFunctionList(ctx, proto, g_attr_entries, g_attr_count);
    static const struct { const char *name; JSCFunction *get, *set; } acc[] = {
        { "cssText", js_get_css_text, js_set_css_text },
        { "length", js_get_length, NULL },
        { "parentRule", js_get_parent_rule, NULL },
    };
    for (size_t i = 0; i < sizeof acc / sizeof acc[0]; i++) {
        JSAtom a = JS_NewAtom(ctx, acc[i].name);
        JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction(ctx, acc[i].get, acc[i].name, 0),
                                acc[i].set ? JS_NewCFunction(ctx, acc[i].set, acc[i].name, 1) : JS_UNDEFINED,
                                JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
        JS_FreeAtom(ctx, a);
    }
}
