/* Auto-generated split from browser_api_impl.cpp */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handles.h"
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"
#include <pthread.h>

GCValue js_navigator_send_beacon(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    const char *url = JS_ToCString(ctx, argv[0]);
    if (url) {
        // Capture the beacon URL
        capture_url_debug(url, "send_beacon");
    }
    
    // Always return true per spec (beacon is "fire and forget")
    return JS_TRUE;
}

// ============================================================================
// Timer API Implementation (setTimeout, setInterval, requestAnimationFrame, etc.)
// ============================================================================

#include <pthread.h>

// Maximum number of concurrent timers
#define MAX_TIMERS 256
#define MAX_TIMER_ARGS 8

typedef enum {
    TIMER_TYPE_TIMEOUT,
    TIMER_TYPE_INTERVAL,
    TIMER_TYPE_RAF,           // requestAnimationFrame
    TIMER_TYPE_IDLE_CALLBACK  // requestIdleCallback
} TimerType;

typedef struct {
    int id;                           // Timer ID (positive integer)
    TimerType type;                   // Type of timer
    unsigned long long trigger_time;  // Time when timer should fire (ms)
    unsigned long long interval;      // For intervals: repeat interval (ms)
    int active;                       // 1 if active, 0 if cleared
    
    // Callback storage - we store the function as a JS value
    // Since GCValue is a handle, we need to keep it alive
    int callback_handle;              // Index into callback storage
    
    // Arguments
    int arg_count;
    int arg_handles[MAX_TIMER_ARGS];  // Indices into callback storage for args
} Timer;

// Timer storage and management
typedef struct {
    Timer timers[MAX_TIMERS];
    int timer_count;
    int next_id;
    pthread_mutex_t mutex;
    
    // Callback storage - parallel array to keep JS objects alive
    // We use a simple scheme: store JS values that the GC knows about
    GCValue callbacks[MAX_TIMERS * (MAX_TIMER_ARGS + 1)];  // +1 for callback itself
    int callback_count;
} TimerState;

// Static timer state - will be properly initialized at runtime
static TimerState g_timer_state;
static int g_timer_state_initialized = 0;

// Initialize timer state on first use
void timer_state_ensure_initialized(void) {
    if (!g_timer_state_initialized) {
        memset(&g_timer_state, 0, sizeof(g_timer_state));
        pthread_mutex_init(&g_timer_state.mutex, NULL);
        g_timer_state.next_id = 1;
        g_timer_state_initialized = 1;
    }
}

// Helper to check if a GCValue contains a reference type that needs root registration
static inline int gcvalue_is_reference(GCValue val) {
    // Negative tags are reference types (objects, strings, symbols, etc.)
    return GC_VALUE_GET_TAG(val) < 0;
}

// Store a JS value in the callback storage, returns handle index
int store_callback(JSContextHandle ctx, GCValue val) {
    (void)ctx;
    if (g_timer_state.callback_count >= MAX_TIMERS * (MAX_TIMER_ARGS + 1)) {
        return -1; // Full
    }
    // Store the handle directly
    int idx = g_timer_state.callback_count;
    g_timer_state.callbacks[idx] = val;
    
    // Register as GC root if it's a reference type (callback functions need to survive GC)
    if (gcvalue_is_reference(val)) {
        GCHandle handle = GC_VALUE_GET_HANDLE(val);
        if (handle != GC_HANDLE_NULL) {
            gc_add_root(handle);
        }
    }
    
    g_timer_state.callback_count++;
    return idx;
}

// Get a stored callback by handle
GCValue get_callback(int handle) {
    if (handle < 0 || handle >= g_timer_state.callback_count) {
        return JS_UNDEFINED;
    }
    return g_timer_state.callbacks[handle];
}

// Helper to unregister all callback roots for a timer
void unregister_timer_roots(Timer *timer) {
    if (!timer) return;
    
    // Unregister callback root
    GCValue callback = g_timer_state.callbacks[timer->callback_handle];
    if (gcvalue_is_reference(callback)) {
        GCHandle handle = GC_VALUE_GET_HANDLE(callback);
        if (handle != GC_HANDLE_NULL) {
            gc_remove_root(handle);
        }
    }
    
    // Unregister argument roots
    for (int i = 0; i < timer->arg_count; i++) {
        GCValue arg = g_timer_state.callbacks[timer->arg_handles[i]];
        if (gcvalue_is_reference(arg)) {
            GCHandle handle = GC_VALUE_GET_HANDLE(arg);
            if (handle != GC_HANDLE_NULL) {
                gc_remove_root(handle);
            }
        }
    }
}

// Clear all timers and callback storage
extern "C" void timer_api_reset(void) {
    timer_state_ensure_initialized();
    pthread_mutex_lock(&g_timer_state.mutex);
    
    // Unregister all GC roots before clearing
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timer_state.timers[i].id != 0) {
            unregister_timer_roots(&g_timer_state.timers[i]);
        }
    }
    
    // Clear all stored callbacks
    memset(&g_timer_state.callbacks, 0, sizeof(g_timer_state.callbacks));
    g_timer_state.callback_count = 0;
    
    // Reset timers
    memset(&g_timer_state.timers, 0, sizeof(g_timer_state.timers));
    g_timer_state.timer_count = 0;
    g_timer_state.next_id = 1;
    
    pthread_mutex_unlock(&g_timer_state.mutex);
}

// Find a free timer slot
int find_free_timer_slot(void) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timer_state.timers[i].active && g_timer_state.timers[i].id == 0) {
            return i;
            }
        }
    return -1;
}

// Add a new timer
int add_timer(JSContextHandle ctx, TimerType type, GCValue callback, 
                     unsigned long long delay_ms, int arg_count, GCValue *args) {
    (void)ctx;
    timer_state_ensure_initialized();
    pthread_mutex_lock(&g_timer_state.mutex);
    
    int slot = find_free_timer_slot();
    if (slot < 0) {
        pthread_mutex_unlock(&g_timer_state.mutex);
        return 0; // No slots available
    }
    
    Timer *timer = &g_timer_state.timers[slot];
    timer->id = g_timer_state.next_id++;
    timer->type = type;
    timer->trigger_time = platform_get_time_ms() + delay_ms;
    timer->interval = (type == TIMER_TYPE_INTERVAL) ? delay_ms : 0;
    timer->active = 1;
    
    // Store callback
    int cb_handle = store_callback(ctx, callback);
    if (cb_handle < 0) {
        timer->id = 0;
        pthread_mutex_unlock(&g_timer_state.mutex);
        return 0;
    }
    timer->callback_handle = cb_handle;
    
    // Store arguments (up to MAX_TIMER_ARGS)
    timer->arg_count = (arg_count > MAX_TIMER_ARGS) ? MAX_TIMER_ARGS : arg_count;
    for (int i = 0; i < timer->arg_count; i++) {
        int arg_handle = store_callback(ctx, args[i]);
        if (arg_handle < 0) {
            // Failed, clean up
            timer->arg_count = i;
            timer->id = 0;
            pthread_mutex_unlock(&g_timer_state.mutex);
            return 0;
        }
        timer->arg_handles[i] = arg_handle;
    }
    
    g_timer_state.timer_count++;
    pthread_mutex_unlock(&g_timer_state.mutex);
    
    return timer->id;
}

// Clear a timer by ID
int clear_timer_by_id(int id) {
    if (id <= 0) return 0;
    
    timer_state_ensure_initialized();
    pthread_mutex_lock(&g_timer_state.mutex);
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timer_state.timers[i].id == id && g_timer_state.timers[i].active) {
            // Unregister GC roots for this timer's callback and args
            unregister_timer_roots(&g_timer_state.timers[i]);
            
            g_timer_state.timers[i].active = 0;
            g_timer_state.timer_count--;
            pthread_mutex_unlock(&g_timer_state.mutex);
            return 1;
        }
    }
    pthread_mutex_unlock(&g_timer_state.mutex);
    return 0;
}

// Find the next timer that should fire, returns its ID or 0 if none
int find_due_timer(unsigned long long current_time) {
    timer_state_ensure_initialized();
    pthread_mutex_lock(&g_timer_state.mutex);
    
    int due_id = 0;
    unsigned long long earliest_time = (unsigned long long)(-1);
    
    for (int i = 0; i < MAX_TIMERS; i++) {
        Timer *t = &g_timer_state.timers[i];
        if (t->active && t->trigger_time <= current_time && t->trigger_time < earliest_time) {
            earliest_time = t->trigger_time;
            due_id = t->id;
        }
    }
    
    pthread_mutex_unlock(&g_timer_state.mutex);
    return due_id;
}

// Real IdleDeadline.timeRemaining() based on the deadline stored in the IdleDeadline object.
static GCValue js_idle_deadline_time_remaining(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    GCValue deadline_val = JS_GetPropertyStr(ctx, this_val, "__deadline");
    int64_t deadline = 0;
    if (JS_IsNumber(deadline_val)) {
        JS_ToInt64(ctx, &deadline, deadline_val);
    }
    int64_t now = (int64_t)platform_get_time_ms();
    double remaining = (double)(deadline - now);
    if (remaining < 0) remaining = 0;
    return JS_NewFloat64(ctx, remaining);
}

// Current idle deadline used by requestIdleCallback.  The event loop sets this
// before draining timers so that IdleDeadline.timeRemaining() reflects the real
// remaining idle budget instead of a fixed value.
static unsigned long long g_idle_deadline_ms = 0;

extern "C" void timer_set_idle_deadline(unsigned long long deadline_ms) {
    g_idle_deadline_ms = deadline_ms;
}

// Execute a timer callback by ID
void execute_timer(JSContextHandle ctx, int id) {
    pthread_mutex_lock(&g_timer_state.mutex);
    
    Timer *timer = NULL;
    int slot = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timer_state.timers[i].id == id && g_timer_state.timers[i].active) {
            timer = &g_timer_state.timers[i];
            slot = i;
            break;
        }
    }
    
    if (!timer) {
        pthread_mutex_unlock(&g_timer_state.mutex);
        return;
    }
    
    // Get callback and args while holding lock
    GCValue callback = get_callback(timer->callback_handle);
    GCValue args[MAX_TIMER_ARGS];
    int arg_count = timer->arg_count;
    for (int i = 0; i < arg_count; i++) {
        args[i] = get_callback(timer->arg_handles[i]);
    }
    
    // Handle different timer types
    if (timer->type == TIMER_TYPE_TIMEOUT || timer->type == TIMER_TYPE_RAF || timer->type == TIMER_TYPE_IDLE_CALLBACK) {
        // One-shot timer: mark as inactive before executing
        timer->active = 0;
        g_timer_state.timer_count--;
    } else if (timer->type == TIMER_TYPE_INTERVAL) {
        // Interval: schedule next trigger
        timer->trigger_time = platform_get_time_ms() + timer->interval;
    }
    
    pthread_mutex_unlock(&g_timer_state.mutex);
    
    // Execute callback (outside lock to avoid deadlock)
    if (JS_IsFunction(ctx, callback)) {
        // For RAF, pass the timestamp as argument
        if (timer->type == TIMER_TYPE_RAF && arg_count == 0) {
            args[0] = JS_NewFloat64(ctx, (double)platform_get_time_ms());
            arg_count = 1;
        }
        
        // For idle callback, pass an IdleDeadline object with a real deadline.
        if (timer->type == TIMER_TYPE_IDLE_CALLBACK && arg_count == 0) {
            unsigned long long idle_deadline_ms = g_idle_deadline_ms ? g_idle_deadline_ms : (platform_get_time_ms() + 16);
            GCValue idle_deadline = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, idle_deadline, "didTimeout", JS_FALSE);
            JS_SetPropertyStr(ctx, idle_deadline, "__deadline",
                JS_NewInt64(ctx, (int64_t)idle_deadline_ms));
            JS_SetPropertyStr(ctx, idle_deadline, "timeRemaining",
                JS_NewCFunction(ctx, js_idle_deadline_time_remaining, "timeRemaining", 0));
            args[0] = idle_deadline;
            arg_count = 1;
        }
        
        GCValue global_obj = JS_GetGlobalObject(ctx);
        GCValue result = JS_Call(ctx, callback, global_obj, arg_count, args);
        if (JS_IsException(result)) {
            GCValue exc = JS_GetException(ctx);
            const char *exc_str = JS_ToCString(ctx, exc);
            GCValue stack_val = JS_GetPropertyStr(ctx, exc, "stack");
            const char *stack = NULL;
            if (!JS_IsUndefined(stack_val) && !JS_IsNull(stack_val)) {
                stack = JS_ToCString(ctx, stack_val);
            }
            {
                if (exc_str) {
                    platform_log(LOG_LEVEL_WARN, "timer", "Timer callback exception: %s", exc_str);
                }
                if (stack) {
                    platform_log(LOG_LEVEL_WARN, "timer", "Timer callback stack: %s", stack);
                }
                // Diagnostic: when the error is "not a function", print the
                // callback identity so we can identify which API is missing.
                if (exc_str && strstr(exc_str, "not a function")) {
                    GCValue cb_name = JS_GetPropertyStr(ctx, callback, "name");
                    GCValue cb_len = JS_GetPropertyStr(ctx, callback, "length");
                    GCValue cb_ctor = JS_GetPropertyStr(ctx, callback, "constructor");
                    GCValue ctor_name = JS_IsObject(cb_ctor) ? JS_GetPropertyStr(ctx, cb_ctor, "name") : JS_UNDEFINED;
                    int32_t cb_len_i = 0; JS_ToInt32(ctx, &cb_len_i, cb_len);
                    const char *name = JS_ToCString(ctx, cb_name);
                    const char *ctor_name_c = JS_IsString(ctor_name) ? JS_ToCString(ctx, ctor_name) : NULL;
                    platform_log(LOG_LEVEL_WARN, "timer", "Timer callback name=%s length=%d ctor=%s", name ? name : "?", cb_len_i, ctor_name_c ? ctor_name_c : "?");
                    if (name) JS_FreeCString(ctx, name);
                    if (ctor_name_c) JS_FreeCString(ctx, ctor_name_c);
                    GCValue toString_fn = JS_GetPropertyStr(ctx, callback, "toString");
                    if (JS_IsFunction(ctx, toString_fn)) {
                        GCValue cb_str_val = JS_Call(ctx, toString_fn, callback, 0, NULL);
                        const char *cb_str = JS_ToCString(ctx, cb_str_val);
                        if (cb_str) {
                            platform_log(LOG_LEVEL_WARN, "timer", "Timer callback toString (first 2000 chars): %.2000s", cb_str);
                        }
                    }
                }
            }
        }
        (void)result; // Result is ignored for timer callbacks
    }
    // For one-shot timers, unregister roots after execution (they won't be accessed again)
    // For intervals, keep roots registered as they'll be called again
    if (timer->type == TIMER_TYPE_TIMEOUT || timer->type == TIMER_TYPE_RAF || timer->type == TIMER_TYPE_IDLE_CALLBACK) {
        // We need to re-acquire lock to safely access timer state
        pthread_mutex_lock(&g_timer_state.mutex);
        unregister_timer_roots(timer);
        pthread_mutex_unlock(&g_timer_state.mutex);
    }
}

// Process all due timers - called from interrupt handler
// Returns 1 if any timers were processed
extern "C" int timer_process_due(JSContextHandle ctx) {
    unsigned long long now = platform_get_time_ms();
    int processed = 0;
    
    // Process up to 10 timers per call to avoid blocking too long
    for (int i = 0; i < 10; i++) {
        int due_id = find_due_timer(now);
        if (due_id == 0) break;
        
        execute_timer(ctx, due_id);
        processed++;
    }
    
    return processed;
}

// Check if any timer is pending (for interrupt handler)
extern "C" int timer_has_pending(void) {
    pthread_mutex_lock(&g_timer_state.mutex);
    
    unsigned long long now = platform_get_time_ms();
    int has_pending = 0;
    
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timer_state.timers[i].active && g_timer_state.timers[i].trigger_time <= now) {
            has_pending = 1;
            break;
        }
    }
    
    pthread_mutex_unlock(&g_timer_state.mutex);
    return has_pending;
}

// ============================================================================
// JavaScript API Bindings
// ============================================================================

// setTimeout(callback, delay, ...args)
GCValue js_set_timeout(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    if (argc < 1) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Check if first argument is undefined or null
    if (JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Accept any object as callback (functions are objects in JS)
    // The actual call will fail gracefully if not callable
    if (!JS_IsObject(argv[0]) && !JS_IsFunction(ctx, argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Parse delay (default 0)
    unsigned long long delay = 0;
    if (argc >= 2) {
        double delay_ms;
        if (JS_ToFloat64(ctx, &delay_ms, argv[1]) == 0) {
            delay = (unsigned long long)(delay_ms > 0 ? delay_ms : 0);
        }
    }
    
    // Extract additional arguments
    GCValue args[MAX_TIMER_ARGS];
    int arg_count = 0;
    for (int i = 2; i < argc && arg_count < MAX_TIMER_ARGS; i++, arg_count++) {
        args[arg_count] = argv[i];
    }
    
    int id = add_timer(ctx, TIMER_TYPE_TIMEOUT, argv[0], delay, arg_count, args);
    return JS_NewInt32(ctx, id);
}

// clearTimeout(id)
GCValue js_clear_timeout(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val;
    
    if (argc < 1) return JS_UNDEFINED;
    
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
        clear_timer_by_id(id);
    }
    
    return JS_UNDEFINED;
}

// setInterval(callback, delay, ...args)
GCValue js_set_interval(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Parse delay (default 0 if not specified, but spec says at least 4ms for intervals)
    unsigned long long delay = 4;  // Minimum 4ms per HTML spec
    if (argc >= 2) {
        double delay_ms;
        if (JS_ToFloat64(ctx, &delay_ms, argv[1]) == 0) {
            delay = (unsigned long long)(delay_ms > 4 ? delay_ms : 4);
        }
    }
    
    // Extract additional arguments
    GCValue args[MAX_TIMER_ARGS];
    int arg_count = 0;
    for (int i = 2; i < argc && arg_count < MAX_TIMER_ARGS; i++, arg_count++) {
        args[arg_count] = argv[i];
    }
    
    int id = add_timer(ctx, TIMER_TYPE_INTERVAL, argv[0], delay, arg_count, args);
    return JS_NewInt32(ctx, id);
}

// clearInterval(id)
GCValue js_clear_interval(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val;
    
    if (argc < 1) return JS_UNDEFINED;
    
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
        clear_timer_by_id(id);
    }
    
    return JS_UNDEFINED;
}

// requestAnimationFrame(callback)
GCValue js_request_animation_frame(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // RAF typically fires at 60fps (~16.67ms), but we'll use 0 for immediate execution
    int id = add_timer(ctx, TIMER_TYPE_RAF, argv[0], 0, 0, NULL);
    return JS_NewInt32(ctx, id);
}

// cancelAnimationFrame(id)
GCValue js_cancel_animation_frame(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val;
    
    if (argc < 1) return JS_UNDEFINED;
    
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
        clear_timer_by_id(id);
    }
    
    return JS_UNDEFINED;
}

// requestIdleCallback(callback, options)
GCValue js_request_idle_callback(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_NewInt32(ctx, 0);
    }
    
    // Parse timeout from options (default 0)
    unsigned long long timeout = 0;
    if (argc >= 2 && JS_IsObject(argv[1])) {
        GCValue timeout_val = JS_GetPropertyStr(ctx, argv[1], "timeout");
        double timeout_ms;
        if (JS_ToFloat64(ctx, &timeout_ms, timeout_val) == 0) {
            timeout = (unsigned long long)(timeout_ms > 0 ? timeout_ms : 0);
        }
    }
    
    int id = add_timer(ctx, TIMER_TYPE_IDLE_CALLBACK, argv[0], timeout, 0, NULL);
    return JS_NewInt32(ctx, id);
}

// cancelIdleCallback(id)
GCValue js_cancel_idle_callback(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)ctx; (void)this_val;
    
    if (argc < 1) return JS_UNDEFINED;
    
    int32_t id;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
        clear_timer_by_id(id);
    }
    
    return JS_UNDEFINED;
}


// ============================================================================
// Scheduler API Implementation (scheduler.postTask / scheduler.yield)
// ============================================================================

#define MAX_SCHEDULER_TASKS 256
#define SCHEDULER_TIME_SLICE_MS 8

typedef enum {
    SCHED_PRIO_USER_BLOCKING = 0,
    SCHED_PRIO_USER_VISIBLE = 1,
    SCHED_PRIO_BACKGROUND = 2,
    SCHED_PRIO_COUNT = 3
} SchedulerPriority;

typedef struct SchedulerTask {
    int id;
    int active;
    SchedulerPriority priority;
    unsigned long long scheduled_time;  /* earliest time the task may run */
    int callback_handle;
    int resolve_handle;
    int reject_handle;
    int arg_count;
    int arg_handles[MAX_TIMER_ARGS];
    struct SchedulerTask *next;
} SchedulerTask;

static SchedulerTask g_scheduler_tasks[MAX_SCHEDULER_TASKS];
static SchedulerTask *g_scheduler_queues[SCHED_PRIO_COUNT];
static int g_scheduler_initialized = 0;
static int g_scheduler_next_id = 1;

static void scheduler_state_ensure_initialized(void) {
    if (!g_scheduler_initialized) {
        memset(g_scheduler_tasks, 0, sizeof(g_scheduler_tasks));
        memset(g_scheduler_queues, 0, sizeof(g_scheduler_queues));
        g_scheduler_next_id = 1;
        g_scheduler_initialized = 1;
    }
}

static SchedulerTask* scheduler_task_alloc(void) {
    scheduler_state_ensure_initialized();
    for (int i = 0; i < MAX_SCHEDULER_TASKS; i++) {
        if (!g_scheduler_tasks[i].active) {
            memset(&g_scheduler_tasks[i], 0, sizeof(SchedulerTask));
            g_scheduler_tasks[i].active = 1;
            g_scheduler_tasks[i].callback_handle = -1;
            g_scheduler_tasks[i].resolve_handle = -1;
            g_scheduler_tasks[i].reject_handle = -1;
            return &g_scheduler_tasks[i];
        }
    }
    return NULL;
}

static void unroot_callback_handle(int handle) {
    if (handle < 0 || handle >= g_timer_state.callback_count) return;
    GCValue val = g_timer_state.callbacks[handle];
    if (gcvalue_is_reference(val)) {
        GCHandle h = GC_VALUE_GET_HANDLE(val);
        if (h != GC_HANDLE_NULL) {
            gc_remove_root(h);
        }
    }
}

static void scheduler_task_free(SchedulerTask *task) {
    if (!task || !task->active) return;
    unroot_callback_handle(task->callback_handle);
    unroot_callback_handle(task->resolve_handle);
    unroot_callback_handle(task->reject_handle);
    for (int i = 0; i < task->arg_count; i++) {
        unroot_callback_handle(task->arg_handles[i]);
    }
    task->active = 0;
}

static SchedulerPriority scheduler_parse_priority(const char *s) {
    if (!s) return SCHED_PRIO_USER_VISIBLE;
    if (strcasecmp(s, "user-blocking") == 0) return SCHED_PRIO_USER_BLOCKING;
    if (strcasecmp(s, "background") == 0) return SCHED_PRIO_BACKGROUND;
    return SCHED_PRIO_USER_VISIBLE;
}

static void scheduler_insert_task(SchedulerTask *task) {
    if (!task) return;
    SchedulerTask **queue = &g_scheduler_queues[task->priority];
    SchedulerTask *prev = NULL;
    SchedulerTask *cur = *queue;
    while (cur && cur->scheduled_time <= task->scheduled_time) {
        prev = cur;
        cur = cur->next;
    }
    task->next = cur;
    if (prev) prev->next = task;
    else *queue = task;
}

static void scheduler_remove_task(SchedulerTask *task) {
    if (!task) return;
    SchedulerTask **queue = &g_scheduler_queues[task->priority];
    SchedulerTask *prev = NULL;
    SchedulerTask *cur = *queue;
    while (cur) {
        if (cur == task) {
            if (prev) prev->next = cur->next;
            else *queue = cur->next;
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static GCValue scheduler_create_promise(JSContextHandle ctx, int *out_resolve_handle, int *out_reject_handle) {
    GCValue resolving_funcs[2];
    GCValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    *out_resolve_handle = store_callback(ctx, resolving_funcs[0]);
    *out_reject_handle = store_callback(ctx, resolving_funcs[1]);
    return promise;
}

static GCValue scheduler_post_task(JSContextHandle ctx, GCValue callback, const char *priority_str,
                                   unsigned long long delay, GCValue *args, int arg_count) {
    scheduler_state_ensure_initialized();
    if (!JS_IsFunction(ctx, callback)) {
        return JS_ThrowTypeError(ctx, "scheduler.postTask: callback must be a function");
    }
    SchedulerTask *task = scheduler_task_alloc();
    if (!task) {
        return JS_ThrowInternalError(ctx, "scheduler task queue full");
    }
    task->id = g_scheduler_next_id++;
    task->priority = scheduler_parse_priority(priority_str);
    task->scheduled_time = platform_get_time_ms() + delay;
    task->callback_handle = store_callback(ctx, callback);
    GCValue promise = scheduler_create_promise(ctx, &task->resolve_handle, &task->reject_handle);
    task->arg_count = 0;
    for (int i = 0; i < arg_count && i < MAX_TIMER_ARGS; i++) {
        task->arg_handles[i] = store_callback(ctx, args[i]);
        task->arg_count++;
    }
    scheduler_insert_task(task);
    return promise;
}

static GCValue scheduler_yield(JSContextHandle ctx, const char *priority_str) {
    scheduler_state_ensure_initialized();
    SchedulerTask *task = scheduler_task_alloc();
    if (!task) {
        return JS_ThrowInternalError(ctx, "scheduler task queue full");
    }
    task->id = g_scheduler_next_id++;
    task->priority = scheduler_parse_priority(priority_str);
    task->scheduled_time = platform_get_time_ms();
    GCValue promise = scheduler_create_promise(ctx, &task->resolve_handle, &task->reject_handle);
    /* No callback: yield resolves with undefined once the task is processed. */
    scheduler_insert_task(task);
    return promise;
}

extern "C" int scheduler_process_tasks(JSContextHandle ctx) {
    scheduler_state_ensure_initialized();
    unsigned long long start = platform_get_time_ms();
    unsigned long long now = start;
    int processed = 0;
    bool keep_going = true;
    while (keep_going) {
        keep_going = false;
        for (int p = 0; p < SCHED_PRIO_COUNT; p++) {
            SchedulerTask *task = g_scheduler_queues[p];
            while (task && task->scheduled_time <= now) {
                /* Respect the time slice so scheduler tasks don't starve the rendering loop. */
                if (platform_get_time_ms() - start >= SCHEDULER_TIME_SLICE_MS) {
                    return processed;
                }
                scheduler_remove_task(task);

                GCValue callback = get_callback(task->callback_handle);
                GCValue sargs[MAX_TIMER_ARGS];
                int sarg_count = task->arg_count;
                for (int i = 0; i < sarg_count; i++) {
                    sargs[i] = get_callback(task->arg_handles[i]);
                }

                GCValue resolve = get_callback(task->resolve_handle);
                GCValue reject = get_callback(task->reject_handle);

                if (JS_IsFunction(ctx, callback)) {
                    GCValue result = JS_Call(ctx, callback, JS_UNDEFINED, sarg_count, sargs);
                    if (JS_IsException(result)) {
                        GCValue exc = JS_GetException(ctx);
                        if (JS_IsFunction(ctx, reject)) {
                            JS_Call(ctx, reject, JS_UNDEFINED, 1, &exc);
                        }
                    } else {
                        if (JS_IsFunction(ctx, resolve)) {
                            JS_Call(ctx, resolve, JS_UNDEFINED, 1, &result);
                        }
                    }
                } else if (JS_IsFunction(ctx, resolve)) {
                    /* scheduler.yield: resolve with undefined. */
                    GCValue undef = JS_UNDEFINED;
                    JS_Call(ctx, resolve, JS_UNDEFINED, 1, &undef);
                }

                scheduler_task_free(task);
                processed++;
                now = platform_get_time_ms();

                task = g_scheduler_queues[p]; /* head may have changed */
            }
            if (g_scheduler_queues[p] && g_scheduler_queues[p]->scheduled_time <= now) {
                keep_going = true; /* higher-priority queue may have new eligible tasks */
            }
        }
    }
    return processed;
}

extern "C" void scheduler_reset(void) {
    scheduler_state_ensure_initialized();
    for (int i = 0; i < MAX_SCHEDULER_TASKS; i++) {
        if (g_scheduler_tasks[i].active) {
            scheduler_task_free(&g_scheduler_tasks[i]);
        }
    }
    memset(g_scheduler_tasks, 0, sizeof(g_scheduler_tasks));
    memset(g_scheduler_queues, 0, sizeof(g_scheduler_queues));
    g_scheduler_next_id = 1;
}

// scheduler.postTask(callback, options)
GCValue js_scheduler_post_task(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "scheduler.postTask requires a callback function");
    }
    GCValue callback = argv[0];
    const char *priority = "user-visible";
    const char *prio_str = NULL;
    unsigned long long delay = 0;
    GCValue args[MAX_TIMER_ARGS];
    int arg_count = 0;

    if (argc >= 2 && JS_IsObject(argv[1])) {
        GCValue prio_val = JS_GetPropertyStr(ctx, argv[1], "priority");
        prio_str = JS_IsString(prio_val) ? JS_ToCString(ctx, prio_val) : NULL;
        if (prio_str) {
            priority = prio_str;
        }
        GCValue delay_val = JS_GetPropertyStr(ctx, argv[1], "delay");
        double delay_ms = 0;
        if (JS_IsNumber(delay_val) && JS_ToFloat64(ctx, &delay_ms, delay_val) == 0) {
            if (delay_ms > 0) delay = (unsigned long long)delay_ms;
        }
        GCValue args_val = JS_GetPropertyStr(ctx, argv[1], "args");
        if (JS_IsArray(ctx, args_val)) {
            int32_t len = 0;
            JS_ToInt32(ctx, &len, JS_GetPropertyStr(ctx, args_val, "length"));
            if (len > MAX_TIMER_ARGS) len = MAX_TIMER_ARGS;
            for (int32_t i = 0; i < len; i++) {
                args[arg_count++] = JS_GetPropertyUint32(ctx, args_val, (uint32_t)i);
            }
        }
    }

    GCValue result = scheduler_post_task(ctx, callback, priority, delay, args, arg_count);
    if (prio_str) JS_FreeCString(ctx, prio_str);
    return result;
}

// scheduler.yield(options)
GCValue js_scheduler_yield(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    const char *priority = "user-visible";
    const char *prio_str = NULL;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        GCValue prio_val = JS_GetPropertyStr(ctx, argv[0], "priority");
        prio_str = JS_IsString(prio_val) ? JS_ToCString(ctx, prio_val) : NULL;
        if (prio_str) {
            priority = prio_str;
        }
    }
    GCValue result = scheduler_yield(ctx, priority);
    if (prio_str) JS_FreeCString(ctx, prio_str);
    return result;
}
