#ifndef WIN32_SYS_TIME_H
#define WIN32_SYS_TIME_H

/*
 * Minimal sys/time.h compatibility for Windows.
 * Provides struct timeval and gettimeofday() using Windows APIs.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

static inline void gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    /* Convert from 100-nanosecond intervals since Jan 1, 1601
       to microseconds since Jan 1, 1970 */
    uli.QuadPart -= 116444736000000000ULL;
    tv->tv_sec = (long)(uli.QuadPart / 10000000ULL);
    tv->tv_usec = (long)((uli.QuadPart % 10000000ULL) / 10);
}

#endif /* WIN32_SYS_TIME_H */
