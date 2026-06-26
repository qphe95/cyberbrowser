/* Auto-generated split from browser_api_impl.cpp */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <minimp4.h>
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

void js_media_source_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle ms_handle = JS_GetOpaqueHandle(val, js_media_source_class_id);
    if (ms_handle == GC_HANDLE_NULL) return;
    mark_func(rt, ms_handle);
    MediaSourceData *ms = (MediaSourceData *)gc_deref(ms_handle);
    if (!ms) return;
    JS_MarkValue(rt, ms->onsourceopen, mark_func);
    JS_MarkValue(rt, ms->onsourceended, mark_func);
    JS_MarkValue(rt, ms->onsourceclose, mark_func);
}

void js_source_buffer_finalizer(JSRuntimeHandle rt, GCValue val) {
    (void)rt;
    GCHandle sb_handle = JS_GetOpaqueHandle(val, js_source_buffer_class_id);
    if (sb_handle != GC_HANDLE_NULL) {
        SourceBufferData *sb = (SourceBufferData *)gc_deref(sb_handle);
        if (sb && sb->append_data) {
            free(sb->append_data);
            sb->append_data = NULL;
            sb->append_size = 0;
            sb->append_capacity = 0;
        }
    }
}

void js_source_buffer_mark(JSRuntimeHandle rt, GCValue val, JS_MarkFunc *mark_func) {
    GCHandle sb_handle = JS_GetOpaqueHandle(val, js_source_buffer_class_id);
    if (sb_handle == GC_HANDLE_NULL) return;
    mark_func(rt, sb_handle);
    SourceBufferData *sb = (SourceBufferData *)gc_deref(sb_handle);
    if (!sb) return;
    JS_MarkValue(rt, sb->onupdatestart, mark_func);
    JS_MarkValue(rt, sb->onupdate, mark_func);
    JS_MarkValue(rt, sb->onupdateend, mark_func);
    JS_MarkValue(rt, sb->onerror, mark_func);
    JS_MarkValue(rt, sb->onabort, mark_func);
}

JSClassDef js_media_source_class_def = {
    .class_name = "MediaSource",
    .finalizer = js_media_source_finalizer,
    .gc_mark   = js_media_source_mark,
};

JSClassDef js_source_buffer_class_def = {
    .class_name = "SourceBuffer",
    .finalizer = js_source_buffer_finalizer,
    .gc_mark   = js_source_buffer_mark,
};

// MediaSource constructor
GCValue js_media_source_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    MediaSourceDataHandle ms = MediaSourceDataHandle::create(ctx);
    if (!ms.valid()) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    
    ms.set_ready_state(1); // open
    
    GCValue obj = JS_NewObjectClass(ctx, js_media_source_class_id);
    ms.attach_to_object(obj);
    
    // Register a stable blob URL for this MediaSource
    GCValue url_args[1] = { obj };
    GCValue blob_url = js_url_create_object_url(ctx, JS_UNDEFINED, 1, url_args);
    const char *url_str = JS_ToCString(ctx, blob_url);
    if (url_str) {
        JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, url_str));
        JS_SetPropertyStr(ctx, obj, "__blobUrl", JS_NewString(ctx, url_str));
    }
    
    // Initialize source buffer lists
    JS_SetPropertyStr(ctx, obj, "__sourceBuffers", JS_NewArray(ctx));
    JS_SetPropertyStr(ctx, obj, "__activeSourceBuffers", JS_NewArray(ctx));
    
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
    
    // Create SourceBuffer object
    GCValue sb_args[1] = { JS_NewString(ctx, mime_type) };
    GCValue sb = js_source_buffer_constructor(ctx, JS_UNDEFINED, 1, sb_args);
    
    JS_SetPropertyStr(ctx, sb, "mimeType", JS_NewString(ctx, mime_type));
    JS_SetPropertyStr(ctx, sb, "updating", JS_NewBool(ctx, false));
    JS_SetPropertyStr(ctx, sb, "mode", JS_NewString(ctx, "segments"));
    JS_SetPropertyStr(ctx, sb, "timestampOffset", JS_NewInt32(ctx, 0));
    
    // Append to sourceBuffers / activeSourceBuffers
    const char *lists[] = { "__sourceBuffers", "__activeSourceBuffers" };
    for (int i = 0; i < 2; i++) {
        GCValue arr = JS_GetPropertyStr(ctx, this_val, lists[i]);
        if (JS_IsUndefined(arr) || JS_IsNull(arr) || !JS_IsObject(arr)) {
            arr = JS_NewArray(ctx);
            JS_SetPropertyStr(ctx, this_val, lists[i], arr);
        }
        int32_t len = 0;
        GCValue len_val = JS_GetPropertyStr(ctx, arr, "length");
        JS_ToInt32(ctx, &len, len_val);
        JS_SetPropertyUint32(ctx, arr, len, sb);
    }
    
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
    GCValue arr = JS_GetPropertyStr(ctx, this_val, "__sourceBuffers");
    if (JS_IsUndefined(arr) || JS_IsNull(arr) || !JS_IsObject(arr)) {
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__sourceBuffers", arr);
    }
    return arr;
}

// MediaSource.activeSourceBuffers getter
GCValue js_media_source_get_active_source_buffers(JSContextHandle ctx, GCValue this_val) {
    GCValue arr = JS_GetPropertyStr(ctx, this_val, "__activeSourceBuffers");
    if (JS_IsUndefined(arr) || JS_IsNull(arr) || !JS_IsObject(arr)) {
        arr = JS_NewArray(ctx);
        JS_SetPropertyStr(ctx, this_val, "__activeSourceBuffers", arr);
    }
    return arr;
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

// Helper: accumulate appendBuffer bytes into the SourceBuffer's own buffer.
static void source_buffer_append_data(JSContextHandle ctx, SourceBufferDataHandle &sb, const uint8_t *data, size_t len) {
    (void)ctx;
    if (!data || len == 0) return;
    size_t old_size = sb.append_size();
    size_t new_size = old_size + len;
    uint8_t *buf = (uint8_t *)realloc(sb.append_data(), new_size);
    if (!buf) return;
    memcpy(buf + old_size, data, len);
    sb.set_append_data(buf, new_size, new_size);
}

// Helper: read callback for minimp4 operating on an in-memory buffer.
typedef struct {
    const uint8_t *data;
    size_t size;
} mp4_mem_token_t;

static int mp4_mem_read(int64_t offset, void *buffer, size_t size, void *token) {
    mp4_mem_token_t *tok = (mp4_mem_token_t *)token;
    if (!tok || !buffer) return -1;
    if (offset < 0 || (size_t)offset > tok->size) return -1;
    size_t avail = tok->size - (size_t)offset;
    size_t to_read = size < avail ? size : avail;
    if (to_read > 0) memcpy(buffer, tok->data + offset, to_read);
    return to_read == size ? 0 : -1;
}

// Helper: turn a 32-bit big-endian fourcc into a null-terminated string.
static void fourcc_to_str(uint32_t fcc, char *out, size_t out_len) {
    if (out_len < 5) {
        if (out_len > 0) out[0] = '\0';
        return;
    }
    out[0] = (char)((fcc >> 24) & 0xFF);
    out[1] = (char)((fcc >> 16) & 0xFF);
    out[2] = (char)((fcc >> 8) & 0xFF);
    out[3] = (char)(fcc & 0xFF);
    out[4] = '\0';
}

// Helper: run minimp4 over the accumulated buffer and expose track/sample metadata.
static void source_buffer_demux(JSContextHandle ctx, GCValue sb_obj, SourceBufferDataHandle &sb) {
    if (!sb.append_data() || sb.append_size() < 8) return;

    mp4_mem_token_t tok = { sb.append_data(), sb.append_size() };
    MP4D_demux_t mp4;
    memset(&mp4, 0, sizeof(mp4));
    if (!MP4D_open(&mp4, mp4_mem_read, &tok, (int64_t)tok.size)) {
        // Fragmented MP4 segments (moof/mdat) are not handled by minimp4's demuxer.
        // The bytes are still retained for future processing.
        return;
    }

    sb.set_track_count((int)mp4.track_count);
    double duration_sec = 0.0;
    if (mp4.timescale > 0) {
        uint64_t dur = ((uint64_t)mp4.duration_hi << 32) | mp4.duration_lo;
        duration_sec = (double)dur / (double)mp4.timescale;
    }
    sb.set_parsed_duration(duration_sec);

    GCValue info = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, info, "trackCount", JS_NewInt32(ctx, (int)mp4.track_count));
    JS_SetPropertyStr(ctx, info, "duration", JS_NewFloat64(ctx, duration_sec));
    JS_SetPropertyStr(ctx, info, "timescale", JS_NewInt32(ctx, (int)mp4.timescale));

    GCValue tracks = JS_NewArray(ctx);
    for (unsigned i = 0; i < mp4.track_count; i++) {
        MP4D_track_t *tr = &mp4.track[i];
        GCValue track = JS_NewObject(ctx);
        char handler[8];
        fourcc_to_str(tr->handler_type, handler, sizeof(handler));
        JS_SetPropertyStr(ctx, track, "handlerType", JS_NewString(ctx, handler));
        JS_SetPropertyStr(ctx, track, "sampleCount", JS_NewInt32(ctx, (int)tr->sample_count));
        JS_SetPropertyStr(ctx, track, "timescale", JS_NewInt32(ctx, (int)tr->timescale));
        JS_SetPropertyStr(ctx, track, "objectType", JS_NewInt32(ctx, (int)tr->object_type_indication));

        if (tr->handler_type == MP4D_HANDLER_TYPE_VIDE) {
            JS_SetPropertyStr(ctx, track, "width", JS_NewInt32(ctx, (int)tr->SampleDescription.video.width));
            JS_SetPropertyStr(ctx, track, "height", JS_NewInt32(ctx, (int)tr->SampleDescription.video.height));
        } else if (tr->handler_type == MP4D_HANDLER_TYPE_SOUN) {
            JS_SetPropertyStr(ctx, track, "channels", JS_NewInt32(ctx, (int)tr->SampleDescription.audio.channelcount));
            JS_SetPropertyStr(ctx, track, "sampleRate", JS_NewInt32(ctx, (int)tr->SampleDescription.audio.samplerate_hz));
        }

        GCValue samples = JS_NewArray(ctx);
        unsigned sample_limit = tr->sample_count < 4096 ? tr->sample_count : 4096;
        for (unsigned s = 0; s < sample_limit; s++) {
            unsigned frame_bytes = 0, ts = 0, dur = 0;
            MP4D_file_offset_t offset = MP4D_frame_offset(&mp4, i, s, &frame_bytes, &ts, &dur);
            GCValue sample = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, sample, "offset", JS_NewInt64(ctx, (int64_t)offset));
            JS_SetPropertyStr(ctx, sample, "size", JS_NewInt32(ctx, (int)frame_bytes));
            JS_SetPropertyStr(ctx, sample, "timestamp", JS_NewInt32(ctx, (int)ts));
            JS_SetPropertyStr(ctx, sample, "duration", JS_NewInt32(ctx, (int)dur));
            JS_SetPropertyUint32(ctx, samples, s, sample);
        }
        JS_SetPropertyStr(ctx, track, "samples", samples);
        JS_SetPropertyUint32(ctx, tracks, i, track);
    }
    JS_SetPropertyStr(ctx, info, "tracks", tracks);
    JS_SetPropertyStr(ctx, sb_obj, "__mp4Info", info);

    MP4D_close(&mp4);
}

// SourceBuffer.prototype.appendBuffer
GCValue js_source_buffer_append_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    SourceBufferDataHandle sb = SourceBufferDataHandle::from_object_check(ctx, this_val);
    if (!sb.valid()) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");

    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");

    // Accept ArrayBuffer, TypedArray, ArrayBufferView, Blob, or plain buffer object
    size_t data_len = 0;
    const uint8_t *data = NULL;
    GCValue buffer_val = argv[0];

    if (JS_IsObject(buffer_val)) {
        GCValue byte_length = JS_GetPropertyStr(ctx, buffer_val, "byteLength");
        if (!JS_IsUndefined(byte_length)) {
            JS_ToInt64(ctx, (int64_t*)&data_len, byte_length);
        }
        // For ArrayBuffer / TypedArray, try to get a pointer via QuickJS
        size_t size = 0;
        uint8_t *ptr = JS_GetArrayBuffer(ctx, &size, buffer_val);
        if (!ptr && JS_IsObject(buffer_val)) {
            GCValue buf_prop = JS_GetPropertyStr(ctx, buffer_val, "buffer");
            if (JS_IsObject(buf_prop)) {
                ptr = JS_GetArrayBuffer(ctx, &size, buf_prop);
                if (ptr) data_len = size;
            }
        }
        data = ptr;
    }

    JS_SetPropertyStr(ctx, this_val, "updating", JS_TRUE);
    sb.set_updating(1);

    GCValue onupdatestart = JS_GetPropertyStr(ctx, this_val, "onupdatestart");
    if (!JS_IsNull(onupdatestart) && !JS_IsUndefined(onupdatestart)) {
        JS_Call(ctx, onupdatestart, this_val, 0, NULL);
    }

    if (data && data_len > 0) {
        source_buffer_append_data(ctx, sb, data, data_len);
        source_buffer_demux(ctx, this_val, sb);

        char captured[256];
        snprintf(captured, sizeof(captured),
                 "media-source://segment?size=%zu&tracks=%d&duration=%.3f",
                 sb.append_size(), sb.track_count(), sb.parsed_duration());
        record_captured_url(captured);
    }

    JS_SetPropertyStr(ctx, this_val, "__appendSize", JS_NewInt64(ctx, (int64_t)sb.append_size()));

    GCValue onupdate = JS_GetPropertyStr(ctx, this_val, "onupdate");
    if (!JS_IsNull(onupdate) && !JS_IsUndefined(onupdate)) {
        JS_Call(ctx, onupdate, this_val, 0, NULL);
    }

    JS_SetPropertyStr(ctx, this_val, "updating", JS_FALSE);
    sb.set_updating(0);

    GCValue updateend = JS_GetPropertyStr(ctx, this_val, "onupdateend");
    if (!JS_IsNull(updateend) && !JS_IsUndefined(updateend)) {
        JS_Call(ctx, updateend, this_val, 0, NULL);
    }

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
