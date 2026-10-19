#ifndef IMAGE_CACHE_H
#define IMAGE_CACHE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

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

/* Request an image for painting at want_w x want_h pixels (0 = natural
 * size).  Starts the fetch when needed and returns the handle at once; the
 * decode targets the largest size requested (JPEG/WebP decode scaled down,
 * SVG rasterizes at that size).  Thread-safe. */
int image_cache_request(ImageCache *cache, const char *url_or_path, int want_w, int want_h);

/* Natural (intrinsic) size of an image, available as soon as its header has
 * been fetched — before any pixels are decoded.  Starts the fetch when
 * needed.  Returns false while unknown.  Thread-safe (layout threads). */
bool image_cache_natural_size(ImageCache *cache, const char *url_or_path,
                              int *out_w, int *out_h);
bool image_cache_get_natural(ImageCache *cache, int handle, int *out_w, int *out_h);

/* Load status (starts the fetch when needed): 1 = fetched and sized (decode
 * may still be running), 0 = in flight, -1 = failed.  Thread-safe. */
int image_cache_status(ImageCache *cache, const char *url_or_path);

/* Called (on the image_cache_process_pending thread) whenever any image
 * becomes sized, decoded or fails, in addition to per-load callbacks. */
void image_cache_set_notify(ImageCache *cache, ImageLoadCallback callback, void *user_data);

/* Wait up to timeout_ms for any image to make progress.  Returns true when a
 * notification is ready for image_cache_process_pending(). */
bool image_cache_wait_progress(ImageCache *cache, int timeout_ms);

typedef struct ImageCacheStats {
    int entries, ready, pending, errors;
    size_t pixel_bytes, compressed_bytes;
} ImageCacheStats;
void image_cache_stats(ImageCache *cache, ImageCacheStats *out);

/* Get decoded RGBA pixels (top-left origin).  Width and height are those of
 * the decoded pixels (possibly smaller than the natural size).  Returns
 * false if the handle is invalid or not decoded yet.  The pointer stays
 * valid until the next image_cache_process_pending() call. */
bool image_cache_get(ImageCache *cache, int handle,
                     int *out_width, int *out_height, int *out_channels,
                     uint8_t **out_pixels);

#ifdef __cplusplus
}
#endif

#endif /* IMAGE_CACHE_H */
