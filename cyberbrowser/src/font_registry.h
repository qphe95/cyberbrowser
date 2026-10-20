#ifndef FONT_REGISTRY_H
#define FONT_REGISTRY_H

/* Font registry: the fonts text is measured and drawn with.
 *
 *  - System fonts: an index of the OS font folders (family names, weight and
 *    style read from each file's name / OS/2 tables), loaded on first use.
 *  - Web fonts: @font-face rules found in the page's style sheets, fetched
 *    over HTTP and decoded (WOFF / WOFF2 / TTF / OTF) when first needed.
 *  - Generic families (sans-serif, serif, monospace, system-ui, ...) map to
 *    the platform's usual system fonts.
 *  - A per-glyph fallback chain covers characters the chosen font lacks.
 *
 * Every loaded font gets a slot (0..FONT_SLOTS_MAX-1); glyph commands carry
 * the slot whose atlas holds their bitmap. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_SLOTS_MAX 255

struct TextShaper;

/* Index the system fonts (idempotent).  Called lazily by the other entry
 * points; call it up front to keep the scan off the first layout. */
void font_registry_init(void);

/* The shaper in `slot`, or NULL. */
struct TextShaper *font_registry_slot(int slot);

/* The default font's slot (the platform sans-serif), -1 when none exists. */
int font_registry_default_slot(void);

/* Resolve a CSS font-family list + weight + italic to a slot.  Web fonts that
 * are not loaded yet are queued (see font_registry_load_pending) and the next
 * family in the list stands in.  Thread-safe. */
int font_registry_resolve(const char *family_list, int weight, bool italic);

/* Register the @font-face rules in `css` (relative URLs against base_url).
 * Duplicates are ignored. */
void font_registry_scan_css(const char *css, size_t len, const char *base_url);

/* Fetch and decode the web fonts queued by font_registry_resolve (in
 * parallel).  Returns how many became usable. */
int font_registry_load_pending(void);

/* Fallback fonts for glyphs the chosen font lacks: entry k (loaded on first
 * use; NULL when it does not exist on this system). */
int font_registry_fallback_count(void);
struct TextShaper *font_registry_fallback(int k);

/* Path (and collection index) of the regular face of a system family or
 * generic family; false when the system has none. */
bool font_registry_family_file(const char *family, char *path, size_t cap, int *face_index);

/* Load a font file directly as the default font (tests, tools). */
bool font_registry_set_default_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* FONT_REGISTRY_H */
