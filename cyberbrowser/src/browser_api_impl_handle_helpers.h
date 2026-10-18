/*
 * Browser API handle helpers
 * ============================================================================
 *
 * The browser API data structs (XMLHttpRequest, DOMNode, EventData, ...) live
 * in the GC heap and are referred to by GCHandle. Their handle types are plain
 * typedefs of GCHandle (see browser_api_impl_types.h) and fields are accessed
 * with the C macros from quickjs_handles.h:
 *
 *     DOMNodeHandle node = dom_node_from_object(obj);
 *     if (SAFE_IS_NULL(node)) return JS_NULL;
 *     int type = SAFE_GET_OR(node, DOMNodeHandle, node_type, 0);
 *     SAFE_SET(node, DOMNodeHandle, parent_node, parent);
 *     SAFE_SET_STR(node, DOMNodeHandle, id, "main");
 *
 * Plain JS-object lookups use the QuickJS API directly:
 *
 *     XMLHttpRequestHandle xhr = JS_GetOpaqueHandle2(ctx, this_val, js_xhr_class_id);
 *     JS_SetOpaqueHandle(obj, xhr);
 *
 * This header holds the few operations that are more than a field access:
 * constructors (<type>_create) and multi-field helpers. Each one dereferences
 * the handle on entry, so the pointer it uses never outlives the call.
 * ============================================================================
 */

#ifndef BROWSER_API_IMPL_HANDLE_HELPERS_H
#define BROWSER_API_IMPL_HANDLE_HELPERS_H

#include "browser_api_impl_types.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* JS class IDs used by the helpers and by JS_GetOpaqueHandle lookups. */
#ifdef __cplusplus
extern "C" {
#endif
extern JSClassID js_xhr_class_id;
extern JSClassID js_video_class_id;
#ifdef __cplusplus
}
#endif
extern JSClassID js_dom_exception_class_id;
extern JSClassID js_map_class_id;
extern JSClassID js_shadow_root_class_id;
extern JSClassID js_custom_element_registry_class_id;
extern JSClassID js_animation_class_id;
extern JSClassID js_keyframe_effect_class_id;
extern JSClassID js_font_face_class_id;
extern JSClassID js_font_face_set_class_id;
extern JSClassID js_mutation_observer_class_id;
extern JSClassID js_resize_observer_class_id;
extern JSClassID js_intersection_observer_class_id;
extern JSClassID js_performance_class_id;
extern JSClassID js_performance_entry_class_id;
extern JSClassID js_performance_observer_class_id;
extern JSClassID js_performance_timing_class_id;
extern JSClassID js_dom_rect_class_id;
extern JSClassID js_dom_rect_read_only_class_id;
extern JSClassID js_media_source_class_id;
extern JSClassID js_source_buffer_class_id;
extern JSClassID js_dom_node_class_id;
extern JSClassID js_date_class_id;
extern JSClassID js_event_class_id;
extern JSClassID js_custom_event_class_id;
extern JSClassID js_mouse_event_class_id;
extern JSClassID js_focus_event_class_id;

/* ------------------------------------------------------------------------
 * XMLHttpRequestHandle  (XMLHttpRequest)
 * ------------------------------------------------------------------------ */

static inline XMLHttpRequestHandle xhr_create(JSContextHandle ctx) {
    GCHandle h = gc_alloc(sizeof(XMLHttpRequest), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        XMLHttpRequest* xhr = (XMLHttpRequest*)gc_deref(h);
        memset(xhr, 0, sizeof(XMLHttpRequest));
        xhr->ctx = ctx;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * HTMLVideoElementHandle  (HTMLVideoElement)
 * ------------------------------------------------------------------------ */

static inline HTMLVideoElementHandle video_create(JSContextHandle ctx) {
    GCHandle h = gc_alloc(sizeof(HTMLVideoElement), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        HTMLVideoElement* vid = (HTMLVideoElement*)gc_deref(h);
        memset(vid, 0, sizeof(HTMLVideoElement));
        vid->ctx = ctx;
        vid->paused = 1;
        vid->volume = 1.0;
        vid->playback_rate = 1.0;
        vid->default_playback_rate = 1.0;
        strncpy(vid->preload, "metadata", sizeof(vid->preload) - 1);
    }
    return h;
}

/* ------------------------------------------------------------------------
 * DOMExceptionDataHandle  (DOMExceptionData)
 * ------------------------------------------------------------------------ */

static inline DOMExceptionDataHandle dom_exception_create(void) {
    GCHandle h = gc_alloc(sizeof(DOMExceptionData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        DOMExceptionData* de = (DOMExceptionData*)gc_deref(h);
        memset(de, 0, sizeof(DOMExceptionData));
        strcpy(de->name, "Error");
    }
    return h;
}

/* ------------------------------------------------------------------------
 * MapDataHandle  (MapData)
 * ------------------------------------------------------------------------ */

static inline MapDataHandle map_data_create(JSContextHandle ctx) {
    GCHandle h = gc_alloc(sizeof(MapData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        MapData* map = (MapData*)gc_deref(h);
        memset(map, 0, sizeof(MapData));
        map->entries = JS_NewObject(ctx);
        map->size = 0;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * ShadowRootDataHandle  (ShadowRootData)
 * ------------------------------------------------------------------------ */

static inline ShadowRootDataHandle shadow_root_create(JSContextHandle ctx, GCValue host, const char* mode) {
    GCHandle h = gc_alloc(sizeof(ShadowRootData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        ShadowRootData* sr = (ShadowRootData*)gc_deref(h);
        memset(sr, 0, sizeof(ShadowRootData));
        sr->host = host;
        strncpy(sr->mode, mode, sizeof(sr->mode) - 1);
        sr->mode[sizeof(sr->mode) - 1] = '\0';
        sr->innerHTML = JS_NewString(ctx, "");
        sr->ctx = ctx;

        // Back the shadow root with a regular DOMNode fragment so it can
        // reuse the normal DOM tree mutation/traversal code.
        GCHandle dn = gc_alloc_grey(sizeof(DOMNode), JS_GC_OBJ_TYPE_DATA);
        if (dn != GC_HANDLE_NULL) {
            DOMNode* node = (DOMNode*)gc_deref(dn);
            memset(node, 0, sizeof(DOMNode));
            node->node_type = DOM_NODE_TYPE_DOCUMENT_FRAGMENT;
            strncpy(node->node_name, "#document-fragment", sizeof(node->node_name) - 1);
            node->node_name[sizeof(node->node_name) - 1] = '\0';
            node->ctx = ctx;
            node->parent_node = JS_NULL;
            node->first_child = JS_NULL;
            node->last_child = JS_NULL;
            node->previous_sibling = JS_NULL;
            node->next_sibling = JS_NULL;
            node->owner_document = JS_NULL;
            node->shadow_root = JS_NULL;
            node->computed_style_handle = GC_HANDLE_NULL;
            node->next_class_sibling = GC_HANDLE_NULL;
            node->next_tag_sibling = GC_HANDLE_NULL;
            node->js_object = JS_NULL;
            gc_publish(dn);
            sr->dom_node = dn;
        }
    }
    return h;
}

static inline bool shadow_root_is_closed(ShadowRootDataHandle h) {
    ShadowRootData *p = (ShadowRootData *)gc_deref(h);
    return p && strcmp(p->mode, "closed") == 0;
}

/* ------------------------------------------------------------------------
 * AnimationDataHandle  (AnimationData)
 * ------------------------------------------------------------------------ */

static inline AnimationDataHandle animation_create(JSContextHandle ctx) {
    GCHandle h = gc_alloc(sizeof(AnimationData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        AnimationData* anim = (AnimationData*)gc_deref(h);
        memset(anim, 0, sizeof(AnimationData));
        anim->ctx = ctx;
        anim->onfinish = JS_NULL;
        anim->effect = JS_NULL;
    }
    return h;
}

static inline const char* animation_play_state_string(AnimationDataHandle h) {
    static const char* states[] = {"idle", "running", "paused", "finished"};
    int state = SAFE_GET_OR(h, AnimationDataHandle, play_state, 0);
    if (state >= 0 && state < 4) return states[state];
    return "idle";
}

/* ------------------------------------------------------------------------
 * KeyFrameEffectDataHandle  (KeyFrameEffectData)
 * ------------------------------------------------------------------------ */

static inline KeyFrameEffectDataHandle keyframe_effect_create(void) {
    GCHandle h = gc_alloc(sizeof(KeyFrameEffectData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        KeyFrameEffectData* effect = (KeyFrameEffectData*)gc_deref(h);
        memset(effect, 0, sizeof(KeyFrameEffectData));
        strcpy(effect->easing, "linear");
    }
    return h;
}

/* ------------------------------------------------------------------------
 * FontFaceDataHandle  (FontFaceData)
 * ------------------------------------------------------------------------ */

static inline FontFaceDataHandle font_face_create(void) {
    GCHandle h = gc_alloc(sizeof(FontFaceData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        FontFaceData* ff = (FontFaceData*)gc_deref(h);
        memset(ff, 0, sizeof(FontFaceData));
    }
    return h;
}

/* ------------------------------------------------------------------------
 * MutationObserverDataHandle  (MutationObserverData)
 * ------------------------------------------------------------------------ */

static inline MutationObserverDataHandle mutation_observer_create(JSContextHandle ctx, GCValue callback) {
    GCHandle h = gc_alloc(sizeof(MutationObserverData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        MutationObserverData* mo = (MutationObserverData*)gc_deref(h);
        memset(mo, 0, sizeof(MutationObserverData));
        mo->ctx = ctx;
        mo->callback = callback;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * ResizeObserverDataHandle  (ResizeObserverData)
 * ------------------------------------------------------------------------ */

static inline ResizeObserverDataHandle resize_observer_create(JSContextHandle ctx, GCValue callback) {
    GCHandle h = gc_alloc(sizeof(ResizeObserverData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        ResizeObserverData* ro = (ResizeObserverData*)gc_deref(h);
        memset(ro, 0, sizeof(ResizeObserverData));
        ro->ctx = ctx;
        ro->callback = callback;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * IntersectionObserverDataHandle  (IntersectionObserverData)
 * ------------------------------------------------------------------------ */

static inline IntersectionObserverDataHandle intersection_observer_create(JSContextHandle ctx, GCValue callback) {
    GCHandle h = gc_alloc(sizeof(IntersectionObserverData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        IntersectionObserverData* io = (IntersectionObserverData*)gc_deref(h);
        memset(io, 0, sizeof(IntersectionObserverData));
        io->ctx = ctx;
        io->callback = callback;
        io->root = JS_NULL;
        strcpy(io->rootMargin, "0px");
        io->threshold = 0.0;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * PerformanceObserverDataHandle  (PerformanceObserverData)
 * ------------------------------------------------------------------------ */

static inline PerformanceObserverDataHandle performance_observer_create(JSContextHandle ctx, GCValue callback) {
    GCHandle h = gc_alloc(sizeof(PerformanceObserverData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        PerformanceObserverData* po = (PerformanceObserverData*)gc_deref(h);
        memset(po, 0, sizeof(PerformanceObserverData));
        po->ctx = ctx;
        po->callback = callback;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * DOMRectDataHandle  (DOMRectData)
 * ------------------------------------------------------------------------ */

static inline DOMRectDataHandle dom_rect_create(void) {
    GCHandle h = gc_alloc(sizeof(DOMRectData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        DOMRectData* rect = (DOMRectData*)gc_deref(h);
        memset(rect, 0, sizeof(DOMRectData));
    }
    return h;
}

static inline void dom_rect_compute_bounds(DOMRectDataHandle h) {
    DOMRectData *p = (DOMRectData *)gc_deref(h);
    if (p) {
        p->top = p->y;
        p->left = p->x;
        p->right = p->x + p->width;
        p->bottom = p->y + p->height;
    }
}

/* ------------------------------------------------------------------------
 * MediaSourceDataHandle  (MediaSourceData)
 * ------------------------------------------------------------------------ */

static inline MediaSourceDataHandle media_source_create(JSContextHandle ctx) {
    GCHandle h = gc_alloc(sizeof(MediaSourceData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        MediaSourceData* ms = (MediaSourceData*)gc_deref(h);
        memset(ms, 0, sizeof(MediaSourceData));
        ms->ctx = ctx;
        ms->ready_state = 0; // closed
        strcpy(ms->source_buffers, "[]");
        strcpy(ms->active_source_buffers, "[]");
    }
    return h;
}

/* ------------------------------------------------------------------------
 * SourceBufferDataHandle  (SourceBufferData)
 * ------------------------------------------------------------------------ */

static inline SourceBufferDataHandle source_buffer_create(JSContextHandle ctx) {
    GCHandle h = gc_alloc(sizeof(SourceBufferData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        SourceBufferData* sb = (SourceBufferData*)gc_deref(h);
        memset(sb, 0, sizeof(SourceBufferData));
        sb->ctx = ctx;
        sb->updating = 0;
        sb->track_count = 0;
        sb->parsed_duration = 0.0;
        strcpy(sb->mode, "segments");
    }
    return h;
}

static inline void source_buffer_set_append_data(SourceBufferDataHandle h, uint8_t* data, size_t size, size_t capacity) {
    SourceBufferData *p = (SourceBufferData *)gc_deref(h);
    if (p) {
        p->append_data = data;
        p->append_size = size;
        p->append_capacity = capacity;
    }
}

/* ------------------------------------------------------------------------
 * DOMNodeHandle  (DOMNode)
 * ------------------------------------------------------------------------ */

static inline DOMNodeHandle dom_node_create(JSContextHandle ctx, int node_type, const char* node_name) {
    /* Allocate grey so the node is invisible to readers/GC while its
     * fields are being written. */
    GCHandle h = gc_alloc_grey(sizeof(DOMNode), JS_GC_OBJ_TYPE_DATA);
    if (h == GC_HANDLE_NULL) return GC_HANDLE_NULL;
    
    DOMNode* node = (DOMNode*)gc_deref(h);
    memset(node, 0, sizeof(DOMNode));
    node->node_type = node_type;
    node->ctx = ctx;
    if (node_name) {
        strncpy(node->node_name, node_name, sizeof(node->node_name) - 1);
        node->node_name[sizeof(node->node_name) - 1] = '\0';
    }
    
    // Initialize all GCHandles to null
    node->parent_node = JS_NULL;
    node->first_child = JS_NULL;
    node->last_child = JS_NULL;
    node->previous_sibling = JS_NULL;
    node->next_sibling = JS_NULL;
    node->owner_document = JS_NULL;
    node->shadow_root = JS_NULL;
    node->computed_style_handle = GC_HANDLE_NULL;
    node->next_class_sibling = GC_HANDLE_NULL;
    node->next_tag_sibling = GC_HANDLE_NULL;
    node->js_object = JS_NULL;
    
    /* Fully initialized; publish so traversal and GC can see it. */
    gc_publish(h);
    return h;
}

static inline DOMNodeHandle dom_node_from_object(GCValue obj) {
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) return GC_HANDLE_NULL;
    GCHandle h = JS_GetOpaqueHandle(obj, js_dom_node_class_id);
    return h;
}

static inline DOMNodeHandle dom_node_from_object_check(JSContextHandle ctx, GCValue obj) {
    if (JS_IsNull(obj) || JS_IsUndefined(obj)) return GC_HANDLE_NULL;
    GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_dom_node_class_id);
    return h;
}

static inline void dom_node_attach_to_object(DOMNodeHandle h, GCValue obj) {
    if (h != GC_HANDLE_NULL) {
        JS_SetOpaqueHandle(obj, h);
        DOMNode *p = (DOMNode *)gc_deref(h);
        if (p) p->js_object = obj;
    }
}

static inline void dom_node_free_attributes(DOMNodeHandle h) {
    DOMNode *p = (DOMNode *)gc_deref(h);
    if (p && p->attributes) {
        free(p->attributes);
        p->attributes = NULL;
        p->attribute_count = 0;
        p->attribute_capacity = 0;
    }
}

static inline const char* dom_node_get_attribute(DOMNodeHandle h, const char* name) {
    DOMNode *p = (DOMNode *)gc_deref(h);
    if (!p || !name) return NULL;
    for (int i = 0; i < p->attribute_count; i++) {
        if (strcmp(p->attributes[i].name, name) == 0) {
            return p->attributes[i].value;
        }
    }
    return NULL;
}

static inline void dom_node_set_attribute(DOMNodeHandle h, const char* name, const char* value) {
    DOMNode *p = (DOMNode *)gc_deref(h);
    if (!p || !name || !value) return;
    
    // Check if attribute already exists
    for (int i = 0; i < p->attribute_count; i++) {
        if (strcmp(p->attributes[i].name, name) == 0) {
            strncpy(p->attributes[i].value, value, sizeof(p->attributes[i].value) - 1);
            p->attributes[i].value[sizeof(p->attributes[i].value) - 1] = '\0';
            // Keep the dedicated id/class caches in sync with the attribute
            // list so querySelector and .id/.className see the same value.
            if (strcmp(name, "id") == 0) {
                strncpy(p->id, value, sizeof(p->id) - 1);
                p->id[sizeof(p->id) - 1] = '\0';
            } else if (strcmp(name, "class") == 0) {
                strncpy(p->class_name, value, sizeof(p->class_name) - 1);
                p->class_name[sizeof(p->class_name) - 1] = '\0';
            }
            return;
        }
    }
    
    // Allocate or grow the attribute array on demand. It is owned by the
    // DOMNode and freed in the destructor/finalizer.
    if (p->attribute_count >= p->attribute_capacity) {
        int new_cap = p->attribute_capacity ? p->attribute_capacity * 2 : 4;
        if (new_cap < 4) new_cap = 4;
        if (new_cap > DOM_MAX_ATTRIBUTES) new_cap = DOM_MAX_ATTRIBUTES;
        if (p->attribute_count >= new_cap) return; // at hard limit
        DOMAttribute* new_attrs = (DOMAttribute*)malloc(sizeof(DOMAttribute) * new_cap);
        if (!new_attrs) return; // allocation failure
        if (p->attributes) {
            memcpy(new_attrs, p->attributes, sizeof(DOMAttribute) * p->attribute_count);
            free(p->attributes);
        }
        p->attributes = new_attrs;
        p->attribute_capacity = new_cap;
    }
    
    // Add new attribute
    memset(&p->attributes[p->attribute_count], 0, sizeof(DOMAttribute));
    strncpy(p->attributes[p->attribute_count].name, name, 
            sizeof(p->attributes[p->attribute_count].name) - 1);
    p->attributes[p->attribute_count].name[
        sizeof(p->attributes[p->attribute_count].name) - 1] = '\0';
    strncpy(p->attributes[p->attribute_count].value, value,
            sizeof(p->attributes[p->attribute_count].value) - 1);
    p->attributes[p->attribute_count].value[
        sizeof(p->attributes[p->attribute_count].value) - 1] = '\0';
    p->attribute_count++;
    if (strcmp(name, "id") == 0) {
        strncpy(p->id, value, sizeof(p->id) - 1);
        p->id[sizeof(p->id) - 1] = '\0';
    } else if (strcmp(name, "class") == 0) {
        strncpy(p->class_name, value, sizeof(p->class_name) - 1);
        p->class_name[sizeof(p->class_name) - 1] = '\0';
    }
}

static inline void dom_node_remove_attribute(DOMNodeHandle h, const char* name) {
    DOMNode *p = (DOMNode *)gc_deref(h);
    if (!p || !name) return;
    
    for (int i = 0; i < p->attribute_count; i++) {
        if (strcmp(p->attributes[i].name, name) == 0) {
            // Shift remaining attributes
            for (int j = i; j < p->attribute_count - 1; j++) {
                p->attributes[j] = p->attributes[j + 1];
            }
            p->attribute_count--;
            return;
        }
    }
}

/* ------------------------------------------------------------------------
 * EventHandle  (EventData)
 * ------------------------------------------------------------------------ */

static inline EventHandle event_from_object_check(JSContextHandle ctx, GCValue obj) {
    (void)ctx;
    GCHandle h = JS_GetOpaqueHandle(obj, js_event_class_id);
    if (h == GC_HANDLE_NULL) h = JS_GetOpaqueHandle(obj, js_custom_event_class_id);
    if (h == GC_HANDLE_NULL) h = JS_GetOpaqueHandle(obj, js_mouse_event_class_id);
    if (h == GC_HANDLE_NULL) h = JS_GetOpaqueHandle(obj, js_focus_event_class_id);
    return h;
}

static inline EventHandle event_create(JSContextHandle ctx, const char* type) {
    GCHandle h = gc_alloc(sizeof(EventData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        EventData* ev = (EventData*)gc_deref(h);
        memset(ev, 0, sizeof(EventData));
        if (type) {
            strncpy(ev->type, type, sizeof(ev->type) - 1);
            ev->type[sizeof(ev->type) - 1] = '\0';
        }
        ev->timeStamp = 0;
        ev->target = JS_NULL;
        ev->currentTarget = JS_NULL;
        ev->path = JS_NULL;
        ev->ctx = ctx;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * CustomEventHandle  (CustomEventData)
 * ------------------------------------------------------------------------ */

static inline CustomEventHandle custom_event_create(JSContextHandle ctx, const char* type) {
    GCHandle h = gc_alloc(sizeof(CustomEventData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        CustomEventData* ev = (CustomEventData*)gc_deref(h);
        memset(ev, 0, sizeof(CustomEventData));
        if (type) {
            strncpy(ev->base.type, type, sizeof(ev->base.type) - 1);
            ev->base.type[sizeof(ev->base.type) - 1] = '\0';
        }
        ev->base.timeStamp = 0;
        ev->base.target = JS_NULL;
        ev->base.currentTarget = JS_NULL;
        ev->base.path = JS_NULL;
        ev->base.ctx = ctx;
        ev->detail = JS_NULL;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * MouseEventHandle  (MouseEventData)
 * ------------------------------------------------------------------------ */

static inline MouseEventHandle mouse_event_create(JSContextHandle ctx, const char* type) {
    GCHandle h = gc_alloc(sizeof(MouseEventData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        MouseEventData* ev = (MouseEventData*)gc_deref(h);
        memset(ev, 0, sizeof(MouseEventData));
        if (type) {
            strncpy(ev->base.type, type, sizeof(ev->base.type) - 1);
            ev->base.type[sizeof(ev->base.type) - 1] = '\0';
        }
        ev->base.timeStamp = 0;
        ev->base.target = JS_NULL;
        ev->base.currentTarget = JS_NULL;
        ev->base.path = JS_NULL;
        ev->base.ctx = ctx;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * FocusEventHandle  (FocusEventData)
 * ------------------------------------------------------------------------ */

static inline FocusEventHandle focus_event_create(JSContextHandle ctx, const char* type) {
    GCHandle h = gc_alloc(sizeof(FocusEventData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        FocusEventData* ev = (FocusEventData*)gc_deref(h);
        memset(ev, 0, sizeof(FocusEventData));
        if (type) {
            strncpy(ev->base.type, type, sizeof(ev->base.type) - 1);
            ev->base.type[sizeof(ev->base.type) - 1] = '\0';
        }
        ev->base.timeStamp = 0;
        ev->base.target = JS_NULL;
        ev->base.currentTarget = JS_NULL;
        ev->base.path = JS_NULL;
        ev->base.ctx = ctx;
        ev->relatedTarget = JS_NULL;
    }
    return h;
}

/* ------------------------------------------------------------------------
 * DateDataHandle  (DateData)
 * ------------------------------------------------------------------------ */

static inline DateDataHandle date_data_create(void) {
    GCHandle h = gc_alloc(sizeof(DateData), JS_GC_OBJ_TYPE_DATA);
    if (h != GC_HANDLE_NULL) {
        DateData* d = (DateData*)gc_deref(h);
        memset(d, 0, sizeof(DateData));
        d->timestamp_ms = 0;
        d->is_valid = 1;
    }
    return h;
}

#endif /* BROWSER_API_IMPL_HANDLE_HELPERS_H */
