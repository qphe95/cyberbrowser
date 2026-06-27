#ifndef HTTP_DOWNLOAD_H
#define HTTP_DOWNLOAD_H

#include <stdbool.h>
#include <stddef.h>

/* Android JNI support - only included when building for Android */
#ifdef BE_PLATFORM_ANDROID
#include <jni.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HttpBuffer {
    char *data;
    size_t size;
} HttpBuffer;

/* Shared state for async downloads. UI thread polls this; download thread writes it.
 * All fields are updated with atomic operations; no mutex needed. */
typedef struct {
    size_t bytes_downloaded;
    size_t bytes_total;
    int state; /* 0=idle, 1=probing, 2=downloading, 3=concatenating, 4=done, 5=error */
    char status[256];
} DownloadState;

void download_state_init(DownloadState *state);
void download_state_reset(DownloadState *state);

bool http_get_to_memory(const char *url, HttpBuffer *outBuffer,
                        char *err, size_t errLen);

bool http_get_to_memory_with_headers(const char *url, const char **headers, size_t headerCount,
                                     HttpBuffer *outBuffer, char *err, size_t errLen);

bool http_post_to_memory(const char *url, const char *postData, size_t postDataLen,
                         const char **headers, size_t headerCount,
                         HttpBuffer *outBuffer, int *outStatus,
                         char *err, size_t errLen);

void http_free_buffer(HttpBuffer *buffer);

/* Main file download API. Pass a DownloadState pointer for progress tracking.
 * The download thread updates state->bytes_downloaded and state->state directly.
 * The caller polls state at its own frame rate. */
bool http_download_to_file(const char *url, const char *filePath,
                           DownloadState *state,
                           char *err, size_t errLen);

/* Cookie management for media downloads */
void http_set_cookies(const char *cookies);
const char* http_get_cookies(void);
void http_clear_cookies(void);

/* Android-specific functions - only available on Android */
#ifdef BE_PLATFORM_ANDROID
// WebView-based downloading
void http_download_via_webview(const char *url, void *app);

// WebView session management
void http_download_set_jni_refs(JavaVM *vm, jobject activity);
void http_download_load_page(const char *url);
void http_download_set_cookies(const char *cookies);
void http_download_set_js_session_data(const char *session);
#endif

#ifdef __cplusplus
}
#endif

#endif
