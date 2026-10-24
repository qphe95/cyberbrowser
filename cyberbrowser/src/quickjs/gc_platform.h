/*
 * OS layer for the garbage collector: address-space reservation, page
 * commit, dirty-page tracking, address-based sleeping, threads and stack
 * bounds.  No locks: everything here is either a system call or a plain
 * atomic.
 */
#ifndef GC_PLATFORM_H
#define GC_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GC_OS_PAGE_SIZE 4096u

/* Reserve address space without committing memory.  On Windows the range
 * is reserved with MEM_WRITE_WATCH when write_watch is true. */
void *gc_os_reserve(size_t size, bool write_watch);
void gc_os_release(void *base, size_t size);

/* Commit / decommit pages inside a reserved range.  Committed memory reads
 * as zero the first time it is committed. */
bool gc_os_commit(void *addr, size_t size);
void gc_os_decommit(void *addr, size_t size);

/* True when the OS tracks writes to memory reserved with write_watch
 * (Windows GetWriteWatch).  Elsewhere the collector uses software dirty
 * flags set by the engine's write helpers. */
bool gc_os_has_write_watch(void);

/* Read and reset the write-watch state of [addr, addr+size).  Calls fn for
 * every page written since the last reset.  Returns false if the OS
 * reported an error. */
typedef void (*GCDirtyPageFn)(void *page, void *opaque);
bool gc_os_write_watch_pull(void *addr, size_t size, GCDirtyPageFn fn, void *opaque);
/* Reset without reading. */
void gc_os_write_watch_reset(void *addr, size_t size);

/* Sleep while *addr == expected, for at most timeout_ms (-1 = forever).
 * Wake-ups can be spurious. */
void gc_os_wait(volatile uint32_t *addr, uint32_t expected, int timeout_ms);
void gc_os_wake_all(volatile uint32_t *addr);

typedef void (*GCThreadFn)(void *arg);
typedef struct GCThread { void *native; } GCThread;
bool gc_os_thread_start(GCThread *t, GCThreadFn fn, void *arg);
/* The same with a stack of stack_size bytes (0: the default). */
bool gc_os_thread_start_stack(GCThread *t, GCThreadFn fn, void *arg, size_t stack_size);
void gc_os_thread_join(GCThread *t);
void gc_os_yield(void);
uint64_t gc_os_thread_id(void);

/* Highest address of the calling thread's stack. */
uintptr_t gc_os_stack_base(void);

uint64_t gc_os_now_ns(void);
/* CPU time the calling thread has used. */
uint64_t gc_os_thread_cpu_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* GC_PLATFORM_H */
