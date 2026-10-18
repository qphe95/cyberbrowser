/*
 * Job system (PARALLEL_STYLE_LAYOUT.md §5).
 *
 *   - Worker threads, each owning a Chase–Lev work-stealing deque: the owner
 *     pushes and pops at the bottom, thieves steal from the top with one CAS.
 *   - A thread that is not a worker (the JS thread) gets its own deque while
 *     it runs a job graph, and helps: it executes jobs until the graph is done.
 *   - Dependencies are counters.  A job's `pending` counts itself plus its
 *     outstanding children; whoever brings it to zero runs the job's
 *     continuation (if any) and then notifies the job's parent.  Nobody waits
 *     for children: the last one to finish carries the parent forward.
 *   - Jobs are allocated from per-thread frame arenas and never freed one by
 *     one; job_graph_run() resets the arenas when the graph is done.
 *   - No mutexes, no spinning on another job's flag.  Idle workers sleep on
 *     an address after a failed round of steals; that is the only blocking.
 *
 * Switches: CSS_THREADS=<n> total threads (1 = everything inline on the
 * calling thread), CSS_STEAL=random (random victim order and occasional
 * yields, to shake out ordering bugs).
 */
#ifndef JOB_SYSTEM_H
#define JOB_SYSTEM_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "frame_arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Job Job;
typedef void (*JobFn)(Job *job);

struct Job {
    JobFn fn;                   /* body */
    JobFn cont;                 /* continuation, run when pending reaches 0 */
    Job *parent;                /* notified when this job (and cont) finished */
    _Atomic int32_t pending;    /* 1 (self) + outstanding children */
    _Atomic uint32_t done;      /* set on a root job when the graph is done */
    void *ctx;
    uint32_t a, b;              /* payload: a range, an index, ... */
};

/* Allocate a job from the calling thread's arena.  `parent` may be NULL for
 * a root.  The job is not scheduled until job_spawn(). */
Job *job_new(JobFn fn, Job *parent, void *ctx, uint32_t a, uint32_t b);

/* Schedule `job` (counts it in its parent's pending first).  Runs it inline
 * when the calling thread's deque is full. */
void job_spawn(Job *job);

/* Set the continuation of the running job: it runs once the job body and all
 * children spawned so far and from now on have finished. */
static inline void job_set_continuation(Job *job, JobFn cont) { job->cont = cont; }

/* Fork-join inside a running job (for recursive algorithms that cannot be
 * split into continuations): create a group, spawn children into it, then
 * wait.  Waiting runs jobs (the thread's own children first), so it never
 * idles while work exists and cannot deadlock: a group only waits for its
 * own descendants. */
Job *job_group_new(void);
void job_group_spawn(Job *group, JobFn fn, void *ctx, uint32_t a, uint32_t b);
void job_group_wait(Job *group);

/* Run a job graph rooted at `root` (parent NULL) and return when it is done.
 * The calling thread executes jobs while it waits.  Not reentrant: one graph
 * at a time per calling thread. */
void job_graph_run(Job *root);

/* Number of threads that execute jobs (workers + the helping thread). */
int job_thread_count(void);
/* Index of the calling thread among job threads: 0 = the helping (graph
 * owner) thread, 1..n-1 = workers; -1 for unrelated threads. */
int job_thread_index(void);
/* The calling job thread's frame arena (jobs and per-graph scratch). */
FrameArena *job_thread_arena(void);

/* Statistics of the last graph: jobs run and successful steals. */
void job_last_stats(uint64_t *jobs, uint64_t *steals);

#ifdef __cplusplus
}
#endif

#endif /* JOB_SYSTEM_H */
