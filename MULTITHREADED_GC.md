# Multithreaded Garbage Collector Design

## Core Principle

One layer of indirection, two handle tables. The active handle table pointer is atomically swapped after compaction. Both tables contain correct addresses for all live objects — pre-swap and post-swap.

## Why Two Tables?

During compaction the GC thread copies live objects to a new buffer. The main thread keeps allocating. New allocations must be referenceable **both before and after the swap**. Solution: write new handles to **both** tables during compaction. After the swap, the new table (now active) already has every correct pointer.

### New allocations always go to the post-compaction buffer

Once marking completes, the GC knows exactly how many objects are garbage (unmarked). The main thread uses this information to **always allocate into the post-compaction buffer** — the same buffer the GC thread is compacting into. This means:

- New objects are born in their final location. No copying needed later.
- The post-compaction buffer receives both (a) compacted live objects from the old buffer and (b) brand-new allocations from the main thread.
- The GC thread and main thread share the same bump pointer region, serialized by `alloc_mutex`.
- After the swap, the new table is active and already contains correct pointers for both compacted objects and new allocations.

## Handle Resolution

```cpp
void *gc_deref(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return NULL;
    void **active = atomic_load_explicit(&g_gc.active_handle_table, memory_order_acquire);
    return (handle < active_count(active)) ? active[handle] : NULL;
}
```

One atomic load + one array index. No encoding, no forwarding map.

## Data Structures

```cpp
#define GC_BUFFER_COUNT 2

typedef struct GCBuffer {
    void       **handles;        // Handle table: direct object pointers
    uint32_t    handle_count;
    uint32_t    handle_capacity;
    uint32_t   *free_list;
    uint32_t    free_count;
    uint32_t    free_capacity;

    uint8_t    *storage;         // Object storage
    size_t      storage_size;
    size_t      storage_capacity;
} GCBuffer;

typedef struct GCState {
    GCBuffer    buffers[GC_BUFFER_COUNT];

    _Atomic(void**) active_handle_table;  // Points to one buffer's handles[]
    _Atomic(uint32_t) compaction_target;    // Which buffer GC is writing to

    _Atomic(uint32_t) gc_phase;   // IDLE, MARKING, COMPACTING, READY
    _Atomic(bool)     gc_requested;

    PlatformMutex  *alloc_mutex;  // Serializes table writes + bump pointer
    PlatformMutex  *gc_mutex;     // Root set

    GCHandle       *gc_roots;
    uint32_t        gc_root_count;
    uint32_t        gc_root_capacity;
} GCState;
```

## Allocation

### Normal Operation

```cpp
GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum type) {
    void **active = atomic_load_explicit(&g_gc.active_handle_table, memory_order_acquire);
    GCBuffer *buf = buffer_from_table(active);

    platform_mutex_lock(g_gc.alloc_mutex);

    uint8_t *ptr = bump_allocate(buf, size);
    init_header(ptr, type, size);
    GCHandle handle = alloc_handle(active, ptr);

    platform_mutex_unlock(g_gc.alloc_mutex);
    return handle;
}
```

### During Compaction

Once marking is complete, all new allocations go into the **post-compaction buffer** (the compaction target). The handle is written to **both** tables: the active (pre-swap) table so the object is immediately referenceable, and the new (post-swap) table so it remains valid after the swap.

```cpp
GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum type) {
    void **active = atomic_load_explicit(&g_gc.active_handle_table, memory_order_acquire);
    uint32_t target = atomic_load_explicit(&g_gc.compaction_target, memory_order_acquire);
    GCBuffer *new_buf = &g_gc.buffers[target];
    void **new_table = new_buf->handles;

    platform_mutex_lock(g_gc.alloc_mutex);

    uint8_t *ptr = bump_allocate(new_buf, size);
    init_header(ptr, type, size);
    GCHandle handle = alloc_handle(active, ptr);   // Written to active table: immediately visible
    new_table[handle] = ptr;                          // Written to post-swap table: valid after swap

    platform_mutex_unlock(g_gc.alloc_mutex);
    return handle;
}
```

New objects are born in the post-compaction buffer and are referenceable **immediately** through the active table.

## GC Cycle

### 1. Mark

```cpp
static void gc_mark_phase(void) {
    atomic_store_explicit(&g_gc.gc_phase, GC_PHASE_MARKING, memory_order_release);
    gc_snapshot_roots();

    // Reset marks in both buffers
    for (int b = 0; b < GC_BUFFER_COUNT; b++) {
        for (uint32_t i = 0; i < g_gc.buffers[b].handle_count; i++) {
            void *ptr = g_gc.buffers[b].handles[i];
            if (ptr) ((GCHeader*)ptr)->mark = 0;
        }
    }

    for (uint32_t i = 0; i < g_gc.gc_root_count; i++) {
        gc_mark_object(g_gc.gc_roots[i]);
    }
}
```

### 2. Compact

```cpp
static void gc_compact_phase(void) {
    atomic_store_explicit(&g_gc.gc_phase, GC_PHASE_COMPACTING, memory_order_release);

    void **active = atomic_load_explicit(&g_gc.active_handle_table, memory_order_acquire);
    GCBuffer *old_buf = buffer_from_table(active);
    GCBuffer *new_buf = other_buffer(old_buf);
    void **new_table = new_buf->handles;

    uint32_t new_idx = (new_buf == &g_gc.buffers[0]) ? 0 : 1;
    atomic_store_explicit(&g_gc.compaction_target, new_idx, memory_order_release);

    new_buf->storage_size = 0;
    new_buf->handle_count = 1;  // 0 reserved
    new_buf->free_count = 0;

    platform_mutex_lock(g_gc.alloc_mutex);

    for (uint32_t handle = 1; handle < old_buf->handle_count; handle++) {
        void *old_ptr = active[handle];
        if (!old_ptr) continue;

        GCHeader *old_hdr = (GCHeader *)old_ptr;
        if (!old_hdr->mark) {
            if (old_hdr->finalizer) old_hdr->finalizer(old_ptr);
            active[handle] = NULL;  // Clear from active table
            continue;
        }

        // Copy to post-compaction buffer
        uint8_t *new_ptr = bump_allocate(new_buf, old_hdr->size);
        memcpy(new_ptr, old_ptr, aligned_size(old_hdr->size));

        // Update BOTH tables
        active[handle] = new_ptr;      // Pre-swap: main thread sees new location immediately
        new_table[handle] = new_ptr;   // Post-swap: new table already correct

        GCHeader *new_hdr = (GCHeader *)new_ptr;
        new_hdr->handle = handle;
        new_hdr->mark = 0;
    }

    platform_mutex_unlock(g_gc.alloc_mutex);
}
```

### 3. Swap

```cpp
static void gc_swap_phase(void) {
    void **active = atomic_load_explicit(&g_gc.active_handle_table, memory_order_acquire);
    GCBuffer *old_buf = buffer_from_table(active);
    GCBuffer *new_buf = other_buffer(old_buf);

    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&g_gc.active_handle_table, new_buf->handles, memory_order_release);

    // Reset old buffer for next cycle
    old_buf->storage_size = 0;
    old_buf->handle_count = 1;
    old_buf->free_count = 0;

    atomic_store_explicit(&g_gc.gc_phase, GC_PHASE_IDLE, memory_order_release);
}
```

The swap is a single pointer flip. Both tables already have correct pointers.

## Why It Works

| Phase | Active Table | New Table | Main Thread Allocates Into |
|-------|-------------|-----------|---------------------------|
| Normal | All entries → buffer 0 | — | Buffer 0 |
| Marking | All entries → buffer 0 | — | Buffer 0 |
| Compacting | Compacted entries → buffer 1, old entries → buffer 0 | All entries → buffer 1 | **Post-compaction buffer** (writes to both tables) |
| Post-swap | All entries → buffer 1 | (now active) | Buffer 1 |

- **GC thread** copies live objects into the **post-compaction buffer** and updates the **active table in-place** so pre-swap dereferences see new locations immediately.
- **Main thread** allocates into the **post-compaction buffer** (same buffer as compaction) and writes handles to **both tables** — active table for immediate visibility, new table for post-swap validity.
- **After swap** the new table is active and already has every correct pointer. No remapping needed.

## Edge Cases

- **Handle table growth**: `alloc_mutex` protects both tables. Both tables grow together.
- **Dead objects**: Active table entry set to NULL. New table never gets an entry.
- **Finalizers**: Run on GC thread. Must not allocate GC objects.
- **GC slower than frame**: Skip request, continue. GC runs to completion.
- **Emergency sync GC**: If buffer full and background GC not ready, stop-the-world fallback.

## Performance

| Metric | Stop-the-World | This Design |
|--------|---------------|-------------|
| Main thread pause | 10-100ms | ~0 (pointer flip) |
| Memory overhead | 1x heap | 2x heap |
| Dereference | None | Atomic load + array index |
| Allocation | Bump | Bump + mutex during compaction |
| Worst case | Full pause | Sync fallback |
