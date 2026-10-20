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

/* One table of an sfnt being assembled. */
typedef struct {
    uint32_t tag;
    const uint8_t *data;
    size_t len;
} FontSfntTable;

/* Write an sfnt from `tables` (sorted in place; offsets aligned, checksums
 * filled).  head.indexToLocFormat becomes loca_format when it is >= 0. */
bool font_sfnt_build(uint32_t flavor, FontSfntTable *tables, int n, int loca_format,
                     uint8_t **out, size_t *out_len);

/* ---- Variable fonts (font_instance.c) ---- */

typedef struct {
    uint32_t tag;        /* axis tag, e.g. 'wght' */
    float value;         /* user-space value, e.g. 500 */
} FontAxisValue;

#define FONT_AXIS_TAG(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* Does the sfnt carry TrueType variations (fvar + gvar)? */
bool font_is_variable(const uint8_t *sfnt, size_t len);

/* An axis's user-space range; false when the font has no such axis. */
bool font_axis_range(const uint8_t *sfnt, size_t len, uint32_t tag, float *min, float *def, float *max);

/* A static instance of a variable font at the given axis values (others at
 * their defaults): glyf / loca / hmtx rebuilt from the gvar deltas, the
 * variation tables dropped.  malloc'd into *out. */
bool font_instance(const uint8_t *sfnt, size_t len, const FontAxisValue *values, int nvalues,
                   uint8_t **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* FONT_LOADER_H */
