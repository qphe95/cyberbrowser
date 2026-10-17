#ifndef IMAGE_CACHE_H
#define IMAGE_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ImageCache ImageCache;

/* Callback invoked on the main thread when an async image load completes.
 * The URL is provided for identification; user_data is the pointer passed to
 * image_cache_load_async(). */
typedef void (*ImageLoadCallback)(const char *url, void *user_data);

ImageCache *image_cache_create(void);
void        image_cache_destroy(ImageCache *cache);

/* Load an image from a local path or an http(s) URL. Returns a handle >= 0 or -1 on failure. */
int image_cache_load(ImageCache *cache, const char *url_or_path);

/* Start loading an image asynchronously. Returns a handle >= 0 immediately, or
 * -1 on failure. The handle may refer to a placeholder entry until the load
 * completes; use image_cache_get() to test whether decoded pixels are available.
 * When the download and decode finish, callback(url, user_data) is invoked from
 * image_cache_process_pending(). */
int image_cache_load_async(ImageCache *cache, const char *url_or_path,
                           ImageLoadCallback callback, void *user_data);

/* Poll pending async loads. Decode completed downloads and invoke callbacks.
 * Returns true if any pending operation made progress. */
bool image_cache_process_pending(ImageCache *cache);

/* Block until all pending async loads have completed or errored, then process
 * them.  Useful for tests and final cleanup before shutdown. */
void image_cache_wait_pending(ImageCache *cache);

/* Returns true while at least one async load has not yet completed or errored. */
bool image_cache_has_pending(ImageCache *cache);

/* Get decoded RGBA pixels (top-left origin). Returns false if handle invalid. */
bool image_cache_get(ImageCache *cache, int handle,
                     int *out_width, int *out_height, int *out_channels,
                     uint8_t **out_pixels);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_CACHE_H */
