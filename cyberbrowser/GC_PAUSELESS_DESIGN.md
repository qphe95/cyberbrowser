# A handle-free, pauseless, concurrent collector for cyberbrowser

Status: proposal. Replaces the handle-table collector in `src/quickjs/` (`gc_concurrent.c`,
`gc_collector.c`, `quickjs_gc_unified.c`).

## Why

Measured on a YouTube watch-page load (es5 bundle, 17.3 s wall, 11.1 s of JS-thread CPU):

| JS-thread cost | Time |
|---|---|
| Handle lookups inlined into the engine (`gc_deref`, `handle_header`, frame/atom lookups) | ~1.3 s |
| Dual writes to the front and back handle tables | 0.49 s |
| Waiting for the GC thread (`gc_os_wait`) | 0.43 s |
| Boundary handshakes | 0.40 s |
| Allocation | 0.38 s |
| OS write-watch pulls | 0.29 s |
| Handle-table flip (`step_flip`) | 0.25 s |
| Conservative stack scans | 0.25 s |
| Marking, realloc, other | ~0.7 s |

About 40% of the JS thread goes to memory management. The collector thread adds about 11 s of
cycle time per load: 150 minor cycles averaging 52 ms and 11 major ones. The open YouTube
modern-bundle crash only happens with handle reuse plus the background collector.

The load allocates about 600 MB (150 minor cycles × 4 MB), roughly 12 million objects, at about
30 ns per allocation. A bare bump allocation is about 3 ns. The rest is per-object bookkeeping
(handle allocation, dual writes, a start-bitmap bit, an atomic publish of the region top) and
cache misses on fresh memory.

Most of these costs exist to support moving objects concurrently behind a handle table. This
design keeps concurrency and generations, and drops moving.

## Goals

- **No handles.** A reference is the object's address, stored compressed in 32 bits so that
  `GCValue` and every struct field keep their size.
- **Pauseless.** The JS thread never waits for the collector. Synchronous work is limited to two
  short handshakes per cycle. Each scans only the topmost frame and the C stack above the
  outermost JS entry, with a target under 50 µs.
- **Concurrent** marking and sweeping on the collector thread.
- **Generational without moving:** sticky mark bits (section 5) reclaim short-lived objects every
  few MB of allocation, so the allocator keeps reusing memory that's still in cache.
- **No per-object bookkeeping on allocation:** a bump, nothing else in the common case.
- **Cheaper than today on every JS-thread line** (section 10).

## Non-goals (v1)

- **Compaction and evacuation.** Objects never move. The line/hole heap (section 3) keeps
  fragmentation down without it.
- **Several mutator threads.** The JS heap has exactly one mutator, the JS thread.
  `gc_thread_pool_*` and `js_object_prealloc_properties` have no callers outside the engine.
  Network, image and font threads never touch JS objects.
- **Precise rooting of native code.** The C stack above the outermost `JS_Call` is still scanned
  conservatively (section 6). Removing that scan would need V8-style handle scopes in about 270
  binding sites plus the runtime; the scan costs under 0.2 ms per load.

## 1. How Chrome does it, and what carries over

Chrome (V8 for JS objects, Oilpan for Blink's C++ objects) doesn't eliminate stack scanning or
allocation cost; it makes them cheap:

| | Chrome | This design |
|---|---|---|
| JS frames | Precise: interpreter frames hold tagged values, optimized code has safepoint tables | Frames are heap objects with known layouts. Only the top frame is scanned synchronously; a return barrier covers the rest (section 6) |
| Native code holding JS objects | V8: `HandleScope`/`Local<>`, an explicit root stack, so no C++ stack scan for JS objects. Oilpan (Blink objects): conservative native stack scan | Conservative scan of the C stack above the outermost JS entry: a few KB, 1–5 µs |
| When stacks are scanned | Inside pauses: the young-generation scavenger stops the world, as does the final step of major marking | Two handshakes per cycle, top frame and bounded C stack only |
| Allocation | Inline bump in a moving young generation, a few instructions | Inline bump into line holes; no per-object bookkeeping (section 3) |
| Short-lived objects | Scavenger: cost proportional to survivors, nursery reused while warm | Sticky-mark-bit young generation: young lines reclaimed every few MB, reused while warm (section 5) |
| Allocation removal | Optimizing compiler: escape analysis, allocation folding | No JIT. The interpreter reuses its own temporaries instead (section 4) |
| Pauses | Short, not zero; GC work scheduled into idle time between frames | No pauses beyond handshakes |

## 2. References

`GCRef` replaces `GCHandle`, and is still a `uint32_t`:

```c
typedef uint32_t GCRef;                    /* 0 = null */
static inline void *gc_ptr(GCRef r)  { return r ? g_heap_base + ((size_t)r << 4) + GC_HEADER_SIZE : NULL; }
static inline GCRef gc_ref(const void *p) { return p ? (GCRef)(((uint8_t *)p - GC_HEADER_SIZE - g_heap_base) >> 4) : 0; }
```

- Headers are 16-byte aligned (`GC_GRANULE`), so 32 bits address 64 GB. The heap reserve is
  16 GB.
- `gc_ptr` is an add and a shift with no memory load. It replaces `gc_deref`, which loads
  `high_water`, loads `front`, then loads the table entry. `g_heap_base` is a constant after
  startup and stays in a register in hot loops.
- `GCValue` keeps its layout (tag plus 32-bit payload). `GC_VALUE_GET_HANDLE` becomes
  `GC_VALUE_GET_REF`.
- Because objects never move, a raw pointer obtained from `gc_ptr` stays valid for as long as
  the object is reachable. The "never store the result of gc_deref()" rule goes away.
- Identity is address equality. There's no handle reuse, so the class of bug behind the
  modern-bundle crash can't exist. A dangling reference is still possible, but debug builds can
  catch it with quarantine and poisoning (section 13).

## 3. Heap layout: lines and holes (Immix-style), bump allocation without moving

Per-size-class free lists would cost more than today's bump allocator: pointer chasing, more
pages in use and a scattered allocation order. Instead the heap is the non-moving form of
**Immix** (Blackburn & McKinley, 2008), which keeps bump allocation.

- Address space is reserved once, as now, in 1 MB **regions**. Each region is split into 32 KB
  **blocks** of 256 **lines** of 128 bytes.
- **Small and medium objects (≤ 8 KB)** are bump-allocated into **holes**: runs of free lines.
  An object may span lines.
- **Large objects (> 8 KB)** take whole blocks, and humongous objects (> 256 KB) their own
  regions, as now.
- **Marking** sets the object's bit in a side **mark bitmap** (one bit per 16 bytes) and the
  **line-mark bytes** of the lines it covers. Neither touches object memory, so marking doesn't
  dirty object pages.
- **Sweeping** reads only line marks. A block's free lines become its holes, and the block is
  published as `SWEPT`. The sweeper never touches objects either.
- **Conservative line marking:** a small object (< 128 bytes) marks only its first line, and a
  line counts as live if it or the previous line is marked. This is standard Immix, and it
  saves computing every object's end while marking.

### The allocation fast path does nothing but bump

```c
static inline void *gc_alloc(size_t total) {         /* 16-byte multiple, header included */
    uint8_t *p = g_alloc.cursor;
    if (likely(p + total <= g_alloc.line_end)) {       /* stays inside the current line run */
        g_alloc.cursor = p + total;
        return p;
    }
    return gc_alloc_cross(p, total);   /* crossing a line, or the hole is exhausted */
}
```

There's no other per-object work:

- **No handle and no table write:** the reference *is* the address.
- **No start bit per object.** Finding an object from an arbitrary word (the conservative C-stack
  scan, and debug checks) uses a **per-line first-object byte** instead. It holds the offset, in
  16-byte granules, of the first object header that starts in the line, or a "continued" marker
  when no object starts there. The byte is written only by `gc_alloc_cross`, when an allocation
  enters a new line. That's at most one store per 128 bytes allocated, and none for objects that
  stay inside a line.
  - To resolve a candidate address: read its line's byte, step back over "continued" lines to
    the line where an object starts, then walk headers forward (by size) to the object that
    covers the address. Lines are 128 bytes, so the walk is a few headers.
- **No atomic publish.** The collector never reads objects in the hole the JS thread is filling.
  Objects allocated during marking are black: the hole's lines are marked when the JS thread
  takes the hole, not per object.
- **No per-object counter.** The allocation counter that triggers cycles advances once per hole,
  in `gc_alloc_cross`.

More details:

- **Overflow allocation:** an object bigger than a line that doesn't fit the current hole goes
  to a separate overflow cursor in an empty block, rather than skipping the hole. Without this,
  Immix wastes space on medium objects.
- **Zeroing:** the JS thread zeroes each line as it enters it in `gc_alloc_cross`, so the line
  stays in its own cache. Today the collector thread zeroes whole regions, which leaves them in
  another core's cache. Constructors that initialise every field (most of QuickJS) use
  `gc_alloc_noinit`, which skips the zeroing.
- **Warm reuse:** the young generation (section 5) reclaims young lines every few MB of
  allocation, and the slow path prefers holes from the most recent minor cycle. The allocator
  therefore cycles through a few MB that are still in L2/L3, instead of walking tens of MB of
  cold memory.
- **Header:** 8 bytes as now (type, size, flags). Without the handle field there are 4 spare
  bytes.
- `ArrayBuffer` storage (`JS_GC_OBJ_TYPE_BYTES`) never moved anyway, so nothing changes for it.

## 4. Allocating less

The cheapest allocation is one that doesn't happen. Without a JIT there's no escape analysis,
but the interpreter can avoid its own temporaries:

- **In-object properties:** `JS_NewObjectFromShape` currently makes two allocations, the object
  and its property array. Objects get up to 4 property slots inside the object, as V8 does (the
  shape records which properties are inline), and grow out of line on demand. That halves
  allocations for the common small object.
- **Frames:** already pooled (28b22d9), so plain calls allocate nothing.
- **Var refs** for a frame's captured variables are allocated in one batch at entry, not one
  object each.
- **Iterator results:** `for-of` over arrays, `Map`, `Set` and strings uses the built-in
  iterators' internal `next` directly (`OP_for_of_next` already special-cases some of this),
  never materialising `{value, done}`. For generators and user iterators the result object is
  still created, since script can see it.
- **`arguments`:** functions that use `arguments` only as `arguments.length` or `arguments[i]`
  (detected at compile time, no escape) read the frame's argument slots and never create the
  object.
- **Spread and rest arrays:** `f(...args)` with an array argument passes the array's storage as
  `argv` instead of copying into a new array; a rest parameter that's only indexed or iterated
  uses the frame's slots.
- **Promise reactions:** a reaction record that settles immediately (an `await` of an
  already-resolved promise) is a stack-allocated job, not a heap object.

Each item is measured on its own, using the allocation count per load that the allocator now
reports (one counter per hole, section 3).

## 5. Generations with sticky mark bits

Objects never move, so the young generation isn't a separate space. It's **the set of objects
allocated since the last cycle**: they're unmarked, and everything that survived a cycle keeps
its mark bit (the bit "sticks"). This is the non-moving generational scheme used by
sticky-mark-bit Immix and by several JVM and .NET collectors.

- **Minor cycle:** mark from the roots and from the **remembered set** (old objects that had a
  young reference stored into them), treating already-marked (old) objects as done. Only young
  objects get traced, and only young lines are swept. Survivors stay marked, which promotes
  them.
- **Major cycle:** clear all marks (by flipping the mark polarity, not by touching the
  bitmaps), then mark everything. It runs when the old generation has grown by `1.0 × live`
  since the last major cycle.
- **Trigger for minors:** every 8 MB of young allocation. That matches today's 4 MB nursery in
  spirit, but the cycle is cheap: it traces survivors, not the whole heap.
- **Remembered set:** a **card table** with one byte per 512 bytes of heap, marked by the store
  barrier (section 6). A minor cycle scans dirty cards in old blocks, then clears them.
- Major and minor cycles are both concurrent and use the same handshakes.

Expected collector-thread work: a minor cycle costs about as much as the surviving bytes plus
the dirty cards, typically 1–3 ms. That's in line with what Chrome's scavenger does with a
similar nursery, and far below today's 52 ms per minor cycle.

## 6. Barriers

Two jobs, both at heap reference stores:

- the **remembered set** for minor cycles, all the time;
- the **SATB deletion barrier** for marking, only while a cycle marks. When a reference in a heap
  object is overwritten, the old value is recorded. Objects allocated during marking are black.
  Together these guarantee that everything reachable at the snapshot gets marked, so new
  references never need shading: their targets are either new (black) objects or were reachable
  at the snapshot.

```c
static inline void gc_store_ref(GCRef *slot, GCRef v) {
    g_cards[((uint8_t *)slot - g_heap_base) >> 9] = 1;      /* unconditional byte store */
    if (unlikely(g_marking)) gc_satb_record(*slot);          /* old value only */
    *slot = v;
}
```

The card store is unconditional: one store without a branch, which is cheaper than testing
whether the object is old. Cards in young blocks are simply ignored by minor cycles.

What keeps the barrier cheap:

- **Initialising stores take no barrier at all.** A field written for the first time in a newly
  allocated object has no old value to record (it's null or a non-reference). The object is
  young, so no card is needed either. These stores use `gc_init_ref`, a plain store:
  constructors, `JS_NewObjectFromShape`, array literals, closure creation,
  `js_create_from_ctor`. Most heap pointer stores on this workload are this kind.
- **Only reference-tagged old values matter.** For `GCValue` slots, the slow-path test is
  `g_marking && GC_VALUE_IS_REF(old)`, so ints, floats, booleans and undefined skip it.
- **The interpreter keeps `g_marking` in a register.** It changes only at a poll (section 7), so
  `JS_CallInternal` reloads it into a local at each poll point. The marking test is then a
  register test and a predicted-not-taken branch.
- **Frames take no barrier.** Interpreter stores to locals and the operand stack are plain
  stores. Running frames are roots, covered by the handshakes and the return barrier (section
  7). A frame that outlives its call (closures, generators) becomes an ordinary heap object when
  it leaves the chain. `js_rt_pop_stack_frame` has a hook there: it marks the frame's cards, and
  during marking it records the frame's reference slots once. This replaces today's
  `gc_dirty_range`.
- **The SATB slow path is inline and needs no atomics:** `*g_satb.top++ = old; if (top == end)
  flush()`. A full buffer goes to the collector through a single-producer queue, and the JS
  thread never touches mark bits.

Outside the interpreter (the runtime in C and the browser bindings), `g_marking` is a global
byte that stays in L1 during marking. Outside marking the branch is never taken.

**Native-side holders** (`g_dom.wrapper`, `g_foreign`, timers, fetch requests, `GCValueVec`)
are roots, through `gc_add_root` or engine-root enumeration. The collector reads them at the
start handshake, and the barrier protects them afterwards: their setters use `gc_store_ref` on
the root slot.

What the barriers replace: OS write watch, software dirty cards, the ref log, remark rescans and
the flip.

## 7. Handshakes, polling and stack scanning

Handshakes are rare and short.

- **Polls only at back edges and function entry.** Today `gc_poll` runs on *every bytecode
  instruction*. Instead, it goes where upstream QuickJS polls interrupts: loop back edges
  (`OP_goto`, `OP_if_*` backwards), function entry, and calls into native code. It's merged with
  the interrupt countdown into one poll word:

  ```c
  if (unlikely(g_poll_word))      /* nonzero: interrupt due, or a handshake requested */
      js_poll_slow(ctx);
  ```

  The collector requests a handshake by setting a bit in `g_poll_word`, with no OS wait. The JS
  thread handles it at its next poll, which is at most one loop iteration or call away.
- **Lazy frame scanning with a return barrier.** The start handshake scans only the **topmost
  frame** and the **C stack**, then sets a stack watermark at the frame below. The frames below
  the watermark aren't running (they're suspended callers), so **the collector scans them
  concurrently**, top-down.
  - When the JS thread returns into a frame below the watermark, it checks
    `frame->scan_epoch != g_epoch` (one compare in the return path) and, if the collector hasn't
    reached that frame yet, scans it itself before continuing. This is the stack-watermark
    technique ZGC and V8 use.
  - Frames already live on the GC heap, so the collector can read them in place.
  - Frames entered after the start handshake, including pooled frames reused for new calls, are
    stamped with the current epoch on entry. Everything in them came from the already scanned
    top frame, from new objects, or from the heap under the barrier, so they never need scanning.
- **C stack scan.** A conservative scan of the C stack is still needed, because native code holds
  JS objects in locals. For example, a native DOM method can take a node out of its parent and
  run JS before inserting it again, leaving the node reachable only from a C local. Only the
  part between the current stack pointer and the outermost `JS_Call` entry is scanned: the
  browser's main loop below that holds no JS references. That part is usually a few KB, about
  1–5 µs, and each word is resolved through the per-line first-object bytes (section 3).
- **Finish handshake:** flush the JS thread's SATB buffer and, if the collector has drained
  everything (stack frames included), clear `g_marking`. Otherwise the collector keeps marking
  and asks again later. The JS thread never waits.

Expected: about 2 handshakes per cycle at 5–50 µs each. With minor cycles every 8 MB, that's
roughly 75 minor and a few major cycles per load, about 150 handshakes, under 5 ms in total.
Removing the per-instruction `gc_poll` also takes a load out of the interpreter's dispatch loop.

## 8. Cycle

```
idle ─ trigger ─► [start handshake] ─► concurrent mark ─► [finish handshake] ─► weak processing ─► concurrent sweep ─► idle
                   top frame + C stack   frames below watermark,     SATB flush
                                         heap (minor: young + cards),
                                         SATB buffers
```

1. **Trigger:** see section 5. The collector watches the allocation counter, which the JS
   thread updates once per hole.
2. **Start handshake:** set `g_marking` and the epoch, scan the top frame, the C stack, engine
   roots and the root registry, then set the watermark. A minor cycle also snapshots the dirty
   cards.
3. **Concurrent mark:** a mark stack plus `mark_children` per type, as now, reading through
   `gc_ptr`. The collector also scans suspended frames below the watermark and drains SATB
   buffers. A minor cycle skips objects that are already marked (old).
4. **Finish handshake:** see section 7.
5. **Weak processing**, on the collector thread, with the JS thread told at a safepoint:
   - ephemerons (WeakMap/WeakSet): iterate to a fixed point during marking, as V8 does;
   - WeakRef: clear dead targets;
   - FinalizationRegistry: queue cleanup jobs;
   - the shape hash: remove dead entries;
   - native finalizers (DOM wrappers, foreign objects): run on the JS thread at its next poll,
     before any block containing their objects is reused. Blocks with pending finalizers stay
     `SWEEPING` until the JS thread acknowledges.
   - Weak tables keep the rule they follow today: a lookup that hits an unmarked entry during a
     cycle must shade it (a `gc_satb_record` of the entry) or treat it as missing. The SATB
     snapshot makes "shade on read" sufficient.
   - Minor cycles handle weak entries whose targets are young; old targets wait for a major.
6. **Concurrent sweep:** build each block's holes from its line marks (minor cycles: young
   blocks only), then publish the block as `SWEPT`. Fully empty blocks go back to the pool.

## 9. Things that move today and must stop moving

`gc_realloc` keeps a handle stable while moving the payload. Without handles, the 13 call sites
in `quickjs.c` have to change in one of two ways. The sites are shapes and property arrays in
`resize_properties`, `compact_properties` and `add_property`, plus fast arrays, string buffers
and `gc_data_array_resize`.

- **Out-of-line storage** (the default): the object keeps a fixed header that points to a
  separate buffer. The buffer is reallocated and the pointer updated through `gc_store_ref`.
  Property arrays already work this way (`prop_handle`), and so can fast-array values and string
  buffers.
- **Shapes:** a resize returns a *new* shape, which is assigned to the one object being changed,
  as upstream QuickJS does. Hashed (shared) shapes are already immutable and cloned before any
  change (`js_shape_prepare_update`, `add_shape_property`). So only unshared shapes are ever
  resized, and each has a single owner.

## 10. Expected effect on the JS thread

| | Now | After | Why |
|---|---|---|---|
| Handle lookups | ~1.3 s | ~0 | `gc_ptr` folds into address arithmetic |
| Dual writes, flip, write watch, OS waits | ~1.5 s | 0 | no handle tables, no moving, no write watch |
| Boundary work and polling | 0.40 s (+ a load per instruction) | < 5 ms | ~150 handshakes of 5–50 µs; polls only at back edges and calls |
| Stack scans | 0.25 s | < 1 ms | top frame and the C stack above the JS entry only; the rest is concurrent |
| Barrier | none (OS write watch) | ~0.05–0.1 s | initialising stores unbarriered; unconditional card byte; register test for marking |
| Allocation | 0.38 s | ~0.05–0.1 s | bump only, no per-object bookkeeping; warm young lines; fewer allocations (section 4) |

That's a net of about 4 s off this load's 11.1 s of JS-thread CPU, more than a third of it. The
floor for allocation is the memory traffic of the bytes actually allocated, which section 4
lowers. On the collector thread, minor cycles should drop from about 52 ms to 1–3 ms each.

## 11. Migration plan

Each phase ends with the full test suite passing and a set of record/replay YouTube loads
(`CYBER_RECORD`/`CYBER_REPLAY`): node counts within 2% of the baseline, and GC verify and poison
runs clean.

| Phase | Work | Gate |
|---|---|---|
| **0. Barrier audit** | Route every heap reference store through `gc_store_ref` or `gc_init_ref`; today some go through `SAFE_SET` macros and raw struct writes. Add a debug checker: with `QJS_GC_BARRIER_CHECK=1`, the collector compares each scanned object with a shadow copy and reports unbarriered changes. | Checker clean on the suites and on replays |
| **1. Polling** | Poll at back edges, function entry and native calls only, through one poll word shared with the interrupt countdown (section 7). Independent of the rest, and a speedup on its own. | Interrupt test passes; benchmark faster |
| **2. Allocating less** | In-object properties, batched var refs, iterator results, `arguments`, spread/rest, immediate promise reactions (section 4), one at a time. Independent of the collector. | Allocation count per load drops; suites pass |
| **3. Stop moving** | Disable nursery evacuation and compaction, and turn the `gc_realloc` sites into out-of-line storage or new-shape assignment (section 9). Handles still exist, but the table never changes after allocation. | Replays match baseline; GC verify clean |
| **4. Line/hole heap** | The Immix allocator with per-line first-object bytes (section 3). The stop-the-world collector sweeps into holes. | Allocation microbenchmark at least as fast as today's bump |
| **5. References** | `GCHandle` → `GCRef`, `gc_deref` → `gc_ptr`. Mostly mechanical in `quickjs_gc_unified.h` and the `SAFE_*` macros. Remove the handle tables, `handle_allocate` and dual writes. | YouTube JS-thread CPU drops by ≥ 1.5 s |
| **6. Concurrent collector** | Handshakes, return barrier, SATB buffers, black allocation, concurrent mark and sweep (sections 6–8), major cycles only at first. Delete the flip, write-watch and ref-log code in `gc_concurrent.c`. | Longest handshake < 50 µs; no JS-thread waits in the profile |
| **7. Generations** | Sticky mark bits, card table in the barrier, minor cycles (section 5). | Minor cycles ≤ 3 ms; allocation ≤ 0.1 s in the profile |
| **8. Weak structures and finalizers** | Port the weak step to the new cycles (section 8, step 5). | WeakMap/WeakRef/FinalizationRegistry tests; modern-bundle replays clean |

Phases 0–4 are worth having even if the project stops there: they remove the moving collector's
correctness risks and speed up allocation. Phase 5 brings most of the speed.

## 12. Risks

- **Missed barriers** mean freed live objects, the hardest bugs to find. Phase 0's checker and
  the stale-reference tools (section 13) are the mitigation; land phase 0 first.
- **Misclassified initialising stores.** `gc_init_ref` is only correct for a slot whose old value
  holds no reference, in an object allocated since the last minor cycle (so no card is needed).
  The barrier checker verifies both: the shadow copy also records the allocation epoch.
- **Floating garbage across generations.** A young object referenced only from an old object
  that has died survives minor cycles until the next major cycle. Normal for sticky-mark-bit
  schemes; watch old-generation growth.
- **Fragmentation without compaction.** Immix line reuse keeps it modest in published results,
  but watch `live / committed`. If it's worse in practice, add opportunistic evacuation of
  sparse blocks (see Later).
- **The return barrier** has to cover every way a frame resumes: return, exception unwind, and
  generator or async resumption. Missing one leaves a suspended frame unscanned.
- **C stack scan bound.** Code that enters JS from deep native recursion (DOM mutation
  callbacks, for example) scans more; the bound is still the stack above the outermost
  `JS_Call`.
- **Weak-table reads during marking** must shade or ignore (section 8, step 5). The shape hash
  and WeakRef `deref` are the known readers.
- **Allocation-avoidance changes** (section 4) alter observable behaviour if their escape
  checks are wrong (`arguments` aliasing, iterator protocol overrides). Each needs spec tests
  for the patterns it must bail out on.

## 13. Debugging support to keep or port

- `CYBER_RECORD` / `CYBER_REPLAY` for repeatable page loads.
- `QJS_GC_HISTORY` becomes per-slot history: allocation and free cycle, type, previous type.
- `QJS_GC_NO_REUSE` becomes **quarantine**: freed lines stay unallocatable for N cycles and are
  poisoned, and a debug `gc_ptr` checks the object's mark/allocation state.
- `QJS_GC_VERIFY`: a stop-the-world reachability check after marking (debug only).
- `QJS_GC_BARRIER_CHECK` (phase 0).

## Later

- **Precise rooting of native code** (V8 `HandleScope`-style) if the bounded C-stack scan ever
  shows up in profiles. It would remove conservative scanning entirely.
- **Opportunistic evacuation of sparse blocks** at a handshake, as in full Immix, with a
  forwarding check scoped to the blocks being evacuated.
