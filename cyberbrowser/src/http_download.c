#include "http_download.h"
#include "tls_client.h"
#include "utf8_filename.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <time.h>
#include <pthread.h>
#include "platform.h"


#define LOG_TAG "http_download"
#define LOGI(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) platform_log(LOG_LEVEL_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

#ifdef _MSC_VER
#include <windows.h>
static inline void atomic_store_size(size_t *ptr, size_t val) {
    InterlockedExchange64((LONG64*)ptr, (LONG64)val);
}
static inline void atomic_store_int(int *ptr, int val) {
    InterlockedExchange((LONG*)ptr, (LONG)val);
}
static inline void atomic_add_size(size_t *ptr, size_t val) {
    InterlockedAdd64((LONG64*)ptr, (LONG64)val);
}
static inline size_t atomic_load_size(size_t *ptr) {
    return *ptr;
}
#define __atomic_store_n(ptr, val, order) \
    (sizeof(*(ptr)) == 8 ? (void)atomic_store_size((size_t*)(ptr), (size_t)(val)) : \
     sizeof(*(ptr)) == 4 ? (void)atomic_store_int((int*)(ptr), (int)(val)) : \
     (void)(*(ptr) = (val)))
#define __atomic_add_fetch(ptr, val, order) \
    (sizeof(*(ptr)) == 8 ? (atomic_add_size((size_t*)(ptr), (size_t)(val)), *(ptr)) : \
     sizeof(*(ptr)) == 4 ? (atomic_add_size((size_t*)(ptr), (size_t)(val)), *(ptr)) : \
     (*(ptr) += (val)))
#define __atomic_sub_fetch(ptr, val, order) \
    (sizeof(*(ptr)) == 8 ? (atomic_add_size((size_t*)(ptr), -(size_t)(val)), *(ptr)) : \
     sizeof(*(ptr)) == 4 ? (atomic_add_size((size_t*)(ptr), -(size_t)(val)), *(ptr)) : \
     (*(ptr) -= (val)))
#define __atomic_load_n(ptr, order) (*(ptr))
#endif

#define CHUNK_SIZE 8192

/* ============================================================================
 * DownloadState helpers (atomic, no mutex)
 * ============================================================================ */

void download_state_init(DownloadState *state) {
    memset(state, 0, sizeof(DownloadState));
}

void download_state_reset(DownloadState *state) {
    __atomic_store_n(&state->bytes_downloaded, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->bytes_total, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->state, 0, __ATOMIC_SEQ_CST);
    state->status[0] = '\0';
}

static void download_state_set(DownloadState *state, int new_state, const char *fmt, ...) {
    __atomic_store_n(&state->state, new_state, __ATOMIC_SEQ_CST);
    if (fmt) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(state->status, sizeof(state->status), fmt, args);
        va_end(args);
    }
}

static void download_state_set_progress(DownloadState *state, size_t downloaded, size_t total) {
    __atomic_store_n(&state->bytes_downloaded, downloaded, __ATOMIC_RELAXED);
    __atomic_store_n(&state->bytes_total, total, __ATOMIC_RELAXED);
}
#define MAX_REDIRECTS 5
#define MAX_RESUME_CHUNKS 50

/* Simple HTTP response parser */
typedef struct HttpResponse {
    int status_code;
    char *body;
    size_t body_len;
    size_t body_capacity;
    char location[2048];
    char cookies[4096];  /* Store cookies from response */
} HttpResponse;

/* Forward declarations */
static bool http_request_with_method_internal(const char *url, const char *method,
                                               const char *postData, size_t postDataLen,
                                               const char **custom_headers, size_t custom_header_count,
                                               HttpBuffer *outBuffer, int *outStatus,
                                               char *err, size_t errLen, const char *cookies,
                                               DownloadState *state,
                                               FILE *bodyStream, size_t *outTotalSize);
static bool http_request_with_method(const char *url, const char *method,
                                     const char *postData, size_t postDataLen,
                                     const char **custom_headers, size_t custom_header_count,
                                     HttpBuffer *outBuffer, int *outStatus,
                                     char *err, size_t errLen, const char *cookies,
                                     DownloadState *state,
                                     FILE *bodyStream);
static bool http_request_with_cookies(const char *url, HttpBuffer *outBuffer,
                                      char *err, size_t errLen, const char *cookies,
                                      DownloadState *state,
                                      FILE *bodyStream);
static bool http_request(const char *url, HttpBuffer *outBuffer,
                         char *err, size_t errLen,
                         DownloadState *state,
                         FILE *bodyStream);
static bool http_get_to_memory_resumable(const char *url, HttpBuffer *outBuffer,
                                         char *err, size_t errLen);

static bool parse_url(const char *url, char *host, size_t host_len,
                      char *path, size_t path_len, char *port, size_t port_len) {
    const char *p = url;
    
    /* Skip scheme */
    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        strncpy(port, "443", port_len);
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
        strncpy(port, "80", port_len);
    } else {
        strncpy(port, "443", port_len);
    }
    
    /* Extract host and path */
    const char *slash = strchr(p, '/');
    if (slash) {
        size_t host_sz = (size_t)(slash - p);
        if (host_sz >= host_len) host_sz = host_len - 1;
        memcpy(host, p, host_sz);
        host[host_sz] = '\0';
        
        strncpy(path, slash, path_len - 1);
        path[path_len - 1] = '\0';
    } else {
        strncpy(host, p, host_len - 1);
        host[host_len - 1] = '\0';
        strncpy(path, "/", path_len);
    }
    
    return true;
}

/* Global context to pass cookies between requests */
static char g_cookie_jar[4096] = {0};

void http_set_cookies(const char *cookies) {
    if (cookies) {
        strncpy(g_cookie_jar, cookies, sizeof(g_cookie_jar) - 1);
        g_cookie_jar[sizeof(g_cookie_jar) - 1] = '\0';
        LOGI("Set cookies: %.100s...", g_cookie_jar);
    }
}

const char* http_get_cookies(void) {
    return g_cookie_jar[0] ? g_cookie_jar : NULL;
}

void http_clear_cookies(void) {
    g_cookie_jar[0] = '\0';
}

static size_t parse_content_length_from_headers(const char *data, size_t data_len) {
    const char *p = data;
    const char *end = data + data_len;
    while (p < end) {
        if ((p == data || p[-1] == '\n') &&
            strncasecmp(p, "content-length:", 15) == 0) {
            const char *val = p + 15;
            while (val < end && (*val == ' ' || *val == '\t')) val++;
            size_t len = 0;
            while (val < end && *val >= '0' && *val <= '9') {
                len = len * 10 + (size_t)(*val - '0');
                val++;
            }
            return len;
        }
        p++;
    }
    return 0;
}

static size_t parse_content_range_total_size(const char *data, size_t data_len) {
    const char *p = data;
    const char *end = data + data_len;
    while (p < end) {
        if ((p == data || p[-1] == '\n') &&
            strncasecmp(p, "content-range:", 14) == 0) {
            const char *val = p + 14;
            while (val < end && (*val == ' ' || *val == '\t')) val++;
            /* Format: bytes X-Y/Z  or  bytes X-Y/* */
            const char *slash = NULL;
            for (const char *q = val; q < end && *q != '\r' && *q != '\n'; q++) {
                if (*q == '/') slash = q;
            }
            if (slash && slash + 1 < end) {
                if (slash[1] == '*') return 0; /* unknown total */
                size_t total = 0;
                const char *num = slash + 1;
                while (num < end && *num >= '0' && *num <= '9') {
                    total = total * 10 + (size_t)(*num - '0');
                    num++;
                }
                return total;
            }
        }
        p++;
    }
    return 0;
}

static bool http_request_with_method_internal(const char *url, const char *method,
                                               const char *postData, size_t postDataLen,
                                               const char **custom_headers, size_t custom_header_count,
                                               HttpBuffer *outBuffer, int *outStatus,
                                               char *err, size_t errLen, const char *cookies,
                                               DownloadState *state,
                                               FILE *bodyStream, size_t *outTotalSize) {
    char host[256] = {0};
    char path[2048] = {0};
    char port[8] = {0};
    
    if (!parse_url(url, host, sizeof(host), path, sizeof(path), port, sizeof(port))) {
        snprintf(err, errLen, "Failed to parse URL");
        return false;
    }
    
    TlsClient client = {0};
    if (!tls_client_connect(&client, host, port, err, errLen)) {
        return false;
    }
    
    /* Build HTTP request */
    char request[16384];
    int req_len = snprintf(request, sizeof(request),
             "%s %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
             "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8\r\n"
             "Accept-Language: en-US,en;q=0.9\r\n"
             "Accept-Encoding: identity\r\n"
             "Connection: close\r\n",
             method, path, host);
    
    /* Add custom headers */
    for (size_t i = 0; i < custom_header_count; i++) {
        if (custom_headers[i]) {
            req_len += snprintf(request + req_len, sizeof(request) - req_len,
                               "%s\r\n", custom_headers[i]);
        }
    }
    
    /* Add Content-Length for POST */
    if (postData && postDataLen > 0) {
        req_len += snprintf(request + req_len, sizeof(request) - req_len,
                           "Content-Length: %zu\r\n", postDataLen);
    }
    
    /* Add cookies if provided */
    if (cookies && cookies[0]) {
        req_len += snprintf(request + req_len, sizeof(request) - req_len,
                           "Cookie: %s\r\n", cookies);
        LOGI("Adding cookies to request for %s", host);
    }
    
    /* Add final CRLF */
    req_len += snprintf(request + req_len, sizeof(request) - req_len, "\r\n");
    
    ssize_t sent = tls_client_write(&client, (unsigned char *)request, strlen(request));
    if (sent < 0) {
        snprintf(err, errLen, "Failed to send request");
        tls_client_close(&client);
        return false;
    }
    LOGI("HTTP request sent: %zd bytes", sent);
    LOGI("Request headers:\n%.500s", request);
    
    /* Send POST body if present */
    if (postData && postDataLen > 0) {
        sent = tls_client_write(&client, (unsigned char *)postData, postDataLen);
        if (sent < 0) {
            snprintf(err, errLen, "Failed to send POST body");
            tls_client_close(&client);
            return false;
        }
        LOGI("POST body sent: %zd bytes", sent);
    }
    
    /* Set socket timeout */
    int sockfd = client.net.fd;
#ifndef _WIN32
    struct timeval tv;
    tv.tv_sec = 60;
    tv.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 10;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#else
    DWORD recv_timeout = 60000;
    DWORD send_timeout = 10000;
    setsockopt((SOCKET)sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_timeout, sizeof(recv_timeout));
    setsockopt((SOCKET)sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_timeout, sizeof(send_timeout));
#endif
    LOGI("Set socket timeout (read=60s, write=10s)");

    /* Read response */
    outBuffer->data = malloc(CHUNK_SIZE);
    outBuffer->size = 0;
    if (!outBuffer->data) {
        snprintf(err, errLen, "Out of memory");
        tls_client_close(&client);
        return false;
    }
    
    size_t capacity = CHUNK_SIZE;
    unsigned char buf[CHUNK_SIZE];
    ssize_t n;
    time_t start_time = time(NULL);
    const int max_header_time = 30;
    int max_body_time = 300;
    int zero_reads = 0;
    const int max_zero_reads = 6000;
    bool headers_parsed = false;
    size_t header_len_val = 0;
    size_t content_length_val = 0;
    size_t total_size_val = 0;
    bool body_streaming = false;
    size_t body_written = 0;
    
    LOGI("Starting to read HTTP response...");
    size_t last_size = 0;
    while ((n = tls_client_read(&client, buf, sizeof(buf))) > 0 ||
           (n == 0 && zero_reads < max_zero_reads)) {
        if (n == 0) {
            zero_reads++;
            platform_sleep_ms(10);
            int current_max_time = headers_parsed ? max_body_time : max_header_time;
            if (time(NULL) - start_time > current_max_time) {
                LOGE("HTTP read timeout after %d seconds", current_max_time);
                snprintf(err, errLen, "Download timeout");
                free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
                outBuffer->data = NULL;
                tls_client_close(&client);
                if (bodyStream) fclose(bodyStream);
                return false;
            }
            continue;
        }
        zero_reads = 0;
        if (outBuffer->size > last_size + 1024*1024) {
            LOGE("[DL] Received %zu bytes so far", outBuffer->size);
            last_size = outBuffer->size;
        }
        if (!headers_parsed) {
            if (outBuffer->size + (size_t)n > capacity) {
                capacity *= 2;
                char *new_data = realloc(outBuffer->data, capacity);
                if (!new_data) {
                    snprintf(err, errLen, "Out of memory");
                    free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
                    outBuffer->data = NULL;
                    tls_client_close(&client);
                    if (bodyStream) fclose(bodyStream);
                    return false;
                }
                outBuffer->data = new_data;
            }
            memcpy(outBuffer->data + outBuffer->size, buf, (size_t)n);
            outBuffer->size += (size_t)n;

            char *header_end = strstr(outBuffer->data, "\r\n\r\n");
            if (!header_end) header_end = strstr(outBuffer->data, "\n\n");
            if (header_end) {
                size_t delimiter_len = 4;
                if (header_end >= outBuffer->data + 1 &&
                    header_end[-1] == '\n' &&
                    (header_end == outBuffer->data || header_end[-2] != '\r')) {
                    delimiter_len = 2;
                }
                header_len_val = (size_t)(header_end - outBuffer->data) + delimiter_len;
                content_length_val = parse_content_length_from_headers(outBuffer->data, header_len_val);
                total_size_val = parse_content_range_total_size(outBuffer->data, header_len_val);
                if (total_size_val == 0 && content_length_val > 0) {
                    total_size_val = content_length_val;
                }
                headers_parsed = true;
                if (content_length_val > 0) {
                    max_body_time = 30 + (int)((content_length_val / (1024ULL * 1024ULL)) * 120);
                    if (max_body_time < 300) max_body_time = 300;
                    if (max_body_time > 7200) max_body_time = 7200;
                    LOGI("Content-Length: %zu bytes, body timeout: %d seconds",
                         content_length_val, max_body_time);
                }
                if (bodyStream) {
                    size_t body_so_far = outBuffer->size > header_len_val ? outBuffer->size - header_len_val : 0;
                    if (body_so_far > 0) {
                        size_t w = fwrite(outBuffer->data + header_len_val, 1, body_so_far, bodyStream);
                        if (w != body_so_far) {
                            snprintf(err, errLen, "Failed to write to file");
                            free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
                            outBuffer->data = NULL;
                            tls_client_close(&client);
                            fclose(bodyStream);
                            return false;
                        }
                        body_written += w;
                    }
                    outBuffer->size = header_len_val;
                    body_streaming = true;
                }
            }
        } else if (body_streaming) {
            size_t w = fwrite(buf, 1, (size_t)n, bodyStream);
            if (w != (size_t)n) {
                snprintf(err, errLen, "Failed to write to file");
                free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
                outBuffer->data = NULL;
                tls_client_close(&client);
                fclose(bodyStream);
                return false;
            }
            body_written += w;
        } else {
            if (outBuffer->size + (size_t)n > capacity) {
                capacity *= 2;
                char *new_data = realloc(outBuffer->data, capacity);
                if (!new_data) {
                    snprintf(err, errLen, "Out of memory");
                    free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
                    outBuffer->data = NULL;
                    tls_client_close(&client);
                    if (bodyStream) fclose(bodyStream);
                    return false;
                }
                outBuffer->data = new_data;
            }
            memcpy(outBuffer->data + outBuffer->size, buf, (size_t)n);
            outBuffer->size += (size_t)n;
        }
        if (state && headers_parsed && content_length_val > 0) {
            size_t downloaded = body_streaming ? body_written : (outBuffer->size > header_len_val ? outBuffer->size - header_len_val : 0);
            if (downloaded > content_length_val) downloaded = content_length_val;
            download_state_set_progress(state, downloaded, content_length_val);
        }
        
        /* If we have received all expected body bytes, stop reading */
        if (headers_parsed && content_length_val > 0) {
            size_t body_received = body_streaming ? body_written : (outBuffer->size > header_len_val ? outBuffer->size - header_len_val : 0);
            if (body_received >= content_length_val) {
                break;
            }
        }
        
        int current_max_time = headers_parsed ? max_body_time : max_header_time;
        if (time(NULL) - start_time > current_max_time) {
            LOGE("HTTP read timeout after %d seconds", current_max_time);
            snprintf(err, errLen, "Download timeout");
            free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
            outBuffer->data = NULL;
            tls_client_close(&client);
            return false;
        }
    }

    LOGI("Finished reading response: %zu bytes total", outBuffer->size);
    tls_client_close(&client);
    
    LOGI("Received %zu bytes response", outBuffer->size);
    if (outBuffer->size > 0) {
        char debug_buf[256];
        size_t debug_len = outBuffer->size < 100 ? outBuffer->size : 100;
        for (size_t i = 0; i < debug_len && i < sizeof(debug_buf)-4; i++) {
            unsigned char c = (unsigned char)outBuffer->data[i];
            if (c >= 32 && c < 127) {
                debug_buf[i] = c;
            } else {
                debug_buf[i] = '.';
            }
        }
        debug_buf[debug_len] = '\0';
        LOGI("Response start: [%s]", debug_buf);
    }
    
    char *header_end = strstr(outBuffer->data, "\r\n\r\n");
    if (!header_end) {
        header_end = strstr(outBuffer->data, "\n\n");
    }
    if (!header_end) {
        snprintf(err, errLen, "Invalid HTTP response (received %zu bytes)", outBuffer->size);
        free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
        outBuffer->data = NULL;
        return false;
    }
    
    /* Extract status code */
    int status = 0;
    sscanf(outBuffer->data, "HTTP/%*s %d", &status);
    if (outStatus) *outStatus = status;
    
    LOGI("HTTP status: %d", status);
    
    if (cookies && cookies[0]) {
        LOGI("Sending cookies: %.200s...", cookies);
    }
    
    /* Extract cookies from response headers */
    char *set_cookie = strstr(outBuffer->data, "Set-Cookie:");
    while (set_cookie) {
        set_cookie += 11;
        while (*set_cookie == ' ') set_cookie++;
        
        char *line_end = strchr(set_cookie, '\r');
        if (!line_end) line_end = strchr(set_cookie, '\n');
        if (!line_end) line_end = set_cookie + strlen(set_cookie);
        
        char *first_semi = strchr(set_cookie, ';');
        if (first_semi && first_semi < line_end) {
            size_t nv_len = (size_t)(first_semi - set_cookie);
            if (nv_len > 0 && nv_len < 2000) {
                char name_value[2048];
                memcpy(name_value, set_cookie, nv_len);
                name_value[nv_len] = '\0';
                
                char *eq = strchr(name_value, '=');
                if (eq && eq > name_value) {
                    *eq = '\0';
                    char *cookie_name = name_value;
                    char *cookie_value = eq + 1;
                    
                    if (strcmp(cookie_name, "Domain") != 0 && 
                        strcmp(cookie_name, "Path") != 0 &&
                        strcmp(cookie_name, "Expires") != 0 &&
                        strcmp(cookie_name, "Max-Age") != 0 &&
                        strcmp(cookie_name, "HttpOnly") != 0 &&
                        strcmp(cookie_name, "Secure") != 0 &&
                        strcmp(cookie_name, "SameSite") != 0) {
                        
                        bool duplicate = false;
                        char *search = g_cookie_jar;
                        while ((search = strstr(search, cookie_name)) != NULL) {
                            if (search == g_cookie_jar || search[-1] == ' ' && search[-2] == ';') {
                                if (search[strlen(cookie_name)] == '=') {
                                    duplicate = true;
                                    break;
                                }
                            }
                            search++;
                        }
                        
                        if (!duplicate) {
                            size_t current_len = strlen(g_cookie_jar);
                            size_t remaining = sizeof(g_cookie_jar) - current_len - 1;
                            size_t needed = strlen(cookie_name) + strlen(cookie_value) + 3;
                            if (g_cookie_jar[0]) {
                                needed += 2;
                            }
                            
                            if (remaining >= needed) {
                                if (g_cookie_jar[0]) {
                                    strncat(g_cookie_jar, "; ", remaining);
                                    remaining -= 2;
                                }
                                strncat(g_cookie_jar, cookie_name, remaining);
                                remaining -= strlen(cookie_name);
                                strncat(g_cookie_jar, "=", remaining);
                                remaining -= 1;
                                strncat(g_cookie_jar, cookie_value, remaining);
                                LOGI("Captured cookie: %s=...", cookie_name);
                            } else {
                                LOGE("Cookie buffer full, skipping: %s", cookie_name);
                            }
                        }
                    }
                }
            }
        }
        set_cookie = strstr(line_end, "Set-Cookie:");
    }
    
    /* Handle redirects */
    if (status >= 300 && status < 400) {
        char *location = strstr(outBuffer->data, "Location:");
        if (location) {
            location += 9;
            while (*location == ' ') location++;
            char redirect_url[2048];
            size_t i = 0;
            while (*location && *location != '\r' && *location != '\n' && i < sizeof(redirect_url) - 1) {
                redirect_url[i++] = *location++;
            }
            redirect_url[i] = '\0';
            
            LOGI("Redirect to: %s", redirect_url);
            
            if (strstr(redirect_url, "accounts.google.com") ||
                strstr(redirect_url, "ServiceLogin") ||
                strstr(redirect_url, "/signin") ||
                strstr(redirect_url, "/login")) {
                LOGE("Blocking redirect to authentication page: %s", redirect_url);
                snprintf(err, errLen, "Redirect to login page blocked");
                free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
                outBuffer->data = NULL;
                return false;
            }
            
            free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
            outBuffer->data = NULL;
            return http_request_with_method(redirect_url, method, postData, postDataLen,
                                            custom_headers, custom_header_count,
                                            outBuffer, outStatus, err, errLen, cookies, state, bodyStream);
        }
    }
    
    size_t delimiter_len = 4;
    if (status < 200 || status >= 300) {
        snprintf(err, errLen, "HTTP error %d", status);
        free(outBuffer->data);
        free(outBuffer->headers);
        outBuffer->headers = NULL;
        outBuffer->headers_size = 0;
        outBuffer->data = NULL;
        if (bodyStream) fclose(bodyStream);
        return false;
    }

    /* Capture response headers for callers that need them (CORS, etc.) */
    {
        size_t delimiter_len = 4;
        if (header_end >= outBuffer->data + 1 &&
            header_end[-1] == '\n' &&
            (header_end == outBuffer->data || header_end[-2] != '\r')) {
            delimiter_len = 2;
        }
        size_t header_len = (size_t)(header_end - outBuffer->data) + delimiter_len;
        free(outBuffer->headers);
        outBuffer->headers = (char*)malloc(header_len + 1);
        if (outBuffer->headers) {
            memcpy(outBuffer->headers, outBuffer->data, header_len);
            outBuffer->headers[header_len] = '\0';
            outBuffer->headers_size = header_len;
        }
    }

    if (bodyStream) {
        /* Keep headers in outBuffer so callers can inspect them (e.g., for Content-Range) */
        if (outBuffer->data && outBuffer->size > 0) {
            outBuffer->data[outBuffer->size] = '\0';
        }
        if (outTotalSize) *outTotalSize = total_size_val;
        return true;
    }
    
    if (header_end >= outBuffer->data + 1 && 
        header_end[-1] == '\n' && 
        (header_end == outBuffer->data || header_end[-2] != '\r')) {
        delimiter_len = 2;
    }
    size_t header_len = (size_t)(header_end - outBuffer->data) + delimiter_len;
    size_t body_len = outBuffer->size - header_len;
    
    char *te_header = strstr(outBuffer->data, "Transfer-Encoding:");
    bool is_chunked = false;
    if (te_header && te_header < header_end + delimiter_len) {
        te_header += 18;
        while (*te_header == ' ') te_header++;
        if (strncasecmp(te_header, "chunked", 7) == 0) {
            is_chunked = true;
            LOGI("Detected chunked transfer encoding");
        }
    }
    
    if (is_chunked) {
        char *src = outBuffer->data + header_len;
        char *dst = outBuffer->data;
        size_t decoded_len = 0;
        
        while (true) {
            char *chunk_start = src;
            char *line_end = strstr(chunk_start, "\r\n");
            if (!line_end) line_end = strchr(chunk_start, '\n');
            if (!line_end) break;
            
            size_t chunk_size = 0;
            for (char *p = chunk_start; p < line_end && *p != ';' && *p != '\r' && *p != '\n'; p++) {
                if (*p >= '0' && *p <= '9') {
                    chunk_size = chunk_size * 16 + (*p - '0');
                } else if (*p >= 'a' && *p <= 'f') {
                    chunk_size = chunk_size * 16 + (*p - 'a' + 10);
                } else if (*p >= 'A' && *p <= 'F') {
                    chunk_size = chunk_size * 16 + (*p - 'A' + 10);
                } else {
                    break;
                }
            }
            
            src = line_end;
            if (*src == '\r') src++;
            if (*src == '\n') src++;
            
            if (chunk_size == 0) {
                break;
            }
            
            if (decoded_len + chunk_size > outBuffer->size - header_len) {
                LOGE("Chunk size %zu would overflow buffer", chunk_size);
                break;
            }
            memmove(dst + decoded_len, src, chunk_size);
            decoded_len += chunk_size;
            src += chunk_size;
            
            if (*src == '\r') src++;
            if (*src == '\n') src++;
        }
        
        dst[decoded_len] = '\0';
        outBuffer->size = decoded_len;
        LOGI("Decoded chunked response: %zu bytes -> %zu bytes", body_len, decoded_len);
    } else {
        memmove(outBuffer->data, outBuffer->data + header_len, body_len);
        outBuffer->data[body_len] = '\0';
        outBuffer->size = body_len;
    }
    
    if (outTotalSize) *outTotalSize = total_size_val;
    return true;
}

static bool http_request_with_method(const char *url, const char *method,
                                     const char *postData, size_t postDataLen,
                                     const char **custom_headers, size_t custom_header_count,
                                     HttpBuffer *outBuffer, int *outStatus,
                                     char *err, size_t errLen, const char *cookies,
                                     DownloadState *state,
                                     FILE *bodyStream) {
    return http_request_with_method_internal(url, method, postData, postDataLen,
                                             custom_headers, custom_header_count,
                                             outBuffer, outStatus, err, errLen, cookies,
                                             state, bodyStream, NULL);
}

static bool http_request_with_cookies(const char *url, HttpBuffer *outBuffer,
                                      char *err, size_t errLen, const char *cookies,
                                      DownloadState *state,
                                      FILE *bodyStream) {
    return http_request_with_method(url, "GET", NULL, 0, NULL, 0, outBuffer, NULL, err, errLen, cookies, state, bodyStream);
}

static bool http_request(const char *url, HttpBuffer *outBuffer,
                         char *err, size_t errLen,
                         DownloadState *state,
                         FILE *bodyStream) {
    return http_request_with_cookies(url, outBuffer, err, errLen, g_cookie_jar, state, bodyStream);
}

bool http_get_to_memory(const char *url, HttpBuffer *outBuffer,
                        char *err, size_t errLen) {
    return http_request(url, outBuffer, err, errLen, NULL, NULL);
}

bool http_get_to_memory_with_headers(const char *url, const char **headers, size_t headerCount,
                                     HttpBuffer *outBuffer, char *err, size_t errLen) {
    return http_request_with_method(url, "GET", NULL, 0, headers, headerCount,
                                    outBuffer, NULL, err, errLen, g_cookie_jar, NULL, NULL);
}

bool http_post_to_memory(const char *url, const char *postData, size_t postDataLen,
                         const char **headers, size_t headerCount,
                         HttpBuffer *outBuffer, int *outStatus,
                         char *err, size_t errLen) {
    return http_request_with_method(url, "POST", postData, postDataLen, headers, headerCount,
                                    outBuffer, outStatus, err, errLen, NULL, NULL, NULL);
}

bool http_request_to_memory(const char *url, const char *method,
                            const char *postData, size_t postDataLen,
                            const char **headers, size_t headerCount,
                            HttpBuffer *outBuffer, int *outStatus,
                            char *err, size_t errLen) {
    /* Generic request with no automatic cookies; caller controls credentials. */
    return http_request_with_method(url, method, postData, postDataLen, headers, headerCount,
                                    outBuffer, outStatus, err, errLen, NULL, NULL, NULL);
}

void http_free_buffer(HttpBuffer *buffer) {
    if (buffer) {
        if (buffer->data) {
            free(buffer->data);
            buffer->data = NULL;
            buffer->size = 0;
        }
        if (buffer->headers) {
            free(buffer->headers);
            buffer->headers = NULL;
            buffer->headers_size = 0;
        }
    }
}

static bool http_get_to_memory_resumable(const char *url, HttpBuffer *outBuffer,
                                         char *err, size_t errLen) {
    size_t total_expected = 0;
    size_t downloaded = 0;
    int chunks = 0;

    outBuffer->data = NULL;
    outBuffer->size = 0;

    LOGE("[RESUME] Starting resumable download from %s", url);

    while (chunks < MAX_RESUME_CHUNKS) {
        char range_header[64];
        snprintf(range_header, sizeof(range_header), "Range: bytes=%zu-", downloaded);
        const char *headers[] = { range_header };

        HttpBuffer chunk_buffer = {0};
        int status = 0;
        size_t chunk_total_size = 0;

        LOGE("[RESUME] Chunk %d: Range bytes=%zu-", chunks, downloaded);
        bool req_success = http_request_with_method_internal(url, "GET", NULL, 0,
                                                              headers, 1, &chunk_buffer, &status,
                                                              err, errLen, g_cookie_jar,
                                                              NULL, NULL, &chunk_total_size);
        LOGE("[RESUME] Chunk %d: req_success=%d status=%d chunk_size=%zu total=%zu", chunks, req_success, status, chunk_buffer.size, chunk_total_size);

        if (!req_success) {
            LOGE("[RESUME] Chunk %d: request failed: %s", chunks, err);
            if (downloaded > 0 && chunks > 0) {
                LOGI("Request failed after %zu bytes, assuming EOF", downloaded);
                break;
            }
            http_free_buffer(outBuffer);
            return false;
        }

        if (status == 416) {
            LOGI("Range Not Satisfiable after %zu bytes, download complete", downloaded);
            break;
        }

        if (status != 200 && status != 206) {
            snprintf(err, errLen, "HTTP error %d", status);
            LOGE("[RESUME] Chunk %d: HTTP error %d", chunks, status);
            http_free_buffer(outBuffer);
            return false;
        }

        if (total_expected == 0) {
            total_expected = chunk_total_size;
            if (total_expected == 0 && chunk_buffer.headers) {
                total_expected = parse_content_length_from_headers(chunk_buffer.headers, chunk_buffer.headers_size);
            }
            LOGI("Expected total size: %zu bytes", total_expected);
        }

        /* chunk_buffer.data contains only body data (headers are in chunk_buffer.headers) */
        if (chunk_buffer.size == 0) {
            if (downloaded > 0) {
                LOGI("Empty chunk after %zu bytes, assuming complete", downloaded);
            } else {
                snprintf(err, errLen, "Empty response");
                http_free_buffer(outBuffer);
                return false;
            }
            break;
        }

        /* Append chunk to outBuffer */
        char *new_data = realloc(outBuffer->data, outBuffer->size + chunk_buffer.size + 1);
        if (!new_data) {
            snprintf(err, errLen, "Out of memory");
            http_free_buffer(&chunk_buffer);
            http_free_buffer(outBuffer);
            return false;
        }
        outBuffer->data = new_data;
        memcpy(outBuffer->data + outBuffer->size, chunk_buffer.data, chunk_buffer.size);
        outBuffer->size += chunk_buffer.size;
        outBuffer->data[outBuffer->size] = '\0';

        http_free_buffer(&chunk_buffer);

        downloaded = outBuffer->size;
        LOGE("[RESUME] Chunk %d: appended, downloaded=%zu total_expected=%zu", chunks, downloaded, total_expected);

        if (total_expected > 0 && downloaded >= total_expected) {
            LOGI("Download complete: %zu / %zu bytes", downloaded, total_expected);
            break;
        }

        chunks++;
    }
    LOGE("[RESUME] Download loop exited: chunks=%d downloaded=%zu", chunks, downloaded);

    if (outBuffer->size == 0) {
        snprintf(err, errLen, "No data downloaded");
        return false;
    }

    return true;
}

/* ============================================================================
 * Public API: callback-free file download with shared-state progress
 * ============================================================================ */

static bool http_download_parallel_state(const char *url, const char *filePath,
                                          int num_connections,
                                          DownloadState *state,
                                          char *err, size_t errLen);

bool http_download_to_file(const char *url, const char *filePath,
                           DownloadState *state,
                           char *err, size_t errLen) {
    if (!filePath || !filePath[0]) {
        LOGI("Download to file: no file path specified");
        return true;
    }

    FILE *file = fopen_utf8(filePath, "wb");
    if (!file) {
        if (err && errLen > 0) {
            snprintf(err, errLen, "Failed to open file for writing: %s", filePath);
        }
        return false;
    }

    HttpBuffer buffer = {0};
    bool result = http_request(url, &buffer, err, errLen, state, file);
    
    fflush(file);
    fclose(file);
    http_free_buffer(&buffer);
    
    if (!result) {
        remove_utf8(filePath);
        if (state) download_state_set(state, 5, "Download failed");
        return false;
    }
    
    if (state) {
        download_state_set(state, 4, "Download complete");
    }
    LOGI("Downloaded and saved to %s", filePath);
    return true;
}

/* ============================================================================
 * Parallel range download using pthreads (native TLS stack)
 * ============================================================================ */

#define MAX_PARALLEL_CONNECTIONS 32
#define MIN_CHUNK_SIZE (1024 * 1024)

typedef struct {
    size_t start;
    size_t end;
    char temp_path[512];
    bool success;
    char err[256];
} ChunkInfo;

typedef struct {
    DownloadState *state;
    size_t total_expected;
    PlatformMutex *mutex;
} ParallelState;

typedef struct {
    const char *url;
    const char *cookies;
    ChunkInfo *chunk;
    ParallelState *shared;
} ThreadArgs;

static void* parallel_download_thread(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    ChunkInfo *chunk = args->chunk;

    FILE *file = fopen_utf8(chunk->temp_path, "wb");
    if (!file) {
        snprintf(chunk->err, sizeof(chunk->err), "Failed to open temp file");
        chunk->success = false;
        return NULL;
    }

    char range_header[64];
    snprintf(range_header, sizeof(range_header), "Range: bytes=%zu-%zu", chunk->start, chunk->end);
    const char *headers[] = { range_header };

    HttpBuffer buffer = {0};
    int status = 0;
    size_t chunk_total_size = 0;

    bool ok = http_request_with_method_internal(args->url, "GET", NULL, 0,
                                                 headers, 1, &buffer, &status,
                                                 chunk->err, sizeof(chunk->err),
                                                 args->cookies,
                                                 NULL, file, &chunk_total_size);

    long actual_size = ftell(file);
    fclose(file);
    http_free_buffer(&buffer);

    if (ok && (status == 200 || status == 206) && actual_size > 0) {
        chunk->success = true;
        platform_mutex_lock(args->shared->mutex);
        if (args->shared->state) {
            args->shared->state->bytes_downloaded += (size_t)actual_size;
        }
        platform_mutex_unlock(args->shared->mutex);
        LOGI("Chunk %zu-%zu downloaded: %ld bytes", chunk->start, chunk->end, actual_size);
    } else {
        chunk->success = false;
        if (actual_size <= 0 && chunk->err[0] == '\0') {
            snprintf(chunk->err, sizeof(chunk->err), "Chunk download failed (HTTP %d)", status);
        }
        LOGE("Chunk %zu-%zu failed: ok=%d status=%d size=%ld err=%s",
             chunk->start, chunk->end, ok, status, actual_size,
             chunk->err[0] ? chunk->err : "unknown");
    }
    return NULL;
}

static bool http_download_parallel_state(const char *url, const char *filePath,
                                          int num_connections,
                                          DownloadState *state,
                                          char *err, size_t errLen) {
    if (!url || !filePath || num_connections < 1) {
        snprintf(err, errLen, "Invalid arguments");
        return false;
    }
    if (num_connections > MAX_PARALLEL_CONNECTIONS) {
        num_connections = MAX_PARALLEL_CONNECTIONS;
    }

    if (state) download_state_set(state, 1, "Getting file size...");
    size_t total_size = 0;
    {
        HttpBuffer buffer = {0};
        int status = 0;
        const char *headers[] = { "Range: bytes=0-0" };
        if (!http_request_with_method_internal(url, "GET", NULL, 0,
                                                headers, 1, &buffer, &status,
                                                err, errLen, g_cookie_jar,
                                                NULL, NULL, &total_size)) {
            http_free_buffer(&buffer);
            snprintf(err, errLen, "Failed to get file size");
            if (state) download_state_set(state, 5, "Failed to get file size");
            return false;
        }
        http_free_buffer(&buffer);
        if (status != 200 && status != 206) {
            snprintf(err, errLen, "Server returned HTTP %d", status);
            if (state) download_state_set(state, 5, "Range requests not supported");
            return false;
        }
        if (total_size == 0) {
            snprintf(err, errLen, "Could not determine file size");
            if (state) download_state_set(state, 5, "Could not determine file size");
            return false;
        }
    }

    if (state) {
        download_state_set(state, 2, "Downloading...");
        download_state_set_progress(state, 0, total_size);
    }

    ChunkInfo chunks[MAX_PARALLEL_CONNECTIONS];
    int num_chunks = num_connections;
    size_t chunk_size = total_size / num_chunks;
    if (chunk_size < MIN_CHUNK_SIZE) {
        chunk_size = MIN_CHUNK_SIZE;
        num_chunks = (int)((total_size + chunk_size - 1) / chunk_size);
        if (num_chunks > MAX_PARALLEL_CONNECTIONS) {
            num_chunks = MAX_PARALLEL_CONNECTIONS;
            chunk_size = total_size / num_chunks;
        }
    }

    for (int i = 0; i < num_chunks; i++) {
        chunks[i].start = (size_t)i * chunk_size;
        chunks[i].end = (i == num_chunks - 1) ? total_size - 1 : chunks[i].start + chunk_size - 1;
        snprintf(chunks[i].temp_path, sizeof(chunks[i].temp_path), "%s.part_%03d", filePath, i);
        chunks[i].success = false;
        chunks[i].err[0] = '\0';
    }

    ParallelState shared = {
        .state = state,
        .total_expected = total_size,
        .mutex = platform_mutex_create()
    };
    if (!shared.mutex) {
        snprintf(err, errLen, "Failed to create mutex");
        return false;
    }

    pthread_t threads[MAX_PARALLEL_CONNECTIONS];
    ThreadArgs thread_args[MAX_PARALLEL_CONNECTIONS];
    for (int i = 0; i < num_chunks; i++) {
        thread_args[i].url = url;
        thread_args[i].cookies = g_cookie_jar;
        thread_args[i].chunk = &chunks[i];
        thread_args[i].shared = &shared;
        if (pthread_create(&threads[i], NULL, parallel_download_thread, &thread_args[i]) != 0) {
            for (int j = 0; j < i; j++) pthread_join(threads[j], NULL);
            platform_mutex_destroy(shared.mutex);
            for (int j = 0; j < i; j++) remove_utf8(chunks[j].temp_path);
            snprintf(err, errLen, "Failed to create thread %d", i);
            return false;
        }
        if (i < num_chunks - 1) platform_sleep_ms(num_chunks > 16 ? 50 : 150);
    }

    for (int i = 0; i < num_chunks; i++) {
        pthread_join(threads[i], NULL);
    }

    bool all_success = true;
    for (int i = 0; i < num_chunks; i++) {
        if (!chunks[i].success) all_success = false;
    }

    if (!all_success) {
        for (int retry = 0; retry < 2; retry++) {
            bool any_failed = false;
            for (int i = 0; i < num_chunks; i++) {
                if (!chunks[i].success) {
                    LOGI("Retrying chunk %d (attempt %d)", i, retry + 1);
                    ThreadArgs args = { .url = url, .cookies = g_cookie_jar,
                                        .chunk = &chunks[i], .shared = &shared };
                    parallel_download_thread(&args);
                    if (!chunks[i].success) any_failed = true;
                }
            }
            if (!any_failed) { all_success = true; break; }
        }
    }

    platform_mutex_destroy(shared.mutex);

    if (!all_success) {
        for (int i = 0; i < num_chunks; i++) remove_utf8(chunks[i].temp_path);
        snprintf(err, errLen, "Some chunks failed to download");
        if (state) download_state_set(state, 5, "Some chunks failed to download");
        return false;
    }

    if (state) download_state_set(state, 3, "Finishing...");
    FILE *out = fopen_utf8(filePath, "wb");
    if (!out) {
        for (int i = 0; i < num_chunks; i++) remove_utf8(chunks[i].temp_path);
        snprintf(err, errLen, "Failed to open output file");
        if (state) download_state_set(state, 5, "Failed to open output file");
        return false;
    }

    for (int i = 0; i < num_chunks; i++) {
        FILE *in = fopen_utf8(chunks[i].temp_path, "rb");
        if (!in) {
            fclose(out); remove_utf8(filePath);
            for (int j = 0; j < num_chunks; j++) remove_utf8(chunks[j].temp_path);
            snprintf(err, errLen, "Failed to open temp file %d", i);
            if (state) download_state_set(state, 5, "Failed to open temp file");
            return false;
        }
        char buf[CHUNK_SIZE];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) {
                fclose(in); fclose(out); remove_utf8(filePath);
                for (int j = 0; j < num_chunks; j++) remove_utf8(chunks[j].temp_path);
                snprintf(err, errLen, "Failed to write to output file");
                if (state) download_state_set(state, 5, "Failed to write to output file");
                return false;
            }
        }
        fclose(in);
        remove_utf8(chunks[i].temp_path);
    }
    fclose(out);

    if (state) {
        download_state_set_progress(state, total_size, total_size);
        download_state_set(state, 4, "Download complete");
    }
    LOGI("Parallel download complete: %s (%zu bytes)", filePath, total_size);
    return true;
}

/* Legacy WebView functions - now no-ops */
void http_download_via_webview(const char *url, void *app) {
    (void)url;
    (void)app;
    LOGI("WebView mode disabled - using native HTTP");
}

#ifdef BE_PLATFORM_ANDROID
void http_download_set_jni_refs(JavaVM *vm, jobject activity) {
    (void)vm;
    (void)activity;
}
#endif

void http_download_load_page(const char *url) {
    (void)url;
}

void http_download_set_cookies(const char *cookies) {
    (void)cookies;
}

void http_download_set_js_session_data(const char *session) {
    (void)session;
}
