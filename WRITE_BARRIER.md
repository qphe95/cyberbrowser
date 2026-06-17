# Plan: Write Barriers for Concurrent Marking

## Goal
Move the GC **mark phase** onto the existing GC thread pool so it can run concurrently with the mutator (main QuickJS thread), without requiring a full stop-the-world pause during marking.

This document assumes the allocator already knows which objects are "new" (allocated during the current GC cycle) and defers collection of those objects. Even with that safety net, a write barrier is still required for writes **between existing (old) objects**.

---

## Why a Barrier Is Still Required

The "lost object" problem is not about new allocations. It happens when the mutator writes a reference to an **old, unmarked** object into an **old, already-scanned** object while the marker is running.

### Example

1. Roots reach objects **A** and **B**.
2. Marker finishes scanning **A** (now "black" / fully processed).
3. Mutator writes `A.child = C`, where **C** is an existing old object that the marker has not reached yet.
4. Marker finishes scanning **B** and never sees a path to **C**.
5. Sweep frees **C** because it is unmarked.
6. The program later reads `A.child` and dereferences freed memory.

**C was reachable the entire time**, but the marker missed the reference because it was created in an object that had already been scanned.

A write barrier fixes this by either:

- **Marking the target immediately** when the reference is written (Dijkstra / snapshot-at-beginning style).
- **Rescanning the source object** when the reference is written (Yuasa / incremental-update style).

Either approach prevents the marker from freeing live objects.

---

## High-Level Design

### 1. Object Colors

Extend `GCHeader` with a tri-state color for the duration of a GC cycle:

- **White** — not yet marked; candidate for collection.
- **Grey** — marked, but children not yet scanned.
- **Black** — marked and fully scanned.

New objects allocated during marking are **Black** (assumed live for this cycle) and handled by the allocator's existing "new object" logic.

```c
typedef enum {
    GC_COLOR_WHITE = 0,
    GC_COLOR_GREY,
    GC_COLOR_BLACK,
} GCColor;
```

The current single `mark` bit can be expanded into two bits in `GCHeader` (e.g. `gc_color : 2`), or the `flags` byte can be used.

### 2. Grey Work Queue

The mark thread consumes a global work queue of grey object handles. It is produced by:

- Root marking at the start of the GC cycle.
- The write barrier (when it marks/greys a target object).
- New-object initialization (if the allocator does not immediately scan outgoing refs).

The queue must be thread-safe. A simple design:

- One `CRITICAL_SECTION` / `pthread_mutex_t`.
- One `CONDITION_VARIABLE` / `pthread_cond_t` for the mark thread to sleep when empty.
- Or a lock-free Treiber stack if contention becomes an issue.

### 3. Write Barrier Hook

Every site that stores a GC-managed reference into another GC-managed object must call:

```c
void gc_write_barrier(GCHandle source, GCHandle target);
```

**Semantics (Dijkstra barrier — recommended first implementation):**

```c
void gc_write_barrier(GCHandle source, GCHandle target) {
    if (atomic_load_u32(&g_gc.gc_phase) != GC_PHASE_MARKING) return;
    if (source == GC_HANDLE_NULL || target == GC_HANDLE_NULL) return;

    GCHeader *src_hdr = gc_header_from_handle(source);
    GCHeader *tgt_hdr = gc_header_from_handle(target);
    if (!src_hdr || !tgt_hdr) return;

    /* Only barrier writes into black objects. */
    if (src_hdr->gc_color != GC_COLOR_BLACK) return;

    /* If the target is white, mark it now so it survives this cycle. */
    if (tgt_hdr->gc_color == GC_COLOR_WHITE) {
        tgt_hdr->gc_color = GC_COLOR_GREY;
        gc_grey_queue_push(target);
    }
}
```

This is the simplest correct barrier. It can keep some floating garbage alive (overcount), but it never undercount, so it never frees live objects.

### 4. Mark Thread

A dedicated GC job runs the mark phase:

```c
static void gc_mark_job_func(void *arg) {
    (void)arg;

    atomic_store_u32(&g_gc.gc_phase, GC_PHASE_MARKING);

    /* 1. Shade all roots grey. */
    gc_mark_roots_grey();

    /* 2. Drain the grey queue. */
    while (gc_grey_queue_pop(&handle)) {
        gc_scan_object(handle);   /* mark children, turn source black */
    }

    /* 3. Handshake with the mutator to flush any pending barriers. */
    gc_marking_handshake();

    /* 4. Transition to sweep. */
    atomic_store_u32(&g_gc.gc_phase, GC_PHASE_SWEEPING);
}
```

The handshake is required because the mutator may have performed a barrier write but not yet pushed to the queue, or vice versa. A minimal handshake:

- Request the mutator to reach a safepoint briefly.
- At the safepoint, drain the queue one final time.
- No new barrier writes can occur while at the safepoint.

For the first implementation, the handshake can be a short stop-the-world pause at the end of marking. The mutator is still concurrent for the bulk of the mark phase.

### 5. Integration with QuickJS

The hard part is finding every place QuickJS stores a GC reference. The barrier must wrap these writes:

| Category | Examples |
|----------|----------|
| Object properties | `JS_SetPropertyInternal`, `js_define_property`, property descriptor application |
| Arrays | `JS_SetPropertyUint32`, array element writes |
| Prototypes | `JS_SetPrototype` |
| Functions / closures | `JS_DefineProperty` on function objects, captured variable cells |
| Shapes / atoms | Shape transitions that reference atoms or other shapes (read-only during mark) |
| Typed arrays / exotic objects | Any `->data` or internal slot write |

Recommended approach:

1. Define a macro `GC_WRITE_BARRIER(src, tgt)`.
2. Audit `quickjs.cpp` for raw writes of `GCValue`, `JSValue`, `GCHandle`, or `JSObject*` into heap objects.
3. Insert the macro at each write site.
4. For writes performed through helper functions like `set_value`, add the barrier inside the helper.

Because the project uses `GCValue` / `JSValue` wrappers, the barrier can often be placed at the value-assignment helpers rather than at every call site.

### 6. Root Set and Handle Table

During marking, the handle table must remain valid. With the current double-buffered design:

- The mutator allocates into the active buffer.
- The mark thread reads from the active buffer.
- No compaction happens during marking.

The handle table pointer is stable during the mark phase, so no extra synchronization is needed for handle reads. Raw pointers obtained from `gc_deref()` must not be cached across barrier points; the marker already uses handles or re-derives pointers.

### 7. Sweep and Compact

Sweeping and compaction remain stop-the-world phases for the first implementation:

1. **Marking** — concurrent with mutator, protected by write barriers.
2. **Final handshake** — brief pause to drain grey queue and ensure no pending writes.
3. **Sweeping** — single-threaded, mutator paused.
4. **Compaction** — single-threaded, mutator paused, followed by atomic handle-table swap.

This gives most of the latency benefit (marking is the largest phase) while keeping the later phases simple and correct.

---

## Implementation Steps

1. **Extend `GCHeader`** with `gc_color` (White/Grey/Black) and helper accessors.
2. **Implement the grey work queue** with the same native primitives used by the thread pool (`CRITICAL_SECTION` / `CONDITION_VARIABLE` on Windows).
3. **Add `gc_write_barrier()`** and `GC_WRITE_BARRIER()` macro.
4. **Create `gc_mark_job_func()`** and split `gc_run_internal()` into:
   - `gc_mark_phase()` (runs on pool worker).
   - `gc_sweep_phase()` (runs on pool worker or main thread).
   - `gc_compact_phase()` (runs on pool worker or main thread).
5. **Audit QuickJS reference writes** and insert barriers. Start with the most common paths (`JS_SetPropertyInternal`, array stores, prototype changes) and expand.
6. **Add handshake** at end of marking to drain the queue safely.
7. **Tests**:
   - Unit test for barrier coloring: write a white→black reference and verify the target becomes grey.
   - Stress test that allocates and mutates object graphs while repeatedly triggering background marking.
   - Verify the existing 283-test suite still passes.

---

## Risks and Trade-offs

- **Barrier overhead** — every reference write gains a branch and possibly an atomic load. This is the standard cost of concurrent GC.
- **Missing write sites** — any unbarriered write path is a potential use-after-free bug. A complete audit of QuickJS is required.
- **Floating garbage** — the Dijkstra barrier keeps objects alive if a reference was briefly written during marking. This is safe but increases memory pressure.
- **Raw pointers** — if any code caches a raw object pointer across a GC point, concurrent compaction could move the object. The existing handle-based design must be strictly used during concurrent phases.

---

## Summary

Moving marking onto the GC thread pool is feasible, but it requires a write barrier because the mutator can install references to old, unmarked objects into old, already-scanned objects. The recommended first step is a **Dijkstra-style barrier** that marks the target grey immediately, plus a **grey work queue** consumed by the mark thread, with a **brief handshake** at the end of marking to flush pending work before sweeping.