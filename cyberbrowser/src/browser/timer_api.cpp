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
        
        // For idle callback, pass a mock IdleDeadline object
        if (timer->type == TIMER_TYPE_IDLE_CALLBACK && arg_count == 0) {
            GCValue idle_deadline = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, idle_deadline, "didTimeout", JS_FALSE);
            // timeRemaining returns always some positive number
            JS_SetPropertyStr(ctx, idle_deadline, "timeRemaining", 
                JS_NewCFunction(ctx, js_true, "timeRemaining", 0));
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
            // YouTube's internal analytics / error-reporting paths (compiled into
            // dynamic functions and tagged <lazy>) throw benign errors such as
            // "Error: Ec" and "TypeError: not a function" in this headless
            // context. They do not affect page functionality, so suppress the
            // noisy warning while preserving logging for unexpected exceptions.
            bool is_benign_yt_analytics = false;
            if (exc_str && stack && strstr(stack, "<lazy>")) {
                if (strstr(exc_str, "Error: Ec") == exc_str ||
                    strcmp(exc_str, "TypeError: not a function") == 0) {
                    is_benign_yt_analytics = true;
                }
            }
            if (!is_benign_yt_analytics) {
                if (exc_str) {
                    platform_log(LOG_LEVEL_WARN, "timer", "Timer callback exception: %s", exc_str);
                }
                if (stack) {
                    platform_log(LOG_LEVEL_WARN, "timer", "Timer callback stack: %s", stack);
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

