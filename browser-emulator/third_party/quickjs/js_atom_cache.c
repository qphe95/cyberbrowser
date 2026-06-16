/*
 * Atom cache implementation
 * 
 * Uses quadratic probing for collision resolution.
 * When table is full, evicts the oldest item in the probe sequence.
 */
#include "js_atom_cache.h"
#include <string.h>
#include <assert.h>

void js_atom_cache_init(JSAtomCache *cache) {
    memset(cache, 0, sizeof(JSAtomCache));
    /* Mark all entries as empty (hash = 0, atom = 0) */
    for (int i = 0; i < JS_ATOM_CACHE_SIZE; i++) {
        cache->entries[i].hash = 0;
        cache->entries[i].atom = 0;
        cache->entries[i].timestamp = 0;
    }
    cache->timestamp_counter = 1;
    cache->initialized = true;
}

void js_atom_cache_reset(JSAtomCache *cache) {
    js_atom_cache_init(cache);
}

JSAtom js_atom_cache_lookup(JSAtomCache *cache, const char *str,
                            size_t len, uint32_t hash) {
    if (!cache->initialized) {
        js_atom_cache_init(cache);
    }
    
    uint32_t idx = hash & JS_ATOM_CACHE_MASK;
    
    /* Quadratic probing: 0, 1, 4, 9, 16, 25, ... */
    for (int probe = 0; probe < JS_ATOM_CACHE_SIZE; probe++) {
        uint32_t i = (idx + probe * probe) & JS_ATOM_CACHE_MASK;
        JSAtomCacheEntry *entry = &cache->entries[i];
        
        /* Empty slot - not found */
        if (entry->atom == 0) {
            cache->miss_count++;
            return 0;
        }
        
        /* Check hash first (fast path) */
        if (entry->hash != hash) {
            continue;
        }
        
        /* Hash matches, check length and string content */
        if (entry->len == len && memcmp(entry->str, str, len) == 0) {
            /* Cache hit! Update timestamp for LRU */
            entry->timestamp = cache->timestamp_counter++;
            cache->hit_count++;
            return entry->atom;
        }
    }
    
    /* Table is full and item not found */
    cache->miss_count++;
    return 0;
}

void js_atom_cache_insert(JSAtomCache *cache, const char *str, size_t len,
                          uint32_t hash, JSAtom atom) {
    if (!cache->initialized) {
        js_atom_cache_init(cache);
    }
    
    assert(atom != 0);
    
    uint32_t idx = hash & JS_ATOM_CACHE_MASK;
    uint32_t oldest_timestamp = 0xFFFFFFFF;
    uint32_t oldest_idx = 0;
    bool found_empty = false;
    
    /* Quadratic probing to find slot */
    for (int probe = 0; probe < JS_ATOM_CACHE_SIZE; probe++) {
        uint32_t i = (idx + probe * probe) & JS_ATOM_CACHE_MASK;
        JSAtomCacheEntry *entry = &cache->entries[i];
        
        /* Found empty slot */
        if (entry->atom == 0) {
            idx = i;
            found_empty = true;
            break;
        }
        
        /* Same hash - replace (shouldn't happen often) */
        if (entry->hash == hash && entry->len == len &&
            memcmp(entry->str, str, len) == 0) {
            /* Same string, update atom */
            entry->atom = atom;
            entry->timestamp = cache->timestamp_counter++;
            return;
        }
        
        /* Track oldest entry in probe sequence for eviction */
        if (entry->timestamp < oldest_timestamp) {
            oldest_timestamp = entry->timestamp;
            oldest_idx = i;
        }
    }
    
    /* If no empty slot found, evict oldest in probe sequence */
    if (!found_empty) {
        idx = oldest_idx;
    }
    
    /* Insert new entry */
    JSAtomCacheEntry *entry = &cache->entries[idx];
    entry->hash = hash;
    entry->atom = atom;
    entry->str = str;
    entry->len = (uint32_t)len;
    entry->timestamp = cache->timestamp_counter++;
}
