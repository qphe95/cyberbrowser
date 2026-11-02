#ifndef BIDI_H
#define BIDI_H
/* The Unicode Bidirectional Algorithm (UAX #9): embedding levels of a
 * paragraph's characters and the visual order of a line (BIDI_DESIGN.md). */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BIDI_L, BIDI_R, BIDI_AL, BIDI_EN, BIDI_ES, BIDI_ET, BIDI_AN, BIDI_CS, BIDI_NSM, BIDI_BN, BIDI_B, BIDI_S,
    BIDI_WS, BIDI_ON, BIDI_LRE, BIDI_LRO, BIDI_RLE, BIDI_RLO, BIDI_PDF, BIDI_LRI, BIDI_RLI, BIDI_FSI, BIDI_PDI,
} BidiClass;

#define BIDI_MAX_DEPTH 125

BidiClass bidi_class(uint32_t cp);
/* The glyph drawn for cp at an odd (right-to-left) level: its mirror, or cp. */
uint32_t bidi_mirror(uint32_t cp);

/* P2 / P3: the level of the first strong character of cps[0, n) (isolates
 * skipped, stopping at a paragraph separator): 0, 1, or -1 when none. */
int bidi_first_strong_level(const uint32_t *cps, int n);

/* The embedding level of each of cps[0, n) (rules X1 - I2), into
 * levels[n], each paragraph (P1: up to and including a paragraph separator)
 * on its own.  base_level 0 or 1, or -1 for P2 / P3 (0 when no strong
 * character).  Characters X9 removes (embedding controls, BN) get the
 * level of the character before them, an embedding or override initiator
 * that of the one after.  Returns the first paragraph's level, or -1 when
 * out of memory. */
int bidi_resolve(const uint32_t *cps, int n, int base_level, uint8_t *levels);

/* L1 on the line cps[start, end) of a resolved paragraph: segment and
 * paragraph separators, and the white space (and isolate controls) before
 * them or at the line's end, back to the paragraph level. */
void bidi_line_levels(const uint32_t *cps, int start, int end, int para_level, uint8_t *levels);

/* L2: the visual order of a line of n levels: order[i] is the logical index
 * drawn i-th from the left. */
void bidi_reorder(const uint8_t *levels, int n, int *order);

#endif
