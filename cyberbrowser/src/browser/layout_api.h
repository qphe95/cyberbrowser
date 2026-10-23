/* Layout metrics for scripts: offsetWidth, clientWidth, getBoundingClientRect, ... */
#ifndef LAYOUT_API_H
#define LAYOUT_API_H

#include <stdbool.h>
#include <stdint.h>
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Border box of an element in page coordinates, from the cached layout.
 * Returns false (all zeros) when the element has no box: not connected,
 * display:none itself or through an ancestor, or not an element. */
typedef struct {
    double x, y, width, height;
    double border_top, border_right, border_bottom, border_left;
    bool inline_level;
} LayoutQueryBox;

bool layout_query_element_box(JSContextHandle ctx, GCValue element, LayoutQueryBox *out);

/* The element's used font size in px from the cached layout (computed
 * values for getComputedStyle when only a relative declaration is known). */
bool layout_query_font_size(JSContextHandle ctx, GCValue element, double *px);

/* offsetWidth/Height/Top/Left, offsetParent, clientWidth/Height/Top/Left,
 * scrollWidth/Height, scrollTop/Left on Element.prototype. */
void layout_api_install(JSContextHandle ctx, GCValue element_proto);

/* Forced layouts run / queries answered since start (diagnostics, tests). */
/* Forced layouts avoided: changes since the cached one could not alter a box. */
uint64_t layout_api_kept(void);
/* Of the forced layouts, those for a query inside a region a change altered. */
uint64_t layout_api_region_layouts(void);
void layout_api_stats(uint64_t *queries, uint64_t *layouts, uint64_t *layout_ms);
/* Queries answered from the cached layout without re-serializing. */
uint64_t layout_api_cheap_hits(void);
/* Layouts that styled display:none subtrees too, for a computed-style query
 * inside one. */
uint64_t layout_api_style_all_layouts(void);

/* Drop the cached layout (the next query lays out again). */
void layout_api_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif
