# Faster JavaScript after the baseline JIT

Status: N0–N3 done; N4 onward revised after the steady-state benchmarks (section 4.3).
Follows `JIT_DESIGN.md` (T1 and the first inline caches, M2 and part of M4) and builds on the
handle-free references of `GC_PAUSELESS_DESIGN.md`. It changes the order of the remaining work:
engine representation and runtime paths first, then feedback shared by all tiers, then a
speculative optimizer shaped like Maglev, entered from the interpreter.

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

The steady-state benchmarks (JetStream's Octane, Web Tooling Benchmark and other plain-JS
tests; section 4.3) confirm it on code that runs long enough for any JIT to pay. Against the
interpreter, T1 is 30–36% faster on tight numeric loops (crypto, navier-stokes,
gaussian-blur), up to 23% slower on code built from objects, calls, class fields, Proxy and
BigInts, and −0.4% over all 62 tests. Its first iteration is about as fast as its steady
state, so the loss is in each execution, not in compiling or warming up. A later start would
not change it. What T1 lacks is what only speculation gives: values kept unboxed in registers,
property accesses compiled to a shape check and a load, and calls made directly to a known
callee.

So the recommendation is:

1. **Freeze T1; do not build on it.** It stays in the build, off by default, tested in every
   mode, but gets no new work. The optimizer does not depend on it: its exits go to the
   interpreter, whose frames T1 shares. Whether T1 stays at all is decided when the optimizer
   is measured, interpreter + T2 against interpreter + T1 + T2 (section 6.6).
2. **Make the engine itself cheap, for every tier and every native call** (N1–N3, done):
   8-byte values;
   unchecked field loads on hot paths; fewer frame allocations, then calls without heap frames;
   an inline allocation fast path; caches for defining, adding and generically getting
   properties. This is where most of the remaining time is, and it speeds up the interpreter,
   T1, natives and the DOM bindings alike. The JS engine is not all of the time, though: DOM
   and layout code are 15–72% of the loads measured (section 4.2), and this plan does not
   cover them.
3. **Add feedback vectors:** per-function slots recording shapes, types and call targets,
   written by the interpreter and read by the optimizer. Their inline caches also serve the
   interpreter, for the sites that have none today (private fields, field definitions).
4. **Then one optimizer, T2, shaped like Maglev from its first version.** It builds a
   control-flow graph in SSA form straight from bytecode and feedback, specializes on
   feedback (int32 and double arithmetic, shape-checked loads, direct calls), keeps values
   unboxed in registers across blocks and loops, and exits to the interpreter at the current
   bytecode when a guard fails. Because frames are interpreter-compatible, that exit is a
   deoptimization that needs no separate machinery. The block-local first stage of the
   previous plan is dropped: the wins measured in 4.3 are in loops, which a block-local tier
   cannot keep in registers. Loop entry (on-stack replacement), then inlining and escape
   analysis, follow on measurement. The end state is the interpreter and T2, with T1 kept
   only if it measures a gain between them.

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
| **Sparkplug** (2021) | Bytecode to machine code in one pass, with no IR; interpreter-compatible frames; calls builtins for most operations | Exactly what T1 is. Sparkplug gains 5–15% on an engine whose values, calls and allocation are already cheap; T1 gains nothing measurable on ours (section 4.1), and −0.4% over the steady-state suite (4.3). Since T2 exits to the interpreter, T1 is not needed as an exit target either; it is frozen until T2 shows whether a middle tier pays (6.6) |
| **Maglev** (2023) | CFG SSA built from bytecode and feedback; speculation with deoptimization; about 10× slower to compile than Sparkplug, 10–100× faster than TurboFan | The target design for T2. Its cost (frame states, deoptimization, a register allocator, inlining) pays only where hot, type-stable code exists; the steady-state suite (4.3) shows where that is. T2 starts from its graph, speculation and exits, and adds loop entry, inlining and escape analysis on measurement (section 6) |
| **TurboFan / Turboshaft** | The top tier | Out of scope |
| **Compact values and pointer compression** (2020) | Small integers and 32-bit compressed pointers in tagged words; about 40% less heap | We have compressed references already; our values are still 16 bytes. Section 5.1 |
| **Cheap calls** | JS frames live on the machine stack; builtins are called with a fixed convention ("short builtin calls"), with no heap objects per call | Our frames, JS and native, are GC objects (pooled per depth) that every call initializes and every return releases. Section 5.3 |
| **Fast allocation** | Bump-pointer allocation inline in generated code; a parallel scavenger for the young generation | Our allocation path is a C call with alignment and hole handling (`gc_align16`, `alloc_cross`). Section 5.2 |
| **Embedded builtins** | Common runtime functions written once, callable from every tier at low cost | Our helpers (`jsop_*`, `jit_*`) play this part; the cost of reaching them is what 5.3 cuts |

Two more precedents bear on T2's exit model:

- **HHVM's region JIT** compiles "tracelets" specialized on observed types, with side exits to
  the interpreter.
- **CPython 3.13's tier 2** builds superblocks of micro-ops with guards whose exits return to
  the interpreter.

Both get part of an optimizer's benefit without SSA, by using the interpreter as the
deoptimization target. Our interpreter-compatible frames make that unusually cheap here.
T2 takes the exit model from them, but not their limit to straight-line regions: its graph
covers the whole function from the first version (section 6).

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
| Feedback vectors with caches in the interpreter (5.4) | Uncached private-field and definition sites; call-site targets | 1–3% on pages; up to 10% on class-heavy benchmarks |
| T2 (6.1, 6.2) | Tag checks, stack traffic, helper calls and call setup in hot code, across blocks and loops | 2–5% on pages; 25–40% steady state on the benchmarks' hot loops (4.3) |
| T2 inlining and allocation sinking (6.3) | Calls to small callees; short-lived objects | Unknown until T2's counters |

Together, the rows before T2 are the bulk of a plausible 20–30% of the JS-heavy loads' JS-thread
time. None of them is a compiler. On pages dominated by DOM and layout, such as the Wikipedia
article (72% DOM and layout), the gain is proportionally smaller. The page loads are mostly
code that runs once, so they bound what any compiler can gain on loads; the steady-state
benchmarks (4.3) are where a compiler shows what it can do, and both are measured for each
milestone from N4 on.

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

### 4.3 Steady-state benchmarks

The replays are page loads: most of their code runs once or a few times, which is what a
browser mostly does, but it cannot show what a compiler gains on code that runs long enough
to be optimized. For that, the standard suites are used as they are, with no benchmark
written here.

- **`tools/jsbench.js`** loads JetStream's own driver (`JetStreamDriver.js`, from a checkout of
  WebKit's JetStream outside the repository) and captures, for each test, the script its
  command-line runner would evaluate: the files, preloads, iteration counts, deterministic
  `Math.random` and runner code, exactly as JetStream assembles them. Each is written out as a
  standalone script, with a prefix that stands in for the shell, and run through
  `CYBER_PROBE_JS` in a fresh process. JetStream includes Octane and the Web Tooling Benchmark,
  so one harness covers all three suites. Tests that need WebAssembly, workers or modules are
  left out.
- **`tests/jit/jsbench.sh`** runs the tests under several settings, interleaved and rotated as
  in `replay.sh`. Each run reports every iteration's time, reduced to the first iteration
  (startup), the mean of the last half (steady state) and the total.
  **`tools/jsbench_stats.js`** reports each setting's medians and its change from the first,
  with 95% bootstrap intervals, and the geometric mean over the tests.
- **Coverage:** 62 of JetStream's 68 plain-JS tests run. Basic and WSL need the legacy
  `RegExp.lastMatch` statics, the two babylonjs-scene tests need browser APIs a probe script
  lacks, source-map needs `WebAssembly.instantiate`, and OfflineAssembler exceeds 15 minutes
  interpreted.

Bringing the suite up found three engine bugs, all older than N3: a GC crash when a WeakMap's
state outlived its object (babel), class fields named `async` rejected by the parser
(esprima-next), and a lazily parsed body that took the parameter of `function of(n)` for a
declaration of its own, hiding the enclosing `n` (lebab's lodash). One interpreted run of Air
in three crashed; it is open.

**T1 against the interpreter** (3 rounds, 8-byte values, threshold 50,000):

| Kind of code | Tests | Steady state, T1 |
|---|---|---|
| Integer and bitwise loops over arrays | stanford-crypto-aes, -sha256, -pbkdf2, crypto, navier-stokes, gaussian-blur | −29% to −36% |
| Numeric code with objects | Box2D, delta-blue, raytrace, ai-astar | −7% to −14% |
| Objects, calls, class fields, closures | raytrace-private-class-fields, chai, UniPoker, Air, ML, mobx, js-tokens, prettier, acorn | +7% to +20% |
| BigInts | bigint-bigdenary, bigint-paillier | +6% to +23% |
| Everything else | 41 tests | within ±7%; 19 tests within noise |
| **All 62 (geometric mean)** | | **−0.4%** (first iteration −0.1%) |

T1 is faster on 13 tests and slower on 30, beyond each one's interval.

**Where T1 loses** (CPU samples, raytrace-private-class-fields and UniPoker):

- **Calls.** Compiled-to-compiled calls go through `jit_call_prepare_kind`, `jit_call_prepare`,
  `jit_call_finish` and frame entry: about 12% of raytrace-private-class-fields under T1. The
  interpreter makes the same calls inside `JS_CallInternal`, cheaper.
- **Sites with no cache are as slow in both tiers.** Private fields (`find_own_property` from
  `JS_GetPrivateField`, 11–14% in both tiers), field definitions (`jsop_define_field`) and
  bitwise operators on values outside int32 (`js_binary_logic_slow`) take the same runtime
  path; T1 adds its stencils' frame loads and stores (`jit:get_loc`, `jit:put_loc`) on top.
- **GC is 15–29% of these runs in both tiers**, and property lookups 10–26%: costs no tier
  removes without caches and allocation work.

**What it means:**

- **Tiering T1 later would not help.** It already compiles only after 50,000 entries or loop
  iterations, and on these tests its first iteration is as fast as its last: the code is
  compiled early and stays slower. A non-speculative tier cannot keep a value in a register
  across a call, prove a type once, or skip a generic call path, which is where the time goes.
- **What wins is specialization:** the six tests where T1 gains a third are those where a
  stencil's fast path (int32 arithmetic, array element access) is all the work. A speculating
  compiler gets that on every kind of site feedback has made monomorphic.
- **Caches pay in every tier.** Private-field and definition sites need feedback slots read
  by the interpreter too, not only by compiled code.

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

**Step 1 as built (N3).** `GCValue`'s fields are reached through `GCV_U(v)` and `GCV_TAG(v)`
in the accessor layer (`quickjs_types.h`, `quickjs.h`). Configured with `-DQJS_OPAQUE_VALUES`,
the fields get other names, so any direct `.u` or `.tag` on a value is a compile error. The
`build-opaque` configuration builds everything, the browser and tests included, and passes
the test suite. Direct accesses were few: 15 in all (6 in `gc_value_helpers.h`, 4 redundant
stores, the bytecode fingerprint, the JIT's payload/tag immediates, one stencil and a test),
now through the accessors.

What step 2 has to change hides from that check, because it uses the layout without naming
the fields:

| Where | Sites | What changes |
|---|---|---|
| Tags compared with `JS_TAG_FLOAT64` | 24 `case` labels (17 under `switch(tag)` of a raw tag), 28 `==`/`!=`, 27 tag variables read raw | With NaN boxing a double has no single tag: these read `JS_VALUE_GET_NORM_TAG` (54 places already do) |
| The JIT | 62 uses of the stencils' value helpers (`vset`, `vld`, `vcopy`…), the payload/tag immediates, `jit_value_load`, the layout asserts | Values become one 8-byte move: the helpers and the immediates change, and the store-forwarding workaround goes |
| The GC | A conservative scan reading a tag word at +4 (`gc_concurrent.c:407`), the verifier's two scans for the 16-byte pattern | Read the 8-byte encoding |
| `JSProperty` | The `init` member (16 bytes) is unused since AUTOINIT stores nothing | Remove it, so property slots shrink to 8 bytes with values |
| Short big ints | `JS_SHORT_BIG_INT_BITS` is 64 | 32 bits fit in a NaN box, as upstream QuickJS has it |
| `quickjs.h` | A dead NaN-boxing branch for 32-bit pointers (it typedefs `GCValue` as `uint64_t` and does not compile) | The starting point for the encoding, or deleted |

The browser side barely notices: 9 raw tag reads in 5 files, all through the macros. Nothing
outside the engine touches the fields. Step 2 is therefore concentrated in `quickjs.c`
(about 80 tag sites to review), the stencils and three GC scans, with the deterministic
replays and the four-mode differential tests to check it.

**Step 2 as built (N3): 8-byte values are the default.** `QJS_VALUE8` (the CMake default;
`-DQJS_VALUE16=ON` keeps the old layout) makes `GCValue` a one-word struct, NaN-boxed as
upstream QuickJS's `JS_NAN_BOXING`: `(tag << 32) | payload` for ints, bools, handles, catch
offsets and short big ints. Doubles are stored offset by `GC_FLOAT64_TAG_ADDEND << 32`, which
moves every double's upper word out of the tags' window once NaNs are canonical. It stays a
struct, so values cannot be added or compared with `==` by mistake. Most of the engine was
already written for this, inherited from upstream: its switches default to
`JS_TAG_IS_FLOAT64(tag)`, and its range checks are unsigned. What had to change:

- **Signed reference tests.** A double's raw tag is usually negative, so `tag < 0` would take
  it for a reference. `GC_IS_REFERENCE` became the unsigned test, and nine browser sites that
  wrote `GC_VALUE_GET_TAG(v) < 0` use it. In the DOM's root marking those would have marked
  random handles.
- **Four float tag checks** read the raw tag (`tag_is_number`, a constant's atom, the
  bytecode fingerprint, a debug printer); the other 50-odd already used the normalized tag.
- **Raw layout:** 16-byte atomics on value slots (`gcv_atomic_load`/`store`/`cas` now),
  `jit_value_load`'s two halves, the GC verifier's and barrier check's value scans, and the
  JIT's constant immediates. The stencils get a second header
  (`tools/jit_stencils.js --value8`), and their value helpers are single moves.
- **`JSProperty`** lost its unused AUTOINIT payload, so a property slot is 8 bytes too.
- **Short big ints are 32-bit, and limbs too** (as upstream with NaN boxing), because the
  BigInt code assumes a one-limb value fits a short one.

The BigInt check found three bugs that were already in the 16-byte build. `GC_VALUE_GET_SHORT_BIG_INT`
read 32 of the 64 bits (`2n ** 31n` printed `-2147483648`, `2n ** 32n` printed `0`).
`js_bigint_shr` shifted the wrong limbs into negative values (`(-(2n ** 70n)) >> 3n` was
positive). `BigInt.asUintN` returned negative values unchanged when `bits` was at least
their width (`asUintN(64, -1n)` was `-1n`). All three are fixed, and `tests/jit/bigint.js`
checks against known answers.

Results (24 rounds against the 16-byte build, the same calls and DOM nodes, deterministic over
repeated runs, clean under GC stress, poisoning and `QJS_GC_VERIFY`):

| Recording | JIT on | Interpreter | Bytes allocated |
|---|---|---|---|
| YouTube watch | −12.4% | −17.4% | −21% (objects −24%, property and array storage −40%) |
| YouTube home | −11.6% | −13.5% | |
| YouTube search | −11.0% | −16.0% | |
| BBC | −3.5% | −3.8% | |
| Wikipedia | −2.0% | −3.4% | −24% |

**The JIT now loses to the interpreter**: 10.5 G against 9.9 G cycles on the watch page. The
interpreter's value copies got cheaper, and the stencils, still the copy-and-patch design of
M2, did not gain as much. **So T1 is off by default again** (`QJS_JIT=1` turns it on), until
the optimizer (section 6) gives compiled code something the interpreter cannot match. Same
binary, 24 rounds, the interpreter against T1: −5.5% (watch), −4.0% (home), −7.3% (search),
BBC and Wikipedia at parity. T1 stays in the build, tested in every mode; T2 exits to the
interpreter, and whether T1 stays is decided when T2 is measured (6.6).

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
  (transition: old shape → new shape, slot), getters (holder, getter function), and private
  fields and brands (`get_private_field`, `put_private_field`, `define_private_field`: shape
  → slot), which today take a shape-table lookup on every access in every tier (4.3).
- **Call sites:** the callee (monomorphic or megamorphic), and whether it is compiled.
- **Arithmetic and comparison sites:** the operand kinds seen (int, double, string, other),
  as bits.
- **Allocation sites:** the final property count (5.2).

The interpreter reads and writes the property and call slots on its fast paths, as T1 does
now. Immutable shapes (M4) make the interpreter's cache pay, which the reverted October
experiment showed it could not before. The interpreter is the tier that records feedback:
with T1 off by default, it is also the tier that runs everything T2 has not compiled, so its
caches are the ones that matter until T2 exists. T2 reads the type, shape and call-target
slots to decide what to specialize.

Each change is measured on both benchmarks: the replays (24 rounds, as in N1) and the
steady-state suite (4.3), interpreted. The feedback slots cost a little on cold code; the
replays show whether lazy allocation keeps that below the gain.

**As built so far (N4, not yet gated).** A function gets its vector (`JSFeedback`) once its
hotness passes about 8 calls, counted in every mode. It holds one cache per `get_field`,
`get_field2`, `get_length`, `put_field`, `get_private_field` and `put_private_field` site,
and a map from each instruction's offset to its site; it lives in old space on the GC heap.
The interpreter checks way 0 inline. Ways cover own and prototype properties, absent
properties, getters (called directly), writes to existing properties, adds, and private
fields. A site past four shapes uses no cache.

- **Weak, not retained.** Reusing T1's retention set made Air 22% slower: the JS thread
  marked every shape the caches had ever named, each cycle (mark start and remark rose from
  47 to 248–294 ms). The interpreter's ways hold their shapes and holders weakly instead,
  and the weak step of every collection clears the ways naming condemned objects before
  the sweep can free their handles (`js_feedback_clear_dead`). T1 keeps its own caches.
- **Megamorphic sites skip the cache.** acorn sends half its gets through sites with more
  shapes than ways, where the shared megamorphic table cost more than the plain lookup.
- **Call targets and arithmetic kinds move to N5.** Only T2 reads them; recorded now they
  would cost the interpreter's tightest paths with nothing to gain. N5 records them in
  functions that are candidates for T2.
- **Tests:** `tests/jit/feedback.js` changes what each kind of way assumed (prototype
  changes, getters, setters and read-only properties appearing after an add is cached,
  frozen and non-extensible objects, private names, shapes dying under GC) and matches
  node, feedback off and GC stress.
- **First numbers** (8 tests, 3 rounds, before getters and puts were cached): richards
  −6%, delta-blue −7% to −11%; acorn and chai +4% to +8%, before megamorphic sites skipped
  the cache. The gate (the full suite and the replays) is still to run.

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
the build, tested in every mode.

N1 found 50,000 best on the replays (5.6), and after N3 the interpreter beat T1 there, so T1
is off by default (5.1). The steady-state suite (4.3) settles the threshold question: on code
that runs for seconds, T1 compiles early at any threshold and is as slow in its last iteration
as in its first, so no threshold makes it a gain where it loses. Its future is decided with
T2's (6.6), not by tuning.

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
result T1 was turned on by default, until N3 (section 5.1).

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

## 6. The optimizer

V8 runs four tiers: Ignition, Sparkplug, Maglev and TurboFan. JavaScriptCore and SpiderMonkey
also put an optimizer between their baseline and top tiers. What each optimizer gains over its
baseline is the same: values kept unboxed in registers, a type or shape checked once rather
than at every use, property accesses compiled to a check and a load, and calls made straight
to a known callee. A baseline compiler can do none of it, and section 4.3 measures the
consequence here: T1 gains a third where a stencil's fast path is all the work, and loses
where the work is calls, objects and uncached sites.

So the third tier is **one optimizer, T2, shaped like Maglev from its first version**. The
previous plan built it in stages, starting with a block-local tier (a virtual stack and
registers within a basic block). That stage is dropped. The gains measured in 4.3 are in
loops, and a block-local tier writes every value back to the frame at each block's end, so it
would keep nothing in a register across a loop iteration; no engine runs such a tier for that
reason. Building the graph in SSA form from the start costs little more than a virtual stack
(Maglev builds it in one pass over the bytecode, as below) and is what every later step needs.

What makes this affordable is the exit model. T1 and the interpreter share frames, so a failed
guard can exit to the interpreter at a bytecode boundary: it writes back the values the
interpreter's frame should hold and sets `pc`. That *is* a deoptimization. Its frame state,
meaning where each interpreter slot's value lives at that bytecode, is the concept Maglev
records, but simpler, because the target is always the interpreter's own frame. T2 needs no
other deoptimizer, and it does not need T1.

**Input:** the bytecode, plus the feedback vector (5.4).

**Output:** machine code that uses the interpreter's frame layout, like T1.

### 6.1 T2: the graph, speculation and exits

- **Graph building in one pass.** T2 walks the bytecode once, in order, simulating the
  operand stack and locals abstractly: each slot holds the SSA value that defines it. `get_loc`,
  `put_loc`, `dup`, `swap` and constants make no nodes. Basic blocks start at jump targets and
  exception handlers; a loop header gets a phi for each slot the loop writes, filled when its
  back edge is reached. This is how Maglev builds its graph, with no separate SSA
  construction pass.
- **Nodes.** A short list: load and store frame slot, int32 and float64 arithmetic with
  overflow guards, compares and branches, tag and box/unbox conversions, guard kind, guard
  shape, load and store property slot, load and store array element (with a bounds guard),
  call helper, call compiled code, call builtin, return, and exit. Every node that can fail
  carries the frame state of its bytecode: for each interpreter slot live there, the SSA value
  that holds it.
- **Speculation from feedback.** An arithmetic site that has seen only int32s becomes one kind
  guard per operand and untagged int32 arithmetic, with an overflow guard; doubles become
  float64 arithmetic. A property site that has seen one shape becomes a shape guard and a
  slot load or store; a few shapes, a short chain of compares. A call site with one target
  becomes a target guard and a direct call. Sites feedback calls megamorphic, or has never
  seen, stay calls to the same runtime helpers T1 and the interpreter use.
- **Representation selection.** Each value is tagged, int32 or float64. A pass over the graph
  picks, per value, the representation its uses want, so a loop's int32 induction variable and
  a float64 accumulator stay unboxed across the loop, and boxing happens only where a value
  escapes to the frame, a helper or an exit.
- **Removing checks.** A value a guard has proven needs no second guard, in any block it
  dominates. Shape guards on an object not written in a loop move to the loop's preheader.
  Redundant loads of the same slot of the same object, with no store or call between, are
  shared. This is the "simple global optimizations" list, kept short on purpose.
- **Exits.** A failed guard jumps to a per-site exit stub. The stub boxes the frame state's
  values, writes them to the interpreter's frame, sets `pc` to the instruction's start, and
  continues in the interpreter, which runs the instruction and updates the feedback. A site
  whose exit keeps firing makes the function recompile without that speculation; a function
  that keeps exiting stays interpreted (or in T1, if T1 stays).
- **Calls.** A direct call to a compiled callee enters it with the interpreter's frame layout
  on the JS stack (5.3), with no `jit_call_prepare` path: the target guard proved what the
  generic path checks. Calls to small builtins that feedback identifies (`Math.*`,
  `charCodeAt`, array `length` and `push`) are inlined behind the target guard.
- **Register allocation:** linear scan over the graph, in block order, with values live across
  calls spilled to their frame slots (which the interpreter and the collector already
  understand).
- **Exceptions and generators.** A function with `try` compiles; a throw exits to the
  interpreter's handler lookup like any exit. Generators, async functions and functions using
  `with` or direct `eval` are not compiled by the first version.

Compiling stays roughly linear: one pass to build the graph, one for representations and
checks, one for registers, one to emit. It runs on the JS thread at first; moving it to a
background thread is an option once its cost is measured.

### 6.2 Loop entry

The steady-state tests call their hot functions many times, so entering T2 at function entry
is enough for them. Pages do not: a loop in a function that runs once (initialization,
parsing a response, building a list) never re-enters it. T2's second step is **entry at a loop
header** from the interpreter (on-stack replacement): at a back edge whose counter passes the
threshold, the function is compiled with an extra entry at that header, which reads the
header's frame state in the other direction, loading each live slot from the interpreter's
frame and unboxing it under a guard. Because the frames are shared, no frame is converted.

### 6.3 Inlining and allocation

If T2's counters show hot calls to small monomorphic callees, and short-lived objects in hot
code:

- **Inlining** of small monomorphic callees. A frame state then describes a stack of
  interpreter frames, and an exit materializes the inlined frames before entering the
  interpreter.
- **Escape analysis and allocation sinking.** An object that never leaves the function is
  kept in registers and allocated only on the exits that need it. This is the one
  optimization that attacks GC directly (15–29% of the steady-state tests, 21% of the YouTube
  loads).

### 6.4 Code generation

Register allocation doesn't fit copy-and-patch well: stencils pin their operands to fixed
registers, and every combination would need its own variant. T2 therefore uses a small macro
assembler per target. It emits x86-64 first, then arm64, from the graph's short list of
nodes. It is the one new code generator; it does not share T1's stencils, only the runtime
helpers, frames and caches they call.

### 6.5 Tiering

- **Interpreter:** runs everything first and records feedback. A function's feedback vector is
  allocated after a few calls (lazy feedback allocation), so code that runs once pays nothing
  for it.
- **Interpreter → T2:** when a function's budget, counted in calls and loop iterations
  weighted by its size (as V8's interrupt budget), passes the threshold, and its feedback has
  settled, meaning no site changed state since the last check. The threshold is measured on
  both benchmarks, starting near V8's (on the order of 10^4–10^5 executed instructions).
- **T2 → interpreter:** a guard failure exits for that execution. A function whose guards fail
  repeatedly recompiles once without the failing speculation, then stays interpreted for a
  while before T2 tries again with fresher feedback.
- **Loop entry** (6.2) uses the same budget, counted at back edges.

### 6.6 T1's future

T1 is frozen: off by default, kept correct and tested in every mode (`QJS_JIT=1`), with no
new features. When T2 is measured (N5's gate), three configurations are compared on both
benchmarks: the interpreter alone, the interpreter + T2, and the interpreter + T1 + T2 (T1 for
warm functions below T2's threshold, and for functions T2 does not compile or gave up on).

- If T1 adds a measurable gain on top of T2, it stays as the middle tier, the role Sparkplug
  has in V8, at a threshold measured then.
- If it does not, it is removed: the stencil extraction, its two stencil headers (16- and
  8-byte values), its code cache and its test modes. One compiler and an interpreter are much
  less to maintain than two compilers.

Either way, nothing in T2 waits for this decision.

### 6.7 T2's interfaces (N5, step 5b)

N5 is built in steps that each work on their own: 5a records call and arithmetic feedback in
functions that are T2 candidates; 5b builds the compiler's skeleton with no speculation; 5c
adds int32 and float64 speculation; 5d shape guards and inline property access, and array
element kinds (an array known to hold only int32s or only doubles, with no holes, read and
written with a bounds check and no tag test); 5e direct calls; 5f register allocation across
blocks and check removal. 5b gains nothing by itself.
What it settles is the part every later step depends on: entry, exits, frame states, code
generation and agreement with the interpreter in every mode.

**Files.** `quickjs_t2.c` (graph building, passes and code emission), `quickjs_t2_x64.h` (a
macro assembler for x86-64), `quickjs_t2.h`. Code memory, unwind registration and the
profiler's map come from `quickjs_jit.c`, which exports allocation and unwind entry points.

**The code is an ordinary Win64 function**, `int f(JSInterpFrame *F)`, with a prologue that
saves the callee-saved registers it uses and one `RUNTIME_FUNCTION` describing it, so the
OS, the sampler and C++ exceptions unwind through it like compiled C. It needs no trampoline:
`JS_CallInternal` calls it directly where it would enter T1 (`restart:`), and handles its
result as T1's: `JSOP_NEXT` continues the same frame in the interpreter at `F->pc` with
`F->sp`, `JSOP_EXCEPTION` goes to the interpreter's handler lookup, `JSOP_DONE` returns.
`JSFunctionBytecode.t2_code` holds it (before `debug`, so stripped bytecode keeps it).

**Registers.** `rbx` holds `F`; `r12`, `r13`, `r14` hold `arg_buf`, `var_buf` and
`stack_buf`. `rsi`, `rdi`, `rbp` and `r15` hold values; so do the caller-saved registers
between calls. In 5b a value is always a tagged 8-byte `GCValue`, kept in a slot of the
native frame from its definition to its last use (the conservative stack scan sees it there
across helper calls); from 5c an int32 may be unboxed in a general register and a float64 in
an XMM register, and 5f allocates registers.

**The graph.** One block per bytecode basic block, each a list of nodes in SSA form. A node has
an operation, its inputs, the representation of its result (tagged, int32, float64, none), and
the offset of the bytecode instruction it belongs to. Building is one pass in bytecode order
with a map from each interpreter slot (argument, local, operand-stack entry) to its value:

- **A slot is either in the frame or held by a value.** Reading a slot that is in the frame
  makes a load node; `get_loc`, `put_loc`, `dup`, `swap`, constants and the like only update
  the map. A slot written since it was last stored is dirty.
- **A generic node** calls the instruction's helper `jsop_<op>(F)`, the code the interpreter
  itself runs. Before it, the dirty slots are stored and `F->sp` and `F->pc` set; after it,
  every slot is in the frame again, because a helper (or a function it calls, through a
  closure's reference to a captured local) may have written any of them. From 5c, slots the
  bytecode says no closure captures stay in registers across helpers.
- **Block boundaries.** In 5b every slot is stored at the end of a block, so a block starts
  with all slots in the frame and needs no phis. The graph has phis from the start: 5f keeps
  values in registers across blocks by giving each block parameters for the slots its
  predecessors leave in registers.
- **Branches** (`goto`, `if_true`, `if_false` and the fused compare-and-branch forms) are
  modelled; truthiness is tested inline for booleans and int32s and through `jit_truth`
  otherwise.

**Frame states and exits.** Every node that can leave compiled code (a helper that threw, and
from 5c a failed guard) records the frame state of its instruction: the stack depth and, for
each slot that is dirty there, the value that holds it. Its exit stub stores those values,
boxing unboxed ones, sets `F->sp` and `F->pc` to the instruction's start, and returns
`JSOP_NEXT` (or `JSOP_EXCEPTION`). The interpreter then runs the instruction itself. One
mechanism serves the exceptions of 5b, the guards of 5c and, with entry at loop headers read
the other way, on-stack replacement.

**What 5b compiles.** Functions whose instructions are all either modelled or have a helper
that does not jump (`jsop_jumps`). Functions with `try`, `with`, `gosub`, generators and
async functions are left to the interpreter at first. A function T2 cannot compile is marked
and never tried again.

**Modes and checks.** `QJS_T2=1` compiles functions past a hotness threshold; `QJS_T2=stress`
compiles every function at its first call, which runs the whole test suite through T2;
`QJS_T2_LOG=1` reports what was compiled and why not. `QJS_T2_VERIFY=1` checks, at each
return to the interpreter, that `F->sp` matches the stack depth the bytecode has at `F->pc`.
`tests/jit/run.sh` gains the T2 modes, so every JIT test compares T2's output with the
interpreter's.

**5c as built: arithmetic speculation.** `add`, `sub`, `mul`, `div`, the comparisons and
equalities, the bitwise operators and shifts, `neg`, `inc`/`dec` in all their forms
(`post_inc`, `inc_loc`, `add_loc`) and `lnot` of a known boolean are speculated on int32s,
or on float64s once a site has seen a number outside int32. Values carry a representation
(tagged, int32, boolean, float64) and are boxed only where they reach the frame: a helper,
a block's end, a return, an exit. A branch on a comparison tests its boolean directly.

- **Feedback from exits, not from the interpreter (5a folded in).** Rather than have the
  interpreter record operand kinds at every arithmetic instruction, T2 speculates int32
  first. A failed guard stores the frame, and `t2_deopt` looks at the instruction's
  operands: both numbers mark the site float64, anything else marks it generic, in the
  site's state byte in the feedback vector. The code is dropped and the function compiles
  again when it is hot again. A site only widens (int32, then float64, then generic), so a
  function recompiles at most twice per site; past 16 drops it stays interpreted. The
  interpreter's arithmetic is untouched and pays nothing.
- **Guards:** an int32 is a tagged value whose upper word is 0 (one unsigned compare); a
  float64 is one whose upper word lies outside the tags' window. Overflow, `-0` from `mul`
  and `neg`, and a `>>>` result above INT32_MAX exit; such a `>>>` site then uses its
  helper (Uint32 values are not represented yet).
- **An exit is made before its guard**, because making it loads any slot that aliases
  another; the compiler refuses a graph where an exit would store a value computed after
  its guard. (The first version made it after, and exits stored unloaded slots:
  `tests/jit/t2arith.js` found it.)
- **Tests:** `tests/jit/t2arith.js` warms each operation on int32s, then sends overflow,
  `-0`, NaN, doubles, strings, objects and booleans, in both T2 modes.

**5d as built: properties and arrays.** `get_field`, `get_field2` and `put_field` compile
inline from the site's feedback cache when its ways can be: up to four shapes, own data
properties, and for gets also prototype data properties and absent ones (each behind the
prototype epoch check of `jit_proto_epoch`, and not for an exotic receiver). The code tests
the receiver's tag and shape against the ways and loads or stores the slot directly,
marking the card on a store; a watched object exits. `get_array_el`, `get_array_el2` and
`put_array_el` on a fast array (class `Array`, `fast_array` set) with an int32 index below
its count load and store the element inline.

- **Compiled code keeps what it names alive.** The interpreter's caches hold shapes weakly
  (5.4), but compiled code compares against shape handles and reads holders by handle: a
  dead shape whose handle was reused would match the wrong layout. The shapes and holders a
  T2 site compiles inline go in the inline caches' retention set, as T1's do; it is bounded
  by compiled code, not by everything the interpreter ever saw.
- **Exits.** A shape miss, a stale epoch, a non-object or a watched object drops the code;
  the interpreter fills the missing way, and the next compilation includes it. Array
  accesses have a state byte: a failed array guard marks the site generic.
- **Not yet:** getters, adds and private fields stay helper calls; element kinds (an array
  known to hold only int32s or only doubles, read without a tag test) are not tracked, so an
  element read is tagged and unboxed by the arithmetic that uses it; typed arrays use their
  helpers.
- **Tests:** `tests/jit/t2props.js` (polymorphic and fifth shapes, prototype changes and
  shadowing, absent properties defined later, getters added after compiling, read-only,
  frozen and accessor receivers, adds and deletes, out-of-bounds, holey, frozen, typed and
  non-array element accesses, shape churn under GC). The `t2` mode of `tests/jit/run.sh`
  lowers the feedback threshold, so caches fill before T2 compiles.

**Where T2 stands after 5d** (12 tests, 2 rounds, steady state against the interpreter, at
the default thresholds):

| | T2 | T1 |
|---|---|---|
| navier-stokes | −61% | −33% |
| crypto | −42% | −29% |
| stanford-crypto-aes | −39% | −33% |
| Box2D | −9% | −12% |
| richards | −4% | −4% |
| delta-blue | +3% | −2% |
| gaussian-blur | ±0 | −31% |
| Air, acorn, chai, earley-boyer, raytrace | +14% to +19% | +1% to +35% |
| **Geometric mean** | **−10.5%** | **−9.2%** |

T2 now beats T1 on arithmetic-heavy code and over the twelve. What remains: calls, still
through the generic helper and the interpreter's frame setup (5e: direct calls); values
stored to the frame at every helper and block end (5f); and gaussian-blur's hot loop, which
runs in a function called once, so T2 never enters it (loop entry, 6.2).

## 7. Milestones

| | Work | Exit criterion |
|---|---|---|
| N0 | Cycle-based timing, four more recordings, per-subsystem counters (section 4) | A 1% change is detectable on every recording |
| N1 | In this order, each measured alone (section 4.1): a frame pool that does not miss; T1's threshold; generic get and define/add caches in the runtime, `put_field`/`define_field` caches in compiled code; unchecked hot-path loads; inline allocation fast path | −8% JS-thread cycles on the YouTube replays; parity on all five. **Met** (section 5.6): −10.5% to −12.7% |
| N2 | Calls without heap frames for simple callees; lighter native calls; call caches; lazy function prototypes and the allocator's line crossing (5.6) | −6% more; parity; GC stress clean. **Partly met** (5.3): −3% to −5% with the JIT on, −5% interpreted; parity; GC stress clean |
| N3 | 8-byte values engine-wide | −6% more; memory down; all four JIT modes and replays at parity. **Met** (5.1): −11% to −12% with the JIT on, −13% to −17% interpreted, 21% fewer bytes allocated |
| N4 | Feedback vectors (5.4), read and written by the interpreter: property sites (including private fields and definitions), call targets, arithmetic kinds; lazily allocated. The steady-state suite (4.3) made a gate: the Air crash fixed, its spread measured | Interpreted: −5% geometric mean on the steady-state suite, −2% more on the JS-heavy replays, parity on the others; feedback that T2 can read, with counters for how many sites are monomorphic |
| N5 | T2 (6.1), x86-64: graph built from bytecode and feedback, speculation with exits to the interpreter, representation selection, check removal, direct calls, linear scan; entry at function start. T1's future decided (6.6) | Against the interpreter: −25% geometric mean steady state on the suite, no test more than 5% slower; −3% more on the JS-heavy replays, parity on the others; all modes and GC stress clean |
| N6 | Loop entry (6.2); T2's threshold measured; arm64 | Loop-heavy replays and run-once loops gain; arm64 passes every mode |
| N7 | Inlining and escape analysis (6.3) | Go/no-go from N5's and N6's counters (hot small callees, short-lived objects) |

The order is by gain per unit of work and risk. N1 is local changes behind existing
interfaces, ordered by what N0 measured: the frame pool and the threshold first (small and
measured), then the caches where the 10 M generic property operations happen, then the two
engine-wide mechanical changes. N2 changes frames but keeps their layout visible to the collector. N3 touches
everything, but mechanically, and is safest after the measurement and test infrastructure
has caught the smaller changes. N4 pays on its own, in the interpreter that now runs
everything, and gives T2 its input. N5 is the largest step, a new compiler, and is judged
mostly on the steady-state suite, where a compiler can show its effect; the replays guard
against it costing more than it gains on code that runs once. N6 brings T2 to that code.

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
  speculation after a few, and leave a function that keeps failing in the interpreter.
- **T2 is a large first step.** Dropping the block-local stage means the first T2 already has
  a graph, representations and a register allocator. Mitigation: its first version compiles a
  subset (no generators, async functions, `with` or direct `eval`; megamorphic sites as helper
  calls), with a stress mode that compiles every function at its first call and compares each
  exit's frame state against the interpreter's (`QJS_T2_VERIFY`), and the differential tests
  of every mode. Graph building and exits are tested first, with no speculation, before guards
  are added.
- **Two code generators while T1 lives.** Mitigation: T1 is frozen, so only T2's assembler
  changes; both share the helpers, frames and caches; 6.6 removes T1 if it does not pay.
- **The benchmarks are not the pages.** JetStream measures code that runs for seconds; pages
  run most code once. A T2 tuned on the suite could cost more than it gains on loads.
  Mitigation: every gate includes the replays, with "parity on the others" as a hard
  condition, and the threshold is measured on both.
- **The broad profile may stay broad.** If the pages show no hot code even after N4, T2 gains
  on them only through loop entry. N4's counters (monomorphic sites, hot loops in run-once
  functions) make that visible before N6.

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
