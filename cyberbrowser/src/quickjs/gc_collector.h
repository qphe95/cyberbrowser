/*
 * Collector state shared by gc_collector.c (marking, sweeping, the
 * synchronous collection) and gc_concurrent.c (the GC thread, boundary
 * steps, concurrent cycles, the nursery).
 */
#ifndef GC_COLLECTOR_H
#define GC_COLLECTOR_H

#include "gc_core.h"

enum {
    GC_CYCLE_NONE = 0,
    GC_CYCLE_FULL,      /* stop-the-world, whole heap */
    GC_CYCLE_MAJOR,     /* concurrent, whole heap */
    GC_CYCLE_MINOR,     /* concurrent, nursery */
};

typedef struct GCWorklist {
    GCHandle *items;
    uint32_t n;
    uint32_t cap;
} GCWorklist;

typedef struct GCCollector {
    int kind;                 /* GC_CYCLE_* of the cycle being marked/swept */
    uint32_t parity;          /* mark bitmap in use */
    GCWorklist work;
    uintptr_t *pins;          /* headers pinned by the last pin_running_code, sorted */
    uint32_t pins_n, pins_cap;
    uintptr_t stack_lo;       /* JS thread stack range for the current scan */
    uintptr_t stack_hi;
} GCCollector;

extern GCCollector g_col;

typedef void JS_MarkFunc(JSRuntimeHandle rt, GCHandle handle);

/* Engine hooks (quickjs.c) */
void mark_children(JSRuntimeHandle rt, GCHandle handle, JS_MarkFunc *mark_func);
void JS_MarkActiveStackFrames(JSRuntimeHandle rt, JS_MarkFunc *mark_func);
void JS_GCEnumerateEngineRoots(JSRuntimeHandle rt, JS_MarkFunc *mark_func);
void JS_GCForEachActiveFrameObject(JSRuntimeHandle rt, JS_MarkFunc *fn);
void JS_GCWeakStep(JSRuntimeHandle rt);
int JS_GCHasActiveFrames(JSRuntimeHandle rt);

/* Engine hooks (quickjs.c), continued */
void JS_GCMarkContextRoots(JSRuntimeHandle rt, JSContextHandle ctx, JS_MarkFunc *mark_func);
int JS_GCWeakContainerHasDead(GCHandle h, int kind);
void *JS_GCWeakCleanShapes(JSRuntimeHandle rt);
void JS_GCWeakApply(JSRuntimeHandle rt, void *cleaned_shapes,
                    const GCHandle *const lists[3], const uint32_t counts[3]);

/* gc_collector.c */
void gc_col_mark_cb(JSRuntimeHandle rt, GCHandle h);
bool gc_col_drain(JSRuntimeHandle rt, uint32_t limit);
void gc_col_enumerate_roots(JSRuntimeHandle rt, bool pin);
void gc_col_enumerate_roots_cb(JSRuntimeHandle rt, JS_MarkFunc *cb);
void gc_col_capture_stack(uintptr_t lo);
void gc_col_pin_running_code(JSRuntimeHandle rt);
void gc_col_weak_step(JSRuntimeHandle rt);
void gc_col_finalize_dead(JSRuntimeHandle rt, bool finalizers);
void gc_col_begin_cycle(int kind, bool clear_bits);
void gc_col_end_cycle(void);
bool gc_col_header_condemned(const GCHeader *hdr);
bool gc_col_is_marked(const GCRegion *r, const GCHeader *hdr);
bool gc_col_set_mark(GCRegion *r, const GCHeader *hdr);
bool gc_col_in_cycle(const GCRegion *r, const GCHeader *hdr);
bool gc_col_is_pinned(const GCHeader *hdr);
GCHeader *gc_col_handle_header(GCHandle h);
void gc_col_worklist_push(GCHandle h);
void gc_col_rescan_card(JSRuntimeHandle rt, uint32_t card);
size_t gc_col_sweep_region(GCRegion *r, uintptr_t limit, bool now);
void gc_col_sort_uintptr(uintptr_t *a, size_t n);

/* gc_concurrent.c */
void gc_col_finish_concurrent_cycle(uintptr_t stack_lo);
void gc_col_freed_ring_push(GCHandle h);
void gc_col_verify_impl(JSRuntimeHandle rt);
void gc_col_mark_ref_log(JSRuntimeHandle rt, uint32_t from);
void gc_core_after_full_collection(void);
uint32_t gc_test_heap_check(void);

#endif /* GC_COLLECTOR_H */
