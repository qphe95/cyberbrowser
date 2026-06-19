# Grey Deletion Design

## Problem

The runtime keeps objects in many auxiliary data structures besides the main GC handle table:

- atom hash tables and atom arrays
- shape hash tables and shape lists
- property arrays and object shapes
- WeakMap / WeakSet / WeakRef registries
- finalization registries
- module import/export tables
- job queue entries
- debugger maps and source tables

When the GC identifies an object as unreachable, any of these tables may still hold a handle to it. If the object is reclaimed while those references still exist, later reads can dereference freed memory. We need a window between "identified as garbage" and "memory reclaimed" during which dependent tables can drop their references safely.

## Solution: accessor-based lazy deletion

Every accessor that reads from or writes to one of these auxiliary tables checks whether the involved object is in a pending-deletion state. If it is, the accessor removes the entry and returns a miss. The runtime therefore never observes a table that exposes a dead object.

## Deletion-grey state

Add a new color distinct from mark-grey and publish-grey:

```c
typedef enum {
    GC_COLOR_WHITE = 0,   /* not yet reached during marking */
    GC_COLOR_GREY = 1,    /* reachable, children pending scan */
    GC_COLOR_BLACK = 2,   /* reachable and fully scanned */
    GC_COLOR_DEAD = 3,    /* unreachable; pending cleanup and reclamation */
} GCColor;
```

Lifecycle:

```
allocate -> WHITE or BLACK (publish)
mark     -> GREY -> BLACK
sweep    -> WHITE -> DEAD -> freed
```

A `DEAD` object remains memory-valid and keeps its handle until the final reclamation pass runs. Accessors can safely dereference it to inspect type or key information while deciding whether to drop the reference.

## Grey-aware accessors

### Read accessors

Every accessor that returns a GC handle from an auxiliary table follows this pattern:

```c
GCHandle atom_hash_find(JSRuntimeHandle rt, JSAtom atom)
{
    GCHandle h = lockfree_hash_lookup(rt->atom_hash, atom);
    if (h == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    if (gc_color_state(h) == GC_COLOR_DEAD) {
        lockfree_hash_remove(rt->atom_hash, atom);
        return GC_HANDLE_NULL;
    }
    return h;
}
```

Apply the same pattern to:

- atom hash tables and atom arrays
- shape hash tables and shape lists
- property arrays and object shapes
- WeakMap / WeakSet / WeakRef registries
- finalization registries
- module import/export tables
- job queue entries
- debugger maps and source tables

### Iterators

Iterators skip dead entries and remove them when supported:

```c
BOOL shape_iter_next(ShapeIter *it, GCHandle *out_shape)
{
    for (;;) {
        if (!it->advance(it)) return FALSE;
        GCHandle h = it->current;
        if (gc_color_state(h) == GC_COLOR_DEAD) {
            shape_table_remove(it, h);
            continue;
        }
        *out_shape = h;
        return TRUE;
    }
}
```

### Write accessors

Writes reject dead handles so new references to dying objects cannot be created:

```c
void weakmap_set(WeakMapHandle map, GCHandle key, GCHandle value)
{
    if (key != GC_HANDLE_NULL && gc_color_state(key) == GC_COLOR_DEAD) return;
    if (value != GC_HANDLE_NULL && gc_color_state(value) == GC_COLOR_DEAD) return;
    /* ... normal insert or update ... */
}
```

## Sweep and reclamation

The sweep phase has two steps:

1. **Mark dead.** Iterate the handle table and atomically transition every white object to `DEAD`. Add its handle to `dead_list`.
2. **Reclaim.** At a later safe point, iterate `dead_list`:
   - Verify the object is still `DEAD` (a barrier may have rescued it).
   - Run the finalizer if present.
   - Clear the handle table entry and return the handle to the free list.

No global table scan is required before reclamation. Accessors guarantee that no live table returns a dead reference by the time the reclamation pass runs.

## Safety

### No inconsistent state

By construction, every accessor either returns a live object or returns a miss. A dead object may still be memory-valid, but it is never exposed through an accessor, so normal code cannot reach it. There is no observable moment where one table still references a dead object while another does not.

### Memory validity

`DEAD` objects are not reclaimed until the reclamation pass. Accessors can dereference them to read headers, types, or weak-map keys while removing entries. No subsystem sees partially freed memory.

### Resurrection

In a concurrent design, the write barrier must rescue a dead object if a mutator stores it into a live object:

```c
void gc_write_barrier(GCHandle source, GCHandle target)
{
    if (target != GC_HANDLE_NULL && gc_color_state(target) == GC_COLOR_DEAD) {
        gc_shade(target);
        gc_remove_from_dead_list(target);
    }
    /* ... existing barrier logic ... */
}
```

Under stop-the-world marking this cannot happen because mutators are paused while objects transition to `DEAD`.

### Finalizers

Finalizers run during the reclamation pass, after accessors have had a chance to drop references. A finalizer therefore cannot observe the object through WeakMap, WeakSet, the job queue, or any other auxiliary table.

## Implementation status

Implemented in `browser-emulator/third_party/quickjs/quickjs_gc_unified.cpp`,
`quickjs_gc_unified.h`, and `quickjs.cpp`.

- `GC_COLOR_DEAD` added to the color enum.
- `gc_color_state()` and `gc_object_is_dead()` helpers added.
- `gc_sweep_unified()` transitions white objects to `DEAD` and pushes handles onto
  `g_gc.dead_list`.
- `gc_compact_move_objects()` skips `DEAD` and `WHITE` objects instead of copying
  them.
- `gc_reclaim_dead()` finalizes and reclaims handles from `dead_list` before
  compaction runs.
- `gc_run_internal()` reordered so weak-reference cleanup and atom sweeping happen
  while objects are `DEAD`, and reclamation happens before compaction.
- Updated accessors:
  - `js_weakref_is_live()` now treats `GC_COLOR_DEAD` targets as not live, so
    WeakMap / WeakSet / WeakRef / FinalizationRegistry accessors automatically
    filter dead entries.
  - `find_hashed_shape_proto()` and `find_hashed_shape_prop()` return NULL for
    dead shapes.
  - Job queue enqueue/dequeue and the C accessors reject/skip dead job entries.
- Atoms continue to use their existing `JS_ATOM_TYPE_DEAD` mechanism, which
  cooperates with the new color state: atoms are marked `DEAD` by sweep and then
  finalized/freed by `gc_sweep_atoms()`; `gc_reclaim_dead()` skips any handle
  that has already been cleared.

Property arrays, module tables, and debugger maps are not updated with explicit
`DEAD` checks because their entries are owned by live GC objects; a dead object
cannot be reached through those tables in the stop-the-world implementation. If
concurrent mutators are introduced later, per-property value checks can be added
using the same helper pattern.

## Implementation sketch

1. Add `GC_COLOR_DEAD` to `GCColor`.
2. Add `gc_color_state(GCHandle)` and `gc_object_is_dead(GCHandle)` helpers.
3. In sweep/compaction, transition white objects to `DEAD` and push handles onto
   `dead_list`.
4. Audit and update accessors in every subsystem that holds GC handles:
   - atom hash / atom array
   - shape hash / shape list
   - property arrays / object shape lookups
   - WeakMap / WeakSet / WeakRef registries
   - finalization registries
   - module import/export tables
   - job queue entries
   - debugger maps / source tables
5. Update iterators to skip and remove dead entries.
6. Update write accessors to reject dead handles.
7. Run finalizers and reclaim `dead_list` handles at a safe point after accessors
   have purged references.

## Advantages

- **No inconsistent state.** The runtime never sees a dead object returned from a table.
- **No global cleanup pass.** Tables clean themselves on demand through accessors.
- **Local reasoning.** Each subsystem only needs to make its own accessors grey-aware.
- **Safe finalization.** Finalizers run after public references have been hidden.
- **Symmetric with allocation.** Just as publish-grey hides objects under construction, deletion-grey hides objects being torn down.
