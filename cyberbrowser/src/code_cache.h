#ifndef CODE_CACHE_H
#define CODE_CACHE_H

/* code_cache.h - the bytecode cache.  A page script's compiled form is kept,
 * keyed by its source's SHA-256, and the next load runs it instead of
 * parsing the script again.
 *
 * Bytecode is written only for a full compile (JS_EVAL_FLAG_NO_LAZY): a
 * lazily compiled function can't be written.  So a script missing from the
 * cache runs as always (lazily), is noted, and code_cache_flush compiles it
 * fully and saves it later, when the page is idle.
 *
 * The embedder keeps the entries: the tab in its site's folder through the
 * storage service (cache/code/<key>), the harness in a folder (CYBER_CODE_CACHE).
 * An entry is checked (its engine build, its source's whole SHA-256) before
 * its bytecode is read. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"

typedef struct CodeCacheStore {
    /* The entry for key (32 hex digits) into *data (malloc'd); false if none. */
    bool (*load)(const char *key, uint8_t **data, size_t *len);
    void (*save)(const char *key, const uint8_t *data, size_t len);
} CodeCacheStore;

/* NULL: no cache (the default). */
void code_cache_set_store(const CodeCacheStore *store);

/* The cached compile of a classic script, or JS_UNDEFINED (then the script
 * is noted for code_cache_flush).  The caller runs it with JS_EvalFunction. */
GCValue code_cache_lookup(JSContextHandle ctx, const char *src, size_t len);

/* Compile and save up to max noted scripts; true if some are left. */
bool code_cache_flush(JSContextHandle ctx, int max);

typedef struct CodeCacheStats {
    int hits, misses, saved, rejected;
    double read_ms, hash_ms;
} CodeCacheStats;
void code_cache_stats(CodeCacheStats *out);

#endif
