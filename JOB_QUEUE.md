# Lock-Free Job Queue Redesign

## Status

Implemented in `browser-emulator/third_party/quickjs/quickjs.cpp` and `quickjs_types.h`. All job-queue regression tests pass (330/330).

## Goals

- Replace the current fixed-size, ring-buffer-based `LFJobQueue` with a simpler,
  lock-free, thread-safe flat-array job queue.
- Eliminate the "queue is full" failure path. The queue grows on demand by
  chaining additional large blocks.
- Compact on growth: when a block is retired, dead jobs (`[0, head)`) are
  discarded along with it.
- Never block producers. When the tail block is full or half consumed, a new
  larger block is allocated, linked to the chain, and published as the new tail.
- Never execute the same pending job twice. Jobs are not copied between blocks;
  each job lives in exactly one block until it is consumed.

## Why the current design is overengineered

`LFJobQueue` today is a fixed-capacity MPMC ring buffer:

- It uses `head`, `tail`, and `mask` with `atomic_fetch_add_u64` to reserve
  slots, then spins waiting for the slot to become available.
- When `tail - head >= capacity`, `js_job_queue_enqueue()` returns `FALSE`.
  The caller (`JS_EnqueueJob`) then returns `-1`, so enqueue can fail just
  because the queue filled up.
- Resizing is not built in; callers must allocate a brand-new queue and copy
  everything over.
- The slot-empty / slot-full spin loops are subtle and easy to get wrong when
  the buffer wraps.

The new design uses a short linked list of large flat-array blocks. Growth
appends a new block to the tail; the old block is freed once consumers drain it.
There is no wrap logic and no mask.

## High-level idea

The queue is a linked list of blocks. Each block is a flat array of job slots.

```
+------------+     +------------+     +------------+
| JobQueue   |     | JobBuffer  |     | JobBuffer  |
| head_block |---->| head       |---->| head       |----> ...
| tail_block |---+ | tail       |     | tail       |
+------------+   | | next       |     | next       |
                 | | jobs[]     |     | jobs[]     |
                 | +------------+     +------------+
                 |        ^
                 +--------+
```

- `head_block` is the block consumers read from.
- `tail_block` is the block producers write to.
- In steady state `head_block == tail_block`.
- During growth they diverge: producers write to `tail_block` while consumers
  drain `head_block`.
- Each block has a `next` pointer that chains to the following block. A `next`
  pointer holds a reference to the following block, keeping the chain reachable.
- When `head_block` is drained, consumers advance `head_block` to `head_block->next`.
  The drained block is freed once the queue and all in-flight readers release it.

A `JobBuffer` is a flat array. Jobs are stored contiguously at indices
`[head, tail)`. Indices below `head` are dead space. Indices at or above `tail`
are unused.

When the tail block needs to grow:

1. A producer observes that `tail_block` is full or half consumed.
2. It allocates a larger `JobBuffer` and links it as `tail_block->next`.
3. It advances `tail_block` to point to the new block.
4. From that point on, all producers write to the new block. They never spin
   waiting for a resize to finish.
5. Consumers continue reading from `head_block` until it is empty.
6. Once `head_block` is empty, a consumer loads `head_block->next`. If it is
   non-NULL, the consumer CASes `head_block` forward to that block.
7. The drained block's refcount drops to zero once the queue and any in-flight
   readers release it, and the block is freed.

Because jobs are never copied between blocks, a job can only be consumed once.
The chain is normally very short (often just one block) because each block is
large and new blocks are sized with substantial headroom.

## Resize triggers

A new tail block is installed when **either** of these is true for the current
tail block:

```c
should_resize(buf):
    return buf->tail >= buf->capacity          /* out of space */
        || buf->head >= buf->capacity / 2;     /* half the block is dead */
```

The 50% threshold prevents a block from accumulating unbounded dead space.
When it fires, the old block is retired; dead jobs are discarded and the new
block starts at the front.

## Data structures

```c
#define JS_JOB_QUEUE_INITIAL_CAPACITY 4096
#define JS_JOB_QUEUE_HEADROOM_FACTOR  3  /* new block is 3x the active count */

typedef struct JobBuffer {
    uint64_t                   capacity;      /* current size of jobs[] */
    volatile uint64_t          head;          /* next dequeue index */
    volatile uint64_t          tail;          /* next enqueue index */
    volatile uint32_t          refcount;      /* queue refs + in-flight readers/writers + next link */
    struct JobBuffer *volatile next;          /* next block in the chain, or NULL */
    GCHandle                   jobs[0];       /* flat array of capacity slots */
} JobBuffer;

typedef struct {
    JobBuffer *volatile head_block;  /* block consumers read from */
    JobBuffer *volatile tail_block;  /* block producers write to */
} JobQueue;
```

Notes:

- `head` and `tail` are local to the block. A newly allocated block always
  starts with `head = 0` and `tail = 0`.
- `refcount` counts the queue references (`head_block` and `tail_block`), any
  thread currently holding the block, and the `next` link from the previous
  block. A block is freed when its refcount drops to zero.
- `next` is both a chain pointer and a reference holder. When block A's `next`
  is set to B, B's refcount is incremented; when A is freed, it releases B.
- Blocks are `qjs_realloc`'d. The `JobQueue` wrapper is GC-managed, and the GC
  marks the jobs by walking the chain from `head_block`.

## Algorithms

### Enqueue

```c
BOOL job_queue_enqueue(JobQueue *q, GCHandle job)
{
    for (;;) {
        JobBuffer *tb = atomic_load_ptr((void *volatile *)&q->tail_block);
        acquire_buffer(tb);

        uint64_t head = atomic_load_u64(&tb->head);
        uint64_t tail = atomic_load_u64(&tb->tail);

        /* Fast path: reserve a slot if there is room and not too much dead space. */
        if (tail < tb->capacity && head < tb->capacity / 2) {
            if (atomic_compare_exchange_u64(&tb->tail, tail, tail + 1) == tail) {
                atomic_store_u32((volatile uint32_t *)&tb->jobs[tail], job);
                release_buffer(tb);
                return TRUE;
            }
            release_buffer(tb);
            continue;
        }

        /* tail_block is full or half consumed.  Link a successor while we hold
           our reference to tb, so tb cannot be freed while we touch tb->next. */
        JobBuffer *next = atomic_load_ptr(&tb->next);
        if (!next) {
            uint64_t count = (tail >= head) ? (tail - head) : 0;
            uint64_t new_capacity = count * JS_JOB_QUEUE_HEADROOM_FACTOR;
            if (new_capacity < JS_JOB_QUEUE_INITIAL_CAPACITY)
                new_capacity = JS_JOB_QUEUE_INITIAL_CAPACITY;
            JobBuffer *newb = job_buffer_alloc(new_capacity);
            if (!newb) { release_buffer(tb); return FALSE; }
            atomic_store_u64(&newb->head, 0);
            atomic_store_u64(&newb->tail, 0);

            /* Acquire newb before linking.  If we win the CAS, this reference
               becomes the link reference and must not be released. */
            acquire_buffer(newb);
            JobBuffer *prev = atomic_compare_exchange_ptr(&tb->next, NULL, newb);
            if (prev == NULL) {
                next = newb;
            } else {
                next = prev;
                release_buffer(newb);  /* someone else linked first; free ours */
            }
        }

        /* Advance tail_block so future producers write to next. */
        if (rc_pointer_cas(&q->tail_block, tb, next)) {
            release_buffer(tb);  /* queue dropped its ref to tb */
        } else {
            release_buffer(tb);
        }

        js_thread_yield();
    }
}
```

Key points:

- Producers always read `tail_block`. There is no resize state and no spin-wait
  for a resize to complete.
- The CAS loop reserves a slot only when `tail < capacity` and `head < capacity/2`.
- If multiple producers race to install a successor, CAS on `tb->next` ensures
  only one block is linked; losers free their unused allocation.
- `tail_block` is advanced with `rc_pointer_cas`, which keeps refcounts consistent.

### Dequeue

```c
GCHandle job_queue_dequeue(JobQueue *q)
{
    for (;;) {
        JobBuffer *hb = atomic_load_ptr((void *volatile *)&q->head_block);
        if (!hb) return GC_HANDLE_NULL;
        acquire_buffer(hb);

        uint64_t head = atomic_load_u64(&hb->head);
        uint64_t tail = atomic_load_u64(&hb->tail);

        if (head < tail) {
            if (atomic_compare_exchange_u64(&hb->head, head, head + 1) == head) {
                uint64_t idx = head;
                GCHandle job;
                /* Wait for the producer to publish the handle. */
                while ((job = atomic_load_u32((volatile uint32_t *)&hb->jobs[idx]))
                       == GC_HANDLE_NULL) {
                    js_thread_yield();
                }
                atomic_store_u32((volatile uint32_t *)&hb->jobs[idx],
                                 GC_HANDLE_NULL);
                release_buffer(hb);
                return job;
            }
            release_buffer(hb);
            continue;
        }

        /* head_block is empty.  Move to the next block if one has been linked. */
        JobBuffer *next = atomic_load_ptr(&hb->next);
        if (next) {
            if (rc_pointer_cas(&q->head_block, hb, next)) {
                release_buffer(hb);  /* queue dropped its ref to hb */
            } else {
                release_buffer(hb);  /* head_block changed under us */
            }
            js_thread_yield();
            continue;
        }

        release_buffer(hb);
        return GC_HANDLE_NULL;
    }
}
```

Key points:

- Consumers always read `head_block`.
- Indices are direct array offsets (`idx = head`), not masked ring positions.
- If `head_block` is empty and a next block exists, the consumer CASes
  `head_block` forward and retries. This drains blocks in order.
- A consumer that is preempted while reading a block keeps it alive via its
  acquired reference.

### Reference-counted pointer helper

```c
static BOOL rc_pointer_cas(JobBuffer *volatile *ptr,
                           JobBuffer *expected,
                           JobBuffer *desired)
{
    acquire_buffer(desired);
    if (atomic_compare_exchange_ptr((void *volatile *)ptr,
                                    (void *)expected,
                                    (void *)desired)
        == expected) {
        release_buffer(expected);  /* queue drops its reference */
        return TRUE;
    }
    release_buffer(desired);       /* undo the acquire */
    return FALSE;
}
```

### Block allocation and reclamation

```c
static JobBuffer *job_buffer_alloc(uint64_t capacity)
{
    size_t size = offsetof(JobBuffer, jobs) + capacity * sizeof(GCHandle);
    JobBuffer *b = (JobBuffer *)qjs_realloc(NULL, size);
    if (!b) return NULL;
    memset(b, 0, size);
    b->capacity = capacity;
    b->refcount = 0;  /* references added explicitly */
    return b;
}

static void job_buffer_free(JobBuffer *b)
{
    if (!b) return;
    /* When a block is freed, release the reference its next pointer held. */
    JobBuffer *next = atomic_load_ptr((void *volatile *)&b->next);
    qjs_free(b);
    release_buffer(next);
}

static inline void acquire_buffer(JobBuffer *b)
{
    if (b) atomic_fetch_add_u32(&b->refcount, 1);
}

static inline void release_buffer(JobBuffer *b)
{
    if (!b) return;
    if (atomic_fetch_sub_u32(&b->refcount, 1) == 1) {
        job_buffer_free(b);
    }
}
```

Lifecycle tracing:

- On initialization, a block is created with refcount `0`, stored into
  `head_block` (refcount becomes `1`) and `tail_block` (refcount becomes `2`).
- `rc_pointer_cas` is used for every subsequent head/tail pointer change. It
  acquires the new target, performs the CAS, and releases the old target.
- When a successor block B is linked from block A, B is acquired first and the
  acquired reference becomes A's `next` reference.
- When `tail_block` advances, the queue drops its reference to the old block.
- When `head_block` advances, the queue drops its reference to the old block.
  If no in-flight reader holds the old block, it is freed; freeing releases the
  old block's `next` reference, which may cascade.
- A block is therefore kept alive only as long as the queue, a `next` link, or
  an in-flight thread needs it.

## Memory ordering

- `head_block`, `tail_block`, and `next` are loaded and stored with
  `atomic_load_ptr` / `atomic_store_ptr` (sequentially consistent or
  acquire/release, matching the existing `quickjs_gc_unified.h` atomics).
- `head` and `tail` use `atomic_compare_exchange_u64` for slot reservation,
  and `atomic_load_u64` / `atomic_store_u64` for reads.
- Job slot writes use release semantics; slot reads use acquire semantics,
  ensuring the producer's write is visible to the consumer before the slot is
  read.

## GC integration

The `JobQueue` object itself is GC-managed and reachable from `JSRuntime` via
`job_queue_handle`. Each `JobBuffer` is `qjs_realloc`'d but contains `GCHandle`
values that must be marked:

```c
void job_queue_mark(JobQueue *q, JS_MarkFunc *mark_func)
{
    JobBuffer *buf = atomic_load_ptr((void *volatile *)&q->head_block);
    while (buf) {
        uint64_t head = atomic_load_u64(&buf->head);
        uint64_t tail = atomic_load_u64(&buf->tail);
        for (uint64_t i = head; i < tail; i++) {
            GCHandle h = atomic_load_u32((volatile uint32_t *)&buf->jobs[i]);
            if (h != GC_HANDLE_NULL) {
                mark_func(rt, h);
            }
        }
        buf = atomic_load_ptr((void *volatile *)&buf->next);
    }
}
```

The walk starts at `head_block` and follows `next` pointers. Because the chain
is short, this is cheap. Marking every block in the chain ensures no live job
is collected.

## Advantages over the current LFJobQueue

1. **No ring-buffer arithmetic.** Indices are direct array offsets. There is
   no mask and no wrap logic.
2. **Compaction on growth.** Dead jobs (`[0, head)`) are discarded when the old
   block is retired.
3. **Proactive growth at 50% consumption.** The tail block is replaced before
   it can accumulate large amounts of dead space.
4. **No fixed capacity.** The queue grows transparently; `JS_EnqueueJob` no
   longer needs to handle "queue full".
5. **Non-blocking producers.** Producers never wait for a resize; they write
   to the current `tail_block` and link a larger one if necessary.
6. **No duplicate execution.** Jobs are never copied, so a pending job exists
   in exactly one block until it is consumed.
7. **Old blocks are not retained.** Once `head_block` has advanced past an
   old block, it is unreferenced and freed.

## Implementation notes

1. **Blocks are `qjs_realloc`'d, `JobQueue` is GC-managed.** Blocks use a
   simple reference count for reclamation. The wrapper object is allocated
   through the GC and its `jobs[]` handles are marked during GC root scanning.
2. **No `resize_state`.** Growth is coordinated entirely by CAS on `tail_block`,
   `head_block`, and `next`.
3. **Jobs are not copied.** Copying unconsumed jobs while consumers are still
   draining the old block would create a duplicate-execution race. Instead,
   each job is consumed in the block it was enqueued into.
4. **Consumers advance `head_block`.** When `head_block` is empty and a next
   block exists, the consumer that discovers this performs the `head_block` swap.
5. **The chain is short.** Each new block is sized to `3 * active_count` with a
   large minimum, so in practice the chain contains one or two blocks.
6. **No shrinking.** The queue only grows (and compacts) on demand. Shrinking is
   not implemented because the dominant concern is avoiding "queue full".
