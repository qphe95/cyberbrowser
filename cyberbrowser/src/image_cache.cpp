#include "image_cache.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define atomic_set(ptr, val) InterlockedExchange((ptr), (val))
#else
#include <pthread.h>
#define atomic_set(ptr, val) __sync_lock_test_and_set((ptr), (val))
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "http_download.h"
#include "platform.h"

#define LOG_TAG "image_cache"
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

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

static bool is_url(const char *s)
{
    return strncasecmp(s, "http://", 7) == 0 || strncasecmp(s, "https://", 8) == 0;
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
        pixels = stbi_load_from_memory((const stbi_uc *)e->download_data,
                                       (int)e->download_size,
                                       &width, &height, &channels, 4);
    } else {
        /* Local file path. */
        pixels = stbi_load(e->source, &width, &height, &channels, 4);
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

    if (is_url(url_or_path)) {
        HttpBuffer buffer = {0};
        char err[256] = {0};
        if (!http_get_to_memory(url_or_path, &buffer, err, sizeof(err))) {
            LOG_ERROR("Failed to download image %s: %s", url_or_path, err);
            cache->count--;
            free(e);
            return -1;
        }
        e->download_data = buffer.data;
        e->download_size = buffer.size;
    }

    if (!decode_entry(e)) {
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
    if (decode_entry(e)) {
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
            /* Already loaded synchronously: invoke callback immediately. */
            if (callback) callback(url_or_path, user_data);
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
    if (is_url(url_or_path)) {
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
