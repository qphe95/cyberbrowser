#include "image_cache.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "http_download.h"
#include "platform.h"

#define LOG_TAG "image_cache"
#define LOG_ERROR(...) platform_log(LOG_LEVEL_ERROR, LOG_TAG, __VA_ARGS__)

typedef struct ImageCacheEntry {
    char      *source;
    int        width;
    int        height;
    int        channels;
    uint8_t   *pixels;
} ImageCacheEntry;

struct ImageCache {
    ImageCacheEntry *entries;
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
        free(cache->entries[i].source);
        stbi_image_free(cache->entries[i].pixels);
    }
    free(cache->entries);
    free(cache);
}

static int add_entry(ImageCache *cache)
{
    if (cache->count >= cache->capacity) {
        int new_cap = cache->capacity ? cache->capacity * 2 : 8;
        ImageCacheEntry *ne = (ImageCacheEntry *)realloc(cache->entries,
                                                         new_cap * sizeof(ImageCacheEntry));
        if (!ne) return -1;
        cache->entries = ne;
        cache->capacity = new_cap;
    }
    int idx = cache->count++;
    memset(&cache->entries[idx], 0, sizeof(ImageCacheEntry));
    return idx;
}

int image_cache_load(ImageCache *cache, const char *url_or_path)
{
    if (!cache || !url_or_path || !url_or_path[0]) return -1;

    /* Already loaded? */
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i].source && strcmp(cache->entries[i].source, url_or_path) == 0) {
            return i;
        }
    }

    int width = 0, height = 0, channels = 0;
    uint8_t *pixels = NULL;

    if (is_url(url_or_path)) {
        HttpBuffer buffer = {0};
        char err[256] = {0};
        if (!http_get_to_memory(url_or_path, &buffer, err, sizeof(err))) {
            LOG_ERROR("Failed to download image %s: %s", url_or_path, err);
            return -1;
        }
        pixels = stbi_load_from_memory((const stbi_uc *)buffer.data,
                                       (int)buffer.size,
                                       &width, &height, &channels, 4);
        http_free_buffer(&buffer);
    } else {
        pixels = stbi_load(url_or_path, &width, &height, &channels, 4);
    }

    if (!pixels) {
        LOG_ERROR("Failed to decode image %s", url_or_path);
        return -1;
    }

    int idx = add_entry(cache);
    if (idx < 0) {
        stbi_image_free(pixels);
        return -1;
    }
    ImageCacheEntry *e = &cache->entries[idx];
    e->source = strdup(url_or_path);
    e->width = width;
    e->height = height;
    e->channels = 4;
    e->pixels = pixels;
    return idx;
}

bool image_cache_get(ImageCache *cache, int handle,
                     int *out_width, int *out_height, int *out_channels,
                     uint8_t **out_pixels)
{
    if (!cache || handle < 0 || handle >= cache->count) return false;
    ImageCacheEntry *e = &cache->entries[handle];
    if (!e->pixels) return false;
    if (out_width)    *out_width    = e->width;
    if (out_height)   *out_height   = e->height;
    if (out_channels) *out_channels = e->channels;
    if (out_pixels)   *out_pixels   = e->pixels;
    return true;
}
