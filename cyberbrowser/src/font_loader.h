#ifndef FONT_LOADER_H
#define FONT_LOADER_H

/* Web font decoding: WOFF (zlib) and WOFF2 (Brotli + the glyf / loca / hmtx
 * transforms) to a plain sfnt (TrueType / OpenType) that stb_truetype reads.
 * Plain sfnt input is copied through. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode `data` into malloc'd sfnt bytes (*out, *out_len).  Returns false for
 * unknown or malformed input (and font collections inside WOFF2). */
bool font_decode_web(const uint8_t *data, size_t len, uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* FONT_LOADER_H */
