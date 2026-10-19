/*
 * Browser API handle helpers
 * ============================================================================
 *
 * The browser API data structs (XMLHttpRequest, EventData, ...) live in the GC
 * heap and are referred to by GCHandle. Their handle types are plain typedefs
 * of GCHandle (see browser_api_impl_types.h) and fields are accessed with the
 * C macros from quickjs_handles.h:
 *
 *     HTMLVideoElementHandle vid = JS_GetOpaqueHandle(obj, js_video_class_id);
 *     if (SAFE_IS_NULL(vid)) return JS_NULL;
 *     SAFE_SET(vid, HTMLVideoElementHandle, onplay, callback);
 *
 * DOM nodes are not GC structs: they live in the node store (dom_store.h).
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
#include "dom_wrap.h"
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
        GC_SET(xhr->ctx, ctx);
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
        GC_SET(vid->ctx, ctx);
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
        GC_SET(map->entries, JS_NewObject(ctx));
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
        GC_SET(sr->host, host);
        strncpy(sr->mode, mode, sizeof(sr->mode) - 1);
        sr->mode[sizeof(sr->mode) - 1] = '\0';
        GC_SET(sr->innerHTML, JS_NewString(ctx, ""));
        GC_SET(sr->ctx, ctx);

        // Back the shadow root with a store fragment attached to the host,
        // so the normal tree mutation/traversal code works on it.
        NodeId frag = dom_create_fragment(DOM_NULL);
        NodeId hn = dom_node_of(host);
        if (hn) {
            g_dom.owner[dom_slot(frag)] = g_dom.owner[dom_slot(hn)];
            dom_attach_shadow(hn, frag);
        }
        sr = (ShadowRootData*)gc_deref(h);
        GC_SET(sr->dom_node, dom_wrap_new(frag));
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
        GC_SET(anim->ctx, ctx);
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
        GC_SET(mo->ctx, ctx);
        GC_SET(mo->callback, callback);
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
        GC_SET(ro->ctx, ctx);
        GC_SET(ro->callback, callback);
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
        GC_SET(io->ctx, ctx);
        GC_SET(io->callback, callback);
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
        GC_SET(po->ctx, ctx);
        GC_SET(po->callback, callback);
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
        GC_SET(ms->ctx, ctx);
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
        GC_SET(sb->ctx, ctx);
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
        GC_SET(ev->ctx, ctx);
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
        GC_SET(ev->base.ctx, ctx);
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
        GC_SET(ev->base.ctx, ctx);
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
        GC_SET(ev->base.ctx, ctx);
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
