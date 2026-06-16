#ifndef WIN32_COMPAT_H
#define WIN32_COMPAT_H

/*
 * Windows compatibility layer for POSIX APIs and types.
 * Included from platform.h on Windows builds.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <basetsd.h>
#include <string.h>
#include <time.h>
#include <direct.h>
#include <io.h>

/* ssize_t is not defined by MSVC; use intptr_t equivalent */
#ifndef _SSIZE_T_DEFINED
#ifdef _WIN64
typedef long long ssize_t;
#else
typedef int ssize_t;
#endif
#define _SSIZE_T_DEFINED
#endif

/* socklen_t is not defined on Windows */
#ifndef _SOCKLEN_T_DEFINED
typedef int socklen_t;
#define _SOCKLEN_T_DEFINED
#endif

/* Case-insensitive string comparisons (POSIX -> MSVC) */
#define strcasecmp _stricmp
#define strncasecmp _strnicmp

/* timegm is a GNU/BSD extension; MSVC provides _mkgmtime */
#define timegm _mkgmtime

/* gmtime_r and localtime_r are POSIX; provide wrappers using gmtime_s/localtime_s */
static inline struct tm *gmtime_r(const time_t *t, struct tm *tm) {
    if (gmtime_s(tm, t) != 0) return NULL;
    return tm;
}
static inline struct tm *localtime_r(const time_t *t, struct tm *tm) {
    if (localtime_s(tm, t) != 0) return NULL;
    return tm;
}

/* mkdir on Windows takes only one argument */
#define mkdir(path, mode) _mkdir(path)

/* access() constants */
#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 6
#endif
#define access _access

/* Windows socket compatibility */
#include <winsock2.h>
#include <ws2tcpip.h>

/* Minimal pthread compatibility for Windows */
#ifdef _MSC_VER
#include "win32/pthread.h"
#else
#include <pthread.h>
#endif

/* Convenience: setsockopt timeout using DWORD milliseconds on Windows */
static inline int win32_setsockopt_timeout(int sockfd, int optname, int seconds) {
    DWORD timeout_ms = (DWORD)(seconds * 1000);
    return setsockopt((SOCKET)sockfd, SOL_SOCKET, optname, (const char *)&timeout_ms, sizeof(timeout_ms));
}

#endif /* WIN32_COMPAT_H */
