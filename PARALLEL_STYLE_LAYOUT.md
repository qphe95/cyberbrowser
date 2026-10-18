# Parallel style and layout

This replaces `PARALLEL_CSS_APPLICATION.md`, `PARALLEL_CSS_V2.md` and
`PARALLEL_CSS_LAYOUT.md`. It describes the style and layout pipeline as implemented:
stylesheet parsing, selector matching and the cascade ("style"), and box layout.

Like standard browser engines, the pipeline runs **synchronously** on the engine (JS)
thread: `css_layout_run()` returns when style and layout are done. Inside that call
the work is parallel, in the way Stylo (Servo's and Firefox's style system)
parallelizes style, and built from game-engine patterns:

| Pattern | Used for |
|---|---|
| **Job system**: work-stealing deques, dependency counters, last-finisher continuations, a waiting thread that runs jobs | All parallel work (§3) |
| **Data-oriented tables**: flat arrays indexed by node, interned integer names, compiled selector arrays | Matching compares integers, not strings (§5) |
| **Frame arenas**: per-thread bump allocators, blocks recycled through a lock-free pool | Jobs, custom-property maps, per-thread scratch (§4) |
| **Insert-only lock-free tables** (CAS on empty slots) | Atoms, glyph metrics, kerning pairs (§5.1, §7) |
| **Caches keyed by content** | Parsed sheets, compiled style sets, fetched sheet text, glyph metrics (§6, §7) |
| **Optimistic parallelism with validation** | Chunked sheet parsing: chunks parse in parallel from guessed boundaries, then are checked and re-parsed if a guess was wrong (§6.2) |

Hard rules:
- workers never touch the GC heap, QuickJS or the JS DOM;
- no mutexes or spinlocks; no job body waits on another job's flag;
- the output is identical for any number of threads, any steal order and any job
  grain.

---

## 1. Results

Wikipedia main-page fixture (3,952 layout nodes, 2,331 elements, 9 sheets, 1,206
rules). Release build, 2-core container. Times are for the whole of
`css_layout_run` (tree build, style, layout):

| | Old engine | New, first layout | New, cached sheets |
|---|---|---|---|
| 1 thread | — | 20.8 ms | 9.6 ms |
| 2 threads | 1,010–1,270 ms | 18.9 ms | 6.6 ms |

The first layout of a page parses and compiles its sheets. Later layouts of the same
sheets — hit tests, re-renders, the same page at the same viewport — reuse both.

The other fixtures:

| Page | Old | New (cached) |
|---|---|---|
| YouTube watch page (350 nodes) | 5.9 ms | 0.47 ms |
| Random page, 1,421 nodes, 250 rules | 72 ms | 4.5 ms |
| Random page, 1,357 nodes | 41 ms | 4.1 ms |

Stages of a cached Wikipedia layout, 2 threads (`CSS_LOG=1`):

| Stage | Time |
|---|---|
| Tree build | 0.8 ms |
| Collect sheets, cache lookup | 0.7 ms |
| Element features (§5.3) | 1.4 ms |
| Style, parallel (§5.4–5.7) | 4.4 ms |
| Layout, flow + absolute (§8) | 1.8 ms |

Matching statistics on that page:
- 71,146 candidate selectors were tested out of 5.3 M (rule, element) pairs.
- 84% of the candidates were rejected by the ancestor Bloom filter, leaving 11,153
  full matches.
- 484 of 2,331 elements took their style from the sharing cache.

Matching dropped from about 10 G instructions to 8 M.

---

## 2. Why the earlier attempts did not work

**Almost all of the time was brute-force matching.** Callgrind on the old engine:

| Work | Share |
|---|---|
| Selector text re-parsed at match time (`::before`/`::after` scans, specificity), mostly `memset` of 2.6 KB selector structs | ≈ 33% |
| Every `@media` string re-parsed per rule per node | ≈ 23% |
| Every rule tested against every node, twice (custom-property pass, then the cascade) | ≈ 40% |
| The cascade itself | 0.3% |
| Layout | < 0.1% |

**The old designs parallelized the wrong things in the wrong way:**
- *Spin-waiting layout passes.* Nodes spun on their parent's and previous sibling's
  flags, inside pool jobs. A spinning job holds a thread whose dependency may still
  be queued, so it can livelock. The model also could not place blocks, because a
  child's y depends on its siblings' heights; that code had already been replaced by
  a serial pass.
- *Grey/black publication.* Workers wrote JS-visible, GC-heap tables. That ties CSS
  to the concurrent moving collector and to QuickJS, which is not thread-safe, and it
  parallelized the cheap part.

**The code that existed had races:**
- workers wrote `::after` text into the shared document, and their order depended
  on timing;
- selectors were compiled lazily, without synchronization;
- measuring text wrote the glyph atlas.

---

## 3. Job system (`src/job_system.c`)

- One worker per core except one. Each owns a fixed-size **Chase–Lev deque**:
  - the owner pushes and pops at the bottom;
  - thieves steal from the top with one CAS;
  - a release store on `bottom` publishes a job, and a thief's acquire load sees
    everything written before it.
- The thread that calls `job_graph_run()`, normally the engine thread, becomes
  thread 0 for the graph and **runs jobs until the graph is done**.
- Idle workers spin briefly, announce themselves as sleepers, check for work once
  more (no lost wake-ups), then sleep on an address. `job_spawn` wakes them only
  when sleepers exist. Sleeping with nothing to do is the only blocking.
- **Counters.** A job's `pending` counts itself plus its outstanding children.
  Whoever brings it to zero runs the job's **continuation**, if any, and then
  notifies the parent: the last finisher carries the parent forward.
  - Sheet parsing uses this: chunk jobs, then a merge continuation (§6.2).
  - Style uses plain spawning: a parent is styled before its subtrees are spawned
    (§5.4).
- **Fork-join groups** (`job_group_new/spawn/wait`) serve recursive code that cannot
  be split into continuations, i.e. layout (§8). Waiting runs jobs, the waiting
  thread's own children first. A group waits only for its own descendants, so it
  cannot deadlock.
- A deque that is full runs the job inline. A thread outside the pool that starts a
  graph while another thread owns the pool runs it inline, depth-first.
- Jobs come from the calling thread's frame arena. The arenas are reset when the
  graph finishes, when every job has run.

Switches:
- `CSS_THREADS=<n>`: total threads; 1 runs everything inline.
- `CSS_STEAL=random`: random victim order plus random yields, to shuffle schedules
  in tests.

## 4. Frame arenas (`src/frame_arena.c`)

- A bump allocator over 256 KB blocks.
- Blocks come from a process-wide **Treiber stack** whose head carries a 16-bit
  generation tag, so ABA cannot happen, and return to it on reset. Oversized
  requests get their own block.
- `frame_arena_absorb` moves one arena's blocks into another. Per-thread maps built
  during style end up owned by the layout context that outlives the pass.

---

## 5. Style (`src/css_style.c`, style pass in `src/css_layout.c`)

### 5.1 Atoms

- Tag, id, class and attribute names are interned, lower-cased, into a `CssAtom`
  (`uint32_t`).
- The table is open addressing with **lock-free insert** (a CAS on an empty slot).
- Capacity is reserved before jobs start (`css_atom_reserve`, from upper bounds on
  the names to be interned), so the table never resizes while jobs run.
- It is separate from QuickJS atoms.

### 5.2 Compiled style set (`CssStyleSet`)

Built once per (sheets, viewport) and immutable afterwards:
- **Selectors.** Every branch of every selector list is compiled from the existing
  parser's structures into atom compounds. The matcher therefore has the legacy
  matcher's exact semantics, including its quirks (greedy descendant matching,
  sibling combinators that never match, name truncation, attribute selectors that
  test presence only).
- **Buckets.** A branch is filed under one key of its rightmost compound: id, else a
  class, else an attribute name, else a tag, else the universal list. Each bucket
  map is a sorted array with an open-addressing index.
- **Bloom requirements.** Each branch carries up to 8 atoms that its ancestor
  compounds require (tags, ids, classes).
- **Media.** Each rule's `@media` condition is evaluated once, at build time. Rules
  that fail never enter a bucket.
- **Pseudo-elements.**
  - `::after` branches go to their own buckets, keyed by their base selector.
  - The "aspect" base (text before `:before`/`:after`) is compiled per rule.
  - This replaces the per-node string scans.
- **Revalidation chains.** These are branches that test sibling position or
  `:empty`, directly or inside `:where()`/`:is()`. They are used by style sharing
  (§5.6).

### 5.3 Element features (`CssStyleDoc`)

Per DOM index, in flat arrays:
- tag, id, class atoms and attribute-name atoms;
- the link flag and the "has children" flag;
- the parent;
- the four sibling positions (first, last, first-of-type, last-of-type), counted the
  way the legacy `css_element_index` counted them.

Built once per layout, immutable while jobs run.

### 5.4 Range jobs

A style job styles the subtree at a preorder position; a subtree is a contiguous
preorder range.
- It styles the node, then walks its children.
- Child subtrees larger than `CSS_GRAIN` nodes (default 128) are spawned as jobs.
  Smaller ones are styled in place.
- A parent is always final before its children are visited, and every node writes
  only its own slot. No job waits.

### 5.5 One visit per element

1. **Ancestor Bloom filter**, per thread: push on descent, pop to the parent on
   ascent, rebuilt from the parent chain when a thread starts on an unrelated
   subtree.
2. **Match.**
   - Collect candidates from the buckets of the node's id, classes, attribute names
     and tag, plus the universal list.
   - Reject a candidate whose Bloom atoms are missing from the filter.
   - Match the rest right to left.
   - A rule's specificity is that of its most specific matching branch.
   - Declarations are sorted by the legacy comparator: importance, then
     specificity, then the order key `sheet*1e6 + rule*1e3 + decl`.
3. **Custom properties.**
   - A node's `--*` declarations, then its inline ones, go into a **copy-on-write
     map**. A node that sets none shares its parent's map pointer.
   - Maps live in the thread's arena.
   - This replaces the separate serial pass that matched every selector a second
     time.
4. **Aspect ratio and `::after` content**, as in the legacy code but through the
   compiled tables.
5. **Cascade**: `layout_apply_declaration` per declaration, with `var()` resolved,
   then inline style.
6. **Inheritance and fix-ups.** The parent is final, so all of these happen in the
   same visit:
   - em and % font sizes, unitless line-height, deferred em margins;
   - inherited properties;
   - blockification.

### 5.6 Style sharing (Stylo's sharing cache)

- Each thread keeps its last 32 styled elements.
- An element without an id or inline style copies a cached element's style when
  all of these hold:
  - the same tag, classes and attribute names;
  - an equivalent parent style;
  - equal results for every revalidation chain (computed only when a candidate
    passes the cheap tests).
- Parents are equivalent when they have the same style **equivalence class**: a
  shared element inherits its source's class. Equivalent parents therefore have
  feature-equal ancestor chains, so every non-positional selector gives the same
  answer for both elements.
- `CSS_SHARE=0` disables it. Sharing is also switched off when a style set has more
  than 512 revalidation chains.

### 5.7 Generated content

`::after` strings found by workers are collected per thread. After the pass they are
sorted by preorder position and written into the document on the engine thread.

---

## 6. Stylesheets

### 6.1 Collection and caches

- Sheets are collected as text in cascade order:
  - the UA sheet;
  - `<style>` and `<link>` elements in tree order;
  - constructed and adopted sheets;
  - the caller's sheet.
- **Sheet cache.** Parsed sheets are cached by text (64-bit hash plus exact
  comparison) and shadow scope; 128 entries, least recently used first.
- **URL cache.** Fetched `<link>` texts are cached by URL for the life of the
  process, so a hit test or re-render never downloads a sheet again.
- **Style-set cache.** Compiled style sets are cached by (sheet entries, viewport);
  4 entries. A sheet referenced by a cached style set is never evicted.
- Sheets that parse to no rules are dropped, as before, so order keys are
  unchanged.

### 6.2 Parallel parsing

- Each sheet missing from the cache is a job.
- A sheet larger than `CSS_SHEET_CHUNK` bytes (default 32 KB) is cut at guessed
  top-level boundaries (just after a `}` that closes depth 0, outside comments and
  strings). Each chunk is parsed by `css_stylesheet_parse_range` in a job of its
  own.
- A **continuation** run by the last chunk to finish checks that each chunk started
  exactly where the previous one stopped. The top-level parser carries no state
  besides its position, so matching boundaries mean an identical result. A chunk
  that guessed wrong is re-parsed from the true position.
- The result always equals a single serial parse. A test checks this with chunks of
  1 byte and up, on sheets full of braces in strings, comments and `url()`.

---

## 7. Text measurement (`src/text_shaper.c`)

- Measuring no longer touches the glyph atlas. Each font has an insert-only
  **metrics table** (codepoint → glyph index and advance, in 256-entry pages
  published by CAS) and a **kerning-pair cache** (open addressing, CAS insert).
- The values are those `ensure_glyph` computed, including the missing-glyph
  fallback, so widths are unchanged.
- The atlas is filled only by the display-list builder, on one thread.
- This made measurement thread-safe. It also made layout about 6× faster on text,
  because the old path rasterized every glyph it measured and did a linear search
  per character.

---

## 8. Layout

- Layout is the existing recursive algorithm, run inside a job graph.
- **Flex containers and table rows** place every item before laying out any item's
  subtree; the code was split into "place all", then "lay out all". Those subtrees
  are units: each reads only its own boxes and writes only its own boxes.
- **Floats** are the exception. The engine keeps one page-wide float stack in
  absolute coordinates, and a float inside one item can intrude into a later
  item's lines.
- So a container's items fork into jobs (`layout_units`) only when no item subtree
  contains a float. Then the float stack is only read, and each item computes
  exactly the same arithmetic as before. Otherwise the items run in order.
- Switches: `CSS_LAYOUT_PAR=0` turns the forking off; `CSS_LAYOUT_GRAIN` (default 64
  nodes) is the smallest group worth forking.

On the Wikipedia page, 4 regions run in parallel. The article column stays serial
because of its floated thumbnails. Layout is 1.8 ms there, so the parallel gain is
small. A wider gain needs spec-correct, BFC-scoped floats (§11).

---

## 9. Invariants

| # | Invariant | Mechanism |
|---|---|---|
| S1 | Workers never touch the GC heap, QuickJS or the JS DOM. | Jobs read the layout tree, `CssStyleSet`, `CssStyleDoc`, font metrics and boxes. Constructed sheets are read from JS on the engine thread before jobs start. |
| S2 | No job body waits on another job's flag. | Dependencies are counters. Only a group wait or a graph wait waits, and they run jobs. |
| S3 | Each box is written by one job. | Range jobs own subtrees. Layout units own item subtrees. A shared style is copied only from an element the same thread finished. |
| S4 | Shared tables are immutable or insert-only while jobs run. | Atoms (capacity reserved first), style set, element features, glyph metrics, kerning cache. |
| S5 | The output is independent of thread count, steal order and grain. | Per-node results, generated content merged in preorder, parse chunks validated, layout forks only float-free units. |
| S6 | Caches never return stale data. | Content keys with exact comparison. Style sets pin their sheets. |

---

## 10. Testing

**Golden oracle** (`golden/` in the working tree, not shipped):
- A harness dumps every `LayoutBox` field, all text, and the display list (glyph
  indices included) for 12 fixtures at two viewports: Wikipedia, YouTube, selector
  and layout coverage pages, and 8 random pages.
- The **oracle** is the old engine forced onto one thread. The old two-thread engine
  differed from it on one random page: its `::after` race.
- The new engine matches the oracle byte for byte with 1 to 8 threads, grain 1 to
  128, `CSS_STEAL=random`, sheet chunks of 17 bytes, and sharing on or off.
- **Fuzz:** 160 more random pages, including sibling combinators, attribute values,
  `:not`, `:is`, nth-of-type and upper-case selectors. All identical.

**ThreadSanitizer** (gcc): no reports on the fixtures under
`CSS_THREADS=3–4 CSS_GRAIN=1–4 CSS_LAYOUT_GRAIN=1–4 CSS_SHEET_CHUNK=500
CSS_STEAL=random`.

**Unit tests** (`tests/test_css_style.c`; they replace the three tests that called the
removed `css_apply_document_styles`):

| Test | What it checks |
|---|---|
| Continuations | A 200,000-element sum tree of jobs with continuations |
| Nested groups | Fork-join groups nested inside jobs |
| Back-to-back graphs | Graphs run one after another |
| Frame arenas | Alignment, oversized blocks, absorb, recycling |
| Atoms | Case folding, and the same ids from 8 concurrent jobs |
| Matcher vs legacy | For every (rule, element) pair of 12 random documents: same match, same specificity, cascade order sorted |
| Chunked parsing | Equals serial parsing for 40 adversarial sheets at 6 chunk sizes |
| Text measurement | 16 concurrent jobs measure exactly what a serial run measures |
| Determinism | Identical layout with `CSS_SHARE=0`, `CSS_GRAIN=1`, `CSS_LAYOUT_GRAIN=1`, `CSS_LAYOUT_PAR=0`, `CSS_SHEET_CHUNK=13`, `CSS_SHEET_CACHE=0`, and with warm caches |

**Switches**

| Variable | Effect |
|---|---|
| `CSS_THREADS=<n>` | Threads in the pool (read once) |
| `CSS_STEAL=random` | Random victims and yields (read once) |
| `CSS_GRAIN=<n>` | Style subtree size that becomes a job (128) |
| `CSS_SHARE=0` | No style sharing |
| `CSS_LAYOUT_PAR=0`, `CSS_LAYOUT_GRAIN=<n>` | Layout forking off / smallest forked group (64) |
| `CSS_SHEET_CHUNK=<bytes>` | Parse chunk size (32 KB) |
| `CSS_SHEET_CACHE=0` | Drop the sheet and style-set caches before each layout |
| `CSS_LOG=1` | Per-stage times and match, sharing and layout statistics |
| `CSS_DUMP=<path>` | Write the box dump (by default only the main render writes `layout_dump.txt`) |

---

## 11. Changes in behaviour, and what is not done

**Behaviour changes**
- `layout_dump.txt` is written only by the main render (the browser and the
  `CYBER_LAYOUT_HTML` probe), not by every layout. Hit tests used to rewrite it.
  `CSS_DUMP` forces it for any layout.
- `::after` text injection is deterministic (preorder). The old order depended on
  thread timing.
- The rendered JPEG can differ from before at the edges of a few glyphs, such as
  list bullets. The atlas is now filled in paint order instead of measurement order,
  and the rasterizer's filtering reads neighbouring atlas texels. Layout and the
  display list are unchanged.
- Measurement no longer drops glyphs when the atlas is full.
- `html_parse` no longer keeps a stale node pointer across child parsing; that was a
  crash on large pages.

**Not done yet**
- **Computed-style structs (§4.5 of the first design).** Values still live in the
  984-byte `LayoutBox`, and sharing copies it. Splitting inherited, box and visual
  structs shared by pointer would cut memory, and turn sharing into pointer copies.
- **Typed declarations.** Properties are still matched by name in
  `layout_apply_declaration`. A `PropId` per declaration at parse time would remove
  the `strcasecmp` chain, one of the larger remaining costs of the style pass.
- **Incremental restyle and relayout.** Every layout rebuilds the tree from an
  `HtmlDocument`. Dirty bits need DOM change tracking, and a stable node identity
  across rebuilds.
- **BFC-scoped floats.** Floats that stay inside their block formatting context, as
  the spec says, would let block formatting contexts lay out in parallel and be
  cached by constraint. That changes layout output, so it needs its own
  pixel-comparison pass against Chrome.
- **Splitting inside one large `@media` block.** Chunk boundaries are only at depth
  0.
