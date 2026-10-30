#ifndef WIN32_PTHREAD_COMPAT_H
#define WIN32_PTHREAD_COMPAT_H

/*
 * Minimal pthread compatibility layer for Windows.
 * Provides just enough for the cyberbrowser codebase:
 *  - mutex (SRWLOCK-based)
 *  - condition variables (CONDITION_VARIABLE)
 *  - thread creation/join/detach
 *
 * This header shadows the system <pthread.h> on Windows builds
 * by placing its directory earlier in the include path.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>

/* ============================================================================
 * Mutex
 * ============================================================================ */

typedef SRWLOCK pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER SRWLOCK_INIT

static inline int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr) {
    (void)attr;
    InitializeSRWLock(mutex);
    return 0;
}

static inline int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    (void)mutex;
    /* SRWLOCKs do not require explicit destruction */
    return 0;
}

static inline int pthread_mutex_lock(pthread_mutex_t *mutex) {
    AcquireSRWLockExclusive(mutex);
    return 0;
}

static inline int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    ReleaseSRWLockExclusive(mutex);
    return 0;
}

/* ============================================================================
 * Threads
 * ============================================================================ */

typedef HANDLE pthread_t;
typedef struct { int unused; } pthread_attr_t;     /* (attributes are ignored) */

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
} win32_pthread_wrapper_data;

static DWORD WINAPI win32_pthread_wrapper(LPVOID lpParam) {
    win32_pthread_wrapper_data *data = (win32_pthread_wrapper_data *)lpParam;
    void *(*start)(void *) = data->start_routine;
    void *arg = data->arg;
    free(data);
    start(arg);
    return 0;
}

static inline int pthread_create(pthread_t *thread, const void *attr,
                                 void *(*start_routine)(void *), void *arg) {
    (void)attr;
    win32_pthread_wrapper_data *data =
        (win32_pthread_wrapper_data *)malloc(sizeof(*data));
    if (!data) return -1;
    data->start_routine = start_routine;
    data->arg = arg;
    *thread = CreateThread(NULL, 0, win32_pthread_wrapper, data, 0, NULL);
    if (*thread == NULL) {
        free(data);
        return -1;
    }
    return 0;
}

static inline int pthread_join(pthread_t thread, void **retval) {
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (retval) *retval = NULL;
    return 0;
}

static inline int pthread_detach(pthread_t thread) {
    CloseHandle(thread);
    return 0;
}

/* ============================================================================
 * Condition variables (CONDITION_VARIABLE over the SRWLOCK mutexes)
 * ============================================================================ */

typedef CONDITION_VARIABLE pthread_cond_t;
#define PTHREAD_COND_INITIALIZER CONDITION_VARIABLE_INIT

static inline int pthread_cond_init(pthread_cond_t *cond, const void *attr) {
    (void)attr;
    InitializeConditionVariable(cond);
    return 0;
}

static inline int pthread_cond_destroy(pthread_cond_t *cond) {
    (void)cond;
    return 0;
}

static inline int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    return SleepConditionVariableSRW(cond, mutex, INFINITE, 0) ? 0 : -1;
}

/* Until abstime (CLOCK_REALTIME), as POSIX has it: ETIMEDOUT when it passed. */
static inline int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                                         const struct timespec *abstime) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    long long ms = ((long long)abstime->tv_sec - (long long)now.tv_sec) * 1000LL +
                   ((long long)abstime->tv_nsec - (long long)now.tv_nsec) / 1000000LL;
    if (ms < 0) ms = 0;
    if (ms > 0x7ffffffeLL) ms = 0x7ffffffeLL;
    if (SleepConditionVariableSRW(cond, mutex, (DWORD)ms, 0)) return 0;
    return GetLastError() == ERROR_TIMEOUT ? ETIMEDOUT : EINVAL;
}

static inline int pthread_cond_signal(pthread_cond_t *cond) {
    WakeConditionVariable(cond);
    return 0;
}

static inline int pthread_cond_broadcast(pthread_cond_t *cond) {
    WakeAllConditionVariable(cond);
    return 0;
}

#endif /* WIN32_PTHREAD_COMPAT_H */
