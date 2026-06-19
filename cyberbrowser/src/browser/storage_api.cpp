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

/* ============================================================================
 * Storage API Implementation (localStorage/sessionStorage)
 * ============================================================================ */

// Static storage data for localStorage and sessionStorage
static StorageData g_local_storage_data = {0};
static StorageData g_session_storage_data = {0};

// Helper to identify which storage type based on object
static StorageData* get_storage_data(JSContextHandle ctx, GCValue obj) {
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
    for (int i = 0; i < storage->count && i < STORAGE_MAX_ITEMS; i++) {
        if (storage->items[i].used && strcmp(storage->items[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

// Helper to find first empty slot in storage
int storage_find_empty_slot(StorageData *storage) {
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        if (!storage->items[i].used) {
            return i;
        }
    }
    return -1;
}

// Storage.getItem(key)
GCValue js_storage_get_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_NULL;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_NULL;
    
    int idx = storage_find_item(storage, key);
    if (idx >= 0) {
        return JS_NewString(ctx, storage->items[idx].value);
    }
    return JS_NULL;
}

// Storage.setItem(key, value)
GCValue js_storage_set_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 2) return JS_UNDEFINED;
    
    const char *key = JS_ToCString(ctx, argv[0]);
    const char *value = JS_ToCString(ctx, argv[1]);
    if (!key || !value) return JS_UNDEFINED;
    
    // Check key length
    size_t key_len = strlen(key);
    if (key_len >= STORAGE_MAX_KEY_LEN) {
        platform_log(LOG_LEVEL_ERROR, "Storage", "FATAL: Storage key length %zu exceeds maximum %d. Aborting.", key_len, STORAGE_MAX_KEY_LEN - 1);
        abort();
    }
    
    // Check value length
    size_t value_len = strlen(value);
    if (value_len >= STORAGE_MAX_VALUE_LEN) {
        platform_log(LOG_LEVEL_ERROR, "Storage", "FATAL: Storage value length %zu exceeds maximum %d. Aborting.", value_len, STORAGE_MAX_VALUE_LEN - 1);
        abort();
    }
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_UNDEFINED;
    
    // Check if key already exists
    int idx = storage_find_item(storage, key);
    if (idx >= 0) {
        // Update existing
        strncpy(storage->items[idx].value, value, STORAGE_MAX_VALUE_LEN - 1);
        storage->items[idx].value[STORAGE_MAX_VALUE_LEN - 1] = '\0';
    } else {
        // Find empty slot
        idx = storage_find_empty_slot(storage);
        if (idx < 0) {
            // Storage full - abort the program
            platform_log(LOG_LEVEL_ERROR, "Storage", "FATAL: Storage limit of %d items exceeded. Aborting.", STORAGE_MAX_ITEMS);
            abort();
        }
        // Add new item
        strncpy(storage->items[idx].key, key, STORAGE_MAX_KEY_LEN - 1);
        storage->items[idx].key[STORAGE_MAX_KEY_LEN - 1] = '\0';
        strncpy(storage->items[idx].value, value, STORAGE_MAX_VALUE_LEN - 1);
        storage->items[idx].value[STORAGE_MAX_VALUE_LEN - 1] = '\0';
        storage->items[idx].used = 1;
        storage->count++;
    }
    
    return JS_UNDEFINED;
}

// Storage.removeItem(key)
GCValue js_storage_remove_item(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_UNDEFINED;
    
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_UNDEFINED;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_UNDEFINED;
    
    int idx = storage_find_item(storage, key);
    if (idx >= 0) {
        storage->items[idx].used = 0;
        storage->count--;
    }
    
    return JS_UNDEFINED;
}

// Storage.clear()
GCValue js_storage_clear(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_UNDEFINED;
    
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        storage->items[i].used = 0;
    }
    storage->count = 0;
    
    return JS_UNDEFINED;
}

// Storage.key(index)
GCValue js_storage_key(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    if (argc < 1) return JS_NULL;
    
    int index;
    if (JS_IsNumber(argv[0])) {
        JS_ToInt32(ctx, &index, argv[0]);
    } else {
        return JS_NULL;
    }
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_NULL;
    
    // Find the nth used slot
    int current = 0;
    for (int i = 0; i < STORAGE_MAX_ITEMS; i++) {
        if (storage->items[i].used) {
            if (current == index) {
                return JS_NewString(ctx, storage->items[i].key);
            }
            current++;
        }
    }
    return JS_NULL;
}

// Storage.length getter
GCValue js_storage_get_length(JSContextHandle ctx, GCValue this_val, int argc, GCValue *argv) {
    (void)argc; (void)argv;
    
    StorageData *storage = get_storage_data(ctx, this_val);
    if (!storage) return JS_NewInt32(ctx, 0);
    
    return JS_NewInt32(ctx, storage->count);
}

