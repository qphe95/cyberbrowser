/*
 * Lock-free open-addressing hash table implementation.
 */

#include "lockfree_hash_table.h"
#include <stdlib.h>
#include <string.h>

static inline uint32_t round_up_power_of_two(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v < 16 ? 16 : v;
}

LFHashTable *lf_hash_create(uint32_t bucket_count) {
    bucket_count = round_up_power_of_two(bucket_count);
    size_t alloc_size = sizeof(LFHashTable) + (bucket_count - 1) * sizeof(LFHashBucket);
    LFHashTable *t = (LFHashTable *)calloc(1, alloc_size);
    if (!t) return NULL;
    t->bucket_count = bucket_count;
    t->bucket_bits = 0;
    uint32_t tmp = bucket_count;
    while (tmp > 1) { tmp >>= 1; t->bucket_bits++; }
    return t;
}

void lf_hash_destroy(LFHashTable *t) {
    free(t);
}

bool lf_hash_insert(LFHashTable *t, uint32_t hash, GCHandle key, GCHandle value) {
    if (!t) return false;
    uint32_t mask = t->bucket_count - 1;
    uint32_t idx = hash & mask;

    for (uint32_t probe = 0; probe < t->bucket_count; probe++) {
        LFHashBucket *b = &t->buckets[idx];
        uint32_t state = atomic_load_u32(&b->state);

        if (state == LF_HASH_EMPTY || state == LF_HASH_TOMBSTONE) {
            /* Try to claim this bucket for writing. */
            uint32_t prev = atomic_compare_exchange_u32(&b->state, state, LF_HASH_WRITING);
            if (prev == state) {
                b->hash = hash;
                b->key = key;
                b->value = value;
                atomic_store_u32(&b->state, LF_HASH_OCCUPIED);
                return true;
            }
            /* Someone else changed the bucket; re-probe. */
        } else if (state == LF_HASH_OCCUPIED && b->hash == hash && b->key == key) {
            /* Update existing entry.  Because the key matches, readers see the
             * old or new value but never an inconsistent key/value pair. */
            b->value = value;
            return true;
        }

        idx = (idx + 1) & mask;
    }

    return false; /* table full */
}

GCHandle lf_hash_lookup(LFHashTable *t, uint32_t hash, GCHandle key) {
    if (!t) return GC_HANDLE_NULL;
    uint32_t mask = t->bucket_count - 1;
    uint32_t idx = hash & mask;

    for (uint32_t probe = 0; probe < t->bucket_count; probe++) {
        LFHashBucket *b = &t->buckets[idx];
        uint32_t state = atomic_load_u32(&b->state);

        if (state == LF_HASH_OCCUPIED && b->hash == hash && b->key == key) {
            return b->value;
        }
        if (state == LF_HASH_EMPTY) {
            return GC_HANDLE_NULL;
        }

        idx = (idx + 1) & mask;
    }

    return GC_HANDLE_NULL;
}

GCHandle lf_hash_lookup_ex(LFHashTable *t, uint32_t hash, void *lookup_key,
                           LFHashEqFunc eq, void *user_data) {
    if (!t || !eq) return GC_HANDLE_NULL;
    uint32_t mask = t->bucket_count - 1;
    uint32_t idx = hash & mask;

    for (uint32_t probe = 0; probe < t->bucket_count; probe++) {
        LFHashBucket *b = &t->buckets[idx];
        uint32_t state = atomic_load_u32(&b->state);

        if (state == LF_HASH_OCCUPIED && b->hash == hash &&
            eq(b->key, b->hash, lookup_key, user_data)) {
            return b->value;
        }
        if (state == LF_HASH_EMPTY) {
            return GC_HANDLE_NULL;
        }

        idx = (idx + 1) & mask;
    }

    return GC_HANDLE_NULL;
}

bool lf_hash_remove(LFHashTable *t, uint32_t hash, GCHandle key) {
    if (!t) return false;
    uint32_t mask = t->bucket_count - 1;
    uint32_t idx = hash & mask;

    for (uint32_t probe = 0; probe < t->bucket_count; probe++) {
        LFHashBucket *b = &t->buckets[idx];
        uint32_t state = atomic_load_u32(&b->state);

        if (state == LF_HASH_OCCUPIED && b->hash == hash && b->key == key) {
            uint32_t prev = atomic_compare_exchange_u32(&b->state, LF_HASH_OCCUPIED, LF_HASH_TOMBSTONE);
            return prev == LF_HASH_OCCUPIED;
        }
        if (state == LF_HASH_EMPTY) {
            return false;
        }

        idx = (idx + 1) & mask;
    }

    return false;
}

bool lf_hash_remove_ex(LFHashTable *t, uint32_t hash, void *lookup_key,
                       LFHashEqFunc eq, void *user_data) {
    if (!t || !eq) return false;
    uint32_t mask = t->bucket_count - 1;
    uint32_t idx = hash & mask;

    for (uint32_t probe = 0; probe < t->bucket_count; probe++) {
        LFHashBucket *b = &t->buckets[idx];
        uint32_t state = atomic_load_u32(&b->state);

        if (state == LF_HASH_OCCUPIED && b->hash == hash &&
            eq(b->key, b->hash, lookup_key, user_data)) {
            uint32_t prev = atomic_compare_exchange_u32(&b->state, LF_HASH_OCCUPIED, LF_HASH_TOMBSTONE);
            return prev == LF_HASH_OCCUPIED;
        }
        if (state == LF_HASH_EMPTY) {
            return false;
        }

        idx = (idx + 1) & mask;
    }

    return false;
}

bool lf_hash_resize(LFHashTable **pt, uint32_t new_bucket_count) {
    if (!pt) return false;
    LFHashTable *old_t = (LFHashTable *)atomic_load_ptr((void *volatile *)pt);
    if (!old_t) return false;

    new_bucket_count = round_up_power_of_two(new_bucket_count);
    if (new_bucket_count <= old_t->bucket_count) new_bucket_count = old_t->bucket_count * 2;

    LFHashTable *new_t = lf_hash_create(new_bucket_count);
    if (!new_t) return false;

    /* Copy occupied entries.  No CAS needed because new_t is not visible yet. */
    for (uint32_t i = 0; i < old_t->bucket_count; i++) {
        LFHashBucket *b = &old_t->buckets[i];
        if (atomic_load_u32(&b->state) == LF_HASH_OCCUPIED) {
            bool ok = lf_hash_insert(new_t, b->hash, b->key, b->value);
            if (!ok) {
                lf_hash_destroy(new_t);
                return false;
            }
        }
    }

    if (!atomic_compare_exchange_ptr((void *volatile *)pt, old_t, new_t)) {
        lf_hash_destroy(new_t);
        return false;
    }
    return true;
}

uint32_t lf_hash_count(const LFHashTable *t) {
    if (!t) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < t->bucket_count; i++) {
        if (atomic_load_u32((volatile uint32_t *)&t->buckets[i].state) == LF_HASH_OCCUPIED) {
            count++;
        }
    }
    return count;
}
