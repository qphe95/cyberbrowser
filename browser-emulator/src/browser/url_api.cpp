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

// URL.createObjectURL() - Creates blob URLs and captures them
GCValue js_url_create_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_ThrowTypeError(ctx, "requires at least 1 argument(s)");
    
    GCValue obj = argv[0];
    
    // Generate a unique blob URL
    static int blob_counter = 0;
    char blob_url[512];
    
    // Check if it's a MediaSource object
    MediaSourceDataHandle ms = MediaSourceDataHandle::from_object(obj);
    if (ms.valid()) {
        snprintf(blob_url, sizeof(blob_url), "blob:mediasource:%d", ++blob_counter);
        ms.set_ready_state(1); // open
        
        // Capture the URL
        capture_url_debug(blob_url, "url_create_object_url_ms");
        
        return JS_NewString(ctx, blob_url);
    }
    
    // For other objects (File, Blob), create generic blob URL
    snprintf(blob_url, sizeof(blob_url), "blob:generic:%d", ++blob_counter);
    
    // Capture the URL
    capture_url_debug(blob_url, "url_create_object_url_generic");
    
    return JS_NewString(ctx, blob_url);
}

// URL.revokeObjectURL()
GCValue js_url_revoke_object_url(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    // No-op for stub implementation
    return JS_UNDEFINED;
}

// URL constructor - new URL(url, base)
GCValue js_url_constructor(JSContextHandle ctx, GCValue new_target, int argc, GCValue *argv) {
    if (argc < 1) {
        return JS_ThrowTypeError(ctx, "URL constructor requires at least 1 argument");
    }
    
    const char *url_str = JS_ToCString(ctx, argv[0]);
    if (!url_str) return JS_ThrowTypeError(ctx, "Invalid URL");
    
    // For URL parsing, we'll create a simple object with URL components
    GCValue url_obj = JS_NewObject(ctx);
    
    // Store the full URL
    JS_SetPropertyStr(ctx, url_obj, "href", JS_NewString(ctx, url_str));
    
    // Simple URL parsing (very basic)
    char protocol[64] = "";
    char hostname[256] = "";
    char pathname[512] = "/";
    char search[512] = "";
    char hash[256] = "";
    int port = 0;
    
    const char *p = url_str;
    
    // Parse protocol
    const char *proto_end = strstr(p, "://");
    if (proto_end) {
        size_t proto_len = proto_end - p;
        if (proto_len < sizeof(protocol)) {
            strncpy(protocol, p, proto_len);
            protocol[proto_len] = '\0';
        }
        p = proto_end + 3;
        
        // Parse hostname and port
        const char *path_start = strchr(p, '/');
        const char *query_start = strchr(p, '?');
        const char *hash_start = strchr(p, '#');
        
        const char *host_end = path_start;
        if (!host_end || (query_start && query_start < host_end)) host_end = query_start;
        if (!host_end || (hash_start && hash_start < host_end)) host_end = hash_start;
        
        if (host_end) {
            size_t host_len = host_end - p;
            if (host_len >= sizeof(hostname)) host_len = sizeof(hostname) - 1;
            strncpy(hostname, p, host_len);
            hostname[host_len] = '\0';
            
            // Check for port in hostname
            char *port_ptr = strchr(hostname, ':');
            if (port_ptr) {
                *port_ptr = '\0';
                port = atoi(port_ptr + 1);
            }
        } else {
            strncpy(hostname, p, sizeof(hostname) - 1);
            hostname[sizeof(hostname) - 1] = '\0';
        }
        
        // Parse pathname
        if (path_start) {
            const char *path_end = query_start;
            if (!path_end) path_end = hash_start;
            if (!path_end) path_end = path_start + strlen(path_start);
            
            size_t path_len = path_end - path_start;
            if (path_len >= sizeof(pathname)) path_len = sizeof(pathname) - 1;
            strncpy(pathname, path_start, path_len);
            pathname[path_len] = '\0';
        }
        
        // Parse search
        if (query_start) {
            const char *search_end = hash_start;
            if (!search_end) search_end = query_start + strlen(query_start);
            
            size_t search_len = search_end - query_start;
            if (search_len >= sizeof(search)) search_len = sizeof(search) - 1;
            strncpy(search, query_start, search_len);
            search[search_len] = '\0';
        }
        
        // Parse hash
        if (hash_start) {
            strncpy(hash, hash_start, sizeof(hash) - 1);
            hash[sizeof(hash) - 1] = '\0';
        }
    }
    
    // Set URL properties
    JS_SetPropertyStr(ctx, url_obj, "protocol", JS_NewString(ctx, protocol));
    JS_SetPropertyStr(ctx, url_obj, "hostname", JS_NewString(ctx, hostname));
    JS_SetPropertyStr(ctx, url_obj, "host", JS_NewString(ctx, hostname));
    JS_SetPropertyStr(ctx, url_obj, "pathname", JS_NewString(ctx, pathname));
    JS_SetPropertyStr(ctx, url_obj, "search", JS_NewString(ctx, search));
    JS_SetPropertyStr(ctx, url_obj, "hash", JS_NewString(ctx, hash));
    JS_SetPropertyStr(ctx, url_obj, "port", JS_NewInt32(ctx, port));
    
    // origin = protocol + // + hostname
    char origin[512];
    if (strlen(protocol) > 0) {
        snprintf(origin, sizeof(origin), "%s://%s", protocol, hostname);
    } else {
        origin[0] = '\0';
    }
    JS_SetPropertyStr(ctx, url_obj, "origin", JS_NewString(ctx, origin));
    
    return url_obj;
}

// ============================================================================
// Request/Response API (for fetch)
// ============================================================================

// Request constructor
