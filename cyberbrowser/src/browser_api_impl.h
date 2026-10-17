/*
 * Browser Stubs - C implementation of DOM/Browser APIs for QuickJS
 */
#ifndef BROWSER_API_IMPL_H
#define BROWSER_API_IMPL_H

#include <quickjs.h>
#include "browser_api_impl_handles.h"

// Class IDs for DOM/Browser APIs
extern JSClassID js_shadow_root_class_id;
extern JSClassID js_animation_class_id;
extern JSClassID js_keyframe_effect_class_id;
extern JSClassID js_font_face_class_id;
extern JSClassID js_font_face_set_class_id;
extern JSClassID js_custom_element_registry_class_id;
extern JSClassID js_mutation_observer_class_id;
extern JSClassID js_resize_observer_class_id;
extern JSClassID js_performance_timing_class_id;
extern JSClassID js_intersection_observer_class_id;
extern JSClassID js_performance_class_id;
extern JSClassID js_performance_entry_class_id;
extern JSClassID js_performance_observer_class_id;
extern JSClassID js_dom_rect_class_id;
extern JSClassID js_dom_rect_read_only_class_id;
extern JSClassID js_map_class_id;
extern JSClassID js_media_source_class_id;
extern JSClassID js_source_buffer_class_id;
extern JSClassID js_date_class_id;

// URL capture callback type
typedef void (*URLCaptureCallback)(const char *url);

// Set URL capture callback (called when URLs are captured via video.src, fetch, XHR, createObjectURL)
void browser_api_impl_set_url_capture_callback(URLCaptureCallback callback);

// Initialize all browser stubs
void init_browser_api_impl(JSContextHandle ctx, GCValue global);

// Helper to get a prototype from a constructor: Constructor.prototype
GCValue js_get_prototype(JSContextHandle ctx, GCValueConst ctor);

/* ============================================================================
 * Parallel CSS support helpers
 * ============================================================================ */

typedef struct CssComputedStyle CssComputedStyle;
typedef struct CssDocumentState CssDocumentState;

/* Allocate or return the existing computed-style object for a DOM node. */
GCHandle css_ensure_computed_style(DOMNodeHandle node);

/* Store a computed CSS property for a node. */
void css_computed_set_property(JSContextHandle ctx, DOMNodeHandle node,
                               JSAtom prop_atom, const char *value);

/* Look up a computed CSS property and return it as a JS value. */
GCValue css_computed_get_property(JSContextHandle ctx, DOMNodeHandle node,
                                  JSAtom prop_atom);

/* Apply a sorted/unsorted declaration list to the computed-style table. */
typedef struct CssAppliedDecl CssAppliedDecl;
void css_computed_apply_declarations(JSContextHandle ctx, DOMNodeHandle node,
                                     CssAppliedDecl *applied, int count);

/* Parse an inline style attribute and store its declarations in the
 * computed-style table. */
void css_computed_apply_inline_style(JSContextHandle ctx, DOMNodeHandle node,
                                     const char *style_attr);

/* Allocate and attach the per-document CSS index tables. */
CssDocumentState *css_document_state_ensure(JSRuntimeHandle rt);

/* Free the per-document CSS index tables.  Call before gc_cleanup(). */
void css_document_state_destroy(JSRuntimeHandle rt);

/* Clear all entries from the CSS index tables.  Call when a new document is
 * populated so lookups do not return nodes from a previous document. */
void css_document_state_clear(JSRuntimeHandle rt);

/* Insert a DOM node into the id/class/tag index tables. */
void css_index_insert_node(JSContextHandle ctx, DOMNodeHandle node);

/* Index-table readers. */
GCValue css_get_element_by_id(JSContextHandle ctx, JSAtom id);
GCValue css_get_elements_by_class_name(JSContextHandle ctx, JSAtom class_atom);
GCValue css_get_elements_by_tag_name(JSContextHandle ctx, JSAtom tag_atom);

#endif // BROWSER_API_IMPL_H
