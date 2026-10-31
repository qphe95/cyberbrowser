/*
 * character_data.c - CharacterData and the node interfaces over it (DOM
 * 4.10-4.14): CharacterData's length and data editing, Text (its
 * constructor, splitText, wholeText), Comment, CDATASection,
 * ProcessingInstruction (target) and DocumentType (name, publicId,
 * systemId).
 *
 * Offsets count UTF-16 code units, as `data` does in JS: the editing
 * methods slice the data as a JS string and store it back through the data
 * setter (which queues the characterData mutation record).
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

extern NodeId g_dom_document;

static GCValue illegal_ctor(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "Illegal constructor");
}

static NodeId chardata_of(GCValue v) {
    NodeId n = dom_node_of(v);
    return dom_kind_is_chardata(dom_kind(n)) ? n : DOM_NULL;
}

#define THIS_CHARDATA(n)                                                     \
    NodeId n = chardata_of(this_val);                                        \
    if (!n) return JS_ThrowTypeError(ctx, "Illegal invocation")

/* data as a JS string, and its length in code units. */
static GCValue data_of(JSContextHandle ctx, GCValue this_val, uint32_t *len) {
    GCValue d = js_node_get_data(ctx, this_val, 0, NULL);
    GCValue l = JS_GetPropertyStr(ctx, d, "length");
    JS_ToUint32(ctx, len, l);
    return d;
}

static GCValue str_slice(JSContextHandle ctx, GCValue s, uint32_t a, uint32_t b) {
    GCValue fn = JS_GetPropertyStr(ctx, s, "slice");
    GCValue args[2] = { JS_NewUint32(ctx, a), JS_NewUint32(ctx, b) };
    return JS_Call(ctx, fn, s, 2, args);
}

static GCValue str_concat3(JSContextHandle ctx, GCValue a, GCValue b, GCValue c) {
    GCValue fn = JS_GetPropertyStr(ctx, a, "concat");
    GCValue args[2] = { b, c };
    return JS_Call(ctx, fn, a, 2, args);
}

/* An "unsigned long" argument (WebIDL: modulo 2^32); false if missing. */
static bool ulong_arg(JSContextHandle ctx, int argc, GCValue *argv, int i, uint32_t *out) {
    if (argc <= i) return false;
    return JS_ToUint32(ctx, out, argv[i]) == 0;
}

static GCValue not_enough(JSContextHandle ctx, const char *method, int n) {
    return JS_ThrowTypeError(ctx, "Failed to execute '%s' on 'CharacterData': %d argument%s required.", method, n,
                             n == 1 ? "" : "s");
}

/* DOM "replace data": offset past the end is an IndexSizeError. */
static GCValue replace_data(JSContextHandle ctx, GCValue this_val, uint32_t off, uint32_t count, GCValue repl) {
    uint32_t len = 0;
    GCValue d = data_of(ctx, this_val, &len);
    if (off > len) return throw_dom_exception(ctx, "IndexSizeError", "The offset is greater than the data's length.");
    if (count > len - off) count = len - off;
    GCValue s = str_concat3(ctx, str_slice(ctx, d, 0, off), repl, str_slice(ctx, d, off + count, len));
    if (JS_IsException(s)) return s;
    return js_node_set_data(ctx, this_val, 1, &s);
}

static GCValue string_arg(JSContextHandle ctx, GCValue v) {
    return JS_IsString(v) ? v : JS_ToString(ctx, v);
}

/* ---- CharacterData ---- */

static GCValue js_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    THIS_CHARDATA(n);
    (void)n;
    uint32_t len = 0;
    data_of(ctx, this_val, &len);
    return JS_NewUint32(ctx, len);
}

static GCValue js_substring_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_CHARDATA(n);
    (void)n;
    uint32_t off, count;
    if (!ulong_arg(ctx, argc, argv, 0, &off) || !ulong_arg(ctx, argc, argv, 1, &count))
        return argc < 2 ? not_enough(ctx, "substringData", 2) : JS_EXCEPTION;
    uint32_t len = 0;
    GCValue d = data_of(ctx, this_val, &len);
    if (off > len) return throw_dom_exception(ctx, "IndexSizeError", "The offset is greater than the data's length.");
    return str_slice(ctx, d, off, count > len - off ? len : off + count);
}

static GCValue js_append_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_CHARDATA(n);
    (void)n;
    if (argc < 1) return not_enough(ctx, "appendData", 1);
    GCValue s = string_arg(ctx, argv[0]);
    if (JS_IsException(s)) return s;
    uint32_t len = 0;
    data_of(ctx, this_val, &len);
    return replace_data(ctx, this_val, len, 0, s);
}

static GCValue js_insert_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_CHARDATA(n);
    (void)n;
    uint32_t off;
    if (argc < 2) return not_enough(ctx, "insertData", 2);
    if (!ulong_arg(ctx, argc, argv, 0, &off)) return JS_EXCEPTION;
    GCValue s = string_arg(ctx, argv[1]);
    if (JS_IsException(s)) return s;
    return replace_data(ctx, this_val, off, 0, s);
}

static GCValue js_delete_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_CHARDATA(n);
    (void)n;
    uint32_t off, count;
    if (argc < 2) return not_enough(ctx, "deleteData", 2);
    if (!ulong_arg(ctx, argc, argv, 0, &off) || !ulong_arg(ctx, argc, argv, 1, &count)) return JS_EXCEPTION;
    return replace_data(ctx, this_val, off, count, JS_NewString(ctx, ""));
}

static GCValue js_replace_data(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    THIS_CHARDATA(n);
    (void)n;
    uint32_t off, count;
    if (argc < 3) return not_enough(ctx, "replaceData", 3);
    if (!ulong_arg(ctx, argc, argv, 0, &off) || !ulong_arg(ctx, argc, argv, 1, &count)) return JS_EXCEPTION;
    GCValue s = string_arg(ctx, argv[2]);
    if (JS_IsException(s)) return s;
    return replace_data(ctx, this_val, off, count, s);
}

/* ---- Text ---- */

/* The prototype of a constructed object: new.target's (subclasses), else the
 * interface's. */
static GCValue ctor_proto(JSContextHandle ctx, GCValue new_target, const char *name) {
    GCValue p = JS_IsObject(new_target) ? JS_GetPropertyStr(ctx, new_target, "prototype") : JS_UNDEFINED;
    if (JS_IsObject(p)) return p;
    GCValue c = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), name);
    return JS_IsObject(c) ? JS_GetPropertyStr(ctx, c, "prototype") : JS_UNDEFINED;
}

/* new Text(data = "") / new Comment(data = ""): a node of the document of
 * the constructor's window (a frame's, for its realm's). */
static GCValue construct_chardata(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv, int kind,
                                  const char *name) {
    if (JS_IsUndefined(new_target))
        return JS_ThrowTypeError(ctx, "Failed to construct '%s': Please use the 'new' operator", name);
    size_t n = 0;
    const char *s = "";
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        s = JS_ToCStringLen(ctx, &n, argv[0]);
        if (!s) return JS_EXCEPTION;
    }
    NodeId doc = dom_node_of(JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "document"));
    if (dom_kind(doc) != DOM_K_DOCUMENT) doc = g_dom_document;
    GCValue obj = dom_js(ctx, dom_create_chardata(kind, s, n, doc));
    if (!JS_IsObject(obj)) return JS_EXCEPTION;
    GCValue proto = ctor_proto(ctx, new_target, name);
    if (JS_IsObject(proto)) JS_SetPrototype(ctx, obj, proto);
    return obj;
}

static GCValue js_text_ctor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    return construct_chardata(ctx, new_target, argc, argv, DOM_K_TEXT, "Text");
}

static GCValue js_comment_ctor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    return construct_chardata(ctx, new_target, argc, argv, DOM_K_COMMENT, "Comment");
}

static bool is_text_kind(int k) { return k == DOM_K_TEXT || k == DOM_K_CDATA; }

/* splitText(offset): the data from offset on, as a new Text after this one. */
static GCValue js_split_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    NodeId n = dom_node_of(this_val);
    if (!is_text_kind(dom_kind(n))) return JS_ThrowTypeError(ctx, "Illegal invocation");
    uint32_t off;
    if (argc < 1) return not_enough(ctx, "splitText", 1);
    if (!ulong_arg(ctx, argc, argv, 0, &off)) return JS_EXCEPTION;
    uint32_t len = 0;
    GCValue d = data_of(ctx, this_val, &len);
    if (off > len) return throw_dom_exception(ctx, "IndexSizeError", "The offset is greater than the data's length.");
    GCValue tail = str_slice(ctx, d, off, len);
    size_t tn = 0;
    const char *ts = JS_ToCStringLen(ctx, &tn, tail);
    if (!ts) return JS_EXCEPTION;
    NodeId owner = dom_owner(n) ? dom_owner(n) : g_dom_document;
    GCValue node = dom_js(ctx, dom_create_chardata(DOM_K_TEXT, ts, tn, owner));
    if (!JS_IsObject(node)) return JS_EXCEPTION;
    GCValue parent = dom_js(ctx, dom_parent(n));
    if (JS_IsObject(parent)) {
        GCValue args[2] = { node, dom_js(ctx, dom_next_sibling(n)) };
        GCValue r = js_node_insertBefore_real(ctx, parent, 2, args);
        if (JS_IsException(r)) return r;
    }
    GCValue r = replace_data(ctx, this_val, off, len - off, JS_NewString(ctx, ""));
    if (JS_IsException(r)) return r;
    return node;
}

/* wholeText: the data of the contiguous Text nodes around this one. */
static GCValue js_whole_text(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (!is_text_kind(dom_kind(n))) return JS_ThrowTypeError(ctx, "Illegal invocation");
    NodeId s = n;
    while (is_text_kind(dom_kind(dom_prev_sibling(s)))) s = dom_prev_sibling(s);
    size_t total = 0;
    for (NodeId c = s; is_text_kind(dom_kind(c)); c = dom_next_sibling(c)) {
        uint32_t l = 0;
        dom_text(c, &l);
        total += l;
    }
    char *buf = malloc(total + 1);
    if (!buf) return JS_ThrowOutOfMemory(ctx);
    size_t o = 0;
    for (NodeId c = s; is_text_kind(dom_kind(c)); c = dom_next_sibling(c)) {
        uint32_t l = 0;
        const char *t = dom_text(c, &l);
        memcpy(buf + o, t, l);
        o += l;
    }
    GCValue r = JS_NewStringLen(ctx, buf, o);
    free(buf);
    return r;
}

/* ---- ProcessingInstruction, DocumentType ---- */

static GCValue js_pi_target(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (dom_kind(n) != DOM_K_PI) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_NewString(ctx, dom_node_name(n));
}

static GCValue js_doctype_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv, int magic) {
    (void)argc; (void)argv;
    NodeId n = dom_node_of(this_val);
    if (dom_kind(n) != DOM_K_DOCTYPE) return JS_ThrowTypeError(ctx, "Illegal invocation");
    return JS_NewString(ctx, magic == 0   ? dom_node_name(n)
                             : magic == 1 ? dom_doctype_public_id(n)
                                          : dom_doctype_system_id(n));
}

/* ---- installing ---- */

static void getter(JSContextHandle ctx, GCValue proto, const char *name, JSCFunction *fn) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunction(ctx, fn, name, 0), JS_UNDEFINED,
                            JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

static void getter_magic(JSContextHandle ctx, GCValue proto, const char *name, JSCFunctionMagic *fn, int magic) {
    JSAtom a = JS_NewAtom(ctx, name);
    JS_DefinePropertyGetSet(ctx, proto, a, JS_NewCFunctionMagic(ctx, fn, name, 0, JS_CFUNC_generic_magic, magic),
                            JS_UNDEFINED, JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, a);
}

static void method(JSContextHandle ctx, GCValue proto, const char *name, JSCFunction *fn, int length) {
    JS_DefinePropertyValueStr(ctx, proto, name, JS_NewCFunction(ctx, fn, name, length),
                              JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
}

/* An interface: its constructor (illegal unless ctor) on the global and
 * window, with proto (inheriting from parent) and its toStringTag. */
static GCValue interface(JSContextHandle ctx, GCValue global, const char *name, JSCFunction *ctor, int length,
                         GCValue proto, GCValue parent) {
    if (!JS_IsObject(proto)) proto = JS_NewObject(ctx);
    if (JS_IsObject(parent)) JS_SetPrototype(ctx, proto, parent);
    GCValue c = JS_NewCFunction2(ctx, ctor ? ctor : illegal_ctor, name, length, JS_CFUNC_constructor_or_func, 0);
    JS_SetConstructor(ctx, c, proto);
    JS_DefinePropertyValueStr(ctx, global, name, c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue win = JS_GetPropertyStr(ctx, global, "window");
    if (JS_IsObject(win) && !JS_StrictEq(ctx, win, global))
        JS_DefinePropertyValueStr(ctx, win, name, c, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE);
    GCValue sym = JS_GetPropertyStr(ctx, global, "Symbol");
    JSAtom tag = JS_ValueToAtom(ctx, JS_GetPropertyStr(ctx, sym, "toStringTag"));
    JS_DefinePropertyValue(ctx, proto, tag, JS_NewString(ctx, name), JS_PROP_CONFIGURABLE);
    JS_FreeAtom(ctx, tag);
    return proto;
}

/* character_data_proto: the prototype Text and Comment nodes had (data and
 * the ChildNode methods are on it); it becomes CharacterData.prototype. */
void character_data_install(JSContextHandle ctx, GCValue global, GCValue character_data_proto, GCValue node_proto) {
    GCValue cd = interface(ctx, global, "CharacterData", NULL, 0, character_data_proto, JS_UNDEFINED);
    getter(ctx, cd, "length", js_length);
    method(ctx, cd, "substringData", js_substring_data, 2);
    method(ctx, cd, "appendData", js_append_data, 1);
    method(ctx, cd, "insertData", js_insert_data, 2);
    method(ctx, cd, "deleteData", js_delete_data, 2);
    method(ctx, cd, "replaceData", js_replace_data, 3);

    GCValue text = interface(ctx, global, "Text", (JSCFunction *)js_text_ctor, 0, JS_UNDEFINED, cd);
    method(ctx, text, "splitText", js_split_text, 1);
    getter(ctx, text, "wholeText", js_whole_text);
    interface(ctx, global, "CDATASection", NULL, 0, JS_UNDEFINED, text);
    interface(ctx, global, "Comment", (JSCFunction *)js_comment_ctor, 0, JS_UNDEFINED, cd);
    GCValue pi = interface(ctx, global, "ProcessingInstruction", NULL, 0, JS_UNDEFINED, cd);
    getter(ctx, pi, "target", js_pi_target);

    GCValue dt = interface(ctx, global, "DocumentType", NULL, 0, JS_UNDEFINED, node_proto);
    getter_magic(ctx, dt, "name", js_doctype_get, 0);
    getter_magic(ctx, dt, "publicId", js_doctype_get, 1);
    getter_magic(ctx, dt, "systemId", js_doctype_get, 2);
    /* (ChildNode) */
    static const char *const child_node[] = { "before", "after", "replaceWith", "remove" };
    for (size_t i = 0; i < sizeof child_node / sizeof child_node[0]; i++) {
        GCValue f = JS_GetPropertyStr(ctx, cd, child_node[i]);
        if (JS_IsFunction(ctx, f))
            JS_DefinePropertyValueStr(ctx, dt, child_node[i], f, JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    }
}
