#ifndef IMAGE_CACHE_H
#define IMAGE_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ImageCache ImageCache;

ImageCache *image_cache_create(void);
void        image_cache_destroy(ImageCache *cache);

/* Load an image from a local path or an http(s) URL. Returns a handle >= 0 or -1 on failure. */
int image_cache_load(ImageCache *cache, const char *url_or_path);

/* Get decoded RGBA pixels (top-left origin). Returns false if handle invalid. */
bool image_cache_get(ImageCache *cache, int handle,
                     int *out_width, int *out_height, int *out_channels,
                     uint8_t **out_pixels);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_CACHE_H */
