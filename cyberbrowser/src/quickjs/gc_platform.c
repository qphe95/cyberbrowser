/*
 * OS layer for the garbage collector.  See gc_platform.h.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "gc_platform.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#endif

/* ------------------------------------------------------------------------ */
/* Address space                                                            */
/* ------------------------------------------------------------------------ */

#ifdef _WIN32

void *gc_os_reserve(size_t size, bool write_watch) {
    DWORD type = MEM_RESERVE | (write_watch ? MEM_WRITE_WATCH : 0);
    return VirtualAlloc(NULL, size, type, PAGE_NOACCESS);
}

void gc_os_release(void *base, size_t size) {
    (void)size;
    if (base) VirtualFree(base, 0, MEM_RELEASE);
}

bool gc_os_commit(void *addr, size_t size) {
    return VirtualAlloc(addr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

void gc_os_decommit(void *addr, size_t size) {
    VirtualFree(addr, size, MEM_DECOMMIT);
}

bool gc_os_has_write_watch(void) { return true; }

bool gc_os_write_watch_pull(void *addr, size_t size, GCDirtyPageFn fn, void *opaque) {
    /* Query in chunks whose page count equals the buffer size, so one call
     * always reports every dirty page of its chunk.  (With the reset flag a
     * call that ran out of buffer space would also reset pages it did not
     * report.) */
    enum { BATCH = 4096 };
    static _Thread_local void *pages[BATCH];
    uint8_t *cur = (uint8_t *)addr;
    uint8_t *end = cur + size;
    while (cur < end) {
        size_t len = (size_t)(end - cur);
        if (len > (size_t)BATCH * GC_OS_PAGE_SIZE) len = (size_t)BATCH * GC_OS_PAGE_SIZE;
        ULONG_PTR count = BATCH;
        DWORD granularity = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, cur, len, pages, &count, &granularity) != 0)
            return false;
        for (ULONG_PTR i = 0; i < count; i++)
            fn(pages[i], opaque);
        cur += len;
    }
    return true;
}

void gc_os_write_watch_reset(void *addr, size_t size) {
    ResetWriteWatch(addr, size);
}

#else /* POSIX */

void *gc_os_reserve(size_t size, bool write_watch) {
    (void)write_watch;
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void gc_os_release(void *base, size_t size) {
    if (base) munmap(base, size);
}

bool gc_os_commit(void *addr, size_t size) {
    return mprotect(addr, size, PROT_READ | PROT_WRITE) == 0;
}

void gc_os_decommit(void *addr, size_t size) {
    /* Drop the pages and make the range inaccessible again. */
    mmap(addr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

bool gc_os_has_write_watch(void) { return false; }

bool gc_os_write_watch_pull(void *addr, size_t size, GCDirtyPageFn fn, void *opaque) {
    (void)addr; (void)size; (void)fn; (void)opaque;
    return false;
}

void gc_os_write_watch_reset(void *addr, size_t size) {
    (void)addr; (void)size;
}

#endif

/* ------------------------------------------------------------------------ */
/* Address-based sleeping                                                   */
/* ------------------------------------------------------------------------ */

#if defined(_WIN32)

void gc_os_wait(volatile uint32_t *addr, uint32_t expected, int timeout_ms) {
    WaitOnAddress((volatile VOID *)addr, &expected, sizeof(expected),
                  timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms);
}

void gc_os_wake_all(volatile uint32_t *addr) {
    WakeByAddressAll((PVOID)addr);
}

#elif defined(__linux__)

void gc_os_wait(volatile uint32_t *addr, uint32_t expected, int timeout_ms) {
    struct timespec ts, *pts = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }
    syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAIT_PRIVATE, expected, pts, NULL, 0);
}

void gc_os_wake_all(volatile uint32_t *addr) {
    syscall(SYS_futex, (uint32_t *)addr, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL, NULL, 0);
}

#else /* macOS and others: no public futex; poll with short sleeps */

void gc_os_wait(volatile uint32_t *addr, uint32_t expected, int timeout_ms) {
    int waited = 0;
    while (*addr == expected && (timeout_ms < 0 || waited < timeout_ms)) {
        struct timespec ts = { 0, 1000000L };  /* 1 ms */
        nanosleep(&ts, NULL);
        waited++;
    }
}

void gc_os_wake_all(volatile uint32_t *addr) {
    (void)addr;
}

#endif

/* ------------------------------------------------------------------------ */
/* Threads, stacks, time                                                    */
/* ------------------------------------------------------------------------ */

typedef struct GCThreadStart {
    GCThreadFn fn;
    void *arg;
} GCThreadStart;

#ifdef _WIN32

static DWORD WINAPI gc_os_thread_entry(LPVOID p) {
    GCThreadStart s = *(GCThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

bool gc_os_thread_start(GCThread *t, GCThreadFn fn, void *arg) {
    GCThreadStart *s = (GCThreadStart *)malloc(sizeof(*s));
    if (!s) return false;
    s->fn = fn;
    s->arg = arg;
    HANDLE h = CreateThread(NULL, 0, gc_os_thread_entry, s, 0, NULL);
    if (!h) { free(s); return false; }
    t->native = h;
    return true;
}

void gc_os_thread_join(GCThread *t) {
    if (!t->native) return;
    WaitForSingleObject((HANDLE)t->native, INFINITE);
    CloseHandle((HANDLE)t->native);
    t->native = NULL;
}

void gc_os_yield(void) { SwitchToThread(); }

uint64_t gc_os_thread_id(void) { return (uint64_t)GetCurrentThreadId(); }

uintptr_t gc_os_stack_base(void) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    return (uintptr_t)tib->StackBase;
}

uint64_t gc_os_now_ns(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
}

uint64_t gc_os_thread_cpu_ns(void) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    return ((((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime) +
            (((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 100;
}

#else

static void *gc_os_thread_entry(void *p) {
    GCThreadStart s = *(GCThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

bool gc_os_thread_start(GCThread *t, GCThreadFn fn, void *arg) {
    GCThreadStart *s = (GCThreadStart *)malloc(sizeof(*s));
    if (!s) return false;
    s->fn = fn;
    s->arg = arg;
    pthread_t *th = (pthread_t *)malloc(sizeof(pthread_t));
    if (!th) { free(s); return false; }
    if (pthread_create(th, NULL, gc_os_thread_entry, s) != 0) {
        free(s);
        free(th);
        return false;
    }
    t->native = th;
    return true;
}

void gc_os_thread_join(GCThread *t) {
    if (!t->native) return;
    pthread_join(*(pthread_t *)t->native, NULL);
    free(t->native);
    t->native = NULL;
}

void gc_os_yield(void) { sched_yield(); }

uint64_t gc_os_thread_id(void) { return (uint64_t)(uintptr_t)pthread_self(); }

uintptr_t gc_os_stack_base(void) {
#if defined(__APPLE__)
    return (uintptr_t)pthread_get_stackaddr_np(pthread_self());
#else
    static _Thread_local uintptr_t cached;
    if (!cached) {
        pthread_attr_t attr;
        void *addr = NULL;
        size_t size = 0;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            if (pthread_attr_getstack(&attr, &addr, &size) == 0)
                cached = (uintptr_t)addr + size;
            pthread_attr_destroy(&attr);
        }
    }
    return cached;
#endif
}

uint64_t gc_os_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t gc_os_thread_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif
