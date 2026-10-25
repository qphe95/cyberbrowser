/* Media Source Extensions and media element playback.
 *
 * MediaSource / SourceBuffer keep their media in a MediaBuffer
 * (src/media/media_buffer.h: demux, buffered ranges, decode).  A video
 * element whose src is a MediaSource's object URL gets a player here: the
 * playback clock, readyState and the media events (loadedmetadata, canplay,
 * playing, timeupdate, waiting, ...).  Events are queued as tasks and fired
 * from media_tick(), at the top of an event-loop turn, like a browser's
 * media task source; appendBuffer()'s update events come the same way.
 * Audio is demuxed (its buffered ranges gate playback) but not played. */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include "det_clock.h"
#include <quickjs.h>
#include <quickjs_gc_unified.h>
#include "browser_api_impl.h"
#include "browser_api_impl_types.h"
#include "browser_api_impl_handle_helpers.h"
#include "browser_api_impl_internal.h"
#include "html_dom.h"
#include "css_parser.h"
#include "gc_value_helpers.h"
#include "platform.h"
#include "dom_wrap.h"
#include "media/media_buffer.h"
#include "cyber_env.h"
#include <stdio.h>

// ============================================================================
// Classes
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
        if (sb && sb->mb) {
            media_buffer_destroy(sb->mb);
            sb->mb = NULL;
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

/* CYBER_MEDIA_LOG=1: appends, ranges and player state on stderr. */
static bool media_log_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = cyber_getenv("CYBER_MEDIA_LOG");
        on = e && e[0] == '1';
    }
    return on;
}
#define MEDIA_LOG(...) do { if (media_log_on()) { fprintf(stderr, "[media] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

static MediaBuffer *sb_media(GCValue sb_obj) {
    SourceBufferData *sb = (SourceBufferData *)gc_deref(JS_GetOpaqueHandle(sb_obj, js_source_buffer_class_id));
    return sb ? sb->mb : NULL;
}

// ============================================================================
// Media tasks: events fired at the top of the next event-loop turn
// ============================================================================

typedef struct MediaTask {
    GCValue target;             /* rooted while queued */
    char type[24];
    bool clear_updating;        /* SourceBuffer: updating = false first */
} MediaTask;

static MediaTask *g_tasks;
static int g_ntasks, g_captasks;

static void root_value(GCValue v, bool add) {
    if (GC_VALUE_GET_TAG(v) >= 0) return;
    GCHandle h = GC_VALUE_GET_HANDLE(v);
    if (h == GC_HANDLE_NULL) return;
    if (add) gc_add_root(h); else gc_remove_root(h);
}

static void media_queue_event(GCValue target, const char *type, bool clear_updating) {
    if (g_ntasks == g_captasks) {
        int nc = g_captasks ? g_captasks * 2 : 64;
        MediaTask *t = (MediaTask *)realloc(g_tasks, (size_t)nc * sizeof(MediaTask));
        if (!t) return;
        g_tasks = t;
        g_captasks = nc;
    }
    MediaTask *t = &g_tasks[g_ntasks++];
    t->target = target;
    snprintf(t->type, sizeof(t->type), "%s", type);
    t->clear_updating = clear_updating;
    root_value(target, true);
}

/* target.dispatchEvent(new Event(type)) */
static void media_fire(JSContextHandle ctx, GCValue target, const char *type) {
    MEDIA_LOG("event %s", type);
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue ctor = JS_GetPropertyStr(ctx, global, "Event");
    GCValue dispatch = JS_GetPropertyStr(ctx, target, "dispatchEvent");
    if (!JS_IsFunction(ctx, ctor) || !JS_IsFunction(ctx, dispatch)) return;
    GCValue name = JS_NewString(ctx, type);
    GCValue ev = JS_CallConstructor(ctx, ctor, 1, &name);
    if (JS_IsException(ev)) { JS_GetException(ctx); return; }
    GCValue r = JS_Call(ctx, dispatch, target, 1, &ev);
    if (JS_IsException(r)) JS_GetException(ctx);
}

/* A TimeRanges object over ranges[0..n). */
static GCValue time_ranges_at(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv,
                              const char *which) {
    uint32_t i = 0;
    if (argc > 0) JS_ToUint32(ctx, &i, argv[0]);
    GCValue a = JS_GetPropertyStr(ctx, this_val, which);
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, JS_GetPropertyStr(ctx, a, "length"));
    if (i >= n) return JS_ThrowRangeError(ctx, "IndexSizeError: index out of range");
    return JS_GetPropertyUint32(ctx, a, i);
}

static GCValue time_ranges_start(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return time_ranges_at(ctx, this_val, argc, argv, "__starts");
}

static GCValue time_ranges_end(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return time_ranges_at(ctx, this_val, argc, argv, "__ends");
}

static GCValue make_time_ranges(JSContextHandle ctx, const MediaRange *r, int n) {
    GCValue tr = JS_NewObject(ctx);
    GCValue starts = JS_NewArray(ctx), ends = JS_NewArray(ctx);
    for (int i = 0; i < n; i++) {
        JS_SetPropertyUint32(ctx, starts, (uint32_t)i, JS_NewFloat64(ctx, r[i].start));
        JS_SetPropertyUint32(ctx, ends, (uint32_t)i, JS_NewFloat64(ctx, r[i].end));
    }
    JS_SetPropertyStr(ctx, tr, "__starts", starts);
    JS_SetPropertyStr(ctx, tr, "__ends", ends);
    JS_SetPropertyStr(ctx, tr, "length", JS_NewInt32(ctx, n));
    JS_SetPropertyStr(ctx, tr, "start", JS_NewCFunction(ctx, time_ranges_start, "start", 1));
    JS_SetPropertyStr(ctx, tr, "end", JS_NewCFunction(ctx, time_ranges_end, "end", 1));
    return tr;
}

#define MAX_RANGES 64

// ============================================================================
// MediaSource
// ============================================================================

GCValue js_media_source_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)new_target; (void)argc; (void)argv;
    MediaSourceDataHandle ms = media_source_create(ctx);
    if (SAFE_IS_NULL(ms)) return JS_ThrowTypeError(ctx, "Invalid MediaSource");

    GCValue obj = JS_NewObjectClass(ctx, js_media_source_class_id);
    JS_SetOpaqueHandle(obj, ms);

    // Register a stable blob URL for this MediaSource
    GCValue url_args[1] = { obj };
    GCValue blob_url = js_url_create_object_url(ctx, JS_UNDEFINED, 1, url_args);
    const char *url_str = JS_ToCString(ctx, blob_url);
    if (url_str) {
        JS_SetPropertyStr(ctx, obj, "url", JS_NewString(ctx, url_str));
        JS_SetPropertyStr(ctx, obj, "__blobUrl", JS_NewString(ctx, url_str));
    }

    // Initialize source buffer lists
    GCValue lists_[2] = { JS_NewArray(ctx), JS_NewArray(ctx) };
    js_media_make_event_target(ctx, lists_[0]);
    js_media_make_event_target(ctx, lists_[1]);
    JS_SetPropertyStr(ctx, obj, "__sourceBuffers", lists_[0]);
    JS_SetPropertyStr(ctx, obj, "__activeSourceBuffers", lists_[1]);

    return obj;
}

// SourceBuffer constructor (internal use)
GCValue js_source_buffer_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    (void)new_target;
    SourceBufferDataHandle sb = source_buffer_create(ctx);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");

    const char *mime_type = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (mime_type) SAFE_SET_STR(sb, SourceBufferDataHandle, mime_type, mime_type);
    SourceBufferData *d = (SourceBufferData *)gc_deref(sb);
    if (d) d->mb = media_buffer_create(mime_type ? mime_type : "");

    GCValue obj = JS_NewObjectClass(ctx, js_source_buffer_class_id);
    JS_SetOpaqueHandle(obj, sb);
    return obj;
}

// MediaSource.isTypeSupported(static)
GCValue js_media_source_is_type_supported(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const char *mime_type = JS_ToCString(ctx, argv[0]);
    return JS_NewBool(ctx, mime_type && media_type_supported(mime_type));
}

static void list_push(JSContextHandle ctx, GCValue arr, GCValue v) {
    int32_t len = 0;
    JS_ToInt32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
    JS_SetPropertyUint32(ctx, arr, (uint32_t)len, v);
}

// MediaSource.prototype.addSourceBuffer
GCValue js_media_source_add_source_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    MediaSourceDataHandle ms = JS_GetOpaqueHandle2(ctx, this_val, js_media_source_class_id);
    if (SAFE_IS_NULL(ms)) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    const char *mime_type = JS_ToCString(ctx, argv[0]);
    if (!mime_type) return JS_ThrowTypeError(ctx, "Invalid MIME type");
    MEDIA_LOG("addSourceBuffer(%s) supported=%d", mime_type, media_type_supported(mime_type));
    if (!media_type_supported(mime_type))
        return JS_ThrowTypeError(ctx, "NotSupportedError: unsupported type %s", mime_type);

    GCValue sb_args[1] = { JS_NewString(ctx, mime_type) };
    GCValue sb = js_source_buffer_constructor(ctx, JS_UNDEFINED, 1, sb_args);
    if (JS_IsException(sb)) return sb;
    JS_SetPropertyStr(ctx, sb, "mimeType", JS_NewString(ctx, mime_type));
    JS_SetPropertyStr(ctx, sb, "__mediaSource", this_val);

    list_push(ctx, JS_GetPropertyStr(ctx, this_val, "__sourceBuffers"), sb);
    list_push(ctx, JS_GetPropertyStr(ctx, this_val, "__activeSourceBuffers"), sb);
    return sb;
}

// MediaSource.prototype.removeSourceBuffer
GCValue js_media_source_remove_source_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    static const char *const lists[] = { "__sourceBuffers", "__activeSourceBuffers" };
    for (int l = 0; l < 2; l++) {
        GCValue arr = JS_GetPropertyStr(ctx, this_val, lists[l]);
        int32_t len = 0;
        JS_ToInt32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
        uint32_t k = 0;
        for (int32_t i = 0; i < len; i++) {
            GCValue v = JS_GetPropertyUint32(ctx, arr, (uint32_t)i);
            if (JS_VALUE_GET_HANDLE(v) != JS_VALUE_GET_HANDLE(argv[0]))
                JS_SetPropertyUint32(ctx, arr, k++, v);
        }
        JS_SetPropertyStr(ctx, arr, "length", JS_NewInt32(ctx, (int32_t)k));
    }
    return JS_UNDEFINED;
}

// MediaSource.prototype.endOfStream
GCValue js_media_source_end_of_stream(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    MediaSourceDataHandle ms = JS_GetOpaqueHandle2(ctx, this_val, js_media_source_class_id);
    if (SAFE_IS_NULL(ms)) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    if (SAFE_GET_OR(ms, MediaSourceDataHandle, ready_state, 0) != 1) return JS_UNDEFINED;
    SAFE_SET(ms, MediaSourceDataHandle, ready_state, 2); // ended
    /* The duration becomes the end of what is buffered. */
    double end = 0.0;
    GCValue arr = JS_GetPropertyStr(ctx, this_val, "__activeSourceBuffers");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
    for (int32_t i = 0; i < len; i++) {
        MediaRange r[MAX_RANGES];
        int n = media_buffer_ranges(sb_media(JS_GetPropertyUint32(ctx, arr, (uint32_t)i)), r, MAX_RANGES);
        if (n > 0 && r[n - 1].end > end) end = r[n - 1].end;
    }
    if (end > 0.0) SAFE_SET(ms, MediaSourceDataHandle, duration, end);
    media_queue_event(this_val, "sourceended", false);
    return JS_UNDEFINED;
}

// MediaSource.readyState getter
GCValue js_media_source_get_ready_state(JSContextHandle ctx, GCValue this_val) {
    MediaSourceDataHandle ms = JS_GetOpaqueHandle2(ctx, this_val, js_media_source_class_id);
    if (SAFE_IS_NULL(ms)) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    static const char *states[] = {"closed", "open", "ended"};
    int state = SAFE_GET_OR(ms, MediaSourceDataHandle, ready_state, 0);
    return JS_NewString(ctx, state >= 0 && state < 3 ? states[state] : "closed");
}

// MediaSource.duration getter/setter
GCValue js_media_source_get_duration(JSContextHandle ctx, GCValue this_val) {
    MediaSourceDataHandle ms = JS_GetOpaqueHandle2(ctx, this_val, js_media_source_class_id);
    if (SAFE_IS_NULL(ms)) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    if (SAFE_GET_OR(ms, MediaSourceDataHandle, ready_state, 0) == 0) return JS_NewFloat64(ctx, NAN);
    return JS_NewFloat64(ctx, SAFE_GET_OR(ms, MediaSourceDataHandle, duration, NAN));
}

GCValue js_media_source_set_duration(JSContextHandle ctx, GCValue this_val, GCValue val) {
    MediaSourceDataHandle ms = JS_GetOpaqueHandle2(ctx, this_val, js_media_source_class_id);
    if (SAFE_IS_NULL(ms)) return JS_ThrowTypeError(ctx, "Invalid MediaSource");
    double duration;
    JS_ToFloat64(ctx, &duration, val);
    SAFE_SET(ms, MediaSourceDataHandle, duration, duration);
    GCValue video = JS_GetPropertyStr(ctx, this_val, "__mediaElement");
    if (JS_IsObject(video)) media_queue_event(video, "durationchange", false);
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

// ============================================================================
// SourceBuffer
// ============================================================================

/* Start an update (appendBuffer / remove): updating until its events fire. */
static bool sb_begin_update(JSContextHandle ctx, GCValue this_val, SourceBufferDataHandle sb) {
    if (SAFE_GET_OR(sb, SourceBufferDataHandle, updating, 0)) {
        JS_ThrowTypeError(ctx, "InvalidStateError: the SourceBuffer is updating");
        return false;
    }
    SAFE_SET(sb, SourceBufferDataHandle, updating, 1);
    media_queue_event(this_val, "updatestart", false);
    return true;
}

static void sb_end_update(GCValue this_val) {
    media_queue_event(this_val, "update", true);
    media_queue_event(this_val, "updateend", false);
}

// SourceBuffer.prototype.appendBuffer
GCValue js_source_buffer_append_buffer(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    SourceBufferDataHandle sb = JS_GetOpaqueHandle2(ctx, this_val, js_source_buffer_class_id);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");

    /* ArrayBuffer, or a view on one (TypedArray / DataView). */
    size_t size = 0;
    const uint8_t *data = JS_GetArrayBuffer(ctx, &size, argv[0]);
    if (!data && JS_IsObject(argv[0])) {
        GCValue buf = JS_GetPropertyStr(ctx, argv[0], "buffer");
        size_t whole = 0;
        const uint8_t *base = JS_IsObject(buf) ? JS_GetArrayBuffer(ctx, &whole, buf) : NULL;
        if (base) {
            int64_t off = 0, len = 0;
            JS_ToInt64(ctx, &off, JS_GetPropertyStr(ctx, argv[0], "byteOffset"));
            JS_ToInt64(ctx, &len, JS_GetPropertyStr(ctx, argv[0], "byteLength"));
            if (off >= 0 && len >= 0 && (uint64_t)(off + len) <= whole) {
                data = base + off;
                size = (size_t)len;
            }
        }
    }
    if (!data) return JS_ThrowTypeError(ctx, "appendBuffer: not an ArrayBuffer or view");
    if (!sb_begin_update(ctx, this_val, sb)) return JS_EXCEPTION;

    SourceBufferData *d = (SourceBufferData *)gc_deref(sb);
    if (d && d->mb && !media_buffer_append(d->mb, data, size, d->timestamp_offset))
        media_queue_event(this_val, "error", false);
    d = (SourceBufferData *)gc_deref(sb);
    if (media_log_on() && d) {
        MediaRange r[MAX_RANGES];
        int n = media_buffer_ranges(d->mb, r, MAX_RANGES);
        MEDIA_LOG("append %zu bytes to %.60s: init=%d ranges=%d [%.3f, %.3f]", size, d->mime_type,
                  media_buffer_has_init(d->mb), n, n ? r[0].start : 0.0, n ? r[n - 1].end : 0.0);
    }
    /* A newly complete initialization segment declares the duration. */
    GCValue ms_obj = JS_GetPropertyStr(ctx, this_val, "__mediaSource");
    MediaSourceDataHandle ms = JS_IsObject(ms_obj) ? JS_GetOpaqueHandle(ms_obj, js_media_source_class_id) : GC_HANDLE_NULL;
    d = (SourceBufferData *)gc_deref(sb);
    if (!SAFE_IS_NULL(ms) && d && d->mb && isnan(SAFE_GET_OR(ms, MediaSourceDataHandle, duration, NAN))) {
        double dur = media_buffer_duration(d->mb);
        if (dur > 0.0) SAFE_SET(ms, MediaSourceDataHandle, duration, dur);
    }
    sb_end_update(this_val);
    return JS_UNDEFINED;
}

// SourceBuffer.prototype.abort
GCValue js_source_buffer_abort(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    SourceBufferDataHandle sb = JS_GetOpaqueHandle2(ctx, this_val, js_source_buffer_class_id);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    SourceBufferData *d = (SourceBufferData *)gc_deref(sb);
    if (d && d->mb) media_buffer_reset_parser(d->mb);
    return JS_UNDEFINED;
}

// SourceBuffer.prototype.changeType
GCValue js_source_buffer_change_type(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    SourceBufferDataHandle sb = JS_GetOpaqueHandle2(ctx, this_val, js_source_buffer_class_id);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    const char *mime_type = JS_ToCString(ctx, argv[0]);
    if (!mime_type || !media_type_supported(mime_type))
        return JS_ThrowTypeError(ctx, "NotSupportedError: unsupported type");
    SAFE_SET_STR(sb, SourceBufferDataHandle, mime_type, mime_type);
    /* The next initialization segment describes the new codec. */
    SourceBufferData *d = (SourceBufferData *)gc_deref(sb);
    if (d) {
        if (d->mb) media_buffer_destroy(d->mb);
        d->mb = media_buffer_create(mime_type);
    }
    return JS_UNDEFINED;
}

// SourceBuffer.prototype.remove
GCValue js_source_buffer_remove(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    SourceBufferDataHandle sb = JS_GetOpaqueHandle2(ctx, this_val, js_source_buffer_class_id);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    double start = 0.0, end = 0.0;
    if (argc > 0) JS_ToFloat64(ctx, &start, argv[0]);
    if (argc > 1) JS_ToFloat64(ctx, &end, argv[1]);
    if (!sb_begin_update(ctx, this_val, sb)) return JS_EXCEPTION;
    SourceBufferData *d = (SourceBufferData *)gc_deref(sb);
    if (d && d->mb) media_buffer_remove(d->mb, start, end);
    sb_end_update(this_val);
    return JS_UNDEFINED;
}

// SourceBuffer.updating getter
GCValue js_source_buffer_get_updating(JSContextHandle ctx, GCValue this_val) {
    SourceBufferDataHandle sb = JS_GetOpaqueHandle2(ctx, this_val, js_source_buffer_class_id);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    return JS_NewBool(ctx, SAFE_GET_OR(sb, SourceBufferDataHandle, updating, 0));
}

// SourceBuffer.buffered getter
GCValue js_source_buffer_get_buffered(JSContextHandle ctx, GCValue this_val) {
    MediaRange r[MAX_RANGES];
    int n = media_buffer_ranges(sb_media(this_val), r, MAX_RANGES);
    return make_time_ranges(ctx, r, n);
}

// SourceBuffer.timestampOffset getter/setter
GCValue js_source_buffer_get_timestamp_offset(JSContextHandle ctx, GCValue this_val) {
    SourceBufferDataHandle sb = JS_GetOpaqueHandle2(ctx, this_val, js_source_buffer_class_id);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    return JS_NewFloat64(ctx, SAFE_GET_OR(sb, SourceBufferDataHandle, timestamp_offset, 0.0));
}

GCValue js_source_buffer_set_timestamp_offset(JSContextHandle ctx, GCValue this_val, GCValue val) {
    SourceBufferDataHandle sb = JS_GetOpaqueHandle2(ctx, this_val, js_source_buffer_class_id);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    double offset;
    JS_ToFloat64(ctx, &offset, val);
    SAFE_SET(sb, SourceBufferDataHandle, timestamp_offset, offset);
    return JS_UNDEFINED;
}

// SourceBuffer.mode getter/setter
GCValue js_source_buffer_get_mode(JSContextHandle ctx, GCValue this_val) {
    SourceBufferDataHandle sb = JS_GetOpaqueHandle2(ctx, this_val, js_source_buffer_class_id);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    return JS_NewString(ctx, SAFE_GET_OR(sb, SourceBufferDataHandle, mode, "segments"));
}

GCValue js_source_buffer_set_mode(JSContextHandle ctx, GCValue this_val, GCValue val) {
    SourceBufferDataHandle sb = JS_GetOpaqueHandle2(ctx, this_val, js_source_buffer_class_id);
    if (SAFE_IS_NULL(sb)) return JS_ThrowTypeError(ctx, "Invalid SourceBuffer");
    const char *mode = JS_ToCString(ctx, val);
    if (mode) SAFE_SET_STR(sb, SourceBufferDataHandle, mode, mode);
    return JS_UNDEFINED;
}

const JSCFunctionListEntry js_source_buffer_proto_funcs[] = {
    JS_CFUNC_DEF("appendBuffer", 1, js_source_buffer_append_buffer),
    JS_CFUNC_DEF("abort", 0, js_source_buffer_abort),
    JS_CFUNC_DEF("changeType", 1, js_source_buffer_change_type),
    JS_CFUNC_DEF("remove", 2, js_source_buffer_remove),
    JS_CGETSET_DEF("updating", js_source_buffer_get_updating, NULL),
    JS_CGETSET_DEF("buffered", js_source_buffer_get_buffered, NULL),
    JS_CGETSET_DEF("timestampOffset", js_source_buffer_get_timestamp_offset, js_source_buffer_set_timestamp_offset),
    JS_CGETSET_DEF("mode", js_source_buffer_get_mode, js_source_buffer_set_mode),
    JS_PROP_DOUBLE_DEF("appendWindowStart", 0, JS_PROP_WRITABLE),
    JS_PROP_DOUBLE_DEF("appendWindowEnd", INFINITY, JS_PROP_WRITABLE),
};
const size_t js_source_buffer_proto_funcs_count = sizeof(js_source_buffer_proto_funcs) / sizeof(js_source_buffer_proto_funcs[0]);

void js_media_make_event_target(JSContextHandle ctx, GCValue obj) {
    GCValue ctor = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "EventTarget");
    GCValue proto = JS_IsObject(ctor) ? JS_GetPropertyStr(ctx, ctor, "prototype") : JS_UNDEFINED;
    if (!JS_IsObject(proto)) return;
    static const char *const names[] = { "addEventListener", "removeEventListener", "dispatchEvent" };
    for (int i = 0; i < 3; i++) {
        GCValue fn = JS_GetPropertyStr(ctx, proto, names[i]);
        if (JS_IsFunction(ctx, fn)) JS_SetPropertyStr(ctx, obj, names[i], fn);
    }
}

// ============================================================================
// Media element playback
// ============================================================================

enum { HAVE_NOTHING, HAVE_METADATA, HAVE_CURRENT_DATA, HAVE_FUTURE_DATA, HAVE_ENOUGH_DATA };

typedef struct MediaPlayer {
    GCValue video, ms;          /* rooted while attached */
    NodeId node;
    bool paused, waiting, seeking, ended;
    double t;                   /* current time at base_ms */
    unsigned long long base_ms;
    double rate;
    int ready;
    bool sent_metadata, sent_loadeddata;
    unsigned long long last_timeupdate_ms;
    unsigned long long last_paint_ms;
} MediaPlayer;

#define MAX_PLAYERS 32
static MediaPlayer *g_players[MAX_PLAYERS];
static int g_nplayers;
static JSContextHandle g_media_ctx;   /* the context players were attached in */

static MediaPlayer *player_of(GCValue video) {
    if (!JS_IsObject(video)) return NULL;
    for (int i = 0; i < g_nplayers; i++)
        if (JS_VALUE_GET_HANDLE(g_players[i]->video) == JS_VALUE_GET_HANDLE(video)) return g_players[i];
    return NULL;
}

static double player_time(const MediaPlayer *p) {
    if (p->paused || p->waiting || p->seeking || p->ready < HAVE_FUTURE_DATA) return p->t;
    return p->t + (double)((unsigned long long)page_clock_now_ms() - p->base_ms) / 1000.0 * p->rate;
}

static void player_settle_clock(MediaPlayer *p) {
    p->t = player_time(p);
    p->base_ms = (unsigned long long)page_clock_now_ms();
}

static double player_duration(MediaPlayer *p) {
    MediaSourceDataHandle ms = JS_GetOpaqueHandle(p->ms, js_media_source_class_id);
    if (SAFE_IS_NULL(ms) || SAFE_GET_OR(ms, MediaSourceDataHandle, ready_state, 0) == 0) return NAN;
    return SAFE_GET_OR(ms, MediaSourceDataHandle, duration, NAN);
}

/* The active SourceBuffers' media. */
static int player_buffers(JSContextHandle ctx, MediaPlayer *p, MediaBuffer **out, int max) {
    GCValue arr = JS_GetPropertyStr(ctx, p->ms, "__activeSourceBuffers");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, JS_GetPropertyStr(ctx, arr, "length"));
    int n = 0;
    for (int32_t i = 0; i < len && n < max; i++) {
        MediaBuffer *mb = sb_media(JS_GetPropertyUint32(ctx, arr, (uint32_t)i));
        if (mb) out[n++] = mb;
    }
    return n;
}

/* Is [t, t + ahead) buffered in mb?  Starting up to `slack` early counts
 * (a stream whose first frame is a few ms after 0 still starts at 0). */
static bool covers(MediaBuffer *mb, double t, double ahead, double duration) {
    MediaRange r[MAX_RANGES];
    int n = media_buffer_ranges(mb, r, MAX_RANGES);
    const double slack = 0.3;
    for (int i = 0; i < n; i++) {
        if (t < r[i].start - slack || t >= r[i].end) continue;
        double need = t + ahead;
        if (!isnan(duration) && need > duration) need = duration;
        return r[i].end >= need - 0.05;
    }
    return false;
}

/* The buffered ranges of the element: the intersection of its SourceBuffers'. */
static int player_ranges(JSContextHandle ctx, MediaPlayer *p, MediaRange *out, int max) {
    MediaBuffer *mbs[8];
    int nb = player_buffers(ctx, p, mbs, 8);
    if (nb == 0) return 0;
    int n = media_buffer_ranges(mbs[0], out, max);
    for (int b = 1; b < nb; b++) {
        MediaRange other[MAX_RANGES], res[MAX_RANGES];
        int m = media_buffer_ranges(mbs[b], other, MAX_RANGES), k = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < m && k < MAX_RANGES; j++) {
                double s = fmax(out[i].start, other[j].start), e = fmin(out[i].end, other[j].end);
                if (e > s) res[k++] = (MediaRange){ s, e };
            }
        n = k < max ? k : max;
        memcpy(out, res, (size_t)n * sizeof(MediaRange));
    }
    return n;
}

static void player_update(JSContextHandle ctx, MediaPlayer *p) {
    MediaBuffer *mbs[8];
    int nb = player_buffers(ctx, p, mbs, 8);
    bool meta = nb > 0;
    for (int i = 0; i < nb; i++) if (!media_buffer_has_init(mbs[i])) meta = false;
    double duration = player_duration(p);
    double t = player_time(p);

    int ready = HAVE_NOTHING;
    if (meta) {
        bool cur = true, future = true;
        for (int i = 0; i < nb; i++) {
            if (!covers(mbs[i], t, 0.0, duration)) cur = false;
            if (!covers(mbs[i], t, 1.0, duration)) future = false;
        }
        ready = future ? HAVE_ENOUGH_DATA : cur ? HAVE_CURRENT_DATA : HAVE_METADATA;
    }
    int old = p->ready;
    if (ready != old)
        MEDIA_LOG("readyState %d -> %d at t=%.3f (paused=%d, duration=%.3f)", old, ready, t, p->paused, duration);

    if (ready >= HAVE_METADATA && !p->sent_metadata) {
        p->sent_metadata = true;
        media_queue_event(p->video, "durationchange", false);
        media_queue_event(p->video, "loadedmetadata", false);
        media_queue_event(p->video, "resize", false);
    }
    if (ready < HAVE_FUTURE_DATA && old >= HAVE_FUTURE_DATA) {
        player_settle_clock(p);         /* freeze the clock while stalled */
        if (!p->paused) { p->waiting = true; media_queue_event(p->video, "waiting", false); }
    }
    p->ready = ready;
    if (ready >= HAVE_CURRENT_DATA && !p->sent_loadeddata) {
        p->sent_loadeddata = true;
        media_queue_event(p->video, "loadeddata", false);
    }
    if (p->seeking && ready >= HAVE_CURRENT_DATA) {
        p->seeking = false;
        p->base_ms = (unsigned long long)page_clock_now_ms();
        media_queue_event(p->video, "seeked", false);
        media_queue_event(p->video, "timeupdate", false);
    }
    if (ready >= HAVE_FUTURE_DATA && old < HAVE_FUTURE_DATA) {
        p->base_ms = (unsigned long long)page_clock_now_ms();
        media_queue_event(p->video, "canplay", false);
        if (ready == HAVE_ENOUGH_DATA) media_queue_event(p->video, "canplaythrough", false);
        if (!p->paused) { p->waiting = false; media_queue_event(p->video, "playing", false); }
    }

    unsigned long long now = (unsigned long long)page_clock_now_ms();
    MediaSourceDataHandle ms = JS_GetOpaqueHandle(p->ms, js_media_source_class_id);
    bool stream_ended = !SAFE_IS_NULL(ms) && SAFE_GET_OR(ms, MediaSourceDataHandle, ready_state, 0) == 2;
    if (!p->paused && !p->ended && stream_ended && !isnan(duration) && player_time(p) >= duration - 0.05) {
        p->t = duration;
        p->base_ms = now;
        p->paused = true;
        p->ended = true;
        media_queue_event(p->video, "timeupdate", false);
        media_queue_event(p->video, "pause", false);
        media_queue_event(p->video, "ended", false);
    }
    bool running = !p->paused && !p->waiting && p->ready >= HAVE_FUTURE_DATA;
    if (running && now - p->last_timeupdate_ms >= 250) {
        p->last_timeupdate_ms = now;
        media_queue_event(p->video, "timeupdate", false);
    }
    /* A new frame needs a repaint (but is not a document change: the
     * page can still be quiet while a video plays). */
    if ((running || p->ready != old) && now - p->last_paint_ms >= 500) {
        p->last_paint_ms = now;
        g_dom_needs_layout = 1;
    }
}

int media_tick(JSContextHandle ctx) {
    for (int i = 0; i < g_nplayers; i++) player_update(ctx, g_players[i]);
    if (g_ntasks == 0) return 0;
    /* Fire what is queued now; events these handlers queue wait a turn. */
    int n = g_ntasks;
    MediaTask *tasks = (MediaTask *)malloc((size_t)n * sizeof(MediaTask));
    if (!tasks) return 0;
    memcpy(tasks, g_tasks, (size_t)n * sizeof(MediaTask));
    g_ntasks = 0;
    for (int i = 0; i < n; i++) {
        if (tasks[i].clear_updating) {
            SourceBufferDataHandle sb = JS_GetOpaqueHandle(tasks[i].target, js_source_buffer_class_id);
            if (!SAFE_IS_NULL(sb)) SAFE_SET(sb, SourceBufferDataHandle, updating, 0);
        }
        media_fire(ctx, tasks[i].target, tasks[i].type);
        root_value(tasks[i].target, false);
    }
    free(tasks);
    return n;
}

bool media_tick_pending(void) { return g_ntasks > 0; }

bool media_element_attach(JSContextHandle ctx, GCValue video, GCValue ms_obj) {
    MEDIA_LOG("attach");
    MediaSourceDataHandle ms = JS_GetOpaqueHandle(ms_obj, js_media_source_class_id);
    if (SAFE_IS_NULL(ms)) return false;
    media_element_detach(ctx, video);
    if (g_nplayers == MAX_PLAYERS) return false;
    MediaPlayer *p = (MediaPlayer *)calloc(1, sizeof(MediaPlayer));
    if (!p) return false;
    p->video = video;
    p->ms = ms_obj;
    root_value(video, true);
    root_value(ms_obj, true);
    p->node = dom_node_of(video);
    g_media_ctx = ctx;
    p->paused = true;
    p->rate = 1.0;
    p->base_ms = (unsigned long long)page_clock_now_ms();
    g_players[g_nplayers++] = p;
    JS_SetPropertyStr(ctx, ms_obj, "__mediaElement", video);
    SAFE_SET(ms, MediaSourceDataHandle, ready_state, 1);   /* open */
    media_queue_event(video, "loadstart", false);
    media_queue_event(ms_obj, "sourceopen", false);
    return true;
}

void media_element_detach(JSContextHandle ctx, GCValue video) {
    for (int i = 0; i < g_nplayers; i++) {
        MediaPlayer *p = g_players[i];
        if (JS_VALUE_GET_HANDLE(p->video) != JS_VALUE_GET_HANDLE(video)) continue;
        MediaSourceDataHandle ms = JS_GetOpaqueHandle(p->ms, js_media_source_class_id);
        if (!SAFE_IS_NULL(ms)) SAFE_SET(ms, MediaSourceDataHandle, ready_state, 0);
        media_queue_event(p->ms, "sourceclose", false);
        JS_SetPropertyStr(ctx, p->ms, "__mediaElement", JS_UNDEFINED);
        root_value(p->video, false);
        root_value(p->ms, false);
        free(p);
        g_players[i] = g_players[--g_nplayers];
        return;
    }
}

bool media_element_active(GCValue video) { return player_of(video) != NULL; }

double media_element_current_time(GCValue video) {
    MediaPlayer *p = player_of(video);
    return p ? player_time(p) : 0.0;
}

void media_element_seek(GCValue video, double t) {
    MediaPlayer *p = player_of(video);
    if (!p || isnan(t)) return;
    if (t < 0) t = 0;
    p->t = t;
    p->base_ms = (unsigned long long)page_clock_now_ms();
    p->seeking = true;
    p->ended = false;
    media_queue_event(p->video, "seeking", false);
}

double media_element_duration(GCValue video) {
    MediaPlayer *p = player_of(video);
    return p ? player_duration(p) : NAN;
}

int media_element_ready_state(GCValue video) {
    MediaPlayer *p = player_of(video);
    return p ? p->ready : 0;
}

bool media_element_paused(GCValue video) {
    MediaPlayer *p = player_of(video);
    return p ? p->paused : true;
}

bool media_element_ended(GCValue video) {
    MediaPlayer *p = player_of(video);
    return p ? p->ended : false;
}

GCValue media_element_buffered(JSContextHandle ctx, GCValue video) {
    MediaPlayer *p = player_of(video);
    MediaRange r[MAX_RANGES];
    int n = p ? player_ranges(ctx, p, r, MAX_RANGES) : 0;
    return make_time_ranges(ctx, r, n);
}

void media_element_play(GCValue video) {
    MEDIA_LOG("play()");
    MediaPlayer *p = player_of(video);
    if (!p || !p->paused) return;
    if (p->ended) { p->t = 0.0; p->ended = false; }
    p->paused = false;
    p->base_ms = (unsigned long long)page_clock_now_ms();
    media_queue_event(p->video, "play", false);
    if (p->ready >= HAVE_FUTURE_DATA) media_queue_event(p->video, "playing", false);
    else { p->waiting = true; media_queue_event(p->video, "waiting", false); }
}

void media_element_pause(GCValue video) {
    MediaPlayer *p = player_of(video);
    if (!p || p->paused) return;
    player_settle_clock(p);
    p->paused = true;
    p->waiting = false;
    media_queue_event(p->video, "timeupdate", false);
    media_queue_event(p->video, "pause", false);
}

void media_element_set_rate(GCValue video, double rate) {
    MediaPlayer *p = player_of(video);
    if (!p || isnan(rate)) return;
    player_settle_clock(p);
    p->rate = rate;
    media_queue_event(p->video, "ratechange", false);
}

static MediaBuffer *player_video_buffer(JSContextHandle ctx, MediaPlayer *p) {
    MediaBuffer *mbs[8];
    int nb = player_buffers(ctx, p, mbs, 8);
    for (int i = 0; i < nb; i++)
        if (media_buffer_kind(mbs[i]) == MEDIA_KIND_VIDEO) return mbs[i];
    return NULL;
}

void media_element_video_size(JSContextHandle ctx, GCValue video, int *w, int *h) {
    *w = *h = 0;
    MediaPlayer *p = player_of(video);
    MediaBuffer *mb = p ? player_video_buffer(ctx, p) : NULL;
    if (mb) media_buffer_video_size(mb, w, h);
}

bool media_video_frame_for_node(NodeId node, const uint8_t **rgba,
                                int *w, int *h, uint64_t *frame_id) {
    for (int i = 0; i < g_nplayers; i++) {
        MediaPlayer *p = g_players[i];
        if (p->node != node) continue;
        MediaBuffer *mb = player_video_buffer(g_media_ctx, p);
        return mb && media_buffer_frame_at(mb, player_time(p), rgba, w, h, frame_id);
    }
    return false;
}
