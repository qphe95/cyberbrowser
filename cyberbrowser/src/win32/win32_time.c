/* win32_time.c - clock_gettime and nanosleep over Win32, so that nothing
 * (the engine, or the static ffmpeg's libavutil) links MinGW's winpthreads
 * for them.  MinGW's <time.h> makes clock_gettime and nanosleep inline
 * wrappers around clock_gettime64 and nanosleep64: those are defined here. */
#include <errno.h>
#include <time.h>
#include <windows.h>

int clock_gettime64(clockid_t clock, struct _timespec64 *tp) {
    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    if (clock == CLOCK_REALTIME) {
        /* 100 ns units since 1601. */
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL;
        tp->tv_sec = (__time64_t)(t / 10000000ULL);
        tp->tv_nsec = (long)(t % 10000000ULL) * 100;
        return 0;
    }
    if (clock == CLOCK_MONOTONIC || clock == CLOCK_PROCESS_CPUTIME_ID || clock == CLOCK_THREAD_CPUTIME_ID) {
        static LARGE_INTEGER freq;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        tp->tv_sec = (__time64_t)(c.QuadPart / freq.QuadPart);
        tp->tv_nsec = (long)((c.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
        return 0;
    }
    errno = EINVAL;
    return -1;
}

int nanosleep64(const struct _timespec64 *req, struct _timespec64 *rem) {
    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }
    unsigned long long ms = (unsigned long long)req->tv_sec * 1000ULL + (unsigned long long)(req->tv_nsec + 999999L) / 1000000ULL;
    while (ms > 0) {
        DWORD step = ms > 0x7fffffffULL ? 0x7fffffffUL : (DWORD)ms;
        Sleep(step);
        ms -= step;
    }
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}
