# Parallel CSS Application Design

## Goal

Apply CSS selectors and computed-style property writes in parallel across the
GC thread pool, while keeping the DOM and the GC safe when an element is
allocated but its dependent properties (tree links, attributes, computed style,
inline style) are still being written.

The design stays in the project's existing **game-engine C style**: hand-rolled
atomics (`_Interlocked*` / `__sync_*`), GC `GCHandle` indirection, no
`std::atomic`, no `stdatomic.h`, explicit Windows/POSIX splits, and fixed-size
or double-buffered data structures rather than generic resizing containers.

---

## The Core Problem

Today `css_apply_node_styles_parallel()` only parallelizes **selector
matching**. The actual writes to `element.style` are still done serially on the
mutator thread because:

1. QuickJS objects are not safe for concurrent mutation.
2. A DOM element is not atomically created: allocation, tree linking,
   attribute copying, `style` object creation, and CSS property writes happen
   as separate steps.
3. Readers such as `querySelector`, `getElementById`, and the GC can observe
   the element in the middle of construction.

To parallelize the write phase we need:

- A **publication state** so a partially-built element is invisible to readers.
- **Thread-safe lookup tables** (id/class/tag indices and computed-style maps)
  that workers can write concurrently without locks.

---

## 1. Object Publication State ("Construction Grey")

We add a small, atomic publication state to every runtime DOM object.  This is
*separate* from the GC tri-color state, but the idea is the same: a "grey"
object exists but is not yet reachable through normal paths.

```c
typedef enum {
    PUBLISH_UNBORN = 0,   /* slot not yet used                */
    PUBLISH_GREY   = 1,   /* object allocated, being written  */
    PUBLISH_BLACK  = 2,   /* fully constructed and visible    */
} PublishState;
```

### Where to store it

We store publication state in a **parallel `uint32_t` array indexed by
`GCHandle`** inside `GCState` (`quickjs_gc_unified.h/cpp`).  This avoids
bloating every struct and works for any GC-managed object, not just DOM nodes.

```c
typedef enum GCPublishState {
    PUBLISH_UNBORN = 0,
    PUBLISH_GREY   = 1,
    PUBLISH_BLACK  = 2,
} GCPublishState;

GCHandle gc_alloc_grey(size_t size, JSGCObjectTypeEnum gc_obj_type);
void     gc_publish(GCHandle handle);
void     gc_unpublish(GCHandle handle);
GCPublishState gc_publish_state_load(GCHandle handle);
bool     gc_is_published(GCHandle handle);
```

Ordinary `gc_alloc()` still publishes immediately (`PUBLISH_BLACK`) so the
rest of the engine continues to work unchanged.  New parallel-construction
code can allocate with `gc_alloc_grey()` and call `gc_publish()` when the
object is fully initialized.

### Construction protocol

```c
GCHandle elem = gc_alloc_grey(sizeof(DOMNode), JS_GC_OBJ_TYPE_DATA);
/* 1. object is already grey; no reader can observe it yet */

/* 2. write all dependent fields (no reader observes these yet) */
dom_set_attributes(elem, attrs);
dom_link_into_tree(elem, parent);
css_computed_create(elem);
element_ensure_style_object(elem);

/* 3. parallel CSS workers write computed properties */
/*    (each element is owned by exactly one worker)   */

/* 4. publish with a release store */
gc_publish(elem);
```

### Reader rule

Every lookup path treats non-`BLACK` objects as absent:

```c
GCHandle found = hash_lookup(class_table, "my-class");
if (found != GC_HANDLE_NULL &&
    atomic_load_u32(&publish_state[found]) == PUBLISH_BLACK) {
    return found;
}
return GC_HANDLE_NULL;
```

### GC interaction

A grey object must not be collected while it is being constructed.  The simplest
way is to keep construction on a thread that has a temporary root for the new
handle.  In practice this means:

- The constructing thread owns the handle in a local variable or a small
  thread-local "pending creation" array until publication.
- The GC only traces through `BLACK` objects; grey objects are opaque roots for
  the duration of the construction job.
- Once `PUBLISH_BLACK` is stored, normal reachability (parent pointers, hash
  tables, etc.) takes over.

This also means the DOM node class finally needs a real `gc_mark` callback that
marks `parent_node`, `first_child`, `next_sibling`, and the computed-style
handle.  Without it the `GCValue` fields inside `DOMNode` are invisible to the
collector.

---

## 2. Lock-Free Hash Table

We need several lookup tables:

| Table | Key | Value |
|-------|-----|-------|
| `id_table` | string/atom | one `GCHandle` |
| `class_table` | string/atom | head of a list of `GCHandle`s |
| `tag_table` | string/atom | head of a list of `GCHandle`s |
| `computed_style` (per element) | property atom | CSS value handle/string |

The project already has atomic primitives and `GCHandle` indirection.  For a
game-engine renderer the most natural shape is **open addressing with atomic
bucket CAS**:

- Flat array of buckets → excellent cache locality.
- No per-entry GC allocation for one-to-one tables.
- Lock-free inserts via CAS on an empty bucket.
- Wait-free lookups (probe until match or empty).
- Fixed bucket count; rebuild by swapping the table pointer.

We recommend **linear probing** because it is the simplest and most
Cache-friendly.  Quadratic probing is also viable if primary clustering becomes
noticeable, but it pays a cache-locality penalty and requires a table size that
avoids secondary clustering cycles.  Keep the load factor ≤ 0.5 for linear
probing and the probe chains stay short.

### One-to-one table layout (e.g. computed style, id index)

```c
typedef struct CssHashBucket {
    uint32_t key_hash;      /* pre-computed hash of the key   */
    GCHandle key_handle;    /* handle to key string/atom      */
    GCHandle value_handle;  /* handle to value                */
} CssHashBucket;

typedef struct CssHashTable {
    uint32_t       bucket_count;  /* power of two, fixed      */
    CssHashBucket  buckets[1];    /* variable-length          */
} CssHashTable;
```

Empty buckets are `{ 0, GC_HANDLE_NULL, GC_HANDLE_NULL }`.  Because
`key_handle == GC_HANDLE_NULL` is never a valid key, that single sentinel is
enough.

### Insert

```c
void css_hash_insert(CssHashTable *t, uint32_t hash,
                     GCHandle key_handle, GCHandle value_handle)
{
    uint32_t mask = t->bucket_count - 1;
    uint32_t idx  = hash & mask;

    for (uint32_t probe = 0; probe < t->bucket_count; probe++) {
        CssHashBucket *b = &t->buckets[idx];

        /* Try to claim an empty bucket. */
        if (atomic_load_u32(&b->key_handle) == GC_HANDLE_NULL) {
            /* Write payload first, then publish the key with a CAS. */
            b->key_hash    = hash;
            b->value_handle = value_handle;

            if (atomic_compare_exchange_u32(&b->key_handle,
                                            GC_HANDLE_NULL,
                                            key_handle) == GC_HANDLE_NULL) {
                return;
            }
            /* Someone else took this slot; fall through to check it. */
        }

        /* If the key is already present, update the value. */
        if (b->key_hash == hash && b->key_handle == key_handle) {
            b->value_handle = value_handle;
            return;
        }

        idx = (idx + 1) & mask;   /* linear probe */
    }

    /* Table is full.  Rebuild and retry, or abort for this document. */
}
```

### Lookup

```c
GCHandle css_hash_lookup(CssHashTable *t, uint32_t hash, GCHandle key_handle)
{
    uint32_t mask = t->bucket_count - 1;
    uint32_t idx  = hash & mask;

    for (uint32_t probe = 0; probe < t->bucket_count; probe++) {
        CssHashBucket *b = &t->buckets[idx];
        GCHandle bucket_key = atomic_load_u32(&b->key_handle);

        if (bucket_key == GC_HANDLE_NULL)
            return GC_HANDLE_NULL;   /* empty slot → not found */

        if (b->key_hash == hash && bucket_key == key_handle)
            return b->value_handle;

        idx = (idx + 1) & mask;
    }

    return GC_HANDLE_NULL;
}
```

### Why this beats chained buckets here

- **One allocation** for the whole table; no per-insert `gc_alloc` during the
  hot parallel CSS pass.
- **Cache-friendly**: probing touches contiguous memory.
- **Simpler code**: no `next` pointers, no handle-to-entry dereferences on
  every lookup.
- **Natural fit for computed-style tables**: one property atom maps to one
  value.

### Class/tag tables (one-to-many)

Open addressing still works as the outer map, but the value is the head of a
small lock-free list of elements:

```c
typedef struct ElemListNode {
    GCHandle elem;
    GCHandle next;
} ElemListNode;

typedef struct ClassIndexBucket {
    uint32_t  key_hash;
    GCHandle  key_handle;     /* class/tag string handle */
    GCHandle  list_head;      /* head of ElemListNode chain */
} ClassIndexBucket;
```

Insert first probes for an existing bucket with the same key.  If found, it
prepends a new `ElemListNode` to the list with a CAS on `list_head`:

```c
GCHandle node = gc_allocz(sizeof(ElemListNode), JS_GC_OBJ_TYPE_DATA);
ElemListNode *n = (ElemListNode *)gc_deref(node);
n->elem = elem;

GCHandle expected;
do {
    expected = atomic_load_u32(&bucket->list_head);
    n->next = expected;
} while (atomic_compare_exchange_u32(&bucket->list_head,
                                    expected, node) != expected);
```

If no bucket exists, claim an empty bucket with a CAS on `key_handle` exactly
like the one-to-one table.

### Resizing

Lock-free in-place resizing is hard, so we follow the game-engine pattern:

1. Size the table at creation time to the next power of two that gives a load
   factor ≤ 0.5.
2. When load exceeds the threshold, allocate a larger table on the side,
   rehash every entry into it, then atomically swap the pointer readers use:

```c
CssHashTable *new_table = css_hash_rebuild(old_table, new_bucket_count);
atomic_store_ptr((void *volatile *)&global_class_table, new_table);
```

Old readers continue with the old table; new readers pick up the new one.  For
short-lived documents the simplest policy is to **never resize**: just size
generously up front and accept the memory cost.

### Implementation note

A reusable one-to-one lock-free hash table is implemented in
`browser-emulator/include/lockfree_hash_table.h` and
`browser-emulator/src/lockfree_hash_table.cpp`.  It uses the same three-state
bucket (`EMPTY` → `WRITING` → `OCCUPIED`) plus tombstones, and exposes
`lf_hash_create`, `lf_hash_insert`, `lf_hash_lookup`, `lf_hash_remove`, and
`lf_hash_resize`.  The atomic primitives used by the table were moved from
`quickjs_gc_unified.cpp` into `quickjs_gc_unified.h` so other modules can use
them without duplicating the Windows/POSIX split.

---

## 3. Parallel CSS Application Flow

```
1. html_parse() creates HtmlDocument and HtmlNode tree (not GC objects).

2. html_populate_js_document() creates JS element objects one by one.
   For each element:
       allocate DOMNode
       atomic_store(publish_state, PUBLISH_GREY)
       initialize fields, attach to JS object, link into parent
       insert into id/class/tag tables (still grey)

3. css_apply_document_styles_parallel()
   a. Collect stylesheets (serial, mutator thread).
   b. Submit worker jobs that each own a disjoint chunk of elements.
      Each worker:
          - reads HtmlNode data (read-only, safe)
          - matches selectors
          - writes computed-style hash table for its elements
   c. Wait for all workers.

4. Publish phase (serial, mutator thread)
   For each newly created element:
       atomic_store(publish_state, PUBLISH_BLACK)

5. Scripts run.  Readers now see a fully consistent tree and styles.
```

### Why this is safe

- No two workers write the same element's computed-style table (partition by
  element index), so intra-object races disappear.
- Workers only read shared read-only data: `HtmlDocument`, `CssSheetList`, and
  the parsed HTML tree.
- The hash tables use lock-free insert, so multiple workers can insert elements
  into the same class bucket without blocking.
- Readers and the GC ignore grey objects, so a half-initialized element is never
  observed.

### Inline styles

Inline `style="..."` attributes still win over stylesheet declarations.  The
**parsing** of inline styles can be moved into the parallel workers: each
worker reads the raw `style` attribute string of its assigned elements and
stores the resulting declarations in the per-element computed-style table.

However, the **application** of those declarations to the JS `element.style`
object must remain in the serial publish phase because QuickJS is not
thread-safe for object mutation.  Even when workers touch different elements,
they still share runtime state such as the shape hash table, atom cache, and
property-array allocator.  Mutating JS objects from multiple threads would
corrupt that shared state.

So the flow is:

1. Parallel workers parse `style="..."` and stylesheet matches into the same
   per-element lock-free declaration table, tagging inline declarations with a
   higher specificity/source flag.
2. Serial publish phase writes the merged result to `element.style` and any
   other JS-visible objects, then sets `PUBLISH_BLACK`.

---

## 4. Thread-Safety Rules for Game-Dev C

1. **No plain `GCHandle` writes across threads.**  Use
   `atomic_store_u32`/`atomic_load_u32` for every bucket head and `next` link.

2. **No writes to object payload while state is `BLACK` from a worker.**  Once
   published, only the mutator thread may mutate JS-visible objects.

3. **Workers never call into QuickJS mutator APIs.**  They only allocate
   `JS_GC_OBJ_TYPE_DATA` objects and write `GCHandle`/`uint32_t` fields.

4. **Use `memory_order_acq_rel` semantics implicitly.**  The existing
   `_ReadWriteBarrier` / `__sync_synchronize` helpers are strong enough for the
   publication pattern: full barrier after writing payload, full barrier before
   `atomic_store_u32(state, BLACK)`.

5. **Hash tables are rebuilt, not resized in place.**  This avoids the
   complexity of lock-free table migration.

---

## 5. API Sketch

```c
/* Object publication */
void     publish_state_init(void);
void     publish_set_grey(GCHandle obj);
void     publish_set_black(GCHandle obj);
uint32_t publish_state_get(GCHandle obj);

/* Lock-free hash table */
CssHashTable *css_hash_create(uint32_t bucket_count);
void          css_hash_destroy(CssHashTable *t);
void          css_hash_insert(CssHashTable *t, uint32_t hash,
                              GCHandle key, GCHandle value);
GCHandle      css_hash_lookup(CssHashTable *t, uint32_t hash, GCHandle key);

/* One-to-many class/tag index */
void     css_class_index_insert(CssHashTable *t, uint32_t hash,
                                GCHandle class_key, GCHandle elem);
GCHandle css_class_index_lookup(CssHashTable *t, uint32_t hash,
                                GCHandle class_key);

/* Per-element computed style */
GCHandle css_computed_create(void);
void     css_computed_set(GCHandle computed, JSAtom prop,
                          GCHandle value_handle);
GCHandle css_computed_get(GCHandle computed, JSAtom prop);
```

---

## 6. Open Questions / Risks

1. **DOMNode GC marking.**  We must add a `gc_mark` callback for the DOM node
   class so parent/child/sibling `GCValue` links and the computed-style handle
   keep the referenced objects alive.

2. **Quiescence for table rebuild.**  If we rebuild a class index, we need a
   safe point where no reader is walking the old table.  Documents are usually
   short-lived, so simply not rebuilding may be acceptable.

3. **Worker memory ordering on non-x86.**  The current atomic helpers use
   compiler barriers, which are sufficient for x86/ARM release/acquire patterns
   but should be reviewed if the project is ported to weaker architectures.

4. **Dynamic DOM mutations.**  This design covers the initial parse+style pass.
   Runtime mutations (`appendChild`, `className = ...`) still need to be
   serialized on the mutator thread or batched through the same grey-state
   mechanism.

---

## Summary

- Use an atomic **publication state** (`UNBORN`/`GREY`/`BLACK`) to hide
  partially-constructed elements from readers and the GC.
- Use a **lock-free chained hash table** built from `GCHandle` bucket heads and
  entry `next` links, inserted with CAS.
- Keep the existing worker-pool model: parallel selector matching + parallel
  per-element computed-style writes, then a serial publish sweep.
- Stay in the project's game-engine C style: hand-rolled atomics, GC handles,
  fixed/double-buffered structures, no `std::atomic`.

---

## 7. Porting Plan: Thread-Safe Shared Runtime State

To allow parallel CSS **object mutation** (not just selector matching) we must
audit and thread-safe every piece of shared runtime state that is touched while
objects are being created or modified.  The audit below covers the QuickJS
runtime in `browser-emulator/third_party/quickjs`.

### 7.1 Audit summary

Only the GC's handle/memory reservation and the grey work queue are already
lock-free.  Nearly all higher-level shared state is unprotected:

| Category | Shared state | Current protection | Thread-safe replacement |
|----------|--------------|--------------------|-------------------------|
| **Atoms** | `atom_hash_*`, `atom_array_handle`, `atom_free_index`, `atom_gc_marks_handle`, per-context `atom_cache` | None | Lock-free open-addressing atom hash table; per-context atom cache stays per-context but insertions must use CAS. |
| **Shapes** | `shape_hash_*`, `shape_hash_handle`, `JSShape` hash chains | None | Lock-free shape hash table (open addressing or chained handles); shape transition cache per context. |
| **Property arrays** | `JSObject.prop_handle` reallocations, `JSProperty[]` contents | Underlying bump allocator is atomic; no higher-level protection | Per-object property-array version + atomic slot CAS; pre-sized property arrays for DOM nodes. |
| **Classes** | `class_count`, `class_array_handle`, `ctx_class_proto[]` | None | Immutable class array after init; RCU-style swap for rare additions; class prototypes stored as handles with atomic loads. |
| **Exceptions** | `current_exception`, `current_exception_is_uncatchable` | None | Thread-local exception slot per worker; main thread merges/owns runtime exception. |
| **Job queue** | `job_queue` ring buffer | None | Lock-free MPMC ring buffer with atomic head/tail. |
| **GC state** | `free_list`, `free_count`, `type_buckets`, `active_handle_table` swap, `active_buffer_index`, `bytes_allocated` | Partial / plain stores | Lock-free free list (atomic stack); atomic table swap; type buckets moved to per-thread or lock-free append-only arrays. |
| **Global class ID** | `js_class_id_alloc` | Mutex only under `CONFIG_ATOMICS` | Atomic fetch-add. |

### 7.2 Design rules for the port

1. **Handles, not pointers.**  Every table entry is a `GCHandle` so GC
   compaction never invalidates a concurrent reader.
2. **Open addressing for maps.**  Atom, shape, and computed-style maps become
   flat arrays of buckets with atomic CAS insert and linear-probe lookup.
3. **Per-object versioning for property arrays.**  A property store first
   checks an atomic version counter; if the array is being resized the writer
   retries with the new array handle.  This avoids a global lock on property
   arrays.
4. **No QuickJS JS-object mutation from workers.**  Workers only touch
   `JS_GC_OBJ_TYPE_DATA` backing structs and atomic tables.  The mutator thread
   performs the final JS-object writes during publication.
5. **Grey state gates publication.**  Any object whose backing data or
   computed-style table is still being written stays `PUBLISH_GREY`.  Tables
   skip grey entries; the GC only traces through `PUBLISH_BLACK` objects.

### 7.3 Implementation order

| # | Task | Status |
|---|------|--------|
| 1 | **Object publication state** — per-handle `PUBLISH_UNBORN`/`GREY`/`BLACK` array, `gc_alloc_grey()`, `gc_publish()` | **Done** (`quickjs_gc_unified.h/cpp`, unit tests passing) |
| 2 | **Reusable lock-free hash table** — open addressing, CAS insert, resize | **Done** (`lockfree_hash_table.h/cpp`, single + multi-threaded tests passing) |
| 3 | **Plumb grey state through DOM/JS creation** — `DOMNodeHandle::create`, `JS_NewObject` now allocate grey and publish black before returning; GC mark callback deferred | **Done** (tests verify published state of JS objects and DOM nodes) |
| 4 | **Property-array allocator** — per-object versioning, atomic slot CAS | Not started |
| 5 | **Shape hash table** — lock-free open-addressing handle table | **Done** (`LFHashTable` integrated into QuickJS; immutable hashed shapes; CAS resize; retired-table reclamation; sharing + resize + GC cleanup tests passing) |
| 6 | **Atom cache / atom hash** — lock-free atom hash table + atomic class ID | Not started |
| 7 | **Class array / prototypes** — freeze after init; atomic handle loads | Not started |
| 8 | **Job queue** — lock-free ring buffer | Not started |
| 9 | **GC free list / type buckets** — **Done** (`Treiber stack` free list + lock-free append-only type buckets; atomic pointer CAS added) |
| 10 | **Move CSS inline-style parsing and stylesheet matching into workers**, keep JS-object writes serial | Not started |

Phases 1, 2, 3, 5 and 9 are complete.  These provide the foundational building
blocks (publication state, lock-free containers, thread-safe shape cache,
thread-safe allocation metadata) for the remaining QuickJS core changes.

### 7.4 Grey-state integration points

```
Element construction:
  allocate JS object + DOMNode backing
  publish_state = PUBLISH_GREY
  initialize tree links, attributes, style object
  insert into id/class/tag tables (readers skip grey entries)

Parallel CSS pass:
  worker owns a disjoint element chunk
  worker parses inline style and matches selectors
  worker writes computed-style declarations into per-element lock-free table
  (element stays grey; no JS-object mutation)

Serial publication:
  for each new element:
      write merged computed/inline declarations to element.style
      publish_state = PUBLISH_BLACK
```

### 7.5 Risks

- **Shape transitions** are the hardest part: adding a property to one object
  can create a new shape that other objects then transition to.  A lock-free
  shape cache with CAS insertion is required.
- **Property-array resize** must copy existing slots and update the object's
  `prop_handle` atomically.  Concurrent readers may see the old or new array;
  both are valid because grey objects are not read by other threads.
- **GC compaction** still stops the world.  Until we make marking fully
  concurrent with mutators, workers must not allocate while a GC cycle is in
  progress.  The simplest guard is a global "GC active" flag that workers spin
  on before allocating.

---

## 8. Summary

- Use an atomic **publication state** (`UNBORN`/`GREY`/`BLACK`) to hide
  partially-constructed elements from readers and the GC.
- Use **lock-free open-addressing hash tables** built from atomic `GCHandle`
  bucket CAS for atoms, shapes, and computed styles.
- Use **per-object property-array versioning** so property writes don't need a
  global lock.
- Port the remaining shared runtime state (atom/shape/class/job/GC free lists)
  to lock-free or RCU-style structures.
- Keep actual JS-object mutation on the mutator thread; workers only write
  `JS_GC_OBJ_TYPE_DATA` backing data while objects are grey.
- Stay in the project's game-engine C style: hand-rolled atomics, GC handles,
  fixed/double-buffered structures, no `std::atomic`.
