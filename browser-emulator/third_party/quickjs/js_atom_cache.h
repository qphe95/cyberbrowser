/*
 * Fast atom cache for QuickJS
 * 
 * Uses open addressing with quadratic probing.
 * When table is full, evicts first item with matching hash bucket.
 * 
 * This provides O(1) average case lookup with minimal overhead.
 */
#ifndef JS_ATOM_CACHE_H
#define JS_ATOM_CACHE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration - JSAtom is uint32_t in QuickJS */
typedef uint32_t JSAtom;

#define JS_ATOM_CACHE_SIZE 2048  /* Must be power of 2 */
#define JS_ATOM_CACHE_MASK (JS_ATOM_CACHE_SIZE - 1)

typedef struct JSAtomCacheEntry {
    uint32_t hash;          /* Full 32-bit hash */
    JSAtom atom;            /* The atom value */
    const char *str;        /* Pointer to string data */
    uint32_t len;           /* String length */
    uint32_t timestamp;     /* For simple LRU eviction */
} JSAtomCacheEntry;

typedef struct JSAtomCache {
    JSAtomCacheEntry entries[JS_ATOM_CACHE_SIZE];
    uint32_t timestamp_counter;
    uint32_t hit_count;
    uint32_t miss_count;
    bool initialized;
} JSAtomCache;

/* Initialize atom cache */
void js_atom_cache_init(JSAtomCache *cache);

/* Reset/clear atom cache */
void js_atom_cache_reset(JSAtomCache *cache);

/* Lookup atom in cache. Returns 0 if not found. */
JSAtom js_atom_cache_lookup(JSAtomCache *cache, const char *str, 
                            size_t len, uint32_t hash);

/* Insert atom into cache */
void js_atom_cache_insert(JSAtomCache *cache, const char *str, size_t len,
                          uint32_t hash, JSAtom atom);

/* Get cache statistics */
static inline void js_atom_cache_stats(JSAtomCache *cache, 
                                       uint32_t *hits, uint32_t *misses) {
    *hits = cache->hit_count;
    *misses = cache->miss_count;
}

/* Clear statistics */
static inline void js_atom_cache_clear_stats(JSAtomCache *cache) {
    cache->hit_count = 0;
    cache->miss_count = 0;
}

/* Fast hash function (FNV-1a or similar) */
static inline uint32_t js_atom_hash(const char *str, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 16777619u;
    }
    return hash;
}

#ifdef __cplusplus
}
#endif

#endif /* JS_ATOM_CACHE_H */
