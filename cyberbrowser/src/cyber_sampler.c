/* Sampling profiler (CYBER_SAMPLE=<path>): a thread suspends the main thread
 * every CYBER_SAMPLE_US microseconds (default 1000), records its instruction
 * pointer and the return addresses the x64 unwind tables give, and writes
 * one line per sample ("rva rva ..." leaf first, relative to the image
 * base) to <path> at exit.  tools/sample_report.js symbolizes them.
 * CYBER_SAMPLE_MAX_MS=<n> stops sampling and writes the file after n ms,
 * for a process that hangs and never exits.  CYBER_SAMPLE_CPU=1 keeps only
 * samples where the thread ran since the previous one (an on-CPU profile).
 * A frame in the JIT's code region is written as its offset in the region
 * with bit 62 set; the JIT writes <path>.jit, its map of that region. */
#include "cyber_sampler.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

#define SAMPLE_DEPTH 24

static HANDLE g_target;
static HANDLE g_thread;
static volatile LONG g_stop;
static uint64_t *g_samples;     /* SAMPLE_DEPTH + 1 words per sample: depth, rvas */
static size_t g_n, g_cap;
static uint64_t g_base, g_end;
static unsigned g_interval_us = 1000;
static bool g_cpu_only;          /* CYBER_SAMPLE_CPU: skip samples where the thread did not run */
static ULONG64 g_last_cycles;
static size_t g_skipped;
static char *g_path;
static unsigned g_max_ms;
/* The JIT's code region (quickjs_jit.c), 0 until it has one. */
void jit_region_bounds(uintptr_t *base, size_t *size);
extern volatile LONG g_jit_os_busy;
#define SAMPLE_JIT_FLAG ((uint64_t)1 << 62)
static volatile LONG g_written;   /* the file is written once */

static void write_samples(void) {
    if (InterlockedExchange(&g_written, 1)) return;
    FILE *f = fopen(g_path, "w");
    if (!f) return;
    fprintf(f, "# interval_us %u samples %zu%s\n", g_interval_us, g_n, g_cpu_only ? " on-cpu" : "");
    if (g_cpu_only) fprintf(stderr, "[sampler] %zu samples skipped (thread not running)\n", g_skipped);
    for (size_t i = 0; i < g_n; i++) {
        const uint64_t *rec = g_samples + i * (SAMPLE_DEPTH + 1);
        for (uint64_t d = 0; d < rec[0]; d++) fprintf(f, d ? " %llx" : "%llx", (unsigned long long)rec[1 + d]);
        fputc('\n', f);
    }
    fclose(f);
    fprintf(stderr, "[sampler] %zu samples written to %s\n", g_n, g_path);
}

/* The return addresses of a suspended thread, image-relative (0 outside the
 * image), leaf first; returns how many. */
static int walk(const CONTEXT *c0, uint64_t *out, int max) {
    CONTEXT c = *c0;
    int d = 0;
    uintptr_t jit_base = 0;
    size_t jit_size = 0;
    jit_region_bounds(&jit_base, &jit_size);
    while (d < max && c.Rip) {
        if (c.Rip >= g_base && c.Rip < g_end) out[d++] = c.Rip - g_base;
        else if (c.Rip - jit_base < jit_size) out[d++] = SAMPLE_JIT_FLAG | (c.Rip - jit_base);
        else out[d++] = 0;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image, NULL);
        if (!fn) {
            /* A leaf function (no unwind data): the return address is at
             * [rsp].  Deeper, missing data means the walk is lost. */
            if (d > 1 || !c.Rsp) break;
            c.Rip = *(DWORD64 *)c.Rsp;
            c.Rsp += 8;
            continue;
        }
        void *handler = NULL;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c, &handler, &frame, NULL);
    }
    return d;
}

static void record(const CONTEXT *c0) {
    if (g_n == g_cap) {
        size_t nc = g_cap ? g_cap * 2 : 65536;
        uint64_t *ns = (uint64_t *)realloc(g_samples, nc * (SAMPLE_DEPTH + 1) * sizeof(uint64_t));
        if (!ns) return;
        g_samples = ns;
        g_cap = nc;
    }
    uint64_t *rec = g_samples + g_n * (SAMPLE_DEPTH + 1);
    rec[0] = (uint64_t)walk(c0, rec + 1, SAMPLE_DEPTH);
    g_n++;
}

static DWORD WINAPI sampler_main(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    LARGE_INTEGER freq, start, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    start = next;
    while (!g_stop) {
        if (g_max_ms && (next.QuadPart - start.QuadPart) * 1000 / freq.QuadPart >= g_max_ms) {
            write_samples();
            break;
        }
        next.QuadPart += freq.QuadPart * g_interval_us / 1000000;
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= next.QuadPart) break;
            LONGLONG left_us = (next.QuadPart - now.QuadPart) * 1000000 / freq.QuadPart;
            Sleep(left_us > 1500 ? (DWORD)(left_us / 1000) : 0);
        }
        if (SuspendThread(g_target) == (DWORD)-1) break;
        if (g_jit_os_busy) {
            /* Unwinding now could wait forever for a lock the stopped
             * thread holds: skip this sample. */
            ResumeThread(g_target);
            continue;
        }
        CONTEXT c;
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        /* CYBER_SAMPLE_CPU=1: only samples where the thread ran since the
         * last one (its cycle count moved), so waits and descheduled time
         * do not show up as time in whatever code was interrupted. */
        ULONG64 cycles = 0;
        bool ran = true;
        if (g_cpu_only && QueryThreadCycleTime(g_target, &cycles)) {
            /* Suspending the thread charges it a little kernel work: count
             * it as running when it used about a hundredth of the interval
             * (30 of the 3000 cycles per microsecond of a 3 GHz core). */
            ran = cycles - g_last_cycles > (ULONG64)g_interval_us * 30;
            g_last_cycles = cycles;
        }
        if (ran && GetThreadContext(g_target, &c)) record(&c);
        else if (!ran) g_skipped++;
        ResumeThread(g_target);
    }
    timeEndPeriod(1);
    return 0;
}

void cyber_sampler_start(void) {
    const char *p = getenv("CYBER_SAMPLE");
    if (!p || !p[0] || g_thread) return;
    const char *cpu = getenv("CYBER_SAMPLE_CPU");
    g_cpu_only = cpu && cpu[0] && cpu[0] != '0';
    const char *us = getenv("CYBER_SAMPLE_US");
    if (us && atoi(us) > 0) g_interval_us = (unsigned)atoi(us);
    const char *max_ms = getenv("CYBER_SAMPLE_MAX_MS");
    if (max_ms && atoi(max_ms) > 0) g_max_ms = (unsigned)atoi(max_ms);
    g_path = strdup(p);
    HMODULE mod = GetModuleHandleA(NULL);
    g_base = (uint64_t)(uintptr_t)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)mod + dos->e_lfanew);
    g_end = g_base + nt->OptionalHeader.SizeOfImage;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    g_thread = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
}

void cyber_sampler_stop(void) {
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = NULL;
    /* Unless the time limit already wrote it. */
    write_samples();
}

/* Heap checker (CYBER_HEAPCHECK=1): a thread validates the process heaps
 * about every millisecond and, at the first damage it finds, prints the
 * main thread's stack (rvas, for addr2line) and stops.  It tells when the
 * heap was damaged, not who did it: the stack shows what the main thread
 * was doing just after. */
static HANDLE g_hc_target;

static DWORD WINAPI heapcheck_main(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    for (;;) {
        HANDLE heaps[64];
        DWORD n = GetProcessHeaps(64, heaps);
        for (DWORD i = 0; i < n && i < 64; i++) {
            if (HeapValidate(heaps[i], 0, NULL)) continue;
            uint64_t rvas[SAMPLE_DEPTH];
            int d = 0;
            if (SuspendThread(g_hc_target) != (DWORD)-1) {
                CONTEXT c;
                memset(&c, 0, sizeof(c));
                c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                if (GetThreadContext(g_hc_target, &c)) d = walk(&c, rvas, SAMPLE_DEPTH);
                ResumeThread(g_hc_target);
            }
            /* Print only now: the main thread may have held the heap lock. */
            fprintf(stderr, "[heapcheck] heap %lu is damaged; main thread:", (unsigned long)i);
            for (int k = 0; k < d; k++) fprintf(stderr, " %llx", (unsigned long long)rvas[k]);
            fputc('\n', stderr);
            fflush(stderr);
            timeEndPeriod(1);
            return 0;
        }
        Sleep(1);
    }
}

/* Heap damage found by the heap manager itself (a bad or double free) is
 * raised as STATUS_HEAP_CORRUPTION before the process ends: print where. */
static LONG CALLBACK heapcheck_veh(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != 0xC0000374) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t rvas[SAMPLE_DEPTH];
    int d = walk(ep->ContextRecord, rvas, SAMPLE_DEPTH);
    char line[1024];
    int len = snprintf(line, sizeof(line), "[heapcheck] heap corruption raised at:");
    for (int k = 0; k < d && len < (int)sizeof(line) - 20; k++)
        len += snprintf(line + len, sizeof(line) - (size_t)len, " %llx", (unsigned long long)rvas[k]);
    line[len++] = '\n';
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, (DWORD)len, &w, NULL);
    return EXCEPTION_CONTINUE_SEARCH;
}

void cyber_heapcheck_start(void) {
    const char *p = getenv("CYBER_HEAPCHECK");
    if (!p || !p[0] || p[0] == '0' || g_hc_target) return;
    HMODULE mod = GetModuleHandleA(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)mod + dos->e_lfanew);
    g_base = (uint64_t)(uintptr_t)mod;
    g_end = g_base + nt->OptionalHeader.SizeOfImage;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_hc_target,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        return;
    AddVectoredExceptionHandler(1, heapcheck_veh);
    HANDLE t = CreateThread(NULL, 0, heapcheck_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
    fprintf(stderr, "[heapcheck] validating the process heaps\n");
}

#else
void cyber_sampler_start(void) {}
void cyber_sampler_stop(void) {}
void cyber_heapcheck_start(void) {}
#endif
