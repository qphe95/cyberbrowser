# A baseline JIT for the QuickJS engine

Status: proposal. Adds a compiler tier beside the interpreter in `src/quickjs/` (new
`quickjs_jit*.c`, a build-time stencil generator under `tools/`), and touches the interpreter's
call path, frames and polling in `quickjs.c`.

## Why

Measured on the YouTube watch-page replay (three CPU-only sampled runs at 3b1246d): JS execution
takes ~2.15 s per load, 55% of the JS thread. By leaf function:

| JS execution, by leaf | Share | ~ms per load |
|---|---|---|
| Interpreter loop (`JS_CallInternal` itself) | 24.5% | 530 |
| Property get/set/define | 16.3% | 350 |
| GC handle loads and write barriers | 11.3% | 245 |
| GC allocation | 9.4% | 200 |
| Natives called from JS (DOM bindings and builtins) | 7.5% | 160 |
| Frames, closures, variable references | 7.2% | 155 |
| Atoms, strings, numbers, regexps, other | ~24% | 510 |

Inside the interpreter loop, the hottest lines are function entry and the call opcodes (about 550
samples of 1583), then `get_field`. A baseline JIT removes:

- **Dispatch:** the indirect jump, opcode decode and operand fetch of every instruction.
- **Stack traffic within an instruction:** operands and results stay in registers.
- **Most of the JS-to-JS call sequence:** the prologue re-reads the function, bytecode and context
  through handles on every call.

It doesn't by itself shrink allocation, barriers or natives.

**What the inline cache experiment taught.** A per-instruction property cache in the interpreter
(tried and reverted, October 2026) hit on 84% of cacheable accesses yet saved nothing. The hits
removed 148 samples from `find_own_property`, and the cache lookup itself cost 149–203. The
original lookup is already about 12 ns. Shapes here change in place, so a hit still had to load
the shape and validate the slot, and the global table added misses of its own. Inline caches pay
only when the hit is one load and one compare; section 7 says what that requires.

## Goals

- **Tier 1, a baseline JIT** built by copy-and-patch, emitting 50–150 ns per bytecode
  instruction.
- **The interpreter's semantics exactly.** Any frame can continue in the interpreter at any
  bytecode boundary, because the JIT keeps the interpreter's frame layout.
- **Targets:** x86-64 (Windows with mingw, Linux) and arm64 (Android, macOS).
- **25–40% less JS execution time on the YouTube load**, if the hot code concentrates as expected.
  This is an estimate; milestone M0 measures the ceiling first.
- **`QJS_JIT=1`** turns it on (it was on by default from `JS_PERF_DESIGN.md` N1 until 8-byte
  values made the interpreter faster in N3), and a stress mode compiles every function at its
  first call.

## Non-goals (v1)

- An optimizing tier, type speculation or deoptimization. Section 11 sketches them.
- Register allocation across instructions.
- Changing the bytecode or the 16-byte `GCValue`.
- Compiling on worker threads.

## 1. How others do it, and what carries over

| Engine | Tiers |
|---|---|
| V8 | Ignition interpreter → **Sparkplug** (2021: compiles straight from bytecode with no IR, frames compatible with the interpreter) → **Maglev** (2023: CFG/SSA mid-tier using IC feedback; about 20× slower to compile than Sparkplug, 10–100× faster than the top tier) → TurboFan with **Turboshaft** (CFG IR replacing sea of nodes) |
| JavaScriptCore | LLInt interpreter → Baseline JIT → DFG → FTL (B3) |
| SpiderMonkey | Baseline interpreter → Baseline compiler → Warp (built from CacheIR, the IC stubs' own IR) |
| CPython 3.13+ | Copy-and-patch JIT over micro-ops |
| LuaJIT Remake (Deegen, 2024–26) | Interpreter and baseline JIT *generated from one description* of the bytecode semantics; copy-and-patch code generation with inline caches as the only baseline optimization; about 4.6× faster than PUC Lua |
| R (2025) | Copy-and-patch JIT: about 980 bytecode instructions per ms; 1.15–1.91× over GNU R |

What carries over:

- **Sparkplug's frame compatibility.** T1 uses the interpreter's frames, so moving between tiers
  needs no deoptimization machinery.
- **Copy-and-patch** gives native code for x86-64 and arm64 without writing an assembler for
  either. C compiled by clang at build time is the template; the JIT copies bytes and patches
  holes.
- **Deegen's view that inline caches are the one baseline optimization worth its cost**, together
  with our measurement of when they pay.
- **Deegen's long-term direction:** generating the interpreter and the JIT from one source, so the
  two can't disagree.

## 2. Architecture

```
            hotness counter ≥ H, or loop back edges ≥ L
 T0 interpreter ─────────────────────────────────────────► T1 baseline code
   ▲   (counters added)        compile (JS thread)            │
   │                                                           │
   └──── exception, unsupported op, debugger, generator resume ┘
         continue in the interpreter at (pc, sp): same frame
```

- **T0** is today's interpreter, plus per-function counters (section 8).
- **T1** code for a function is one contiguous native routine. It holds a copied stencil for each
  bytecode instruction, plus a shared prologue, poll stubs and exits.
- **T1.5** adds inline caches to T1, once shapes are immutable (section 7).
- **T2** comes later.

## 3. Copy-and-patch stencils

### 3.1 Stencils from C

Each bytecode handler is written once as a C function in `quickjs_jit_ops.c`:

```c
JIT_OP(get_loc) {                       /* pinned: ctx, sf, sp, var_buf, arg_buf */
    sp[0] = var_buf[JIT_IMM_U16(0)];
    sp++;
    JIT_CONTINUE();
}

JIT_OP(add) {
    GCValue a = sp[-2], b = sp[-1];
    if (likely(JS_VALUE_GET_TAG(a) == JS_TAG_INT && JS_VALUE_GET_TAG(b) == JS_TAG_INT)) {
        int32_t r;
        if (likely(!__builtin_add_overflow(JS_VALUE_GET_INT(a), JS_VALUE_GET_INT(b), &r))) {
            sp[-2] = JS_NewInt32(ctx, r);
            sp--;
            JIT_CONTINUE();
        }
    }
    JIT_SLOW(js_op_add_slow);            /* the interpreter's slow path, as a helper */
}
```

The macros expand to:

- **Holes:** `extern char __jit_imm0[]` and similar, whose addresses become relocations in the
  object file.
- **The continuation:** a tail call to the hole `__jit_continue`, written with
  `[[clang::musttail]]` and `__attribute__((preserve_none))` (clang 19 and later). The pinned
  state (`ctx`, `sf`, `sp`, `var_buf`, `arg_buf`) then stays in registers from one stencil to the
  next, with no prologue or epilogue between them.

Each target compiles this file with clang `-O2`, `-fno-asynchronous-unwind-tables` and
`-fno-stack-protector`, under a code model that keeps immediates patchable.

### 3.2 Extracting the stencils at build time

`tools/jit_stencils.py` reads each object file's relocations, via `llvm-readobj` JSON output for
COFF x86-64, ELF arm64 and Mach-O arm64. It writes `jit_stencils_<target>.h`, which holds for
each op:

- The code bytes.
- The holes: offset, kind (abs64, rel32, arm64 `adrp`/`add`, `movz`/`movk`, `b`/`bl` imm26) and
  symbol (`IMMn`, `CONTINUE`, `TARGET`, or a helper function).
- A small data section for literal constants.

When a stencil ends in its continuation jump and the next stencil follows directly, the jump is
cut, as CPython does.

The generated headers are checked in. Normal builds stay on mingw gcc, and clang is needed only to
regenerate stencils after an op changes, with a build check that the headers are current.

### 3.3 Emission

`jit_compile(b)` makes two passes over the bytecode:

1. Pick each instruction's stencil variant and compute its native offset. This builds `pc_map`,
   bytecode offset to native offset.
2. Copy and patch: immediates, helper addresses, and jump targets from `pc_map`.

Helper calls use rel32 on x86-64. For that, the code region is reserved within ±2 GB of the
executable image, at hint addresses near the image base. Otherwise a per-region literal pool holds
absolute addresses. On arm64, `adrp`/`add` pairs or the literal pool are used.

The result:

```c
typedef struct JitCode {
    uint8_t *entry;                  /* function entry (after the shared prologue stub) */
    uint32_t size;
    uint32_t *pc_map;                /* delta-encoded: OSR entries, generator resume, cur_pc */
    uint32_t n_pc_map;
    uint16_t *op_at;                 /* native offset → op, for profiles and dumps */
    JSFunctionBytecodeHandle owner;  /* weak, see section 6 */
} JitCode;
```

### 3.4 Fused instructions

Frequent pairs get one stencil:

- A compare and its branch: `lt` + `if_false` → `cmp` + `jge`.
- `get_loc` + `get_field`.
- `push_i32` + `add`.
- `get_loc` + `put_loc`, and increment of a local.

Short opcodes are off in this build (`SHORT_OPCODES 0`), so stencils are specialized on immediate
values instead. M0's pair frequencies choose the set.

Top-of-stack caching (keeping the top value in a register across instructions, with one stencil
variant per cache state) is a later step: it multiplies the stencils.

## 4. Values and fast paths

A `GCValue` is 16 bytes: an 8-byte payload and an 8-byte tag. Stencils keep a value in a register
pair. Inline fast paths cover:

- Int32 arithmetic with overflow checks (`jo` on x86-64, `adds` then `b.vs` on arm64).
- Float64 arithmetic and comparisons.
- Booleans, branches, and local and argument access.
- Closure variable reads through `JSVarRef`.
- Strict equality on equal tags.

Everything else calls the helper the interpreter itself uses. M1 factors the interpreter's slow
paths out of `JS_CallInternal` into `js_op_*_slow(ctx, sf, sp, ...)` functions shared by both
tiers, so the two have one implementation of every semantic corner.

**Values move in 8-byte halves.** Nearly every stencil reads what the previous one just wrote, so
the store buffer forwards it, unless the load spans two stores: a 16-byte `movups` of a value
stored as two halves, or an 8-byte load of a payload stored as two 4-byte pieces, stalls for a
dozen cycles. Measured in M2, that stall was 13% of a call-heavy benchmark, in `return` alone.
Stencils therefore store a value as an 8-byte payload (an int zero-extended) and an 8-byte tag, and
copy it as two 8-byte loads and stores; the C code compiled code calls (`jit_call_prepare`, frame
setup) loads values it may have just been handed the same way (`jit_value_load`), and takes them
by address, not by value, since the Win64 ABI copies a 16-byte struct argument through memory.

Moving `GCValue` to 8-byte NaN boxing would halve the stack and frame traffic of both tiers. That
is a separate project.

## 5. Frames, calls, exceptions and generators

**Frames.** T1 uses the interpreter's frames unchanged: `JSStackFrame` plus the argument, variable
and stack buffers from `js_alloc_bytecode_frame` and its per-depth pool. The stack pointer lives
in a register. Before any helper that can throw, poll the GC or read the stack (backtraces), the
stencil stores the bytecode offset into `sf->cur_pc` with one immediate store, as
`JS_SF_SET_PC` does in the interpreter.

**Entry.** After `JS_CallInternal` sets up a frame, a function with JIT code runs it:
`if (b->jit) return jit_enter(...)`. Otherwise its counters are updated.

**JS-to-JS calls.** A call stencil checks whether the callee is a bytecode function with JIT code
and a simple argument shape. If so, it sets up the frame through a shared helper and calls the
native entry directly, skipping the interpreter prologue that costs ~550 samples today. A call IC
(the callee's bytecode → entry) skips the check when the site is monomorphic. Every other callee
goes through `JS_CallInternal`.

**Exceptions.** A helper that returns an exception jumps to the function's shared exit, which
stores `sp` and `cur_pc` and returns to a trampoline. The trampoline continues the same frame in
the interpreter at its exception handler. "Continue this frame in the interpreter at (pc, sp)"
already exists for resuming generators and async functions, and T1 reuses it. The same path takes
any op T1 doesn't support yet, so functions can be compiled in part during bring-up.

**Generators and async functions.** Their frames are GC objects. Suspension writes pc and sp as
the interpreter does. Resumption enters T1 through `pc_map` when the function has JIT code, and
the interpreter otherwise.

**On-stack replacement.** A back-edge counter in the interpreter triggers compilation. The JIT
then enters at the loop header through `pc_map`. The frame is already compatible: only the
registers are loaded from it.

## 6. The collector

- **Polls.** The interpreter polls at function entry and on every jump (`JS_POLL_POINT`: `gc_poll`
  and the interrupt countdown). T1 emits the same check inline at entry and at every back edge,
  calling a helper only when it fires.
- **Roots.** Values live in frame buffers, as in the interpreter. Registers hold copies only within
  one stencil, and across helper calls everything is in memory. The JS thread's stack is scanned
  conservatively in any case: any 32-bit word equal to a live handle, or any interior pointer,
  keeps its object alive and pins it. So T1 needs no stack maps.
- **No raw pointers across GC points.** Objects can be evacuated unless pinned, so a stencil never
  keeps a pointer to a GC object across a helper call or poll; it dereferences the handle again,
  which is the interpreter's rule too. The handle-free collector proposal
  (`GC_PAUSELESS_DESIGN.md`) would make dereferencing cheaper still for both tiers.
- **Write barrier inline:** a card mark, `off = p - heap_base; if (off < reserved) cards[off >>
  shift] = 1`.
- **Code lifetime.** JIT code is owned by its bytecode and freed when the bytecode dies.
  Bytecodes with code sit in a weak table, swept after each major cycle with the weak-reference
  machinery. Per-object finalizers (`gc_set_handle_finalizer`) are a linear registry, which
  doesn't scale to thousands of functions.

## 7. Inline caches (T1.5)

The interpreter experiment shows what a cache hit must cost to pay: one load of the receiver's
shape and one compare. Three conditions get there.

1. **Immutable shapes.** Today an object's private (unhashed) shape changes in place on add,
   delete, flag and prototype changes, which is why a hit must validate the slot. Instead, every
   change makes or reuses a new shape through the hashed transition tree that `add_property`
   already uses. Objects that change too often switch to dictionary mode (a flag on the object),
   and caches never cache them. This change is engine-wide and is the prerequisite.
2. **Caches beside the code, not in a global table.** Each site has a slot in an IC slab next to
   its code. A monomorphic site patches the expected shape and slot as immediates into the code:
   `cmp [obj+shape], imm32; jne miss; load [props + idx*16]`. Up to four shapes chain into
   polymorphic stubs, as Deegen's JIT IC inline slab does; more than four makes the site
   megamorphic, and it calls the generic lookup.
3. **Shapes kept alive by the code that names them.** An IC slab references its shapes through a
   GC-visible array owned by the bytecode, so a handle in a cache can't be reused for another
   shape. Caches are cleared at major cycles to let dead shapes go.

What it targets: property access, 16% of JS execution; prototype method hits, 12% of `get_field`;
and missing-property walks, 7%, which can cache absence once prototype shapes are immutable too.
Immutable shapes also make an interpreter-level cache pay, so they are worth doing first either
way.

**As built (M4, first part).** The engine already treated hashed shapes as immutable: every
mutator clones a hashed shape first (`js_shape_prepare_update`, `add_shape_property`), and only
private shapes, each owned by one object, change in place. The remaining in-place flag changes
(global variables) now go through `js_shape_prepare_update` too. So instead of a new shape
regime:

- **Transition cache.** Each shape remembers its last transition (atom, flags → hashed child,
  which it keeps alive), so a repeated `add_property` skips the shape hash table and its
  whole-shape comparison. On the YouTube replay this halved `find_hashed_shape_prop`.
- **Per-site caches for `get_field`/`get_field2`** in compiled code (`JitIC`, four ways, way 0
  tested inline), filled by `jit_get_field_ic`. A way caches a hashed receiver shape: own
  property (slot), property on a prototype (holder, slot) or absent. A private receiver shape
  gets own-property ways only; it is marked `ic_cached`, and is then cloned, like a hashed one,
  before any change but an append (appends keep slots).
- **Prototype ways** hold while `jit_proto_epoch(atom)` is unchanged: a change to property A
  of an object that is a prototype (`is_prototype`) bumps A's counter, a structural change
  (prototype link, slots moving) a global one.
- **A megamorphic cache** (8192 entries by shape and atom) serves sites past four shapes.
- **GC:** the shapes and holders ways name sit in a retention set enumerated as a root (mark
  start and remark run on the JS thread), so a cached handle never comes to name another
  object.

On the replay, about 90% of compiled `get_field` lookups hit (inline, other ways, prototype ways or
megamorphic); the misses are accessors (getters, mostly DOM), exotic objects and prototype
lookups through private receivers. `props` (micro-benchmark) improves 6%. End to end, the load's
JS thread time moves by less than its run-to-run noise (about ±3% here): property lookups are
a smaller part of this load than the 16% above, and each hit still touches the receiver's memory.
Not done yet: `put_field`/`define_field` caches (adding properties, about 2% of the load), getter
calls through the cache, and caches in the interpreter.

## 8. Tiering

Each `JSFunctionBytecode` gets a hotness budget. Entry adds a weight, and every back edge adds the
loop body's length (V8's approach). When it passes H, the function compiles, starting at
H = 2,000 bytecode instructions executed. A loop that runs more than L = 500 iterations compiles
and enters by OSR.

Compilation is synchronous on the JS thread at first: at 50–150 ns per instruction, a 300-op
function takes 15–45 µs. Budgets decay at major cycles.

The code region is 32 MB, and cold code is evicted (its bytecode falls back to T0).

The threshold matters more than the emitter's speed. Compiling everything that runs once would
cost more than it saves. M0 measures how execution concentrates and sets H.

## 9. Platforms

| Platform | Code memory | Notes |
|---|---|---|
| Windows x86-64 | `VirtualAlloc` reserve near the image base; write RW, flip to RX per compiled batch with `VirtualProtect`; `FlushInstructionCache` | Register unwind data for the region (`RtlAddFunctionTable` or `RtlInstallFunctionTableCallback`) so SEH and the in-process sampler, which walks x64 unwind tables, can unwind through JIT frames. The shared prologue sets up one fixed frame, including the Win64 shadow space for helper calls, and the stencil generator rejects any stencil that moves the stack pointer, so one unwind entry describes a whole function. If the binary is ever built with Control Flow Guard, entries need `SetProcessValidCallTargets` |
| Android arm64 | Dual mapping: `memfd_create`, then one RW and one RX mapping of the same memory, so permissions never flip | `__builtin___clear_cache` after writing |
| macOS arm64 | `MAP_JIT`; `pthread_jit_write_protect_np(0)` while writing on the JS thread, `(1)` before running | The `com.apple.security.cs.allow-jit` entitlement; `sys_icache_invalidate`. Write permission is per thread, so only the JS thread writes |
| Linux x86-64 | Like Android, or `mprotect` flips | `perf` map file (`/tmp/perf-<pid>.map`) for profiling |

**Hardening.** Code pages are never writable and executable at once. Immediates patched from
script constants (integers and doubles) can be XORed with a per-process key and decoded in a
second instruction ("constant blinding"), against JIT spraying. Pages are untrusted, so blinding
should be on before T1 is enabled by default; it can wait during bring-up.

## 10. Debugging and verification

- **Switches:** `QJS_JIT=0` (off), `1` (normal), `stress` (compile at first call, threshold 0);
  `QJS_JIT_LOG=1` (compiles and exits to the interpreter, with reasons).
- **Profiles:** JIT ranges are registered with the sampler, so samples in JIT code symbolize as
  `jit:<function>:<op>` through `op_at`. No disassembler is needed: native offsets map back to
  bytecode.
- **Differential testing:**
  - The unit tests, a test262 subset and the four recorded sites, each in stress mode against
    `QJS_JIT=0`, comparing output, DOM and layout dumps.
  - The same with GC stress and poisoning.
  - Fuzzing in the style of Fuzzilli: generated programs whose interpreter and JIT outputs must
    agree.
- **Exit checks** (debug build): every exit to the interpreter checks that `sp` and `cur_pc` are
  consistent with the stack depth `pc_map` records for that pc.

## 11. Expected effect

Estimates, which M0 refines.

| Per YouTube load | Today | T1 | T1.5 (immutable shapes + ICs) |
|---|---|---|---|
| Interpreter dispatch (530 ms) | 530 ms | ~150 ms | ~150 ms |
| Call sequence, frames, varrefs (part of 155 ms + entry lines) | ~250 ms | ~150 ms | ~150 ms |
| Property access (350 ms) | 350 ms | 350 ms | ~180 ms |
| Compile cost | — | +20–50 ms | +25–60 ms |
| **JS execution** | **~2.15 s** | **~1.75 s** | **~1.6 s** |

These assume about 70% of execution runs in functions above the threshold. Allocation, barriers
and natives (~600 ms) need other work: allocation removal by an optimizing tier, the pauseless
collector's cheaper barriers, and faster bindings.

## 12. Milestones

| | Work | Exit criterion |
|---|---|---|
| M0 | Profile: executed instructions per function, op-pair frequencies, the share of execution in functions above H | The ceiling and H measured; fused-op list chosen |
| M1 | Interpreter slow paths factored into `js_op_*_slow` helpers, no behavior change | Tests and four-site parity; no slowdown |
| M2 | Stencil toolchain; T1 on Windows x86-64 for a subset of ops (locals, arithmetic, compares, branches, `get_field`/`put_field` through helpers, calls through `JS_CallInternal`); partial compilation with exits | Stress-mode differential tests pass |
| M3 | All ops, direct JS-to-JS calls, exceptions, generators, OSR, unwind registration; arm64 (Android, macOS) | Four sites at parity in stress mode; JS execution −15% or better on the replay |
| M4 | Immutable shapes (engine-wide), then per-site ICs | Property access share halved |
| M5 | T2 study: a Maglev-like mid-tier | Go/no-go from M0–M4 data |

## 13. Risks

- **The engine's own overheads cap the gain.** 16-byte values, handle dereferences and card-mark
  barriers stay. This tier attacks dispatch and calls; the collector proposal and NaN boxing
  attack the rest.
- **Two execution engines can disagree.** One semantic implementation per op (the shared slow
  paths), differential testing in stress mode, and eventually generating interpreter and stencils
  from one source, Deegen-style, address this.
- **Unwinding on Windows.** SEH, the sampler and debuggers must see JIT frames. Registering unwind
  data is part of M3, not an afterthought.
- **Build complexity.** Stencils need clang, but only to regenerate the checked-in headers.
- **Code memory** is bounded by the region and eviction.

## 14. Later: an optimizing tier (T2)

Following Maglev rather than a sea of nodes, which V8 itself is leaving for CFG IRs (Turboshaft):

- **A CFG SSA IR** built from bytecode plus the T1.5 IC feedback.
- **Speculation** on int32 arithmetic, shapes (from ICs) and call targets, guarded by checks.
- **Deoptimization:** every guard carries a frame state describing how to materialize the
  interpreter frame (values for each local and stack slot), and a failing guard rebuilds the
  T0/T1 frame and continues there.
- **Linear-scan register allocation.**
- **Inlining** of small monomorphic callees.
- **Escape analysis and allocation sinking**, which target the 9% spent on allocation.

T2 makes sense only if M0–M4 show hot, type-stable code worth it. YouTube's profile is broad, so
T1 and the caches are the first and surest steps.

## References

- Xu, Kjolstad, *Copy-and-Patch Compilation* (2021): [arxiv.org/pdf/2011.13127](https://arxiv.org/pdf/2011.13127).
- Xu, *Deegen: A JIT-Capable VM Generator for Dynamic Languages*: [arxiv.org/pdf/2411.11469](https://arxiv.org/pdf/2411.11469),
  [SPLASH/OOPSLA 2026](https://2026.splashcon.org/details/oopsla-2026/46/Deegen-A-JIT-Capable-VM-Generator-for-Dynamic-Languages),
  [LLVM Dev Meeting 2023 slides](https://www.llvm.org/devmtg/2023-10/slides/techtalks/Xu-Deegen-LLVM-Based-Compiler.pdf).
- Copy-and-patch JIT for R: [VMIL 2025](https://conf.researchr.org/details/icfp-splash-2025/vmil-2025/4/Copy-and-Patch-Just-in-Time-Compiler-for-R),
  [MPLR 2026 follow-up](https://janvitek.org/pubs/mplr26.pdf).
- V8 tiers: [Holiday season 2023 (Maglev, Turboshaft)](https://V8.dev/blog/holiday-season-2023),
  [V8 overview](https://en.wikipedia.org/wiki/V8_(JavaScript_engine)).
- W^X and JIT memory on Apple silicon: [W^X](https://en.wikipedia.org/wiki/W%5EX),
  [JEP 391: macOS/AArch64 port](https://openjdk.org/jeps/391).
- Ertl, *Stack caching for interpreters*, PLDI 1995 (top-of-stack caching); Ertl, Gregg, *The
  structure and performance of efficient interpreters*, JILP 2003 (dispatch costs,
  superinstructions).
