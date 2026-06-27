#ifndef WIN32_PTHREAD_COMPAT_H
#define WIN32_PTHREAD_COMPAT_H

/*
 * Minimal pthread compatibility layer for Windows.
 * Provides just enough for the cyberbrowser codebase:
 *  - mutex (SRWLOCK-based)
 *  - thread creation/join (for jobs.c)
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

#endif /* WIN32_PTHREAD_COMPAT_H */
