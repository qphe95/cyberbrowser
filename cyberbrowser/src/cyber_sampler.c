/* Sampling profiler (CYBER_SAMPLE=<path>): a thread suspends the main thread
 * every CYBER_SAMPLE_US microseconds (default 1000), records its instruction
 * pointer and the return addresses the x64 unwind tables give, and writes
 * one line per sample ("rva rva ..." leaf first, relative to the image
 * base) to <path> at exit.  tools/sample_report.js symbolizes them. */
#include "cyber_sampler.h"

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
static char *g_path;

static void record(const CONTEXT *c0) {
    if (g_n == g_cap) {
        size_t nc = g_cap ? g_cap * 2 : 65536;
        uint64_t *ns = (uint64_t *)realloc(g_samples, nc * (SAMPLE_DEPTH + 1) * sizeof(uint64_t));
        if (!ns) return;
        g_samples = ns;
        g_cap = nc;
    }
    uint64_t *rec = g_samples + g_n * (SAMPLE_DEPTH + 1);
    CONTEXT c = *c0;
    int d = 0;
    while (d < SAMPLE_DEPTH && c.Rip) {
        rec[1 + d++] = c.Rip >= g_base && c.Rip < g_end ? c.Rip - g_base : 0;
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
    rec[0] = (uint64_t)d;
    g_n++;
}

static DWORD WINAPI sampler_main(LPVOID arg) {
    (void)arg;
    timeBeginPeriod(1);
    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);
    while (!g_stop) {
        next.QuadPart += freq.QuadPart * g_interval_us / 1000000;
        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= next.QuadPart) break;
            LONGLONG left_us = (next.QuadPart - now.QuadPart) * 1000000 / freq.QuadPart;
            Sleep(left_us > 1500 ? (DWORD)(left_us / 1000) : 0);
        }
        if (SuspendThread(g_target) == (DWORD)-1) break;
        CONTEXT c;
        memset(&c, 0, sizeof(c));
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(g_target, &c)) record(&c);
        ResumeThread(g_target);
    }
    timeEndPeriod(1);
    return 0;
}

void cyber_sampler_start(void) {
    const char *p = getenv("CYBER_SAMPLE");
    if (!p || !p[0] || g_thread) return;
    const char *us = getenv("CYBER_SAMPLE_US");
    if (us && atoi(us) > 0) g_interval_us = (unsigned)atoi(us);
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
    FILE *f = fopen(g_path, "w");
    if (!f) return;
    fprintf(f, "# interval_us %u samples %zu\n", g_interval_us, g_n);
    for (size_t i = 0; i < g_n; i++) {
        const uint64_t *rec = g_samples + i * (SAMPLE_DEPTH + 1);
        for (uint64_t d = 0; d < rec[0]; d++) fprintf(f, d ? " %llx" : "%llx", (unsigned long long)rec[1 + d]);
        fputc('\n', f);
    }
    fclose(f);
    fprintf(stderr, "[sampler] %zu samples written to %s\n", g_n, g_path);
}

#else
void cyber_sampler_start(void) {}
void cyber_sampler_stop(void) {}
#endif
