#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#ifdef BE_PLATFORM_ANDROID
#include <android/log.h>
#endif
#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "quickjs-internal.h"

/* Platform threading support */
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#endif

/* 
 * Debug logging for GC - controlled via QJS_DEBUG environment variable
 * Set QJS_DEBUG=1 for info level, QJS_DEBUG=2 for verbose debug
 */
static inline int qjs_gc_debug_level(void) {
    static int level = -1;
    if (level < 0) {
        const char *env = getenv("QJS_DEBUG");
        if (env) {
            level = atoi(env);
            if (level == 0 && env[0] != '0') level = 1;
        } else {
            level = 0;
        }
    }
    return level;
}

#define GC_LOGD(...) do { if (qjs_gc_debug_level() >= 2) { fprintf(stderr, "[D/GC] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } } while(0)
#define GC_LOGI(...) do { if (qjs_gc_debug_level() >= 1) { fprintf(stderr, "[I/GC] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } } while(0)
#define GC_LOGW(...) do { if (qjs_gc_debug_level() >= 1) { fprintf(stderr, "[W/GC] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } } while(0)
#define GC_LOGE(...) do { fprintf(stderr, "[E/GC] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while(0)

/* External C function declarations */
extern "C" void js_quickjs_reset_class_ids(void);
extern "C" void browser_api_impl_reset(void);

/* Forward declarations for QuickJS GC bridge functions (defined in quickjs.cpp) */
extern "C" {
    /* mark_children - recursively marks all children of a GC object */
    typedef void JS_MarkFunc(JSRuntimeHandle rt, GCHandle handle);
    void mark_children(JSRuntimeHandle rt, GCHandle handle, JS_MarkFunc *mark_func);
    
    /* JS_MarkValue - marks a JSValue if it contains GC references */
    void JS_MarkValue(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func);
    
    /* JS_MarkContext - marks all roots in a JSContext */
    void JS_MarkContext(JSRuntimeHandle rt, uint32_t ctx_handle, JS_MarkFunc *mark_func);
    
    /* Weak reference handling */
    void gc_remove_weak_objects(JSRuntimeHandle rt);
    
    /* Atom sweeping */
    void gc_sweep_atoms(JSRuntimeHandle rt);
    
    /* Shape hash cleanup */
    void gc_cleanup_shape_hash_table(JSRuntimeHandle rt);
    
    /* Stack frame marking */
    typedef struct JSStackFrame JSStackFrame;
    JSStackFrame *JSRuntime_get_current_stack_frame(JSRuntimeHandle rt);
    
    /* Atom marking helpers */
    uint32_t JSRuntime_get_permanent_atom_count(JSRuntimeHandle rt);
    uint32_t JSRuntime_get_atom_hash_size(JSRuntimeHandle rt);
    uint32_t JSRuntime_get_atom_hash_count(JSRuntimeHandle rt);
    uint32_t *JSRuntime_get_atom_hash(JSRuntimeHandle rt);
    uint32_t JSString_get_hash_next(uint32_t atom_idx);
    void JSString_set_hash_next(uint32_t atom_idx, uint32_t next);
    
    /* Shape hash helpers */
    uint32_t JSRuntime_get_shape_hash_size(JSRuntimeHandle rt);
    uint32_t JSRuntime_get_shape_hash_count(JSRuntimeHandle rt);
    void JSRuntime_set_shape_hash_count(JSRuntimeHandle rt, uint32_t count);
    uint32_t *JSRuntime_get_shape_hash(JSRuntimeHandle rt);
    int JSShape_is_hashed(uint32_t shape_handle);
    void JSShape_set_is_hashed(uint32_t shape_handle, int val);
    
    /* Job queue iteration - returns handles directly */
    uint32_t JSRuntime_job_queue_count(JSRuntimeHandle rt);
    uint32_t JSRuntime_job_queue_get_handle(JSRuntimeHandle rt, int index);
    uint32_t JSJobEntry_get_realm_handle(uint32_t job_handle);
    int JSJobEntry_get_argc(uint32_t job_handle);
    GCValue JSJobEntry_get_argv(uint32_t job_handle, int index);
    
    /* Handle arrays in runtime */
    uint32_t JSRuntime_get_shape_hash_handle(JSRuntimeHandle rt);
    uint32_t JSRuntime_get_atom_array_handle(JSRuntimeHandle rt);
    GCValue JSRuntime_get_current_exception(JSRuntimeHandle rt);
    
    /* Stack frame helpers */
    GCValue JSStackFrame_get_cur_func(JSStackFrame *sf);
    JSStackFrame *JSStackFrame_get_prev(JSStackFrame *sf);
    
    /* Module iteration */
    typedef void (*ContextIteratorFunc)(uint32_t ctx_handle, void *user_data);
    void JSRuntime_for_each_context(JSRuntimeHandle rt, ContextIteratorFunc func, void *user_data);
}

#define ALIGN16(size) (((size) + 15) & ~15)
/* Layout: [16-byte prefix] [64-byte GCHeader] [user data (ALIGN16(size))] [16-byte suffix].
 * The 16-byte prefix ensures the user pointer is 16-byte aligned while keeping
 * objects contiguous on 16-byte boundaries. */
#define MIN_OBJECT_SIZE (sizeof(GCHeader) + 32)  /* 64 + 32 = 96 bytes minimum */

/* 
 * Calculate total allocation size from header.
 * Layout: [16-byte prefix] [64-byte GCHeader] [user data (aligned)] [16-byte suffix]
 */
static inline size_t gc_alloc_total_size(GCHeader *hdr) {
    if (!hdr) return MIN_OBJECT_SIZE;
    /* Mask out FREED flag (bit 31) when calculating size */
    uint32_t user_size = hdr->size & 0x7FFFFFFF;
    return 16 + sizeof(GCHeader) + ALIGN16(user_size) + 16;
}

GCState g_gc = {0};

static void gc_run_internal(void);
static void gc_maybe_run(void);

/* Forward declarations for canary functions (defined later) */
static inline uint64_t *gc_canary_prefix_ptr(GCHeader *hdr);
static inline uint64_t *gc_canary_suffix_ptr(GCHeader *hdr);
static inline void gc_set_canaries(GCHeader *hdr);
static inline void gc_corrupt_canaries(GCHeader *hdr);
static GCCanaryStatus gc_validate_canaries_hdr(GCHeader *hdr, void **out_ptr);

/* ============================================================================
 * DOUBLE-BUFFERED GC - Buffer Management
 * ============================================================================
 */

static inline GCBuffer *gc_active_buffer(void) {
    return &g_gc.buffers[gc_active_buffer_index()];
}

static inline GCBuffer *gc_inactive_buffer(void) {
    return &g_gc.buffers[1 - gc_active_buffer_index()];
}

static inline uint8_t *gc_active_heap(void) {
    return gc_active_buffer()->storage;
}

static inline size_t gc_active_heap_size(void) {
    return gc_active_buffer()->storage_size;
}

static inline size_t gc_active_bump_offset(void) {
    return gc_active_buffer()->bump_offset;
}

/* Get the active handle table (the one currently being used for dereferencing) */
static inline void **gc_active_handles(void) {
    return (void **)g_gc.active_handle_table;
}

static inline uint32_t gc_active_handle_count(void) {
    return gc_active_buffer()->handle_count;
}

static inline uint32_t gc_active_handle_capacity(void) {
    return gc_active_buffer()->handle_capacity;
}

/* ============================================================================
 * THREAD POOL
 * ============================================================================
 */

#define GC_THREAD_POOL_MIN_THREADS 2
#define GC_SCHEDULER_PERIOD_MS 1000

typedef void (*GCThreadPoolJobFunc)(void *arg);

typedef struct GCThreadPoolJob {
    GCThreadPoolJobFunc func;
    void *arg;
    struct GCThreadPoolJob *next;
} GCThreadPoolJob;

typedef struct {
#ifdef _WIN32
    HANDLE *threads;
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE cond;
    CONDITION_VARIABLE completion_cond;
#else
    pthread_t *threads;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_cond_t completion_cond;
#endif
    uint32_t thread_count;
    GCThreadPoolJob *job_queue_head;
    GCThreadPoolJob *job_queue_tail;
    volatile bool shutdown;
    volatile uint32_t active_jobs;
    volatile uint32_t pending_jobs;
} GCThreadPool;

static GCThreadPool g_gc_pool = {0};
static bool g_gc_pool_initialized = false;
static volatile bool g_gc_scheduler_running = false;
static volatile bool g_gc_requested = false;
#ifdef _WIN32
static HANDLE g_gc_scheduler_thread = NULL;
#else
static pthread_t g_gc_scheduler_thread = 0;
#endif

static uint32_t gc_get_processor_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    return (uint32_t)n;
#endif
}

static void gc_thread_pool_lock(GCThreadPool *pool) {
#ifdef _WIN32
    EnterCriticalSection(&pool->mutex);
#else
    pthread_mutex_lock(&pool->mutex);
#endif
}

static void gc_thread_pool_unlock(GCThreadPool *pool) {
#ifdef _WIN32
    LeaveCriticalSection(&pool->mutex);
#else
    pthread_mutex_unlock(&pool->mutex);
#endif
}

static GCThreadPoolJob *gc_thread_pool_pop_job(GCThreadPool *pool) {
    GCThreadPoolJob *job = pool->job_queue_head;
    if (job) {
        pool->job_queue_head = job->next;
        if (!pool->job_queue_head) {
            pool->job_queue_tail = NULL;
        }
        job->next = NULL;
    }
    return job;
}

#ifdef _WIN32
static DWORD WINAPI gc_thread_pool_worker(LPVOID arg) {
#else
static void *gc_thread_pool_worker(void *arg) {
#endif
    GCThreadPool *pool = (GCThreadPool *)arg;
    
    while (true) {
        gc_thread_pool_lock(pool);
        
        while (!pool->shutdown && pool->job_queue_head == NULL) {
#ifdef _WIN32
            SleepConditionVariableCS(&pool->cond, &pool->mutex, INFINITE);
#else
            pthread_cond_wait(&pool->cond, &pool->mutex);
#endif
        }
        
        if (pool->shutdown) {
            gc_thread_pool_unlock(pool);
            break;
        }
        
        GCThreadPoolJob *job = gc_thread_pool_pop_job(pool);
        if (job) {
            atomic_fetch_add_u32((volatile uint32_t *)&pool->active_jobs, 1);
            atomic_fetch_add_u32((volatile uint32_t *)&pool->pending_jobs, (uint32_t)-1);
        }
        gc_thread_pool_unlock(pool);
        
        if (job) {
            job->func(job->arg);
            free(job);
            
            gc_thread_pool_lock(pool);
            atomic_fetch_add_u32((volatile uint32_t *)&pool->active_jobs, (uint32_t)-1);
            if (pool->active_jobs == 0 && pool->pending_jobs == 0) {
#ifdef _WIN32
                WakeAllConditionVariable(&pool->completion_cond);
#else
                pthread_cond_broadcast(&pool->completion_cond);
#endif
            }
            gc_thread_pool_unlock(pool);
        }
    }
    
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static bool gc_thread_pool_init(void) {
    if (g_gc_pool_initialized) return true;
    
    uint32_t cores = gc_get_processor_count();
    uint32_t thread_count = cores * 2;
    if (thread_count < GC_THREAD_POOL_MIN_THREADS) {
        thread_count = GC_THREAD_POOL_MIN_THREADS;
    }
    
    g_gc_pool.thread_count = thread_count;
    g_gc_pool.threads = (HANDLE *)malloc(thread_count * sizeof(HANDLE));
    if (!g_gc_pool.threads) return false;
    
#ifdef _WIN32
    InitializeCriticalSection(&g_gc_pool.mutex);
    InitializeConditionVariable(&g_gc_pool.cond);
    InitializeConditionVariable(&g_gc_pool.completion_cond);
#else
    pthread_mutex_init(&g_gc_pool.mutex, NULL);
    pthread_cond_init(&g_gc_pool.cond, NULL);
    pthread_cond_init(&g_gc_pool.completion_cond, NULL);
#endif
    
    g_gc_pool.shutdown = false;
    g_gc_pool.active_jobs = 0;
    g_gc_pool.pending_jobs = 0;
    g_gc_pool.job_queue_head = NULL;
    g_gc_pool.job_queue_tail = NULL;
    
    for (uint32_t i = 0; i < thread_count; i++) {
#ifdef _WIN32
        g_gc_pool.threads[i] = CreateThread(NULL, 0, gc_thread_pool_worker, &g_gc_pool, 0, NULL);
        if (!g_gc_pool.threads[i]) {
            /* Shutdown already created threads */
            g_gc_pool.shutdown = true;
            WakeAllConditionVariable(&g_gc_pool.cond);
            for (uint32_t j = 0; j < i; j++) {
                WaitForSingleObject(g_gc_pool.threads[j], INFINITE);
                CloseHandle(g_gc_pool.threads[j]);
            }
            free(g_gc_pool.threads);
            g_gc_pool.threads = NULL;
            DeleteCriticalSection(&g_gc_pool.mutex);
            return false;
        }
#else
        if (pthread_create(&g_gc_pool.threads[i], NULL, gc_thread_pool_worker, &g_gc_pool) != 0) {
            g_gc_pool.shutdown = true;
            pthread_cond_broadcast(&g_gc_pool.cond);
            for (uint32_t j = 0; j < i; j++) {
                pthread_join(g_gc_pool.threads[j], NULL);
            }
            free(g_gc_pool.threads);
            g_gc_pool.threads = NULL;
            pthread_mutex_destroy(&g_gc_pool.mutex);
            pthread_cond_destroy(&g_gc_pool.cond);
            pthread_cond_destroy(&g_gc_pool.completion_cond);
            return false;
        }
#endif
    }
    
    g_gc_pool_initialized = true;
    GC_LOGI("GC thread pool initialized with %u threads (%u cores)", thread_count, cores);
    return true;
}

static void gc_thread_pool_shutdown(void) {
    if (!g_gc_pool_initialized) return;
    
    gc_thread_pool_lock(&g_gc_pool);
    g_gc_pool.shutdown = true;
    gc_thread_pool_unlock(&g_gc_pool);
    
#ifdef _WIN32
    WakeAllConditionVariable(&g_gc_pool.cond);
#else
    pthread_cond_broadcast(&g_gc_pool.cond);
#endif
    
    for (uint32_t i = 0; i < g_gc_pool.thread_count; i++) {
#ifdef _WIN32
        WaitForSingleObject(g_gc_pool.threads[i], INFINITE);
        CloseHandle(g_gc_pool.threads[i]);
#else
        pthread_join(g_gc_pool.threads[i], NULL);
#endif
    }
    
    /* Free any remaining jobs */
    GCThreadPoolJob *job = g_gc_pool.job_queue_head;
    while (job) {
        GCThreadPoolJob *next = job->next;
        free(job);
        job = next;
    }
    
    free(g_gc_pool.threads);
    g_gc_pool.threads = NULL;
    g_gc_pool.thread_count = 0;
    g_gc_pool.job_queue_head = NULL;
    g_gc_pool.job_queue_tail = NULL;
    g_gc_pool.active_jobs = 0;
    g_gc_pool.pending_jobs = 0;
    g_gc_pool_initialized = false;
    
#ifdef _WIN32
    DeleteCriticalSection(&g_gc_pool.mutex);
#else
    pthread_mutex_destroy(&g_gc_pool.mutex);
    pthread_cond_destroy(&g_gc_pool.cond);
    pthread_cond_destroy(&g_gc_pool.completion_cond);
#endif
    
    GC_LOGI("GC thread pool shut down");
}

static bool gc_thread_pool_submit(GCThreadPoolJobFunc func, void *arg) {
    if (!g_gc_pool_initialized) return false;
    
    GCThreadPoolJob *job = (GCThreadPoolJob *)malloc(sizeof(GCThreadPoolJob));
    if (!job) return false;
    job->func = func;
    job->arg = arg;
    job->next = NULL;
    
    gc_thread_pool_lock(&g_gc_pool);
    if (g_gc_pool.shutdown) {
        gc_thread_pool_unlock(&g_gc_pool);
        free(job);
        return false;
    }
    if (g_gc_pool.job_queue_tail) {
        g_gc_pool.job_queue_tail->next = job;
    } else {
        g_gc_pool.job_queue_head = job;
    }
    g_gc_pool.job_queue_tail = job;
    atomic_fetch_add_u32((volatile uint32_t *)&g_gc_pool.pending_jobs, 1);
    gc_thread_pool_unlock(&g_gc_pool);
    
#ifdef _WIN32
    WakeAllConditionVariable(&g_gc_pool.cond);
#else
    pthread_cond_broadcast(&g_gc_pool.cond);
#endif
    
    return true;
}

/* ============================================================================
 * GC SCHEDULER & JOB
 * ============================================================================
 */

static void gc_job_func(void *arg) {
    (void)arg;
    
    /* Only run if GC is idle. Multiple jobs may be submitted while one is
     * running; redundant ones exit quickly. */
    uint32_t phase = atomic_load_u32(&g_gc.gc_phase);
    if (phase != GC_PHASE_IDLE) return;
    
    g_gc_requested = false;
    atomic_store_u32(&g_gc.gc_phase, GC_PHASE_MARKING);
    gc_run_internal();
}

#ifdef _WIN32
static DWORD WINAPI gc_scheduler_thread_func(LPVOID arg) {
#else
static void *gc_scheduler_thread_func(void *arg) {
#endif
    (void)arg;
    GC_LOGI("GC scheduler thread started");
    
    while (g_gc_scheduler_running) {
        /* Wait first so that startup (runtime/context creation) is not
         * racing with a background GC cycle. */
#ifdef _WIN32
        Sleep(GC_SCHEDULER_PERIOD_MS);
#else
        usleep(GC_SCHEDULER_PERIOD_MS * 1000);
#endif
        if (!g_gc_scheduler_running) break;

        /* The scheduler only services outstanding GC requests; it does not
         * generate new ones. This keeps behavior equivalent to the previous
         * dedicated background thread, which only ran when requested. */
        uint32_t phase = atomic_load_u32(&g_gc.gc_phase);
        bool requested = g_gc_requested;
        if (phase == GC_PHASE_IDLE && requested) {
            gc_request_background();
        }
    }
    
    GC_LOGI("GC scheduler thread exiting");
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ============================================================================
 * GC THREAD CONTROL (compatibility wrappers around the thread pool)
 * ============================================================================
 */

bool gc_thread_start(void) {
    if (g_gc_pool_initialized) return true;
    
    if (!gc_thread_pool_init()) return false;
    
    /* Start the periodic scheduler thread */
    g_gc_scheduler_running = true;
#ifdef _WIN32
    g_gc_scheduler_thread = CreateThread(NULL, 0, gc_scheduler_thread_func, NULL, 0, NULL);
    if (!g_gc_scheduler_thread) {
        g_gc_scheduler_running = false;
        gc_thread_pool_shutdown();
        return false;
    }
#else
    if (pthread_create(&g_gc_scheduler_thread, NULL, gc_scheduler_thread_func, NULL) != 0) {
        g_gc_scheduler_running = false;
        gc_thread_pool_shutdown();
        return false;
    }
#endif
    
    GC_LOGI("GC thread pool and scheduler started");
    return true;
}

void gc_thread_stop(void) {
    if (!g_gc_pool_initialized) return;
    
    /* Stop scheduler first */
    g_gc_scheduler_running = false;
#ifdef _WIN32
    if (g_gc_scheduler_thread) {
        WaitForSingleObject(g_gc_scheduler_thread, INFINITE);
        CloseHandle(g_gc_scheduler_thread);
        g_gc_scheduler_thread = NULL;
    }
#else
    if (g_gc_scheduler_thread) {
        pthread_join(g_gc_scheduler_thread, NULL);
        g_gc_scheduler_thread = 0;
    }
#endif
    
    /* Wait for any in-progress GC, then shut down the pool */
    gc_wait_for_completion();
    gc_thread_pool_shutdown();
}

void gc_request_background(void) {
    if (!g_gc_pool_initialized) return;
    if (g_gc.rt == GC_HANDLE_NULL) return;
    
    uint32_t phase = atomic_load_u32(&g_gc.gc_phase);
    if (phase != GC_PHASE_IDLE) {
        /* Mark that a GC was requested; the running GC or a later scheduler
         * tick will handle it. */
        g_gc_requested = true;
        return;
    }
    
    g_gc_requested = true;
    gc_thread_pool_submit(gc_job_func, NULL);
}

void gc_wait_for_completion(void) {
    /* Wait for all submitted pool jobs to finish, then for phase to go idle */
    gc_thread_pool_wait_empty();
    while (atomic_load_u32(&g_gc.gc_phase) != GC_PHASE_IDLE) {
#ifdef _WIN32
        Sleep(1);
#else
        usleep(1000);
#endif
    }
}

bool gc_is_background_running(void) {
    return atomic_load_u32(&g_gc.gc_phase) != GC_PHASE_IDLE;
}
bool gc_is_marking_phase(void) {
    return atomic_load_u32(&g_gc.gc_phase) == (uint32_t)GC_PHASE_MARKING;
}


/* ============================================================================
 * THREAD POOL PUBLIC TEST HELPERS
 * ============================================================================
 */

uint32_t gc_thread_pool_get_thread_count(void) {
    return g_gc_pool_initialized ? g_gc_pool.thread_count : 0;
}

void gc_thread_pool_wait_empty(void) {
    if (!g_gc_pool_initialized) return;
    
    gc_thread_pool_lock(&g_gc_pool);
    while (g_gc_pool.active_jobs > 0 || g_gc_pool.pending_jobs > 0) {
#ifdef _WIN32
        SleepConditionVariableCS(&g_gc_pool.completion_cond, &g_gc_pool.mutex, INFINITE);
#else
        pthread_cond_wait(&g_gc_pool.completion_cond, &g_gc_pool.mutex);
#endif
    }
    gc_thread_pool_unlock(&g_gc_pool);
}

bool gc_thread_pool_submit_job(GCThreadPoolJobFunc func, void *arg) {
    return gc_thread_pool_submit(func, arg);
}

bool gc_thread_pool_submit_test_job(GCThreadPoolJobFunc func, void *arg) {
    return gc_thread_pool_submit_job(func, arg);
}

/* ============================================================================
 * GREY WORK QUEUE - thread-safe work list for concurrent marking
 * ============================================================================ */

/* ============================================================================
 * GREY WORK QUEUE (continued)
 * ============================================================================ */

typedef struct GCGreyNode {
    GCHandle handle;
    struct GCGreyNode *next;
} GCGreyNode;

typedef struct {
#ifdef _WIN32
    CRITICAL_SECTION mutex;
    CONDITION_VARIABLE cond;
#else
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif
    GCGreyNode *head;
    GCGreyNode *tail;
    volatile uint32_t count;
    volatile bool shutdown;
} GCGreyQueue;

static GCGreyQueue g_grey_queue = {0};
static bool g_grey_queue_initialized = false;

/* Synchronization primitive used by the mark job to signal completion to the
 * thread that submitted it. Using a dedicated event avoids the deadlock that
 * would occur if a pool job tried to wait on gc_thread_pool_wait_empty(). */
#ifdef _WIN32
static HANDLE g_gc_mark_done_event = NULL;
#else
static pthread_cond_t g_gc_mark_done_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_gc_mark_done_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool g_gc_mark_done = false;
#endif

static void gc_mark_signal_done(void) {
#ifdef _WIN32
    if (g_gc_mark_done_event) SetEvent(g_gc_mark_done_event);
#else
    pthread_mutex_lock(&g_gc_mark_done_mutex);
    g_gc_mark_done = true;
    pthread_cond_broadcast(&g_gc_mark_done_cond);
    pthread_mutex_unlock(&g_gc_mark_done_mutex);
#endif
}

static void gc_mark_wait_done(void) {
#ifdef _WIN32
    if (g_gc_mark_done_event) {
        WaitForSingleObject(g_gc_mark_done_event, INFINITE);
        ResetEvent(g_gc_mark_done_event);
    }
#else
    pthread_mutex_lock(&g_gc_mark_done_mutex);
    while (!g_gc_mark_done) {
        pthread_cond_wait(&g_gc_mark_done_cond, &g_gc_mark_done_mutex);
    }
    g_gc_mark_done = false;
    pthread_mutex_unlock(&g_gc_mark_done_mutex);
#endif
}

static void gc_grey_queue_lock(GCGreyQueue *q) {
#ifdef _WIN32
    EnterCriticalSection(&q->mutex);
#else
    pthread_mutex_lock(&q->mutex);
#endif
}

static void gc_grey_queue_unlock(GCGreyQueue *q) {
#ifdef _WIN32
    LeaveCriticalSection(&q->mutex);
#else
    pthread_mutex_unlock(&q->mutex);
#endif
}

static bool gc_grey_queue_init(void) {
    if (g_grey_queue_initialized) return true;
#ifdef _WIN32
    if (!g_gc_mark_done_event) {
        g_gc_mark_done_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    }
    InitializeCriticalSection(&g_grey_queue.mutex);
    InitializeConditionVariable(&g_grey_queue.cond);
#else
    g_gc_mark_done = false;
    pthread_mutex_init(&g_grey_queue.mutex, NULL);
    pthread_cond_init(&g_grey_queue.cond, NULL);
#endif
    g_grey_queue.head = NULL;
    g_grey_queue.tail = NULL;
    g_grey_queue.count = 0;
    g_grey_queue.shutdown = false;
    g_grey_queue_initialized = true;
    return true;
}

static void gc_grey_queue_cleanup(void) {
    if (!g_grey_queue_initialized) return;
    gc_grey_queue_lock(&g_grey_queue);
    g_grey_queue.shutdown = true;
    GCGreyNode *node = g_grey_queue.head;
    while (node) {
        GCGreyNode *next = node->next;
        free(node);
        node = next;
    }
    g_grey_queue.head = NULL;
    g_grey_queue.tail = NULL;
    g_grey_queue.count = 0;
    gc_grey_queue_unlock(&g_grey_queue);
#ifdef _WIN32
    if (g_gc_mark_done_event) {
        CloseHandle(g_gc_mark_done_event);
        g_gc_mark_done_event = NULL;
    }
    DeleteCriticalSection(&g_grey_queue.mutex);
#else
    g_gc_mark_done = false;
    pthread_mutex_destroy(&g_grey_queue.mutex);
    pthread_cond_destroy(&g_grey_queue.cond);
#endif
    g_grey_queue_initialized = false;
}

bool gc_grey_queue_push(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return false;
    if (!g_grey_queue_initialized) return false;

    GCGreyNode *node = (GCGreyNode*)malloc(sizeof(GCGreyNode));
    if (!node) return false;
    node->handle = handle;
    node->next = NULL;

    gc_grey_queue_lock(&g_grey_queue);
    if (g_grey_queue.shutdown) {
        gc_grey_queue_unlock(&g_grey_queue);
        free(node);
        return false;
    }
    if (g_grey_queue.tail) {
        g_grey_queue.tail->next = node;
    } else {
        g_grey_queue.head = node;
    }
    g_grey_queue.tail = node;
    atomic_fetch_add_u32((volatile uint32_t *)&g_grey_queue.count, 1);
    gc_grey_queue_unlock(&g_grey_queue);

#ifdef _WIN32
    WakeConditionVariable(&g_grey_queue.cond);
#else
    pthread_cond_signal(&g_grey_queue.cond);
#endif
    return true;
}

static bool gc_grey_queue_pop(GCHandle *out_handle) {
    gc_grey_queue_lock(&g_grey_queue);
    while (!g_grey_queue.shutdown && g_grey_queue.head == NULL) {
#ifdef _WIN32
        SleepConditionVariableCS(&g_grey_queue.cond, &g_grey_queue.mutex, INFINITE);
#else
        pthread_cond_wait(&g_grey_queue.cond, &g_grey_queue.mutex);
#endif
    }
    if (g_grey_queue.head == NULL) {
        gc_grey_queue_unlock(&g_grey_queue);
        return false;
    }
    GCGreyNode *node = g_grey_queue.head;
    g_grey_queue.head = node->next;
    if (!g_grey_queue.head) g_grey_queue.tail = NULL;
    *out_handle = node->handle;
    atomic_fetch_add_u32((volatile uint32_t *)&g_grey_queue.count, (uint32_t)-1);
    gc_grey_queue_unlock(&g_grey_queue);
    free(node);
    return true;
}

static bool gc_grey_queue_try_pop(GCHandle *out_handle) {
    gc_grey_queue_lock(&g_grey_queue);
    if (g_grey_queue.head == NULL) {
        gc_grey_queue_unlock(&g_grey_queue);
        return false;
    }
    GCGreyNode *node = g_grey_queue.head;
    g_grey_queue.head = node->next;
    if (!g_grey_queue.head) g_grey_queue.tail = NULL;
    *out_handle = node->handle;
    atomic_fetch_add_u32((volatile uint32_t *)&g_grey_queue.count, (uint32_t)-1);
    gc_grey_queue_unlock(&g_grey_queue);
    free(node);
    return true;
}

static void gc_grey_queue_drain(void) {
    gc_grey_queue_lock(&g_grey_queue);
    g_grey_queue.shutdown = true;
    gc_grey_queue_unlock(&g_grey_queue);
#ifdef _WIN32
    WakeAllConditionVariable(&g_grey_queue.cond);
#else
    pthread_cond_broadcast(&g_grey_queue.cond);
#endif
}

/* ============================================================================
 * WRITE BARRIER
 * ============================================================================
 *
 * Dijkstra-style barrier: when the mutator writes a reference to a white
 * object into a black object, shade the target grey immediately. This is safe
 * even without a mutator read barrier.
 */

void gc_write_barrier(GCHandle source, GCHandle target) {
    if (source == GC_HANDLE_NULL || target == GC_HANDLE_NULL) return;
    if (atomic_load_u32(&g_gc.gc_phase) != (uint32_t)GC_PHASE_MARKING) return;

    GCHeader *src_hdr = gc_header_from_handle(source);
    if (!src_hdr) return;
    uint32_t src_color = atomic_load_u32(&src_hdr->gc_color_state);
    if (src_color != GC_COLOR_BLACK) return;

    GCHeader *tgt_hdr = gc_header_from_handle(target);
    if (!tgt_hdr) return;

    uint32_t old_color = atomic_load_u32(&tgt_hdr->gc_color_state);
    if (old_color != GC_COLOR_WHITE) return;

    uint32_t swapped = atomic_compare_exchange_u32(&tgt_hdr->gc_color_state,
                                                    GC_COLOR_WHITE, GC_COLOR_GREY);
    if (swapped == GC_COLOR_WHITE) {
        gc_grey_queue_push(target);
    }
}

bool gc_ptr_in_heap(void *ptr) {
    if (!ptr) return false;
    uint32_t idx = gc_active_buffer_index();
    GCBuffer *buf = &g_gc.buffers[idx];
    uint8_t *start = buf->storage;
    uint8_t *end = start + buf->bump_offset;
    return (uint8_t *)ptr >= start && (uint8_t *)ptr < end;
}

/* ============================================================================
 * GC INITIALIZATION & CLEANUP
 * ============================================================================
 */

static bool gc_buffer_init(GCBuffer *buf, size_t storage_size, uint32_t handle_capacity) {
    buf->storage = (uint8_t*)malloc(storage_size);
    if (!buf->storage) return false;
    buf->storage_size = storage_size;
    buf->bump_offset = 0;
    
    buf->handles = (void**)malloc(handle_capacity * sizeof(void*));
    if (!buf->handles) {
        free(buf->storage);
        buf->storage = NULL;
        return false;
    }
    memset(buf->handles, 0, handle_capacity * sizeof(void*));
    buf->handle_capacity = handle_capacity;
    buf->handle_count = 1;  /* Handle 0 is reserved as GC_HANDLE_NULL */
    
    return true;
}

static void gc_buffer_cleanup(GCBuffer *buf) {
    if (buf->storage) {
        free(buf->storage);
        buf->storage = NULL;
    }
    buf->storage_size = 0;
    buf->bump_offset = 0;
    
    if (buf->handles) {
        free(buf->handles);
        buf->handles = NULL;
    }
    buf->handle_capacity = 0;
    buf->handle_count = 0;
}

bool gc_init(void) {
    if (g_gc.initialized) return true;
    
    /* Initialize both buffers */
    if (!gc_buffer_init(&g_gc.buffers[0], GC_HEAP_SIZE, GC_INITIAL_HANDLES)) {
        return false;
    }
    if (!gc_buffer_init(&g_gc.buffers[1], GC_HEAP_SIZE, GC_INITIAL_HANDLES)) {
        gc_buffer_cleanup(&g_gc.buffers[0]);
        return false;
    }
    
    /* Buffer 0 is active initially */
    g_gc.active_handle_table = g_gc.buffers[0].handles;
    
    /* Initialize lock-free free-list next-links */
    g_gc.free_next = (uint32_t*)malloc(GC_INITIAL_HANDLES * sizeof(uint32_t));
    if (!g_gc.free_next) {
        gc_buffer_cleanup(&g_gc.buffers[0]);
        gc_buffer_cleanup(&g_gc.buffers[1]);
        return false;
    }
    g_gc.free_head = GC_HANDLE_NULL;
    g_gc.free_next_capacity = GC_INITIAL_HANDLES;
    g_gc.free_count = 0;
    
    /* Initialize per-handle publication state */
    g_gc.publish_state = (uint32_t*)calloc(GC_INITIAL_HANDLES, sizeof(uint32_t));
    if (!g_gc.publish_state) {
        free(g_gc.free_next);
        g_gc.free_next = NULL;
        gc_buffer_cleanup(&g_gc.buffers[0]);
        gc_buffer_cleanup(&g_gc.buffers[1]);
        return false;
    }
    g_gc.publish_state_capacity = GC_INITIAL_HANDLES;
    
    /* Initialize root set */
    g_gc.root_set.capacity = 256;
    g_gc.root_set.roots = (GCHandle*)malloc(g_gc.root_set.capacity * sizeof(GCHandle));
    if (!g_gc.root_set.roots) {
        free(g_gc.free_next);
        g_gc.free_next = NULL;
        gc_buffer_cleanup(&g_gc.buffers[0]);
        gc_buffer_cleanup(&g_gc.buffers[1]);
        return false;
    }
    g_gc.root_set.count = 0;
    
    /* Initialize typed handle arrays */
    gc_handle_array_init(&g_gc.weakmap_handles, 100);
    gc_handle_array_init(&g_gc.weakref_handles, 100);
    gc_handle_array_init(&g_gc.finrec_handles, 100);
    gc_handle_array_init(&g_gc.atom_handles, 1000);
    
    /* Initialize type buckets */
    for (int i = 0; i < GC_BUCKET_COUNT; i++) {
        gc_type_bucket_init(&g_gc.type_buckets[i]);
    }
    
    atomic_store_u32(&g_gc.gc_phase, (uint32_t)GC_PHASE_IDLE);
    atomic_store_u32(&g_gc.compaction_target, (uint32_t)0);
    g_gc.bytes_allocated = 0;
    g_gc.gc_threshold = GC_DEFAULT_THRESHOLD;
    g_gc.rt = GC_HANDLE_NULL;
    
    /* Initialize backward-compatible handles shim */
    g_gc.handles.ptrs = g_gc.buffers[0].handles;
    g_gc.handles.count = g_gc.buffers[0].handle_count;
    g_gc.handles.capacity = g_gc.buffers[0].handle_capacity;
    g_gc.handles.free_list = NULL;   /* legacy array replaced by lock-free stack */
    g_gc.handles.free_count = 0;
    g_gc.handles.free_capacity = g_gc.free_next_capacity;
    
    g_gc.initialized = true;
    
    /* Start background GC thread / thread pool */
    gc_thread_start();
    
    /* Initialize the grey work queue used by concurrent marking */
    if (!gc_grey_queue_init()) {
        gc_thread_stop();
        /* Cleanup already-initialized state */
        gc_cleanup();
        return false;
    }
    
    return true;
}

bool gc_is_initialized(void) {
    return g_gc.initialized;
}

void gc_cleanup(void) {
    /* Stop background GC thread first */
    gc_thread_stop();
    
    /* Free the grey work queue */
    gc_grey_queue_cleanup();
    
    /* Free typed handle arrays */
    gc_handle_array_free(&g_gc.weakmap_handles);
    gc_handle_array_free(&g_gc.weakref_handles);
    gc_handle_array_free(&g_gc.finrec_handles);
    gc_handle_array_free(&g_gc.atom_handles);
    
    /* Free type buckets */
    for (int i = 0; i < GC_BUCKET_COUNT; i++) {
        gc_type_bucket_free(&g_gc.type_buckets[i]);
    }
    
    /* Free root set array */
    if (g_gc.root_set.roots) {
        free(g_gc.root_set.roots);
        g_gc.root_set.roots = NULL;
    }
    
    /* Free handle free list next-links */
    if (g_gc.free_next) {
        free(g_gc.free_next);
        g_gc.free_next = NULL;
    }
    
    /* Free publication state array */
    if (g_gc.publish_state) {
        free(g_gc.publish_state);
        g_gc.publish_state = NULL;
        g_gc.publish_state_capacity = 0;
    }
    
    /* Free both buffers */
    gc_buffer_cleanup(&g_gc.buffers[0]);
    gc_buffer_cleanup(&g_gc.buffers[1]);
    
    memset(&g_gc, 0, sizeof(g_gc));
}

void gc_set_runtime(JSRuntimeHandle rt) {
    g_gc.rt = rt.handle();
}

JSRuntimeHandle gc_get_runtime(void) {
    return JSRuntimeHandle(g_gc.rt);
}

void gc_set_handle_finalizer(GCHandle handle, GCFinalizerFunc *finalizer) {
    if (handle == GC_HANDLE_NULL || handle >= gc_active_handle_count()) return;
    
    void *ptr = gc_active_handles()[handle];
    if (!ptr) return;
    
    GCHeader *hdr = gc_header(ptr);
    if (!hdr) return;
    
    hdr->finalizer = finalizer;
}

GCFinalizerFunc *gc_get_handle_finalizer(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= gc_active_handle_count()) return NULL;
    
    void *ptr = gc_active_handles()[handle];
    if (!ptr) return NULL;
    
    GCHeader *hdr = gc_header(ptr);
    if (!hdr) return NULL;
    
    return hdr->finalizer;
}

/* ============================================================================
 * HANDLE ARRAY MANAGEMENT
 * ============================================================================ */

int gc_handle_array_init(JSHandleArray *arr, uint32_t capacity) {
    if (!arr) return -1;
    arr->handles = (GCHandle*)malloc(capacity * sizeof(GCHandle));
    if (!arr->handles) return -1;
    memset(arr->handles, 0, capacity * sizeof(GCHandle));
    arr->count = 0;
    arr->capacity = capacity;
    return 0;
}

void gc_handle_array_free(JSHandleArray *arr) {
    if (!arr) return;
    if (arr->handles) {
        free(arr->handles);
        arr->handles = NULL;
    }
    arr->count = 0;
    arr->capacity = 0;
}

int gc_handle_array_add(JSHandleArray *arr, GCHandle handle) {
    if (!arr || handle == GC_HANDLE_NULL) return -1;
    if (arr->count >= arr->capacity) {
        /* Grow array */
        uint32_t new_capacity = arr->capacity * 2;
        GCHandle *new_handles = (GCHandle*)realloc(arr->handles, new_capacity * sizeof(GCHandle));
        if (!new_handles) return -1;
        arr->handles = new_handles;
        arr->capacity = new_capacity;
    }
    arr->handles[arr->count++] = handle;
    return 0;
}

int gc_handle_array_push_with_index(JSHandleArray *arr, GCHandle handle, uint32_t *out_index) {
    if (!arr || handle == GC_HANDLE_NULL) return -1;
    
    /* Grow array if full */
    if (arr->count >= arr->capacity) {
        uint32_t new_capacity = arr->capacity * 2;
        if (new_capacity < arr->capacity) {
            GC_LOGE("gc_handle_array_push_with_index: capacity overflow, cannot grow");
            return -1;
        }
        GC_LOGI("gc_handle_array_push_with_index: growing array from %u to %u", arr->capacity, new_capacity);
        GCHandle *new_handles = (GCHandle*)realloc(arr->handles, new_capacity * sizeof(GCHandle));
        if (!new_handles) {
            GC_LOGE("gc_handle_array_push_with_index: realloc failed, out of memory");
            return -1;
        }
        /* Zero-initialize new slots */
        memset(new_handles + arr->capacity, 0, (new_capacity - arr->capacity) * sizeof(GCHandle));
        arr->handles = new_handles;
        arr->capacity = new_capacity;
        GC_LOGI("gc_handle_array_push_with_index: array grown to capacity=%u", arr->capacity);
    }
    
    uint32_t index = arr->count++;  /* 0-based array index */
    arr->handles[index] = handle;   /* Store unified GC handle */
    
    GC_LOGI("gc_handle_array_push_with_index: arr=%p arr->handles=%p added handle=%u (index=%u)", 
            (void*)arr, (void*)arr->handles, handle, index);
    
    if (out_index) {
        *out_index = index;
    }
    return 0;
}

void gc_handle_array_remove(JSHandleArray *arr, GCHandle handle) {
    if (!arr || handle == GC_HANDLE_NULL) return;
    for (uint32_t i = 0; i < arr->count; i++) {
        if (arr->handles[i] == handle) {
            /* Mark as freed (compaction will remove it) */
            arr->handles[i] = GC_HANDLE_FREED;
            return;
        }
    }
}

void gc_handle_array_compact(JSHandleArray *arr) {
    if (!arr) return;
    uint32_t j = 0;
    for (uint32_t i = 0; i < arr->count; i++) {
        GCHandle handle = arr->handles[i];
        if (handle == GC_HANDLE_NULL || handle == GC_HANDLE_FREED) {
            continue;  /* Skip freed/invalid entries */
        }
        /* Check if object still exists (size != 0) */
        void *ptr = gc_deref(handle);
        if (ptr) {
            GCHeader *hdr = gc_header(ptr);
            if (hdr && hdr->size != 0) {
                arr->handles[j++] = handle;
            }
        }
    }
    arr->count = j;
}

/* ============================================================================
 * TYPE BUCKET MANAGEMENT
 * ============================================================================
 *
 * Type buckets are append-only arrays that are lock-free for inserts.
 * - `count` and `capacity` are atomic.
 * - Growth uses CAS on the handles pointer; the old array is intentionally
 *   leaked until a quiescence/retirement mechanism is added.
 * - Compaction is only called during stop-the-world GC.
 * - Scans count/live-check entries on demand so they remain correct outside GC.
 */

static inline bool gc_handle_is_alive_in_bucket(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle == (GCHandle)(uintptr_t)-1) return false;
    void *ptr = gc_deref(handle);
    if (!ptr) return false;
    GCHeader *hdr = gc_header(ptr);
    return hdr && hdr->size != 0;
}

void gc_type_bucket_init(GCTypeBucket *bucket) {
    if (!bucket) return;
    bucket->handles = NULL;
    bucket->count = 0;
    bucket->capacity = 0;
    bucket->version = 0;
}

void gc_type_bucket_free(GCTypeBucket *bucket) {
    if (!bucket) return;
    if (bucket->handles) {
        free(bucket->handles);
        bucket->handles = NULL;
    }
    bucket->count = 0;
    bucket->capacity = 0;
    bucket->version = 0;
}

int gc_type_bucket_add(GCTypeBucket *bucket, GCHandle handle) {
    if (!bucket || handle == GC_HANDLE_NULL) return -1;
    
    for (;;) {
        uint32_t cap = atomic_load_u32(&bucket->capacity);
        uint32_t cnt = atomic_load_u32(&bucket->count);
        
        if (cnt < cap) {
            uint32_t idx = atomic_fetch_add_u32(&bucket->count, 1);
            if (idx < cap) {
                bucket->handles[idx] = handle;
                atomic_fetch_add_u32(&bucket->version, 1);
                return 0;
            }
            /* Another thread grew the table; retry with new capacity/count. */
            continue;
        }
        
        /* Need to grow the array. */
        uint32_t new_cap = cap == 0 ? 64 : cap * 2;
        GCHandle *new_handles = (GCHandle*)malloc(new_cap * sizeof(GCHandle));
        if (!new_handles) return -1;
        if (cap > 0 && bucket->handles) {
            memcpy(new_handles, bucket->handles, cap * sizeof(GCHandle));
        }
        memset(new_handles + cap, 0, (new_cap - cap) * sizeof(GCHandle));
        
        /* Attempt to install the new array. */
        GCHandle *old_handles = (GCHandle *)atomic_load_ptr((void *volatile *)&bucket->handles);
        void *swapped = atomic_compare_exchange_ptr((void *volatile *)&bucket->handles,
                                                    old_handles, new_handles);
        if (swapped == old_handles) {
            atomic_store_u32(&bucket->capacity, new_cap);
            /* Old array is intentionally not freed here to avoid racing
             * with readers that loaded the old pointer before the CAS. */
        } else {
            free(new_handles);
        }
        /* Retry append with (hopefully) larger capacity. */
    }
}

void gc_type_bucket_remove(GCTypeBucket *bucket, GCHandle handle) {
    if (!bucket || handle == GC_HANDLE_NULL) return;
    
    uint32_t n = atomic_load_u32(&bucket->count);
    for (uint32_t i = 0; i < n; i++) {
        if (bucket->handles[i] == handle) {
            /* Mark as removed (compaction will clean up) */
            bucket->handles[i] = (GCHandle)(uintptr_t)-1;
            atomic_fetch_add_u32(&bucket->version, 1);
            return;
        }
    }
}

void gc_type_bucket_compact(GCTypeBucket *bucket) {
    if (!bucket) return;
    
    /* Compaction is only called during stop-the-world GC, so no lock is needed. */
    uint32_t j = 0;
    uint32_t n = atomic_load_u32(&bucket->count);
    for (uint32_t i = 0; i < n; i++) {
        GCHandle handle = bucket->handles[i];
        if (gc_handle_is_alive_in_bucket(handle)) {
            bucket->handles[j++] = handle;
        }
    }
    atomic_store_u32(&bucket->count, j);
    atomic_fetch_add_u32(&bucket->version, 1);
}

void gc_for_each_object_of_type(JSGCObjectTypeEnum type, GCTypeIteratorFunc func, void *user_data) {
    if (!func) return;
    
    GCObjectBucketType bucket_type = gc_type_to_bucket(type);
    GCTypeBucket *bucket = &g_gc.type_buckets[bucket_type];
    
    uint32_t n = atomic_load_u32(&bucket->count);
    for (uint32_t i = 0; i < n; i++) {
        GCHandle handle = bucket->handles[i];
        if (gc_handle_is_alive_in_bucket(handle)) {
            func(handle, user_data);
        }
    }
}

uint32_t gc_count_objects_of_type(JSGCObjectTypeEnum type) {
    GCObjectBucketType bucket_type = gc_type_to_bucket(type);
    GCTypeBucket *bucket = &g_gc.type_buckets[bucket_type];
    
    uint32_t count = 0;
    uint32_t n = atomic_load_u32(&bucket->count);
    for (uint32_t i = 0; i < n; i++) {
        GCHandle handle = bucket->handles[i];
        if (gc_handle_is_alive_in_bucket(handle)) {
            count++;
        }
    }
    return count;
}

GCTypeIterator gc_type_iterator_begin(JSGCObjectTypeEnum type) {
    GCTypeIterator it = {0};
    GCObjectBucketType bucket_type = gc_type_to_bucket(type);
    it.bucket = &g_gc.type_buckets[bucket_type];
    it.index = 0;
    it.version = atomic_load_u32(&it.bucket->version);
    
    /* Skip to first valid entry */
    uint32_t n = atomic_load_u32(&it.bucket->count);
    while (it.index < n) {
        GCHandle handle = it.bucket->handles[it.index];
        if (gc_handle_is_alive_in_bucket(handle)) {
            break;
        }
        it.index++;
    }
    
    return it;
}

GCTypeIterator gc_type_iterator_begin_for_bucket(GCTypeBucket *bucket) {
    GCTypeIterator it = {0};
    it.bucket = bucket;
    it.index = 0;
    it.version = atomic_load_u32(&bucket->version);
    
    uint32_t n = atomic_load_u32(&bucket->count);
    while (it.index < n) {
        GCHandle handle = bucket->handles[it.index];
        if (handle != GC_HANDLE_NULL && handle != (GCHandle)(uintptr_t)-1) {
            break;
        }
        it.index++;
    }
    
    return it;
}

bool gc_type_iterator_valid(GCTypeIterator *it) {
    if (!it || !it->bucket) return false;
    /* Check if bucket was modified during iteration */
    if (it->version != atomic_load_u32(&it->bucket->version)) return false;
    return it->index < atomic_load_u32(&it->bucket->count);
}

void gc_type_iterator_next(GCTypeIterator *it) {
    if (!it || !it->bucket) return;
    
    it->index++;
    uint32_t n = atomic_load_u32(&it->bucket->count);
    while (it->index < n) {
        GCHandle handle = it->bucket->handles[it->index];
        if (gc_handle_is_alive_in_bucket(handle)) {
            break;
        }
        it->index++;
    }
}

GCHandle gc_type_iterator_handle(GCTypeIterator *it) {
    if (!it || !it->bucket) return GC_HANDLE_NULL;
    uint32_t n = atomic_load_u32(&it->bucket->count);
    if (it->index >= n) return GC_HANDLE_NULL;
    return it->bucket->handles[it->index];
}

/* ============================================================================
 * BUMP ALLOCATOR & HANDLE TABLE - Double-Buffered
 * ============================================================================
 */

/*
 * CANARY-ENABLED BUMP ALLOCATOR
 * 
 * Layout: [16-byte prefix] [64-byte GCHeader] [user data] [16-byte suffix]
 * Total overhead: 32 bytes per allocation
 * 
 * During normal operation (non-compaction): allocates from active buffer.
 * During compaction: allocates from compaction_target buffer.
 */
static void *bump_alloc(size_t size) {
    GCBuffer *buf = gc_active_buffer();
    
    /* During compaction, allocate from the compaction target buffer */
    if (atomic_load_u32(&g_gc.gc_phase) == (uint32_t)GC_PHASE_COMPACTING) {
        uint32_t target = atomic_load_u32(&g_gc.compaction_target);
        buf = &g_gc.buffers[target];
    }
    
    /* Add space for prefix and suffix canaries.
     * Prefix is 16 bytes: 8-byte canary + 8-byte padding so that the user
     * pointer (header + 64) is 16-byte aligned while objects stay contiguous
     * on 16-byte boundaries.  Suffix is 16 bytes for the same alignment. */
    size_t user_size = ALIGN16(size);
    size_t total_size = 16 + sizeof(GCHeader) + user_size + 16; /* canaries + header + data */
    
    /* Atomic bump allocation using fetch_add */
    size_t old_offset = atomic_fetch_add_zu(&buf->bump_offset, total_size);
    /* CRITICAL: Ensure offset is 16-byte aligned before allocation */
    size_t aligned_offset = ALIGN16(old_offset);
    /* Header starts 16 bytes after the prefix canary */
    size_t alloc_start = aligned_offset + 16;
    size_t new_offset = aligned_offset + total_size;
    if (new_offset > buf->storage_size) return NULL;
    
    /* Set up canaries and header */
    uint8_t *prefix_ptr = buf->storage + aligned_offset;
    uint8_t *hdr_ptr = buf->storage + alloc_start;
    GCHeader *hdr = (GCHeader*)hdr_ptr;
    uint8_t *user_ptr = hdr_ptr + sizeof(GCHeader);
    uint8_t *suffix_ptr = user_ptr + user_size;
    
    /* Set prefix canary */
    *(uint64_t*)prefix_ptr = GC_CANARY_PREFIX;
    
    /* Initialize header */
    hdr->gc_obj_type = 0;
    hdr->gc_color_state = GC_COLOR_WHITE;
    hdr->flags = 0;
    memset(hdr->pad, 0, sizeof(hdr->pad));
    hdr->link.next = NULL;
    hdr->link.prev = NULL;
    hdr->handle = GC_HANDLE_NULL;
    hdr->size = user_size;  /* Store USER size, not total size */
    hdr->flags = 0;
    memset(hdr->pad, 0, sizeof(hdr->pad));
    hdr->finalizer = NULL;  /* No finalizer by default */
    hdr->reserved1 = 0;
    hdr->reserved2 = 0;
    
    /* Clear user data */
    memset(user_ptr, 0, user_size);
    
    /* Set suffix canary */
    *(uint64_t*)suffix_ptr = GC_CANARY_SUFFIX;
    
    return user_ptr;
}

/* Grow both handle tables when needed */
static bool grow_handle_tables(void) {
    uint32_t new_capacity = gc_active_buffer()->handle_capacity * 2;
    if (new_capacity < 1000) new_capacity = 1000;
    
    /* Grow both buffers' handle tables together */
    for (int i = 0; i < 2; i++) {
        void **new_ptrs = (void**)realloc(g_gc.buffers[i].handles, new_capacity * sizeof(void*));
        if (!new_ptrs) {
            fprintf(stderr, "[FATAL] Failed to grow handle table %d to %u entries\n", i, new_capacity);
            return false;
        }
        
        /* Zero out the new slots */
        memset(&new_ptrs[g_gc.buffers[i].handle_capacity], 0, 
               (new_capacity - g_gc.buffers[i].handle_capacity) * sizeof(void*));
        
        g_gc.buffers[i].handles = new_ptrs;
        g_gc.buffers[i].handle_capacity = new_capacity;
    }
    
    /* Update active_handle_table pointer in case realloc moved it */
    g_gc.active_handle_table = g_gc.buffers[gc_active_buffer_index()].handles;
    
    /* Grow the publication-state array in lock-step with handle tables */
    if (new_capacity > g_gc.publish_state_capacity) {
        uint32_t *new_pub = (uint32_t*)realloc(g_gc.publish_state, new_capacity * sizeof(uint32_t));
        if (!new_pub) {
            fprintf(stderr, "[FATAL] Failed to grow publish state to %u entries\n", new_capacity);
            return false;
        }
        memset(&new_pub[g_gc.publish_state_capacity], 0,
               (new_capacity - g_gc.publish_state_capacity) * sizeof(uint32_t));
        g_gc.publish_state = new_pub;
        g_gc.publish_state_capacity = new_capacity;
    }
    
    /* Grow the free-list next-link array in lock-step with handle tables */
    if (new_capacity > g_gc.free_next_capacity) {
        uint32_t *new_next = (uint32_t*)realloc(g_gc.free_next, new_capacity * sizeof(uint32_t));
        if (!new_next) {
            fprintf(stderr, "[FATAL] Failed to grow free next-links to %u entries\n", new_capacity);
            return false;
        }
        g_gc.free_next = new_next;
        g_gc.free_next_capacity = new_capacity;
    }
    
    GC_LOGI("Grew handle tables to %u entries", new_capacity);
    return true;
}

/*
 * push_free_handle - Lock-free push onto the handle free list (Treiber stack).
 *
 * free_next[handle] stores the previous head.  We CAS free_head until our
 * snapshot matches.  free_next is grown in lock-step with handle tables, so
 * capacity is always sufficient.
 */
static inline void push_free_handle(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.free_next_capacity) return;
    
    uint32_t old_head = atomic_load_u32(&g_gc.free_head);
    do {
        g_gc.free_next[handle] = old_head;
    } while (atomic_compare_exchange_u32(&g_gc.free_head, old_head, handle) != old_head);
    
    atomic_fetch_add_u32(&g_gc.free_count, 1);
}

/*
 * write_handle_pointer - Write a pointer into the active handle table.
 * During compaction, also writes to the compaction target handle table.
 */
static void write_handle_pointer(GCHandle handle, void *ptr) {
    GCBuffer *active = gc_active_buffer();
    active->handles[handle] = ptr;
    
    /* During compaction, also write to compaction target */
    if (atomic_load_u32(&g_gc.gc_phase) == (uint32_t)GC_PHASE_COMPACTING) {
        uint32_t target = atomic_load_u32(&g_gc.compaction_target);
        g_gc.buffers[target].handles[handle] = ptr;
    }
}

/*
 * allocate_handle_slot - Atomically reserve a handle index (no pointer written).
 * 
 * Lock-free: first tries to pop from the free list, otherwise atomically
 * increments handle_count to reserve a new slot. The caller must later write
 * the object pointer with write_handle_pointer().
 */
static GCHandle allocate_handle_slot(void) {
    /* Fast path: lock-free pop from the handle free list (Treiber stack). */
    uint32_t old_head = atomic_load_u32(&g_gc.free_head);
    while (old_head != GC_HANDLE_NULL) {
        uint32_t new_head = g_gc.free_next[old_head];
        uint32_t swapped = atomic_compare_exchange_u32(&g_gc.free_head, old_head, new_head);
        if (swapped == old_head) {
            GCHandle handle = old_head;
            if (handle < g_gc.publish_state_capacity) {
                atomic_store_u32(&g_gc.publish_state[handle], (uint32_t)PUBLISH_UNBORN);
            }
            atomic_fetch_sub_u32(&g_gc.free_count, 1);
            return handle;
        }
        old_head = swapped;
    }
    
    /* Need to allocate a new slot - atomically increment handle_count */
    GCBuffer *active = gc_active_buffer();
    if (active->handle_count >= active->handle_capacity) {
        if (!grow_handle_tables()) {
            fprintf(stderr, "[FATAL] Out of handles (count=%u, capacity=%u) - grow failed\n", 
                    active->handle_count, active->handle_capacity);
            return GC_HANDLE_NULL;
        }
        GC_LOGI("Grew handle tables, now capacity=%u", active->handle_capacity);
    }
    
    GCHandle handle = atomic_fetch_add_u32(&active->handle_count, 1);
    
    /* Also ensure the other buffer's handle_count stays in sync */
    GCBuffer *other = gc_inactive_buffer();
    if (other->handle_count < handle + 1) {
        other->handle_count = handle + 1;
    }
    
    /* New slots start as UNBORN until the allocation publishes them */
    if (handle < g_gc.publish_state_capacity) {
        atomic_store_u32(&g_gc.publish_state[handle], (uint32_t)PUBLISH_UNBORN);
    }
    
    return handle;
}

bool gc_ptr_is_valid(void *ptr) {
    if (!ptr) return false;
    if (!g_gc.initialized) return false;
    
    /* Check if pointer is within either buffer's storage range */
    for (int i = 0; i < 2; i++) {
        uint8_t *storage = g_gc.buffers[i].storage;
        size_t size = g_gc.buffers[i].storage_size;
        if (storage && (uint8_t*)ptr >= storage && (uint8_t*)ptr < storage + size)
            return true;
    }
    return false;
}

GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    return gc_alloc_ex(size, gc_obj_type, GC_HANDLE_ARRAY_GC);
}

GCHandle gc_alloc_ex(size_t size, JSGCObjectTypeEnum gc_obj_type,
                     GCHandleArrayType array_type) {
    if (!g_gc.initialized) return GC_HANDLE_NULL;
    
    /* Step 1: Atomically reserve a handle slot first.
     * This is lock-free: either a free-list pop or an atomic increment of
     * handle_count. The pointer is written later, so handle reservation and
     * memory reservation are separate atomic steps. */
    GCHandle handle = allocate_handle_slot();
    if (handle == GC_HANDLE_NULL) {
        return GC_HANDLE_NULL;
    }
    
    /* Step 2: Atomically reserve memory from the bump pointer.
     * Because step 1 and step 2 are separate atomic operations, allocations
     * from different threads (or even sequential ones) can reserve memory
     * out of handle order. Compaction sorts live objects by handle to restore
     * a deterministic layout. */
    void *ptr = bump_alloc(size);
    if (!ptr) {
        /* Memory allocation failed - release the reserved handle slot */
        push_free_handle(handle);
        fprintf(stderr, "[FATAL] bump_alloc failed: out of memory (requested %zu bytes)\n", size);
        fprintf(stderr, "[FATAL] GC would have been triggered, but this is disabled to prevent handle corruption.\n");
        fflush(stderr);
        abort();
    }
    
    /* Step 3: Initialize header and publish the pointer in the handle table(s) */
    GCHeader *hdr = gc_header(ptr);
    hdr->gc_obj_type = gc_obj_type;
    hdr->handle = handle;
    
    write_handle_pointer(handle, ptr);
    
    g_gc.bytes_allocated += hdr->size;
    
    /* Auto-add to typed handle array based on array_type (weakrefs and atoms only) */
    JSHandleArray *target_array = NULL;
    switch (array_type) {
        case GC_HANDLE_ARRAY_WEAKREF:
            target_array = &g_gc.weakref_handles;
            break;
        case GC_HANDLE_ARRAY_ATOM:
            target_array = &g_gc.atom_handles;
            break;
        case GC_HANDLE_ARRAY_CONTEXT:
        case GC_HANDLE_ARRAY_JOB:
            /* These are now handled via type buckets and job_queue */
            break;
        case GC_HANDLE_ARRAY_GC:
        default:
            /* No special array needed */
            break;
    }
    if (target_array) {
        gc_handle_array_add(target_array, handle);
    }
    
    /* Auto-add to type bucket for fast iteration */
    GCObjectBucketType bucket_type = gc_type_to_bucket(gc_obj_type);
    if (bucket_type < GC_BUCKET_COUNT) {
        gc_type_bucket_add(&g_gc.type_buckets[bucket_type], handle);
    }
    
    /* Ordinary allocations are fully published immediately for backward
     * compatibility.  Grey-state allocations should use gc_alloc_grey()
     * and publish explicitly when construction is complete. */
    if (handle < g_gc.publish_state_capacity) {
        atomic_store_u32(&g_gc.publish_state[handle], (uint32_t)PUBLISH_BLACK);
    }
    
    return handle;
}

GCHandle gc_alloc_grey(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    GCHandle handle = gc_alloc(size, gc_obj_type);
    if (handle != GC_HANDLE_NULL && handle < g_gc.publish_state_capacity) {
        atomic_store_u32(&g_gc.publish_state[handle], (uint32_t)PUBLISH_GREY);
    }
    return handle;
}

void gc_publish_state_init(GCHandle handle, GCPublishState state) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.publish_state_capacity) return;
    atomic_store_u32(&g_gc.publish_state[handle], (uint32_t)state);
}

void gc_publish(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.publish_state_capacity) return;
    atomic_store_u32(&g_gc.publish_state[handle], (uint32_t)PUBLISH_BLACK);
}

void gc_unpublish(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.publish_state_capacity) return;
    atomic_store_u32(&g_gc.publish_state[handle], (uint32_t)PUBLISH_UNBORN);
}

GCPublishState gc_publish_state_load(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= g_gc.publish_state_capacity) return PUBLISH_UNBORN;
    return (GCPublishState)atomic_load_u32(&g_gc.publish_state[handle]);
}

bool gc_is_published(GCHandle handle) {
    return gc_publish_state_load(handle) == PUBLISH_BLACK;
}

/*
 * gc_free - Explicitly free a GC-managed object.
 * 
 * This is primarily used for testing and special cases. In normal operation,
 * objects are freed automatically by the GC when unreachable. The handle is
 * returned to the free list and the object is marked as freed.
 */
void gc_free(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return;
    
    GCBuffer *active = gc_active_buffer();
    if (handle >= active->handle_count) return;
    
    void *ptr = active->handles[handle];
    if (!ptr) return;
    
    GCHeader *hdr = gc_header(ptr);
    if (!hdr) return;
    
    /* Already freed */
    if (hdr->size & 0x80000000) return;
    
    /* Mark as freed */
    hdr->size |= 0x80000000;
    
    /* Corrupt canaries to detect use-after-free */
    gc_corrupt_canaries(hdr);
    
    /* Clear pointer in active handle table */
    active->handles[handle] = NULL;
    
    /* During compaction, also clear in compaction target */
    if (atomic_load_u32(&g_gc.gc_phase) == (uint32_t)GC_PHASE_COMPACTING) {
        uint32_t target = atomic_load_u32(&g_gc.compaction_target);
        g_gc.buffers[target].handles[handle] = NULL;
    }
    
    /* Return handle to free list */
    push_free_handle(handle);
}

/*
 * gc_realloc - Reallocate memory while preserving the original handle.
 * 
 * This function allocates new memory, copies data from the old location,
 * and updates the original handle to point to the new memory. The old
 * memory is marked as freed and will be reclaimed by the GC.
 * 
 * Returns the original handle (now pointing to new memory), or GC_HANDLE_NULL on failure.
 */
GCHandle gc_realloc(GCHandle handle, size_t new_size) {
    if (handle == GC_HANDLE_NULL) {
        return gc_alloc(new_size, JS_GC_OBJ_TYPE_DATA);
    }
    
    if (new_size == 0) return GC_HANDLE_NULL;
    
    void *old_ptr = gc_deref(handle);
    if (!old_ptr) return gc_alloc(new_size, JS_GC_OBJ_TYPE_DATA);
    
    GCHeader *old_hdr = gc_header(old_ptr);
    JSGCObjectTypeEnum old_type = (JSGCObjectTypeEnum)old_hdr->gc_obj_type;
    size_t old_user_size = old_hdr->size;
    
    old_hdr->size = old_user_size | 0x80000000;
    
    /* Allocate new memory (gets a temporary handle) */
    GCHandle temp_handle = gc_alloc(new_size, old_type);
    if (temp_handle == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    
    void *new_ptr = gc_deref(temp_handle);
    size_t copy_size = old_user_size < new_size ? old_user_size : new_size;
    memcpy(new_ptr, old_ptr, copy_size);
    
    /* Update the new memory's header to point to the ORIGINAL handle */
    GCHeader *new_hdr = gc_header(new_ptr);
    new_hdr->handle = handle;
    
    /* Update handle tables to point to new memory */
    /* Update active buffer */
    gc_active_buffer()->handles[handle] = new_ptr;
    
    /* During compaction, also update compaction target */
    if (atomic_load_u32(&g_gc.gc_phase) == (uint32_t)GC_PHASE_COMPACTING) {
        uint32_t target = atomic_load_u32(&g_gc.compaction_target);
        g_gc.buffers[target].handles[handle] = new_ptr;
    }
    
    /* Clear the temporary handle slot to prevent handle aliasing */
    gc_active_buffer()->handles[temp_handle] = NULL;
    push_free_handle(temp_handle);
    
    return handle;  /* Original handle, now pointing to new memory */
}

GCHandle gc_realloc2(GCHandle handle, size_t new_size, size_t *pslack) {
    GCHandle new_handle = gc_realloc(handle, new_size);
    if (pslack && new_handle != GC_HANDLE_NULL) {
        size_t usable = gc_usable_size(new_handle);
        *pslack = (usable > new_size) ? (usable - new_size) : 0;
    }
    return new_handle;
}

/*
 * gc_deref - Dereference a handle to get the object pointer.
 * 
 * Uses atomic load of active_handle_table for thread safety.
 * During compaction: after the atomic swap, this automatically returns
 * pointers from the new buffer. Before the swap, returns pointers from
 * the old buffer (which are still valid since live objects haven't moved yet).
 */
void *gc_deref(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return NULL;
    
    /* Atomic load of active handle table pointer */
    void **table = (void **)g_gc.active_handle_table;
    
    /* Get handle count from active buffer */
    uint32_t count = gc_active_buffer()->handle_count;
    if (handle >= count) return NULL;
    
    return table[handle];
}

bool gc_handle_is_valid(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return false;
    
    void *ptr = gc_deref(handle);
    if (!ptr) return false;
    GCHeader *hdr = gc_header(ptr);
    return (hdr->size & 0x7FFFFFFF) > 0;
}

JSGCObjectTypeEnum gc_handle_get_type(GCHandle handle) {
    return gc_handle_get_type_inline(handle);
}

/* ============================================================================
 * MARK PHASE
 * ============================================================================
 */

/* Forward declarations */
static void gc_mark_value(GCValue val);
static void gc_mark_object(JSObject *p);

/* Helper to mark any GC object by pointer */
static void gc_mark_ptr(void *ptr) {
    if (!ptr) return;
    GCHeader *hdr = gc_header(ptr);
    if (hdr->size == 0 || hdr->gc_color_state != GC_COLOR_WHITE) return;
    
    hdr->gc_color_state = GC_COLOR_BLACK;
    
    /* For JSVarRef, mark its value and the referenced frame (to keep parent frames alive) */
    if (hdr->gc_obj_type == JS_GC_OBJ_TYPE_VAR_REF) {
        JSVarRefHandle var_ref = JSVarRefHandle(hdr->handle);
        gc_mark_value(var_ref.get_detached_value());
        /* Mark the frame handle to keep parent frames alive for closures */
        GCHandle frame_handle = var_ref.frame_handle();
        if (frame_handle != GC_HANDLE_NULL) {
            /* Mark by setting the mark bit directly on the header */
            void *frame_ptr = gc_deref(frame_handle);
            if (frame_ptr) {
                GCHeader *frame_hdr = gc_header(frame_ptr);
                if ((frame_hdr->size & 0x7FFFFFFF) > 0 && frame_hdr->gc_color_state == GC_COLOR_WHITE) {
                    frame_hdr->gc_color_state = GC_COLOR_BLACK;
                }
            }
        }
    }
    /* Add other types as needed */
}

static void gc_mark_value(GCValue val) {
    int tag = JS_VALUE_GET_TAG(val);
    GCHandle handle = GC_VALUE_GET_HANDLE(val);
    switch(tag) {
    case JS_TAG_OBJECT:
    case JS_TAG_SYMBOL:
    case JS_TAG_STRING:
        {
            /* Use handle for safe marking - dereference only when needed */
            if (handle != GC_HANDLE_NULL) {
                void *ptr = gc_deref(handle);
                if (ptr) {
                    GCHeader *hdr = gc_header(ptr);
                    if ((hdr->size & 0x7FFFFFFF) > 0 && hdr->gc_color_state == GC_COLOR_WHITE) {
                        hdr->gc_color_state = GC_COLOR_BLACK;
                        if (tag == JS_TAG_OBJECT) {
                            gc_mark_object((JSObject*)ptr);
                        }
                        else if (tag == JS_TAG_STRING && hdr->gc_obj_type == JS_GC_OBJ_TYPE_JS_STRING_ROPE) {
                            JSStringRopeHandle rope(handle);
                            /* Mark left and right components of the rope */
                            gc_mark_value(rope.left());
                            gc_mark_value(rope.right());
                        }
                    }
                }
            }
        }
        break;
    }
}

static void gc_mark_object(JSObject *p) {
    if (!p) return;

    /* Mark the shape */
    JSShapeHandle shape = JSShapeHandle(p->shape_handle);
    if (shape.valid()) {
        GCHeader *shape_hdr = gc_header(gc_deref(shape.handle()));
        if ((shape_hdr->size & 0x7FFFFFFF) > 0) shape_hdr->gc_color_state = GC_COLOR_BLACK;
        
        /* Mark the shape's prototype handle - CRITICAL: shapes reference
         * prototype objects that must be kept alive */
        if (shape.proto_handle() != GC_HANDLE_NULL) {
            void *proto = gc_deref(shape.proto_handle());
            if (proto) {
                GCHeader *proto_hdr = gc_header(proto);
                if ((proto_hdr->size & 0x7FFFFFFF) > 0) proto_hdr->gc_color_state = GC_COLOR_BLACK;
            }
        }
    }

    /* Mark the object's prototype value */
    if (!JS_IsUndefined(p->prototype) && !JS_IsNull(p->prototype)) {
        gc_mark_value(p->prototype);
    }

    /* Mark properties */
    JSProperty *obj_props = (JSProperty*)gc_deref(p->prop_handle);
    if (shape && obj_props) {
        /* Shape properties (with flags) come right after JSShape header */
        JSShapeProperty *shape_props = (JSShapeProperty *)((uint8_t *)gc_deref(shape.handle()) + sizeof(JSShape));
        
        for (uint32_t i = 0; i < shape.prop_count(); i++) {
            JSShapeProperty *prs = &shape_props[i];
            JSProperty *pr = &obj_props[i];
            
            if (prs->atom == JS_ATOM_NULL) continue;
            
            /* Check property type using shape property flags */
            uint32_t prop_flags = prs->flags & JS_PROP_TMASK;
            if (prop_flags) {
                if (prop_flags == JS_PROP_GETSET) {
                    if (pr->u.getset.getter_handle != GC_HANDLE_NULL)
                        gc_mark_value(GC_MKHANDLE(JS_TAG_OBJECT, pr->u.getset.getter_handle));
                    if (pr->u.getset.setter_handle != GC_HANDLE_NULL)
                        gc_mark_value(GC_MKHANDLE(JS_TAG_OBJECT, pr->u.getset.setter_handle));
                } else if (prop_flags == JS_PROP_VARREF) {
                    /* Mark the var_ref if handle is non-null.
                     * CRITICAL: Don't use 'if (var_ref)' here because that calls valid()
                     * which may return false if the var_ref was previously unmarked.
                     * We need to mark it to keep it alive during this GC cycle. */
                    if (pr->u.var_ref_handle != GC_HANDLE_NULL) {
                        gc_mark_ptr(gc_deref(pr->u.var_ref_handle));
                    }
                }
                /* JS_PROP_AUTOINIT handled separately if needed */
            } else {
                /* JS_PROP_NORMAL - mark the value */
                gc_mark_value(pr->u.value);
            }
        }
    }
}

/* ============================================================================
 * UNIFIED GC BRIDGE - Integration with QuickJS comprehensive marking
 * ============================================================================ */

/* ============================================================================
 * CONCURRENT MARKING - tri-color marker with grey work queue
 * ============================================================================ */

/* Shade a white object grey and enqueue it for scanning. Called by the marker
 * when it discovers a child, and by the write barrier when the mutator stores
 * a reference into a black object. */
static void gc_shade(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return;
    GCHeader *hdr = gc_header_from_handle(handle);
    if (!hdr) return;
    if ((hdr->size & 0x7FFFFFFF) == 0) return;  /* Freed slot */

    uint32_t old_color = atomic_load_u32(&hdr->gc_color_state);
    if (old_color != GC_COLOR_WHITE) return;

    uint32_t swapped = atomic_compare_exchange_u32(&hdr->gc_color_state,
                                                    GC_COLOR_WHITE, GC_COLOR_GREY);
    if (swapped == GC_COLOR_WHITE) {
        gc_grey_queue_push(handle);
    }
}

/* Bridge mark callback used by QuickJS's mark_children(). Instead of recursing,
 * it shades children grey and lets the marker thread scan them later. */
static void gc_unified_mark_func(JSRuntimeHandle rt, GCHandle handle) {
    (void)rt;
    gc_shade(handle);
}

/* Scan one grey object: mark all its children grey, then turn it black. */
static void gc_scan_object(JSRuntimeHandle rt, GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return;
    GCHeader *hdr = gc_header_from_handle(handle);
    if (!hdr) return;
    if ((hdr->size & 0x7FFFFFFF) == 0) return;

    uint32_t color = atomic_load_u32(&hdr->gc_color_state);
    if (color != GC_COLOR_GREY) return;

    /* Ask QuickJS to visit every child reference; gc_unified_mark_func shades them. */
    mark_children(rt, handle, gc_unified_mark_func);

    atomic_store_u32(&hdr->gc_color_state, GC_COLOR_BLACK);
}

/* Clear all object colors before a new marking phase. The mutator must be
 * paused (or not yet started) during this step. */
static void gc_clear_marks(void) {
    for (int buf_idx = 0; buf_idx < 2; buf_idx++) {
        GCBuffer *buf = &g_gc.buffers[buf_idx];
        for (uint32_t i = 1; i < buf->handle_count; i++) {
            void *user_ptr = buf->handles[i];
            if (user_ptr) {
                GCHeader *hdr = gc_header(user_ptr);
                if ((hdr->size & 0x7FFFFFFF) > 0) {
                    hdr->gc_color_state = GC_COLOR_WHITE;
                }
            }
        }
    }
}

/* Helper: mark an object black immediately (used for leaf roots such as
 * permanent atoms that have no children to scan). */
static void gc_mark_black(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return;
    GCHeader *hdr = gc_header_from_handle(handle);
    if (hdr && (hdr->size & 0x7FFFFFFF) > 0) {
        atomic_store_u32(&hdr->gc_color_state, GC_COLOR_BLACK);
    }
}

/* Context marking callback: shade each context so the marker scans its roots. */
static void gc_mark_context_callback(uint32_t ctx_handle, void *user_data) {
    (void)user_data;
    gc_shade(ctx_handle);
}

/* Enqueue all roots for marking. Does not drain the queue. */
static void gc_mark_roots(JSRuntimeHandle rt) {
    /* Permanent atoms are leaf roots. */
    for (uint32_t i = 0; i < g_gc.atom_handles.count; i++) {
        GCHandle atom_handle = g_gc.atom_handles.handles[i];
        if (gc_handle_array_entry_is_valid(atom_handle)) {
            gc_mark_black(atom_handle);
        }
    }

    /* Contexts are scanned for globals, shapes, prototypes, etc. */
    JSRuntime_for_each_context(rt, gc_mark_context_callback, &rt);

    /* Current exception. */
    GCValue exc = JSRuntime_get_current_exception(rt);
    GCHandle exc_handle = GC_VALUE_GET_HANDLE(exc);
    if (exc_handle != GC_HANDLE_NULL) {
        gc_shade(exc_handle);
    }

    /* Job queue entries. */
    int job_count = JSRuntime_job_queue_count(rt);
    for (int i = 0; i < job_count; i++) {
        uint32_t job_handle = JSRuntime_job_queue_get_handle(rt, i);
        if (job_handle == GC_HANDLE_NULL) continue;

        uint32_t realm_handle = JSJobEntry_get_realm_handle(job_handle);
        if (realm_handle != GC_HANDLE_NULL) {
            gc_shade(realm_handle);
        }

        int argc = JSJobEntry_get_argc(job_handle);
        for (int j = 0; j < argc; j++) {
            GCValue arg = JSJobEntry_get_argv(job_handle, j);
            GCHandle arg_handle = GC_VALUE_GET_HANDLE(arg);
            if (arg_handle != GC_HANDLE_NULL) {
                gc_shade(arg_handle);
            }
        }
    }

    /* User-added roots. */
    for (uint32_t i = 0; i < g_gc.root_set.count; i++) {
        gc_shade(g_gc.root_set.roots[i]);
    }

    /* Runtime handle arrays. */
    uint32_t atom_array_handle = JSRuntime_get_atom_array_handle(rt);
    if (atom_array_handle != GC_HANDLE_NULL) {
        gc_shade(atom_array_handle);
    }

    /* JS stack frames. */
    JSStackFrame *sf = JSRuntime_get_current_stack_frame(rt);
    while (sf != NULL) {
        GCValue cur_func = JSStackFrame_get_cur_func(sf);
        GCHandle func_handle = GC_VALUE_GET_HANDLE(cur_func);
        if (func_handle != GC_HANDLE_NULL) {
            gc_shade(func_handle);
        }
        sf = JSStackFrame_get_prev(sf);
    }

    /* Shapes are roots for the atoms they reference. */
    for (uint32_t i = 1; i < gc_active_handle_count(); i++) {
        GCHandle handle = (GCHandle)i;
        if (!gc_handle_array_entry_is_valid(handle)) continue;
        if (gc_handle_is_freed(handle)) continue;
        if (gc_handle_get_type_inline(handle) == JS_GC_OBJ_TYPE_SHAPE) {
            gc_shade(handle);
        }
    }
}

/* Synchronous mark phase: clear colors, enqueue roots, and drain the queue on
 * the calling thread. Used when no mutator concurrency is desired. */
static void gc_mark_phase(JSRuntimeHandle rt) {
    gc_clear_marks();
    gc_mark_roots(rt);

    GCHandle handle;
    while (gc_grey_queue_pop(&handle)) {
        gc_scan_object(rt, handle);
    }
}

/* Background mark job submitted to the GC thread pool. It drains the grey
 * queue until gc_grey_queue_drain() signals completion (the handshake). */
#ifdef _WIN32
static DWORD WINAPI gc_mark_job_func(LPVOID arg) {
#else
static void *gc_mark_job_func(void *arg) {
#endif
    (void)arg;
    JSRuntimeHandle rt(g_gc.rt);

    GC_LOGI("mark job: starting");

    gc_clear_marks();
    gc_mark_roots(rt);

    /* Drain the grey queue. In this first implementation the mutator is
     * paused while the mark job runs, so an empty queue means marking is done.
     * For true concurrent marking this loop must keep spinning until the
     * mutator calls the end-of-marking handshake. */
    GCHandle handle;
    for (;;) {
        if (gc_grey_queue_try_pop(&handle)) {
            gc_scan_object(rt, handle);
        } else if (atomic_load_u32((volatile uint32_t *)&g_grey_queue.count) == 0) {
            break;
        } else {
            /* A worker may have popped the last item but not yet decremented
             * the count; yield briefly and retry. */
#ifdef _WIN32
            Sleep(0);
#else
            sched_yield();
#endif
        }
    }

    GC_LOGI("mark job: finished");
    gc_mark_signal_done();

#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Legacy gc_mark() - now redirects to the synchronous tri-color mark phase */
static void gc_mark(void) {
    if (g_gc.rt == GC_HANDLE_NULL) {
        /* No runtime set, fall back to simple root marking */
        gc_clear_marks();
        for (uint32_t i = 0; i < g_gc.root_set.count; i++) {
            GCHandle h = g_gc.root_set.roots[i];
            void **table = gc_active_handles();
            uint32_t count = gc_active_handle_count();
            if (h < count && table[h]) {
                void *ptr = table[h];
                GCHeader *hdr = gc_header(ptr);
                if ((hdr->size & 0x7FFFFFFF) > 0 && hdr->gc_color_state == GC_COLOR_WHITE) {
                    hdr->gc_color_state = GC_COLOR_BLACK;
                    JSGCObjectTypeEnum obj_type = (JSGCObjectTypeEnum)hdr->gc_obj_type;
                    if (obj_type == JS_GC_OBJ_TYPE_JS_OBJECT) {
                        gc_mark_object((JSObject*)ptr);
                    }
                }
            }
        }
        return;
    }

    JSRuntimeHandle rt(g_gc.rt);
    gc_mark_phase(rt);
}

/* ============================================================================
 * COMPACTION - Double-Buffered Design
 * ============================================================================
 *
 * The compaction phase moves live objects from the active buffer to the
 * compaction target buffer, then atomically swaps the active buffer pointer.
 *
 * Key invariants:
 * - During compaction, new allocations go to the compaction target buffer.
 * - New allocations write their handle to BOTH handle tables.
 * - After compaction completes, we atomically swap active_handle_table.
 * - No forwarding map needed: active table is updated in-place during move.
 */

/* Helper: Get user pointer from GCHeader */
static inline void *gc_header_to_ptr(GCHeader *hdr) {
    return (uint8_t*)hdr + sizeof(GCHeader);
}

/* Helper: Check if a pointer points into a specific buffer */
static inline bool gc_ptr_in_buffer(void *ptr, GCBuffer *buf) {
    if (!ptr || !buf || !buf->storage) return false;
    uint8_t *p = (uint8_t*)ptr;
    return p >= buf->storage && p < buf->storage + buf->storage_size;
}

/* Helper: Get buffer index (0 or 1) that contains this pointer, or -1 */
static inline int gc_ptr_to_buffer_index(void *ptr) {
    if (!ptr) return -1;
    for (int i = 0; i < 2; i++) {
        if (gc_ptr_in_buffer(ptr, &g_gc.buffers[i])) return i;
    }
    return -1;
}

/* Helper: Map an interior pointer to the handle of the GC object that owns it.
 * This walks the buffer layout to find the object whose payload contains ptr.
 * It is only safe when ptr is known to point into a GC-managed object payload. */
GCHandle gc_ptr_to_handle(void *ptr) {
    if (!ptr) return GC_HANDLE_NULL;
    int buf_idx = gc_ptr_to_buffer_index(ptr);
    if (buf_idx < 0) return GC_HANDLE_NULL;
    GCBuffer *buf = &g_gc.buffers[buf_idx];
    uint8_t *p = (uint8_t *)ptr;
    uint8_t *read = buf->storage;
    size_t bump = buf->bump_offset;

    while ((size_t)(read - buf->storage) < bump) {
        uint64_t *prefix_ptr = (uint64_t *)read;
        GCHeader *hdr;
        if (*prefix_ptr == GC_CANARY_PREFIX || *prefix_ptr == GC_CANARY_CORRUPTED) {
            hdr = (GCHeader *)(read + 16);
        } else {
            read += MIN_OBJECT_SIZE;
            continue;
        }

        uint32_t raw_size = hdr->size;
        if (raw_size == 0) {
            read += MIN_OBJECT_SIZE;
            continue;
        }
        bool is_freed = (raw_size & 0x80000000) != 0;
        uint32_t user_size = raw_size & 0x7FFFFFFF;
        size_t total_size = gc_alloc_total_size(hdr);

        if (!is_freed) {
            uint8_t *payload_start = (uint8_t *)gc_header_to_ptr(hdr);
            uint8_t *payload_end = payload_start + user_size;
            if (p >= payload_start && p < payload_end) {
                return hdr->handle;
            }
        }
        read += total_size;
    }
    return GC_HANDLE_NULL;
}

/* Sortable record of a live object used during compaction */
typedef struct {
    GCHandle handle;
    GCHeader *src_hdr;
    size_t total_size;
} GCLiveObject;

static int gc_compare_live_objects_by_handle(const void *a, const void *b) {
    const GCLiveObject *obj_a = (const GCLiveObject *)a;
    const GCLiveObject *obj_b = (const GCLiveObject *)b;
    if (obj_a->handle < obj_b->handle) return -1;
    if (obj_a->handle > obj_b->handle) return 1;
    return 0;
}

/*
 * gc_compact_move_objects - Move live objects from active buffer to compaction target.
 * 
 * Lock-free allocations can reserve handles and memory out of order, so we
 * collect all live objects from the source buffer, sort them by handle, and
 * copy them to the destination buffer in handle order. This restores a
 * deterministic, cache-friendly layout after compaction.
 */
static void gc_compact_move_objects(void) {
    GCBuffer *src = gc_active_buffer();
    uint32_t target_idx = atomic_load_u32(&g_gc.compaction_target);
    GCBuffer *dst = &g_gc.buffers[target_idx];
    
    GC_LOGI("gc_compact_move_objects: src=%d (bump=%zu) dst=%d (bump=%zu)",
            gc_active_buffer_index(), src->bump_offset,
            target_idx, dst->bump_offset);
    
    JSRuntimeHandle rt(g_gc.rt);
    size_t new_bytes = 0;
    int corrupted_objects = 0;
    (void)corrupted_objects;
    
    /* Pass 1: scan the source buffer, validate objects, collect live objects
     * and process dead objects (call finalizers, clear handles). */
    uint32_t live_capacity = src->handle_count > 1 ? src->handle_count : 1;
    GCLiveObject *live_objects = (GCLiveObject *)malloc(live_capacity * sizeof(GCLiveObject));
    if (!live_objects) {
        GC_LOGE("gc_compact_move_objects: failed to allocate live object array");
        return;
    }
    uint32_t live_count = 0;
    
    uint8_t *read = src->storage;
    size_t bump = src->bump_offset;
    while ((size_t)(read - src->storage) < bump) {
        uint64_t *prefix_ptr = (uint64_t*)read;
        GCHeader *hdr;
        
        if (*prefix_ptr == GC_CANARY_PREFIX || *prefix_ptr == GC_CANARY_CORRUPTED) {
            hdr = (GCHeader*)(read + 16);
        } else {
            /* Invalid prefix - skip minimum object size */
            read += MIN_OBJECT_SIZE;
            continue;
        }
        
        uint32_t raw_size = hdr->size;
        int is_freed = (raw_size & 0x80000000) != 0;
        
        if (raw_size == 0) {
            read += MIN_OBJECT_SIZE;
            continue;
        }
        
        size_t total_size = gc_alloc_total_size(hdr);
        
        if (is_freed) {
            read += total_size;
            continue;
        }
        
        GCCanaryStatus status = gc_validate_canaries_hdr(hdr, NULL);
        if (status != GC_CANARY_OK) {
            corrupted_objects++;
            read += total_size;
            continue;
        }
        
        if (hdr->gc_color_state != GC_COLOR_WHITE) {
            /* Live object - record for sorted copying */
            if (live_count < live_capacity) {
                live_objects[live_count].handle = hdr->handle;
                live_objects[live_count].src_hdr = hdr;
                live_objects[live_count].total_size = total_size;
                live_count++;
            }
        } else {
            /* Dead object - call finalizer if present */
            if (hdr->finalizer && status == GC_CANARY_OK) {
                hdr->finalizer(rt, hdr->handle, gc_header_to_ptr(hdr));
            }
            
            /* Free the handle */
            if (hdr->handle < src->handle_count) {
                src->handles[hdr->handle] = NULL;
                /* Don't push to free list here - we'll rebuild it after compaction */
            }
            
            gc_corrupt_canaries(hdr);
        }
        
        read += total_size;
    }
    
    /* Pass 2: sort live objects by handle to restore deterministic layout */
    if (live_count > 1) {
        qsort(live_objects, live_count, sizeof(GCLiveObject), gc_compare_live_objects_by_handle);
    }
    
    /* Pass 3: copy live objects to destination buffer in handle order */
    uint8_t *write = dst->storage + dst->bump_offset;
    for (uint32_t i = 0; i < live_count; i++) {
        GCHeader *hdr = live_objects[i].src_hdr;
        size_t total_size = live_objects[i].total_size;
        uint8_t *src_start = (uint8_t *)hdr - 16;  /* include prefix canary */
        
        memcpy(write, src_start, total_size);
        
        /* Update handle table to point to new location */
        GCHeader *new_hdr = (GCHeader*)(write + 16);
        if (new_hdr->handle < src->handle_count) {
            void *new_ptr = gc_header_to_ptr(new_hdr);
            
            /* Update active handle table */
            src->handles[new_hdr->handle] = new_ptr;
            
            /* Also write to destination handle table */
            dst->handles[new_hdr->handle] = new_ptr;
        }
        
        /* Clear mark bit in new location */
        new_hdr->gc_color_state = GC_COLOR_WHITE;
        new_hdr->reserved1 = 0;
        
        write += total_size;
        new_bytes += total_size;
    }
    
    free(live_objects);
    
    /* Update destination buffer bump pointer */
    dst->bump_offset = write - dst->storage;
    
    GC_LOGI("gc_compact_move_objects: DONE, moved %u objects (%zu bytes) to buffer %d, new bump=%zu",
            live_count, new_bytes, target_idx, dst->bump_offset);
}

/*
 * gc_compact_rebuild_free_list - Rebuild the free list after compaction.
 * 
 * After compaction, scan the handle table and add all NULL entries to the free list.
 */
static void gc_compact_rebuild_free_list(void) {
    /* Compaction is single-threaded; reset the lock-free stack and rebuild. */
    atomic_store_u32(&g_gc.free_head, GC_HANDLE_NULL);
    atomic_store_u32(&g_gc.free_count, 0);
    
    GCBuffer *active = gc_active_buffer();
    for (uint32_t i = 1; i < active->handle_count; i++) {
        if (active->handles[i] == NULL) {
            push_free_handle(i);
        }
    }
    
    GC_LOGI("gc_compact_rebuild_free_list: rebuilt with %u free handles", atomic_load_u32(&g_gc.free_count));
}

/*
 * gc_compact - Full compaction cycle.
 * 
 * Phase 1: Move live objects from active buffer to compaction target.
 * Phase 2: Rebuild free list.
 * Phase 3: Atomically swap active buffer.
 * Phase 4: Reset old buffer for next cycle.
 */
static void gc_compact(void) {
    GC_LOGI("gc_compact: ENTER");
    
    /* Phase 1: Move objects */
    gc_compact_move_objects();
    
    /* Phase 2: Rebuild free list */
    gc_compact_rebuild_free_list();
    
    /* Phase 3: Atomically swap active buffer */
    uint32_t old_active = gc_active_buffer_index();
    uint32_t new_active = atomic_load_u32(&g_gc.compaction_target);
    
    GC_LOGI("gc_compact: swapping active buffer %d -> %d", old_active, new_active);
    
    g_gc.active_handle_table = g_gc.buffers[new_active].handles;
    g_gc.active_buffer_index = new_active;
    
    /* Update backward-compatible handles shim */
    g_gc.handles.ptrs = g_gc.buffers[new_active].handles;
    g_gc.handles.count = g_gc.buffers[new_active].handle_count;
    g_gc.handles.capacity = g_gc.buffers[new_active].handle_capacity;
    g_gc.handles.free_list = NULL;
    g_gc.handles.free_count = atomic_load_u32(&g_gc.free_count);
    g_gc.handles.free_capacity = g_gc.free_next_capacity;
    
    /* Phase 4: Reset old buffer for next compaction cycle */
    GCBuffer *old_buf = &g_gc.buffers[old_active];
    old_buf->bump_offset = 0;
    /* Zero out handles in old buffer (they're now in the new buffer) */
    memset(old_buf->handles, 0, old_buf->handle_capacity * sizeof(void*));
    /* Keep handle_count the same - the free list tracks available slots */
    old_buf->handle_count = g_gc.buffers[new_active].handle_count;
    
    /* Update bytes_allocated */
    g_gc.bytes_allocated = 0;
    GCBuffer *new_buf = gc_active_buffer();
    uint8_t *scan = new_buf->storage;
    while ((size_t)(scan - new_buf->storage) < new_buf->bump_offset) {
        uint64_t *prefix = (uint64_t*)scan;
        if (*prefix == GC_CANARY_PREFIX || *prefix == GC_CANARY_CORRUPTED) {
            GCHeader *hdr = (GCHeader*)(scan + 16);
            if (hdr->size != 0 && (hdr->size & 0x80000000) == 0) {
                g_gc.bytes_allocated += hdr->size;
            }
            scan += gc_alloc_total_size(hdr);
        } else {
            scan += MIN_OBJECT_SIZE;
        }
    }
    
    GC_LOGI("gc_compact: DONE, new active buffer=%d, bytes_allocated=%zu",
            new_active, g_gc.bytes_allocated);
}

/* ============================================================================
 * SWEEP PHASE - Free unmarked objects
 * ============================================================================
 */

static void gc_sweep_unified(JSRuntimeHandle rt) {
    GC_LOGI("gc_sweep_unified: ENTER, handles.count=%u", gc_active_handle_count());
    
    void **table = gc_active_handles();
    uint32_t count = gc_active_handle_count();
    
    for (uint32_t i = 1; i < count; i++) {
        GCHandle handle = (GCHandle)i;
        void *user_ptr = table[i];
        
        if (!user_ptr) continue;
        
        /* Safety check: user_ptr must point to valid memory in GC heap */
        if (!gc_ptr_is_valid(user_ptr)) {
            GC_LOGW("gc_sweep_unified: entry %d user_ptr=%p not in GC heap, clearing", i, user_ptr);
            table[i] = NULL;
            push_free_handle((GCHandle)i);
            continue;
        }
        
        GCHeader *hdr = gc_header(user_ptr);
        
        /* Check if already freed */
        if (hdr->size == 0) {
            table[i] = NULL;
            push_free_handle((GCHandle)i);
            continue;
        }
        
        /* Check if marked */
        if (hdr->gc_color_state == GC_COLOR_WHITE) {
            /* Object is unreachable - call finalizer */
            if (hdr->finalizer) {
                hdr->finalizer(rt, handle, user_ptr);
            }
            /* Note: Type bucket removal skipped - buckets are unused and
             * removing during sweep is O(n^2) which causes hangs with
             * large numbers of objects. Compaction handles stale entries. */
            /* Mark as freed - set FREED flag but keep size for compaction */
            hdr->size |= 0x80000000;  /* Set FREED flag */
            gc_corrupt_canaries(hdr);
            table[i] = NULL;
            push_free_handle((GCHandle)i);
        }
    }
    
    GC_LOGI("gc_sweep_unified: DONE");
}

/* ============================================================================
 * GC RUN CYCLE
 * ============================================================================
 */

static void gc_run_internal(void) {
    if (!g_gc.initialized) return;
    
    JSRuntimeHandle rt(g_gc.rt);
    bool has_runtime = (g_gc.rt != GC_HANDLE_NULL);
    
    GC_LOGI("gc_run_internal: ENTER has_runtime=%d", has_runtime);
    
    /* Enter MARKING phase */
    atomic_store_u32(&g_gc.gc_phase, (uint32_t)GC_PHASE_MARKING);
    
    if (has_runtime) {
        /* Phase 0: Remove weak objects (WeakMap, WeakSet, WeakRef) */
        /* This must happen BEFORE marking so weak refs are properly cleared */
        GC_LOGI("gc_run_internal: Phase 0 - removing weak objects");
        gc_remove_weak_objects(rt);
    }
    
    /* Phase 1: Mark phase - run on a pool worker while the mutator is paused.
     * The mark job signals g_gc_mark_done_event when it has drained the grey
     * queue. */
    GC_LOGI("gc_run_internal: Phase 1 - marking (background)");
    gc_thread_pool_submit_job(gc_mark_job_func, NULL);
    gc_mark_wait_done();
    
    if (has_runtime) {
        /* Phase 2: Clean up shape hash table before compaction */
        GC_LOGI("gc_run_internal: Phase 2 - cleaning shape hash table");
        gc_cleanup_shape_hash_table(rt);
        
        /* Phase 3: Sweep phase - free unmarked objects */
        GC_LOGI("gc_run_internal: Phase 3 - sweeping");
        gc_sweep_unified(rt);
        
        /* Phase 4: Sweep atoms */
        GC_LOGI("gc_run_internal: Phase 4 - sweeping atoms");
        gc_sweep_atoms(rt);
    }
    
    /* Enter COMPACTING phase */
    atomic_store_u32(&g_gc.gc_phase, (uint32_t)GC_PHASE_COMPACTING);
    /* Set compaction target to the inactive buffer */
    atomic_store_u32(&g_gc.compaction_target, (uint32_t)(1 - gc_active_buffer_index()));
    
    /* Phase 5: Compact phase - move live objects and update handles */
    GC_LOGI("gc_run_internal: Phase 5 - compacting into buffer %d",
            atomic_load_u32(&g_gc.compaction_target));
    gc_compact();
    
    /* Enter SWAPPING phase (brief) then back to IDLE */
    atomic_store_u32(&g_gc.gc_phase, (uint32_t)GC_PHASE_SWAPPING);
    atomic_store_u32(&g_gc.compaction_target, (uint32_t)0);
    atomic_store_u32(&g_gc.gc_phase, (uint32_t)GC_PHASE_IDLE);
    
    /* Phase 6: Compact typed handle arrays */
    GC_LOGI("gc_run_internal: Phase 6 - compacting handle arrays");
    gc_handle_array_compact(&g_gc.weakref_handles);
    gc_handle_array_compact(&g_gc.finrec_handles);
    gc_handle_array_compact(&g_gc.atom_handles);
    
    GC_LOGI("gc_run_internal: DONE");
}

static void gc_maybe_run(void) {
    if (g_gc.bytes_allocated > g_gc.gc_threshold) {
        gc_request_background();
    }
}

void gc_run(void) {
    gc_request_background();
    gc_wait_for_completion();
}

void gc_reset(void) {
    if (!g_gc.initialized) return;
    
    /* Wait for any background GC to complete */
    gc_wait_for_completion();
    
    /* Reset active buffer */
    GCBuffer *active = gc_active_buffer();
    active->bump_offset = 0;
    
    /* Clear handle table */
    for (uint32_t i = 1; i < active->handle_count; i++) {
        active->handles[i] = NULL;
    }
    active->handle_count = 1;
    
    /* Reset root set */
    g_gc.root_set.count = 0;
    
    g_gc.bytes_allocated = 0;
    atomic_store_u32(&g_gc.free_head, GC_HANDLE_NULL);
    atomic_store_u32(&g_gc.free_count, 0);
}

void gc_reset_full(void) {
    gc_wait_for_completion();
    browser_api_impl_reset();
    js_quickjs_reset_class_ids();
    gc_cleanup();
    gc_init();
}

bool gc_add_root(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return true;
    
    /* Resize root set if needed */
    if (g_gc.root_set.count >= g_gc.root_set.capacity) {
        uint32_t new_capacity = g_gc.root_set.capacity * 2;
        GCHandle *new_roots = (GCHandle*)realloc(g_gc.root_set.roots, 
                                                  new_capacity * sizeof(GCHandle));
        if (!new_roots) return false;  /* Failed to resize, can't add root */
        g_gc.root_set.roots = new_roots;
        g_gc.root_set.capacity = new_capacity;
    }
    
    g_gc.root_set.roots[g_gc.root_set.count++] = handle;
    return true;
}

void gc_remove_root(GCHandle handle) {
    for (uint32_t i = 0; i < g_gc.root_set.count; i++) {
        if (g_gc.root_set.roots[i] == handle) {
            g_gc.root_set.roots[i] = g_gc.root_set.roots[--g_gc.root_set.count];
            return;
        }
    }
}

size_t gc_used_bytes(void) {
    if (!g_gc.initialized) return 0;
    return gc_active_buffer()->bump_offset;
}

size_t gc_available_bytes(void) {
    if (!g_gc.initialized) return 0;
    GCBuffer *active = gc_active_buffer();
    return active->storage_size - active->bump_offset;
}

size_t gc_total_bytes(void) {
    if (!g_gc.initialized) return 0;
    return gc_active_buffer()->storage_size;
}

/* ============================================================================
 * CANARY VALIDATION FOR HEAP CORRUPTION DETECTION
 * ============================================================================
 * 
 * Canaries are placed before and after each allocation to detect buffer
 * overflows and memory corruption. The canary layout is:
 * 
 * [PREFIX CANARY: 8 bytes] [GCHeader: 64 bytes] [user data] [SUFFIX CANARY: 8 bytes]
 * 
 * Both canaries are checked at:
 * - Allocation time (to catch use-after-free)
 * - Reallocation time
 * - GC compaction time
 * - Explicit validation calls
 */

/* Get pointer to prefix canary (16 bytes before GCHeader) */
static inline uint64_t *gc_canary_prefix_ptr(GCHeader *hdr) {
    if (!hdr) return NULL;
    return (uint64_t*)((uint8_t*)hdr - 16);
}

/* Get pointer to suffix canary (after user data) */
static inline uint64_t *gc_canary_suffix_ptr(GCHeader *hdr) {
    if (!hdr) return NULL;
    /* With hdr->size = user_size:
     * suffix is at hdr + sizeof(GCHeader) + user_size
     * Note: mask out FREED flag (bit 31) if set
     */
    uint32_t user_size = hdr->size & 0x7FFFFFFF;
    uint8_t *suffix = (uint8_t*)hdr + sizeof(GCHeader) + user_size;
    return (uint64_t*)suffix;
}

/* Set canaries for a new allocation */
static inline void gc_set_canaries(GCHeader *hdr) {
    if (!hdr) return;
    uint64_t *prefix = gc_canary_prefix_ptr(hdr);
    uint64_t *suffix = gc_canary_suffix_ptr(hdr);
    if (prefix) *prefix = GC_CANARY_PREFIX;
    if (suffix) *suffix = GC_CANARY_SUFFIX;
}

/* Corrupt canaries (called when freeing to catch use-after-free) */
static inline void gc_corrupt_canaries(GCHeader *hdr) {
    if (!hdr) return;
    uint64_t *prefix = gc_canary_prefix_ptr(hdr);
    uint64_t *suffix = gc_canary_suffix_ptr(hdr);
    if (prefix) *prefix = GC_CANARY_CORRUPTED;
    if (suffix) *suffix = GC_CANARY_CORRUPTED;
}

/* Validate canaries for a raw GCHeader pointer */
static GCCanaryStatus gc_validate_canaries_hdr(GCHeader *hdr, void **out_ptr) {
    if (!hdr) return GC_CANARY_NULL_POINTER;
    
    /* Check if handle is valid */
    if (hdr->handle == GC_HANDLE_NULL || hdr->handle >= gc_active_handle_count()) {
        return GC_CANARY_INVALID_HANDLE;
    }
    
    /* Check prefix canary */
    uint64_t *prefix = gc_canary_prefix_ptr(hdr);
    if (prefix && *prefix != GC_CANARY_PREFIX) {
        if (out_ptr) *out_ptr = (uint8_t*)hdr + sizeof(GCHeader);
        return GC_CANARY_CORRUPTED_PREFIX;
    }
    
    /* Check suffix canary */
    uint64_t *suffix = gc_canary_suffix_ptr(hdr);
    if (suffix && *suffix != GC_CANARY_SUFFIX) {
        if (out_ptr) *out_ptr = (uint8_t*)hdr + sizeof(GCHeader);
        return GC_CANARY_CORRUPTED_SUFFIX;
    }
    
    if (out_ptr) *out_ptr = (uint8_t*)hdr + sizeof(GCHeader);
    return GC_CANARY_OK;
}

/* Public API: Validate canaries for a handle */
GCCanaryStatus gc_validate_canaries(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return GC_CANARY_INVALID_HANDLE;
    if (handle >= gc_active_handle_count()) return GC_CANARY_INVALID_HANDLE;
    
    void *ptr = gc_active_handles()[handle];
    if (!ptr) return GC_CANARY_NULL_POINTER;
    
    GCHeader *hdr = gc_header(ptr);
    return gc_validate_canaries_hdr(hdr, NULL);
}

/* Public API: Validate canaries for a pointer */
GCCanaryStatus gc_validate_canaries_ptr(void *ptr) {
    if (!ptr) return GC_CANARY_NULL_POINTER;
    GCHeader *hdr = gc_header(ptr);
    return gc_validate_canaries_hdr(hdr, NULL);
}

/* Get string description of canary status */
const char *gc_canary_status_string(GCCanaryStatus status) {
    switch (status) {
        case GC_CANARY_OK: return "OK";
        case GC_CANARY_CORRUPTED_PREFIX: return "CORRUPTED_PREFIX (buffer underflow or header corruption)";
        case GC_CANARY_CORRUPTED_SUFFIX: return "CORRUPTED_SUFFIX (buffer overflow)";
        case GC_CANARY_INVALID_HANDLE: return "INVALID_HANDLE";
        case GC_CANARY_NULL_POINTER: return "NULL_POINTER";
        default: return "UNKNOWN";
    }
}

/* Get type name for debugging */
static const char *gc_obj_type_name(JSGCObjectTypeEnum type) {
    switch (type) {
        case JS_GC_OBJ_TYPE_JS_OBJECT: return "JSObject";
        case JS_GC_OBJ_TYPE_FUNCTION_BYTECODE: return "FunctionBytecode";
        case JS_GC_OBJ_TYPE_SHAPE: return "JSShape";
        case JS_GC_OBJ_TYPE_VAR_REF: return "JSVarRef";
        case JS_GC_OBJ_TYPE_ASYNC_FUNCTION: return "JSAsyncFunction";
        case JS_GC_OBJ_TYPE_JS_CONTEXT: return "JSContext";
        case JS_GC_OBJ_TYPE_JS_RUNTIME: return "JSRuntime";
        case JS_GC_OBJ_TYPE_MODULE: return "JSModule";
        /* case JS_GC_OBJ_TYPE_JOB_ENTRY: return "JobEntry"; */
        case JS_GC_OBJ_TYPE_JS_STRING: return "JSString";
        case JS_GC_OBJ_TYPE_JS_STRING_ROPE: return "JSStringRope";
        case JS_GC_OBJ_TYPE_JS_BIGINT: return "JSBigInt";
        case JS_GC_OBJ_TYPE_DATA: return "Data";
        default: return "Unknown";
    }
}

/* Print detailed corruption info */
void gc_print_corruption_info(GCHandle handle, GCCanaryStatus status) {
    fprintf(stderr, "\n");
    fprintf(stderr, "=================================================\n");
    fprintf(stderr, "GC CANARY CORRUPTION DETECTED!\n");
    fprintf(stderr, "=================================================\n");
    fprintf(stderr, "Handle: %u\n", handle);
    fprintf(stderr, "Status: %s\n", gc_canary_status_string(status));
    
    if (handle == GC_HANDLE_NULL || handle >= gc_active_handle_count()) {
        fprintf(stderr, "Invalid handle, cannot print details\n");
        fprintf(stderr, "=================================================\n");
        return;
    }
    
    void *ptr = gc_active_handles()[handle];
    if (!ptr) {
        fprintf(stderr, "Pointer is NULL\n");
        fprintf(stderr, "=================================================\n");
        return;
    }
    
    GCHeader *hdr = gc_header(ptr);
    fprintf(stderr, "Object Type: %s (%d)\n", gc_obj_type_name((JSGCObjectTypeEnum)hdr->gc_obj_type), hdr->gc_obj_type);
    fprintf(stderr, "Object Size: %u bytes\n", hdr->size);
    fprintf(stderr, "Object Address: %p\n", ptr);
    fprintf(stderr, "Header Address: %p\n", (void*)hdr);
    
    /* Show canary values */
    uint64_t *prefix = gc_canary_prefix_ptr(hdr);
    uint64_t *suffix = gc_canary_suffix_ptr(hdr);
    
    if (prefix) {
        fprintf(stderr, "\nPrefix Canary (expected 0x%016llx):\n", (unsigned long long)GC_CANARY_PREFIX);
        fprintf(stderr, "  Actual: 0x%016llx\n", (unsigned long long)*prefix);
        fprintf(stderr, "  Offset: %p\n", (void*)prefix);
        
        /* Try to interpret as ASCII */
        char *ascii = (char*)prefix;
        fprintf(stderr, "  ASCII: ");
        for (int i = 7; i >= 0; i--) {
            unsigned char c = ascii[i];
            fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        fprintf(stderr, "\n");
    }
    
    if (suffix) {
        fprintf(stderr, "\nSuffix Canary (expected 0x%016llx):\n", (unsigned long long)GC_CANARY_SUFFIX);
        fprintf(stderr, "  Actual: 0x%016llx\n", (unsigned long long)*suffix);
        fprintf(stderr, "  Offset: %p\n", (void*)suffix);
        
        /* Try to interpret as ASCII */
        char *ascii = (char*)suffix;
        fprintf(stderr, "  ASCII: ");
        for (int i = 7; i >= 0; i--) {
            unsigned char c = ascii[i];
            fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
        }
        fprintf(stderr, "\n");
    }
    
    /* Show hex dump of surrounding memory for suffix corruption */
    if (status == GC_CANARY_CORRUPTED_SUFFIX && suffix) {
        fprintf(stderr, "\nMemory dump around corruption:\n");
        uint8_t *dump_start = (uint8_t*)suffix - 64;
        for (int i = 0; i < 80; i += 16) {
            fprintf(stderr, "  %p: ", (void*)(dump_start + i));
            for (int j = 0; j < 16; j++) {
                fprintf(stderr, "%02x ", dump_start[i + j]);
            }
            fprintf(stderr, " |");
            for (int j = 0; j < 16; j++) {
                unsigned char c = dump_start[i + j];
                fprintf(stderr, "%c", (c >= 32 && c < 127) ? c : '.');
            }
            fprintf(stderr, "|\n");
            if (i == 64) fprintf(stderr, "  --> SUFFIX CANARY HERE <--\n");
        }
    }
    
    fprintf(stderr, "=================================================\n");
    fprintf(stderr, "\n");
}

/* Validate all canaries in the heap */
int gc_validate_all_canaries(bool verbose) {
    if (!g_gc.initialized) return 0;
    
    int corrupted_count = 0;
    
    void **table = gc_active_handles();
    uint32_t count = gc_active_handle_count();
    
    for (uint32_t i = 1; i < count; i++) {
        void *ptr = table[i];
        if (!ptr) continue;
        
        GCHeader *hdr = gc_header(ptr);
        GCCanaryStatus status = gc_validate_canaries_hdr(hdr, NULL);
        
        if (status != GC_CANARY_OK) {
            corrupted_count++;
            if (verbose) {
                gc_print_corruption_info(i, status);
            }
        }
    }
    
    return corrupted_count;
}

/* Check canaries and abort if corrupted (for debugging) */
void gc_check_canaries_or_abort(GCHandle handle, const char *location) {
    GCCanaryStatus status = gc_validate_canaries(handle);
    if (status != GC_CANARY_OK) {
        fprintf(stderr, "\n*** CANARY CHECK FAILED at %s ***\n", location ? location : "unknown");
        gc_print_corruption_info(handle, status);
        abort();
    }
}

/* ============================================================================
 * DIAGNOSTIC FUNCTIONS FOR QUICKJS INTEGRATION
 * ============================================================================
 */

/* 
 * Validate a shape object by handle
 * This is called from quickjs.c when shape corruption is suspected
 */
GCCanaryStatus gc_validate_shape_canaries(uint32_t shape_handle) {
    if (shape_handle == GC_HANDLE_NULL) {
        return GC_CANARY_INVALID_HANDLE;
    }
    return gc_validate_canaries(shape_handle);
}

/*
 * Diagnose corruption around a specific object
 * This dumps information about neighboring objects to help identify
 * which object caused the overflow
 */
void gc_diagnose_corruption_context(GCHandle handle) {
    if (handle == GC_HANDLE_NULL || handle >= gc_active_handle_count()) {
        fprintf(stderr, "[DIAGNOSE] Invalid handle %u\n", handle);
        return;
    }
    
    void *ptr = gc_active_handles()[handle];
    if (!ptr) {
        fprintf(stderr, "[DIAGNOSE] Handle %u has NULL pointer\n", handle);
        return;
    }
    
    GCHeader *hdr = gc_header(ptr);
    fprintf(stderr, "\n[DIAGNOSE] Corruption context for handle %u:\n", handle);
    fprintf(stderr, "  Object address: %p\n", ptr);
    fprintf(stderr, "  Header address: %p\n", (void*)hdr);
    fprintf(stderr, "  Object size: %u\n", hdr->size);
    fprintf(stderr, "  Object type: %s\n", gc_obj_type_name((JSGCObjectTypeEnum)hdr->gc_obj_type));
    
    /* Show nearby handles */
    fprintf(stderr, "\n  Nearby objects:\n");
    int start = (int)handle - 5;
    if (start < 1) start = 1;
    int end = (int)handle + 5;
    uint32_t count = gc_active_handle_count();
    if (end > (int)count) end = (int)count;
    
    void **table = gc_active_handles();
    for (int i = start; i < end; i++) {
        void *near_ptr = table[i];
        if (near_ptr) {
            GCHeader *near_hdr = gc_header(near_ptr);
            GCCanaryStatus status = gc_validate_canaries(i);
            const char *status_str = (status == GC_CANARY_OK) ? "OK" : "CORRUPTED";
            fprintf(stderr, "    Handle %d: %s (size=%u, type=%s) [%s]\n",
                    i, status_str, near_hdr->size, 
                    gc_obj_type_name((JSGCObjectTypeEnum)near_hdr->gc_obj_type),
                    (i == (int)handle) ? "<-- TARGET" : "");
        } else {
            fprintf(stderr, "    Handle %d: FREE\n", i);
        }
    }
    
    /* Validate all canaries to find all corruption */
    fprintf(stderr, "\n  Scanning all objects for corruption...\n");
    int total_corrupted = gc_validate_all_canaries(false);
    fprintf(stderr, "  Total corrupted objects: %d\n", total_corrupted);
    
    fprintf(stderr, "\n");
}

/*
 * Emergency heap dump for post-mortem analysis
 * Dumps all objects and their canary status to a file
 */
void gc_dump_heap_for_analysis(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "[DUMP] Failed to open %s for writing\n", filename);
        return;
    }
    
    fprintf(f, "GC Heap Dump\n");
    fprintf(f, "============\n\n");
    fprintf(f, "Total handles: %u\n", gc_active_handle_count());
    fprintf(f, "Used bytes: %zu\n", gc_used_bytes());
    fprintf(f, "\n");
    
    void **table = gc_active_handles();
    uint32_t count = gc_active_handle_count();
    
    for (uint32_t i = 1; i < count; i++) {
        void *ptr = table[i];
        if (!ptr) {
            fprintf(f, "Handle %u: FREE\n", i);
            continue;
        }
        
        GCHeader *hdr = gc_header(ptr);
        GCCanaryStatus status = gc_validate_canaries(i);
        
        fprintf(f, "Handle %u:\n", i);
        fprintf(f, "  Address: %p\n", ptr);
        fprintf(f, "  Size: %u\n", hdr->size);
        fprintf(f, "  Type: %s (%d)\n", gc_obj_type_name((JSGCObjectTypeEnum)hdr->gc_obj_type), hdr->gc_obj_type);
        fprintf(f, "  Mark: %d\n", hdr->gc_color_state != GC_COLOR_WHITE);
        fprintf(f, "  Canary Status: %s\n", gc_canary_status_string(status));
        
        if (status != GC_CANARY_OK) {
            uint64_t *prefix = gc_canary_prefix_ptr(hdr);
            uint64_t *suffix = gc_canary_suffix_ptr(hdr);
            if (prefix) {
                fprintf(f, "  Prefix: 0x%016llx (expected 0x%016llx)\n",
                        (unsigned long long)*prefix, (unsigned long long)GC_CANARY_PREFIX);
            }
            if (suffix) {
                fprintf(f, "  Suffix: 0x%016llx (expected 0x%016llx)\n",
                        (unsigned long long)*suffix, (unsigned long long)GC_CANARY_SUFFIX);
            }
        }
        fprintf(f, "\n");
    }
    
    fclose(f);
    fprintf(stderr, "[DUMP] Heap dump written to %s\n", filename);
}

/* ============================================================================
 * GC-Safe Linked List Implementations
 * ============================================================================
 * 
 * These functions implement doubly-linked lists using GCHandles instead of
 * raw pointers, making them safe to use with the compacting garbage collector.
 */

/* Helper to get GCListHead* from handle and offset */
static inline GCListHead* gc_list_head_from_handle(GCHandle handle, size_t offset) {
    if (handle == GC_HANDLE_NULL) return nullptr;
    void* ptr = gc_deref(handle);
    return ptr ? (GCListHead*)((uint8_t*)ptr + offset) : nullptr;
}

/* Add a node between two existing nodes */
void gc_list_add_between(GCHandle new_node,
                         GCHandle prev, GCHandle next,
                         size_t link_offset) {
    GCListHead *new_link = gc_list_head_from_handle(new_node, link_offset);
    if (!new_link) return;
    
    new_link->prev = prev;
    new_link->next = next;
    gc_write_barrier_for_heap_slot(&new_link->prev, prev);
    gc_write_barrier_for_heap_slot(&new_link->next, next);
    
    if (prev != GC_HANDLE_NULL) {
        GCListHead *prev_link = gc_list_head_from_handle(prev, link_offset);
        if (prev_link) {
            prev_link->next = new_node;
            gc_write_barrier_for_heap_slot(&prev_link->next, new_node);
        }
    }
    
    if (next != GC_HANDLE_NULL) {
        GCListHead *next_link = gc_list_head_from_handle(next, link_offset);
        if (next_link) {
            next_link->prev = new_node;
            gc_write_barrier_for_heap_slot(&next_link->prev, new_node);
        }
    }
}

/* Add node at the head of the list */
void gc_list_add(GCHandle new_node, struct GCListHead *head,
                 size_t link_offset) {
    if (!head || new_node == GC_HANDLE_NULL) return;
    
    GCHandle first = head->next;
    gc_list_add_between(new_node, GC_HANDLE_NULL, first, link_offset);
    
    head->next = new_node;
    gc_write_barrier_for_heap_slot(&head->next, new_node);
    if (head->prev == GC_HANDLE_NULL) {
        head->prev = new_node;
        gc_write_barrier_for_heap_slot(&head->prev, new_node);
    }
}

/* Add node at the tail of the list */
void gc_list_add_tail(GCHandle new_node, struct GCListHead *head,
                      size_t link_offset) {
    if (!head || new_node == GC_HANDLE_NULL) return;
    
    GCHandle last = head->prev;
    gc_list_add_between(new_node, last, GC_HANDLE_NULL, link_offset);
    
    head->prev = new_node;
    gc_write_barrier_for_heap_slot(&head->prev, new_node);
    if (head->next == GC_HANDLE_NULL) {
        head->next = new_node;
        gc_write_barrier_for_heap_slot(&head->next, new_node);
    }
}

/* Delete a node from the list */
void gc_list_del(GCHandle node, struct GCListHead *head,
                 size_t link_offset) {
    if (!head || node == GC_HANDLE_NULL) return;
    
    GCListHead *link = gc_list_head_from_handle(node, link_offset);
    if (!link) return;
    
    GCHandle prev = link->prev;
    GCHandle next = link->next;
    
    if (prev != GC_HANDLE_NULL) {
        GCListHead *prev_link = gc_list_head_from_handle(prev, link_offset);
        if (prev_link) prev_link->next = next;
    } else {
        /* This was the first node */
        head->next = next;
    }
    
    if (next != GC_HANDLE_NULL) {
        GCListHead *next_link = gc_list_head_from_handle(next, link_offset);
        if (next_link) next_link->prev = prev;
    } else {
        /* This was the last node */
        head->prev = prev;
    }
    
    /* Clear the node's links */
    link->prev = GC_HANDLE_NULL;
    link->next = GC_HANDLE_NULL;
}

/* Add node at the tail of a list in a container (handle-based - GC-safe) */
void gc_list_add_tail_in_container(GCHandle new_node, GCHandle container,
                                   size_t list_offset, size_t link_offset) {
    if (container == GC_HANDLE_NULL || new_node == GC_HANDLE_NULL) return;
    
    /* Get the list head pointer temporarily - safe as long as no GC happens here */
    GCListHead *head = (GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    if (!head) return;
    
    gc_list_add_tail(new_node, head, link_offset);
}

/* Delete a node from a list in a container (handle-based - GC-safe) */
void gc_list_del_in_container(GCHandle node, GCHandle container,
                              size_t list_offset, size_t link_offset) {
    if (container == GC_HANDLE_NULL || node == GC_HANDLE_NULL) return;
    
    /* Get the list head pointer temporarily - safe as long as no GC happens here */
    GCListHead *head = (GCListHead *)((uint8_t*)gc_deref(container) + list_offset);
    if (!head) return;
    
    gc_list_del(node, head, link_offset);
}
