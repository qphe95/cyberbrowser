/* Auto-generated split from browser_api_impl.cpp */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handles.h"
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"

// ============================================================================
// MediaSource API Implementation
// ============================================================================

void js_media_source_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

void js_source_buffer_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    (void)val;
}

JSClassDef js_media_source_class_def = {
    .class_name = "MediaSource",
    .finalizer = js_media_source_finalizer,
};

JSClassDef js_source_buffer_class_def = {
    .class_name = "SourceBuffer",
    .finalizer = js_source_buffer_finalizer,
};

// MediaSource constructor
GCValue js_media_source_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::create(ctx);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    
    ms.set_ready_state(0); // closed
    
    GCValue obj = JS_NewObjectClass(ctx, js_media_source_class_id);
    ms.attach_to_object(obj);
    return obj;
}

// SourceBuffer constructor (internal use)
GCValue js_source_buffer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::create(ctx);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    
    if (argc > 0) {
        const char *mime_type = JS_ToCString(ctx, argv[0]);
        if (mime_type) {
            sb.set_mime_type(mime_type);
            // Capture blob URLs for media source
            capture_url_debug(mime_type, "source_buffer_ctor");
        }
    }
    
    GCValue obj = JS_NewObjectClass(ctx, js_source_buffer_class_id);
    sb.attach_to_object(obj);
    return obj;
}

// MediaSource.isTypeSupported(static)
GCValue js_media_source_is_type_supported(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    
    const char *mime_type = JS_ToCString(ctx, argv[0]);
    if (!mime_type) return JS_FALSE;
    
    // Support common media types that YouTube uses
    bool supported = (
        strstr(mime_type, "video/mp4") != NULL ||
        strstr(mime_type, "video/webm") != NULL ||
        strstr(mime_type, "audio/mp4") != NULL ||
        strstr(mime_type, "audio/webm") != NULL ||
        strstr(mime_type, "video/x-matroska") != NULL
    );
    
    return JS_NewBool(ctx, supported);
}

// MediaSource.prototype.addSourceBuffer
GCValue js_media_source_add_source_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    const char *mime_type = JS_ToCString(ctx, argv[0]);
    if (!mime_type) return JS_ThrowTypeError(ctx, "Invalid MIME type");
    
    // Create a mock blob URL for this source buffer
    char blob_url[512];
    static int blob_counter = 0;
    snprintf(blob_url, sizeof(blob_url), "blob:media-source:%d?type=%s", ++blob_counter, mime_type);
    
    // Capture the blob URL
    capture_url_debug(blob_url, "media_source_add_source_buffer");
    
    // Create SourceBuffer object
    GCValue sb_args[1] = { JS_NewString(ctx, mime_type) };
    GCValue sb = js_source_buffer_constructor(ctx, JS_UNDEFINED, 1, sb_args);
    
    return sb;
}

// MediaSource.prototype.removeSourceBuffer
GCValue js_media_source_remove_source_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// MediaSource.prototype.endOfStream
GCValue js_media_source_end_of_stream(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    
    ms.set_ready_state(2); // ended
    return JS_UNDEFINED;
}

// MediaSource.readyState getter
GCValue js_media_source_get_ready_state(JSContextHandle ctx, GCValue this_val) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    
    static const char* states[] = {"closed", "open", "ended"};
    int state = ms.ready_state();
    if (state >= 0 && state < 3) {
        return JS_NewString(ctx, states[state]);
    }
    return JS_NewString(ctx, "closed");
}

// MediaSource.duration getter/setter
GCValue js_media_source_get_duration(JSContextHandle ctx, GCValue this_val) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    return JS_NewFloat64(ctx, ms.duration());
}

GCValue js_media_source_set_duration(JSContextHandle ctx, GCValue this_val, GCValue val) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object_check(ctx, this_val);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    double duration;
    JS_ToFloat64(ctx, &duration, val);
    ms.set_duration(duration);
    return JS_UNDEFINED;
}

// MediaSource.sourceBuffers getter
GCValue js_media_source_get_source_buffers(JSContextHandle ctx, GCValue this_val) {
    return JS_NewArray(ctx);
}

// MediaSource.activeSourceBuffers getter
GCValue js_media_source_get_active_source_buffers(JSContextHandle ctx, GCValue this_val) {
    return JS_NewArray(ctx);
}

const JSCFunctionListEntry js_media_source_proto_funcs[] = {
    JS_CFUNC_DEF("addSourceBuffer", 1, js_media_source_add_source_buffer),
    JS_CFUNC_DEF("removeSourceBuffer", 1, js_media_source_remove_source_buffer),
    JS_CFUNC_DEF("endOfStream", 0, js_media_source_end_of_stream),
    JS_CGETSET_DEF("readyState", js_media_source_get_ready_state, NULL),
    JS_CGETSET_DEF("duration", js_media_source_get_duration, js_media_source_set_duration),
    JS_CGETSET_DEF("sourceBuffers", js_media_source_get_source_buffers, NULL),
    JS_CGETSET_DEF("activeSourceBuffers", js_media_source_get_active_source_buffers, NULL),
};
const size_t js_media_source_proto_funcs_count = sizeof(js_media_source_proto_funcs) / sizeof(js_media_source_proto_funcs[0]);

// SourceBuffer.prototype.appendBuffer
GCValue js_source_buffer_append_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    
    sb.set_updating(1);
    
    // Simulate async update completion
    sb.set_updating(0);
    
    return JS_UNDEFINED;
}

// SourceBuffer.prototype.abort
GCValue js_source_buffer_abort(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    
    sb.set_updating(0);
    return JS_UNDEFINED;
}

// SourceBuffer.prototype.changeType
GCValue js_source_buffer_change_type(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    
    const char *mime_type = JS_ToCString(ctx, argv[0]);
    if (mime_type) {
        sb.set_mime_type(mime_type);
    }
    
    return JS_UNDEFINED;
}

// SourceBuffer.prototype.remove
GCValue js_source_buffer_remove(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return JS_UNDEFINED;
}

// SourceBuffer.updating getter
GCValue js_source_buffer_get_updating(JSContextHandle ctx, GCValue this_val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    return JS_NewBool(ctx, sb.updating());
}

// SourceBuffer.timestampOffset getter/setter
GCValue js_source_buffer_get_timestamp_offset(JSContextHandle ctx, GCValue this_val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    return JS_NewFloat64(ctx, sb.timestamp_offset());
}

GCValue js_source_buffer_set_timestamp_offset(JSContextHandle ctx, GCValue this_val, GCValue val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    double offset;
    JS_ToFloat64(ctx, &offset, val);
    sb.set_timestamp_offset(offset);
    return JS_UNDEFINED;
}

// SourceBuffer.mode getter/setter
GCValue js_source_buffer_get_mode(JSContextHandle ctx, GCValue this_val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    return JS_NewString(ctx, sb.mode());
}

GCValue js_source_buffer_set_mode(JSContextHandle ctx, GCValue this_val, GCValue val) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    const char *mode = JS_ToCString(ctx, val);
    if (mode) {
        sb.set_mode(mode);
    }
    return JS_UNDEFINED;
}

const JSCFunctionListEntry js_source_buffer_proto_funcs[] = {
    JS_CFUNC_DEF("appendBuffer", 1, js_source_buffer_append_buffer),
    JS_CFUNC_DEF("abort", 0, js_source_buffer_abort),
    JS_CFUNC_DEF("changeType", 1, js_source_buffer_change_type),
    JS_CFUNC_DEF("remove", 2, js_source_buffer_remove),
    JS_CGETSET_DEF("updating", js_source_buffer_get_updating, NULL),
    JS_CGETSET_DEF("timestampOffset", js_source_buffer_get_timestamp_offset, js_source_buffer_set_timestamp_offset),
    JS_CGETSET_DEF("mode", js_source_buffer_get_mode, js_source_buffer_set_mode),
    JS_PROP_DOUBLE_DEF("appendWindowStart", 0, JS_PROP_WRITABLE),
    JS_PROP_DOUBLE_DEF("appendWindowEnd", 0, JS_PROP_WRITABLE),
    JS_PROP_STRING_DEF("videoTracks", "", JS_PROP_WRITABLE),
    JS_PROP_STRING_DEF("audioTracks", "", JS_PROP_WRITABLE),
    JS_PROP_STRING_DEF("textTracks", "", JS_PROP_WRITABLE),
};
const size_t js_source_buffer_proto_funcs_count = sizeof(js_source_buffer_proto_funcs) / sizeof(js_source_buffer_proto_funcs[0]);

// ============================================================================
