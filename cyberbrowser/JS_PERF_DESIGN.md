# Faster JavaScript after the baseline JIT

Status: proposal. Follows `JIT_DESIGN.md` (T1 and the first inline caches, M2 and part of M4)
and builds on the handle-free references of `GC_PAUSELESS_DESIGN.md`. It changes the order of
the remaining work: engine representation and runtime paths first, then feedback shared by all
tiers, then an optimizer built in stages, each started only when the previous one's data asks
for it.

## Summary

The copy-and-patch tier did what a baseline tier can do. It removed interpreter dispatch: the
interpreter loop's own time on the YouTube replay fell from 465 ms to 64 ms. The page's JS
thread barely got faster, because dispatch was a small part of its time and the JIT's own
costs (stencils, call setup, compiling) took most of what dispatch gave back.

Chrome teaches the same thing. Sparkplug, V8's non-optimizing compiler, was reported at 5–15%
on Speedometer and real-world benchmarks, on top of an engine whose values, objects, calls and
allocation are already cheap. V8's speed comes mostly from those, and from feedback (inline caches recorded in
every tier and used by the next one), not from its baseline compiler.

So the recommendation is:

1. **Keep T1.** It is cheap, correct, interpreter-compatible, and the exit target for any
   later tier. Do not rewrite it in assembly or as a Sparkplug clone: that buys little.
2. **Make the engine itself cheap, for every tier and every native call:** 8-byte values;
   unchecked field loads on hot paths; calls without heap frames; an inline allocation fast
   path; caches for adding and defining properties. This is where most of the remaining time
   is, and it speeds up the interpreter, T1, natives and the DOM bindings alike.
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
`tests/jit/replay.sh`; JS-thread CPU about 3.0–3.4 s per load).

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
| Whole load, JS-thread CPU | baseline | about −4%, within ±3% run-to-run noise |

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
- **The benchmark is too noisy to steer by.** A ±3% run-to-run spread hides most single
  changes. Measurement comes first (section 4, N0).

## 2. How Chrome got fast, and what carries over

| V8 piece | What it does | What it means here |
|---|---|---|
| **Ignition** (2016) | A register machine with an accumulator; handlers generated from one description (CSA, later Turboshaft) | Fewer, larger instructions than a stack machine, and less stack traffic per operation. Our bytecode stays a stack machine; T2 recovers the difference by tracking the stack symbolically (section 6.1) |
| **Hidden classes, inline caches, feedback vectors** | Every property access, call and arithmetic site records what it saw, in a per-function feedback vector; every tier reads it | The single most transferable idea. Our caches today live in compiled code only and are thrown away by the next tier. Section 5.4 |
| **Sparkplug** (2021) | Bytecode to machine code in one pass, with no IR; interpreter-compatible frames; calls builtins for most operations | Exactly what T1 is. Its published gains (5–15%) match what we see. It is worth having as the cheap tier and the exit target, not as the source of speed |
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

Estimates for the YouTube load (about 3.2 s of JS-thread CPU), to be checked by N0's
measurements before each milestone starts. The ranges overlap: each lever shrinks the base
the next one works on.

| Lever | Attacks | Estimated gain |
|---|---|---|
| 8-byte values (5.1) | Stack, frame and property traffic everywhere; half the value memory | 5–10% |
| Unchecked hot-path loads (5.1) | Null checks on every handle field read (`SAFE_GET_OR`) | 2–4% |
| Inline allocation fast path, fewer allocations (5.2) | Part of the 20% in GC | 4–8% |
| Calls without heap frames, JS and native (5.3) | Call setup, frame initialization, native-call glue | 4–8% |
| Caches for adding and defining properties; caches in the interpreter (5.4) | `JS_DefineProperty`, `add_property`, interpreter lookups | 2–4% |
| T2 stage 1 (6.1) | Tag checks, stack traffic and helper calls within blocks of hot code | 3–6% of what is left |
| T2 stages 2–3 (6.2, 6.3) | Values and checks across blocks and loops; inlining; allocation sinking | Unknown until stage 1's counters |

Together, the first five rows are the bulk of a plausible 20–30%. None of them is a compiler.

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

- **Interpreter → T1:** at today's threshold.
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
| N1 | Unchecked hot-path loads; inline allocation fast path; property-add/define/put caches; interpreter uses the same caches | −8% JS-thread cycles on the replays; parity |
| N2 | Calls without heap frames for simple callees; lighter native calls; call caches | −6% more; parity; GC stress clean |
| N3 | 8-byte values engine-wide | −6% more; memory down; all four JIT modes and replays at parity |
| N4 | Feedback vectors; T2 stage 1 (IR, block-local virtual stack, guards and exits), x86-64 first | −5% more on JS-heavy pages; counters for guard failures, block lengths and values crossing blocks |
| N5 | T2 stage 2 (CFG, SSA, kinds across blocks, linear scan, exits anywhere, loop entry); arm64 | Go/no-go from N4's counters; then −8% more on JS-heavy pages |
| N6 | T2 stage 3 (inlining, escape analysis) | Go/no-go from N5's data |

The order is by gain per unit of work and risk. N1 is local changes behind existing
interfaces. N2 changes frames but keeps their layout visible to the collector. N3 touches
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
