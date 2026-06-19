/*
 * Lock-free open-addressing hash table for GCHandle keys/values.
 *
 * Used by parallel CSS computed-style maps and, eventually, by the atom and
 * shape caches.  Keys are compared by handle equality, so callers must use
 * interned atoms (or otherwise stable keys).
 *
 * The table is flat: good cache locality, no per-bucket GC allocation.
 * Inserts use a three-state bucket (empty -> writing -> occupied) so readers
 * never observe a partially-written entry.
 */

#ifndef LOCKFREE_HASH_TABLE_H
#define LOCKFREE_HASH_TABLE_H

#include <stdint.h>
#include <stdbool.h>
#include "quickjs_gc_unified.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LFHashBucket {
    uint32_t hash;        /* pre-computed hash of the key */
    GCHandle key;         /* key handle (GC_HANDLE_NULL means empty) */
    GCHandle value;       /* value handle */
    uint32_t state;       /* LF_HASH_EMPTY/WRITING/OCCUPIED/TOMBSTONE */
} LFHashBucket;

typedef struct LFHashTable {
    uint32_t bucket_count;   /* power of two */
    uint32_t bucket_bits;    /* log2(bucket_count) */
    struct LFHashTable *next_retired; /* for safe memory reclamation */
    LFHashBucket buckets[1]; /* variable-length array */
} LFHashTable;

/* Bucket lifecycle states */
#define LF_HASH_EMPTY     0
#define LF_HASH_WRITING   1
#define LF_HASH_OCCUPIED  2
#define LF_HASH_TOMBSTONE 3

/* Create/destroy.  bucket_count is rounded up to a power of two. */
LFHashTable *lf_hash_create(uint32_t bucket_count);
void         lf_hash_destroy(LFHashTable *t);

/* Insert or update.  Returns false if the table is full. */
bool lf_hash_insert(LFHashTable *t, uint32_t hash, GCHandle key, GCHandle value);

/* Lookup.  Returns GC_HANDLE_NULL if not found. */
GCHandle lf_hash_lookup(LFHashTable *t, uint32_t hash, GCHandle key);

/*
 * Extended lookup/remove with caller-supplied key equality.
 * The callback receives the stored key/handle, the bucket hash, and the
 * caller's lookup_key.  This lets callers compare by content (e.g. shape
 * proto + properties) rather than by handle identity.
 */
typedef bool (*LFHashEqFunc)(GCHandle key, uint32_t hash, void *lookup_key, void *user_data);

GCHandle lf_hash_lookup_ex(LFHashTable *t, uint32_t hash, void *lookup_key,
                           LFHashEqFunc eq, void *user_data);
bool     lf_hash_remove_ex(LFHashTable *t, uint32_t hash, void *lookup_key,
                           LFHashEqFunc eq, void *user_data);

/* Remove a key.  Returns true if the key was present. */
bool lf_hash_remove(LFHashTable *t, uint32_t hash, GCHandle key);

/*
 * Resize: allocate a new table of new_bucket_count entries, copy all occupied
 * buckets from *pt, and atomically publish the new pointer.  The old table is
 * NOT freed; callers arrange retirement (e.g. quiescence / RCU) if readers may
 * still hold the old pointer.
 */
bool lf_hash_resize(LFHashTable **pt, uint32_t new_bucket_count);

/* Count occupied buckets (not thread-safe with concurrent writers). */
uint32_t lf_hash_count(const LFHashTable *t);

#ifdef __cplusplus
}
#endif

#endif /* LOCKFREE_HASH_TABLE_H */
