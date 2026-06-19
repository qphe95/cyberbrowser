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
 * XMLHttpRequestHandle
 * ============================================================================ */
class XMLHttpRequestHandle {
private:
    GCHandle handle_;
    
    XMLHttpRequest* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (XMLHttpRequest*)gc_deref(handle_);
    }

public:
    /* Default constructor - null handle */
    XMLHttpRequestHandle() : handle_(GC_HANDLE_NULL) {}
    
    /* Construct from GCHandle */
    explicit XMLHttpRequestHandle(GCHandle handle) : handle_(handle) {}
    
    /* Copy constructor */
    XMLHttpRequestHandle(const XMLHttpRequestHandle& other) : handle_(other.handle_) {}
    
    /* Move constructor */
    XMLHttpRequestHandle(XMLHttpRequestHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    /* Assignment operators */
    XMLHttpRequestHandle& operator=(const XMLHttpRequestHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    XMLHttpRequestHandle& operator=(XMLHttpRequestHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    /* Get the underlying handle */
    GCHandle handle() const { return handle_; }
    
    /* Check if handle is valid */
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    
    /* Explicit bool conversion */
    explicit operator bool() const { return valid(); }
    
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
    
    /* Get from JS object using class ID */
    static XMLHttpRequestHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_xhr_class_id);
        return XMLHttpRequestHandle(h);
    }
    
    static XMLHttpRequestHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_xhr_class_id);
        return XMLHttpRequestHandle(h);
    }
    
    /* Store in JS object */
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
    
    JSContextHandle context() const {
        XMLHttpRequest* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * HTMLVideoElementHandle
 * ============================================================================ */
class HTMLVideoElementHandle {
private:
    GCHandle handle_;
    
    HTMLVideoElement* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (HTMLVideoElement*)gc_deref(handle_);
    }

public:
    HTMLVideoElementHandle() : handle_(GC_HANDLE_NULL) {}
    explicit HTMLVideoElementHandle(GCHandle handle) : handle_(handle) {}
    HTMLVideoElementHandle(const HTMLVideoElementHandle& other) : handle_(other.handle_) {}
    HTMLVideoElementHandle(HTMLVideoElementHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    HTMLVideoElementHandle& operator=(const HTMLVideoElementHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    HTMLVideoElementHandle& operator=(HTMLVideoElementHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static HTMLVideoElementHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(HTMLVideoElement), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            HTMLVideoElement* vid = (HTMLVideoElement*)gc_deref(h);
            memset(vid, 0, sizeof(HTMLVideoElement));
            vid->ctx = ctx;
            vid->paused = 1;
        }
        return HTMLVideoElementHandle(h);
    }
    
    static HTMLVideoElementHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_video_class_id);
        return HTMLVideoElementHandle(h);
    }
    
    static HTMLVideoElementHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_video_class_id);
        return HTMLVideoElementHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
    
    JSContextHandle context() const {
        HTMLVideoElement* p = get_ptr();
        return p ? p->ctx : JSContextHandle();
    }
};

/* ============================================================================
 * DOMExceptionDataHandle
 * ============================================================================ */
class DOMExceptionDataHandle {
private:
    GCHandle handle_;
    
    DOMExceptionData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (DOMExceptionData*)gc_deref(handle_);
    }

public:
    DOMExceptionDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit DOMExceptionDataHandle(GCHandle handle) : handle_(handle) {}
    DOMExceptionDataHandle(const DOMExceptionDataHandle& other) : handle_(other.handle_) {}
    DOMExceptionDataHandle(DOMExceptionDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    DOMExceptionDataHandle& operator=(const DOMExceptionDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    DOMExceptionDataHandle& operator=(DOMExceptionDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static DOMExceptionDataHandle create() {
        GCHandle h = gc_alloc(sizeof(DOMExceptionData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            DOMExceptionData* de = (DOMExceptionData*)gc_deref(h);
            memset(de, 0, sizeof(DOMExceptionData));
            strcpy(de->name, "Error");
        }
        return DOMExceptionDataHandle(h);
    }
    
    static DOMExceptionDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_dom_exception_class_id);
        return DOMExceptionDataHandle(h);
    }
    
    static DOMExceptionDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_dom_exception_class_id);
        return DOMExceptionDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class MapDataHandle {
private:
    GCHandle handle_;
    
    MapData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (MapData*)gc_deref(handle_);
    }

public:
    MapDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit MapDataHandle(GCHandle handle) : handle_(handle) {}
    MapDataHandle(const MapDataHandle& other) : handle_(other.handle_) {}
    MapDataHandle(MapDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    MapDataHandle& operator=(const MapDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    MapDataHandle& operator=(MapDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static MapDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_map_class_id);
        return MapDataHandle(h);
    }
    
    static MapDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_map_class_id);
        return MapDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class ShadowRootDataHandle {
private:
    GCHandle handle_;
    
    ShadowRootData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (ShadowRootData*)gc_deref(handle_);
    }

public:
    ShadowRootDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit ShadowRootDataHandle(GCHandle handle) : handle_(handle) {}
    ShadowRootDataHandle(const ShadowRootDataHandle& other) : handle_(other.handle_) {}
    ShadowRootDataHandle(ShadowRootDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    ShadowRootDataHandle& operator=(const ShadowRootDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    ShadowRootDataHandle& operator=(ShadowRootDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static ShadowRootDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_shadow_root_class_id);
        return ShadowRootDataHandle(h);
    }
    
    static ShadowRootDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_shadow_root_class_id);
        return ShadowRootDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class CustomElementRegistryDataHandle {
private:
    GCHandle handle_;
    
    CustomElementRegistryData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (CustomElementRegistryData*)gc_deref(handle_);
    }

public:
    CustomElementRegistryDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit CustomElementRegistryDataHandle(GCHandle handle) : handle_(handle) {}
    CustomElementRegistryDataHandle(const CustomElementRegistryDataHandle& other) : handle_(other.handle_) {}
    CustomElementRegistryDataHandle(CustomElementRegistryDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    CustomElementRegistryDataHandle& operator=(const CustomElementRegistryDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    CustomElementRegistryDataHandle& operator=(CustomElementRegistryDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static CustomElementRegistryDataHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(CustomElementRegistryData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            CustomElementRegistryData* cer = (CustomElementRegistryData*)gc_deref(h);
            memset(cer, 0, sizeof(CustomElementRegistryData));
            cer->registry = JS_NewObject(ctx);
        }
        return CustomElementRegistryDataHandle(h);
    }
    
    static CustomElementRegistryDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_custom_element_registry_class_id);
        return CustomElementRegistryDataHandle(h);
    }
    
    static CustomElementRegistryDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_custom_element_registry_class_id);
        return CustomElementRegistryDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class AnimationDataHandle {
private:
    GCHandle handle_;
    
    AnimationData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (AnimationData*)gc_deref(handle_);
    }

public:
    AnimationDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit AnimationDataHandle(GCHandle handle) : handle_(handle) {}
    AnimationDataHandle(const AnimationDataHandle& other) : handle_(other.handle_) {}
    AnimationDataHandle(AnimationDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    AnimationDataHandle& operator=(const AnimationDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    AnimationDataHandle& operator=(AnimationDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static AnimationDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_animation_class_id);
        return AnimationDataHandle(h);
    }
    
    static AnimationDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_animation_class_id);
        return AnimationDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class KeyFrameEffectDataHandle {
private:
    GCHandle handle_;
    
    KeyFrameEffectData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (KeyFrameEffectData*)gc_deref(handle_);
    }

public:
    KeyFrameEffectDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit KeyFrameEffectDataHandle(GCHandle handle) : handle_(handle) {}
    KeyFrameEffectDataHandle(const KeyFrameEffectDataHandle& other) : handle_(other.handle_) {}
    KeyFrameEffectDataHandle(KeyFrameEffectDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    KeyFrameEffectDataHandle& operator=(const KeyFrameEffectDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    KeyFrameEffectDataHandle& operator=(KeyFrameEffectDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static KeyFrameEffectDataHandle create() {
        GCHandle h = gc_alloc(sizeof(KeyFrameEffectData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            KeyFrameEffectData* effect = (KeyFrameEffectData*)gc_deref(h);
            memset(effect, 0, sizeof(KeyFrameEffectData));
            strcpy(effect->easing, "linear");
        }
        return KeyFrameEffectDataHandle(h);
    }
    
    static KeyFrameEffectDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_keyframe_effect_class_id);
        return KeyFrameEffectDataHandle(h);
    }
    
    static KeyFrameEffectDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_keyframe_effect_class_id);
        return KeyFrameEffectDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class FontFaceDataHandle {
private:
    GCHandle handle_;
    
    FontFaceData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (FontFaceData*)gc_deref(handle_);
    }

public:
    FontFaceDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit FontFaceDataHandle(GCHandle handle) : handle_(handle) {}
    FontFaceDataHandle(const FontFaceDataHandle& other) : handle_(other.handle_) {}
    FontFaceDataHandle(FontFaceDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    FontFaceDataHandle& operator=(const FontFaceDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    FontFaceDataHandle& operator=(FontFaceDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static FontFaceDataHandle create() {
        GCHandle h = gc_alloc(sizeof(FontFaceData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            FontFaceData* ff = (FontFaceData*)gc_deref(h);
            memset(ff, 0, sizeof(FontFaceData));
        }
        return FontFaceDataHandle(h);
    }
    
    static FontFaceDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_font_face_class_id);
        return FontFaceDataHandle(h);
    }
    
    static FontFaceDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_font_face_class_id);
        return FontFaceDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class FontFaceSetDataHandle {
private:
    GCHandle handle_;
    
    FontFaceSetData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (FontFaceSetData*)gc_deref(handle_);
    }

public:
    FontFaceSetDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit FontFaceSetDataHandle(GCHandle handle) : handle_(handle) {}
    FontFaceSetDataHandle(const FontFaceSetDataHandle& other) : handle_(other.handle_) {}
    FontFaceSetDataHandle(FontFaceSetDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    FontFaceSetDataHandle& operator=(const FontFaceSetDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    FontFaceSetDataHandle& operator=(FontFaceSetDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static FontFaceSetDataHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(FontFaceSetData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            FontFaceSetData* ffs = (FontFaceSetData*)gc_deref(h);
            memset(ffs, 0, sizeof(FontFaceSetData));
            ffs->loaded_fonts = JS_NewArray(ctx);
        }
        return FontFaceSetDataHandle(h);
    }
    
    static FontFaceSetDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_font_face_set_class_id);
        return FontFaceSetDataHandle(h);
    }
    
    static FontFaceSetDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_font_face_set_class_id);
        return FontFaceSetDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class MutationObserverDataHandle {
private:
    GCHandle handle_;
    
    MutationObserverData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (MutationObserverData*)gc_deref(handle_);
    }

public:
    MutationObserverDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit MutationObserverDataHandle(GCHandle handle) : handle_(handle) {}
    MutationObserverDataHandle(const MutationObserverDataHandle& other) : handle_(other.handle_) {}
    MutationObserverDataHandle(MutationObserverDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    MutationObserverDataHandle& operator=(const MutationObserverDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    MutationObserverDataHandle& operator=(MutationObserverDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static MutationObserverDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_mutation_observer_class_id);
        return MutationObserverDataHandle(h);
    }
    
    static MutationObserverDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_mutation_observer_class_id);
        return MutationObserverDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class ResizeObserverDataHandle {
private:
    GCHandle handle_;
    
    ResizeObserverData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (ResizeObserverData*)gc_deref(handle_);
    }

public:
    ResizeObserverDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit ResizeObserverDataHandle(GCHandle handle) : handle_(handle) {}
    ResizeObserverDataHandle(const ResizeObserverDataHandle& other) : handle_(other.handle_) {}
    ResizeObserverDataHandle(ResizeObserverDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    ResizeObserverDataHandle& operator=(const ResizeObserverDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    ResizeObserverDataHandle& operator=(ResizeObserverDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static ResizeObserverDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_resize_observer_class_id);
        return ResizeObserverDataHandle(h);
    }
    
    static ResizeObserverDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_resize_observer_class_id);
        return ResizeObserverDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class IntersectionObserverDataHandle {
private:
    GCHandle handle_;
    
    IntersectionObserverData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (IntersectionObserverData*)gc_deref(handle_);
    }

public:
    IntersectionObserverDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit IntersectionObserverDataHandle(GCHandle handle) : handle_(handle) {}
    IntersectionObserverDataHandle(const IntersectionObserverDataHandle& other) : handle_(other.handle_) {}
    IntersectionObserverDataHandle(IntersectionObserverDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    IntersectionObserverDataHandle& operator=(const IntersectionObserverDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    IntersectionObserverDataHandle& operator=(IntersectionObserverDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static IntersectionObserverDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_intersection_observer_class_id);
        return IntersectionObserverDataHandle(h);
    }
    
    static IntersectionObserverDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_intersection_observer_class_id);
        return IntersectionObserverDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class PerformanceDataHandle {
private:
    GCHandle handle_;
    
    PerformanceData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (PerformanceData*)gc_deref(handle_);
    }

public:
    PerformanceDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit PerformanceDataHandle(GCHandle handle) : handle_(handle) {}
    PerformanceDataHandle(const PerformanceDataHandle& other) : handle_(other.handle_) {}
    PerformanceDataHandle(PerformanceDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    PerformanceDataHandle& operator=(const PerformanceDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    PerformanceDataHandle& operator=(PerformanceDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static PerformanceDataHandle create() {
        GCHandle h = gc_alloc(sizeof(PerformanceData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            PerformanceData* perf = (PerformanceData*)gc_deref(h);
            memset(perf, 0, sizeof(PerformanceData));
        }
        return PerformanceDataHandle(h);
    }
    
    static PerformanceDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_performance_class_id);
        return PerformanceDataHandle(h);
    }
    
    static PerformanceDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_performance_class_id);
        return PerformanceDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class PerformanceEntryDataHandle {
private:
    GCHandle handle_;
    
    PerformanceEntryData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (PerformanceEntryData*)gc_deref(handle_);
    }

public:
    PerformanceEntryDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit PerformanceEntryDataHandle(GCHandle handle) : handle_(handle) {}
    PerformanceEntryDataHandle(const PerformanceEntryDataHandle& other) : handle_(other.handle_) {}
    PerformanceEntryDataHandle(PerformanceEntryDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    PerformanceEntryDataHandle& operator=(const PerformanceEntryDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    PerformanceEntryDataHandle& operator=(PerformanceEntryDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static PerformanceEntryDataHandle create() {
        GCHandle h = gc_alloc(sizeof(PerformanceEntryData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            PerformanceEntryData* entry = (PerformanceEntryData*)gc_deref(h);
            memset(entry, 0, sizeof(PerformanceEntryData));
        }
        return PerformanceEntryDataHandle(h);
    }
    
    static PerformanceEntryDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_performance_entry_class_id);
        return PerformanceEntryDataHandle(h);
    }
    
    static PerformanceEntryDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_performance_entry_class_id);
        return PerformanceEntryDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class PerformanceObserverDataHandle {
private:
    GCHandle handle_;
    
    PerformanceObserverData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (PerformanceObserverData*)gc_deref(handle_);
    }

public:
    PerformanceObserverDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit PerformanceObserverDataHandle(GCHandle handle) : handle_(handle) {}
    PerformanceObserverDataHandle(const PerformanceObserverDataHandle& other) : handle_(other.handle_) {}
    PerformanceObserverDataHandle(PerformanceObserverDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    PerformanceObserverDataHandle& operator=(const PerformanceObserverDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    PerformanceObserverDataHandle& operator=(PerformanceObserverDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static PerformanceObserverDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_performance_observer_class_id);
        return PerformanceObserverDataHandle(h);
    }
    
    static PerformanceObserverDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_performance_observer_class_id);
        return PerformanceObserverDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class PerformanceTimingDataHandle {
private:
    GCHandle handle_;
    
    PerformanceTimingData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (PerformanceTimingData*)gc_deref(handle_);
    }

public:
    PerformanceTimingDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit PerformanceTimingDataHandle(GCHandle handle) : handle_(handle) {}
    PerformanceTimingDataHandle(const PerformanceTimingDataHandle& other) : handle_(other.handle_) {}
    PerformanceTimingDataHandle(PerformanceTimingDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    PerformanceTimingDataHandle& operator=(const PerformanceTimingDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    PerformanceTimingDataHandle& operator=(PerformanceTimingDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static PerformanceTimingDataHandle create() {
        GCHandle h = gc_alloc(sizeof(PerformanceTimingData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            PerformanceTimingData* pt = (PerformanceTimingData*)gc_deref(h);
            memset(pt, 0, sizeof(PerformanceTimingData));
        }
        return PerformanceTimingDataHandle(h);
    }
    
    static PerformanceTimingDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_performance_timing_class_id);
        return PerformanceTimingDataHandle(h);
    }
    
    static PerformanceTimingDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_performance_timing_class_id);
        return PerformanceTimingDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
 * DOMRectDataHandle (used for both DOMRect and DOMRectReadOnly)
 * ============================================================================ */
class DOMRectDataHandle {
private:
    GCHandle handle_;
    
    DOMRectData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (DOMRectData*)gc_deref(handle_);
    }

public:
    DOMRectDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit DOMRectDataHandle(GCHandle handle) : handle_(handle) {}
    DOMRectDataHandle(const DOMRectDataHandle& other) : handle_(other.handle_) {}
    DOMRectDataHandle(DOMRectDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    DOMRectDataHandle& operator=(const DOMRectDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    DOMRectDataHandle& operator=(DOMRectDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class MediaSourceDataHandle {
private:
    GCHandle handle_;
    
    MediaSourceData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (MediaSourceData*)gc_deref(handle_);
    }

public:
    MediaSourceDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit MediaSourceDataHandle(GCHandle handle) : handle_(handle) {}
    MediaSourceDataHandle(const MediaSourceDataHandle& other) : handle_(other.handle_) {}
    MediaSourceDataHandle(MediaSourceDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    MediaSourceDataHandle& operator=(const MediaSourceDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    MediaSourceDataHandle& operator=(MediaSourceDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static MediaSourceDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_media_source_class_id);
        return MediaSourceDataHandle(h);
    }
    
    static MediaSourceDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_media_source_class_id);
        return MediaSourceDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class SourceBufferDataHandle {
private:
    GCHandle handle_;
    
    SourceBufferData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (SourceBufferData*)gc_deref(handle_);
    }

public:
    SourceBufferDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit SourceBufferDataHandle(GCHandle handle) : handle_(handle) {}
    SourceBufferDataHandle(const SourceBufferDataHandle& other) : handle_(other.handle_) {}
    SourceBufferDataHandle(SourceBufferDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    SourceBufferDataHandle& operator=(const SourceBufferDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    SourceBufferDataHandle& operator=(SourceBufferDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
    static SourceBufferDataHandle create(JSContextHandle ctx) {
        GCHandle h = gc_alloc(sizeof(SourceBufferData), JS_GC_OBJ_TYPE_DATA);
        if (h != GC_HANDLE_NULL) {
            SourceBufferData* sb = (SourceBufferData*)gc_deref(h);
            memset(sb, 0, sizeof(SourceBufferData));
            sb->ctx = ctx;
            sb->updating = 0;
            strcpy(sb->mode, "segments");
        }
        return SourceBufferDataHandle(h);
    }
    
    static SourceBufferDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_source_buffer_class_id);
        return SourceBufferDataHandle(h);
    }
    
    static SourceBufferDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_source_buffer_class_id);
        return SourceBufferDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
};

/* ============================================================================
 * DOMNodeHandle - Real DOM node tree implementation
 * ============================================================================ */

extern JSClassID js_dom_node_class_id;

class DOMNodeHandle {
private:
    GCHandle handle_;
    
    DOMNode* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (DOMNode*)gc_deref(handle_);
    }

public:
    DOMNodeHandle() : handle_(GC_HANDLE_NULL) {}
    explicit DOMNodeHandle(GCHandle handle) : handle_(handle) {}
    DOMNodeHandle(const DOMNodeHandle& other) : handle_(other.handle_) {}
    DOMNodeHandle(DOMNodeHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    DOMNodeHandle& operator=(const DOMNodeHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    DOMNodeHandle& operator=(DOMNodeHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    // Attributes
    int attribute_count() const {
        DOMNode* p = get_ptr();
        return p ? p->attribute_count : 0;
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
        
        // Add new attribute
        if (p->attribute_count < DOM_MAX_ATTRIBUTES) {
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
class EventHandle {
private:
    GCHandle handle_;
    
    EventData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (EventData*)gc_deref(handle_);
    }

public:
    EventHandle() : handle_(GC_HANDLE_NULL) {}
    explicit EventHandle(GCHandle handle) : handle_(handle) {}
    EventHandle(const EventHandle& other) : handle_(other.handle_) {}
    EventHandle(EventHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    EventHandle& operator=(const EventHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    EventHandle& operator=(EventHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static EventHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_event_class_id);
        return EventHandle(h);
    }
    
    static EventHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_event_class_id);
        return EventHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class CustomEventHandle {
private:
    GCHandle handle_;
    
    CustomEventData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (CustomEventData*)gc_deref(handle_);
    }

public:
    CustomEventHandle() : handle_(GC_HANDLE_NULL) {}
    explicit CustomEventHandle(GCHandle handle) : handle_(handle) {}
    CustomEventHandle(const CustomEventHandle& other) : handle_(other.handle_) {}
    CustomEventHandle(CustomEventHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    CustomEventHandle& operator=(const CustomEventHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    CustomEventHandle& operator=(CustomEventHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static CustomEventHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_custom_event_class_id);
        return CustomEventHandle(h);
    }
    
    static CustomEventHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_custom_event_class_id);
        return CustomEventHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class MouseEventHandle {
private:
    GCHandle handle_;
    
    MouseEventData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (MouseEventData*)gc_deref(handle_);
    }

public:
    MouseEventHandle() : handle_(GC_HANDLE_NULL) {}
    explicit MouseEventHandle(GCHandle handle) : handle_(handle) {}
    MouseEventHandle(const MouseEventHandle& other) : handle_(other.handle_) {}
    MouseEventHandle(MouseEventHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    MouseEventHandle& operator=(const MouseEventHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    MouseEventHandle& operator=(MouseEventHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static MouseEventHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_mouse_event_class_id);
        return MouseEventHandle(h);
    }
    
    static MouseEventHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_mouse_event_class_id);
        return MouseEventHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class FocusEventHandle {
private:
    GCHandle handle_;
    
    FocusEventData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (FocusEventData*)gc_deref(handle_);
    }

public:
    FocusEventHandle() : handle_(GC_HANDLE_NULL) {}
    explicit FocusEventHandle(GCHandle handle) : handle_(handle) {}
    FocusEventHandle(const FocusEventHandle& other) : handle_(other.handle_) {}
    FocusEventHandle(FocusEventHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    FocusEventHandle& operator=(const FocusEventHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    FocusEventHandle& operator=(FocusEventHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static FocusEventHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_focus_event_class_id);
        return FocusEventHandle(h);
    }
    
    static FocusEventHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_focus_event_class_id);
        return FocusEventHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
class DateDataHandle {
private:
    GCHandle handle_;
    
    DateData* get_ptr() const {
        if (handle_ == GC_HANDLE_NULL) return nullptr;
        return (DateData*)gc_deref(handle_);
    }

public:
    DateDataHandle() : handle_(GC_HANDLE_NULL) {}
    explicit DateDataHandle(GCHandle handle) : handle_(handle) {}
    DateDataHandle(const DateDataHandle& other) : handle_(other.handle_) {}
    DateDataHandle(DateDataHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = GC_HANDLE_NULL;
    }
    
    DateDataHandle& operator=(const DateDataHandle& other) {
        handle_ = other.handle_;
        return *this;
    }
    
    DateDataHandle& operator=(DateDataHandle&& other) noexcept {
        handle_ = other.handle_;
        other.handle_ = GC_HANDLE_NULL;
        return *this;
    }
    
    GCHandle handle() const { return handle_; }
    bool valid() const { return handle_ != GC_HANDLE_NULL; }
    explicit operator bool() const { return valid(); }
    
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
    
    static DateDataHandle from_object(GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle(obj, js_date_class_id);
        return DateDataHandle(h);
    }
    
    static DateDataHandle from_object_check(JSContextHandle ctx, GCValue obj) {
        GCHandle h = JS_GetOpaqueHandle2(ctx, obj, js_date_class_id);
        return DateDataHandle(h);
    }
    
    void attach_to_object(GCValue obj) const {
        JS_SetOpaqueHandle(obj, handle_);
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
