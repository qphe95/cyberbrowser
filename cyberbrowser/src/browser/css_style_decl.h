/*
 * css_style_decl.h - CSSStyleDeclaration for element.style (css_style_decl.c).
 */
#ifndef CSS_STYLE_DECL_H
#define CSS_STYLE_DECL_H

#include <stdbool.h>
#include <quickjs.h>
#include "dom_store.h"

extern JSClassID js_css_style_decl_class_id;

/* The class, and the methods and IDL attributes on proto
 * (CSSStyleDeclaration.prototype). */
void css_style_decl_install(JSContextHandle ctx, GCValue proto);

/* A new declaration block for element owner (DOM_NULL: none), with the
 * declarations of text (the style attribute; may be NULL). */
GCValue css_style_decl_new(JSContextHandle ctx, NodeId owner, const char *text);
bool css_style_decl_is(JSContextHandle ctx, GCValue obj);
/* getComputedStyle(el): a read-only, live declaration block of its resolved values. */
GCValue css_style_decl_new_computed(JSContextHandle ctx, NodeId el);
void css_style_decl_set_owner(JSContextHandle ctx, GCValue obj, NodeId owner);

/* cssText (malloc'd), or NULL when obj is not a block. */
char *css_style_decl_text(JSContextHandle ctx, GCValue obj);
/* The style attribute changed: its declarations replace the block's
 * (without writing the attribute back). */
void css_style_decl_sync_from_attr(JSContextHandle ctx, GCValue obj, const char *text);
/* setProperty from C: false when nothing was set. */
bool css_style_decl_set(JSContextHandle ctx, GCValue obj, const char *prop, const char *value, const char *priority);

/* The declarations (longhands and custom properties), in order. */
int css_style_decl_count(JSContextHandle ctx, GCValue obj);
bool css_style_decl_at(JSContextHandle ctx, GCValue obj, int i, const char **name, const char **value, bool *important);

#endif
