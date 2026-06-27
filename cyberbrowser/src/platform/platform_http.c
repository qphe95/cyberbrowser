/*
 * Platform Abstraction Layer - HTTP Client
 *
 * Shared HTTP implementation for all platforms. Delegates to the native
 * http_download.c / tls_client.c stack instead of relying on libcurl.
 */

#include "platform.h"
#include "http_download.h"

#include <string.h>
#include <stdlib.h>

bool platform_http_init(void) {
    return true; /* HTTP is initialized via http_download.c */
}

void platform_http_cleanup(void) {
    /* Nothing to cleanup */
}

static void copy_http_buffer_fields(PlatformHttpBuffer *outBuffer, const HttpBuffer *http_buffer) {
    if (outBuffer && http_buffer) {
        outBuffer->data = http_buffer->data;
        outBuffer->size = http_buffer->size;
        outBuffer->headers = http_buffer->headers;
        outBuffer->headers_size = http_buffer->headers_size;
    }
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

    HttpBuffer http_buffer = {0};
    bool result = http_get_to_memory_with_headers(url, headers, headerCount,
                                                  &http_buffer, error, errorLen);

    if (result) {
        copy_http_buffer_fields(outBuffer, &http_buffer);
    }

    return result;
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

    HttpBuffer http_buffer = {0};
    bool result = http_post_to_memory(url, postData, postDataLen,
                                      headers, headerCount,
                                      &http_buffer, outStatus,
                                      error, errorLen);

    if (result) {
        copy_http_buffer_fields(outBuffer, &http_buffer);
    }

    return result;
}

bool platform_http_request(const char *url,
                           const char *method,
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

    HttpBuffer http_buffer = {0};
    bool result = http_request_to_memory(url, method, postData, postDataLen,
                                         headers, headerCount,
                                         &http_buffer, outStatus,
                                         error, errorLen);

    if (result) {
        copy_http_buffer_fields(outBuffer, &http_buffer);
    }

    return result;
}

void platform_http_free_buffer(PlatformHttpBuffer *buffer) {
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

void platform_http_set_cookies(const char *cookies) {
    http_set_cookies(cookies);
}

const char* platform_http_get_cookies(void) {
    return http_get_cookies();
}

void platform_http_clear_cookies(void) {
    http_clear_cookies();
}
