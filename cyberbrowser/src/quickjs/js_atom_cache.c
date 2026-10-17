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
    (void)cache; (void)str; (void)len; (void)hash;
    /* Atom cache disabled: it stored raw pointers to input strings whose
     * lifetime was not guaranteed, causing use-after-free crashes when
     * parsing/executing large minified scripts (e.g. YouTube's kevlar_base).
     * Returning 0 falls back to the normal atom lookup path. */
    return 0;
}

void js_atom_cache_insert(JSAtomCache *cache, const char *str, size_t len,
                          uint32_t hash, JSAtom atom) {
    (void)cache; (void)str; (void)len; (void)hash; (void)atom;
    /* Atom cache disabled - see js_atom_cache_lookup. */
}
