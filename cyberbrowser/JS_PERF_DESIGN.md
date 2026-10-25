# Faster JavaScript after the baseline JIT

Status: proposal. Follows `JIT_DESIGN.md` (T1 and the first inline caches, M2 and part of M4)
and builds on the handle-free references of `GC_PAUSELESS_DESIGN.md`. It changes the order of
the remaining work: engine representation and runtime paths first, then feedback shared by all
tiers, then an optimizer built in stages, each started only when the previous one's data asks
for it.

## Summary

The copy-and-patch tier did what a baseline tier can do. It removed interpreter dispatch: the
interpreter loop's own time on the YouTube replay fell from 465 ms to 64 ms. Once N0 made the
replays do identical work every run, T1 measured no net gain on these pages: dispatch was a
small part of their time, and the JIT's own costs (stencils, call setup, compiling, 20 MB of
code) took all of what dispatch gave back.

Chrome teaches the same thing. Sparkplug, V8's non-optimizing compiler, was reported at 5–15%
on Speedometer and real-world benchmarks, on top of an engine whose values, objects, calls and
allocation are already cheap. V8's speed comes mostly from those, and from feedback (inline caches recorded in
every tier and used by the next one), not from its baseline compiler.

So the recommendation is:

1. **Keep T1, but make it earn its place.** It is correct, interpreter-compatible, and the
   exit target for any later tier, so it stays. Do not rewrite it in assembly or as a
   Sparkplug clone: that buys little. Until later milestones make compiled code pay, compile
   only what is hot enough to repay compiling (N1 measures the threshold), or leave it off by
   default.
2. **Make the engine itself cheap, for every tier and every native call:** 8-byte values;
   unchecked field loads on hot paths; fewer frame allocations, then calls without heap frames;
   an inline allocation fast path; caches for defining, adding and generically getting
   properties. This is where most of the remaining time is, and it speeds up the interpreter,
   T1, natives and the DOM bindings alike. The JS engine is not all of the time, though: DOM
   and layout code are 15–72% of the loads measured (section 4.2), and this plan does not
   cover them.
3. **Add feedback vectors:** per-function slots recording shapes, types and call targets,
   written by the interpreter and T1, and read by every tier.
4. **Then one optimizer, T2, built in stages toward Maglev's design.** Stage 1 compiles hot
   functions from bytecode into a small IR, specializes on feedback, keeps values in
   registers within a basic block, and exits to the interpreter at the current bytecode when
   a guard fails. Because frames are interpreter-compatible, that exit is a deoptimization
   that needs no separate machinery. Stage 2 extends the same compiler across the function
   (CFG, SSA, register allocation, exits anywhere). Stage 3 adds inlining and escape
   analysis. Each stage starts only when the previous one's counters show it would pay. The
   end state is three tiers, the shape of Chrome's engine without TurboFan.

## 1. What T1 and the first caches taught

Measured on the recorded YouTube watch-page load (`replays/yt-dQw4w9WgXcQ`, replayed with
`tests/jit/replay.sh`; JS-thread CPU about 3.0–3.4 s per load), before N0. Section 4.2 has
the profile N0 measured, with all of the time attributed.

**Where the JS thread's time goes** (CPU samples, JIT on, first match from the leaf):

| Subsystem | Share | Notes |
|---|---|---|
| GC (allocation, marking, barriers) | 20% | `gc_alloc_in`, `gc_align16`, `alloc_cross`, `gc_dirty_*` |
| DOM and layout (native) | 18% | Outside the JS engine's tiers |
| Interpreter category | 15% | Mostly native-call glue (`js_call_c_function`) and cold code |
| Compiled code | 14% | Stencils plus the helpers they call |
| Property access and shapes | 11% | `JS_DefineProperty`, `JS_GetPropertyInternal`, atoms |
| Parsing and compiling | 8.5% | |
| Strings and numbers | 7% | |

**What the JIT changed:**

| | Interpreter | T1 + caches |
|---|---|---|
| Interpreter loop's own time | 465 ms | 64 ms |
| Stencils' own time | — | ~155 ms |
| JS-to-JS call setup (`jit_call_prepare`, frames) | part of the loop | ~135 ms |
| Compiling (3,646 functions, 582 KB of bytecode into 20 MB of code) | — | ~40 ms |
| Whole load, JS-thread CPU (real time, before N0) | baseline | about −4%, within ±3% run-to-run noise |
| Whole load, JS-thread cycles (deterministic replays, N0) | baseline | −0.3% to +3.1% across the five recordings, none outside its interval |

**The lessons:**

- **Dispatch was never the bottleneck.** Each bytecode instruction does more work than its
  dispatch costs, and that work is the same in both tiers: 16-byte values moved through
  memory, handles dereferenced and null-checked, helpers called.
- **The per-call cost is memory, not instructions.** Call setup is dominated by cache misses
  on the callee's function object and bytecode, then by initializing a heap-allocated frame.
  The interpreter pays the same.
- **Inline caches hit, but saved less than hoped.** About 90% of compiled `get_field` lookups
  hit. Each hit still touches the receiver's memory, the cost a full lookup paid first anyway,
  so the gain was the hash walk only: −6% on a property micro-benchmark, below noise on the
  page. Section 7 of `JIT_DESIGN.md` assumed property access was 16% of JS execution; on this
  load, compiled reads are a few percent.
- **The profile is broad.** No single opcode, function or helper dominates. That favours
  changes that make *everything* cheaper (values, frames, allocation) over changes that make
  some code paths much faster.
- **The benchmark was biased, not just noisy.** Real time leaked into what the page did, so a
  slower run did more work, which also flattered faster code. The −4% first measured for T1
  came partly from that. N0 fixed it (section 4): with identical work every run, T1 gains
  nothing on these pages.

## 2. How Chrome got fast, and what carries over

| V8 piece | What it does | What it means here |
|---|---|---|
| **Ignition** (2016) | A register machine with an accumulator; handlers generated from one description (CSA, later Turboshaft) | Fewer, larger instructions than a stack machine, and less stack traffic per operation. Our bytecode stays a stack machine; T2 recovers the difference by tracking the stack symbolically (section 6.1) |
| **Hidden classes, inline caches, feedback vectors** | Every property access, call and arithmetic site records what it saw, in a per-function feedback vector; every tier reads it | The single most transferable idea. Our caches today live in compiled code only and are thrown away by the next tier. Section 5.4 |
| **Sparkplug** (2021) | Bytecode to machine code in one pass, with no IR; interpreter-compatible frames; calls builtins for most operations | Exactly what T1 is. Sparkplug gains 5–15% on an engine whose values, calls and allocation are already cheap; T1 gains nothing measurable on ours (section 4.1). It is worth having as the exit target, not as the source of speed |
| **Maglev** (2023) | CFG SSA built from bytecode and feedback; speculation with deoptimization; about 10× slower to compile than Sparkplug, 10–100× faster than TurboFan | The target design for T2. Its cost (frame states, deoptimization, a register allocator, inlining) pays only where hot, type-stable code exists, so T2 reaches it in stages, each measured first (section 6) |
| **TurboFan / Turboshaft** | The top tier | Out of scope |
| **Compact values and pointer compression** (2020) | Small integers and 32-bit compressed pointers in tagged words; about 40% less heap | We have compressed references already; our values are still 16 bytes. Section 5.1 |
| **Cheap calls** | JS frames live on the machine stack; builtins are called with a fixed convention ("short builtin calls"), with no heap objects per call | Our frames, JS and native, are GC objects (pooled per depth) that every call initializes and every return releases. Section 5.3 |
| **Fast allocation** | Bump-pointer allocation inline in generated code; a parallel scavenger for the young generation | Our allocation path is a C call with alignment and hole handling (`gc_align16`, `alloc_cross`). Section 5.2 |
| **Embedded builtins** | Common runtime functions written once, callable from every tier at low cost | Our helpers (`jsop_*`, `jit_*`) play this part; the cost of reaching them is what 5.3 cuts |

Two more precedents bear on T2's first stage:

- **HHVM's region JIT** compiles "tracelets" specialized on observed types, with side exits to
  the interpreter.
- **CPython 3.13's tier 2** builds superblocks of micro-ops with guards whose exits return to
  the interpreter.

Both get part of an optimizer's benefit without SSA, by using the interpreter as the
deoptimization target. Our interpreter-compatible frames make that unusually cheap here.
Unlike them, T2 treats this as its first stage, not its final form.

## 3. What each lever can reach

Estimates for the YouTube watch and search loads, whose profiles (section 4.2) are the most
JS-heavy of the five recordings. They are checked by measurement as each milestone starts.
The ranges overlap: each lever shrinks the base the next one works on.

| Lever | Attacks | Estimated gain |
|---|---|---|
| 8-byte values (5.1) | Stack, frame and property traffic everywhere; half the value memory | 5–10% |
| Unchecked hot-path loads (5.1) | Null checks on every handle field read (`SAFE_GET_OR`, about 3% of the load in `gc_deref` alone) | 2–4% |
| Inline allocation fast path, fewer allocations (5.2) | Part of the 21% in GC | 4–8% |
| A frame pool that does not miss (5.3) | 577,000 frame allocations, 308 MB per watch-page load | 1–3% |
| Calls without heap frames, JS and native (5.3) | Call setup, frame initialization, native-call glue | 3–6% |
| Caches for defining, adding and generically getting properties (5.4) | `JS_DefineProperty`, `add_property`, `JS_GetPropertyInternal` from natives | 2–4% |
| T1's threshold (5.5) | Compiling, and code that runs too little to repay it | 0–2% |
| T2 stage 1 (6.1) | Tag checks, stack traffic and helper calls within blocks of hot code | 2–5% of what is left |
| T2 stages 2–3 (6.2, 6.3) | Values and checks across blocks and loops; inlining; allocation sinking | Unknown until stage 1's counters |

Together, the rows before T2 are the bulk of a plausible 20–30% of the JS-heavy loads' JS-thread
time. None of them is a compiler. On pages dominated by DOM and layout, such as the Wikipedia
article (72% DOM and layout), the gain is proportionally smaller.

## 4. Measuring (N0)

Nothing in this document should start without a measurement that can see a 1% change.

- **Cycle counts, not wall or thread time.** Read `QueryThreadCycleTime` for the JS thread
  (or a performance counter where available) at the timeline marks. It does not suffer the
  15.6 ms granularity of `GetThreadTimes`. It still moves with machine load, so keep the
  interleaving of `replay.sh`.
- **Several recordings.** Today there is one page. Add YouTube's home page, a search result,
  a news site and a large Wikipedia article, each recorded with `CYBER_RECORD`.
- **Per-subsystem counters.** Count, per load: allocations by type; calls by kind
  (JS→JS compiled, JS→JS interpreted, JS→native, native→JS); property accesses by kind
  (own, prototype, accessor, miss); bytecode instructions per tier. Counters are
  deterministic, so a change can be shown to remove work even when time is noisy.
- **A gate per milestone:** the median over 15 interleaved runs, on every recording, beyond
  the measured noise.

### 4.1 As built

- **`tests/jit/replay.sh`** replays the recordings listed in `tests/jit/replays.txt` under
  several settings, with settings and recordings interleaved and the settings' order rotated
  each round. **`tools/replay_stats.js`** reports each setting's median JS-thread cycles, its
  spread, and its change from the first setting with a 95% bootstrap interval. `COUNTERS=1`
  adds the counters side by side.
- **The timeline** (`CYBER_TIMELINE=1`) reports the JS thread's cycles
  (`QueryThreadCycleTime`). The load is timed to the **"page settled"** mark, at the end of the
  settle loop. After it, the harness rasterizes and JPEG-encodes a screenshot and writes
  dumps, which earlier measurements included: 2% of the YouTube watch page and 22% of BBC.
  `CYBER_SPIN_PCT=<p>` adds exactly p% cycles at that mark, to calibrate the measurement.
- **The sampling profiler** (`CYBER_SAMPLE`) names frames in system DLLs (through DbgHelp, by
  their DLL's exports). `CYBER_SAMPLE_LOAD=1` stops it at "page settled", so profiles match
  the timing. `tools/sample_report.js` names functions without symbols by their source file,
  shows each subsystem's top functions, and shows time in the OS by the first function in our
  code that led there.
- **`CYBER_COUNTERS=1`** (`src/quickjs/cyber_counters.h`) counts calls by kind, frames,
  property paths, inline-cache outcomes and allocations by type and size, and prints them at
  exit.
- **Five recordings:** YouTube watch, home and search pages, BBC News, and Wikipedia's
  "United States".
- **Deterministic replays.** Real time leaked into the page's work. Timers and observers read
  real time, so a slower run ran more interval callbacks and smaller scheduler slices. Fetches,
  stylesheets and images arrived at different points of the run. Replay misses went to the live
  network. The same page varied by up to 20% in work between runs, which made timing bimodal.
  It also biased every comparison: faster code was credited with doing less work.

  `replay.sh` therefore runs with:
  - `CYBER_DET_TIME=1` (`src/det_clock.h`): a page clock that starts at a fixed date and
    advances by a fixed amount per call made (a count that doesn't depend on which tier runs
    the code) and per clock read, plus virtual time. It covers timers, `Date`,
    `performance.now`, media playback and observers, and `Math.random` gets a fixed seed.
    At the start of each event-loop turn, the loop also waits until no fetch, stylesheet or
    image is in flight, so the page receives each at the same point every run.
  - `CYBER_SYNC_FETCH=1`.
  - `CYBER_REPLAY_OFFLINE=1`: a request missing from the recording fails at once.

  Recordings are made with `CYBER_DET_TIME=1 CYBER_SYNC_FETCH=1` too, so the page's "random"
  URLs match the replay's. The recorder now numbers responses in the order they were requested,
  and replays them in that order. Each run reports its call count, and the statistics compare
  only runs at the recording's most common count.

**Calibration** (12 rounds; settings: JIT, an A/A copy of it, the JIT plus a 1% spin, and the
interpreter). Every run of every recording, under every setting, made the same number of calls.
The spread per run was 0.2–1.2%. The A/A copy's interval included 0 on all five recordings.
The 1% spin was detected on three recordings and borderline on two. **A change of about 1.5% is
detectable on every recording at 12 rounds; 1% needs about 24.** The exit criterion is met at
that run count. (That calibration was timed to the later mark, screenshot included; the
spread does not depend on where the mark is.)

**What this means for how the work is done.** Single changes in N1 will often be 1–3%. Each is
measured on its own: its counters first (deterministic, so they show whether it removed the
work it targets), then 24 interleaved rounds against the previous binary. Changes are not
batched, because a batch's total hides one that costs more than it saves.

**What N0 already shows:**

- **The JIT gains nothing on these pages once work is held equal.** Against the interpreter,
  timed to "page settled" over 12 rounds, T1 measured −0.3% (the YouTube watch page), +2.9%
  (home), +1.9% (search), +3.1% (BBC) and +0.9% (Wikipedia). None was outside its 95% interval;
  if anything T1 is slightly slower. That run's spread was 1–4% per run against the
  calibration's 0.2–1.2%: the machine's load varies between sessions, so each gate run checks
  its own spread before its result is used. The 4% gain
  measured before N0 was partly the measurement's bias. T1's own costs and its 20 MB of code
  cancel what dispatch saved.
- **The interpreter runs little, so its caches matter little.** Under the JIT, a watch-page
  load makes about 92,000 interpreted calls and 1 M interpreted property gets, against 4.9 M
  compiled calls. Most generic property work comes from natives and the runtime instead.
- **Frames:** 577,000 of 5 million bytecode frames per watch-page load miss the pool and are
  allocated, 308 MB of them. That is the largest single allocation stream after objects. A
  pool that does not miss is a small change, ahead of N2's stack frames.
- **Property definition, not assignment, adds most properties.** There are 4.7 M
  `JS_DefineProperty` calls (object literals and natives building objects) against 5.3 M
  property adds, and 5.3 M generic gets (`JS_GetPropertyInternal`) from natives and the
  interpreter's slow path. These are N1's cache targets.
- **The shape transition cache** answers 84% of property adds (4.4 M of 5.3 M).

### 4.2 Where all of the time goes

CPU samples to "page settled" on the deterministic replays, JIT on, each sample attributed to
the first subsystem found from its leaf. Time in system DLLs counts toward the subsystem of
the code that called into them.

| Subsystem | YT watch | YT home | YT search | BBC | Wikipedia |
|---|---|---|---|---|---|
| GC | 21.4% | 23.8% | 21.7% | 12.9% | 5.7% |
| DOM and layout (native) | 19.6% | 14.8% | 21.1% | 26.5% | 72.2% |
| Compiled code | 18.0% | 10.3% | 17.8% | 0.4% | 2.5% |
| Interpreter and native-call glue | 14.8% | 16.3% | 13.1% | 13.7% | 3.5% |
| Property access and shapes | 13.4% | 13.1% | 11.6% | 6.8% | 2.4% |
| Strings and numbers | 6.0% | 7.8% | 5.1% | 5.2% | 3.4% |
| Parsing and compiling | 3.6% | 9.3% | 6.3% | 15.7% | 1.9% |
| Fonts and text | 0.9% | 1.3% | 1.4% | 14.9% | 5.6% |
| Harness (replay reads, logging) | 0.9% | 1.7% | 0.6% | 2.4% | 0.4% |
| Regular expressions | 0.7% | 0.9% | 0.6% | 0.4% | 1.3% |
| Other | 0.7% | 0.7% | 0.9% | 1.2% | 1.1% |

Inside the buckets (the watch page):

- **GC:** handle dereferences (`gc_deref`, 3.3%), allocation (`gc_alloc_in`, `gc_align16`,
  `alloc_cross`, `gc_core_alloc_object_memory`; about 6%), dirty-card marking
  (`gc_dirty_range`, `gc_dirty_slot`; 3.4%) and `memcpy` in `gc_realloc`.
- **Compiled code:** the call path (`jit_call_prepare`, frame setup; about 4%), cached property
  reads (`jit:get_field`, `jit_get_field_ic`; about 4%) and closure variables
  (`js_varref_*`; 1.5%).
- **Interpreter:** `JS_CallInternal` (2.1%), native-call glue (`js_call_c_function`, 1.1%),
  and compiling (unwind registration, writing code).
- **Property access:** lookups (`find_own_property`, `find_own_property_handle`; 3.5%),
  definitions (`JS_DefineProperty`, `JS_CreateProperty`; 1.6%), generic gets and shape-table
  lookups.
- **Time in system DLLs,** about 19% of samples, is now named. The largest parts are the C heap
  behind `js_plain_*` (`RtlAllocateHeap`/`RtlFreeHeap`, about 3%) and `memcpy` (2.6%). Waits on
  FFmpeg's decoder threads come next, then the GC's memory commits and waits, string compares
  in style matching, and reading font files.

Two measurement artifacts were found and removed this way: the JIT's map for the profiler
(formatted with `printf` per instruction, 2% of a profile) and, on some runs, a deadlock in the
sampler, which grew its buffer while the main thread was suspended.

## 5. Making the engine cheap

### 5.1 Values and loads

**8-byte values.** `GCValue` is a 64-bit payload plus a 64-bit tag. References are already
32-bit compressed addresses (`gc_deref` is `base + (handle << 4)`), so everything fits in 8
bytes with NaN boxing:

- a double is stored as itself (NaNs canonicalized);
- every other kind sits in the NaN space, with a 16-bit tag and a 32-bit payload: an int32,
  a bool, null, undefined, a compressed reference or a short bigint.

This halves the operand stack, frames, property slots and arrays, and every value copy.
Stencils become simpler too, since the store-forwarding rule of `JIT_DESIGN.md` section 4
disappears with single 8-byte moves.

It is an engine-wide change. Every `GCValue` field, `JS_VALUE_GET_*` macro and the C API
change, and so do the stencils and helpers, which are regenerated from the same macros. The
path is incremental:

1. Make every access go through the existing macros: audit for direct `.u` and `.tag` uses,
   and add a build mode that makes the struct opaque so the compiler finds them.
2. Switch the representation behind the macros.
3. Run the test suite, the four-mode differential tests and the replays.

The GC's marker reads values too, and changes in the same step.

**Unchecked loads on hot paths.** `SAFE_GET_OR` checks for a null handle on every field read,
and `gc_deref` checks again. On paths where the handle is known valid (an object from a value
whose tag says object, a shape from a live object), use unchecked accessors. Keep the checked
ones at API boundaries and in debug builds (`GC_CHECKED_DEREF`). This is mechanical, local,
and measurable path by path.

### 5.2 Allocation

- **An inline fast path.** Bump allocation within the current hole or line, in a static
  inline function and in a T1/T2 stencil, falling back to the existing path when the hole is
  exhausted. `GC_PAUSELESS_DESIGN.md` section 3 already makes allocation "nothing but bump";
  this item is to reach that path without a call, from compiled code too.
- **Allocate less.** Count allocations by type first (N0). Candidates already known:
  - argument objects not needed by the callee;
  - temporary strings from concatenation, which ropes can defer;
  - closures created and called once.
- **Size objects from allocation sites.** Record, per `new` site and object literal, the
  final property count. Allocate that many in-object slots, so that later property adds
  neither reallocate nor move properties out of the object. V8's slack tracking does this.

### 5.3 Calls

Today every bytecode call initializes a pooled heap frame (`JSStackFrame` and its buffers,
`js_bc_frame_enter_pooled`), links it into the runtime's chain and releases it on return.
Every native call takes a pooled C frame too. The frames are heap objects because closures,
generators and the collector need to find them.

- **A pool that does not miss (N1).** The pool keeps one frame per call depth, sized for the
  largest function seen at that depth so far, with headroom. A call needing a larger frame,
  or a frame pinned by a closure, allocates a new one: 577,000 times (308 MB) per watch-page
  load. First, the counters should say which of those it is. A size class per depth (a few
  frames of increasing size), or keeping the replaced frame for reuse, removes most of them
  without changing how frames work.
- **Frames on the machine stack for calls that need nothing else.** That means a callee that
  captures no variables, isn't a generator and doesn't use `arguments`. The bytecode compiler
  knows this statically (`var_ref_count == 0` and the function kind). The collector already
  scans the JS thread's stack conservatively and walks the frame chain. A stack frame joins
  the chain as a small record pointing at its buffers, and is materialized on the heap only
  if something needs it to outlive the call (a debugger, a closure created by `eval`).
- **A lighter native-call path.** A native function whose arguments fit needs no copied
  argument buffer and no C frame object. Pass the caller's operand-stack slots directly, and
  record only what backtraces need.
- **A call cache per site.** This is part of the feedback vector (5.4). It records the
  callee's bytecode and compiled entry, so a monomorphic call skips the class checks and the
  bytecode load.

**As built (N2): a JS stack.** After N1's pool fix, frames were no longer allocated, so what
a stack frame could save was the pool's bookkeeping: a heap frame's dirty marks, its handle
links, and the clearing when it returns. The frames went to a dedicated JS stack rather than
the machine stack. Frames vary in size. A stencil cannot `alloca` beside its `musttail`
continuation, and a fixed buffer per call frame would eat the 1 MB thread stack at
YouTube's 300–600 calls deep. JavaScriptCore keeps its JS stack apart for the same reasons.

- **The chain by pointer.** `JSRuntime.current_frame` and `JSStackFrame.prev_frame` link
  the frames while they run (a running frame never moves). `prev_frame_handle` remains for
  heap frames that outlive their calls. Alone this was −0.5% to −1.0% on the YouTube replays.
- **Which calls.** `js_bc_stack_frame_ok` scans a function's bytecode once. A normal
  function with no var_refs qualifies unless it makes closures or classes, builds mapped
  `arguments`, calls `eval`, or makes references to locals: each can leave a var_ref into
  the frame (`get_var_ref_enhanced` makes them for lazily parsed children too).
  `QJS_JS_STACK=0` turns it off.
- **The frame.** A 16 MB region, pushed and popped by pointer bump. The frame has the pooled
  frame's layout and no handle. The collector marks it as every frame on the chain
  (`JS_MarkActiveStackFrames`), so its slots need no dirty flags. Returning resets the top.
  Running out falls back to a heap frame.

On the watch page 4.54 M of 5.03 M bytecode frames go there. Against N2's previous build:
−1.8% (watch), −2.0% (home), −2.2% (search), BBC and Wikipedia at parity, with the same calls
and DOM nodes. GC stress, poisoning and `QJS_GC_VERIFY` runs are clean: `tests/jit/frames.js`
in every JIT mode, the watch, search and Wikipedia replays with the JIT on and off, and the
test suite. (The suite's `test_dom_wrappers_are_lazy` fails sometimes under stress with the
JS stack on or off: under stress the count of live nodes falls as earlier garbage is swept.)

**C function calls** (4.2 M per watch-page load, as many as bytecode calls) take their
frame from the JS stack too, rather than the per-depth pool of rooted heap frames. The frame
holds `cur_func` for backtraces, plus the arguments copied when there are fewer than the
function's length. Those copies sit between `arg_buf_offset` and `var_refs_offset`, where
the collector marks a bytecode frame's slots. Against the bytecode-only JS stack: −1.3%
(watch), −2.0% (home), −0.6% (search), BBC and Wikipedia at parity.

**Arguments in place did not pay, and was removed.** A callee called from the newest JS
stack frame used the caller's operand-stack slots as its arguments (a negative
`arg_buf_offset`), padding with undefined above the caller's `sp`. 2.7 M calls per
watch-page load took it. Measured against native frames alone it was +0.3% to +0.5%: the
copies it saved cost less than its checks. Getting it right also exposed a bug that is worth
remembering. A function with no parameters, called with `argv` at the stack top, got an
`arg_buf_offset` of 0. `JS_MarkActiveStackFrames` takes such a frame for a C function's and
marks none of its slots. The replays then varied from run to run, only with the concurrent
collector's minor cycles running, and `QJS_GC_VERIFY` could not see it, because it enumerates
roots the same way. What found it was checking, in each marking pass, that every live frame's
slots fell inside a range that pass actually marked. Two invariants from the work remain:
`get_var_ref_enhanced` refuses a frame without a handle (the closure code's walk up the
dynamic chain could reach one), and `tests/jit/frames.js` covers arguments mutated in callees,
padding and zero-parameter calls under GC stress.

**The N2 gate** (N1's last build against N2's, 24 rounds, deterministic replays):

| Recording | JIT on | Interpreter | Default against default (interpreter then, JIT now) |
|---|---|---|---|
| YouTube watch | −3.1% | −5.2% | −6.0% |
| YouTube home | −5.0% | −5.3% | −5.0% |
| YouTube search | −3.7% | −5.0% | −3.6% |
| BBC | ±0 | ±0 | ±0 |
| Wikipedia | ±0 | ±0 | ±0 |

The work is identical (the same calls and DOM nodes), the tests pass in every JIT mode, and
GC stress, poisoning and `QJS_GC_VERIFY` runs of the replays are clean. The −6% target was
not reached with the JIT on: lazy prototypes (about −1%), the JS stack (−2%) and native frames
on it (−1% to −2%) are what calls and allocation had left after N1. The rest of what N2
listed is better done with N3: per-site call caches belong in feedback vectors (5.4), and the
allocator's line crossing (5.6) with 8-byte values, which change every object's size.

### 5.4 Feedback vectors and caches in every tier

Give each `JSFunctionBytecode` a feedback vector, allocated lazily when the function first
runs hot (as V8 allocates them lazily). It has one slot per property access, call,
arithmetic and comparison site, numbered by the bytecode compiler:

- **Property sites:** the `JitIC` ways of today (shapes, holders, slots), moved here from
  `JitCode`. Add `put_field` (existing writable property), `define_field` and property adds
  (transition: old shape → new shape, slot), and getters (holder, getter function).
- **Call sites:** the callee (monomorphic or megamorphic), and whether it is compiled.
- **Arithmetic and comparison sites:** the operand kinds seen (int, double, string, other),
  as bits.
- **Allocation sites:** the final property count (5.2).

The interpreter reads and writes the property and call slots on its fast paths, as T1 does
now. Immutable shapes (M4) make the interpreter's cache pay, which the reverted October
experiment showed it could not before. T1 uses the same slots. T2 reads the type and shape
bits to decide what to specialize, in every stage.

The retention set, prototype epochs and the megamorphic cache of M4 carry over unchanged.

**Before feedback vectors (N1): caches where the runtime's generic paths are.** N0 shows the
interpreter runs little under the JIT, while natives and the runtime call
`JS_GetPropertyInternal` 5.3 M times and `JS_DefineProperty` 4.7 M times per watch-page load.
N1 puts the caches there:

- **A generic get cache:** a global table keyed by (shape, atom) inside
  `JS_GetPropertyInternal`, with the rules of the megamorphic cache (hashed shapes; prototype
  entries checked against the epoch). It serves natives, the DOM bindings and the interpreter
  at once.
- **A define/add cache:** for a definition that adds a data property to an object, the
  shape's transition cache (M4) already gives the new shape. Give `JS_DefineProperty` and
  `JS_CreateProperty` a fast path for that case, so the common definition skips the general
  descriptor logic.
- **`put_field` and `define_field` caches** in compiled code, as for `get_field`.

### 5.5 T1's threshold

T1 compiles a function after about 20 entries, or the equivalent in loop iterations: 3,646
functions and 20 MB of code on the watch page, most of them run too little to repay
compiling. With deterministic replays the threshold can now be measured. Sweep it, from today's
value to compiling only the hottest few hundred functions. If no threshold makes T1 a net
gain, turn it off by default until N2 and N3 make compiled code worth its cost. It stays in
the build, tested in every mode, as T2's exit target.

### 5.6 N1 as built

Each change was measured alone: 24 interleaved rounds against the build before it (section
4.1), with the counters showing what work it removed. Changes are relative to that build.

| Change | Watch | Home | Search | BBC | Wikipedia | Kept |
|---|---|---|---|---|---|---|
| Frame pool: no pinning for lazy stubs | −4.0% | −1.0% | −4.7% | ±0 | −1.4% | yes |
| Define fast path in `JS_DefinePropertyValue` | −6.7% | −6.2% | −6.3% | ±0 | ±0 | yes |
| Generic get cache in `JS_GetPropertyInternal` (two versions) | +0.4 to +1.5% | +1.0% | +0.8 to +1.0% | ±0 | ±0 | no |
| `put_field` inline cache in compiled code | +0.7% | ±0 | ±0 | ±0 | ±0 | no |
| Object and string headers: plain stores, one dirty mark | −1.4% | −1.8% | −1.5% | ±0 | ±0 | yes |
| T1 threshold 2,000 → 50,000 (from the gate's runs) | −1.3% | −1.7% | −1.2% | −1.8% | ±0 | yes |

**The frame pool.** The counters showed the misses were not sizes: they were frames
`js_closure2` pinned so that a lazily compiled closure could later resolve names through
its parent's frame. A stub with a resume-parse state already captured every free name of its
body, and installing it needs no frame, so only the legacy lazy path still pins.

**Defines.** The 4.7 M `JS_DefineProperty` calls per watch-page load were object literals
(`define_field`), C natives and closures' `length`, `name`, `prototype` and `constructor`.
For a new data property of an ordinary extensible object, `JS_DefinePropertyValue` now goes
from one hash lookup to `add_property` (whose transition cache finds the next shape), skipping
the descriptor logic. 4.0 M of the 4.7 M take it.

**What did not pay.** The get cache answered 63% of generic gets, then 1.2 M prototype-chain
gets once restricted to them, and lost both times. A walk on these pages is one or two hash
lookups. A probe on every miss, and refills after the frequent epoch moves of a loading
page, cost more than the walks saved. Sharing the megamorphic table also evicted the JIT's
entries. The `put_field` cache answered 85% of compiled puts, but an out-of-line call costs
what `jit_put_field_fast`'s own lookup did. Inlining its own-property way into the stencil
would gain at most about 0.5%. Both were removed. Feedback vectors (N4) revisit them where
the cache is read inline.

**Unchecked loads and inline allocation, as planned, were the wrong targets.** In the
profile, `gc_deref` (3%) is the object loads themselves: `find_own_property` already
dereferences once. What was worth removing was the dirty marking of `SAFE_SET`.
`JS_NewObjectFromShape` marked an object's cards 13 times for fields the zeroed memory
already held, and strings, which hold no references, were marked at all. An inline
allocation fast path would rarely apply: lines are 128 bytes, objects average 94, so most
allocations cross a line (`alloc_cross`). Each allocation also takes a locked `or` on the
alive bitmap, which the sweeper writes concurrently. Both belong to the allocator work in N2.

**T1.** With the frame pool fixed, compiled code beats the interpreter: 12.6 G against 13.9 G
cycles on the watch page. The sweep found no gain at the old threshold (2,000), −2.5% on the
watch page at 20,000 and 100,000, and the gate confirmed 50,000 as the default. On this
result T1 is now on by default (`QJS_JIT=0` turns it off).

**The gate** (N1's starting build against its end, 24 rounds, deterministic replays):

| Recording | JIT on | Interpreter | JIT on, real time (8 rounds) |
|---|---|---|---|
| YouTube watch | −12.2% | −12.7% | −7.4% |
| YouTube home | −10.5% | −7.4% | −8.0% |
| YouTube search | −10.6% | −12.1% | −13.6% |
| BBC | −3.4% | ±0 | ±0 |
| Wikipedia | −2.2% | ±0 | ±0 |

The work is identical (the same calls and DOM nodes in every run), and the tests pass in every
JIT mode. One run in about 360 of the threshold sweep crashed (yt-search, threshold
500,000) and did not repeat in 76 more runs; it is open.

**Found for later.** 471,000 of the watch page's 486,000 closures make a prototype object
eagerly, with its `constructor` and the function's `prototype` defined. Upstream QuickJS
makes it lazily (`JS_PROP_AUTOINIT`), which this engine disabled for the moving collector.
Making it lazy again would remove a sixth of the page's object allocations. It goes in N2
with allocation.

## 6. The optimizer, built in stages

V8 runs four tiers: Ignition, Sparkplug, Maglev and TurboFan. JavaScriptCore and SpiderMonkey
also put an optimizer between their baseline and top tiers. Nobody runs a "block-local"
speculative tier as a permanent extra step, and there is a reason: such a tier wins only
inside basic blocks. JS code full of calls and property accesses has short blocks, and every
call writes values back to the frame. Most of what Maglev gains crosses blocks: unboxed
values kept in registers, a check made once for the whole function, and registers allocated
over the whole function. A permanent block-local tier would risk being neither cheap nor fast,
and would leave three compilers and an interpreter to maintain.

So the third tier here is **one optimizer, T2, built in stages**. Each stage ships on its own
and is measured, and the next stage extends the same compiler rather than replacing it. The
end state has three tiers (interpreter, T1, T2), the shape of Chrome's engine without
TurboFan.

What makes the staging possible is the exit model. T1 and the interpreter share frames, so a
failed guard can exit to the interpreter at a bytecode boundary: it writes back the values
the interpreter's frame should hold and sets `pc`. That *is* a deoptimization. Its frame
state, meaning where each interpreter slot's value lives at that bytecode, is the same
concept Maglev records, but simpler, because the target is always the interpreter's own
frame. Stage 1 needs it only within a block. Stage 2 needs it across the function. Stage 3
adds inlined frames. The mechanism is built once and grows.

**Input:** the bytecode, plus the feedback vector.

**Output:** machine code that uses the interpreter's frame layout, like T1.

### 6.1 Stage 1: guards, exits and a block-local virtual stack

The first stage removes the work every bytecode instruction still does in T1: tag checks,
operand-stack loads and stores, and calls to helpers for cached property accesses.

- **An IR from the start.** Stage 1 translates bytecode into a small IR of operations on
  virtual registers, one list per basic block, rather than emitting machine code straight
  from bytecode. The operations are: load and store frame slot, guard tag, guard shape, int32
  arithmetic with overflow guard, double arithmetic, load and store property slot, call
  helper, call compiled code, branch and exit. This costs little in stage 1, and it is what
  stage 2 builds its control-flow graph and SSA on.
- **A virtual stack.** Within a block, translation tracks for each operand-stack slot and
  each local which virtual register holds its value, or that it is still in the frame, or a
  known constant. It also tracks what is known of its kind: int32, double, object of shape S,
  or unknown. `get_loc`, `dup`, `swap` and constants become bookkeeping with no code. Values
  are written to the frame only at the end of a block, before a call or a helper, and at a
  guard's exit.
- **Guards from feedback.** Where feedback says a site has only seen int32s, T2 emits one tag
  check and then untagged arithmetic, keeping the result in a register as an int32. A site
  that has seen one shape gets an inline shape compare and a direct slot load. Later uses of
  a value a guard has proven need no check.
- **Exits.** A failed guard jumps to a per-site exit stub. The stub writes the block's live
  values to the frame (boxing int32s and doubles), sets `pc` to the instruction's start, and
  returns to the interpreter. The instruction then runs in the interpreter, which also
  updates the feedback. A site whose exit keeps firing recompiles without that guard.
- **Calls.** Monomorphic call sites with a compiled callee call its entry directly (5.3).
  Calls to small builtins that feedback identifies (`charCodeAt`, `Math.*`, array `length`
  and `push`) get inline fast paths behind a callee check.
- **Registers** are handed out in stack order as values are pushed and freed when they are
  consumed. Within a block that is close to optimal for a stack machine.

Stage 1 is also how the stages are steered. Its counters say how often guards hold, how much
code is type-stable, how long blocks are, and which values are written back at block ends
only to be reloaded by the next block. That last count is exactly what stage 2 would remove.

### 6.2 Stage 2: the whole function

When stage 1's counters show that values cross block boundaries in hot code, extend the same
compiler:

- **A CFG and SSA** over stage 1's blocks and operations. Loop headers get phis. Kinds are
  propagated over the graph, so an int32 induction variable stays unboxed in a register
  across a loop, and a value checked once needs no check in later blocks.
- **Linear-scan register allocation** over the function, replacing stack-order allocation.
- **Exits anywhere.** Each guard records its frame state: for every interpreter slot live at
  that bytecode, the SSA value that holds it. The exit stub boxes and writes those values,
  which generalizes stage 1's block-local exits.
- **Simple global optimizations** that SSA makes cheap: removing redundant guards and
  loads, folding constants, and moving shape checks out of loops.

This is Maglev's design, minus inlining. Compiling stays roughly linear, about one pass for
SSA, one for kinds and one for allocation, so it can still run on the JS thread.

### 6.3 Stage 3: inlining and allocation

If stage 2's data shows hot calls to small monomorphic callees, and short-lived objects in
hot code:

- **Inlining** of small monomorphic callees. A frame state then describes a stack of
  interpreter frames, and an exit materializes the inlined frames before entering the
  interpreter.
- **Escape analysis and allocation sinking.** An object that never leaves the function is
  kept in registers and allocated only on the exits that need it. This is the one
  optimization that attacks the 20% in GC directly.

### 6.4 Code generation

Register allocation doesn't fit copy-and-patch well: stencils pin their operands to fixed
registers, and every combination would need its own variant. T2 therefore uses a small macro
assembler per target. It emits x86-64 and arm64 from the IR's short list of operations, and
it serves all three stages.

T1 stays copy-and-patch: it is the tier every function reaches, and it ports to arm64
without a code generator.

### 6.5 Tiering

- **Interpreter → T1:** at the threshold N1 measures (5.5), if T1 is on by default by then.
- **T1 → T2:** when a function's counter passes a much higher threshold (to be measured, of
  the order of 10^5 executed instructions) and its feedback has settled, meaning no new types
  or shapes seen for a while.
- **T2 → T1:** a function whose guards fail repeatedly goes back to T1 (recompiling with
  fewer guards first, once). Its T2 code is freed when its bytecode's T1 code would be.
- **Entering T2 in a loop:** stage 1 enters at function start only. Stage 2 adds entry at a
  loop header from an interpreter or T1 frame (on-stack replacement). Since the frames are
  shared, that needs only the header's frame state, read in the other direction.

## 7. Milestones

| | Work | Exit criterion |
|---|---|---|
| N0 | Cycle-based timing, four more recordings, per-subsystem counters (section 4) | A 1% change is detectable on every recording |
| N1 | In this order, each measured alone (section 4.1): a frame pool that does not miss; T1's threshold; generic get and define/add caches in the runtime, `put_field`/`define_field` caches in compiled code; unchecked hot-path loads; inline allocation fast path | −8% JS-thread cycles on the YouTube replays; parity on all five. **Met** (section 5.6): −10.5% to −12.7% |
| N2 | Calls without heap frames for simple callees; lighter native calls; call caches; lazy function prototypes and the allocator's line crossing (5.6) | −6% more; parity; GC stress clean. **Partly met** (5.3): −3% to −5% with the JIT on, −5% interpreted; parity; GC stress clean |
| N3 | 8-byte values engine-wide | −6% more; memory down; all four JIT modes and replays at parity |
| N4 | Feedback vectors; T2 stage 1 (IR, block-local virtual stack, guards and exits), x86-64 first | −5% more on JS-heavy pages; counters for guard failures, block lengths and values crossing blocks |
| N5 | T2 stage 2 (CFG, SSA, kinds across blocks, linear scan, exits anywhere, loop entry); arm64 | Go/no-go from N4's counters; then −8% more on JS-heavy pages |
| N6 | T2 stage 3 (inlining, escape analysis) | Go/no-go from N5's data |

The order is by gain per unit of work and risk. N1 is local changes behind existing
interfaces, ordered by what N0 measured: the frame pool and the threshold first (small and
measured), then the caches where the 10 M generic property operations happen, then the two
engine-wide mechanical changes. N2 changes frames but keeps their layout visible to the collector. N3 touches
everything, but mechanically, and is safest after the measurement and test infrastructure
has caught the smaller changes. N4 needs N1's caches and N0's counters to be worth building,
and each later stage starts only on its predecessor's data.

## 8. Risks

- **8-byte values break code that assumes 16.** Native bindings, the DOM layer and the GC
  read values directly in places. Mitigation: make values opaque first (5.1, step 1), then
  switch the representation.
- **Stack frames and the collector.** A frame on the machine stack must still be found,
  marked, and never moved. Conservative stack scanning finds its values. The frame-chain
  record keeps backtraces and `arguments` working. GC stress and poisoning runs, with
  `QJS_GC_VERIFY`, gate N2.
- **Interpreter caches slowing cold code.** A cache lookup on a site seen once costs more
  than the plain lookup. Mitigation: allocate feedback vectors only for functions past a
  small threshold (V8's lazy feedback allocation).
- **The deterministic replays are a model of the page, not the page.** Under
  `CYBER_DET_TIME`, time advances by calls made rather than by the clock, and the event loop
  waits for in-flight loads at each turn. Pages take the same paths every run and the same
  path as when recorded, but a change that alters timing-dependent behaviour (how many
  intervals fire) would not show it. Mitigation: confirm a milestone's gain with a real-time
  replay (`REAL=1`) at the end, with more rounds.
- **The JS engine is not all of the time.** DOM and layout are 15–27% of the YouTube and BBC
  loads and 72% of the Wikipedia one, and nothing in this plan touches them. Gains are stated
  as a share of each load's JS-thread time, which includes them.
- **T2 guards that keep failing.** Mitigation: count exits per site, recompile without the
  guard after a few, and fall back to T1 for a function that keeps failing.
- **Two code generators (stencils and the T2 assembler).** Mitigation: T2's operation set is
  small, and both share the helpers, frames and caches. Differential testing in all modes
  (interpreter, T1, T2 stress) covers both.
- **Stage 1 alone gains little.** Block-local work is limited where blocks are short, which
  is why no engine keeps such a tier on its own. Mitigation: stage 1 is built as the start of
  the optimizer (an IR, the exit model, the code generator), so its work carries into stage
  2, and its counters show early whether stage 2 would pay.
- **The broad profile may stay broad.** If N1–N3 shrink the runtime costs but no hot code
  emerges, T2 may not pay for itself either. N0's counters and N4's exit criterion make that
  visible early, before the larger stages are started.

## References

- V8: [Ignition](https://v8.dev/blog/ignition-interpreter),
  [Sparkplug](https://v8.dev/blog/sparkplug),
  [Maglev](https://v8.dev/blog/maglev),
  [Pointer compression](https://v8.dev/blog/pointer-compression),
  [Lazy feedback allocation](https://v8.dev/blog/v8-release-77),
  [Short builtin calls](https://v8.dev/blog/short-builtin-calls),
  [Slack tracking](https://v8.dev/blog/slack-tracking).
- HHVM: Ottoni, *HHVM JIT: A Profile-Guided, Region-Based Compiler for PHP and Hack*, PLDI 2018.
- CPython: [PEP 744, JIT compilation](https://peps.python.org/pep-0744/) (tier 2 micro-ops and
  the copy-and-patch backend).
- `JIT_DESIGN.md` (T1, M0–M4) and `GC_PAUSELESS_DESIGN.md` (references, allocation, barriers).
