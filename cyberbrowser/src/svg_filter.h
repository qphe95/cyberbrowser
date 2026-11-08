#ifndef SVG_FILTER_H
#define SVG_FILTER_H

/* The filter property with url() in it, on the CPU: a function list applied to a layer
 * of what the box and its subtree painted (Filter Effects 1). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "html_dom.h"

typedef struct SvgFilterEnv {
    HtmlDocument *doc;
    /* a primitive's attribute or presentation property, NULL if not set */
    const char *(*prop)(void *ud, HtmlNode *n, const char *name, char *buf, size_t bufn);
    /* a CSS color to rgba, 0..1 */
    bool (*color)(void *ud, const char *text, double *rgba);
    /* an feImage's image, decoded (false: not there, or not yet) */
    bool (*image)(void *ud, const char *href, const uint8_t **px, int *w, int *h, int *ch);
    /* the element `id` rendered in user space over w x h, straight RGBA (malloc'ed) */
    bool (*element)(void *ud, const char *id, int w, int h, const uint8_t **px, int *ch);
    void *ud;
    double current[4];   /* currentColor */
    bool user_zero;      /* user space from the surface's corner (SVG elements), not the box's */
} SvgFilterEnv;

/* The filter element's region, x0 y0 x1 y1, for a reference box x y w h. */
void svg_filter_region(const SvgFilterEnv *env, HtmlNode *flt, const float box[4], float out[4]);

/* `layer` (W x H x 4, straight RGBA, the viewport's) through the list, in place. */
bool svg_filter_apply(const SvgFilterEnv *env, const char *list, const float box[4], float fs,
                      uint8_t *layer, int W, int H);

#endif
