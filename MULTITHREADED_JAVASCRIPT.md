# Multithreaded JavaScript Environment

This project runs JavaScript on a multithreaded engine where the main thread and helper threads share a single runtime without stopping the world for every GC cycle.

## Core ideas

- **Handle-based object model.** All GC objects are referenced through `GCHandle` indices, not raw pointers. Dereferencing is one atomic load of the active handle table plus an array lookup, so objects can move in memory without invalidating references.
- **Two-handle-table GC.** The GC uses two handle tables and two storage buffers. During compaction the GC thread copies live objects into the second buffer while the main thread keeps allocating. New allocations are written to both tables so they are valid before and after the table swap.
- **Atomic swap.** Once compaction finishes, the active handle table pointer is atomically swapped. The new table already contains correct pointers for every live object.
- **Lock-free job queue.** Pending JS jobs live in a chain of large flat-array blocks. Producers append to the tail block; consumers drain from the head block. When a block fills up a new block is linked on, and drained blocks are freed. The queue uses only CAS and reference counting, with no global resize lock.

## Threading model

- The main JavaScript thread executes JS, allocates objects, and enqueues jobs.
- A separate GC thread performs marking and compaction concurrently with the main thread.
- Multiple threads can safely enqueue and dequeue JS jobs through the lock-free queue.
- Allocation and handle-table writes are serialized by a single `alloc_mutex`; handle reads use acquire-loads on the active table pointer.

## Why this works

- Handles decouple object identity from memory address, so the GC can move objects without updating every pointer in the heap.
- Two tables let new allocations happen in their final location during compaction, avoiding the need to copy them again.
- The atomic table swap makes compaction visible to the main thread in a single instruction.
- The job queue never blocks producers, so background threads can keep feeding work to the runtime while the GC is running.
