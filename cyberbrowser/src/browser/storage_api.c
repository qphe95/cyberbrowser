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
#include "storage_api.h"

/* ============================================================================
 * Storage API Implementation (localStorage/sessionStorage)
 * ============================================================================ */

// Static storage data for localStorage and sessionStorage
static StorageData g_local_storage_data = {0};
static StorageData g_session_storage_data = {0};

static const StorageBackend *g_backend;
static bool g_local_loaded, g_local_changed;

void storage_set_backend(const StorageBackend *backend) {
    g_backend = backend;
}

/* The store as [u32 key length][key][u32 value length][value]... */
static void local_load(void) {
    g_local_loaded = true;
    char *data = NULL;
    size_t len = 0;
    if (!g_backend || !g_backend->load(&data, &len)) return;
    StorageData *st = &g_local_storage_data;
    size_t p = 0;
    while (p + 4 <= len) {
        uint32_t kl, vl;
        memcpy(&kl, data + p, 4);
        if (kl > len - p - 4) break;
        const char *k = data + p + 4;
        p += 4 + (size_t)kl;
        if (p + 4 > len) break;
        memcpy(&vl, data + p, 4);
        if (vl > len - p - 4) break;
        const char *v = data + p + 4;
        p += 4 + (size_t)vl;
        if (st->bytes + kl + vl > STORAGE_QUOTA_BYTES) break;
        if (st->count == st->cap) {
            int ncap = st->cap ? st->cap * 2 : 16;
            StorageItem *ni = realloc(st->items, (size_t)ncap * sizeof(StorageItem));
            if (!ni) break;
            st->items = ni;
            st->cap = ncap;
        }
        StorageItem *it = &st->items[st->count];
        it->key = malloc((size_t)kl + 1);
        it->value = malloc((size_t)vl + 1);
        if (!it->key || !it->value) { free(it->key); free(it->value); break; }
        memcpy(it->key, k, kl); it->key[kl] = '\0'; it->key_len = kl;
        memcpy(it->value, v, vl); it->value[vl] = '\0'; it->value_len = vl;
        st->count++;
        st->bytes += kl + vl;
    }
    free(data);
}

bool storage_local_take_changes(char **data, size_t *len) {
    if (!g_local_changed) return false;
    StorageData *st = &g_local_storage_data;
    size_t n = 0;
    for (int i = 0; i < st->count; i++) n += 8 + st->items[i].key_len + st->items[i].value_len;
    char *out = malloc(n ? n : 1);
    if (!out) return false;
    size_t p = 0;
    for (int i = 0; i < st->count; i++) {
        uint32_t kl = (uint32_t)st->items[i].key_len, vl = (uint32_t)st->items[i].value_len;
        memcpy(out + p, &kl, 4); memcpy(out + p + 4, st->items[i].key, kl); p += 4 + kl;
        memcpy(out + p, &vl, 4); memcpy(out + p + 4, st->items[i].value, vl); p += 4 + vl;
    }
    g_local_changed = false;
    *data = out;
    *len = n;
    return true;
}

static StorageData* get_storage_data_of(JSContextHandle ctx, GCValue obj);

/* The store of a Storage object; localStorage is loaded on first use. */
static StorageData* get_storage_data(JSContextHandle ctx, GCValue obj) {
    StorageData *st = get_storage_data_of(ctx, obj);
    if (st == &g_local_storage_data && !g_local_loaded) local_load();
    return st;
}

static void changed(StorageData *st) {
    if (st == &g_local_storage_data) g_local_changed = true;
}

// Helper to identify which storage type based on object
static StorageData* get_storage_data_of(JSContextHandle ctx, GCValue obj) {
    // Compare object with known localStorage/sessionStorage globals
    GCValue global = JS_GetGlobalObject(ctx);
    GCValue localStorageGlob = JS_GetPropertyStr(ctx, global, "localStorage");
    GCValue sessionStorageGlob = JS_GetPropertyStr(ctx, global, "sessionStorage");
    
    // Check if obj is the same as global localStorage
    if (JS_StrictEq(ctx, obj, localStorageGlob)) {
        return &g_local_storage_data;
    }
    // Check if obj is the same as global sessionStorage
    if (JS_StrictEq(ctx, obj, sessionStorageGlob)) {
        return &g_session_storage_data;
    }
    
    // Also check for window.localStorage and window.sessionStorage pattern
    GCValue window = JS_GetPropertyStr(ctx, global, "window");
    if (!JS_IsUndefined(window) && !JS_IsNull(window)) {
        GCValue winLocalStorage = JS_GetPropertyStr(ctx, window, "localStorage");
        GCValue winSessionStorage = JS_GetPropertyStr(ctx, window, "sessionStorage");
        if (JS_StrictEq(ctx, obj, winLocalStorage)) {
            return &g_local_storage_data;
        }
        if (JS_StrictEq(ctx, obj, winSessionStorage)) {
            return &g_session_storage_data;
        }
    }
    
    // Default to localStorage if we can't identify
    return &g_local_storage_data;
}

// Helper to find item index by key in storage
int storage_find_item(StorageData *storage, const char *key) {
    for (int i = 0; i < storage->count; i++) {
        if (strcmp(storage->items[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static char *storage_dup(const char *s, size_t len) {
    char *d = malloc(len + 1);
    if (d) { memcpy(d, s, len); d[len] = '\0'; }
    return d;
}

// Storage.getItem(key)
GCValue js_storage_get_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_NULL;
    
    int idx = storage_find_item(storage, key);
    if (idx >= 0) {
        return JS_NewStringLen(ctx, storage->items[idx].value, storage->items[idx].value_len);
    }
    return JS_NULL;
}

// Storage.setItem(key, value)
GCValue js_storage_set_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "Failed to execute 'setItem' on 'Storage': 2 arguments required");
    
    size_t key_len = 0, value_len = 0;
    const char *key = JS_ToCStringLen(ctx, &key_len, argv[0]);
    if (!key) return JS_EXCEPTION;
    const char *value = JS_ToCStringLen(ctx, &value_len, argv[1]);
    if (!value) return JS_EXCEPTION;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_UNDEFINED;
    
    int idx = storage_find_item(storage, key);
    size_t old = idx >= 0 ? storage->items[idx].key_len + storage->items[idx].value_len : 0;
    if (storage->bytes - old + key_len + value_len > STORAGE_QUOTA_BYTES) {
        return throw_dom_exception(ctx, "QuotaExceededError",
            "Failed to execute 'setItem' on 'Storage': Setting the value exceeded the quota.");
    }
    char *v = storage_dup(value, value_len);
    if (!v) return JS_ThrowOutOfMemory(ctx);
    if (idx >= 0) {
        free(storage->items[idx].value);
        storage->items[idx].value = v;
        storage->items[idx].value_len = value_len;
    } else {
        char *k = storage_dup(key, key_len);
        if (!k) { free(v); return JS_ThrowOutOfMemory(ctx); }
        if (storage->count == storage->cap) {
            int ncap = storage->cap ? storage->cap * 2 : 16;
            StorageItem *ni = realloc(storage->items, (size_t)ncap * sizeof(StorageItem));
            if (!ni) { free(k); free(v); return JS_ThrowOutOfMemory(ctx); }
            storage->items = ni;
            storage->cap = ncap;
        }
        StorageItem *it = &storage->items[storage->count++];
        it->key = k; it->key_len = key_len;
        it->value = v; it->value_len = value_len;
    }
    storage->bytes = storage->bytes - old + key_len + value_len;
    changed(storage);
    
    return JS_UNDEFINED;
}

// Storage.removeItem(key)
GCValue js_storage_remove_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_EXCEPTION;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_UNDEFINED;
    
    int idx = storage_find_item(storage, key);
    if (idx >= 0) {
        StorageItem *it = &storage->items[idx];
        storage->bytes -= it->key_len + it->value_len;
        free(it->key);
        free(it->value);
        memmove(it, it + 1, (size_t)(storage->count - idx - 1) * sizeof(StorageItem));
        storage->count--;
        changed(storage);
    }
    
    return JS_UNDEFINED;
}

// Storage.clear()
GCValue js_storage_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_UNDEFINED;
    
    for (int i = 0; i < storage->count; i++) {
        free(storage->items[i].key);
        free(storage->items[i].value);
    }
    storage->count = 0;
    storage->bytes = 0;
    changed(storage);
    
    return JS_UNDEFINED;
}

// Storage.key(index)
GCValue js_storage_key(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    uint32_t index;
    if (JS_ToUint32(ctx, &index, argv[0])) return JS_EXCEPTION;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_NULL;
    
    if (index < (uint32_t)storage->count)
        return JS_NewStringLen(ctx, storage->items[index].key, storage->items[index].key_len);
    return JS_NULL;
}

// Storage.length getter
GCValue js_storage_get_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_NewInt32(ctx, 0);
    
    return JS_NewInt32(ctx, storage->count);
}

