#include "jobs.h"

bool jobs_start_thread(pthread_t *thread, void *(*fn)(void *), void *arg) {
    if (!thread || !fn) {
        return false;
    }
    return pthread_create(thread, NULL, fn, arg) == 0;
}
