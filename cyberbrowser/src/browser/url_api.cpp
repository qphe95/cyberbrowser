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

// URL API Implementation
// ============================================================================

typedef struct {
    char protocol[64];
    char hostname[256];
    char host[288];
    char pathname[512];
    char search[512];
    char hash[256];
    char origin[512];
    char href[1024];
    int port;
} UrlComponents;

static void url_unescape_js_string(const char *in, char *out, size_t out_len) {
    size_t i = 0, j = 0;
    while (in[i] && j + 2 < out_len) {
        if (in[i] == '\\' || in[i] == '\'') {
            out[j++] = '\\';
        }
        out[j++] = in[i++];
    }
    out[j] = '\0';
}

static void url_parse_components(const char *url, UrlComponents *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->pathname, "/", sizeof(out->pathname) - 1);

    const char *p = url ? url : "";
    const char *proto_end = strstr(p, "://");
    if (proto_end) {
        size_t proto_len = proto_end - p;
        if (proto_len >= sizeof(out->protocol)) proto_len = sizeof(out->protocol) - 1;
        memcpy(out->protocol, p, proto_len);
        out->protocol[proto_len] = '\0';
        p = proto_end + 3;

        const char *path_start = strchr(p, '/');
        const char *query_start = strchr(p, '?');
        const char *hash_start = strchr(p, '#');

        const char *host_end = path_start;
        if (!host_end || (query_start && query_start < host_end)) host_end = query_start;
        if (!host_end || (hash_start && hash_start < host_end)) host_end = hash_start;

        if (host_end) {
            size_t host_len = host_end - p;
            if (host_len >= sizeof(out->host)) host_len = sizeof(out->host) - 1;
            memcpy(out->host, p, host_len);
            out->host[host_len] = '\0';
        } else {
            strncpy(out->host, p, sizeof(out->host) - 1);
        }

        // Split host into hostname + port
        strncpy(out->hostname, out->host, sizeof(out->hostname) - 1);
        char *port_ptr = strchr(out->hostname, ':');
        if (port_ptr) {
            *port_ptr = '\0';
            out->port = atoi(port_ptr + 1);
        }

        if (path_start) {
            const char *path_end = query_start;
            if (!path_end) path_end = hash_start;
            if (!path_end) path_end = path_start + strlen(path_start);
            size_t path_len = path_end - path_start;
            if (path_len >= sizeof(out->pathname)) path_len = sizeof(out->pathname) - 1;
            memcpy(out->pathname, path_start, path_len);
            out->pathname[path_len] = '\0';
        }

        if (query_start) {
            const char *search_end = hash_start;
            if (!search_end) search_end = query_start + strlen(query_start);
            size_t search_len = search_end - query_start;
            if (search_len >= sizeof(out->search)) search_len = sizeof(out->search) - 1;
            memcpy(out->search, query_start, search_len);
            out->search[search_len] = '\0';
        }

        if (hash_start) {
            strncpy(out->hash, hash_start, sizeof(out->hash) - 1);
        }
    } else if (url) {
        // No protocol: treat whole thing as pathname/search/hash
        const char *query_start = strchr(url, '?');
        const char *hash_start = strchr(url, '#');
        const char *path_end = query_start;
        if (!path_end) path_end = hash_start;
        if (!path_end) path_end = url + strlen(url);
        size_t path_len = path_end - url;
        if (path_len >= sizeof(out->pathname)) path_len = sizeof(out->pathname) - 1;
        memcpy(out->pathname, url, path_len);
        out->pathname[path_len] = '\0';
        if (query_start) {
            const char *search_end = hash_start;
            if (!search_end) search_end = query_start + strlen(query_start);
            size_t search_len = search_end - query_start;
            if (search_len >= sizeof(out->search)) search_len = sizeof(out->search) - 1;
            memcpy(out->search, query_start, search_len);
            out->search[search_len] = '\0';
        }
        if (hash_start) {
            strncpy(out->hash, hash_start, sizeof(out->hash) - 1);
        }
    }

    if (out->protocol[0]) {
        snprintf(out->origin, sizeof(out->origin), "%s://%s", out->protocol, out->hostname);
    }

    // Reconstruct href
    snprintf(out->href, sizeof(out->href), "%s%s%s%s%s%s",
             out->protocol[0] ? out->protocol : "",
             out->protocol[0] ? "://" : "",
             out->host[0] ? out->host : "",
             out->pathname,
             out->search,
             out->hash);
}

static void url_resolve(const char *url, const char *base, UrlComponents *out) {
    UrlComponents base_comp;
    url_parse_components(base ? base : "", &base_comp);

    if (strstr(url, "://")) {
        url_parse_components(url, out);
        return;
    }

    if (strncmp(url, "//", 2) == 0) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s:%s", base_comp.protocol, url);
        url_parse_components(tmp, out);
        return;
    }

    strncpy(out->protocol, base_comp.protocol, sizeof(out->protocol) - 1);
    strncpy(out->hostname, base_comp.hostname, sizeof(out->hostname) - 1);
    strncpy(out->host, base_comp.host, sizeof(out->host) - 1);
    out->port = base_comp.port;
    strncpy(out->origin, base_comp.origin, sizeof(out->origin) - 1);

    if (url[0] == '/') {
        // Absolute path
        const char *query_start = strchr(url, '?');
        const char *hash_start = strchr(url, '#');
        const char *path_end = query_start;
        if (!path_end) path_end = hash_start;
        if (!path_end) path_end = url + strlen(url);
        size_t path_len = path_end - url;
        if (path_len >= sizeof(out->pathname)) path_len = sizeof(out->pathname) - 1;
        memcpy(out->pathname, url, path_len);
        out->pathname[path_len] = '\0';
        if (query_start) {
            const char *search_end = hash_start;
            if (!search_end) search_end = query_start + strlen(query_start);
            size_t search_len = search_end - query_start;
            if (search_len >= sizeof(out->search)) search_len = sizeof(out->search) - 1;
            memcpy(out->search, query_start, search_len);
            out->search[search_len] = '\0';
        } else {
            out->search[0] = '\0';
        }
        if (hash_start) {
            strncpy(out->hash, hash_start, sizeof(out->hash) - 1);
        } else {
            out->hash[0] = '\0';
        }
    } else if (url[0] == '?') {
        // Replace search only
        strncpy(out->pathname, base_comp.pathname, sizeof(out->pathname) - 1);
        const char *hash_start = strchr(url, '#');
        size_t search_len = hash_start ? (size_t)(hash_start - url) : strlen(url);
        if (search_len >= sizeof(out->search)) search_len = sizeof(out->search) - 1;
        memcpy(out->search, url, search_len);
        out->search[search_len] = '\0';
        if (hash_start) {
            strncpy(out->hash, hash_start, sizeof(out->hash) - 1);
        } else {
            out->hash[0] = '\0';
        }
    } else if (url[0] == '#') {
        strncpy(out->pathname, base_comp.pathname, sizeof(out->pathname) - 1);
        strncpy(out->search, base_comp.search, sizeof(out->search) - 1);
        strncpy(out->hash, url, sizeof(out->hash) - 1);
    } else {
        // Relative path: replace last path segment of base
        char base_path[512];
        strncpy(base_path, base_comp.pathname, sizeof(base_path) - 1);
        char *last_slash = strrchr(base_path, '/');
        if (last_slash) {
            *(last_slash + 1) = '\0';
        } else {
            strcpy(base_path, "/");
        }
        // Append relative url (strip its own query/hash and append separately)
        const char *query_start = strchr(url, '?');
        const char *hash_start = strchr(url, '#');
        const char *rel_end = query_start;
        if (!rel_end) rel_end = hash_start;
        if (!rel_end) rel_end = url + strlen(url);

        char combined[512];
        snprintf(combined, sizeof(combined), "%s%.*s", base_path, (int)(rel_end - url), url);
        // Very simple normalization for "/./" and "/../"
        char *p;
        while ((p = strstr(combined, "/./")) != NULL) {
            memmove(p, p + 2, strlen(p + 2) + 1);
        }
        while ((p = strstr(combined, "/../")) != NULL) {
            char *start = combined;
            if (p > start) {
                char *prev = p - 1;
                while (prev > start && *prev != '/') prev--;
                if (prev > start) {
                    memmove(prev, p + 3, strlen(p + 3) + 1);
                    continue;
                }
            }
            memmove(p, p + 3, strlen(p + 3) + 1);
        }
        strncpy(out->pathname, combined, sizeof(out->pathname) - 1);

        if (query_start) {
            const char *search_end = hash_start;
            if (!search_end) search_end = query_start + strlen(query_start);
            size_t search_len = search_end - query_start;
            if (search_len >= sizeof(out->search)) search_len = sizeof(out->search) - 1;
            memcpy(out->search, query_start, search_len);
            out->search[search_len] = '\0';
        } else {
            out->search[0] = '\0';
        }
        if (hash_start) {
            strncpy(out->hash, hash_start, sizeof(out->hash) - 1);
        } else {
            out->hash[0] = '\0';
        }
    }

    snprintf(out->href, sizeof(out->href), "%s%s%s%s%s%s",
             out->protocol[0] ? out->protocol : "",
             out->protocol[0] ? "://" : "",
             out->host[0] ? out->host : "",
             out->pathname,
             out->search,
             out->hash);
}

static void url_apply_components(JSContextHandle ctx, GCValue url_obj, const UrlComponents *comp) {
    JS_SetPropertyStr(ctx, url_obj, "href", JS_NewString(ctx, comp->href));
    JS_SetPropertyStr(ctx, url_obj, "protocol", JS_NewString(ctx, comp->protocol));
    JS_SetPropertyStr(ctx, url_obj, "hostname", JS_NewString(ctx, comp->hostname));
    JS_SetPropertyStr(ctx, url_obj, "host", JS_NewString(ctx, comp->host));
    JS_SetPropertyStr(ctx, url_obj, "pathname", JS_NewString(ctx, comp->pathname));
    JS_SetPropertyStr(ctx, url_obj, "search", JS_NewString(ctx, comp->search));
    JS_SetPropertyStr(ctx, url_obj, "hash", JS_NewString(ctx, comp->hash));
    JS_SetPropertyStr(ctx, url_obj, "port", JS_NewInt32(ctx, comp->port));
    JS_SetPropertyStr(ctx, url_obj, "origin", JS_NewString(ctx, comp->origin));
}

// URL.createObjectURL() - Creates blob URLs and captures them
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
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object(obj);
    if (ms.valid()) {
        is_media_source = true;
        ms.set_ready_state(1); // open
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

// URL constructor - new URL(url, base)
GCValue js_url_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "URL constructor requires at least 1 argument");
    }

    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) return JS_ThrowTypeError(ctx, "Invalid URL");

    const char *base_str = NULL;
    if (argc > 1) {
        base_str = JS_ToCString(ctx, argv[1]);
    }

    UrlComponents comp;
    if (base_str && base_str[0] && !strstr(url_str, "://")) {
        url_resolve(url_str, base_str, &comp);
    } else {
        url_parse_components(url_str, &comp);
    }

    GCValue url_obj = JS_NewObject(ctx);
    url_apply_components(ctx, url_obj, &comp);

    // searchParams getter is added via prototype in browser_api_impl.cpp
    return url_obj;
}

// ============================================================================
// URLSearchParams API
// ============================================================================

static GCValue usp_create(JSContextHandle ctx, const char *search) {
    GCValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "__params", JS_NewArray(ctx));
    if (search && search[0]) {
        const char *p = (*search == '?') ? search + 1 : search;
        char buf[2048];
        strncpy(buf, p, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *pair = strtok(buf, "&");
        while (pair) {
            char *eq = strchr(pair, '=');
            GCValue item = JS_NewArray(ctx);
            if (eq) {
                *eq = '\0';
                JS_SetPropertyUint32(ctx, item, 0, JS_NewString(ctx, pair));
                JS_SetPropertyUint32(ctx, item, 1, JS_NewString(ctx, eq + 1));
            } else {
                JS_SetPropertyUint32(ctx, item, 0, JS_NewString(ctx, pair));
                JS_SetPropertyUint32(ctx, item, 1, JS_NewString(ctx, ""));
            }
            GCValue arr = JS_GetPropertyStr(ctx, obj, "__params");
            int32_t len = 0;
            GCValue len_val = JS_GetPropertyStr(ctx, arr, "length");
            JS_ToInt32(ctx, &len, len_val);
            JS_SetPropertyUint32(ctx, arr, len, item);
            pair = strtok(NULL, "&");
        }
    }
    return obj;
}

static GCValue usp_get_params(JSContextHandle ctx, GCValue obj) {
    return JS_GetPropertyStr(ctx, obj, "__params");
}

static int32_t usp_length(JSContextHandle ctx, GCValue arr) {
    GCValue len_val = JS_GetPropertyStr(ctx, arr, "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, len_val);
    return len;
}

static GCValue usp_get_item(JSContextHandle ctx, GCValue arr, int32_t idx) {
    return JS_GetPropertyUint32(ctx, arr, idx);
}

static const char* usp_get_item_key(JSContextHandle ctx, GCValue item) {
    GCValue v = JS_GetPropertyUint32(ctx, item, 0);
    const char *s = JS_ToCString(ctx, v);
    return s;
}

static const char* usp_get_item_value(JSContextHandle ctx, GCValue item) {
    GCValue v = JS_GetPropertyUint32(ctx, item, 1);
    const char *s = JS_ToCString(ctx, v);
    return s;
}

GCValue js_url_search_params_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    const char *search = "";
    if (argc > 0) {
        if (JS_IsString(argv[0])) {
            search = JS_ToCString(ctx, argv[0]);
        } else if (JS_IsObject(argv[0])) {
            GCValue arr = JS_NewArray(ctx);
            JSPropertyEnum *props = NULL;
            uint32_t prop_count = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, argv[0],
                                       JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < prop_count; i++) {
                    const char *name = JS_AtomToCString(ctx, props[i].atom);
                    if (!name) continue;
                    GCValue val = JS_GetProperty(ctx, argv[0], props[i].atom);
                    const char *val_str = JS_ToCString(ctx, val);
                    GCValue item = JS_NewArray(ctx);
                    JS_SetPropertyUint32(ctx, item, 0, JS_NewString(ctx, name));
                    JS_SetPropertyUint32(ctx, item, 1, JS_NewString(ctx, val_str ? val_str : ""));
                    JS_SetPropertyUint32(ctx, arr, i, item);
                }
                JS_FreePropertyEnum(ctx, props, prop_count);
            }
            GCValue obj = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, obj, "__params", arr);
            return obj;
        }
    }
    return usp_create(ctx, search ? search : "");
}

GCValue js_url_search_params_append(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    const char *key = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!key || !value) return JS_UNDEFINED;
    GCValue arr = usp_get_params(ctx, this_val);
    int32_t len = usp_length(ctx, arr);
    GCValue item = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, item, 0, JS_NewString(ctx, key));
    JS_SetPropertyUint32(ctx, item, 1, JS_NewString(ctx, value));
    JS_SetPropertyUint32(ctx, arr, len, item);
    return JS_UNDEFINED;
}

GCValue js_url_search_params_delete(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_UNDEFINED;
    GCValue arr = usp_get_params(ctx, this_val);
    int32_t len = usp_length(ctx, arr);
    int32_t write_idx = 0;
    for (int32_t i = 0; i < len; i++) {
        GCValue item = usp_get_item(ctx, arr, i);
        const char *k = usp_get_item_key(ctx, item);
        if (!k || strcmp(k, key) != 0) {
            if (i != write_idx) {
                JS_SetPropertyUint32(ctx, arr, write_idx, item);
            }
            write_idx++;
        }
    }
    // truncate
    for (int32_t i = write_idx; i < len; i++) {
        JSAtom idx_atom = JS_NewAtomUInt32(ctx, (uint32_t)i);
        JS_DeleteProperty(ctx, arr, idx_atom, 0);
        JS_FreeAtom(ctx, idx_atom);
    }
    return JS_UNDEFINED;
}

GCValue js_url_search_params_get(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_NULL;
    GCValue arr = usp_get_params(ctx, this_val);
    int32_t len = usp_length(ctx, arr);
    GCValue result = JS_NULL;
    for (int32_t i = 0; i < len; i++) {
        GCValue item = usp_get_item(ctx, arr, i);
        const char *k = usp_get_item_key(ctx, item);
        if (k && strcmp(k, key) == 0) {
            const char *v = usp_get_item_value(ctx, item);
            result = JS_NewString(ctx, v ? v : "");
            break;
        }
    }
    return result;
}

GCValue js_url_search_params_get_all(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NewArray(ctx);
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_NewArray(ctx);
    GCValue out = JS_NewArray(ctx);
    GCValue arr = usp_get_params(ctx, this_val);
    int32_t len = usp_length(ctx, arr);
    int32_t out_idx = 0;
    for (int32_t i = 0; i < len; i++) {
        GCValue item = usp_get_item(ctx, arr, i);
        const char *k = usp_get_item_key(ctx, item);
        if (k && strcmp(k, key) == 0) {
            const char *v = usp_get_item_value(ctx, item);
            JS_SetPropertyUint32(ctx, out, out_idx++, JS_NewString(ctx, v ? v : ""));
        }
    }
    return out;
}

GCValue js_url_search_params_has(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_FALSE;
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_FALSE;
    GCValue arr = usp_get_params(ctx, this_val);
    int32_t len = usp_length(ctx, arr);
    bool found = false;
    for (int32_t i = 0; i < len; i++) {
        GCValue item = usp_get_item(ctx, arr, i);
        const char *k = usp_get_item_key(ctx, item);
        if (k && strcmp(k, key) == 0) found = true;
        if (found) break;
    }
    return JS_NewBool(ctx, found);
}

GCValue js_url_search_params_set(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    const char *key = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!key || !value) return JS_UNDEFINED;
    // Delete all existing entries for key
    js_url_search_params_delete(ctx, this_val, 1, argv);
    // Append new value
    js_url_search_params_append(ctx, this_val, argc, argv);
    return JS_UNDEFINED;
}

GCValue js_url_search_params_to_string(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    GCValue arr = usp_get_params(ctx, this_val);
    int32_t len = usp_length(ctx, arr);
    if (len == 0) {
        return JS_NewString(ctx, "");
    }
    char buf[4096];
    buf[0] = '\0';
    size_t pos = 0;
    for (int32_t i = 0; i < len; i++) {
        GCValue item = usp_get_item(ctx, arr, i);
        const char *k = usp_get_item_key(ctx, item);
        const char *v = usp_get_item_value(ctx, item);
        size_t k_len = k ? strlen(k) : 0;
        size_t v_len = v ? strlen(v) : 0;
        if (pos + k_len + v_len + 4 >= sizeof(buf)) {
            break;
        }
        if (pos > 0) {
            buf[pos++] = '&';
        }
        memcpy(buf + pos, k, k_len); pos += k_len;
        buf[pos++] = '=';
        memcpy(buf + pos, v, v_len); pos += v_len;
        buf[pos] = '\0';
    }
    return JS_NewString(ctx, buf);
}

static GCValue usp_build_array(JSContextHandle ctx, GCValue this_val, int which) {
    // which: 0=entries, 1=keys, 2=values
    GCValue out = JS_NewArray(ctx);
    GCValue arr = usp_get_params(ctx, this_val);
    int32_t len = usp_length(ctx, arr);
    int32_t out_idx = 0;
    for (int32_t i = 0; i < len; i++) {
        GCValue item = usp_get_item(ctx, arr, i);
        if (which == 0) {
            JS_SetPropertyUint32(ctx, out, out_idx++, item);
        } else {
            GCValue v = JS_GetPropertyUint32(ctx, item, which == 1 ? 0 : 1);
            JS_SetPropertyUint32(ctx, out, out_idx++, v);
        }
    }
    return out;
}

GCValue js_url_search_params_entries(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return usp_build_array(ctx, this_val, 0);
}

GCValue js_url_search_params_keys(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return usp_build_array(ctx, this_val, 1);
}

GCValue js_url_search_params_values(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    return usp_build_array(ctx, this_val, 2);
}

// URL.prototype.searchParams getter
GCValue js_url_get_search_params(JSContextHandle ctx, GCValue this_val) {
    GCValue existing = JS_GetPropertyStr(ctx, this_val, "__searchParams");
    if (!JS_IsUndefined(existing) && !JS_IsNull(existing)) return existing;
    GCValue search_val = JS_GetPropertyStr(ctx, this_val, "search");
    const char *search = JS_ToCString(ctx, search_val);
    GCValue sp = usp_create(ctx, search ? search : "");
    JS_SetPropertyStr(ctx, this_val, "__searchParams", sp);
    return sp;
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
        // Copy __original_url so fetch() can decode base64 body through Request chains
        GCValue orig_url_val = JS_GetPropertyStr(ctx, argv[0], "__original_url");
        if (!JS_IsUndefined(orig_url_val)) {
            JS_SetPropertyStr(ctx, request_obj, "__original_url", orig_url_val);
        }
    } else {
        // URL string
        const char *url_str = JS_ToCString(ctx, argv[0]);
        if (url_str) {
            JS_SetPropertyStr(ctx, request_obj, "url", JS_NewString(ctx, url_str));
            // Store original URL so fetch can access it even if url getter is overridden
            JS_SetPropertyStr(ctx, request_obj, "__original_url", JS_NewString(ctx, url_str));
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

