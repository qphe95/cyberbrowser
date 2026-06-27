/*
 * Platform Abstraction Layer
 * 
 * This header defines the platform-independent interface that the cyberbrowser
 * module uses. Platform-specific implementations provide the actual functionality.
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#ifdef _WIN32
#include "win32_compat.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Logging
 * ============================================================================ */

typedef enum {
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR
} LogLevel;

/* Set the minimum log level to output */
void platform_log_set_level(LogLevel level);

/* Log a message with the given level and tag */
void platform_log(LogLevel level, const char *tag, const char *fmt, ...);

/* Log a message with va_list */
void platform_vlog(LogLevel level, const char *tag, const char *fmt, va_list args);

/* Convenience macros */
#define PLATFORM_LOGD(tag, ...) platform_log(LOG_LEVEL_DEBUG, tag, __VA_ARGS__)
#define PLATFORM_LOGI(tag, ...) platform_log(LOG_LEVEL_INFO, tag, __VA_ARGS__)
#define PLATFORM_LOGW(tag, ...) platform_log(LOG_LEVEL_WARN, tag, __VA_ARGS__)
#define PLATFORM_LOGE(tag, ...) platform_log(LOG_LEVEL_ERROR, tag, __VA_ARGS__)

/* ============================================================================
 * Memory Allocation
 * ============================================================================ */

/* Platform-specific memory allocation (may add tracking/alignment) */
void* platform_malloc(size_t size);
void* platform_calloc(size_t nmemb, size_t size);
void* platform_realloc(void *ptr, size_t size);
void platform_free(void *ptr);

/* ============================================================================
 * HTTP Client
 * ============================================================================ */

typedef struct PlatformHttpBuffer {
    char *data;
    size_t size;
    char *headers;      /* raw response header block */
    size_t headers_size;
} PlatformHttpBuffer;

typedef void (*PlatformHttpProgressCallback)(size_t downloaded, size_t total, void *user);

/* Initialize HTTP subsystem */
bool platform_http_init(void);

/* Cleanup HTTP subsystem */
void platform_http_cleanup(void);

/* Perform HTTP GET request, returning response in outBuffer */
bool platform_http_get(const char *url, 
                       PlatformHttpBuffer *outBuffer,
                       char *error, size_t errorLen);

/* Perform HTTP GET with custom headers */
bool platform_http_get_with_headers(const char *url,
                                    const char **headers, size_t headerCount,
                                    PlatformHttpBuffer *outBuffer,
                                    char *error, size_t errorLen);

/* Perform HTTP POST request with body */
bool platform_http_post(const char *url,
                        const char *postData, size_t postDataLen,
                        const char **headers, size_t headerCount,
                        PlatformHttpBuffer *outBuffer,
                        int *outStatus,
                        char *error, size_t errorLen);

/* Generic HTTP request with arbitrary method (used for CORS preflight, etc.) */
bool platform_http_request(const char *url,
                           const char *method,
                           const char *postData, size_t postDataLen,
                           const char **headers, size_t headerCount,
                           PlatformHttpBuffer *outBuffer,
                           int *outStatus,
                           char *error, size_t errorLen);

/* Free HTTP buffer */
void platform_http_free_buffer(PlatformHttpBuffer *buffer);

/* Set global cookies for HTTP requests */
void platform_http_set_cookies(const char *cookies);

/* Get current global cookies */
const char* platform_http_get_cookies(void);

/* Clear global cookies */
void platform_http_clear_cookies(void);

/* ============================================================================
 * File System
 * ============================================================================ */

/* Platform-independent file handle */
typedef struct PlatformFile PlatformFile;

/* Open a file for writing (creates/truncates) */
PlatformFile* platform_file_open_write(const char *path);

/* Open a file for appending (creates if doesn't exist) */
PlatformFile* platform_file_open_append(const char *path);

/* Write data to file, returns bytes written */
size_t platform_file_write(PlatformFile *file, const void *data, size_t size);

/* Flush file to disk */
void platform_file_flush(PlatformFile *file);

/* Close file */
void platform_file_close(PlatformFile *file);

/* Read entire file into buffer (returns malloc'd buffer, caller frees) */
char* platform_file_read_all(const char *path, size_t *outSize);

/* Check if file exists */
bool platform_file_exists(const char *path);

/* Get temporary directory path (null-terminated, includes trailing separator) */
const char* platform_get_temp_dir(void);

/* ============================================================================
 * Threading/Mutex
 * ============================================================================ */

typedef struct PlatformMutex PlatformMutex;

/* Create a mutex */
PlatformMutex* platform_mutex_create(void);

/* Destroy a mutex */
void platform_mutex_destroy(PlatformMutex *mutex);

/* Lock mutex */
void platform_mutex_lock(PlatformMutex *mutex);

/* Unlock mutex */
void platform_mutex_unlock(PlatformMutex *mutex);

/* ============================================================================
 * Time
 * ============================================================================ */

/* Get current time in milliseconds since epoch */
unsigned long long platform_get_time_ms(void);

/* Sleep for specified milliseconds */
void platform_sleep_ms(unsigned int ms);

/* ============================================================================
 * Asset Loading (for embedded JS stubs)
 * ============================================================================ */

/* Read asset file (for browser stubs JS files bundled with the app) */
/* Returns malloc'd buffer, caller frees. Returns NULL if not found. */
char* platform_asset_read(const char *assetName, size_t *outSize);

/* ============================================================================
 * Media Saving
 * ============================================================================ */

/* Initialize media save subsystem (creates output directory).
 * app_name is used as a subdirectory name (e.g., "BGMDWLDR").
 * On POSIX: creates ~/Music/<app_name>/
 * On Android: no-op (use media_store.c directly, or extend this later) */
bool platform_media_save_init(const char *app_name, char *err, size_t err_len);

/* Initialize media save subsystem for video files (creates output directory).
 * app_name is used as a subdirectory name (e.g., "BGMDWLDR").
 * On POSIX: creates ~/Movies/<app_name>/
 * On Windows: creates <Videos folder>\<app_name>\
 * On Android: no-op */
bool platform_media_save_init_video(const char *app_name, char *err, size_t err_len);

/* Save audio/video data to the platform's media storage.
 * filename: base name only (e.g., "bgm.mp4")
 * mime_type: MIME type hint (e.g., "audio/mp4")
 * Returns true on success. */
bool platform_media_save_audio(const char *data, size_t size,
                               const char *filename, const char *mime_type,
                               char *err, size_t err_len);

/* Get the full output directory path (for info/logging).
 * Returns a static buffer valid until next call. Do not free. */
const char* platform_media_save_get_path(void);

/* Cleanup media save subsystem */
void platform_media_save_cleanup(void);

/* ============================================================================
 * Initialization
 * ============================================================================ */

/* Initialize platform layer (call before any other platform functions) */
bool platform_init(void);

/* Cleanup platform layer */
void platform_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_H */
