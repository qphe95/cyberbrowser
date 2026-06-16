/*
 * HTTP Client Implementation using libcurl
 */

#include "platform.h"

#include <curl/curl.h>
#include <string.h>
#include <stdlib.h>

static CURL *g_curl = NULL;
static char g_cookies[4096] = {0};
static PlatformMutex *g_cookie_mutex = NULL;

/* Write callback for libcurl */
static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    PlatformHttpBuffer *buffer = (PlatformHttpBuffer *)userp;
    
    char *new_data = realloc(buffer->data, buffer->size + total_size + 1);
    if (!new_data) {
        return 0;
    }
    
    buffer->data = new_data;
    memcpy(buffer->data + buffer->size, contents, total_size);
    buffer->size += total_size;
    buffer->data[buffer->size] = '\0';
    
    return total_size;
}

/* Header callback to capture Set-Cookie headers */
static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata) {
    (void)userdata;
    size_t total_size = size * nitems;
    
    /* Check for Set-Cookie header */
    if (strncasecmp(buffer, "Set-Cookie:", 11) == 0) {
        char *cookie_start = buffer + 11;
        while (*cookie_start == ' ') cookie_start++;
        
        /* Find end of cookie value (before ; or end of line) */
        char *line_end = strchr(cookie_start, '\r');
        if (!line_end) line_end = strchr(cookie_start, '\n');
        
        if (line_end) {
            /* Look for semicolon to get just NAME=VALUE */
            char *semicolon = strchr(cookie_start, ';');
            if (semicolon && semicolon < line_end) {
                line_end = semicolon;
            }
            
            size_t cookie_len = line_end - cookie_start;
            if (cookie_len > 0 && cookie_len < 2000) {
                platform_mutex_lock(g_cookie_mutex);
                
                /* Parse cookie name=value */
                char cookie_buf[2048];
                memcpy(cookie_buf, cookie_start, cookie_len);
                cookie_buf[cookie_len] = '\0';
                
                /* Find = sign */
                char *eq = strchr(cookie_buf, '=');
                if (eq && eq > cookie_buf) {
                    *eq = '\0';
                    char *name = cookie_buf;
                    char *value = eq + 1;
                    
                    /* Check if we already have this cookie */
                    bool duplicate = false;
                    if (g_cookies[0]) {
                        char *search = g_cookies;
                        while ((search = strstr(search, name)) != NULL) {
                            if ((search == g_cookies || (search[-1] == ' ' && search[-2] == ';')) &&
                                search[strlen(name)] == '=') {
                                duplicate = true;
                                break;
                            }
                            search++;
                        }
                    }
                    
                    if (!duplicate) {
                        size_t current_len = strlen(g_cookies);
                        size_t remaining = sizeof(g_cookies) - current_len - 1;
                        size_t needed = strlen(name) + strlen(value) + 3; /* ; = \0 */
                        
                        if (g_cookies[0]) needed += 2; /* ; and space */
                        
                        if (remaining >= needed) {
                            if (g_cookies[0]) {
                                strncat(g_cookies, "; ", remaining);
                                remaining -= 2;
                            }
                            strncat(g_cookies, name, remaining);
                            remaining -= strlen(name);
                            strncat(g_cookies, "=", remaining);
                            remaining -= 1;
                            strncat(g_cookies, value, remaining);
                        }
                    }
                }
                
                platform_mutex_unlock(g_cookie_mutex);
            }
        }
    }
    
    return total_size;
}

bool platform_http_init(void) {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        return false;
    }
    
    g_cookie_mutex = platform_mutex_create();
    if (!g_cookie_mutex) {
        curl_global_cleanup();
        return false;
    }
    
    return true;
}

void platform_http_cleanup(void) {
    if (g_cookie_mutex) {
        platform_mutex_destroy(g_cookie_mutex);
        g_cookie_mutex = NULL;
    }
    curl_global_cleanup();
}

bool platform_http_get_with_headers(const char *url,
                                    const char **headers, size_t headerCount,
                                    PlatformHttpBuffer *outBuffer,
                                    char *error, size_t errorLen) {
    if (!outBuffer) {
        if (error && errorLen > 0) {
            strncpy(error, "Invalid output buffer", errorLen);
        }
        return false;
    }
    
    memset(outBuffer, 0, sizeof(PlatformHttpBuffer));
    
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (error && errorLen > 0) {
            strncpy(error, "Failed to initialize CURL", errorLen);
        }
        return false;
    }
    
    /* Set URL */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    
    /* Set write callback */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, outBuffer);
    
    /* Set header callback to capture cookies */
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
    
    /* Set timeouts */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    
    /* Follow redirects */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    
    /* Set user agent */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, 
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    
    /* Add custom headers */
    struct curl_slist *header_list = NULL;
    if (headers && headerCount > 0) {
        for (size_t i = 0; i < headerCount; i++) {
            header_list = curl_slist_append(header_list, headers[i]);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }
    
    /* Add cookies */
    platform_mutex_lock(g_cookie_mutex);
    if (g_cookies[0]) {
        curl_easy_setopt(curl, CURLOPT_COOKIE, g_cookies);
    }
    platform_mutex_unlock(g_cookie_mutex);
    
    /* Perform request */
    CURLcode res = curl_easy_perform(curl);
    
    /* Cleanup headers */
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    
    if (res != CURLE_OK) {
        if (error && errorLen > 0) {
            strncpy(error, curl_easy_strerror(res), errorLen);
        }
        curl_easy_cleanup(curl);
        return false;
    }
    
    /* Check HTTP response code */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_easy_cleanup(curl);
    
    if (http_code != 200) {
        if (error && errorLen > 0) {
            snprintf(error, errorLen, "HTTP error %ld", http_code);
        }
        if (outBuffer->data) {
            free(outBuffer->data);
            outBuffer->data = NULL;
        }
        return false;
    }
    
    return true;
}

bool platform_http_get(const char *url, 
                       PlatformHttpBuffer *outBuffer,
                       char *error, size_t errorLen) {
    return platform_http_get_with_headers(url, NULL, 0, outBuffer, error, errorLen);
}

bool platform_http_post(const char *url,
                        const char *postData, size_t postDataLen,
                        const char **headers, size_t headerCount,
                        PlatformHttpBuffer *outBuffer,
                        int *outStatus,
                        char *error, size_t errorLen) {
    if (!outBuffer) {
        if (error && errorLen > 0) {
            strncpy(error, "Invalid output buffer", errorLen);
        }
        return false;
    }
    
    memset(outBuffer, 0, sizeof(PlatformHttpBuffer));
    
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (error && errorLen > 0) {
            strncpy(error, "Failed to initialize CURL", errorLen);
        }
        return false;
    }
    
    /* Set URL */
    curl_easy_setopt(curl, CURLOPT_URL, url);
    
    /* Set POST data */
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)postDataLen);
    
    /* Set write callback */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, outBuffer);
    
    /* Set header callback to capture cookies */
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
    
    /* Set timeouts */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    
    /* Follow redirects */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    
    /* Set user agent */
    curl_easy_setopt(curl, CURLOPT_USERAGENT, 
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
    
    /* Add custom headers */
    struct curl_slist *header_list = NULL;
    if (headers && headerCount > 0) {
        for (size_t i = 0; i < headerCount; i++) {
            header_list = curl_slist_append(header_list, headers[i]);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
    }
    
    /* Add cookies */
    platform_mutex_lock(g_cookie_mutex);
    if (g_cookies[0]) {
        curl_easy_setopt(curl, CURLOPT_COOKIE, g_cookies);
    }
    platform_mutex_unlock(g_cookie_mutex);
    
    /* Perform request */
    CURLcode res = curl_easy_perform(curl);
    
    /* Cleanup headers */
    if (header_list) {
        curl_slist_free_all(header_list);
    }
    
    if (res != CURLE_OK) {
        if (error && errorLen > 0) {
            strncpy(error, curl_easy_strerror(res), errorLen);
        }
        curl_easy_cleanup(curl);
        return false;
    }
    
    /* Check HTTP response code */
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    
    curl_easy_cleanup(curl);
    
    if (outStatus) {
        *outStatus = (int)http_code;
    }
    
    /* Return true if we got any HTTP response (even error codes),
     * false only if there was no response at all */
    if (http_code == 0) {
        if (error && errorLen > 0) {
            snprintf(error, errorLen, "No HTTP response received");
        }
        if (outBuffer->data) {
            free(outBuffer->data);
            outBuffer->data = NULL;
        }
        return false;
    }
    
    return true;
}

void platform_http_free_buffer(PlatformHttpBuffer *buffer) {
    if (buffer && buffer->data) {
        free(buffer->data);
        buffer->data = NULL;
        buffer->size = 0;
    }
}

void platform_http_set_cookies(const char *cookies) {
    if (!cookies) return;
    
    platform_mutex_lock(g_cookie_mutex);
    strncpy(g_cookies, cookies, sizeof(g_cookies) - 1);
    g_cookies[sizeof(g_cookies) - 1] = '\0';
    platform_mutex_unlock(g_cookie_mutex);
}

const char* platform_http_get_cookies(void) {
    return g_cookies[0] ? g_cookies : NULL;
}

void platform_http_clear_cookies(void) {
    platform_mutex_lock(g_cookie_mutex);
    g_cookies[0] = '\0';
    platform_mutex_unlock(g_cookie_mutex);
}
