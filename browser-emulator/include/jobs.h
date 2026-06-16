#ifndef JOBS_H
#define JOBS_H

#include <pthread.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool jobs_start_thread(pthread_t *thread, void *(*fn)(void *), void *arg);

#ifdef __cplusplus
}
#endif

#endif
