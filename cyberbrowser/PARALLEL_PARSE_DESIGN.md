# Parallel parsing and compiling of page scripts

Status: proposal. Touches the parser, compiler and lazy functions in `src/quickjs/quickjs.c`, script
loading in `src/html_media_extract.c`, and the worker pool in `src/quickjs/job_pool.c`.

## Why

Measured on the YouTube watch-page replay (three CPU-only sampled runs at 3b1246d, about 4 s of
JS-thread samples each):

| JS-thread cost per load | Time |
|---|---|
| JS execution | ~2.15 s |
| **Parse and compile** | **~0.7 s** |
| Layout and style | ~0.45 s |
| DOM and HTML | ~0.3 s |

Parse and compile is the second-largest cost on the JS thread, and all of it runs there while 11
other cores sit idle and the JS thread waits 0.6–0.95 s per load for the network.

Where it happens, among samples whose stack reaches a known entry point:

| Phase | Share |
|---|---|
| Lazy compile of a function on its first call (`js_lazy_compile`) | 38% |
| Initial parse of a script (top level, pre-scan of every function body, stubs) | 37% |
| Not attributable (stack deeper than the sampler's 24 frames) | 25% |

What it does, by leaf function:

| Work | Share of parse/compile |
|---|---|
| Tokenizer (`next_token`, `parse_ident`, string literals) | 19% |
| Atom interning (`lf_hash_lookup_ex`, `atom_hash_eq`, `hash_string8`) | 14% |
| Lazy pre-scan, the brace counter (`js_lazy_scan_body`) | 12% |
| GC allocation, handles and barriers | 9% |
| Emitting bytecode, function objects | 7% |
| Scope and variable resolution | 7% |
| Expression and statement parser | 6% |
| Labels, peephole, stack size | 4% |
| System (allocation, copies) and other | ~22% |

Two costs come from lazy parsing itself:

- **Every body that runs is scanned twice.** The pre-scan reads it to skip it, then the first call
  parses it again in full. V8 reports the same double cost as the reason for its compile hints.
- **First calls block the JS thread.** A function compiles at the moment it's needed, on the
  critical path.

The largest script is `kevlar_base` at 9.7 MB, a single minified line.

## Goals

- **JS-thread parse and compile at or under 0.25 s per YouTube load**, down from ~0.7 s.
- **Bytecode identical to the serial compiler's**, apart from atom numbering, checked by a verify
  mode.
- **Errors unchanged:** the same messages at the same positions, reported when they are today.
  Lazy mode already defers some errors in function bodies to the first call; that stays as is.
- **Overlap with the network:** scanning starts while a script's bytes are still arriving.
- **No GC-heap allocation off the JS thread.**

## Non-goals

- A new bytecode format.
- Full early-error compliance for lazily compiled bodies. That needs a full syntax check of every
  body before the script runs, which is a separate decision.
- A JIT (see `JIT_DESIGN.md`).

## 1. What other engines do, and what carries over

| | Engine | What it does |
|---|---|---|
| Off-thread parse of the whole script | V8 script streaming, SpiderMonkey off-thread compile | Scripts parse on a background thread as bytes arrive |
| Parse output without the GC | SpiderMonkey **Stencil** (2020–22) | Off-thread parsing produces GC-free data, which the main thread *instantiates*. Moving to stencils let SpiderMonkey delete the off-thread parse global, the atom-table lock and off-thread GC allocation. Its main-thread instantiation cost became a regression they then had to cut |
| Avoiding double parsing | V8 explicit compile hints (`//# allFunctionsCalledOnLoad`, Chrome 136, 2025) | Eagerly compiles the functions of hinted files on a background thread. On 20 popular pages, 17 improved, with foreground parse and compile time down 630 ms on average |
| Predicting what runs | V8 compile hints from earlier runs, code caches | Remembers which functions ran on load |
| Parallel lexing | Data-parallel FSMs (Mytkowicz et al., ASPLOS 2014), ParPaRaw (2019), reduced interface DFAs (2024), certified split points and windows (2026) | Lex chunks independently: from every possible start state, from speculated states with convergence, or from proven token starts. Compose chunk results with an associative scan |
| Structural indexing | simdjson stage 1 (Langdale & Lemire, 2019) | SIMD bitmasks find quotes, escapes and structural characters 64 bytes at a time; a scalar pass then visits only those positions |

What carries over:

- **SpiderMonkey's split** between GC-free compile output and main-thread instantiation. This engine
  needs it even more than SpiderMonkey did: allocation contexts belong only to the JS thread and
  the collector thread (`GC_OWNER_JS`, `GC_OWNER_GC`), the conservative stack scan covers only the
  JS thread, and atoms are GC strings in permanent regions.
- **V8's background compilation of functions predicted to run**, in place of first-call
  compilation.
- **simdjson's structural indexing**, to make the one serial scan per script fast.

What doesn't carry over yet: **splitting one script across cores for lexing.** The parallel-lexing
work exists for grammars where one input is the whole job. Here the scan is a small part of the
work, and moving it off the JS thread already takes it off the critical path (section 3.6).
JavaScript is also harder than the grammars those papers use: regular-expression literals,
template literals and comments make the state at a chunk boundary context-dependent, and no byte
certifies a token start. Section 11 keeps the design for later, with the measurement that would
justify it.

## 2. Pipeline

```
bytes arrive ──► S1 structure scan (one worker per script) ──► structure index
                                                     │
script runs ◄── S2 skeleton parse (JS thread) ◄──────┘  top level only; bodies skipped by index
     │                    │
     │                    └──► compile units ──► S3 compile to stencils (workers, by priority)
     │                                                   │
first call ──► S4 instantiate stencil (JS thread) ◄──────┘  or compile serially if not started
                          │
                          └──► S5 code cache (disk): structure index, stencils, call profile
```

Each stage can ship on its own (section 9). The parallelism is across scripts in S1 and across
functions in S3. Neither needs speculation: a script is scanned from its start, and a function's
compile depends only on its own unit.

## 3. S1: the structure scan (the brace counter, off the JS thread)

### 3.1 What it produces

The structure scan finds every function body. It produces the same facts `js_lazy_scan_body`
finds today, for every body at once:

```c
typedef struct FnRecord {          /* 32 bytes; array sorted by open */
    uint32_t open, close;          /* offsets of '{' and its matching '}' (arrow: body range) */
    uint32_t parent;               /* enclosing record, or UINT32_MAX */
    uint32_t names, n_names;       /* referenced identifiers: range in the name pool */
    uint32_t decls, n_decls;       /* names certainly declared at this function's level */
    uint16_t flags;                /* EAGER (direct eval, escaped or non-ASCII identifier,
                                      HTML comment, too deep, malformed), USES_THIS,
                                      USES_ARGUMENTS, HAS_YIELD, HAS_AWAIT, STRICT, LIKELY_CALLED */
    uint8_t  kind;                 /* function, method, getter/setter, class body, arrow */
    uint8_t  depth;
} FnRecord;

typedef struct NameRef { uint64_t hash; uint32_t offset; uint16_t len; } NameRef;  /* name pool */
```

Names are 64-bit hashes (rapidhash) of the identifier bytes, with an offset back into the source.
A collision is resolved by comparing source bytes when the name is used.

`LIKELY_CALLED` marks immediately invoked functions recognizable from tokens alone:
`(function`, `!function`, `function(){...}(`, and `(() => {...})(`.

### 3.2 The lexer as a small transducer

The decision of whether a `/` starts a regular expression or divides depends only on the class of
the previous token. The current scanner already reduces that context to 16 classes (`LZ_START`,
`LZ_PUNCT`, `LZ_IDENT`, `LZ_CLOSE_CTRL` and so on). Together with the lexical mode, that makes a
small deterministic transducer:

| Mode | States |
|---|---|
| Code (carries the previous-token class) | 16 |
| `'`, `"`, template, each with an escape state | 6 |
| Regular expression, its character class, its escape | 3 |
| Line comment; block comment and its `*` state | 3 |
| Inside an identifier, number or punctuator | 3 |

That is about 31 states. Two refinements need bracket context:

- **`)` closing a control-statement head** (`if (...) /re/`).
- **`}` closing a block rather than an object literal**, which decides whether a `/` after it
  starts a regular expression.

Template substitutions add a third: a `}` closing `${` returns to template mode. All three depend
on the type of the matching opener, which the scanner's bracket stack knows, since it reads the
script from the start. This is exactly what the serial `js_lazy_scan_body` does today; S1 keeps its
logic and changes where and how fast it runs.

### 3.3 SIMD byte classification

Following simdjson, each 64-byte block becomes bitmasks: quotes (`'`, `"`, backtick), backslash,
`/`, `*`, newline, brackets `{}()[]`, and identifier bytes. The escape mask comes from the
odd-length-backslash-run carry trick. Runs of identifier bytes become spans, and a perfect hash
over spans of up to 10 bytes recognizes the keywords that matter (`if`, `while`, `for`, `with`,
`else`, `do`, `try`, `finally`, `return`, `typeof`, `instanceof`, `in`, `of`, `new`, `delete`,
`void`, `throw`, `case`, `yield`, `await`, `function`, `class`, `this`, `super`, `arguments`,
`eval`).

The transducer then steps only at event positions, the bits set in those masks. It doesn't touch
the bytes inside identifiers, whitespace or digits, which dominate minified code. Targets:
SSE2/AVX2 on x86-64 and NEON on arm64, with a scalar fallback that runs the same transducer.

### 3.4 One scan per script, on a worker

Each script gets one scan job in the worker pool, run serially from the script's start state, so
the lexer state is always exact. Different scripts scan on different workers at the same time;
YouTube loads tens of scripts.

The scan keeps today's logic: the bracket stack with opener types, the previous-token class, and
per-function name collection. Its output is built as it goes:

- **Records** are appended when a function body's `{` is pushed and completed when its `}` pops,
  so the records come out sorted by `open` and `parent` is the record on the stack.
- **Names** are appended to the innermost record's buffer. When a record completes, its names are
  sorted by hash (a radix sort) and deduplicated. The free names of a function are its referenced
  names minus its certain declarations, the same rule as today.

### 3.5 Pipelining with the download and the skeleton parse

Script bodies already stream through the HTTP observer (`http_get_to_memory_observed`). The scan
job consumes bytes as they arrive and stops at the end of the bytes received so far, keeping its
state. The scan then finishes shortly after the last byte arrives.

The index is published incrementally: a release store of the number of completed top-level
records. S2 can start before the scan ends and waits only if it reaches a record the scan hasn't
finished, on the same state word, with `WaitOnAddress` or a futex.

### 3.6 Cost, and when the JS thread waits

Today's serial scan costs ~85 ms per load on the JS thread (12% of parse and compile). In S1 it
leaves the JS thread entirely, and SIMD event skipping should make the scan several times faster
per byte.

For streamed scripts the scan hides behind the network. The JS thread can wait only for a large
script that arrives all at once (from the cache or a fast network): the 9.7 MB bundle should scan
in roughly 15–25 ms on one worker (an estimate, measured in M0 and M1). That wait, if it shows up
on the critical path, is the trigger for the chunk-parallel scan in section 11.

## 4. S2: skeleton parse on the JS thread

The parser runs as today, with a cursor into the structure index. At a function body's `{`, it
looks up the record by its `open` offset: a sequential cursor in the common case, or an
open-addressed hash from offset to index. It creates the lazy stub from the record's free names,
resolved against the parser's scopes exactly as now, and jumps to `close + 1`.
`js_lazy_try_skip_body` and its scan go away.

The JS thread still tokenizes and parses each script's top level. Bundles have thin top levels
(module registration around function bodies), so this is small. M0 measures it.

For every stub it creates, S2 emits a **compile unit**: everything `js_lazy_compile` reads today,
snapshotted into GC-free memory.

```c
typedef struct CompileUnit {
    ScriptSource *src;             /* refcounted malloc copy of the script bytes */
    uint32_t record;               /* FnRecord index */
    uint32_t body_offset, line, col;
    uint8_t  func_kind, func_type, strict, is_expr_body;
    uint16_t arg_count;
    ScopeVar *closure_vars;        /* name (offset/len or predefined atom), closure type, index,
                                      is_const, is_lexical, var_kind: the stub's closure list */
    uint32_t n_closure_vars;
    _Atomic uint32_t state;        /* NONE, QUEUED, RUNNING, DONE, FAILED, CANCELLED */
    Stencil *result;
} CompileUnit;
```

`ScriptSource` holds the bytes the loader received, kept alive by reference count. Workers never
read the GC string, so pinning and moving don't matter to them.

## 5. S3 and S4: compiling bodies off the JS thread

### 5.1 Stencils

A worker compiles a unit to a stencil, a GC-free description of one function:

```c
typedef struct Stencil {           /* in the unit's arena */
    uint8_t  *bytecode;            /* final bytecode; atom operands hold stencil atom ids */
    uint32_t  bytecode_len;
    uint32_t *atom_fixups;         /* bytecode offsets of 4-byte atom operands */
    uint32_t  n_atom_fixups;
    StencilConst *cpool;           /* tagged: float64, int32, string id, BigInt text, template
                                      (cooked and raw ids), regexp (libregexp bytecode bytes),
                                      child stencil, or lazy child (FnRecord index) */
    uint32_t  cpool_n;
    StencilVar *vars, *args;       /* names as stencil atom ids, scope levels, kinds */
    uint32_t  n_vars, n_args;
    uint8_t  *pc2line;             /* the same encoding as today */
    uint32_t  pc2line_len;
    uint16_t  stack_size, flags;   /* strict, has_simple_parameter_list, arguments use, ... */
    StencilError *error;           /* syntax error: message, position (rethrown at instantiate) */
} Stencil;
```

A **stencil atom id** is either a predefined atom (`JS_ATOM_*` below `JS_ATOM_END`, fixed at build
time and emitted directly) or a local id. A local id indexes a worker-local atom table: open
addressing on rapidhash of the bytes, entries pointing into the source or the arena.

Predefined names are found first, through a perfect hash generated from `quickjs-atom.h` at build
time. Interning then happens once per distinct name per unit, instead of once per token occurrence
on the JS thread, which today is a large part of the 14% atom cost.

Nested functions inside a compiled body stay lazy. Their records come from S1, so a child becomes
a lazy-child constant and later its own unit. Units are therefore independent of each other, and
compiling them is embarrassingly parallel.

### 5.2 Making the compiler GC-free

This is the main cost of the design. The parser and compiler, roughly 18k lines of `quickjs.c`
from the tokenizer through `js_create_function`, use:

- `JSFunctionDef` with GC handles and `GC_DATA_ARRAY` vectors.
- `GCByteBuffer` for bytecode and `pc2line`.
- `JS_NewAtom*` for every identifier and property name.
- `GCValue` constants in the constant pool.
- `JS_ThrowSyntaxError` for errors.

The plan is a `CompileEnv` with two backends behind the same parser code:

| Operation | `RuntimeEnv` (JS thread: eval, `Function`, fallback) | `StencilEnv` (workers) |
|---|---|---|
| Vectors, byte buffers | today's GC arrays | arena vectors |
| Atoms | `JS_NewAtomLen` | local atom table |
| Constants | `GCValue` | `StencilConst` |
| Errors | throw | record message and position |
| Finish | `JSFunctionBytecode` | `Stencil` |

The steps:

1. Change `JSFunctionDef`'s vectors to an arena vector type used by both backends. This is the
   largest mechanical change.
2. Route atom, constant, error and finish operations through the env.
3. Split `js_create_function` into *finish* (compute the final bytecode, cpool and var tables) and
   *instantiate* (allocate GC objects).

Byte-identical output to the serial compiler is the acceptance test at every step.

### 5.3 Which functions compile ahead

Workers take units from the existing lock-free pool (Vyukov's MPMC ring), in three priority
classes:

| Priority | Units |
|---|---|
| P0, urgent | The JS thread is about to call this function |
| P1, predicted | `LIKELY_CALLED` records; functions in files with `//# allFunctionsCalledOnLoad`; functions the call profile says ran on load last time (section 6) |
| P2, speculative | Everything else, only while workers are idle and under the stencil memory budget (64 MB outstanding to start), the oldest evicted first |

When the JS thread calls a function, the unit's state decides what happens:

- **NONE or QUEUED:** the JS thread takes the unit with a CAS and compiles it itself through
  `RuntimeEnv`, today's path. It never waits for a worker that hasn't started.
- **RUNNING:** it waits on the unit's state word (`WaitOnAddress` or futex). A body typically
  compiles in well under 1 ms.
- **DONE:** it instantiates.
- **FAILED:** it compiles serially, which raises the real error exactly as today.

### 5.4 S4: instantiation

On the JS thread:

1. Map the stencil's local atom ids to runtime atoms through a per-script cache array, filled
   lazily, with one `JS_NewAtomLen` per distinct name per script.
2. Copy the bytecode and patch atom operands from the fixup list.
3. Build the cpool values and var tables, and allocate the `JSFunctionBytecode`.
4. Install the function with `js_lazy_install`, as today.

This is linear in the bytecode size and touches no parser state. The target is under 10% of the
compile time it replaces. SpiderMonkey's experience says to measure this early: instantiation is
where a stencil design can give back its gains.

## 6. S5: code cache across visits

Stencils hold offsets rather than pointers, so they serialize as they are. The cache is keyed by
(URL, 128-bit content hash, engine build id) and stores:

- The structure index.
- The stencils of the functions that ran during the load.
- The call profile: one bit per record, set on first call.

On the next visit, S1 is skipped, cached functions skip S3, and the profile drives P1. V8 and
SpiderMonkey both ship equivalents. A repeat visit should spend most of today's 0.7 s on
instantiation alone.

## 7. Expected effect on the JS thread

Estimates from the phase split. The unattributed 25% is assumed to divide like the rest, and M0
replaces these with measurements.

| Per YouTube load | Today | After M1 (S1 + S2) | After M3 (+ S3, S4) | Repeat visit (+ S5) |
|---|---|---|---|---|
| Initial parse (incl. pre-scan) | ~0.35 s | ~0.2 s | ~0.15 s | ~0.05 s |
| First-call compiles | ~0.35 s | ~0.35 s | ~0.05 s (instantiate) | ~0.05 s |
| **Total** | **~0.7 s** | **~0.55 s** | **~0.2 s** | **~0.1 s** |

Worker CPU grows by roughly the JS-thread time removed, plus speculative compiles, spread over 11
cores.

## 8. Correctness and verification

- **`CYBER_PARSE_VERIFY=1`:**
  - S2 runs today's serial `js_lazy_scan_body` on every body it skips and compares the end
    position, flags and name sets.
  - S4 compiles each function serially as well and compares, after mapping atoms: bytecode bytes,
    cpool, var tables, `pc2line` and stack size.
  - The existing `[lazy-verify]` end check stays.
- **Scanner differential testing:** S1's index against the serial scanner on the test262 parser
  tests, minified bundles from the four recorded sites, and generated programs dense in regular
  expressions, templates and comments. S1 also runs with its input fed in random-sized pieces, as
  streaming delivers it, and must give the same index.
- **Error parity:** syntax errors in lazily compiled bodies must have the same message, line and
  column whether the body was compiled by a worker or serially.
- **Concurrency:** workers read only immutable inputs (`ScriptSource`, units) and write only their
  unit's arena. Results are published by a release store to the unit's state and read after an
  acquire load.
- **Determinism:** a stencil depends only on its unit, never on scheduling.

## 9. Milestones

| | Work | Exit criterion |
|---|---|---|
| M0 | Instrumentation: per-script and per-function compile time, first-call list, top-level versus body bytes, scan time per script | Section 7's table measured |
| M1 | S1 as one worker scan per script (SIMD event skipping, pipelined with the download) and S2, with verify mode | Four sites identical (DOM, layout dumps); `js_lazy_scan_body` off the JS thread; time the JS thread waits on S1 measured |
| M2 | `CompileEnv`, stencils and instantiation, all on the JS thread (no workers yet) | Byte identity in verify mode; no regression in compile time |
| M3 | Workers compiling functions in parallel, priorities and the take-over protocol | JS-thread parse and compile ≤ 0.25 s on the YouTube replay |
| M4 | Code cache | Repeat-visit parse and compile ≤ 0.12 s |

M1 is useful alone: it removes the pre-scan from the JS thread without touching the compiler.
M2 and M3 are the core of the plan: most of the saving comes from compiling functions off the JS
thread. Section 11 (one script split across cores) is not on this list; M1's wait measurement
decides whether it ever is.

## 10. Risks

- **The compiler refactor (M2) is large.** Two backends behind one parser, with byte identity as a
  test oracle at every step, keep it incremental.
- **Wrong closure capture is silent.** A wrong free-name set compiles a function that reads the
  wrong variable. The S2 verify mode compares every set with the serial scanner's, and the
  existing lazy verification still runs.
- **The JS thread can wait on S1** for a large script that arrives all at once. The wait is
  bounded by one serial scan (estimated 15–25 ms for the largest bundle), and M1 measures it.
- **Instantiation can eat the gain**, as it did for SpiderMonkey at first. Per-script atom caches
  and fixup lists instead of re-parsing keep it linear; M2 measures it before any thread work.
- **Memory:** stencils for speculative compiles are bounded by the budget and evicted.
- **Small inline scripts and `document.write`** take the serial path. Scripts under 32 KB skip S1
  and S3: their overhead would exceed the gain.

## 11. Later: splitting one script across cores (if measured)

Not planned. Build it only if M0 and M1 show the JS thread waiting on S1 for large scripts that
arrive all at once, and the wait matters. The expected gain is small: maybe 15–20 ms on the
largest bundle, against the hardest code in the whole design.

The design, for when it's needed:

### 11.1 Chunks, enumeration and convergence

The script is cut into C chunks: four per worker, at least 256 KB each. Scripts smaller than a few
chunks keep the serial scan.

A worker scans its chunk from every plausible start state at once, keeping a few live paths:

```c
typedef struct Path {
    uint8_t  state;                /* transducer state */
    uint64_t starts;               /* bitset: the start states that lead here */
    ChunkOut out;                  /* bracket summary, records, names, assumptions */
} Path;
```

Each event step advances every live path. Two paths that reach the same state at the same
position merge, since their futures are identical: their `starts` sets are ORed and the later
output is shared. This is the convergence method of data-parallel FSMs. In minified code most paths
merge within a few tokens. The lasting ambiguity is inside versus outside a string or comment,
which resolves at the next quote or comment end. The work is the number of live paths times the
number of events, so it stays close to one scan. A prototype must measure the live-path count first.

**Split points.** A chunk ends at an event position chosen to keep the plausible start states few.
The order of preference is just after `;` or `}` followed by an identifier byte, then after a
newline. Certified split points (2026) would remove speculation entirely for a grammar where some
byte always starts a token. JavaScript has no such byte, so this design speculates and verifies.

### 11.2 Per-chunk output and composition

Each surviving path carries:

- **The end state.**
- **A bracket summary**, an element of the bracket monoid: `U`, the closers in the chunk whose
  openers lie earlier (type and position, in order), and `O`, the openers left unclosed. Two
  summaries compose associatively: `(U1, O1) · (U2, O2)` matches the tail of `O1` against the head
  of `U2`. A type mismatch marks the script `EAGER`, and the serial parser then reports the real
  error.
- **Records and names** found in the chunk, tagged with chunk-local depth.
- **Assumptions:** for each closer in `U` that changes the lexer state (a `)` that might close a
  control head, a `}` that might close `${` or a block), the outcome assumed and its position.

**Resolving.** Chunk 0 starts in the script's real start state. For chunk *i*, the path whose
`starts` contains the end state of chunk *i−1* is taken. If no path contains it, because it was
pruned, chunk *i* runs again from that exact state. All such chunks re-run in parallel, and a
second round is rarely needed.

Then the summaries compose left to right. There are only tens of chunks, and the cost is
proportional to the unmatched brackets. Composition settles every assumption. A chunk with a
wrong assumption re-scans serially from that position, which is expected to be rare. Once the
brackets match, every `{` opener of type *function body* becomes a `FnRecord`, parents come from
the matched stack, and each name goes to its innermost record. Per-record name sets are sorted by
hash with a radix sort, then deduplicated. The free names of a function are its referenced names
minus its certain declarations, the same rule as today.

### 11.3 When it applies

Only for scripts that arrive all at once. A streamed script already has an exact start state for
each chunk once the previous one is done, which is the serial scan of section 3.5.

### 11.4 Cost

With eight workers, the scan of the 9.7 MB bundle drops from roughly 15–25 ms to 3–5 ms (estimates),
plus composition. Verification needs chunk-boundary fuzzing: for small scripts, run with a split
at every byte offset and compare with the serial scan, over test262 parser tests, minified bundles
and programs dense in regular expressions, templates and comments.

## References

- SpiderMonkey stencil and off-thread compilation:
  [Firefox 96–97 newsletter](https://spidermonkey.dev/blog/2022/01/14/newsletter-firefox-96-97.html),
  [Bug 1671895 (instantiation cost)](https://bugzilla.mozilla.org/show_bug.cgi?id=1671895).
- V8 explicit compile hints: [v8.dev/blog/explicit-compile-hints](https://v8.dev/blog/explicit-compile-hints).
- Parallel lexing:
  [Certified Split Points for Parallel Lexing (2026)](https://arxiv.org/pdf/2608.03473),
  [Certified Split Windows (2026)](https://arxiv.org/pdf/2608.09761),
  [ParPaRaw (2019)](https://arxiv.org/pdf/1905.13415),
  [reduced interface DFAs (2024)](https://arxiv.org/abs/2412.14975v1),
  [simultaneous DFAs (2015)](https://arxiv.org/pdf/1512.09228),
  [comp.compilers discussion of chunked lexing](https://compilers.iecc.com/comparch/article/22-03-070).
- Mytkowicz, Musuvathi, Schulte, *Data-Parallel Finite-State Machines*, ASPLOS 2014.
- Langdale, Lemire, *Parsing Gigabytes of JSON per Second* (simdjson), VLDB Journal 2019.
