# Concurrent Pauseless GC: Frame-Based Design

This design replaces `MULTITHREADED_GC.md`, `WRITE_BARRIER.md`, `GREY_DELETE.md`,
`MULTITHREADED_JAVASCRIPT.md` and `CONCURRENT_GC_DESIGN.md`. It is built the way a game
engine splits work between its simulation thread and its worker threads. Sections 1–12
describe the collector as implemented; §13 covers testing, §14 measurements and §15 what
is left.

- **Frames.** The JS thread runs in ticks, one turn of the event loop each, and polls
  between opcodes. The GC is a state machine on its own thread. The two meet only at
  boundary steps: the GC thread posts a request, the JS thread runs it at its next poll
  or tick.
- **Double buffering.** The GC builds the "next frame" in back buffers (a back handle
  table, to-space regions, next-cycle mark bitmaps). The JS thread presents it with one
  atomic pointer swap at a boundary step.
- **Dirty flags instead of code barriers.** On Windows the operating system records which
  heap pages the JS thread wrote (`GetWriteWatch`). On Linux and macOS the engine's write
  helpers store one byte per 4 KB card. Neither is a barrier: nothing branches, nothing
  waits, and no load changes.
- **Generations.** Most JS objects die young. New objects go into a nursery that is
  collected often and cheaply: the cost depends on how many objects survive, not on the
  size of the heap (§8). Collections of the whole heap are rare.
- **No mutexes, no spinlocks, no waiting.** All coordination uses atomic loads, stores,
  `fetch_add` and CAS. The JS thread never waits for the GC. If a step does not fit its
  time budget it declines, and the GC thread does more work before asking again. The GC
  thread sleeps on an address (`WaitOnAddress` or futex) while it waits for an answer.

## 1. Words that sound blocking but aren't

| Term | Blocks? | Meaning here |
|---|---|---|
| GC read/write barrier | No | Usually a few inline instructions on a load or store. This design has no read barrier; on Linux and macOS each reference store also stores one byte (§6). |
| Memory fence (`atomic_thread_fence`, acquire/release) | No | Orders memory accesses. Used throughout. |
| Thread barrier (`pthread_barrier_wait`) | Yes | Not used. |
| Mutex, spinlock, condition variable | Yes | Not used in the GC or in the worker pool. |

The one place the JS thread waits is `gc_run()`, the explicit stop-the-world collection
(`JS_RunGC`): it first asks the GC thread to abandon its cycle and waits for it to stop.

## 2. Why the old double buffer lost data, and what fixes it

`MULTITHREADED_GC.md` had the right shape: copy objects into a second buffer, then make
the copies live with one atomic swap. It failed in three ways. Each one has a fix that
keeps the double buffer.

| Old failure | Fix |
|---|---|
| The JS thread wrote to an object after the GC copied it, so the write was lost at the swap. | At the flip, a copy whose original changed after the copy was made is **rejected**: its back-table entry goes back to the original. Windows detects the change from the page's write-watch bit; Linux and macOS compare the copy with the original. (§7) |
| Raw pointers (`pc`, `sp`, `gc_deref` results) pointed into the old buffer after the swap. | Every object that a raw pointer on the JS thread's stack, or a running frame, points into is **pinned**: its move is rejected at the flip. (§5) |
| The old buffer was reset right after the swap, while it could still be in use. | A from-region is released only when nothing current is left in it, after the back table is brought up to date. Pinned objects never moved, so no stack pointer can point into a released region. (§7) |

The core rule: **nothing the JS thread can still see is changed until it says so at a
boundary step.** Until the flip, the front table and the originals are the truth. After
it, the back table is.

## 3. Threads and boundary steps

```
JS thread:   ─run─|B|─run─|B|─run─|B|─run─|B|─run─
                   ^       ^       ^       ^
                   |       |       |       flip: reject changed/pinned copies, swap tables
                   |       |       remark: finish marking (budgeted), weak step
                   |       mark start: parity, TAMS, root snapshot
GC thread:   ═ prepare ═ mark ═ rescan ═ mark ═ sweep ═ copy ═ release ═
```

The GC thread posts one request at a time (`request`, `req_seq`, `req_budget_ns`) and sets
a poll flag. The JS thread looks at the flag at every **poll point**:

- **every opcode** of the interpreter (one relaxed load, in the dispatch `BREAK`);
- **tick boundaries** (`gc_tick`): after each pending job (`JS_ExecutePendingJob`) and at
  the top of each event-loop turn in `cyberbrowser_main`.

When the flag is set it runs the request's step (`gc_poll_slow` → `run_boundary`) and
answers with `ack_seq` and DONE, DECLINED or ABORTED. Each field has a single writer.

**The stack at a boundary.** Every step spills callee-saved registers with `setjmp` and
scans the JS thread's C stack conservatively (§5), so a step is as safe in the middle of a
script as between tasks. Tick boundaries matter only because the event loop sits there
when nothing else runs.

**Budgets.** A step has a budget (`boundary_budget_us`, default 500 µs). Remark and flip
check it as they go; past it they decline, leaving their work to the GC thread, which does
another concurrent round and asks again with twice the budget. After eight declines the
request carries no budget, so a cycle always finishes. Mark start does not decline (it is
a root snapshot).

**Pacing.** The JS thread counts bytes allocated young and old. A minor cycle starts when
`nursery_size` bytes (default 4 MB) were allocated young since the last minor start; a
major cycle when `major_trigger` bytes (default 32 MB) were allocated old or promoted
since the last major cycle. If old allocation reaches 8 × `major_trigger` without a
finished major cycle, the next poll runs `gc_run()` instead of letting the heap grow
without bound. `gc_request_cycle()` asks for a cycle explicitly; the HTMLElement
constructor and the page-load path in `cyberbrowser_main` use it instead of `JS_RunGC`.

## 4. Data structures

### 4.1 Heap: regions in reserved address space

- One reserved 16 GB range, split into 1 MB regions and committed on demand.
- On Windows the range is reserved with `MEM_WRITE_WATCH`, so the hardware tracks dirty
  pages (§6).
- Region metadata: `state` (FREE, CLAIMED, NURSERY, OLD, HUMONGOUS, HUMONGOUS_CONT),
  `space`, `top` (published with release after every allocation), `tams`, `live_bytes`,
  `alloc_owner`, flags (`collecting`, `in_cset`, `keep`, `pinned`), an object-start bitmap
  (1 bit per 16 bytes) and two mark bitmaps (by parity).
- **Region allocation:** CAS a FREE region to CLAIMED, or bump the high-water mark.
- **Object allocation:** a bump pointer in an allocation context (JS young, JS old, GC
  copy). Each region has one allocating thread at a time.
- Objects of a quarter region or more get dedicated humongous regions and never move.

**Object header: 8 bytes.** `{uint32 handle; uint32 info}` with `info` = type (5 bits) |
total size / 16. Headers sit at 8 mod 16 so payloads are 16-byte aligned. There are no
colour words, canaries or publish states: marks live in side bitmaps.

### 4.2 Handle table: front and back buffers

```c
void **_Atomic front;          /* the table gc_deref reads */
void **tables[2];              /* both reserved for 2^27 entries, committed in chunks */
_Atomic uint32_t high_water;   /* next never-used handle */

static inline void *gc_deref(GCHandle h) {     /* one load + index, as before */
    return atomic_load_explicit(&g_gc.front, memory_order_acquire)[h];
}
```

- **Reads** go only through `front`, which changes only on the JS thread, at a flip.
- **Dual write:** whenever the JS thread changes an entry (allocating, `gc_realloc`,
  `gc_free`) it stores `front[h]` first and then `back[h]`.
- **GC writes to the back table** are a CAS against the value it last read. A CAS that
  fails means the JS thread changed the entry, and the GC drops its copy.
- **Freeing** is a CAS of `front[h]` from the object to NULL, by either thread, so a
  handle is freed once even if the sweep and `gc_free` race.
- After a flip the GC thread brings the new back table (the old front) up to date:
  `CAS(back[h], from, to)` for each accepted move.
- **Handles freed by the JS thread while marking runs are not reused** until marking ends:
  the marker may hold a handle it read earlier, and reuse would show it an object of
  another type.

### 4.3 Mark bitmaps: double-buffered by cycle parity

- Two side bitmaps per region, `mark[0]` and `mark[1]`, 1 bit per 16 bytes. A cycle marks
  into `mark[parity]` with `atomic_fetch_or`. The GC thread clears the next parity's
  bitmaps before posting mark start.
- **Allocate-live (major):** a region records TAMS, its top at mark start. Objects above
  it are live this cycle without being marked.
- **Cycle membership (minor):** only the nursery regions present at minor start
  (`collecting`) are in the cycle; everything else is live by definition.

### 4.4 Communication between threads (all single-writer, lock-free)

| Channel | Writer → reader | Carries |
|---|---|---|
| request / answer words | GC ↔ JS | The pending boundary step, its budget, and the answer. |
| `freed_handles` ring | GC → JS | Handles freed by the concurrent sweep. The JS thread refills its handle cache from it. |
| worklist and pending cards | handed over | The JS thread's remark work when it declines, and the GC thread's when it posts remark. Ownership moves with the request/answer protocol (release/acquire). |
| reference log | JS → GC (read at boundaries) | References stored in off-heap memory hanging off GC objects (§6). |

Each ring is a linked list of blocks: the producer publishes a block's count with a
release store, the consumer reads it with acquire and frees drained blocks. A producer
never waits.

The general-purpose worker pool (used by parallel CSS layout) is a lock-free
multi-producer multi-consumer ring (sequence-numbered cells); idle workers sleep on an
address, and a thread waiting for the pool runs queued jobs itself.

## 5. Roots and pins

- **Precise roots:** every frame on the runtime's frame chain (arguments, locals and the
  whole operand stack), the runtime, contexts and their loaded modules, the current
  exception, the job queue, `gc_add_root` handles, and atoms (permanent).
- **Conservative roots:** the JS thread's C stack and callee-saved registers, spilled
  with `setjmp`. A 32-bit word equal to a live handle, or a pointer into a live object (or
  one past its end), keeps that object alive. Pointers are resolved through the
  object-start bitmap.
- **Pins:** at a flip, every object that a raw pointer on the stack points into, and every
  frame on the chain, is pinned. The flip rejects the move of exactly those objects
  (a sorted list of pinned headers). Stop-the-world compaction pins whole regions.
- **Objects that never move.** ArrayBuffer storage (`JS_GC_OBJ_TYPE_BYTES`) is cached by
  address in the buffer and in its typed arrays, so it is never copied: it is allocated
  in old space and regions holding it stay where they are. Humongous objects never move.

## 6. Dirty tracking

### 6.1 Sources

| Platform | Mechanism | What it sees |
|---|---|---|
| Windows | `VirtualAlloc(MEM_WRITE_WATCH)`; `GetWriteWatch(WRITE_WATCH_FLAG_RESET)` | Every write to a heap page |
| Linux, macOS | A byte per 4 KB card, set by the write helpers | Every store of a reference into memory that may be a GC object |

The software flags are stored by:

- `GC_SET(lvalue, value)`: evaluates the value, stores it, sets the card (release);
- `SAFE_SET`, `SAFE_SET_HANDLE`, `SAFE_SET_VALUE`, `set_value`, `gc_set_heap_*`;
- `gc_memcpy` / `gc_memmove`, `gc_dirty_slot`, `gc_dirty_range`;
- the collector's own copies and `gc_realloc` (the new copy's range).

A card store is `card[(addr - heap_base) >> 12] = 1` after a range check, so stores into
C stack or malloc'd memory cost a compare.

**Coverage is checked by a tool.** `tools/gc_store_check.py` parses every translation
unit with libclang and reports each assignment, `memcpy`/`memmove` and atomic store whose
destination has a reference type (GCValue, GCHandle or a typedef of either, or a struct
containing one), is not a local variable, and is not followed by a dirty-marking call.
Stores of constants that are not references (`JS_UNDEFINED`, `GC_HANDLE_NULL`, numbers)
and `memset` are ignored: removing an edge never hides an object. Off-heap stores are
annotated `/* gc-no-dirty: reason */`. The tree is at zero uncovered stores.

**Exemptions.**

- *Frames on the chain.* The interpreter writes `sp`, `var_buf`, `arg_buf` and
  `stack_buf` without dirty flags. Every frame on the chain is rescanned precisely when
  marking finishes, and a frame that leaves the chain (return, yield, await) has its whole
  value area marked dirty by `js_rt_pop_stack_frame`.
- *Off-heap tables.* References stored in malloc'd memory hanging off a GC object (a DOM
  node's computed-style table) are reported with `gc_note_ref(handle)`. Those handles are
  marked at mark start (minor), at remark, and are the remembered set's off-heap part.
  Off-heap root containers that the JS thread owns (the job queue, the loaded-module
  lists, `gc_add_root`) are re-read at every boundary step instead.

### 6.2 Pulling

Only one thread pulls at a time: the GC thread while it runs, or the JS thread in a
boundary step while the GC thread waits for its answer. A pull reads and resets the
dirty state (`atomic_exchange` per card, or `GetWriteWatch` with reset) and records every
card in the nursery's **remembered set** (`remset_acc`, a bitmap of cards written since
the last minor start) before handing it to the consumer. No consumer can lose another's
information.

### 6.3 Rescanning a card

A card written since it was last looked at may hold references the marker has not seen.
The collector rescans it **conservatively**: for every current object overlapping the
card that is live (marked, or outside the cycle) and may hold references (not a string,
BigInt or byte buffer), every 32-bit word of the object that lies on the card is treated
as a candidate handle. This needs no knowledge of object layouts, which matters because
references live in engine structs, value arrays, property arrays and opaque browser data
whose meaning only the owner's mark function knows. The cost: an integer that looks like
a handle can keep an object alive until the next cycle.

### 6.4 Validating a copy

Copies are made while the JS thread keeps writing to the originals.

- **Windows:** the GC thread resets the write watch of a region (pulling it into the
  remembered set) just before copying its objects. At the flip the JS thread pulls the
  write watch of the evacuated regions; a copy whose original lies on a written page is
  rejected. The reset is a system call that flushes the dirty state on every core, so a
  write made before it is visible to the copy, and a write after it shows at the flip.
- **Linux and macOS:** the software flags cover reference stores only, so the flip
  compares each copy with its original (`memcmp` of header and payload) and rejects any
  that differ. An equal copy is correct by definition. This makes the flip proportional
  to the bytes copied, so one flip validates at most 512 KB of copies (16 MB on Windows).

## 7. Major cycle (whole heap)

**0. Prepare** (GC thread): clear the next parity's mark bitmaps; pull and discard the
dirty state (recording it in the remembered set). Writes after this pull are rescanned
during the cycle.

**1. Mark start** (boundary step):
- flip the parity, set TAMS = top for every region, start deferring handle reuse;
- snapshot the roots (§5) into the worklist with `mark_cb`, and note where the reference
  log stands.

**2. Concurrent mark** (GC thread):
- trace from the worklist with the engine's `mark_children`;
- pull the dirty cards and rescan them (§6.3), in rounds, until a round finds fewer than
  64 dirty cards (at most 16 rounds).

`mark_children` reads objects the JS thread is changing. Every handle is bounds-checked
against the handle table; every loop that reads a count from one object and an array
from another (shape and property array, array length and values, frame counts) is
clamped to the array's real size, so a stale count can only skip slots, whose later
stores set dirty flags. Mark functions never take locks or call into the engine; the
class-prototype table is read directly instead of through its retry loop.

**3. Remark** (boundary step, budgeted):
- the roots again, the reference log since mark start, the last dirty cards;
- trace until the worklist is empty, checking the budget every 256 objects and every 64
  cards; past the budget, decline (the GC thread takes the worklist and the cards);
- when it finishes inside the budget, marking is complete: run the verifier if enabled,
  then the **weak step** on the JS thread (WeakMap/WeakRef/FinalizationRegistry, the
  shape-hash cleanup, finalizers, dead contexts), and recycle the deferred handles.

Running the weak step in the same step as the end of marking means no weak lookup can
hand out an object marking has condemned. During marking itself weak lookups are safe:
whatever the JS thread takes from a weak table it holds on its stack (rescanned at
remark) or stores (a dirty card).

**4. Concurrent sweep** (GC thread): walk each region below TAMS; for a current, unmarked
object, CAS its handle entries to NULL and push the handle to `freed_handles`. Count the
live bytes. A region with no live bytes that nothing was allocated in since mark start,
that no thread is filling and that is not pinned is released.

**5. Choose the collection set** (GC thread): old regions with live bytes at most half
of what was allocated in them, not being filled, nothing allocated since mark start, up
to the validation batch (§6.4).

**6. Concurrent evacuation** (GC thread): copy every current object of the set (except
byte buffers) into GC-owned old regions, `CAS(back[h], from, to)`, and record the move.
The copy's cards are marked dirty (remembered set).

**7. Flip** (boundary step, budgeted):
- pin what running code holds raw pointers to (§5);
- for each move whose entries are still as the GC left them: reject it if the object is
  pinned or changed since the copy (§6.4), by storing `back[h] = from` and keeping its
  region;
- `atomic_store(&front, back)` with release.

**8. Release** (GC thread): `CAS(newback[h], from, to)` for every accepted move, then
release every region of the set that has no current object left. The next batch starts
at step 5.

## 8. Young generation (nursery)

### 8.1 Why

Most JS objects die within moments of being created: temporary arrays, closures, strings
built in loops, iterator results. A minor cycle looks only at young objects and costs
roughly the number of young objects that survive plus the old cards written since the
last minor cycle. Reclaiming 20 MB of fresh garbage no longer means tracing 300 MB of
long-lived DOM and application state.

### 8.2 Layout

- The nursery is the set of NURSERY regions. At minor start the JS thread retires its
  young allocation buffer and labels every NURSERY region `collecting`; new young objects
  go to fresh regions. The collected set is the back buffer, the fresh regions the front.
- **Pretenuring:** the runtime, contexts, shapes, bytecode, module records, function
  definitions, atoms and ArrayBuffer storage are allocated straight into old space.
- **Promotion on first survival:** every object that survives a minor cycle leaves the
  nursery. Once a cycle finishes, the collected set holds nothing, so the remembered set
  never needs entries that outlive a cycle.

### 8.3 Finding the young objects that survive

A young object is live if something reaches it from:

1. **The roots** (§5) and the reference log since the last minor start.
2. **The remembered set:** every card written since the last minor start (`remset_acc`,
   swapped into `remset_cur` at minor start). An old object can only point at a young one
   if it was written after that young object existed, which set the card. Cards of the
   collected nursery itself are skipped: its live objects are traced anyway.
3. **Other live young objects.** Tracing follows references only while they stay inside
   the collected set.

**Buffers owned by old objects.** A DATA object (property array, array storage, value
buffer) is only understood by its owner's mark function, and a minor cycle does not trace
old owners. So a minor cycle scans a marked young DATA object's payload conservatively,
the same way as a dirty card.

### 8.4 Minor cycle

**1. Minor start** (boundary step): retire the young buffer, label the collected set, swap
the remembered set, snapshot the roots and the reference log (then clear it).

**2. Concurrent mark** (GC thread): rescan the remembered set's cards, trace, then rescan
dirty cards in rounds as in a major cycle. Writes made after the minor start into old
objects or fresh young objects are found the same way.

**3. Remark** (boundary step, budgeted), **weak step** in the same step: WeakRefs,
WeakMap entries and FinalizationRegistry targets in the collected set that died are
cleared.

**4. Sweep** (GC thread): free the handles of the collected set's unmarked objects.

**5. Promotion** (GC thread): a region whose survivors fill more than half of it, or that
comes past the flip's validation batch, is promoted in place. The survivors of the other
regions are copied into old space.

**6. Flip** (boundary step, budgeted): as in §7. A rejected copy promotes its region in
place.

**7. Release** (GC thread): bring the back table up to date; regions kept by a rejection
or a byte buffer become OLD, the rest are released.

The remembered set a minor cycle used is cleared when the cycle completes and merged
back into `remset_acc` if the cycle is abandoned, so an aborted cycle loses nothing.

### 8.5 Interaction with major cycles

Minor and major cycles run one after the other on the GC thread, never interleaved. A
major cycle marks and sweeps the nursery too (it is below TAMS) but evacuates only old
regions. Promoted bytes count toward the next major cycle.

### 8.6 Stop-the-world collection

`gc_run()` asks the GC thread to abandon its cycle (`abort`), answers any pending request
with ABORTED, and waits until the thread is idle. Abandoning is safe at every point:

- before remark: nothing was freed or moved;
- during sweep: the condemned objects are unreachable anyway;
- during evacuation: the back-table entries of recorded moves are restored with CAS;
- after a flip: the back-table update and the release always finish first.

Then it runs a full collection on the JS thread: mark from all roots (pinning), the weak
step, sweep, and compaction of sparse regions (regions pinned by raw pointers stay).

## 9. Weak structures

These are the atom hash, shape hash, WeakMap, WeakRef and FinalizationRegistry.

- **During marking** the JS thread may take objects from weak tables freely: it keeps
  them on its stack (rescanned at remark) or stores them (dirty cards).
- **The weak step** runs on the JS thread in the step that completes marking. Entries
  whose object the cycle condemned are removed; finalizers run. Atoms are permanent.
- A conservative rescan can keep a WeakMap key alive for one extra cycle. The language
  allows this: collection timing is unspecified.

## 10. Rules for engine and browser code

1. **Raw pointers to GC memory stay in locals.** Never store one in a global, a malloc'd
   struct or a GC object; store handles. (Exception: byte buffers of type BYTES, which
   never move.)
2. **Store references with the write helpers:** `GC_SET`, `SAFE_SET*`, `set_value`,
   `gc_memcpy`. Run `tools/gc_store_check.py` after changing engine or browser code.
3. **A reference stored in malloc'd memory reachable from a GC object** must be
   reported with `gc_note_ref`.
4. **Only the dual-write helpers change handle-table entries** (`gc_alloc`, `gc_realloc`,
   `gc_free`).
5. **Mark functions only mark:** no locks, no allocation, no calls into the engine, and
   every loop bound clamped to the size of the array it reads (`gc_slots`,
   `gc_clamp_count`).
6. **Two objects' addresses must never be combined.** A frame's `var_refs` grown during
   lazy parsing used to be found by the offset between two objects; it is now a handle
   (`var_refs_handle`).
7. **Only the JS thread allocates** engine objects. Worker-pool jobs work on native data.

## 11. Correctness invariants

| ID | Invariant | Mechanism |
|---|---|---|
| I1 | Every object reachable when marking completes is marked, or was allocated above TAMS (major), or lies outside the collected set (minor). | Root snapshots, dirty-card rescans, rescan of frames on the chain, frame-unlink dirtying, the reference log, remark that completes inside one step |
| I2 | A handle is reused only after both table entries are NULL, and not while marking runs. | CAS free, deferred reuse, the freed ring |
| I3 | `front` changes only on the JS thread, in a flip. | The flip is a boundary step |
| I4 | A presented copy equals the original at the moment of the flip. | Write watch reset before copying (Windows), `memcmp` (elsewhere), rejection |
| I5 | No raw pointer on the stack points into a moved object. | Pins: stack scan and frames on the chain |
| I6 | No thread waits on another to make progress, except in `gc_run()`. | Budgets, declines, single-writer fields, CAS |
| I7 | A weak entry is never handed out after its object was condemned. | The weak step runs in the step that completes marking |
| I8 | Every reference from an old object to a young one is on a card in the remembered set, in the reference log, or in a frame on the chain. | Write helpers or write watch, frame unlink, `gc_note_ref`, copies dirty their cards |
| I9 | When a minor cycle ends, the collected nursery holds no live object except those promoted in place. | Promotion on first survival, in-place promotion |

The verifier (§13) checks I1 and I8 after every remark.

## 12. What the JS thread pays

| Cost | When |
|---|---|
| One acquire load + index per `gc_deref` (as before) | Always |
| One relaxed load of the poll flag | Every opcode and tick |
| A range check and a byte store | Every reference store (Linux, macOS); free on Windows |
| A dirty range over the frame | Each return, yield or await |
| Dual store when an entry changes | Allocating a handle, `gc_realloc`, `gc_free` |
| Boundary steps | Mark start, remark and flip of each cycle (§14) |

## 13. Testing

**Unit tests** (`tests/test_gc.c`, run by the C test suite):

- heap and handles: 8-byte header, 16-byte payload alignment, zeroed memory, interior
  pointers, humongous objects that never move, `gc_realloc`, `gc_free` and handle reuse;
- stop-the-world collection: unreachable objects freed, roots, finalizers that run once,
  compaction preserving contents, objects pinned by raw pointers;
- collection while code runs: locals, arguments, operand stack, native temporaries,
  deep recursion, generators, repeated collections;
- dirty tracking: the write helpers set cards; the Windows write watch reports and
  resets written pages;
- concurrent cycles: a major cycle, promotion by a minor cycle, the remembered set
  (old objects gaining references to young ones through properties, arrays, Maps and
  closures), a mixed workload under back-to-back cycles, every copy rejected,
  half the copies rejected with dense regions moved, `gc_run()` abandoning cycles,
  progress inside a script that never returns to the event loop, budget declines, weak
  references cleared by minor cycles, and ArrayBuffer storage that never moves.

Every concurrent test runs with the verifier on and checks that it reported nothing and
that `gc_test_heap_check()` finds no inconsistent header, start bit or handle entry.

**Switches** (environment variables, also in `GCConfig`):

| Variable | Effect |
|---|---|
| `QJS_GC_BACKGROUND=0` | No GC thread: collections only through `gc_run()` |
| `QJS_GC_NURSERY=0` | No young generation |
| `QJS_GC_STRESS=1` | Cycles back to back (minor, then major) |
| `QJS_GC_STRESS=2` | Also move dense regions and copy every nursery survivor |
| `QJS_GC_REJECT=all` / `half` | Reject every copy, or every other one, at the flip |
| `QJS_GC_VERIFY=1` | After every remark, re-trace from the roots and report reachable objects the cycle left unmarked (also checks the remembered set) |
| `QJS_GC_POISON=1` | Decommit released regions so a stale pointer faults |
| `QJS_GC_BUDGET_US`, `QJS_GC_NURSERY_KB`, `QJS_GC_MAJOR_KB` | Tuning |
| `QJS_GC_LOG=1..3` | Cycle phase times, mark rounds, every boundary step |
| `QJS_GC_TRACE=1` | Record every handle event in a ring; the verifier prints the history of the handles it reports |

**Static check:** `python3 tools/gc_store_check.py build/compile_commands.json` lists
reference stores without a dirty flag (exit status 1 if any).

The JS regression scripts and the audit's GC probes were run under combinations of the
switches above (stress 1 and 2, verify, poison, half rejection); their output matches the
stop-the-world collector's.

## 14. Measurements (Linux, -O2, one GC thread)

Boundary steps on allocation-heavy scripts (three benchmarks below, default settings):

| Step | Average | Worst seen |
|---|---|---|
| Mark start | 24–87 µs | 0.25 ms |
| Remark | 144–204 µs | 0.55 ms |
| Flip | 177–242 µs | 1.5 ms (after declines raised the budget) |

Peak memory and time against the previous code, which collected only when asked:

| Script | Peak RSS before → concurrent | Time before → concurrent |
|---|---|---|
| 400K short-lived objects | 424 MB → 113 MB | 8.2 s → 7.2 s |
| Binary trees | 597 MB → 241 MB | 7.6 s → 7.1 s |
| Array churn | 795 MB → 162 MB | 5.3 s → 3.9 s |
| fib(27) | 176 MB → 40 MB | 0.46 s → 0.15 s |

(The first three spend most of their time in `getenv` calls on the engine's property
paths, which is unrelated to the collector.)

## 15. Limitations and next steps

- **Remark cost** grows with the cards the JS thread dirtied during the last concurrent
  round, mostly initializing new objects. Scanning fresh allocation precisely, or a
  second GC thread, would cut it.
- **Flip cost on Linux and macOS** is proportional to the bytes copied (`memcmp`). The
  batch limit keeps it near 0.2 ms; moving the comparison to the GC thread and comparing
  only cards written since the copy would need write tracking of non-reference stores.
- **One GC thread.** Marking, sweeping and copying could use work-stealing workers.
- **Conservative scans** (stack, dirty cards, young buffers owned by old objects) can
  keep garbage alive for one cycle.
- **A step can exceed its budget** after repeated declines (the budget doubles each
  time); in practice this happens a few times per run of the stress tests.
- **The OS dirty-page alternatives** (Linux soft-dirty, `mprotect`) are not implemented;
  the software cards replace them.
