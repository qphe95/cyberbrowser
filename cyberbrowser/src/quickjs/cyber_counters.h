/*
 * cyber_counters.h - per-subsystem event counters (JS_PERF_DESIGN.md
 * section 4).  CYBER_COUNTERS=1 counts, per run, the work the engine does:
 * allocations by type, calls by kind, property accesses by kind; it prints
 * them at exit as "[counters] <name> <value>".  Counts do not depend on the
 * machine's load, so a change can be shown to remove work when time is too
 * noisy to show it.  Off, each counter costs one predictable branch.
 */
#ifndef CYBER_COUNTERS_H
#define CYBER_COUNTERS_H

#include <stdint.h>

#define CYBER_COUNTER_LIST(X) \
    /* Calls */ \
    X(call_bytecode_interpreted)    /* a bytecode function's call run by the interpreter */ \
    X(call_bytecode_entered_jit)    /* ... entering its compiled code from JS_CallInternal */ \
    X(call_jit_direct)              /* compiled code calling compiled code directly */ \
    X(call_jit_native)              /* compiled code calling a C function directly */ \
    X(call_jit_helper)              /* compiled code's call through the helper */ \
    X(call_native)                  /* any C function call (js_call_c_function) */ \
    X(closure)                      /* js_closure: function objects made */ \
    X(closure_prototype)            /* ... with a (lazy) prototype */ \
    X(prototype_made)               /* lazy prototypes made on first use */ \
    X(closure_legacy_lazy)          /* closures of lazy functions without a resume-parse state (pin frames) */ \
    X(frame_js_stack)               /* bytecode frames on the JS stack */ \
    X(frame_js_stack_c)             /* C function frames on the JS stack */ \
    X(frame_pooled)                 /* bytecode frames taken from the pool */ \
    X(frame_allocated)              /* ... allocated */ \
    X(frame_miss_size)              /* ... the pooled frame at that depth too small */ \
    X(frame_miss_empty)             /* ... none pooled at that depth (first call, or it was pinned) */ \
    X(frame_miss_deep)              /* ... deeper than the pool */ \
    X(frame_pinned)                 /* pooled frames that left the pool, pinned */ \
    /* Property access */ \
    X(get_interp)                   /* interpreter get_field / get_field2 */ \
    X(get_interp_slow)              /* ... through JS_GetPropertyInternal */ \
    X(get_generic)                  /* JS_GetPropertyInternal, any caller */ \
    X(set_generic)                  /* JS_SetPropertyInternal */ \
    X(put_fast_own)                 /* compiled put_field: an own property written (jit_put_field_fast) */ \
    X(put_fast_add)                 /* ... a property added */ \
    X(define_property)              /* JS_DefineProperty */ \
    X(define_fast)                  /* JS_DefinePropertyValue's fast path (not in define_property) */ \
    X(add_property)                 /* add_property */ \
    X(shape_transition_cached)      /* add_property through the shape's transition cache */ \
    X(shape_transition_hashed)      /* ... found in the shape hash table */ \
    X(shape_transition_new)         /* ... a new shape made */ \
    X(ic_lookup)                    /* jit_get_field_ic calls (way 0 misses inline) */ \
    X(ic_hit)                       /* ... answered by a way */ \
    X(ic_hit_mega)                  /* ... answered by the megamorphic cache */ \
    X(ic_fill)                      /* ... a way or megamorphic entry filled */ \
    X(ic_uncacheable)               /* ... accessor or exotic: through the helper */

enum {
#define CYBER_COUNTER_ENUM(name) CC_##name,
    CYBER_COUNTER_LIST(CYBER_COUNTER_ENUM)
#undef CYBER_COUNTER_ENUM
    CC_COUNT
};

#define CC_ALLOC_TYPES 32

extern int g_cyber_counters_on;
extern uint64_t g_cyber_counters[CC_COUNT];
extern uint64_t g_cyber_alloc_count[CC_ALLOC_TYPES], g_cyber_alloc_bytes[CC_ALLOC_TYPES];

#define CC_INC(name) do { \
    if (__builtin_expect(g_cyber_counters_on, 0)) g_cyber_counters[CC_##name]++; \
} while (0)
#define CC_ALLOC(type, size) do { \
    if (__builtin_expect(g_cyber_counters_on, 0)) { \
        unsigned cc_t_ = (unsigned)(type) < CC_ALLOC_TYPES ? (unsigned)(type) : CC_ALLOC_TYPES - 1; \
        g_cyber_alloc_count[cc_t_]++; \
        g_cyber_alloc_bytes[cc_t_] += (size); \
    } \
} while (0)

#endif /* CYBER_COUNTERS_H */
