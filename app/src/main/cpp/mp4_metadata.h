#ifndef MP4_METADATA_H
#define MP4_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Embed album art into an existing MP4/M4A file by injecting a
   udta/meta/ilst/covr atom hierarchy.  Updates stco/co64 chunk offsets.
   Returns true on success. */
bool mp4_embed_album_art(const char *mp4_path, const uint8_t *image_data,
                         size_t image_size, bool is_jpeg);

/* Set the title (©nam) metadata tag in an existing MP4/M4A file.
   Injects a ©nam atom into the existing udta/meta/ilst hierarchy.
   Updates stco/co64 chunk offsets. Returns true on success. */
bool mp4_set_title(const char *mp4_path, const char *title);

#ifdef __cplusplus
}
#endif

#endif /* MP4_METADATA_H */
