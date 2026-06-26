/*
 * Browser Stubs Handle Classes
 * 
 * These C++ classes provide GC-safe access to browser stubs data structures.
 * They wrap GCHandle values and dereference only when needed, making them
 * safe for use with a compacting garbage collector.
 * 
 * Usage:
 *   XMLHttpRequestHandle xhr = XMLHttpRequestHandle::create(ctx);
 *   xhr.set_url("https://example.com");
 *   xhr.set_ready_state(1);
 *   
 *   // Or from an existing JS object:
 *   GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_xhr_class_id);
 *   XMLHttpRequestHandle xhr2(h);
 *   if (xhr2.valid()) {
 *       const char* url = xhr2.url();
 *   }
 */

#ifndef BROWSER_API_IMPL_HANDLES_H
#define BROWSER_API_IMPL_HANDLES_H

#include "browser_api_impl_types.h"
#include <string.h>

/* Forward declaration of external class IDs */
extern "C" JSClassID js_xhr_class_id;
extern "C" JSClassID js_video_class_id;
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
extern JSClassID js_date_class_id;
extern JSClassID js_event_class_id;
extern JSClassID js_custom_event_class_id;
extern JSClassID js_mouse_event_class_id;
extern JSClassID js_focus_event_class_id;

/* ============================================================================
 * GCHandleBase CRTP base template
 * ============================================================================ */
template <typename Derived, typename DataT, JSClassID& ClassId>
class GCHandleBase {
protected:
    GCHandle handle_;

    DataT* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (DataT*)gc_deref(handle_);
    }

public:
    GCHandleBase() : handle_(GC_HANDLE_NULL) {}
    explicit GCHandleBase(GCHandle handle) : handle_(handle) {}
    GCHandleBase(const GCHandleBase& other) : handle_(other.handle_) {}
    GCHandleBase(GCHandleBase&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }

    GCHandleBase& operator=(const GCHandleBase& other) {
        handle_ = other.handle_;
        return *this;
    }

    GCHandleBase& operator=(GCHandleBase&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }

    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }

    static Derived from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, ClassId);
        return Derived(h);
    }

    static Derived from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, ClassId);
        return Derived(h);
    }

    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
    }
};


/* ============================================================================
 * XMLHttpRequestHandle
 * ============================================================================ */
class XMLHttpRequestHandle : public GCHandleBase<XMLHttpRequestHandle, XMLHttpRequest, js_xhr_class_id> {
public:
    using Base = GCHandleBase<XMLHttpRequestHandle, XMLHttpRequest, js_xhr_class_id>;

    XMLHttpRequestHandle() : Base() {}
    explicit XMLHttpRequestHandle(GCHandle handle) : Base(handle) {}

    /* Create new instance from GC */
    static XMLHttpRequestHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(XMLHttpRequest), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            XMLHttpRequest* xhr = (XMLHttpRequest*)gc_deref(h);
            memset(xhr, 0, sizeof(XMLHttpRequest));
            xhr->ctx = ctx;
        }
        return XMLHttpRequestHandle(h);
    }

    /* Property accessors */
    const char* url() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->url : "";
    }

    void set_url(const char* url) {
        XMLHttpRequest* p = get_ptr();
        if (p) {
            strncpy(p->url, url, sizeof(p->url) - 1);
            p->url[sizeof(p->url) - 1] = '\0';
        }
    }

    const char* method() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->method : "";
    }

    void set_method(const char* method) {
        XMLHttpRequest* p = get_ptr();
        if (p) {
            strncpy(p->method, method, sizeof(p->method) - 1);
            p->method[sizeof(p->method) - 1] = '\0';
        }
    }

    int ready_state() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->ready_state : 0;
    }

    void set_ready_state(int state) {
        XMLHttpRequest* p = get_ptr();
        if (p) p->ready_state = state;
    }

    int status() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->status : 0;
    }

    void set_status(int status) {
        XMLHttpRequest* p = get_ptr();
        if (p) p->status = status;
    }

    const char* response_text() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->response_text : "";
    }

    void set_response_text(const char* text) {
        XMLHttpRequest* p = get_ptr();
        if (p) {
            strncpy(p->response_text, text, sizeof(p->response_text) - 1);
            p->response_text[sizeof(p->response_text) - 1] = '\0';
        }
    }

    const char* response_headers() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->response_headers : "";
    }

    void set_response_headers(const char* headers) {
        XMLHttpRequest* p = get_ptr();
        if (p) {
            strncpy(p->response_headers, headers, sizeof(p->response_headers) - 1);
            p->response_headers[sizeof(p->response_headers) - 1] = '\0';
        }
    }

    GCValue onload() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->onload : JS_NULL;
    }

    void set_onload(GCValue val) {
        XMLHttpRequest* p = get_ptr();
        if (p) p->onload = val;
    }

    GCValue onerror() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->onerror : JS_NULL;
    }

    void set_onerror(GCValue val) {
        XMLHttpRequest* p = get_ptr();
        if (p) p->onerror = val;
    }

    GCValue onreadystatechange() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->onreadystatechange : JS_NULL;
    }

    void set_onreadystatechange(GCValue val) {
        XMLHttpRequest* p = get_ptr();
        if (p) p->onreadystatechange = val;
    }

    GCValue headers() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->headers : JS_NULL;
    }

    void set_headers(GCValue val) {
        XMLHttpRequest* p = get_ptr();
        if (p) p->headers = val;
    }

    const char* request_body() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->request_body : "";
    }

    void set_request_body(const char* body) {
        XMLHttpRequest* p = get_ptr();
        if (p) {
            strncpy(p->request_body, body, sizeof(p->request_body) - 1);
            p->request_body[sizeof(p->request_body) - 1] = '\0';
        }
    }

    JSContextHandle context() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * HTMLVideoElementHandle
 * ============================================================================ */
class HTMLVideoElementHandle : public GCHandleBase<HTMLVideoElementHandle, HTMLVideoElement, js_video_class_id> {
public:
    using Base = GCHandleBase<HTMLVideoElementHandle, HTMLVideoElement, js_video_class_id>;

    HTMLVideoElementHandle() : Base() {}
    explicit HTMLVideoElementHandle(GCHandle handle) : Base(handle) {}

    static HTMLVideoElementHandle create(JSContextHandle ctx) {
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
        return HTMLVideoElementHandle(h);
    }

    /* Property accessors */
    const char* id() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->id : "";
    }

    void set_id(const char* id) {
        HTMLVideoElement* p = get_ptr();
        if (p) {
            strncpy(p->id, id, sizeof(p->id) - 1);
            p->id[sizeof(p->id) - 1] = '\0';
        }
    }

    const char* src() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->src : "";
    }

    void set_src(const char* src) {
        HTMLVideoElement* p = get_ptr();
        if (p) {
            strncpy(p->src, src, sizeof(p->src) - 1);
            p->src[sizeof(p->src) - 1] = '\0';
        }
    }

    int ready_state() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->ready_state : 0;
    }

    void set_ready_state(int state) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->ready_state = state;
    }

    int network_state() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->network_state : 0;
    }

    void set_network_state(int state) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->network_state = state;
    }

    double current_time() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->current_time : 0.0;
    }

    void set_current_time(double time) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->current_time = time;
    }

    double duration() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->duration : 0.0;
    }

    void set_duration(double dur) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->duration = dur;
    }

    bool paused() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->paused : true;
    }

    void set_paused(bool val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->paused = val;
    }

    bool ended() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->ended : false;
    }

    void set_ended(bool val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->ended = val;
    }

    bool autoplay() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->autoplay : false;
    }

    void set_autoplay(bool val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->autoplay = val;
    }

    bool loop() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->loop : false;
    }

    void set_loop(bool val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->loop = val;
    }

    bool muted() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->muted : false;
    }

    void set_muted(bool val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->muted = val;
    }

    double volume() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->volume : 1.0;
    }

    void set_volume(double val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->volume = val;
    }

    double playback_rate() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->playback_rate : 1.0;
    }

    void set_playback_rate(double val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->playback_rate = val;
    }

    double default_playback_rate() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->default_playback_rate : 1.0;
    }

    void set_default_playback_rate(double val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->default_playback_rate = val;
    }

    const char* preload() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->preload : "";
    }

    void set_preload(const char* val) {
        HTMLVideoElement* p = get_ptr();
        if (p) {
            strncpy(p->preload, val, sizeof(p->preload) - 1);
            p->preload[sizeof(p->preload) - 1] = '\0';
        }
    }

    const char* cross_origin() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->cross_origin : "";
    }

    void set_cross_origin(const char* val) {
        HTMLVideoElement* p = get_ptr();
        if (p) {
            strncpy(p->cross_origin, val, sizeof(p->cross_origin) - 1);
            p->cross_origin[sizeof(p->cross_origin) - 1] = '\0';
        }
    }

    GCValue onloadstart() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->onloadstart : JS_NULL;
    }

    void set_onloadstart(GCValue val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->onloadstart = val;
    }

    GCValue onloadedmetadata() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->onloadedmetadata : JS_NULL;
    }

    void set_onloadedmetadata(GCValue val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->onloadedmetadata = val;
    }

    GCValue oncanplay() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->oncanplay : JS_NULL;
    }

    void set_oncanplay(GCValue val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->oncanplay = val;
    }

    GCValue onplay() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->onplay : JS_NULL;
    }

    void set_onplay(GCValue val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->onplay = val;
    }

    GCValue onplaying() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->onplaying : JS_NULL;
    }

    void set_onplaying(GCValue val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->onplaying = val;
    }

    GCValue onerror() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->onerror : JS_NULL;
    }

    void set_onerror(GCValue val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->onerror = val;
    }

    GCValue onvolumechange() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->onvolumechange : JS_NULL;
    }

    void set_onvolumechange(GCValue val) {
        HTMLVideoElement* p = get_ptr();
        if (p) p->onvolumechange = val;
    }

    JSContextHandle context() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * DOMExceptionDataHandle
 * ============================================================================ */
class DOMExceptionDataHandle : public GCHandleBase<DOMExceptionDataHandle, DOMExceptionData, js_dom_exception_class_id> {
public:
    using Base = GCHandleBase<DOMExceptionDataHandle, DOMExceptionData, js_dom_exception_class_id>;

    DOMExceptionDataHandle() : Base() {}
    explicit DOMExceptionDataHandle(GCHandle handle) : Base(handle) {}

    static DOMExceptionDataHandle create() {
        GCHandle h = gc_alloc(sizeof(DOMExceptionData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            DOMExceptionData* de = (DOMExceptionData*)gc_deref(h);
            memset(de, 0, sizeof(DOMExceptionData));
            strcpy(de->name, "Error");
        }
        return DOMExceptionDataHandle(h);
    }

    const char* name() const {
        DOMExceptionData* p = get_ptr();
        return p ? p->name : "";
    }

    void set_name(const char* name) {
        DOMExceptionData* p = get_ptr();
        if (p) {
            strncpy(p->name, name, sizeof(p->name) - 1);
            p->name[sizeof(p->name) - 1] = '\0';
        }
    }

    const char* message() const {
        DOMExceptionData* p = get_ptr();
        return p ? p->message : "";
    }

    void set_message(const char* msg) {
        DOMExceptionData* p = get_ptr();
        if (p) {
            strncpy(p->message, msg, sizeof(p->message) - 1);
            p->message[sizeof(p->message) - 1] = '\0';
        }
    }

    int code() const {
        DOMExceptionData* p = get_ptr();
        return p ? p->code : 0;
    }

    void set_code(int code) {
        DOMExceptionData* p = get_ptr();
        if (p) p->code = code;
    }
};

/* ============================================================================
 * MapDataHandle
 * ============================================================================ */
class MapDataHandle : public GCHandleBase<MapDataHandle, MapData, js_map_class_id> {
public:
    using Base = GCHandleBase<MapDataHandle, MapData, js_map_class_id>;

    MapDataHandle() : Base() {}
    explicit MapDataHandle(GCHandle handle) : Base(handle) {}

    static MapDataHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(MapData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            MapData* map = (MapData*)gc_deref(h);
            memset(map, 0, sizeof(MapData));
            map->entries = JS_NewObject(ctx);
            map->size = 0;
        }
        return MapDataHandle(h);
    }

    GCValue entries() const {
        MapData* p = get_ptr();
        return p ? p->entries : JS_NULL;
    }

    void set_entries(GCValue val) {
        MapData* p = get_ptr();
        if (p) p->entries = val;
    }

    int size() const {
        MapData* p = get_ptr();
        return p ? p->size : 0;
    }

    void set_size(int size) {
        MapData* p = get_ptr();
        if (p) p->size = size;
    }

    void increment_size() {
        MapData* p = get_ptr();
        if (p) p->size++;
    }

    void decrement_size() {
        MapData* p = get_ptr();
        if (p) p->size--;
    }
};

/* ============================================================================
 * ShadowRootDataHandle
 * ============================================================================ */
class ShadowRootDataHandle : public GCHandleBase<ShadowRootDataHandle, ShadowRootData, js_shadow_root_class_id> {
public:
    using Base = GCHandleBase<ShadowRootDataHandle, ShadowRootData, js_shadow_root_class_id>;

    ShadowRootDataHandle() : Base() {}
    explicit ShadowRootDataHandle(GCHandle handle) : Base(handle) {}

    static ShadowRootDataHandle create(JSContextHandle ctx, GCValue host, const char* mode) {
        GCHandle h = gc_alloc(sizeof(ShadowRootData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            ShadowRootData* sr = (ShadowRootData*)gc_deref(h);
            memset(sr, 0, sizeof(ShadowRootData));
            sr->host = host;
            strncpy(sr->mode, mode, sizeof(sr->mode) - 1);
            sr->mode[sizeof(sr->mode) - 1] = '\0';
            sr->innerHTML = JS_NewString(ctx, "");
            sr->first_child = JS_NULL;
            sr->last_child = JS_NULL;
            sr->child_count = 0;
            sr->ctx = ctx;
        }
        return ShadowRootDataHandle(h);
    }

    GCValue host() const {
        ShadowRootData* p = get_ptr();
        return p ? p->host : JS_NULL;
    }

    void set_host(GCValue val) {
        ShadowRootData* p = get_ptr();
        if (p) p->host = val;
    }

    const char* mode() const {
        ShadowRootData* p = get_ptr();
        return p ? p->mode : "";
    }

    void set_mode(const char* mode) {
        ShadowRootData* p = get_ptr();
        if (p) {
            strncpy(p->mode, mode, sizeof(p->mode) - 1);
            p->mode[sizeof(p->mode) - 1] = '\0';
        }
    }

    bool is_closed() const {
        ShadowRootData* p = get_ptr();
        return p && strcmp(p->mode, "closed") == 0;
    }

    GCValue innerHTML() const {
        ShadowRootData* p = get_ptr();
        return p ? p->innerHTML : JS_NULL;
    }

    void set_innerHTML(GCValue val) {
        ShadowRootData* p = get_ptr();
        if (p) p->innerHTML = val;
    }

    GCValue first_child() const {
        ShadowRootData* p = get_ptr();
        return p ? p->first_child : JS_NULL;
    }

    void set_first_child(GCValue val) {
        ShadowRootData* p = get_ptr();
        if (p) p->first_child = val;
    }

    GCValue last_child() const {
        ShadowRootData* p = get_ptr();
        return p ? p->last_child : JS_NULL;
    }

    void set_last_child(GCValue val) {
        ShadowRootData* p = get_ptr();
        if (p) p->last_child = val;
    }

    int child_count() const {
        ShadowRootData* p = get_ptr();
        return p ? p->child_count : 0;
    }

    void set_child_count(int count) {
        ShadowRootData* p = get_ptr();
        if (p) p->child_count = count;
    }

    void increment_child_count() {
        ShadowRootData* p = get_ptr();
        if (p) p->child_count++;
    }

    void decrement_child_count() {
        ShadowRootData* p = get_ptr();
        if (p) p->child_count--;
    }

    JSContextHandle context() const {
        ShadowRootData* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * CustomElementRegistryDataHandle
 * ============================================================================ */
class CustomElementRegistryDataHandle : public GCHandleBase<CustomElementRegistryDataHandle, CustomElementRegistryData, js_custom_element_registry_class_id> {
public:
    using Base = GCHandleBase<CustomElementRegistryDataHandle, CustomElementRegistryData, js_custom_element_registry_class_id>;

    CustomElementRegistryDataHandle() : Base() {}
    explicit CustomElementRegistryDataHandle(GCHandle handle) : Base(handle) {}

    static CustomElementRegistryDataHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(CustomElementRegistryData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            CustomElementRegistryData* cer = (CustomElementRegistryData*)gc_deref(h);
            memset(cer, 0, sizeof(CustomElementRegistryData));
            cer->registry = JS_NewObject(ctx);
        }
        return CustomElementRegistryDataHandle(h);
    }

    GCValue registry() const {
        CustomElementRegistryData* p = get_ptr();
        return p ? p->registry : JS_NULL;
    }

    void set_registry(GCValue val) {
        CustomElementRegistryData* p = get_ptr();
        if (p) p->registry = val;
    }
};

/* ============================================================================
 * AnimationDataHandle
 * ============================================================================ */
class AnimationDataHandle : public GCHandleBase<AnimationDataHandle, AnimationData, js_animation_class_id> {
public:
    using Base = GCHandleBase<AnimationDataHandle, AnimationData, js_animation_class_id>;

    AnimationDataHandle() : Base() {}
    explicit AnimationDataHandle(GCHandle handle) : Base(handle) {}

    static AnimationDataHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(AnimationData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            AnimationData* anim = (AnimationData*)gc_deref(h);
            memset(anim, 0, sizeof(AnimationData));
            anim->ctx = ctx;
            anim->onfinish = JS_NULL;
            anim->effect = JS_NULL;
        }
        return AnimationDataHandle(h);
    }

    double current_time() const {
        AnimationData* p = get_ptr();
        return p ? p->current_time : 0.0;
    }

    void set_current_time(double val) {
        AnimationData* p = get_ptr();
        if (p) p->current_time = val;
    }

    double duration() const {
        AnimationData* p = get_ptr();
        return p ? p->duration : 0.0;
    }

    void set_duration(double val) {
        AnimationData* p = get_ptr();
        if (p) p->duration = val;
    }

    int play_state() const {
        AnimationData* p = get_ptr();
        return p ? p->play_state : 0;
    }

    void set_play_state(int val) {
        AnimationData* p = get_ptr();
        if (p) p->play_state = val;
    }

    const char* play_state_string() const {
        static const char* states[] = {"idle", "running", "paused", "finished"};
        int state = play_state();
        if (state >= 0 && state < 4) return states[state];
        return "idle";
    }

    void set_playing() { set_play_state(1); }

    void set_paused() { set_play_state(2); }

    void set_finished() { set_play_state(3); }

    void set_idle() { set_play_state(0); }

    bool is_playing() const { return play_state() == 1; }

    bool is_paused() const { return play_state() == 2; }

    bool is_finished() const { return play_state() == 3; }

    bool is_idle() const { return play_state() == 0; }

    GCValue onfinish() const {
        AnimationData* p = get_ptr();
        return p ? p->onfinish : JS_NULL;
    }

    void set_onfinish(GCValue val) {
        AnimationData* p = get_ptr();
        if (p) p->onfinish = val;
    }

    GCValue effect() const {
        AnimationData* p = get_ptr();
        return p ? p->effect : JS_NULL;
    }

    void set_effect(GCValue val) {
        AnimationData* p = get_ptr();
        if (p) p->effect = val;
    }

    JSContextHandle context() const {
        AnimationData* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * KeyFrameEffectDataHandle
 * ============================================================================ */
class KeyFrameEffectDataHandle : public GCHandleBase<KeyFrameEffectDataHandle, KeyFrameEffectData, js_keyframe_effect_class_id> {
public:
    using Base = GCHandleBase<KeyFrameEffectDataHandle, KeyFrameEffectData, js_keyframe_effect_class_id>;

    KeyFrameEffectDataHandle() : Base() {}
    explicit KeyFrameEffectDataHandle(GCHandle handle) : Base(handle) {}

    static KeyFrameEffectDataHandle create() {
        GCHandle h = gc_alloc(sizeof(KeyFrameEffectData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            KeyFrameEffectData* effect = (KeyFrameEffectData*)gc_deref(h);
            memset(effect, 0, sizeof(KeyFrameEffectData));
            strcpy(effect->easing, "linear");
        }
        return KeyFrameEffectDataHandle(h);
    }

    GCValue target() const {
        KeyFrameEffectData* p = get_ptr();
        return p ? p->target : JS_NULL;
    }

    void set_target(GCValue val) {
        KeyFrameEffectData* p = get_ptr();
        if (p) p->target = val;
    }

    GCValue keyframes() const {
        KeyFrameEffectData* p = get_ptr();
        return p ? p->keyframes : JS_NULL;
    }

    void set_keyframes(GCValue val) {
        KeyFrameEffectData* p = get_ptr();
        if (p) p->keyframes = val;
    }

    double duration() const {
        KeyFrameEffectData* p = get_ptr();
        return p ? p->duration : 0.0;
    }

    void set_duration(double val) {
        KeyFrameEffectData* p = get_ptr();
        if (p) p->duration = val;
    }

    const char* easing() const {
        KeyFrameEffectData* p = get_ptr();
        return p ? p->easing : "linear";
    }

    void set_easing(const char* val) {
        KeyFrameEffectData* p = get_ptr();
        if (p) {
            strncpy(p->easing, val, sizeof(p->easing) - 1);
            p->easing[sizeof(p->easing) - 1] = '\0';
        }
    }
};

/* ============================================================================
 * FontFaceDataHandle
 * ============================================================================ */
class FontFaceDataHandle : public GCHandleBase<FontFaceDataHandle, FontFaceData, js_font_face_class_id> {
public:
    using Base = GCHandleBase<FontFaceDataHandle, FontFaceData, js_font_face_class_id>;

    FontFaceDataHandle() : Base() {}
    explicit FontFaceDataHandle(GCHandle handle) : Base(handle) {}

    static FontFaceDataHandle create() {
        GCHandle h = gc_alloc(sizeof(FontFaceData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            FontFaceData* ff = (FontFaceData*)gc_deref(h);
            memset(ff, 0, sizeof(FontFaceData));
        }
        return FontFaceDataHandle(h);
    }

    const char* family() const {
        FontFaceData* p = get_ptr();
        return p ? p->family : "";
    }

    void set_family(const char* val) {
        FontFaceData* p = get_ptr();
        if (p) {
            strncpy(p->family, val, sizeof(p->family) - 1);
            p->family[sizeof(p->family) - 1] = '\0';
        }
    }

    const char* source() const {
        FontFaceData* p = get_ptr();
        return p ? p->source : "";
    }

    void set_source(const char* val) {
        FontFaceData* p = get_ptr();
        if (p) {
            strncpy(p->source, val, sizeof(p->source) - 1);
            p->source[sizeof(p->source) - 1] = '\0';
        }
    }

    const char* display() const {
        FontFaceData* p = get_ptr();
        return p ? p->display : "";
    }

    void set_display(const char* val) {
        FontFaceData* p = get_ptr();
        if (p) {
            strncpy(p->display, val, sizeof(p->display) - 1);
            p->display[sizeof(p->display) - 1] = '\0';
        }
    }
};

/* ============================================================================
 * FontFaceSetDataHandle
 * ============================================================================ */
class FontFaceSetDataHandle : public GCHandleBase<FontFaceSetDataHandle, FontFaceSetData, js_font_face_set_class_id> {
public:
    using Base = GCHandleBase<FontFaceSetDataHandle, FontFaceSetData, js_font_face_set_class_id>;

    FontFaceSetDataHandle() : Base() {}
    explicit FontFaceSetDataHandle(GCHandle handle) : Base(handle) {}

    static FontFaceSetDataHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(FontFaceSetData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            FontFaceSetData* ffs = (FontFaceSetData*)gc_deref(h);
            memset(ffs, 0, sizeof(FontFaceSetData));
            ffs->loaded_fonts = JS_NewArray(ctx);
        }
        return FontFaceSetDataHandle(h);
    }

    GCValue loaded_fonts() const {
        FontFaceSetData* p = get_ptr();
        return p ? p->loaded_fonts : JS_NULL;
    }

    void set_loaded_fonts(GCValue val) {
        FontFaceSetData* p = get_ptr();
        if (p) p->loaded_fonts = val;
    }
};

/* ============================================================================
 * MutationObserverDataHandle
 * ============================================================================ */
class MutationObserverDataHandle : public GCHandleBase<MutationObserverDataHandle, MutationObserverData, js_mutation_observer_class_id> {
public:
    using Base = GCHandleBase<MutationObserverDataHandle, MutationObserverData, js_mutation_observer_class_id>;

    MutationObserverDataHandle() : Base() {}
    explicit MutationObserverDataHandle(GCHandle handle) : Base(handle) {}

    static MutationObserverDataHandle create(JSContextHandle ctx, GCValue callback) {
        GCHandle h = gc_alloc(sizeof(MutationObserverData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            MutationObserverData* mo = (MutationObserverData*)gc_deref(h);
            memset(mo, 0, sizeof(MutationObserverData));
            mo->ctx = ctx;
            mo->callback = callback;
        }
        return MutationObserverDataHandle(h);
    }

    GCValue callback() const {
        MutationObserverData* p = get_ptr();
        return p ? p->callback : JS_NULL;
    }

    void set_callback(GCValue val) {
        MutationObserverData* p = get_ptr();
        if (p) p->callback = val;
    }

    JSContextHandle context() const {
        MutationObserverData* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * ResizeObserverDataHandle
 * ============================================================================ */
class ResizeObserverDataHandle : public GCHandleBase<ResizeObserverDataHandle, ResizeObserverData, js_resize_observer_class_id> {
public:
    using Base = GCHandleBase<ResizeObserverDataHandle, ResizeObserverData, js_resize_observer_class_id>;

    ResizeObserverDataHandle() : Base() {}
    explicit ResizeObserverDataHandle(GCHandle handle) : Base(handle) {}

    static ResizeObserverDataHandle create(JSContextHandle ctx, GCValue callback) {
        GCHandle h = gc_alloc(sizeof(ResizeObserverData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            ResizeObserverData* ro = (ResizeObserverData*)gc_deref(h);
            memset(ro, 0, sizeof(ResizeObserverData));
            ro->ctx = ctx;
            ro->callback = callback;
        }
        return ResizeObserverDataHandle(h);
    }

    GCValue callback() const {
        ResizeObserverData* p = get_ptr();
        return p ? p->callback : JS_NULL;
    }

    void set_callback(GCValue val) {
        ResizeObserverData* p = get_ptr();
        if (p) p->callback = val;
    }

    JSContextHandle context() const {
        ResizeObserverData* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * IntersectionObserverDataHandle
 * ============================================================================ */
class IntersectionObserverDataHandle : public GCHandleBase<IntersectionObserverDataHandle, IntersectionObserverData, js_intersection_observer_class_id> {
public:
    using Base = GCHandleBase<IntersectionObserverDataHandle, IntersectionObserverData, js_intersection_observer_class_id>;

    IntersectionObserverDataHandle() : Base() {}
    explicit IntersectionObserverDataHandle(GCHandle handle) : Base(handle) {}

    static IntersectionObserverDataHandle create(JSContextHandle ctx, GCValue callback) {
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
        return IntersectionObserverDataHandle(h);
    }

    GCValue callback() const {
        IntersectionObserverData* p = get_ptr();
        return p ? p->callback : JS_NULL;
    }

    void set_callback(GCValue val) {
        IntersectionObserverData* p = get_ptr();
        if (p) p->callback = val;
    }

    GCValue root() const {
        IntersectionObserverData* p = get_ptr();
        return p ? p->root : JS_NULL;
    }

    void set_root(GCValue val) {
        IntersectionObserverData* p = get_ptr();
        if (p) p->root = val;
    }

    const char* root_margin() const {
        IntersectionObserverData* p = get_ptr();
        return p ? p->rootMargin : "0px";
    }

    void set_root_margin(const char* val) {
        IntersectionObserverData* p = get_ptr();
        if (p) {
            strncpy(p->rootMargin, val, sizeof(p->rootMargin) - 1);
            p->rootMargin[sizeof(p->rootMargin) - 1] = '\0';
        }
    }

    double threshold() const {
        IntersectionObserverData* p = get_ptr();
        return p ? p->threshold : 0.0;
    }

    void set_threshold(double val) {
        IntersectionObserverData* p = get_ptr();
        if (p) p->threshold = val;
    }

    JSContextHandle context() const {
        IntersectionObserverData* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * PerformanceDataHandle
 * ============================================================================ */
class PerformanceDataHandle : public GCHandleBase<PerformanceDataHandle, PerformanceData, js_performance_class_id> {
public:
    using Base = GCHandleBase<PerformanceDataHandle, PerformanceData, js_performance_class_id>;

    PerformanceDataHandle() : Base() {}
    explicit PerformanceDataHandle(GCHandle handle) : Base(handle) {}

    static PerformanceDataHandle create() {
        GCHandle h = gc_alloc(sizeof(PerformanceData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            PerformanceData* perf = (PerformanceData*)gc_deref(h);
            memset(perf, 0, sizeof(PerformanceData));
        }
        return PerformanceDataHandle(h);
    }

    double start_time() const {
        PerformanceData* p = get_ptr();
        return p ? p->start_time : 0.0;
    }

    void set_start_time(double val) {
        PerformanceData* p = get_ptr();
        if (p) p->start_time = val;
    }
};

/* ============================================================================
 * PerformanceEntryDataHandle
 * ============================================================================ */
class PerformanceEntryDataHandle : public GCHandleBase<PerformanceEntryDataHandle, PerformanceEntryData, js_performance_entry_class_id> {
public:
    using Base = GCHandleBase<PerformanceEntryDataHandle, PerformanceEntryData, js_performance_entry_class_id>;

    PerformanceEntryDataHandle() : Base() {}
    explicit PerformanceEntryDataHandle(GCHandle handle) : Base(handle) {}

    static PerformanceEntryDataHandle create() {
        GCHandle h = gc_alloc(sizeof(PerformanceEntryData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            PerformanceEntryData* entry = (PerformanceEntryData*)gc_deref(h);
            memset(entry, 0, sizeof(PerformanceEntryData));
        }
        return PerformanceEntryDataHandle(h);
    }

    const char* name() const {
        PerformanceEntryData* p = get_ptr();
        return p ? p->name : "";
    }

    void set_name(const char* val) {
        PerformanceEntryData* p = get_ptr();
        if (p) {
            strncpy(p->name, val, sizeof(p->name) - 1);
            p->name[sizeof(p->name) - 1] = '\0';
        }
    }

    const char* entry_type() const {
        PerformanceEntryData* p = get_ptr();
        return p ? p->entryType : "";
    }

    void set_entry_type(const char* val) {
        PerformanceEntryData* p = get_ptr();
        if (p) {
            strncpy(p->entryType, val, sizeof(p->entryType) - 1);
            p->entryType[sizeof(p->entryType) - 1] = '\0';
        }
    }

    double start_time() const {
        PerformanceEntryData* p = get_ptr();
        return p ? p->startTime : 0.0;
    }

    void set_start_time(double val) {
        PerformanceEntryData* p = get_ptr();
        if (p) p->startTime = val;
    }

    double duration() const {
        PerformanceEntryData* p = get_ptr();
        return p ? p->duration : 0.0;
    }

    void set_duration(double val) {
        PerformanceEntryData* p = get_ptr();
        if (p) p->duration = val;
    }
};

/* ============================================================================
 * PerformanceObserverDataHandle
 * ============================================================================ */
class PerformanceObserverDataHandle : public GCHandleBase<PerformanceObserverDataHandle, PerformanceObserverData, js_performance_observer_class_id> {
public:
    using Base = GCHandleBase<PerformanceObserverDataHandle, PerformanceObserverData, js_performance_observer_class_id>;

    PerformanceObserverDataHandle() : Base() {}
    explicit PerformanceObserverDataHandle(GCHandle handle) : Base(handle) {}

    static PerformanceObserverDataHandle create(JSContextHandle ctx, GCValue callback) {
        GCHandle h = gc_alloc(sizeof(PerformanceObserverData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            PerformanceObserverData* po = (PerformanceObserverData*)gc_deref(h);
            memset(po, 0, sizeof(PerformanceObserverData));
            po->ctx = ctx;
            po->callback = callback;
        }
        return PerformanceObserverDataHandle(h);
    }

    GCValue callback() const {
        PerformanceObserverData* p = get_ptr();
        return p ? p->callback : JS_NULL;
    }

    void set_callback(GCValue val) {
        PerformanceObserverData* p = get_ptr();
        if (p) p->callback = val;
    }

    JSContextHandle context() const {
        PerformanceObserverData* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * PerformanceTimingDataHandle
 * ============================================================================ */
class PerformanceTimingDataHandle : public GCHandleBase<PerformanceTimingDataHandle, PerformanceTimingData, js_performance_timing_class_id> {
public:
    using Base = GCHandleBase<PerformanceTimingDataHandle, PerformanceTimingData, js_performance_timing_class_id>;

    PerformanceTimingDataHandle() : Base() {}
    explicit PerformanceTimingDataHandle(GCHandle handle) : Base(handle) {}

    static PerformanceTimingDataHandle create() {
        GCHandle h = gc_alloc(sizeof(PerformanceTimingData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            PerformanceTimingData* pt = (PerformanceTimingData*)gc_deref(h);
            memset(pt, 0, sizeof(PerformanceTimingData));
        }
        return PerformanceTimingDataHandle(h);
    }

#define DEFINE_TIMING_ACCESSOR(name) \
    double name() const { \
        PerformanceTimingData* p = get_ptr(); \
        return p ? p->name : 0.0; \
    } \
    void set_##name(double val) { \
        PerformanceTimingData* p = get_ptr(); \
        if (p) p->name = val; \
    }

    DEFINE_TIMING_ACCESSOR(navigationStart)
    DEFINE_TIMING_ACCESSOR(unloadEventStart)
    DEFINE_TIMING_ACCESSOR(unloadEventEnd)
    DEFINE_TIMING_ACCESSOR(redirectStart)
    DEFINE_TIMING_ACCESSOR(redirectEnd)
    DEFINE_TIMING_ACCESSOR(fetchStart)
    DEFINE_TIMING_ACCESSOR(domainLookupStart)
    DEFINE_TIMING_ACCESSOR(domainLookupEnd)
    DEFINE_TIMING_ACCESSOR(connectStart)
    DEFINE_TIMING_ACCESSOR(connectEnd)
    DEFINE_TIMING_ACCESSOR(secureConnectionStart)
    DEFINE_TIMING_ACCESSOR(requestStart)
    DEFINE_TIMING_ACCESSOR(responseStart)
    DEFINE_TIMING_ACCESSOR(responseEnd)
    DEFINE_TIMING_ACCESSOR(domLoading)
    DEFINE_TIMING_ACCESSOR(domInteractive)
    DEFINE_TIMING_ACCESSOR(domContentLoadedEventStart)
    DEFINE_TIMING_ACCESSOR(domContentLoadedEventEnd)
    DEFINE_TIMING_ACCESSOR(domComplete)
    DEFINE_TIMING_ACCESSOR(loadEventStart)
    DEFINE_TIMING_ACCESSOR(loadEventEnd)
    
#undef DEFINE_TIMING_ACCESSOR
};

/* ============================================================================
 * DOMRectDataHandle
 * ============================================================================ */
class DOMRectDataHandle : public GCHandleBase<DOMRectDataHandle, DOMRectData, js_dom_rect_class_id> {
public:
    using Base = GCHandleBase<DOMRectDataHandle, DOMRectData, js_dom_rect_class_id>;

    DOMRectDataHandle() : Base() {}
    explicit DOMRectDataHandle(GCHandle handle) : Base(handle) {}

    static DOMRectDataHandle create() {
        GCHandle h = gc_alloc(sizeof(DOMRectData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            DOMRectData* rect = (DOMRectData*)gc_deref(h);
            memset(rect, 0, sizeof(DOMRectData));
        }
        return DOMRectDataHandle(h);
    }

    static DOMRectDataHandle from_object(GCValue obj, JSClassID class_id) {
        GCHandle h = JS_GetOpaqueHandle(obj, class_id);
        return DOMRectDataHandle(h);
    }

    static DOMRectDataHandle from_object_check(JSContextHandle ctx, GCValue obj, JSClassID class_id) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, class_id);
        return DOMRectDataHandle(h);
    }

    static DOMRectDataHandle from_dom_rect(GCValue obj) {
        extern JSClassID js_dom_rect_class_id;
        return from_object(obj, js_dom_rect_class_id);
    }

    static DOMRectDataHandle from_dom_rect_read_only(GCValue obj) {
        extern JSClassID js_dom_rect_read_only_class_id;
        return from_object(obj, js_dom_rect_read_only_class_id);
    }

    double x() const {
        DOMRectData* p = get_ptr();
        return p ? p->x : 0.0;
    }

    void set_x(double val) {
        DOMRectData* p = get_ptr();
        if (p) p->x = val;
    }

    double y() const {
        DOMRectData* p = get_ptr();
        return p ? p->y : 0.0;
    }

    void set_y(double val) {
        DOMRectData* p = get_ptr();
        if (p) p->y = val;
    }

    double width() const {
        DOMRectData* p = get_ptr();
        return p ? p->width : 0.0;
    }

    void set_width(double val) {
        DOMRectData* p = get_ptr();
        if (p) p->width = val;
    }

    double height() const {
        DOMRectData* p = get_ptr();
        return p ? p->height : 0.0;
    }

    void set_height(double val) {
        DOMRectData* p = get_ptr();
        if (p) p->height = val;
    }

    double top() const {
        DOMRectData* p = get_ptr();
        return p ? p->top : 0.0;
    }

    void set_top(double val) {
        DOMRectData* p = get_ptr();
        if (p) p->top = val;
    }

    double right() const {
        DOMRectData* p = get_ptr();
        return p ? p->right : 0.0;
    }

    void set_right(double val) {
        DOMRectData* p = get_ptr();
        if (p) p->right = val;
    }

    double bottom() const {
        DOMRectData* p = get_ptr();
        return p ? p->bottom : 0.0;
    }

    void set_bottom(double val) {
        DOMRectData* p = get_ptr();
        if (p) p->bottom = val;
    }

    double left() const {
        DOMRectData* p = get_ptr();
        return p ? p->left : 0.0;
    }

    void set_left(double val) {
        DOMRectData* p = get_ptr();
        if (p) p->left = val;
    }

    /* Initialize computed values from x, y, width, height */
    void compute_bounds() {
        DOMRectData* p = get_ptr();
        if (p) {
            p->top = p->y;
            p->left = p->x;
            p->right = p->x + p->width;
            p->bottom = p->y + p->height;
        }
    }
};

/* ============================================================================
 * MediaSourceDataHandle
 * ============================================================================ */
class MediaSourceDataHandle : public GCHandleBase<MediaSourceDataHandle, MediaSourceData, js_media_source_class_id> {
public:
    using Base = GCHandleBase<MediaSourceDataHandle, MediaSourceData, js_media_source_class_id>;

    MediaSourceDataHandle() : Base() {}
    explicit MediaSourceDataHandle(GCHandle handle) : Base(handle) {}

    static MediaSourceDataHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(MediaSourceData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            MediaSourceData* ms = (MediaSourceData*)gc_deref(h);
            memset(ms, 0, sizeof(MediaSourceData));
            ms->ctx = ctx;
            ms->ready_state = 0; // closed
            strcpy(ms->source_buffers, "[]");
            strcpy(ms->active_source_buffers, "[]");
        }
        return MediaSourceDataHandle(h);
    }

    int ready_state() const {
        MediaSourceData* p = get_ptr();
        return p ? p->ready_state : 0;
    }

    void set_ready_state(int state) {
        MediaSourceData* p = get_ptr();
        if (p) p->ready_state = state;
    }

    double duration() const {
        MediaSourceData* p = get_ptr();
        return p ? p->duration : 0.0;
    }

    void set_duration(double val) {
        MediaSourceData* p = get_ptr();
        if (p) p->duration = val;
    }

    const char* source_buffers() const {
        MediaSourceData* p = get_ptr();
        return p ? p->source_buffers : "[]";
    }

    GCValue onsourceopen() const {
        MediaSourceData* p = get_ptr();
        return p ? p->onsourceopen : JS_NULL;
    }

    void set_onsourceopen(GCValue val) {
        MediaSourceData* p = get_ptr();
        if (p) p->onsourceopen = val;
    }

    GCValue onsourceended() const {
        MediaSourceData* p = get_ptr();
        return p ? p->onsourceended : JS_NULL;
    }

    void set_onsourceended(GCValue val) {
        MediaSourceData* p = get_ptr();
        if (p) p->onsourceended = val;
    }

    GCValue onsourceclose() const {
        MediaSourceData* p = get_ptr();
        return p ? p->onsourceclose : JS_NULL;
    }

    void set_onsourceclose(GCValue val) {
        MediaSourceData* p = get_ptr();
        if (p) p->onsourceclose = val;
    }

    JSContextHandle context() const {
        MediaSourceData* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * SourceBufferDataHandle
 * ============================================================================ */
class SourceBufferDataHandle : public GCHandleBase<SourceBufferDataHandle, SourceBufferData, js_source_buffer_class_id> {
public:
    using Base = GCHandleBase<SourceBufferDataHandle, SourceBufferData, js_source_buffer_class_id>;

    SourceBufferDataHandle() : Base() {}
    explicit SourceBufferDataHandle(GCHandle handle) : Base(handle) {}

    static SourceBufferDataHandle create(JSContextHandle ctx) {
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
        return SourceBufferDataHandle(h);
    }

    const char* mime_type() const {
        SourceBufferData* p = get_ptr();
        return p ? p->mime_type : "";
    }

    void set_mime_type(const char* val) {
        SourceBufferData* p = get_ptr();
        if (p) {
            strncpy(p->mime_type, val, sizeof(p->mime_type) - 1);
            p->mime_type[sizeof(p->mime_type) - 1] = '\0';
        }
    }

    const char* mode() const {
        SourceBufferData* p = get_ptr();
        return p ? p->mode : "segments";
    }

    void set_mode(const char* val) {
        SourceBufferData* p = get_ptr();
        if (p) {
            strncpy(p->mode, val, sizeof(p->mode) - 1);
            p->mode[sizeof(p->mode) - 1] = '\0';
        }
    }

    int updating() const {
        SourceBufferData* p = get_ptr();
        return p ? p->updating : 0;
    }

    void set_updating(int val) {
        SourceBufferData* p = get_ptr();
        if (p) p->updating = val;
    }

    double timestamp_offset() const {
        SourceBufferData* p = get_ptr();
        return p ? p->timestamp_offset : 0.0;
    }

    void set_timestamp_offset(double val) {
        SourceBufferData* p = get_ptr();
        if (p) p->timestamp_offset = val;
    }

    JSContextHandle context() const {
        SourceBufferData* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }

    uint8_t* append_data() const {
        SourceBufferData* p = get_ptr();
        return p ? p->append_data : nullptr;
    }

    size_t append_size() const {
        SourceBufferData* p = get_ptr();
        return p ? p->append_size : 0;
    }

    size_t append_capacity() const {
        SourceBufferData* p = get_ptr();
        return p ? p->append_capacity : 0;
    }

    void set_append_data(uint8_t* data, size_t size, size_t capacity) {
        SourceBufferData* p = get_ptr();
        if (p) {
            p->append_data = data;
            p->append_size = size;
            p->append_capacity = capacity;
        }
    }

    int track_count() const {
        SourceBufferData* p = get_ptr();
        return p ? p->track_count : 0;
    }

    void set_track_count(int val) {
        SourceBufferData* p = get_ptr();
        if (p) p->track_count = val;
    }

    double parsed_duration() const {
        SourceBufferData* p = get_ptr();
        return p ? p->parsed_duration : 0.0;
    }

    void set_parsed_duration(double val) {
        SourceBufferData* p = get_ptr();
        if (p) p->parsed_duration = val;
    }
};

extern JSClassID js_dom_node_class_id;

/* ============================================================================
 * DOMNodeHandle
 * ============================================================================ */
class DOMNodeHandle : public GCHandleBase<DOMNodeHandle, DOMNode, js_dom_node_class_id> {
public:
    using Base = GCHandleBase<DOMNodeHandle, DOMNode, js_dom_node_class_id>;

    DOMNodeHandle() : Base() {}
    explicit DOMNodeHandle(GCHandle handle) : Base(handle) {}

    // Create a new DOM node
    static DOMNodeHandle create(JSContextHandle ctx, int node_type, const char* node_name) {
        /* Allocate grey so the node is invisible to readers/GC while its
         * fields are being written. */
        GCHandle h = gc_alloc_grey(sizeof(DOMNode), JS_GC_OBJ_TYPE_DATA);
        if (h == GC_HANDLE_NULL) return DOMNodeHandle();
        
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
        return DOMNodeHandle(h);
    }

    // Get from a JS object
    static DOMNodeHandle from_object(GCValue obj) {
        if (JS_IsNull(obj) || JS_IsUndefined(obj)) return DOMNodeHandle();
        GCHandle h = JS_GetOpaqueHandle(obj, js_dom_node_class_id);
        return DOMNodeHandle(h);
    }

    static DOMNodeHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        if (JS_IsNull(obj) || JS_IsUndefined(obj)) return DOMNodeHandle();
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_dom_node_class_id);
        return DOMNodeHandle(h);
    }

    GCValue js_object() const {
        DOMNode* p = get_ptr();
        return p ? p->js_object : JS_NULL;
    }

    void set_js_object(GCValue val) {
        DOMNode* p = get_ptr();
        if (p) p->js_object = val;
    }

    void attach_to_object(GCValue obj) const {
        if (valid()) {
            JS_SetOpaqueHandle(obj, handle_);
            DOMNode* p = get_ptr();
            if (p) p->js_object = obj;
        }
    }

    // Node type
    int node_type() const {
        DOMNode* p = get_ptr();
        return p ? p->node_type : 0;
    }

    void set_node_type(int val) {
        DOMNode* p = get_ptr();
        if (p) p->node_type = val;
    }

    // Node name
    const char* node_name() const {
        DOMNode* p = get_ptr();
        return p ? p->node_name : "";
    }

    void set_node_name(const char* val) {
        DOMNode* p = get_ptr();
        if (p && val) {
            strncpy(p->node_name, val, sizeof(p->node_name) - 1);
            p->node_name[sizeof(p->node_name) - 1] = '\0';
        }
    }

    // Node value
    const char* node_value() const {
        DOMNode* p = get_ptr();
        return p ? p->node_value : "";
    }

    void set_node_value(const char* val) {
        DOMNode* p = get_ptr();
        if (p && val) {
            strncpy(p->node_value, val, sizeof(p->node_value) - 1);
            p->node_value[sizeof(p->node_value) - 1] = '\0';
        }
    }

    // Tree navigation getters/setters
    GCValue parent_node() const {
        DOMNode* p = get_ptr();
        return p ? p->parent_node : JS_NULL;
    }

    void set_parent_node(GCValue val) {
        DOMNode* p = get_ptr();
        if (p) p->parent_node = val;
    }

    GCValue first_child() const {
        DOMNode* p = get_ptr();
        return p ? p->first_child : JS_NULL;
    }

    void set_first_child(GCValue val) {
        DOMNode* p = get_ptr();
        if (p) p->first_child = val;
    }

    GCValue last_child() const {
        DOMNode* p = get_ptr();
        return p ? p->last_child : JS_NULL;
    }

    void set_last_child(GCValue val) {
        DOMNode* p = get_ptr();
        if (p) p->last_child = val;
    }

    GCValue previous_sibling() const {
        DOMNode* p = get_ptr();
        return p ? p->previous_sibling : JS_NULL;
    }

    void set_previous_sibling(GCValue val) {
        DOMNode* p = get_ptr();
        if (p) p->previous_sibling = val;
    }

    GCValue next_sibling() const {
        DOMNode* p = get_ptr();
        return p ? p->next_sibling : JS_NULL;
    }

    void set_next_sibling(GCValue val) {
        DOMNode* p = get_ptr();
        if (p) p->next_sibling = val;
    }

    GCValue owner_document() const {
        DOMNode* p = get_ptr();
        return p ? p->owner_document : JS_NULL;
    }

    void set_owner_document(GCValue val) {
        DOMNode* p = get_ptr();
        if (p) p->owner_document = val;
    }

    GCValue shadow_root() const {
        DOMNode* p = get_ptr();
        return p ? p->shadow_root : JS_NULL;
    }

    void set_shadow_root(GCValue val) {
        DOMNode* p = get_ptr();
        if (p) p->shadow_root = val;
    }

    GCHandle computed_style_handle() const {
        DOMNode* p = get_ptr();
        return p ? p->computed_style_handle : GC_HANDLE_NULL;
    }

    void set_computed_style_handle(GCHandle val) {
        DOMNode* p = get_ptr();
        if (p) p->computed_style_handle = val;
    }

    GCHandle next_class_sibling() const {
        DOMNode* p = get_ptr();
        return p ? p->next_class_sibling : GC_HANDLE_NULL;
    }

    void set_next_class_sibling(GCHandle val) {
        DOMNode* p = get_ptr();
        if (p) p->next_class_sibling = val;
    }

    GCHandle next_tag_sibling() const {
        DOMNode* p = get_ptr();
        return p ? p->next_tag_sibling : GC_HANDLE_NULL;
    }

    void set_next_tag_sibling(GCHandle val) {
        DOMNode* p = get_ptr();
        if (p) p->next_tag_sibling = val;
    }

    // ID
    const char* id() const {
        DOMNode* p = get_ptr();
        return p ? p->id : "";
    }

    void set_id(const char* val) {
        DOMNode* p = get_ptr();
        if (p && val) {
            strncpy(p->id, val, sizeof(p->id) - 1);
            p->id[sizeof(p->id) - 1] = '\0';
        }
    }

    // Class name
    const char* class_name() const {
        DOMNode* p = get_ptr();
        return p ? p->class_name : "";
    }

    void set_class_name(const char* val) {
        DOMNode* p = get_ptr();
        if (p && val) {
            strncpy(p->class_name, val, sizeof(p->class_name) - 1);
            p->class_name[sizeof(p->class_name) - 1] = '\0';
        }
    }

    // Free the heap-allocated attribute array. Called from the DOMNode finalizer
    // before the DOMNode data object itself is reclaimed.
    void free_attributes() {
        DOMNode* p = get_ptr();
        if (p && p->attributes) {
            free(p->attributes);
            p->attributes = nullptr;
            p->attribute_count = 0;
            p->attribute_capacity = 0;
        }
    }

    // Attributes
    int attribute_count() const {
        DOMNode* p = get_ptr();
        return p ? p->attribute_count : 0;
    }

    const DOMAttribute* attributes() const {
        DOMNode* p = get_ptr();
        return p ? p->attributes : nullptr;
    }

    DOMAttribute* attributes() {
        DOMNode* p = get_ptr();
        return p ? p->attributes : nullptr;
    }

    const char* get_attribute(const char* name) const {
        DOMNode* p = get_ptr();
        if (!p || !name) return nullptr;
        for (int i = 0; i < p->attribute_count; i++) {
            if (strcmp(p->attributes[i].name, name) == 0) {
                return p->attributes[i].value;
            }
        }
        return nullptr;
    }

    void set_attribute(const char* name, const char* value) {
        DOMNode* p = get_ptr();
        if (!p || !name || !value) return;
        
        // Check if attribute already exists
        for (int i = 0; i < p->attribute_count; i++) {
            if (strcmp(p->attributes[i].name, name) == 0) {
                strncpy(p->attributes[i].value, value, sizeof(p->attributes[i].value) - 1);
                p->attributes[i].value[sizeof(p->attributes[i].value) - 1] = '\0';
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
    }

    bool has_attribute(const char* name) const {
        return get_attribute(name) != nullptr;
    }

    void remove_attribute(const char* name) {
        DOMNode* p = get_ptr();
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

    // Clear all tree references (used when removing from tree)
    void clear_references() {
        DOMNode* p = get_ptr();
        if (!p) return;
        
        p->parent_node = JS_NULL;
        p->first_child = JS_NULL;
        p->last_child = JS_NULL;
        p->previous_sibling = JS_NULL;
        p->next_sibling = JS_NULL;
    }

    // Context
    JSContextHandle context() const {
        DOMNode* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * EventHandle
 * ============================================================================ */
class EventHandle : public GCHandleBase<EventHandle, EventData, js_event_class_id> {
public:
    using Base = GCHandleBase<EventHandle, EventData, js_event_class_id>;

    EventHandle() : Base() {}
    explicit EventHandle(GCHandle handle) : Base(handle) {}

    static EventHandle create(JSContextHandle ctx, const char* type) {
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
            ev->ctx = ctx;
        }
        return EventHandle(h);
    }

    const char* type() const {
        EventData* p = get_ptr();
        return p ? p->type : "";
    }

    void set_type(const char* val) {
        EventData* p = get_ptr();
        if (p && val) {
            strncpy(p->type, val, sizeof(p->type) - 1);
            p->type[sizeof(p->type) - 1] = '\0';
        }
    }

    int bubbles() const {
        EventData* p = get_ptr();
        return p ? p->bubbles : 0;
    }

    void set_bubbles(int val) {
        EventData* p = get_ptr();
        if (p) p->bubbles = val;
    }

    int cancelable() const {
        EventData* p = get_ptr();
        return p ? p->cancelable : 0;
    }

    void set_cancelable(int val) {
        EventData* p = get_ptr();
        if (p) p->cancelable = val;
    }

    int composed() const {
        EventData* p = get_ptr();
        return p ? p->composed : 0;
    }

    void set_composed(int val) {
        EventData* p = get_ptr();
        if (p) p->composed = val;
    }

    int eventPhase() const {
        EventData* p = get_ptr();
        return p ? p->eventPhase : 0;
    }

    void set_eventPhase(int val) {
        EventData* p = get_ptr();
        if (p) p->eventPhase = val;
    }

    int defaultPrevented() const {
        EventData* p = get_ptr();
        return p ? p->defaultPrevented : 0;
    }

    void set_defaultPrevented(int val) {
        EventData* p = get_ptr();
        if (p) p->defaultPrevented = val;
    }

    double timeStamp() const {
        EventData* p = get_ptr();
        return p ? p->timeStamp : 0;
    }

    void set_timeStamp(double val) {
        EventData* p = get_ptr();
        if (p) p->timeStamp = val;
    }

    GCValue target() const {
        EventData* p = get_ptr();
        return p ? p->target : JS_NULL;
    }

    void set_target(GCValue val) {
        EventData* p = get_ptr();
        if (p) p->target = val;
    }

    GCValue currentTarget() const {
        EventData* p = get_ptr();
        return p ? p->currentTarget : JS_NULL;
    }

    void set_currentTarget(GCValue val) {
        EventData* p = get_ptr();
        if (p) p->currentTarget = val;
    }
};

/* ============================================================================
 * CustomEventHandle
 * ============================================================================ */
class CustomEventHandle : public GCHandleBase<CustomEventHandle, CustomEventData, js_custom_event_class_id> {
public:
    using Base = GCHandleBase<CustomEventHandle, CustomEventData, js_custom_event_class_id>;

    CustomEventHandle() : Base() {}
    explicit CustomEventHandle(GCHandle handle) : Base(handle) {}

    static CustomEventHandle create(JSContextHandle ctx, const char* type) {
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
            ev->base.ctx = ctx;
            ev->detail = JS_NULL;
        }
        return CustomEventHandle(h);
    }

    GCValue detail() const {
        CustomEventData* p = get_ptr();
        return p ? p->detail : JS_NULL;
    }

    void set_detail(GCValue val) {
        CustomEventData* p = get_ptr();
        if (p) p->detail = val;
    }

    // Base Event accessors
    void set_bubbles(int val) {
        CustomEventData* p = get_ptr();
        if (p) p->base.bubbles = val;
    }

    void set_cancelable(int val) {
        CustomEventData* p = get_ptr();
        if (p) p->base.cancelable = val;
    }

    void set_composed(int val) {
        CustomEventData* p = get_ptr();
        if (p) p->base.composed = val;
    }
};

/* ============================================================================
 * MouseEventHandle
 * ============================================================================ */
class MouseEventHandle : public GCHandleBase<MouseEventHandle, MouseEventData, js_mouse_event_class_id> {
public:
    using Base = GCHandleBase<MouseEventHandle, MouseEventData, js_mouse_event_class_id>;

    MouseEventHandle() : Base() {}
    explicit MouseEventHandle(GCHandle handle) : Base(handle) {}

    static MouseEventHandle create(JSContextHandle ctx, const char* type) {
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
            ev->base.ctx = ctx;
        }
        return MouseEventHandle(h);
    }

    double clientX() const {
        MouseEventData* p = get_ptr();
        return p ? p->clientX : 0;
    }

    void set_clientX(double val) {
        MouseEventData* p = get_ptr();
        if (p) p->clientX = val;
    }

    double clientY() const {
        MouseEventData* p = get_ptr();
        return p ? p->clientY : 0;
    }

    void set_clientY(double val) {
        MouseEventData* p = get_ptr();
        if (p) p->clientY = val;
    }

    // Base Event accessors
    void set_bubbles(int val) {
        MouseEventData* p = get_ptr();
        if (p) p->base.bubbles = val;
    }

    void set_cancelable(int val) {
        MouseEventData* p = get_ptr();
        if (p) p->base.cancelable = val;
    }
};

/* ============================================================================
 * FocusEventHandle
 * ============================================================================ */
class FocusEventHandle : public GCHandleBase<FocusEventHandle, FocusEventData, js_focus_event_class_id> {
public:
    using Base = GCHandleBase<FocusEventHandle, FocusEventData, js_focus_event_class_id>;

    FocusEventHandle() : Base() {}
    explicit FocusEventHandle(GCHandle handle) : Base(handle) {}

    static FocusEventHandle create(JSContextHandle ctx, const char* type) {
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
            ev->base.ctx = ctx;
            ev->relatedTarget = JS_NULL;
        }
        return FocusEventHandle(h);
    }

    GCValue relatedTarget() const {
        FocusEventData* p = get_ptr();
        return p ? p->relatedTarget : JS_NULL;
    }

    void set_relatedTarget(GCValue val) {
        FocusEventData* p = get_ptr();
        if (p) p->relatedTarget = val;
    }

    // Base Event accessors
    void set_bubbles(int val) {
        FocusEventData* p = get_ptr();
        if (p) p->base.bubbles = val;
    }

    void set_cancelable(int val) {
        FocusEventData* p = get_ptr();
        if (p) p->base.cancelable = val;
    }
};

/* ============================================================================
 * DateDataHandle
 * ============================================================================ */
class DateDataHandle : public GCHandleBase<DateDataHandle, DateData, js_date_class_id> {
public:
    using Base = GCHandleBase<DateDataHandle, DateData, js_date_class_id>;

    DateDataHandle() : Base() {}
    explicit DateDataHandle(GCHandle handle) : Base(handle) {}

    static DateDataHandle create() {
        GCHandle h = gc_alloc(sizeof(DateData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            DateData* d = (DateData*)gc_deref(h);
            memset(d, 0, sizeof(DateData));
            d->timestamp_ms = 0;
            d->is_valid = 1;
        }
        return DateDataHandle(h);
    }

    long long timestamp_ms() const {
        DateData* p = get_ptr();
        return p ? p->timestamp_ms : 0;
    }

    void set_timestamp_ms(long long val) {
        DateData* p = get_ptr();
        if (p) p->timestamp_ms = val;
    }

    int is_valid() const {
        DateData* p = get_ptr();
        return p ? p->is_valid : 0;
    }

    void set_valid(int val) {
        DateData* p = get_ptr();
        if (p) p->is_valid = val;
    }
};

#endif /* BROWSER_API_IMPL_HANDLES_H */
