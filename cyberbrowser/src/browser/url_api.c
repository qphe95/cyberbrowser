/* Auto-generated split from browser_api_impl.c */
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
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
#include "url_utils.h"

// URL API Implementation
// ============================================================================
//
// URL, URLSearchParams and the URL attributes of <a>/<area> elements follow
// the WHATWG URL Standard.  Parsing and serialization are done in C
// (url_whatwg.c); the interfaces themselves are a small JS layer (below)
// over these natives.  A parsed URL is kept in JS as a "record string":
// every component of a parsed URL is printable ASCII (the parser
// percent-encodes everything else), so control characters can separate
// the fields and the record round-trips exactly (no re-parsing of the
// serialized href, which is not always idempotent).

#include "url_whatwg.h"

/* record: scheme 1 username 1 password 1 host 1 port 1 path 1 query 1 fragment
 *   host:  '-' (null) or 'h' + host
 *   path:  'o' + opaque path, or 'p' + count, then 2 + segment per segment
 *   query, fragment: '-' (null) or 'v' + value */
static char *url_record_from(const WUrl *u)
{
    size_t cap = 256, len = 0;
    char *r = malloc(cap);
#define PUT(str, n) do { size_t n_ = (n); if (len + n_ + 1 > cap) { while (len + n_ + 1 > cap) cap *= 2; r = realloc(r, cap); } memcpy(r + len, (str), n_); len += n_; } while (0)
#define PUTS(str) PUT((str), strlen(str))
#define PUTC(ch) do { char ch_ = (ch); PUT(&ch_, 1); } while (0)
    if (!r)
        return NULL;
    PUTS(u->scheme ? u->scheme : ""); PUTC(1);
    PUTS(u->username ? u->username : ""); PUTC(1);
    PUTS(u->password ? u->password : ""); PUTC(1);
    if (u->host) { PUTC('h'); PUTS(u->host); } else PUTC('-');
    PUTC(1);
    if (u->port >= 0) { char t[16]; snprintf(t, sizeof(t), "%d", u->port); PUTS(t); }
    PUTC(1);
    if (u->opaque_path) {
        PUTC('o'); PUTS(u->opaque_path);
    } else {
        char t[16];
        snprintf(t, sizeof(t), "p%d", u->path_len);
        PUTS(t);
        for (int i = 0; i < u->path_len; i++) { PUTC(2); PUTS(u->path[i]); }
    }
    PUTC(1);
    if (u->query) { PUTC('v'); PUTS(u->query); } else PUTC('-');
    PUTC(1);
    if (u->fragment) { PUTC('v'); PUTS(u->fragment); } else PUTC('-');
    r[len] = 0;
#undef PUT
#undef PUTS
#undef PUTC
    return r;
}

static char *field_dup(const char *s, const char *e) { char *r = malloc(e - s + 1); memcpy(r, s, e - s); r[e - s] = 0; return r; }

static int url_record_to(const char *rec, WUrl *u)
{
    const char *f[8], *p = rec;
    int nf = 0;
    memset(u, 0, sizeof(*u));
    u->port = -1;
    f[nf++] = p;
    for (; *p && nf < 8; p++)
        if (*p == 1)
            f[nf++] = p + 1;
    if (nf != 8)
        return -1;
#define END(i) (i < 7 ? f[i + 1] - 1 : f[i] + strlen(f[i]))
    u->scheme = field_dup(f[0], END(0));
    u->username = field_dup(f[1], END(1));
    u->password = field_dup(f[2], END(2));
    if (f[3][0] == 'h')
        u->host = field_dup(f[3] + 1, END(3));
    if (END(4) > f[4])
        u->port = atoi(f[4]);
    if (f[5][0] == 'o') {
        u->opaque_path = field_dup(f[5] + 1, END(5));
    } else {
        int cnt = atoi(f[5] + 1);
        const char *q = f[5], *e = END(5);
        while (q < e && *q != 2)
            q++;
        u->path_cap = cnt > 0 ? cnt : 1;
        u->path = calloc(u->path_cap, sizeof(char *));
        for (int i = 0; i < cnt && q < e; i++) {
            const char *st = q + 1, *en = st;
            while (en < e && *en != 2)
                en++;
            u->path[u->path_len++] = field_dup(st, en);
            q = en;
        }
    }
    if (f[6][0] == 'v')
        u->query = field_dup(f[6] + 1, END(6));
    if (f[7][0] == 'v')
        u->fragment = field_dup(f[7] + 1, END(7));
#undef END
    return 0;
}

static GCValue url_record_value(JSContextHandle ctx, const WUrl *u)
{
    char *r = url_record_from(u);
    GCValue v = r ? JS_NewString(ctx, r) : JS_NULL;
    free(r);
    return v;
}

/* __url.parse(input [, baseRecord]) -> record or null */
static GCValue js_url_native_parse(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    size_t len;
    const char *in = argc > 0 ? JS_ToCStringLen(ctx, &len, argv[0]) : NULL;
    WUrl base, u;
    int has_base = 0;
    if (!in)
        return JS_EXCEPTION;
    if (argc > 1 && JS_IsString(argv[1])) {
        const char *b = JS_ToCString(ctx, argv[1]);
        if (!b || url_record_to(b, &base))
            return JS_NULL;
        has_base = 1;
    }
    int r = wurl_parse(in, len, has_base ? &base : NULL, &u);
    if (has_base)
        wurl_free(&base);
    if (r)
        return JS_NULL;
    GCValue v = url_record_value(ctx, &u);
    wurl_free(&u);
    return v;
}

/* __url.components(record) -> [href, origin, protocol, username, password,
   host, hostname, port, pathname, search, hash] */
static GCValue js_url_native_components(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    const char *rec = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    WUrl u;
    if (!rec || url_record_to(rec, &u))
        return JS_ThrowTypeError(ctx, "invalid URL record");
    char *f[11] = {
        wurl_href(&u), wurl_origin(&u), wurl_protocol(&u), strdup(u.username),
        strdup(u.password), wurl_host(&u), wurl_hostname(&u), wurl_port(&u),
        wurl_pathname(&u), wurl_search(&u), wurl_hash(&u)
    };
    GCValue arr = JS_NewArray(ctx);
    for (int i = 0; i < 11; i++) {
        JS_SetPropertyUint32(ctx, arr, i, JS_NewString(ctx, f[i] ? f[i] : ""));
        free(f[i]);
    }
    wurl_free(&u);
    return arr;
}

/* __url.set(record, which, value) -> new record, or null if an href setter
   value does not parse */
static GCValue js_url_native_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    int32_t which = 0;
    size_t len;
    const char *rec = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (argc < 3 || !rec || JS_ToInt32(ctx, &which, argv[1]))
        return JS_ThrowTypeError(ctx, "invalid arguments");
    const char *val = JS_ToCStringLen(ctx, &len, argv[2]);
    WUrl u;
    if (!val)
        return JS_EXCEPTION;
    if (url_record_to(rec, &u))
        return JS_ThrowTypeError(ctx, "invalid URL record");
    if (wurl_set(&u, (WUrlSetter)which, val, len)) {
        wurl_free(&u);
        return JS_NULL;
    }
    GCValue v = url_record_value(ctx, &u);
    wurl_free(&u);
    return v;
}

static GCValue js_url_native_form_encode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    size_t len;
    const char *s = argc > 0 ? JS_ToCStringLen(ctx, &len, argv[0]) : NULL;
    if (!s)
        return JS_EXCEPTION;
    char *e = wurl_form_encode(s, len);
    GCValue v = JS_NewString(ctx, e ? e : "");
    free(e);
    return v;
}

static GCValue js_url_native_form_decode(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv)
{
    (void)this_val;
    size_t len, out_len = 0;
    const char *s = argc > 0 ? JS_ToCStringLen(ctx, &len, argv[0]) : NULL;
    if (!s)
        return JS_EXCEPTION;
    char *d = wurl_form_decode(s, len, &out_len);
    GCValue v = d ? JS_NewStringLen(ctx, d, out_len) : JS_NewString(ctx, "");
    free(d);
    return v;
}

GCValue js_url_create_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");

    GCValue obj = argv[0];
    static int blob_counter = 0;
    char blob_url[512];

    const char *origin = "";
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue location = JS_GetPropertyStr(ctx, global_obj, "location");
    if (!JS_IsUndefined(location) && !JS_IsNull(location)) {
        GCValue origin_val = JS_GetPropertyStr(ctx, location, "origin");
        if (!JS_IsUndefined(origin_val) && !JS_IsNull(origin_val)) {
            const char *s = JS_ToCString(ctx, origin_val);
            if (s) origin = s;
        }
    }

    bool is_media_source = false;
    MediaSourceDataHandle ms = JS_GetOpaqueHandle(obj, js_media_source_class_id);
    if (!SAFE_IS_NULL(ms)) {
        is_media_source = true;
    }

    if (origin && origin[0]) {
        snprintf(blob_url, sizeof(blob_url), "blob:%s/%s%d", origin,
                 is_media_source ? "ms-" : "", ++blob_counter);
    } else {
        snprintf(blob_url, sizeof(blob_url), "blob:cyberbrowser/%s%d",
                 is_media_source ? "ms-" : "", ++blob_counter);
    }

    GCValue registry = JS_GetPropertyStr(ctx, global_obj, "__blobRegistry");
    if (!JS_IsUndefined(registry) && !JS_IsNull(registry)) {
        JS_SetPropertyStr(ctx, registry, blob_url, obj);
    }

    if (is_media_source) {
        JS_SetPropertyStr(ctx, obj, "__blobUrl", JS_NewString(ctx, blob_url));
    }

    capture_url_debug(blob_url, is_media_source ? "url_create_object_url_ms" : "url_create_object_url_generic");
    return JS_NewString(ctx, blob_url);
}

// URL.revokeObjectURL()
GCValue js_url_revoke_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_UNDEFINED;
    GCValue global_obj = JS_GetGlobalObject(ctx);
    GCValue registry = JS_GetPropertyStr(ctx, global_obj, "__blobRegistry");
    if (!JS_IsUndefined(registry) && !JS_IsNull(registry)) {
        JSAtom url_atom = JS_NewAtom(ctx, url);
        JS_DeleteProperty(ctx, registry, url_atom, 0);
        JS_FreeAtom(ctx, url_atom);
    }
    return JS_UNDEFINED;
}


// ============================================================================
// URL / URLSearchParams / HTMLHyperlinkElementUtils (JS layer)
// ============================================================================

static const char *url_api_js =
#include "url_api_js.inc"
;

void url_api_install(JSContextHandle ctx, GCValue global)
{
    GCValue natives = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, natives, "parse", JS_NewCFunction(ctx, js_url_native_parse, "parse", 2));
    JS_SetPropertyStr(ctx, natives, "components", JS_NewCFunction(ctx, js_url_native_components, "components", 1));
    JS_SetPropertyStr(ctx, natives, "set", JS_NewCFunction(ctx, js_url_native_set, "set", 3));
    JS_SetPropertyStr(ctx, natives, "formEncode", JS_NewCFunction(ctx, js_url_native_form_encode, "formEncode", 1));
    JS_SetPropertyStr(ctx, natives, "formDecode", JS_NewCFunction(ctx, js_url_native_form_decode, "formDecode", 1));
    JS_SetPropertyStr(ctx, natives, "createObjectURL", JS_NewCFunction(ctx, js_url_create_object_url, "createObjectURL", 1));
    JS_SetPropertyStr(ctx, natives, "revokeObjectURL", JS_NewCFunction(ctx, js_url_revoke_object_url, "revokeObjectURL", 1));
    GCValue fn = JS_Eval(ctx, url_api_js, strlen(url_api_js), "<url_api>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        GCValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        LOG_ERROR("URL API setup failed: %s", m ? m : "?");
        return;
    }
    GCValue args[2] = { global, natives };
    GCValue r = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
    if (JS_IsException(r)) {
        GCValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        LOG_ERROR("URL API setup failed: %s", m ? m : "?");
    }
}

// ============================================================================
// Request/Response API (for fetch)
// ============================================================================

GCValue js_request_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "Request constructor requires at least 1 argument");
    }
    
    GCValue request_obj = JS_NewObject(ctx);
    
    // Handle Request object or URL string
    if (JS_IsObject(argv[0])) {
        // Copy from existing Request
        GCValue url_val = JS_GetPropertyStr(ctx, argv[0], "url");
        JS_SetPropertyStr(ctx, request_obj, "url", url_val);
        GCValue method_val = JS_GetPropertyStr(ctx, argv[0], "method");
        JS_SetPropertyStr(ctx, request_obj, "method", method_val);
    } else {
        // URL string
        const char *url_str = JS_ToCString(ctx, argv[0]);
        if (url_str) {
            JS_SetPropertyStr(ctx, request_obj, "url", JS_NewString(ctx, url_str));
            // Capture the URL
            capture_url_debug(url_str, "request_ctor");
        }
        JS_SetPropertyStr(ctx, request_obj, "method", JS_NewString(ctx, "GET"));
    }
    
    // Handle init options
    if (argc > 1 && JS_IsObject(argv[1])) {
        GCValue method_val = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(method_val)) {
            JS_SetPropertyStr(ctx, request_obj, "method", method_val);
        }
        GCValue headers_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (!JS_IsUndefined(headers_val)) {
            JS_SetPropertyStr(ctx, request_obj, "headers", headers_val);
        }
        GCValue body_val = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(body_val)) {
            JS_SetPropertyStr(ctx, request_obj, "body", body_val);
        }
    }
    
    // Set default headers only if not already set
    GCValue existing_headers = JS_GetPropertyStr(ctx, request_obj, "headers");
    if (JS_IsUndefined(existing_headers) || JS_IsNull(existing_headers)) {
        JS_SetPropertyStr(ctx, request_obj, "headers", JS_NewObject(ctx));
    }
    
    return request_obj;
}

// Response constructor
GCValue js_response_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    GCValue response_obj = JS_NewObject(ctx);
    
    // Set status
    int status = 200;
    if (argc > 1) {
        JS_ToInt32(ctx, &status, argv[1]);
    }
    JS_SetPropertyStr(ctx, response_obj, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, response_obj, "ok", JS_NewBool(ctx, status >= 200 && status < 300));
    JS_SetPropertyStr(ctx, response_obj, "statusText", JS_NewString(ctx, "OK"));
    
    // Set body
    if (argc > 0) {
        JS_SetPropertyStr(ctx, response_obj, "body", argv[0]);
    }
    
    // Headers
    JS_SetPropertyStr(ctx, response_obj, "headers", JS_NewObject(ctx));
    
    return response_obj;
}

// Response.json() static method
GCValue js_response_json(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // Return a promise that resolves to the JSON
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue promise_ctor = JS_GetPropertyStr(ctx, global, "Promise");
    GCValue promise = JS_CallConstructor(ctx, promise_ctor, 0, NULL);
    return promise;
}

// ============================================================================
// Navigator sendBeacon
// ============================================================================

