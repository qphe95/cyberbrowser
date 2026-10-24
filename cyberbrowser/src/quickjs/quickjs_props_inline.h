/*
 * quickjs_props_inline.h - property lookup the interpreter (quickjs.c) and
 * the baseline JIT's stencils (quickjs_jit_ops.c) share, so compiled code
 * inlines exactly the lookup the interpreter does.
 */
#ifndef QUICKJS_PROPS_INLINE_H
#define QUICKJS_PROPS_INLINE_H

#include <stdio.h>
#include "quickjs.h"
#include "cyber_env.h"

typedef struct JSShape JSShape;
typedef struct JSObject JSObject;

static inline GCHandle js_shape_get_proto_handle_atomic(JSShapeHandle sh)
{
    return js_shape_proto_handle_atomic_load(sh);
}

static inline GCHandle js_object_get_proto_handle_atomic(JSObjectHandle obj)
{
    JSShapeHandle sh = (JSShapeHandle)js_obj_shape_handle(obj);
    return js_shape_get_proto_handle_atomic(sh);
}

/* find_own_property's check of a miss under CYBER_TRACE_DF (quickjs.c). */
JSShapeProperty *find_own_property_lost(JSProperty **ppr, JSObject *o, JSShape *s, JSShapeHandle sh,
                                        JSAtom atom);

static inline __attribute__((always_inline)) JSShapeProperty *find_own_property(JSProperty **ppr,
                                                                               JSObjectHandle p,
                                                                               JSAtom atom)
{
    /* The object and its shape are dereferenced once: this runs for every
     * property access the interpreter makes. */
    JSObject *o = (JSObject *)gc_deref(p);
    JSShapeHandle sh = o ? (JSShapeHandle)atomic_load_u32((volatile uint32_t *)&o->shape_handle) : GC_HANDLE_NULL;
    JSShape *s = (JSShape *)gc_deref(sh);
    if (__builtin_expect(!s, 0)) {
        *ppr = NULL;
        return NULL;
    }
    JSShapeProperty *pr, *prop = s->prop;
    uint32_t h = ((uint32_t *)(prop + s->prop_size))[(uintptr_t)atom & s->prop_hash_mask];
    while (h) {
        pr = &prop[h - 1];
        if (__builtin_expect(pr->atom == atom, 1)) {
            /* Valid until the next GC point: callers use it immediately. */
            *ppr = &js_obj_props_of(o)[h - 1];
            return pr;
        }
        h = pr->hash_next;
    }
    /* A miss.  CYBER_TRACE_DF=1 checks the hash chain against the property
     * array (chains were once lost under the moving collector) and reports a
     * property the chain does not reach. */
    if (__builtin_expect(CYBER_ENV(CYBER_T_DF), 0))
        return find_own_property_lost(ppr, o, s, sh, atom);
    *ppr = NULL;
    return NULL;
}

/* get_field's fast path (the interpreter's GET_FIELD_INLINE, less its
 * CYBER_TRACE_NAF bookkeeping): obj.atom when obj is an object and the
 * property a plain data property of it or of a prototype reached through
 * ordinary objects.  0 (nothing done) otherwise. */
static inline __attribute__((always_inline)) int js_get_field_fast(GCValue obj, JSAtom atom, GCValue *pval)
{
    JSObjectHandle p;
    JSProperty *pr;
    JSShapeProperty *prs;

    if (__builtin_expect(GC_VALUE_GET_TAG(obj) != JS_TAG_OBJECT, 0))
        return 0;
    p = (JSObjectHandle)GC_VALUE_GET_HANDLE(obj);
    for (;;) {
        prs = find_own_property(&pr, p, atom);
        if (prs) {
            if (__builtin_expect(prs->flags & JS_PROP_TMASK, 0))
                return 0;
            *pval = pr->u.value;
            return 1;
        }
        if (__builtin_expect(SAFE_GET_OR(p, JSObjectHandle, is_exotic, false), 0))
            return 0;
        GCHandle proto_handle = js_object_get_proto_handle_atomic(p);
        if (proto_handle == GC_HANDLE_NULL) {
            *pval = JS_UNDEFINED;
            return 1;
        }
        p = (JSObjectHandle)(proto_handle);
        if (!SAFE_VALID(p)) {
            *pval = JS_UNDEFINED;
            return 1;
        }
    }
}

#endif /* QUICKJS_PROPS_INLINE_H */
