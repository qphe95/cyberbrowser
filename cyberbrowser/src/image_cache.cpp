#include "image_cache.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include "url_utils.h"

#ifdef _WIN32
#include <windows.h>
#define atomic_set(ptr, val) InterlockedExchange((ptr), (val))
#else
#include <pthread.h>
#define atomic_set(ptr, val) __sync_lock_test_and_set((ptr), (val))
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

#include "http_download.h"
#include "platform.h"
#include "cyber_profile.h"

#define LOG_TAG "image_cache"
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)
#define LOG_INFO(...) platform_log(LOG_LEVEL_INFO, LOG_TAG, __VA_ARGS__)

enum {
    ENTRY_STATE_IDLE = 0,
    ENTRY_STATE_LOADING = 1,
    ENTRY_STATE_DONE = 2,
    ENTRY_STATE_ERROR = 3
};

typedef struct ImageCacheEntry {
    char      *source;
    int        width;
    int        height;
    int        channels;
    uint8_t   *pixels;

    /* Async state. Synchronous entries stay in ENTRY_STATE_IDLE. */
    volatile long state;
    ImageLoadCallback callback;
    void              *user_data;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
    char              *download_data;
    size_t             download_size;
} ImageCacheEntry;

struct ImageCache {
    ImageCacheEntry **entries;  /* Array of individually allocated pointers; stable. */
    int              count;
    int              capacity;
};

static bool is_network_url(const char *s)
{
    return url_is_network_url(s);
}

/* Decode a base64-encoded string into newly allocated binary data.
 * Returns allocated buffer on success, or NULL on failure.  out_len is set to
 * the decoded size. */
static uint8_t *base64_decode(const char *in, size_t in_len, size_t *out_len)
{
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!in || in_len == 0) return NULL;

    size_t max_len = (in_len / 4) * 3 + 2;
    uint8_t *out = (uint8_t *)malloc(max_len);
    if (!out) return NULL;

    int val = 0, valb = -8;
    size_t j = 0;
    for (size_t i = 0; i < in_len && in[i] != '='; i++) {
        const char *p = strchr(b64_table, in[i]);
        if (p) {
            val = (val << 6) + (int)(p - b64_table);
            valb += 6;
            if (valb >= 0) {
                out[j++] = (uint8_t)((val >> valb) & 0xFF);
                valb -= 8;
            }
        }
    }
    *out_len = j;
    return out;
}

/* Supported image MIME types for data: URLs. */
static bool is_supported_image_mime(const char *header, size_t header_len)
{
    if (header_len >= 9 && strncasecmp(header, "image/png", 9) == 0) return true;
    if (header_len >= 10 && strncasecmp(header, "image/jpeg", 10) == 0) return true;
    if (header_len >= 9 && strncasecmp(header, "image/gif", 9) == 0) return true;
    if (header_len >= 9 && strncasecmp(header, "image/bmp", 9) == 0) return true;
    if (header_len >= 10 && strncasecmp(header, "image/webp", 10) == 0) return true;
    if (header_len >= 13 && strncasecmp(header, "image/svg+xml", 13) == 0) return true;
    return false;
}

/* Detect an SVG document by content: '<' start (after BOM/whitespace) with
 * "<svg" appearing early, since raster formats start with magic bytes. */
static bool is_svg_document(const char *data, size_t size)
{
    if (!data || size < 5) return false;
    size_t i = 0;
    if (size >= 3 && (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) i = 3;
    while (i < size && isspace((unsigned char)data[i])) i++;
    if (i >= size || data[i] != '<') return false;
    size_t scan = size - i < 512 ? size - i : 512;
    for (size_t j = i; j + 4 <= i + scan; j++) {
        if (memcmp(data + j, "<svg", 4) == 0) return true;
    }
    return false;
}

/* Rasterize an SVG document to RGBA pixels with nanosvg.  Returns malloc'd
 * pixels (compatible with stbi_image_free) and sets *out_w/*out_h. */
static uint8_t *svg_rasterize(const char *data, size_t size, int *out_w, int *out_h)
{
    char *copy = (char *)malloc(size + 1);
    if (!copy) return NULL;
    memcpy(copy, data, size);
    copy[size] = '\0';

    NSVGimage *img = nsvgParse(copy, "px", 96.0f);
    free(copy);
    if (!img) return NULL;

    /* nsvgParse always resolves width/height (viewBox/content fallback). */
    float w = img->width, h = img->height;
    if (w <= 0.0f || h <= 0.0f) { w = 256.0f; h = 256.0f; }

    /* Cap the raster size so huge documents do not allocate giant buffers. */
    float scale = 1.0f;
    const float max_dim = 2048.0f;
    if (w > max_dim || h > max_dim) {
        scale = max_dim / (w > h ? w : h);
        w *= scale;
        h *= scale;
    }

    int iw = (int)(w + 0.5f), ih = (int)(h + 0.5f);
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (iw <= 0 || ih <= 0 || !rast) {
        if (rast) nsvgDeleteRasterizer(rast);
        nsvgDelete(img);
        return NULL;
    }
    uint8_t *pixels = (uint8_t *)calloc((size_t)iw * (size_t)ih, 4);
    if (pixels) {
        nsvgRasterize(rast, img, 0.0f, 0.0f, scale, pixels, iw, ih, iw * 4);
    }
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);
    if (!pixels) return NULL;
    *out_w = iw;
    *out_h = ih;
    return pixels;
}

/* Parse a data: URL and return the decoded payload if it is a supported image
 * type.  Only base64-encoded raster image data URLs are handled.  Returns an
 * allocated buffer on success, or NULL on failure. */
static uint8_t *decode_data_url(const char *url, size_t *out_len)
{
    if (!url_is_data_url(url)) return NULL;
    const char *comma = strchr(url, ',');
    if (!comma) return NULL;

    const char *header = url + 5; /* after "data:" */
    size_t header_len = (size_t)(comma - header);

    bool base64 = false;
    if (header_len >= 7 && strncasecmp(header + header_len - 7, ";base64", 7) == 0) {
        base64 = true;
        header_len -= 7;
    }
    if (!base64) return NULL;

    if (!is_supported_image_mime(header, header_len)) return NULL;

    return base64_decode(comma + 1, strlen(comma + 1), out_len);
}

ImageCache *image_cache_create(void)
{
    return (ImageCache *)calloc(1, sizeof(ImageCache));
}

void image_cache_destroy(ImageCache *cache)
{
    if (!cache) return;
    for (int i = 0; i < cache->count; i++) {
        ImageCacheEntry *e = cache->entries[i];
        if (!e) continue;
        free(e->source);
        stbi_image_free(e->pixels);
#ifdef _WIN32
        if (e->thread) WaitForSingleObject(e->thread, INFINITE);
#else
        if (e->thread) pthread_join(e->thread, NULL);
#endif
        free(e->download_data);
        free(e);
    }
    free(cache->entries);
    free(cache);
}

static ImageCacheEntry *add_entry(ImageCache *cache)
{
    if (cache->count >= cache->capacity) {
        int new_cap = cache->capacity ? cache->capacity * 2 : 8;
        ImageCacheEntry **ne = (ImageCacheEntry **)realloc(cache->entries,
                                                           new_cap * sizeof(ImageCacheEntry *));
        if (!ne) return NULL;
        cache->entries = ne;
        cache->capacity = new_cap;
    }
    ImageCacheEntry *e = (ImageCacheEntry *)calloc(1, sizeof(ImageCacheEntry));
    if (!e) return NULL;
    e->state = ENTRY_STATE_IDLE;
    int idx = cache->count++;
    cache->entries[idx] = e;
    return e;
}

static ImageCacheEntry *find_entry(ImageCache *cache, const char *url_or_path)
{
    for (int i = 0; i < cache->count; i++) {
        ImageCacheEntry *e = cache->entries[i];
        if (e && e->source && strcmp(e->source, url_or_path) == 0) {
            return e;
        }
    }
    return NULL;
}

static int entry_index(ImageCache *cache, ImageCacheEntry *e)
{
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i] == e) return i;
    }
    return -1;
}

static bool decode_entry(ImageCacheEntry *e)
{
    int width = 0, height = 0, channels = 0;
    uint8_t *pixels = NULL;

    if (e->download_data && e->download_size > 0) {
        if (is_svg_document(e->download_data, e->download_size)) {
            pixels = svg_rasterize(e->download_data, e->download_size, &width, &height);
            channels = 4;
        } else {
            pixels = stbi_load_from_memory((const stbi_uc *)e->download_data,
                                           (int)e->download_size,
                                           &width, &height, &channels, 4);
        }
    } else {
        /* Local file path. */
        pixels = stbi_load(e->source, &width, &height, &channels, 4);
    }

    if (!pixels && e->source && is_network_url(e->source)) {
        /* Retry once with the query string stripped: CDNs like i.ytimg.com
         * select the image format from query parameters (sqp=...), serving
         * AVIF/WebP variants that stb cannot decode.  The plain URL serves a
         * baseline JPEG instead. */
        const char *q = strchr(e->source, '?');
        if (q && q != e->source) {
            size_t base_len = (size_t)(q - e->source);
            char *plain = (char *)malloc(base_len + 1);
            if (plain) {
                memcpy(plain, e->source, base_len);
                plain[base_len] = '\0';
                HttpBuffer buffer = {0};
                char err[256] = {0};
                if (http_get_to_memory(plain, &buffer, err, sizeof(err)) &&
                    buffer.data && buffer.size > 0) {
                    width = height = channels = 0;
                    pixels = stbi_load_from_memory((const stbi_uc *)buffer.data,
                                                   (int)buffer.size,
                                                   &width, &height, &channels, 4);
                    if (pixels) {
                        LOG_INFO("Decoded image after stripping query: %.80s", plain);
                    }
                }
                if (buffer.data) free(buffer.data);
                free(plain);
            }
        }
    }

    if (!pixels) {
        LOG_ERROR("Failed to decode image %s", e->source ? e->source : "(null)");
        return false;
    }

    e->width = width;
    e->height = height;
    e->channels = 4;
    e->pixels = pixels;
    return true;
}

int image_cache_load(ImageCache *cache, const char *url_or_path)
{
    if (!cache || !url_or_path || !url_or_path[0]) return -1;

    ImageCacheEntry *e = find_entry(cache, url_or_path);
    if (e) return entry_index(cache, e);

    e = add_entry(cache);
    if (!e) return -1;
    e->source = strdup(url_or_path);

    if (url_is_data_url(url_or_path)) {
        size_t decoded_len = 0;
        uint8_t *decoded = decode_data_url(url_or_path, &decoded_len);
        if (!decoded) {
            /* Unsupported or malformed data URL (e.g. SVG).  Not an error:
             * just skip it rather than making a bogus network request. */
            LOG_INFO("Skipping unsupported/malformed data URL %.80s", url_or_path);
            cache->count--;
            free(e);
            return -1;
        }
        e->download_data = (char *)decoded;
        e->download_size = decoded_len;
    } else if (is_network_url(url_or_path)) {
        HttpBuffer buffer = {0};
        char err[256] = {0};
        bool got = false;
        /* CDNs (notably upload.wikimedia.org) answer bursts with HTTP 429.
         * Retry with backoff so screenshots don't lose images to rate limits. */
        for (int attempt = 0; attempt < 4 && !got; attempt++) {
            if (attempt > 0) {
                platform_sleep_ms((unsigned int)(400 * attempt * attempt));
                if (buffer.data) { free(buffer.data); buffer.data = NULL; }
                buffer.size = 0;
                err[0] = '\0';
            }
            CP_SCOPE_CAT("image-download", "img");
            got = http_get_to_memory(url_or_path, &buffer, err, sizeof(err));
            if (!got && strstr(err, "429") == NULL) break; /* not retryable */
        }
        if (!got) {
            LOG_ERROR("Failed to download image %s: %s", url_or_path, err);
            cache->count--;
            free(e);
            return -1;
        }
        e->download_data = buffer.data;
        e->download_size = buffer.size;
    }

    int decoded = 0;
    {
        CP_SCOPE_CAT("image-decode", "img");
        decoded = decode_entry(e);
    }
    if (!decoded) {
        free(e->download_data);
        e->download_data = NULL;
        e->download_size = 0;
        cache->count--;
        free(e);
        return -1;
    }
    free(e->download_data);
    e->download_data = NULL;
    e->download_size = 0;
    return entry_index(cache, e);
}

#ifdef _WIN32
static DWORD WINAPI async_download_thread(LPVOID param)
#else
static void *async_download_thread(void *param)
#endif
{
    ImageCacheEntry *e = (ImageCacheEntry *)param;
    HttpBuffer buffer = {0};
    char err[256] = {0};
    if (http_get_to_memory(e->source, &buffer, err, sizeof(err))) {
        e->download_data = buffer.data;
        e->download_size = buffer.size;
        atomic_set(&e->state, ENTRY_STATE_DONE);
    } else {
        if (buffer.data) free(buffer.data);
        atomic_set(&e->state, ENTRY_STATE_ERROR);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

#ifdef _WIN32
static DWORD WINAPI async_local_thread(LPVOID param)
#else
static void *async_local_thread(void *param)
#endif
{
    ImageCacheEntry *e = (ImageCacheEntry *)param;
    int decoded = 0;
    {
        CP_SCOPE_CAT("image-decode-async", "img");
        decoded = decode_entry(e);
    }
    if (decoded) {
        atomic_set(&e->state, ENTRY_STATE_DONE);
    } else {
        atomic_set(&e->state, ENTRY_STATE_ERROR);
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

int image_cache_load_async(ImageCache *cache, const char *url_or_path,
                           ImageLoadCallback callback, void *user_data)
{
    if (!cache || !url_or_path || !url_or_path[0]) return -1;

    ImageCacheEntry *e = find_entry(cache, url_or_path);
    if (e) {
        if (e->pixels) {
            /* Already loaded: do NOT invoke the callback.  The pixels were
             * available before this call, so nothing changed — re-notifying
             * would mark the layout dirty on every render pass and the
             * quiescence loop would never settle. */
        } else if (e->state == ENTRY_STATE_LOADING) {
            /* Already pending: update callback/user_data so the new caller is
             * notified when it completes. */
            e->callback = callback;
            e->user_data = user_data;
        }
        return entry_index(cache, e);
    }

    /* For remote URLs we load synchronously on the calling thread.  The HTTP
     * backend is not guaranteed to be thread-safe for concurrent calls, and
     * this keeps the existing blocking behavior for network images while still
     * providing the async API entry point. */
    if (is_network_url(url_or_path) || url_is_data_url(url_or_path)) {
        int handle = image_cache_load(cache, url_or_path);
        if (handle >= 0 && callback) callback(url_or_path, user_data);
        return handle;
    }

    /* Local files are loaded in a background thread to keep the main loop
     * responsive for disk I/O. */
    e = add_entry(cache);
    if (!e) return -1;
    e->source = strdup(url_or_path);
    e->callback = callback;
    e->user_data = user_data;
    atomic_set(&e->state, ENTRY_STATE_LOADING);

#ifdef _WIN32
    e->thread = CreateThread(NULL, 0, async_local_thread, e, 0, NULL);
    if (!e->thread) {
        atomic_set(&e->state, ENTRY_STATE_ERROR);
        return -1;
    }
#else
    int rc = pthread_create(&e->thread, NULL, async_local_thread, e);
    if (rc != 0) {
        atomic_set(&e->state, ENTRY_STATE_ERROR);
        e->thread = 0;
        return -1;
    }
#endif

    return entry_index(cache, e);
}

bool image_cache_process_pending(ImageCache *cache)
{
    if (!cache) return false;
    bool progress = false;
    for (int i = 0; i < cache->count; i++) {
        ImageCacheEntry *e = cache->entries[i];
        if (!e) continue;
        long s = e->state;
        if (s != ENTRY_STATE_DONE && s != ENTRY_STATE_ERROR) continue;

        /* Join the worker so we can clean up the thread object. */
#ifdef _WIN32
        if (e->thread) {
            WaitForSingleObject(e->thread, INFINITE);
            CloseHandle(e->thread);
            e->thread = NULL;
        }
#else
        if (e->thread) {
            pthread_join(e->thread, NULL);
            e->thread = 0;
        }
#endif

        if (s == ENTRY_STATE_DONE) {
            /* For local files the worker already decoded; for URLs we decode
             * the downloaded buffer here. */
            if (!e->pixels) {
                decode_entry(e);
            }
            free(e->download_data);
            e->download_data = NULL;
            e->download_size = 0;
            if (e->callback) {
                e->callback(e->source, e->user_data);
                e->callback = NULL;
            }
        } else {
            free(e->download_data);
            e->download_data = NULL;
            e->download_size = 0;
        }
        atomic_set(&e->state, ENTRY_STATE_IDLE);
        progress = true;
    }
    return progress;
}

void image_cache_wait_pending(ImageCache *cache)
{
    if (!cache) return;
    bool progress = true;
    while (progress) {
        progress = false;
        for (int i = 0; i < cache->count; i++) {
            ImageCacheEntry *e = cache->entries[i];
            if (!e || e->state != ENTRY_STATE_LOADING) continue;
#ifdef _WIN32
            if (e->thread) {
                WaitForSingleObject(e->thread, INFINITE);
            }
#else
            if (e->thread) {
                pthread_join(e->thread, NULL);
            }
#endif
            progress = true;
        }
        if (progress) image_cache_process_pending(cache);
    }
}

bool image_cache_has_pending(ImageCache *cache)
{
    if (!cache) return false;
    for (int i = 0; i < cache->count; i++) {
        ImageCacheEntry *e = cache->entries[i];
        if (e && e->state == ENTRY_STATE_LOADING) return true;
    }
    return false;
}

bool image_cache_get(ImageCache *cache, int handle,
                     int *out_width, int *out_height, int *out_channels,
                     uint8_t **out_pixels)
{
    if (!cache || handle < 0 || handle >= cache->count) return false;
    ImageCacheEntry *e = cache->entries[handle];
    if (!e || !e->pixels) return false;
    if (out_width)    *out_width    = e->width;
    if (out_height)   *out_height   = e->height;
    if (out_channels) *out_channels = e->channels;
    if (out_pixels)   *out_pixels   = e->pixels;
    return true;
}
