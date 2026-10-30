/*
 * computed_style.h - getComputedStyle's live snapshot of an element's style
 * (computed_style.c).
 */
#ifndef COMPUTED_STYLE_H
#define COMPUTED_STYLE_H

#include <stdbool.h>
#include <stddef.h>
#include <quickjs.h>
#include "dom_store.h"

typedef struct ComputedStyle ComputedStyle;

ComputedStyle *computed_style_new(NodeId el);
void computed_style_free(ComputedStyle *cs);
/* The resolved value of a property (or custom property) prop[0..len),
 * malloc'd: "" when the element has no style or prop is unknown. */
char *computed_style_value(JSContextHandle ctx, ComputedStyle *cs, const char *prop, size_t len);
/* Its declarations: every longhand. */
int computed_style_length(ComputedStyle *cs);
const char *computed_style_item(ComputedStyle *cs, int i);

#endif
