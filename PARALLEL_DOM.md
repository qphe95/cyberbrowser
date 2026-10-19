# Parallel DOM parsing and processing

Status: **implemented** (phases 0–5 of §9, with the deviations listed there). The
open questions of §11 were left out of scope.

It applies the approach of `PARALLEL_STYLE_LAYOUT.md` to the document:
- data-oriented tables in plain memory;
- the same job system and frame arenas;
- speculative parallel work that is validated afterwards;
- workers never touch the GC heap or QuickJS;
- results identical for any thread count.

The pipeline stays **synchronous with JS**: the page is parsed before its scripts
run, and DOM API calls run on the engine thread. Parallelism is inside each step.

Source map:

| File | Contents |
|---|---|
| `src/cyber_env.[ch]` | Phase 0: environment switches read once (`CYBER_ENV(bit)`, `cyber_getenv`) |
| `src/dom_store.[ch]` | The node store (§4) |
| `src/dom_parse.[ch]` | Prescan, chunked tokenizer, tree builder (§5) |
| `src/dom_snapshot.[ch]` | `HtmlDocument` built from the store (layout input) |
| `src/dom_wrap.[ch]` | Lazy wrappers and tree-level lifetime (§6) |
| `src/dom_query.[ch]` | Compiled selectors and indexes (§7) |
| `src/dom_fetch.[ch]` | Script fetch pool (§8) |
| `src/dom_js.h` | Browser-layer helpers used by the DOM APIs |
| `src/browser/dom_api.c`, `src/html_dom.c` | The DOM APIs and the render snapshot, rewritten over the store |
| `tests/test_dom.c`, `tests/legacy_html_parse.[ch]` | New tests; the legacy parser kept as the test oracle |

---

## 1. Where the time went, and where it goes now

`dombench` (golden harness): parse, build the JS DOM, render rebuild. Release
build, same 2-core container as the design measurements.

| Page | Nodes | `html_parse` before → after | Build the JS DOM before → after | Render rebuild before → after |
|---|---|---|---|---|
| Wikipedia main page (469 KB) | 3,952 | 12.3 → 4.5 ms | **1,140 → 1.3 ms** | 31 → 8.0 ms |
| YouTube watch page (1.5 MB) | 350 | 3.0 → 4.5 ms | **92 → 1.7 ms** | 3.9 → 5.8 ms |
| Random page (49 KB) | 1,421 | 2.9 → 1.2 ms | **243 → 0.4 ms** | 10.3 → 1.7 ms |

- Building the JS DOM is now proportional to the nodes scripts touch, not to the
  page. It was ~280 µs per node, mostly `getenv` in QuickJS hot paths (68%), the
  custom-element walk on every append, and per-element objects.
- `html_parse` alone (tokenize + build into the store) is 2.0 ms for Wikipedia and
  1.5 ms for YouTube. The `html_parse` column above also includes building the
  `HtmlDocument` snapshot from the store.
- The YouTube render rebuild got slower in absolute terms because the old one
  serialized truncated text (the legacy `DOMNode` cut text at 4 KB, so 1.5 MB of
  inline scripts became 70 KB); the new one carries the full text.
- Running YouTube's inline scripts on the populated document (`jsgold`, offline):
  1,070 ms → 450–580 ms.

Phase 0 alone (switches read once, measured before the store existed): populate
1,110 → 660 ms on Wikipedia, 91 → 41 ms on YouTube, 263 → 129 ms on the random page.

---

## 2. The design in one paragraph

The document is a **node store**: flat arrays in plain memory, owned by the engine
thread, holding the tree, interned names, attributes and text. **JS objects are
lazy wrappers**, created when script first touches a node. **Parsing** cuts large
inputs into chunks tokenized speculatively in parallel and validated by a
continuation; the tree is built serially from compact token arrays. **Layout**
reads an `HtmlDocument` snapshot built from the store. **Mutations** update the
store, bump versions and set dirty bits. **External scripts** are fetched by a
pool while execution stays in document order.

---

## 3. Pipeline

```
bytes
  │
  ├─► prescan (when chunking): '<' boundaries outside raw-text elements and comments
  ▼
tokenize chunks (root job + jobs, speculative) ──► validate (continuation) ──► redo bad chunks
  ▼
tree build (serial, over token arrays) ── slots assigned in document order, id index fed
  ▼
node store ready ──► populate: new html/head/body, parsed children moved in (no wrappers)
  ▼                       external scripts already in the fetch pool
scripts run in order (wrappers created on demand)
  ▼
render snapshot (HtmlDocument) ──► style + layout (PARALLEL_STYLE_LAYOUT.md)
```

---

## 4. Node store (`dom_store.h`)

### 4.1 Layout

```c
typedef uint32_t NodeId;                /* slot | generation << 24 */

typedef struct DomStore {
    uint8_t  *kind, *gen;               /* element, text, comment, document, fragment */
    uint16_t *flags;                    /* shadow root, template content, CE candidate,
                                           CE descendant, parser script, dirty bits */
    CssAtom  *tag;                      /* lower-case local name, shared with CSS */
    uint32_t *parent, *first, *last, *next, *prev;   /* slots */
    uint32_t *host, *shadow, *content;  /* shadow roots and template contents */
    uint32_t *owner;                    /* owner document */
    uint32_t *tree;                     /* roots: tree record */
    uint32_t *wrapper, *wrap_idx;       /* GC handle of the wrapper, index in its tree */
    DomStr   *text;                     /* text / comment data */
    DomAttrList *attrs;                 /* { CssAtom name; DomStr value; } in source order */
    CssAtom  *id_atom;
    uint32_t *order;                    /* creation sequence */
    DomTreeRec *trees;                  /* { root, tree object handle, wrapper count } */
    uint64_t structure_version, attr_version, text_version, class_version;
} DomStore;
```

- A freed slot bumps its generation, so a stale `NodeId` never resolves to the
  slot's next occupant (`dom_valid`).
- **Strings** (`DomStr`) are either spans of reference-counted arena blocks (what
  the parser writes: no `malloc` per string, the block is freed with its last
  string) or `malloc`'d copies (what mutations write). No length limits anywhere.
- **Trees.** Every node belongs to the tree of its root. Shadow roots and template
  contents continue to their host, so a host and its shadow tree share a tree.
  Trees are the unit of wrapper lifetime (§6.2).
- The store has one writer, the engine thread. Parse jobs write only token arrays
  and arenas of their own chunk; the tree builder runs on the engine thread.

### 4.2 Indexes (`dom_query.c`)

- **Id:** an incremental index (atom → slots, append-only, validated by readers),
  fed by the tree builder and by attribute writes.
- **Class and tag:** sorted node lists in document order, built by counting sort
  over the document's preorder. Above 8,192 nodes the count and scatter passes run
  as range jobs (one job per 8,192-node range, merged by prefix sums). They are
  rebuilt lazily when `structure_version` or `class_version` moved since the last
  build.
- These replace the GC-heap `LFHashTable` CSS indexes; `css_index_insert_node` and
  the `css_document_state_*` functions are now no-ops kept for their callers.

---

## 5. Parsing (`dom_parse.c`)

### 5.1 Tokens

Tokenizers emit token records `{kind, tag atom, attribute span, text span, source
offsets}` into their chunk's arena. Tag and attribute names are interned
(lower-cased) while tokenizing; entities are decoded and whitespace collapsed into
the arena. The atom table is reserved before the jobs start, so interning from
several jobs never reallocates it.

### 5.2 Speculative chunks with validation

- The **prescan** picks boundaries at a `<`, skipping raw-text elements and
  comments, so a boundary almost never falls inside one.
- Each chunk is tokenized in a job assuming the data state at its start. A chunk
  sees the input only up to its limit plus a slack of max(4 × chunk, 1 KB); a
  token that runs past that marks the chunk as overrun.
- A **continuation** (the root job of a `job_graph_run`) checks that chunk k−1
  ended exactly at chunk k's start. A chunk that fails, or overran, is
  re-tokenized from the true position. The token stream is always the serial
  tokenizer's.
- The arena block size adapts to the chunk span, so tiny chunks do not each pin a
  64 KB block.
- **Default:** chunk size 1 MB, so only inputs of 2 MB or more are chunked. On the
  2-core test machine the serial tokenizer runs at ~250–300 MB/s and chunking at
  64 KB was slower (Wikipedia 2.0 → 3.2 ms, YouTube 1.5 → 2.4 ms): the prescan and
  job overhead cost more than the second core saves. `DOM_CHUNK=<bytes>`,
  `DOM_CHUNK_FORCE=1` (exact boundaries, ignoring the prescan) and
  `dom_parse_set_chunking()` override it; `DOM_LOG=1` prints per-parse timings.

### 5.3 Tree construction (serial)

One pass over the token arrays builds the tree into the store and assigns slots in
document order. It **reproduces the legacy recursive parser exactly**, which keeps
every golden layout identical:
- no implied end tags; end tags close by prefix match; stray end tags are dropped;
- comments are dropped; nesting is capped at 256;
- a raw-text start tag is followed by its raw text; `<x/>` records self-closing;
- the legacy parser re-tokenized at the top level after `</…<`; the builder
  reproduces that ("detours").

The only intended difference is that names and values have no length limits (the
legacy parser cut tag names at 128 bytes and values at 4,096).

Post-processing happens inside the builder (id index, custom-element candidate and
descendant flags, parser-script flag); the class and tag indexes are built lazily on
first query (§4.2) rather than after every parse.

### 5.4 Fragments

`innerHTML`, `outerHTML`, `insertAdjacentHTML`, `DOMParser` and
`createHTMLDocument` use the same tokenizer and builder into a detached fragment.
For `innerHTML` the legacy rule is kept: the parsed `<body>`'s children when the
input has a non-empty body, else the top-level nodes, else a text node with the
raw string.

---

## 6. JS wrappers (`dom_wrap.c`)

### 6.1 Lazy creation

- `dom_js(ctx, id)` returns a node's wrapper, creating it through the browser
  layer's factory on first use; the store records the wrapper handle, so the same
  node always returns the same object, and `dom_node_of(obj)` maps back.
- Elements, text, comments, fragments and documents use the DOM node class with a
  `DomWrap` record `{id, tree, style, computed_style}`. Video elements and native
  shadow roots carry the record in their own data. Any other object a script hands
  to a DOM API (polyfill objects) is bound through a side table keyed by handle.
- `style` creates its object lazily. An element's inline `style` attribute is
  applied when its style object is created, so unwrapped elements cost nothing.
- **Kept from the legacy DOM for compatibility** (polyfills were tuned against it):
  - parsed attributes other than `id`/`class`/`style` are reflected as plain
    properties when the wrapper is created;
  - templates have a writable own `content` property;
  - comment wrappers carry their data as own properties, text wrappers use
    `Node.prototype`, templates `HTMLTemplateElement.prototype`, other elements
    `HTMLElement.prototype`;
  - `removeChild` still reports the removed node in the mutation record's
    `addedNodes`.

Mutations create wrappers only when something can see them: a mutation observer
that is interested in the change, a node that already has a wrapper, or a
`<script>` (dynamic script loading). Populating the page (apart from the
document's `html`, `head` and `body`) and `innerHTML` therefore create none for the
parsed nodes.

### 6.2 Lifetime and the concurrent collector

- **Tree objects.** Each tree that has wrappers owns a tree object: a GC object
  holding an array of the tree's wrappers. Every wrapper references its tree
  object. So all wrappers of a tree are reachable as soon as one is:
  - expandos on connected nodes survive (the document wrapper, reachable from
    `document`, keeps the document's tree object alive);
  - a child's wrapper keeps its detached parent tree alive;
  - a detached tree whose wrappers are all unreachable dies as a unit.

  This is tree-level wrapper tracing built from ordinary GC references, so the
  concurrent collector needs no new barrier: the store's arrays hold no GC
  references. It replaces the design's root scanning of a wrapper table (open
  question 3 no longer arises).
- **Freeing.** The engine does not call class finalizers, so the tree object gets a
  *handle* finalizer (`gc_add_handle_finalizer`, new; the existing
  `gc_set_handle_finalizer` searches the registry and is O(n) per call). It runs on
  the engine thread in the collector's finalizer step, before the sweep: it clears
  the dead wrappers' handles, frees their computed-style tables and frees the
  tree's slots.
- **Attach and detach.** Attaching merges the child's tree into the parent's (the
  smaller wrapper set moves, union by size). Detaching moves the subtree's wrappers
  to a new tree, or — when the subtree has none — leaves it for the caller, and
  operations that drop nodes (`innerHTML`, `textContent`, `replaceChildren`,
  `normalize`, `outerHTML`) free an unwrapped removed subtree at once.
- **Custom elements.** Upgrades are enqueued only when a subtree becomes connected,
  by a C walk that descends only into subtrees flagged as containing candidates
  (names with `-`), then `connectedCallback`s for the elements that have one.
  `document.createElement` of a defined name still upgrades synchronously.

---

## 7. Mutations, rendering and queries

**Mutations** are C operations on the store: relinking is O(1), `setAttribute` and
`textContent` replace strings. Each bumps a version (`structure_version`,
`attr_version`, `text_version`, `class_version` for `id`/`class`) and sets **dirty
bits** on the node (children / attribute / text) with a "dirty descendant" bit up
the ancestor chain, through shadow hosts.

**Rendering.** Layout reads an `HtmlDocument` snapshot (`dom_snapshot.c`):
- `html_parse()` returns a snapshot of the parsed store tree, built by a linear walk.
- The render snapshot of the live JS document (`html_document_from_js_dom`) also
  depends on JS state the store does not hold: style objects set through
  `element.style`, and ShadyDOM's composed shadow trees, which only exist behind
  polyfilled JS getters. It is built the way the legacy render was — the rendered
  tree written as HTML and parsed again — but from the store, with the store
  parser, and only reading JS for nodes that have wrappers (unwrapped nodes take
  the store's attributes and the style their inline attribute implies). It
  reproduces the legacy render exactly (§10) and costs 2–8 ms instead of 4–43 ms.
  Each snapshot element records its store node (`HtmlNode.store_id`), so hit
  testing returns real elements.
- Taking a snapshot clears the document's dirty bits.
  `html_document_from_js_dom_if_changed()` skips the rebuild when the rendered
  text hashes the same as the last snapshot; `elementFromPoint` uses it to keep its
  last layout, so repeated hit tests do no parsing and no layout.
- The main render loop still rebuilds on every layout request, because image loads
  also request a re-render without changing the DOM.

**Serialization** (`innerHTML`/`outerHTML` getters, `XMLSerializer`) walks the store
in C. SVG's camelCase attribute names (`viewBox`, …) are written back in their SVG
spelling, as the HTML parser's attribute adjustment would.

**Queries** (`dom_query.c`):
- Selectors are compiled once, cached by text (256 entries), and matched right to
  left on the store: type, `*`, `#id`, `.class`, attribute selectors (`= ~= |= ^= $=
  *=`, `i` flag), descendant / child / next-sibling / subsequent-sibling
  combinators, selector lists, `:not`/`:is`/`:where`, structural pseudo-classes,
  `:checked`/`:disabled`/`:enabled`, `:scope`. Other pseudo-classes match (as the
  legacy matcher did); pseudo-elements never match; an unparsable selector matches
  nothing.
- Candidates come from an index when the rightmost compound has an id, class or
  tag, restricted to the scope by preorder range; otherwise from a scan of the
  scope's subtree. Results are in document order.
- Behaviour changes from the legacy DOM (all spec behaviour, and none changed the
  YouTube or golden outputs):
  - combinators work (the legacy matcher ignored them);
  - id and class matching is case-sensitive;
  - `Element.querySelector`/`querySelectorAll` do not test the element itself;
  - `getElementById`/`getElementsByTagName`/`getElementsByClassName` return
    connected elements in document order, including script-created ones;
  - `elementFromPoint` returns real elements.

---

## 8. Resource loading (`dom_fetch.c`)

- When the page's scripts are extracted, every external script URL is handed to a
  **fetch pool** (6 threads by default, `CYBER_FETCH_THREADS`; 0 disables it),
  separate from the job system because fetches block on sockets.
- Scripts still **execute in document order**: the executor asks for script i with
  `dom_fetch_script()`, which waits for that fetch (counters: queued, completed,
  hits, waits, wait time) or fetches synchronously when the URL was never queued.
  Dynamically inserted `<script src>` uses the same entry point.
- Concurrent requests needed three fixes in the network stack:
  - Mbed TLS / PSA crypto is built with its threading layer
    (`MBEDTLS_THREADING_C`, `MBEDTLS_THREADING_PTHREAD`); without it concurrent
    handshakes corrupted the PSA key store (heap corruption seen in testing);
  - the connection-pool mutex is statically initialized (two first requests raced
    on `pthread_mutex_init`);
  - the cookie jar has a mutex and each thread builds its `Cookie` header in a
    thread-local buffer.
- The Windows `pthread.h` shim gained condition variables
  (`CONDITION_VARIABLE` over its SRW locks) and `pthread_detach`, which the pool
  and Mbed TLS's threading layer use.
- Stylesheets and images keep their existing loaders (the CSS URL cache and the
  image cache).

---

## 9. Plan, and what was built

**Gates** (all phases): golden layout dumps identical; JS suites and
`test_browser_api_impl` with only pre-existing failures; the YouTube data-script
test unchanged; ThreadSanitizer clean on the new threads; Windows syntax check.

| Phase | Planned | Built |
|---|---|---|
| **0. Quick fixes** | Switches read once; per-element functions onto prototypes; upgrades on connect; no fixed buffers | As planned. `CYBER_ENV(bit)` flags word and a cached `cyber_getenv` replace every `getenv("CYBER_*")` (QuickJS hot paths included). The fixed buffers went with `DOMNode` itself in phase 2. |
| **1. Node store and tokenizer** | Store, token arrays, tokenizer and builder reproducing `html_parse`; layout reads the store; serialize + re-parse removed | Store, tokenizer and builder as planned; `html_parse` is now parse-into-store plus a snapshot. **Deviation:** layout still reads an `HtmlDocument` snapshot, and the JS-document render keeps the write-and-parse step (now from the store), because the render depends on JS-side style objects and polyfill shadow trees; it is 4–5× cheaper, exact, and skipped when unchanged. |
| **2. Lazy wrappers** | DOM APIs over the store, tree-level tracing, prototype accessors | As planned, with tree objects instead of root scanning (§6.2). `DOMNode`, its GC tables and the per-element objects are gone. |
| **3. Parallel parsing** | Prescan, speculative chunks with validation, parallel post-processing and indexes | Prescan, chunks and validation as planned; class/tag indexes by parallel counting sort, built lazily. Chunking defaults to inputs ≥ 2 MB (§5.2). |
| **4. Preload scanner** | Fetch pool, in-order execution | Fetch pool for external scripts, in-order execution, counters (§8). The prescan does not feed it: the script extractor already runs before execution and sees the same URLs. |
| **5. Dirty bits into style and layout** | Mutation records into the CSS invalidation map and layout dirty propagation; index-driven queries | Index-driven compiled queries; dirty bits and versions maintained and consumed by the render snapshot; rebuild skipped when the render is unchanged. **Not built:** incremental restyle/relayout of only the changed parts. It needs the layout engine to read the store instead of a rebuilt `HtmlDocument` (phase 1 deviation), which is the next step. |

---

## 10. Testing

**New tests** (`tests/test_dom.c`, run by `cyberbrowser-tests`, 16 tests):
- **Parser equivalence** with the legacy parser (kept in
  `tests/legacy_html_parse.c` as the oracle), comparing kinds, tags, attributes in
  order and text: 20 fixtures (unclosed and stray tags, duplicate attributes,
  `</div>` inside script strings, `</scrip`, comments, entities, CRLF, void and
  self-closing tags, tables, SVG, content after `</body>`), 400-deep nesting, the
  YouTube test page, and 400 generated malformed documents.
- **Chunking:** the same tree for chunk sizes 1, 7, 64, 333, 1,000 and 4,096 bytes
  with forced boundaries (inside scripts and comments), and with prescan
  boundaries. Separately: a 10 KB attribute value survives (no length limits).
- **Wrappers:** identity across `getElementById`, `querySelector`,
  `getElementsByClassName` and navigation; laziness (1,500 parsed nodes, no
  wrappers until queried); expandos across repeated GC cycles; detached trees
  freed once unreachable; an unwrapped removed subtree freed at once; a child
  wrapper keeping its detached parent tree (and its expandos) alive; nodes moved
  back and forth between trees across collections.
- **Queries and indexes:** combinators, `:not`, `:is`, `:nth-child`, attribute
  operators, `:checked`/`:disabled`, `:scope`, document order of selector lists,
  `querySelector` excluding self, `matches`/`closest`, unparsable selectors; index
  tracking of insertion, class change, id change and removal; a 12,000-node
  document where the class index (built with range jobs) equals a brute-force scan.
- **Fetch pool:** six 60 ms fetches overlap (total < 300 ms) and come back in
  order with the right bodies; an unqueued URL is fetched synchronously.
- **Render snapshot:** reuse when unchanged, rebuild after a mutation and after an
  inline-style change (no store version moves), dirty bits cleared, `store_id`
  set on snapshot elements.

**Gates, as run:**
- **Golden layouts:** all 24 dumps identical to the pre-change reference, also with
  `DOM_CHUNK=512 DOM_CHUNK_FORCE=1` and 4 threads.
- **JS DOM equivalence** (`jsgold`, new harness: populate, optionally run the
  page's scripts, then write the render snapshot and lay it out): identical
  serialized DOM and layout to the pre-change build on all 13 fixtures, and after
  running the YouTube pages' scripts. Exact comparison needs
  `CYBER_DOM_LEGACY_LIMITS=1`, which reproduces the legacy `DOMNode`'s truncations
  (attribute names at 63 bytes, values at 2,047, text at 4,095) and its
  id-and-class-first attribute order; without it the only differences are those
  truncations (now gone) and attribute order (now source order).
- **`cyberbrowser-tests`:** 363 passing; the 4 failures (`matchMedia` queries,
  two shadow-root existence checks, flex box sizing) fail identically before the
  change. (`test_location_setters` crashes in both builds and is skipped, as
  before.)
- **`youtube-data-test`:** unchanged — scripts 000–036 succeed (018 skipped as
  JSON-LD), then script 037 overflows the stack in unbounded JS recursion, as it
  does in the pre-change build.
- **ThreadSanitizer:** no reports on parse jobs, index jobs, layout, the fetch pool
  or the network stack. The remaining reports are all between QuickJS and the
  concurrent collector's thread (write barriers, shape tables), unchanged by this
  work.
- **Windows:** every changed C file passes `clang --target=x86_64-w64-mingw32
  -fsyntax-only` with the project's Windows defines, except one pre-existing
  error in `tls_client.c` (`CertOpenSystemStoreA(NULL, …)` with strict MinGW
  headers).

---

## 11. Open questions (not addressed)

1. **`document.write` during parsing.** Not supported: scripts run after the whole
   page is parsed. Supporting it would need the parser to pause at parser-blocking
   scripts; speculative tokenizing past them stays valid, but tree building would
   interleave with script execution.
2. **Spec tree construction.** The builder reproduces the legacy parser (no implied
   end tags, no adoption agency). Matching browsers changes trees and layouts and
   should be its own phase with a pixel comparison against Chrome.
3. **Wrapper-table scanning.** Moot with tree objects (§6.2): there is no wrapper
   root table to scan.
