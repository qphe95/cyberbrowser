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
#include "url_whatwg.h"

/* ============================================================================
 * Storage API Implementation (localStorage/sessionStorage)
 * ============================================================================ */

/* Each origin's stores: frames of other origins share the page's process
 * (frame_api.c), and see only their own origin's. */
typedef struct OriginStores {
    char *origin;
    StorageData local, session;
    bool loaded, changed;               /* localStorage: loaded from the backend; changed since saved */
} OriginStores;

static OriginStores *g_stores;
static int g_nstores, g_cap_stores;

static const StorageBackend *g_backend;

void storage_set_backend(const StorageBackend *backend) {
    g_backend = backend;
}

static OriginStores *stores_of(const char *origin) {
    for (int i = 0; i < g_nstores; i++)
        if (!strcmp(g_stores[i].origin, origin)) return &g_stores[i];
    if (g_nstores == g_cap_stores) {
        int ncap = g_cap_stores ? g_cap_stores * 2 : 4;
        OriginStores *n = realloc(g_stores, (size_t)ncap * sizeof(*n));
        if (!n) return NULL;
        g_stores = n;
        g_cap_stores = ncap;
    }
    OriginStores *o = &g_stores[g_nstores];
    memset(o, 0, sizeof(*o));
    o->origin = strdup(origin);
    if (!o->origin) return NULL;
    g_nstores++;
    return o;
}

/* The store as [u32 key length][key][u32 value length][value]... */
static void local_load(OriginStores *o) {
    o->loaded = true;
    char *data = NULL;
    size_t len = 0;
    /* An opaque origin's storage lives in memory only. */
    if (!g_backend || !strcmp(o->origin, "null") || !g_backend->load(o->origin, &data, &len)) return;
    StorageData *st = &o->local;
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

bool storage_local_take_changes(char **origin, char **data, size_t *len) {
    for (int i = 0; i < g_nstores; i++) {
        OriginStores *o = &g_stores[i];
        if (!o->changed || !strcmp(o->origin, "null")) continue;
        StorageData *st = &o->local;
        size_t n = 0;
        for (int k = 0; k < st->count; k++) n += 8 + st->items[k].key_len + st->items[k].value_len;
        char *out = malloc(n ? n : 1);
        char *org = strdup(o->origin);
        if (!out || !org) { free(out); free(org); return false; }
        size_t p = 0;
        for (int k = 0; k < st->count; k++) {
            uint32_t kl = (uint32_t)st->items[k].key_len, vl = (uint32_t)st->items[k].value_len;
            memcpy(out + p, &kl, 4); memcpy(out + p + 4, st->items[k].key, kl); p += 4 + kl;
            memcpy(out + p, &vl, 4); memcpy(out + p + 4, st->items[k].value, vl); p += 4 + vl;
        }
        o->changed = false;
        *origin = org;
        *data = out;
        *len = n;
        return true;
    }
    return false;
}

/* The origin of the realm ctx (its document's location), malloc'd. */
static char *realm_origin(JSContextHandle ctx) {
    GCValue loc = JS_GetPropertyStr(ctx, JS_GetGlobalObject(ctx), "location");
    GCValue href = JS_IsObject(loc) ? JS_GetPropertyStr(ctx, loc, "href") : JS_UNDEFINED;
    const char *h = JS_IsString(href) ? JS_ToCString(ctx, href) : NULL;
    char *out = NULL;
    WUrl u;
    if (h && wurl_parse(h, strlen(h), NULL, &u) == 0) {
        out = wurl_origin(&u);
        wurl_free(&u);
    }
    if (h) JS_FreeCString(ctx, h);
    return out ? out : strdup("null");
}

/* The store a Storage object (of realm ctx) stands for, and whether it is
 * localStorage; localStorage is loaded on first use. */
static StorageData *get_storage_data_in(JSContextHandle ctx, GCValue obj, OriginStores **of) {
    char *origin = realm_origin(ctx);
    OriginStores *o = origin ? stores_of(origin) : NULL;
    free(origin);
    if (!o) return NULL;
    GCValue global = JS_GetGlobalObject(ctx);
    bool session = JS_StrictEq(ctx, obj, JS_GetPropertyStr(ctx, global, "sessionStorage"));
    if (of) *of = o;
    if (session) return &o->session;
    if (!o->loaded) local_load(o);
    return &o->local;
}

static StorageData* get_storage_data(JSContextHandle ctx, GCValue obj) {
    return get_storage_data_in(ctx, obj, NULL);
}

static void changed(StorageData *st) {
    for (int i = 0; i < g_nstores; i++)
        if (st == &g_stores[i].local) g_stores[i].changed = true;
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

