/*
 * Platform Abstraction Layer - Common Implementation
 */

#include "platform.h"
#include <stdlib.h>
#include <string.h>

/* Default log level - can be overridden via BROWSER_EMULATOR_LOG environment variable
 * Levels: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
 * Default is WARN to suppress verbose output during normal operation
 */
static LogLevel g_log_level = LOG_LEVEL_WARN;
static int g_log_level_initialized = 0;

static void init_log_level(void) {
    if (g_log_level_initialized) return;
    
    const char *env = getenv("BROWSER_EMULATOR_LOG");
    if (env) {
        if (strcmp(env, "DEBUG") == 0 || strcmp(env, "0") == 0) {
            g_log_level = LOG_LEVEL_DEBUG;
        } else if (strcmp(env, "INFO") == 0 || strcmp(env, "1") == 0) {
            g_log_level = LOG_LEVEL_INFO;
        } else if (strcmp(env, "WARN") == 0 || strcmp(env, "2") == 0) {
            g_log_level = LOG_LEVEL_WARN;
        } else if (strcmp(env, "ERROR") == 0 || strcmp(env, "3") == 0) {
            g_log_level = LOG_LEVEL_ERROR;
        }
    }
    g_log_level_initialized = 1;
}

void platform_log_set_level(LogLevel level) {
    g_log_level = level;
    g_log_level_initialized = 1;
}

LogLevel platform_get_log_level(void) {
    if (!g_log_level_initialized) {
        init_log_level();
    }
    return g_log_level;
}

void* platform_malloc(size_t size) {
    return malloc(size);
}

void* platform_calloc(size_t nmemb, size_t size) {
    return calloc(nmemb, size);
}

void* platform_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

void platform_free(void *ptr) {
    free(ptr);
}
