#ifndef HTTP_DOWNLOAD_H
#define HTTP_DOWNLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
    char *headers;      /* raw response header block (HTTP/1.1 ... \r\n...\r\n) */
    size_t headers_size;
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

/* As http_get_to_memory_with_headers, also handing the body to `observe` as
 * it arrives: de-chunked and decoded (identity or br; a body in another
 * coding is not observed), in pieces, on the thread that reads it (the
 * caller's).  For starting what the body asks for (a preload scan) before
 * the download ends; the buffer still gets the whole body. */
typedef void (*HttpBodyObserver)(void *user, const char *data, size_t len);
bool http_get_to_memory_observed(const char *url, const char **headers, size_t headerCount,
                                 HttpBuffer *outBuffer, char *err, size_t errLen,
                                 HttpBodyObserver observe, void *user);

bool http_request_to_memory(const char *url, const char *method,
                            const char *postData, size_t postDataLen,
                            const char **headers, size_t headerCount,
                            HttpBuffer *outBuffer, int *outStatus,
                            char *err, size_t errLen);

bool http_post_to_memory(const char *url, const char *postData, size_t postDataLen,
                         const char **headers, size_t headerCount,
                         HttpBuffer *outBuffer, int *outStatus,
                         char *err, size_t errLen);

void http_free_buffer(HttpBuffer *buffer);

/* Keep-alive connection pool (shared by every request; see tls_client.h).
 * http_net_log_stats prints a one-line summary to stderr;
 * http_net_close_idle closes the parked connections (at shutdown). */
void http_net_log_stats(void);
void http_net_close_idle(void);

/* Main file download API. Pass a DownloadState pointer for progress tracking.
 * The download thread updates state->bytes_downloaded and state->state directly.
 * The caller polls state at its own frame rate. */
bool http_download_to_file(const char *url, const char *filePath,
                           DownloadState *state,
                           char *err, size_t errLen);

/* Cookie management for media downloads */
void http_cookie_set_for_request(const char *url, const char *set_cookie_header);
void http_cookie_set_for_document(const char *url, const char *cookie_line);
const char* http_cookie_get_for_request(const char *url);
const char* http_cookie_get_for_document(const char *url);
void http_cookie_clear(void);

/* Cookie jars.  A thread's requests and cookie calls use the jar it chose
 * (NULL: the process's).  The network process keeps one per site. */
typedef struct HttpCookieJar HttpCookieJar;
HttpCookieJar *http_cookie_jar_new(void);
void http_cookie_jar_use(HttpCookieJar *jar);
/* Changes with every cookie written to the jar. */
uint64_t http_cookie_jar_version(HttpCookieJar *jar);
/* The jar's persistent cookies as text (malloc'd), and back. */
char *http_cookie_jar_save(HttpCookieJar *jar, size_t *len);
void http_cookie_jar_load(HttpCookieJar *jar, const char *data, size_t len);

/* A process with no network of its own (a tab): every request and
 * document.cookie access goes to these, and the network code refuses to
 * run.  request: as http_request_to_memory (the body in out->data,
 * NUL-terminated; the response headers in out->headers). */
typedef struct HttpRemote {
    bool (*request)(const char *url, const char *method, const char *body, size_t body_len,
                    const char **headers, size_t nheaders, HttpBuffer *out, int *status,
                    char *err, size_t err_len);
    char *(*cookie_get_for_document)(const char *url);      /* malloc'd, or NULL */
    void (*cookie_set_for_document)(const char *url, const char *line);
} HttpRemote;
void http_set_remote(const HttpRemote *remote);

/* What the requests this thread makes next are for (SANDBOX_DESIGN.md
 * section 2.4), which the network process needs for ORB, CORS and Private
 * Network Access.  The caller sets it before a request and restores the
 * previous one after (http_request_context_get).  Unset: no-cors, from the
 * page's own origin. */
enum {
    HTTP_MODE_NO_CORS = 0,      /* subresources: images, scripts, styles, fonts, media */
    HTTP_MODE_CORS,             /* fetch / XMLHttpRequest across origins */
    HTTP_MODE_NAVIGATE,         /* the page's own document */
    HTTP_MODE_FRAME,            /* an iframe's document */
};
typedef struct HttpRequestContext {
    int mode;                   /* HTTP_MODE_* */
    char initiator[512];        /* the requesting document's origin; "" = the page's */
} HttpRequestContext;
void http_request_context_set(int mode, const char *initiator);
void http_request_context_get(HttpRequestContext *out);

/* The URL the last request on this thread ended at (after redirects).  A
 * remote (HttpRemote) sets it from its answer with http_set_last_url. */
const char *http_last_url(void);
void http_set_last_url(const char *url);

/* Redirects on this thread go to filter first (NULL: none): false and the
 * request stops there, failing.  A relative Location is resolved against
 * the URL it came from; more than 20 hops fail. */
typedef bool (*HttpRedirectFilter)(const char *next_url, void *user);
void http_set_redirect_filter(HttpRedirectFilter filter, void *user);

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
