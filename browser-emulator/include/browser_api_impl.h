/*
 * Browser Stubs - C implementation of DOM/Browser APIs for QuickJS
 */
#ifndef BROWSER_API_IMPL_H
#define BROWSER_API_IMPL_H

#include <quickjs.h>

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

#endif // BROWSER_API_IMPL_H
