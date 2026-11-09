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
 * Every loaded typeface (a face, or a synthetic-bold face) gets a slot
 * (0..FONT_SLOTS_MAX-1); sizes are not slots: text draws at any size from
 * the text shaper's strikes and glyph atlas.  System faces keep their slots;
 * a web font's slot lasts while the web font cache (by URL, most recently
 * used first, bounded in files and bytes) keeps its file, and is reused for
 * another face a few documents after it was let go. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_SLOTS_MAX 1023

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
/* The same with the rest of what picks a web face (CSS Fonts 4, 5.2): oblique
 * rather than italic, at an angle, and a stretch in percent. */
int font_registry_resolve_style(const char *family_list, int weight, bool italic, bool oblique, float angle,
                                float stretch);

/* Register the @font-face rules in `css` (relative URLs against base_url).
 * Duplicates are ignored. */
void font_registry_scan_css(const char *css, size_t len, const char *base_url);
/* A new document: the faces declared so far are gone (the files stay loaded). */
void font_registry_forget_faces(void);
/* Which forgetting this is: a sheet kept across documents is scanned again in a new one. */
uint32_t font_registry_face_generation(void);

/* Fetch and decode the web fonts queued by font_registry_resolve (in
 * parallel).  Returns how many became usable. */
int font_registry_load_pending(void);
/* Web faces asked for and not loaded yet. */
int font_registry_queued_count(void);

/* Fallback fonts for glyphs the chosen font lacks: entry k (loaded on first
 * use; NULL when it does not exist on this system). */
int font_registry_fallback_count(void);
/* The fonts of a font-family list that are there, in its order (slots), at most max. */
int font_registry_resolve_chain(const char *family_list, int weight, bool italic, int *out, int max);
struct TextShaper *font_registry_fallback(int k);

/* Path (and collection index) of the regular face of a system family or
 * generic family; false when the system has none. */
bool font_registry_family_file(const char *family, char *path, size_t cap, int *face_index);

/* Load a font file directly as the default font (tests, tools). */
bool font_registry_set_default_file(const char *path);

#ifdef __cplusplus
}
#endif

/* @font-feature-values: the numbers `name` stands for in family's `kind` block
 * (styleset, stylistic, ...); the count, 0 when not defined. */
int font_feature_values_get(const char *family, const char *kind, const char *name, int *vals, int cap);
/* (@font-palette-values --name, for a font-family list: its base palette (kind 0 none, 1 light,
 * 2 dark, 3 index), override colors as text; false: no such rule for these families) */
bool font_palette_values_get(const char *families, const char *name, int *kind, int *index, char *overrides, size_t cap);
void font_feature_values_reset(void);

#endif /* FONT_REGISTRY_H */
