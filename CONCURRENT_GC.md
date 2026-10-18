# Concurrent Pauseless GC: Frame-Based Design

This design replaces `MULTITHREADED_GC.md`, `WRITE_BARRIER.md`, `GREY_DELETE.md`,
`MULTITHREADED_JAVASCRIPT.md` and `CONCURRENT_GC_DESIGN.md`. It is built the way a game
engine splits work between its simulation thread and its worker threads.

- **Frames.** The JS thread runs in ticks, one turn of the event loop each. The GC is a
  state machine on worker threads. The two meet only at tick boundaries.
- **Double buffering.** The GC builds the "next frame" in back buffers (a back handle
  table, to-space regions, next-cycle mark bitmaps). The JS thread presents it with one
  atomic pointer swap at a tick boundary.
- **Dirty flags instead of code barriers.** The operating system records which heap pages
  the JS thread wrote. No load or store in the engine changes.
- **Command buffers.** The GC never edits structures the JS thread owns (hash tables, weak
  maps, the job queue). It records commands in single-producer rings, and the JS thread
  applies them at the next tick boundary.
- **Fences.** Old memory is released a tick after the flip, the way a renderer frees a GPU
  resource only after the fence for the frame that used it has passed.
- **No mutexes, no spinlocks, no waiting.** All coordination uses atomic loads, stores,
  `fetch_add` and CAS. The JS thread never waits for the GC. If the GC isn't ready at a
  tick boundary, nothing happens and the JS thread carries on. Idle GC workers sleep on an
  address (`WaitOnAddress` or futex), which isn't a lock and never involves the JS thread.

## 1. Words that sound blocking but aren't

| Term | Blocks? | Meaning here |
|---|---|---|
| GC read/write barrier | No | A few inline instructions on a load or store. This design has none: page tracking replaces them. |
| Memory fence (`atomic_thread_fence`, acquire/release) | No | Orders memory accesses. Used throughout. |
| Thread barrier (`pthread_barrier_wait`) | Yes | Not used. |
| Mutex, spinlock, condition variable | Yes | Not used in the GC. |

## 2. Why the old double buffer lost data, and what fixes it

`MULTITHREADED_GC.md` had the right shape: copy objects into a second buffer, then make
the copies live with one atomic swap. It failed in three ways. Each one has a fix that
keeps the double buffer.

| Old failure | Fix |
|---|---|
| The JS thread wrote to an object after the GC copied it, so the write was lost at the swap. | Every heap write sets the page's OS dirty bit. At the flip, a copied object whose page was written after its copy is **rejected**: its back-table entry goes back to the old copy. Only clean copies are presented. (§6) |
| Raw pointers (`pc`, `sp`, `gc_deref` results) pointed into the old buffer after the swap. | The swap happens only at a tick boundary, when no JS frames exist, or at a mid-script poll after a stack scan. Objects that a raw pointer on the stack points to are **pinned**: their move is rejected for this flip. (§5) |
| The old buffer was reset right after the swap, while it could still be in use. | Old regions are released one tick later (the fence). (§7) |

The core rule: **nothing the JS thread can still see is changed until it says so at a tick
boundary.** Until the flip, the front table and the old copies are the truth. After it,
the back table is.

## 3. Threads and the tick

```
JS thread:   ──tick──|B|──tick──|B|──tick──|B|──tick──|B|──
                      ^          ^          ^
                      |          |          present: flip back table (O(1) + dirty check)
                      |          finish marking (bounded remark)
                      start marking (snapshot roots)
GC workers:  ═══ mark ═══ mark ═══ sweep ═══ evacuate ═══ prepare back table ═══
```

At every tick boundary `B` the JS thread does one atomic load of the GC's state word. When
the GC has posted a request, it runs that request's boundary step with a **time budget**
(for example 0.5 ms). If the step doesn't fit, it **declines**: it posts "declined" and
returns to JS with nothing changed, and the GC does more work concurrently before posting
again. A boundary step never waits on anything, so the worst case for the JS thread is the
budget.

A tick boundary is where the event loop sits between tasks, when no JS frames exist. The
host has to keep one rule there: **no raw pointers into the GC heap held across the
boundary, only handles** (§9). `cyberbrowser_main` already follows it, because compaction
at the top level works today.

**Long scripts.** A script that runs for seconds produces no tick boundaries. The
interpreter therefore checks the state word at function entry and at loop back-edges,
which is one relaxed atomic load. When memory is tight it runs a boundary step there,
after a conservative scan of the stack (§5) that pins everything the running code points
to. This is the same scan the stop-the-world fix added.

## 4. Data structures

### 4.1 Heap: regions in reserved address space

- One reserved virtual range, split into 1 MB regions and committed on demand.
- On Windows, regions are allocated with `MEM_WRITE_WATCH`, so the hardware tracks dirty
  pages for free (§6).
- Region metadata (all atomics):
  - `state`: FREE, ALLOC, FULL, FROM (being evacuated), TO, HUMONGOUS;
  - `live_bytes`;
  - `alloc_epoch`;
  - an object-start bitmap with 1 bit per 16 bytes.
- **Region allocation:** CAS a FREE region to ALLOC.
- **Object allocation:** a bump pointer in a thread-local buffer (TLAB) carved from the
  region. Plain stores, no atomics.
- Objects larger than half a region get dedicated humongous regions and never move.

### 4.2 Handle table: front and back buffers

```c
typedef struct {
    void **_Atomic front;        /* the table gc_deref reads */
    void **tables[2];            /* both are reserved for the maximum size up front and never move */
    _Atomic uint32_t high_water; /* next never-used handle (fetch_add) */
} HandleTables;

static inline void *gc_deref(GCHandle h) {          /* unchanged in cost: one load + index */
    return atomic_load_explicit(&g_ht.front, memory_order_relaxed)[h];
}
```

- **Reads** go only through `front`. That load can be relaxed: `front` changes only at a
  tick boundary on the JS thread itself, and GC threads synchronize through the state word.
- **The back table** is written by the GC (object moves) and by the JS thread (dual
  writes, below).
- **Dual write:** whenever the JS thread changes an entry (allocating a handle,
  `gc_realloc` moving an object), it stores the new pointer to `front[h]` first and then
  `back[h]`. These are plain stores.
- **GC writes to the back table** are always a CAS against the value it last read. A CAS
  that fails means the JS thread changed the entry, and the GC drops its copy. So neither
  side can overwrite the other's newer value, and nobody takes a lock.
- After a flip, the new back table (the old front) is brought up to date by GC workers:
  `CAS(back[h], back_old, front[h])` for each entry. The CAS combined with the JS thread's
  front-then-back write order makes this safe against concurrent dual writes.
- **Both tables are reserved at maximum size** (for example 2^27 entries × 8 bytes each,
  committed in chunks). There is no `realloc`, so a reader can never see a table being
  freed.

### 4.3 Mark bitmaps: double-buffered by cycle parity

- There are two side bitmaps per region with 1 bit per 16 bytes, `mark[0]` and `mark[1]`.
  Cycle `n` marks into `mark[n & 1]` using `atomic_fetch_or` on whole words.
- The other bitmap holds the finished result of the previous cycle. The weak-table rule in
  §8 reads it, and GC workers clear it, concurrently, before it's reused.
- No colour words live in object headers any more.
- **Allocate-live:** a region records the bump position at mark start (TAMS, top at mark
  start). Objects above it are live this cycle without being marked.

### 4.4 Communication between threads (all single-producer rings, lock-free)

| Ring | Producer → consumer | Carries |
|---|---|---|
| `freed_handles` | GC → JS thread | Handles freed by sweep. The JS thread refills its local handle cache from here, or calls `fetch_add` on `high_water`. |
| `commands` | GC → JS thread | Weak-table removals, atom-table removals, finalizer jobs, FinalizationRegistry jobs. Applied at the tick boundary. |
| `roots_log` | JS thread → GC | Changes to off-heap root containers (job queue enqueues, `gc_add_root`) since the last boundary. |
| per-worker deques | GC worker ↔ GC workers | Marking work, using Chase-Lev work stealing (lock-free). |

A ring is an array with a `head` written only by the consumer and a `tail` written only by
the producer, using release stores and acquire loads. A ring that fills up grows by
handing over a bigger array in a pointer swap, like a double-buffered command list, so a
producer never waits.

The state word, an atomic 64-bit value, holds `{cycle, phase, request, ack}`. The GC posts
a request; the JS thread acknowledges with done or declined. Each field is written by only
one side.

The existing thread pool's job queue uses a mutex and a condition variable. The GC gets
its own workers that sleep on an address instead.

## 5. Roots and pins (implemented in the stop-the-world fix)

- **Precise roots:** every frame on the runtime's frame chain (locals, arguments, operand
  stack), the runtime, contexts, the current exception, and the registered off-heap root
  containers.
- **Conservative roots:** the JS thread's C stack and registers, spilled with `setjmp`.
  A 32-bit word equal to a live handle, or a pointer into a live object, keeps that object
  alive. Pointers are resolved through the object-start bitmap.
- **Pins:** at a mid-script flip, every object that a raw pointer on the stack points into
  is pinned. Its move is rejected even if its page is clean. At a normal tick boundary
  there are no JS frames, so almost nothing is pinned.

## 6. Dirty pages: the "dirty flag" mechanism

There is no barrier code. The OS reports which pages were written since the last reset.

| Platform | Mechanism | Cost |
|---|---|---|
| Windows | `VirtualAlloc(MEM_WRITE_WATCH)`, then `GetWriteWatch(WRITE_WATCH_FLAG_RESET, ...)` | Hardware-tracked; no faults |
| Linux | Soft-dirty bits: clear with `/proc/self/clear_refs`, read with `/proc/self/pagemap` | One syscall per batch of pages |
| macOS, Android, Linux without soft-dirty | `mprotect` read-only plus a fault handler that marks the page and unprotects it (as the Boehm collector does) | One fault per page per cycle |

Only the GC state machine resets or reads the OS dirty state, plus the JS thread during
its own boundary step, while the GC is guaranteed to be idle (§3). So there's a single
reader at any moment and no locking. The GC merges what it reads into its own dirty
bitmaps:

- `dirty_mark`: pages written since marking last scanned them;
- `dirty_evac`: pages written since evacuation copied their objects.

**Ordering argument for evacuation.** Take one from-space page P.

1. The GC resets P's dirty state.
2. Then it copies the objects in P.
3. Any JS write to P after step 1 marks P dirty.
4. Any JS write before step 1 is visible to the copy in step 2, because the reset has
   acquire semantics (for `GetWriteWatch` and the `mprotect` fault, it's a system call
   boundary).
5. So if P is still clean at the flip, the copy equals the original.

## 7. One collection cycle

Each step says who does it and whether the JS thread does any work.

**1. Mark start** (boundary step, budgeted):
- the JS thread snapshots the off-heap roots and the C stack;
- the GC flips mark parity and records TAMS for every region;
- the GC resets the OS dirty state for the whole heap.

**2. Concurrent mark** (GC workers):
- Trace from the snapshot using work stealing, setting bits in `mark[cycle & 1]`.
- Each round, pull the OS dirty state into `dirty_mark`. Re-scan every marked object, or
  object above TAMS, that overlaps a dirty page.
- Repeat until a round finds few dirty pages.

**3. Remark** (boundary step, budgeted):
- the JS thread pulls the final dirty pages, the `roots_log` and its stack;
- it traces from those, up to the budget.
- If it runs out of budget, it declines and the GC does another concurrent round.
- **Marking is complete only when a remark finishes inside its budget.** Nothing has been
  committed before that, so declining costs only time.

**4. Concurrent sweep** (GC workers):
- Walk the handle table. An entry whose object is unmarked and below TAMS is dead:
  - its handle goes to `freed_handles`, after both table entries are CAS'd to NULL;
  - its bytes come off the region's `live_bytes`;
  - finalizer, weak-table and atom-removal commands go to `commands`.
- Regions with zero live bytes become FREE, through the fence in step 8.

**5. Pick the collection set** (GC):
- Choose the sparsest regions and mark them FROM.

**6. Concurrent evacuation** (GC workers):
- Work page by page: reset P's dirty state, then copy every live object that starts in
  P into TO regions. Resetting just before copying avoids rejecting good copies because
  of writes that happened before the copy. Linux soft-dirty can only reset the whole
  process, so there the GC resets once and re-copies dirty pages in rounds.
- `CAS(back[h], old, new)`; if it fails, the JS thread changed the entry, so drop the copy.
- Keep pulling dirty state into `dirty_evac`. Objects on dirtied pages are re-copied (reset
  the page, then copy again) or simply rejected, depending on a re-copy budget.
- When done, post FLIP_READY and stop touching evacuation state.

**7. Flip** (boundary step, budgeted):
- pull the last dirty pages for FROM regions;
- reject each object on them, and each pinned object: `back[h] = front[h]`;
- `atomic_store(&front, back)`, with release ordering;
- post FLIP_DONE with the current tick number.
- **The JS thread's work** is the rejects plus one pointer store. That's usually a handful
  of objects, because the GC already rejected most dirty ones concurrently.

**8. Fence and release** (GC):
- When the JS thread's tick counter has passed the flip tick, nothing can hold a pointer
  into the FROM regions:
  - at a tick boundary there are no raw pointers;
  - at a mid-script flip, pinned objects weren't moved.
- Objects whose move was rejected stay alive in place: their region goes back to FULL, not
  FREE. Every other FROM region is released (decommitted in debug builds, so a stale
  pointer faults immediately).
- Then GC workers bring the new back table up to date (§4.2).

Before applying the step-4 commands, the JS thread checks the previous cycle's bitmap for
each target. An object that's marked again in a newer cycle is skipped, so a command can't
act on something that's live now.

## 8. Weak structures

These are the atom hash, shape hash, WeakMap, WeakRef and FinalizationRegistry.

- **During marking**, a lookup that returns an object sets its bit in the current mark
  bitmap (`atomic_fetch_or`). The JS thread can't take hold of something the marker
  already passed over without also keeping it alive.
- **After remark and before its command is applied**, an entry whose object is unmarked
  in the finished cycle counts as missing. The lookup reads the previous-parity bitmap,
  with no locks.
- **Removals happen only on the JS thread**, by applying `commands` at a tick boundary. A
  hash table is never modified by two threads at once.

## 9. Rules for engine and browser code

1. **Raw pointers to GC memory stay in locals.** Never store one in a global, a malloc'd
   struct or a GC object, and never hold one across a tick boundary. Store handles.
2. **Off-heap structures that hold handles** (the job queue, browser structs) report
   changes through `roots_log`, or are registered so they're re-snapshotted at remark.
3. **Only the dual-write helpers change handle-table entries** (`gc_alloc`, `gc_realloc`).
   Nothing writes `g_gc.handles.ptrs[...]` directly; the shape-clone path does today and
   must change.
4. **Finalizers run on the JS thread**, from `commands`.

## 10. Correctness invariants

| ID | Invariant | Mechanism |
|---|---|---|
| I1 | Every object reachable at remark is marked, or was allocated above TAMS. | Root snapshot, dirty-page re-scan, remark completes inside its budget |
| I2 | A handle is reused only after both table entries are NULL and the handle went through `freed_handles`. | Sweep, ring |
| I3 | `front` changes only on the JS thread, at a boundary. | Flip is a boundary step |
| I4 | A presented copy equals the original at the moment of the flip. | Dirty reset before copy, reject dirty or pinned at the flip |
| I5 | A FROM region is released only after the tick that follows its flip. | Tick-counter fence |
| I6 | No thread ever waits on another to make progress. | Budgets, declines, single-writer fields, CAS |
| I7 | A weak entry is never handed out after its object was found dead. | Bitmap check on lookup |

## 11. What the JS thread pays

| Cost | When |
|---|---|
| One relaxed load per `gc_deref` (same as today) | Always |
| One relaxed load of the state word | At each tick boundary, function entry and loop back-edge |
| Dual store when an entry changes | Allocating a handle, `gc_realloc` |
| OS dirty tracking | Free on Windows; one fault per page per cycle with `mprotect` |
| Boundary steps (mark start, remark, flip) | Up to the budget, three times per cycle; declined instead of exceeded |

## 12. Testing

- **Torture:** request a boundary step at every poll. The hook already exists from the
  stop-the-world fix. Run the C test suite and JS regression scripts under it.
- **Verifier (debug):** after a remark is accepted, redo marking from scratch in a scratch
  bitmap. Any object marked there but not in the cycle's bitmap is a bug in dirty
  tracking or roots.
- **Poisoned FROM regions (debug):** decommit released regions, so a stale raw pointer
  faults at the culprit.
- **Concurrency harness:** an extended `conc.c` from the audit. Allocate and write
  constantly across thousands of cycles and check contents and canaries.
- **Reject-path testing:** force every copy to be rejected, and then none, to exercise
  both flip paths.
- **ThreadSanitizer** on Linux, with the `mprotect` and soft-dirty back ends.
- **Telemetry:** record each boundary step's duration, and whether it was declined, in the
  `cyber_profile` traces.

## 13. Implementation plan

**1. Foundations**
- Reserved-region heap, TLABs, start bitmaps.
- The two fixed handle tables with dual writes.
- The state word and rings.
- GC workers that sleep on an address.
- Remove the old double buffers, the lock-free free queue, the grey queue mutex, publish
  states and the dead list.
- Collection stays stop-the-world at tick boundaries.
- *Done when:* the tests pass under torture and nothing commits 8 GB at startup.

**2. Dirty tracking**
- Windows write-watch, then Linux soft-dirty, then the `mprotect` fallback.
- Unit tests of the reset-then-copy ordering.

**3. Concurrent mark and sweep**
- Parity bitmaps, TAMS, budgeted remark, command buffers, weak lookups.
- *Done when:* the verifier is clean under torture.

**4. Concurrent evacuation and flip**
- Back-table CAS, reject logic, fence release, back-table update.
- *Done when:* the harness runs thousands of cycles clean with poisoning on.

**5. Pacing**
- Start cycles from the allocation rate. When the heap is nearly full, the allocating
  thread does GC work itself instead of aborting.
- Tune region size and budgets.
