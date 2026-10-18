/*
 * Frame arenas.  See frame_arena.h.
 */
#include "frame_arena.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Free list head: pointer in the low 48 bits, generation tag in the high 16.
 * x86-64 and AArch64 user pointers fit in 48 bits. */
static _Atomic uint64_t g_pool_head;
static _Atomic size_t g_pool_count;

#define PTR_MASK 0x0000FFFFFFFFFFFFull

static FrameArenaBlock *pool_pop(void)
{
    uint64_t head = atomic_load_explicit(&g_pool_head, memory_order_acquire);
    for (;;) {
        FrameArenaBlock *b = (FrameArenaBlock *)(uintptr_t)(head & PTR_MASK);
        if (!b) return NULL;
        /* Reading b->next is safe: blocks are never returned to the OS. */
        FrameArenaBlock *next = b->next;
        uint64_t tag = (head >> 48) + 1;
        uint64_t nh = ((uint64_t)(uintptr_t)next & PTR_MASK) | (tag << 48);
        if (atomic_compare_exchange_weak_explicit(&g_pool_head, &head, nh,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
            atomic_fetch_sub_explicit(&g_pool_count, 1, memory_order_relaxed);
            return b;
        }
    }
}

static void pool_push(FrameArenaBlock *b)
{
    uint64_t head = atomic_load_explicit(&g_pool_head, memory_order_relaxed);
    for (;;) {
        b->next = (FrameArenaBlock *)(uintptr_t)(head & PTR_MASK);
        uint64_t tag = (head >> 48) + 1;
        uint64_t nh = ((uint64_t)(uintptr_t)b & PTR_MASK) | (tag << 48);
        if (atomic_compare_exchange_weak_explicit(&g_pool_head, &head, nh,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            atomic_fetch_add_explicit(&g_pool_count, 1, memory_order_relaxed);
            return;
        }
    }
}

size_t frame_arena_pool_blocks(void) { return atomic_load(&g_pool_count); }

static FrameArenaBlock *block_new(size_t need)
{
    FrameArenaBlock *b = NULL;
    if (need <= FRAME_ARENA_BLOCK) {
        b = pool_pop();
        if (!b) {
            b = malloc(sizeof(FrameArenaBlock) + FRAME_ARENA_BLOCK);
            if (!b) return NULL;
            b->cap = FRAME_ARENA_BLOCK;
            b->pooled = 1;
        }
    } else {
        b = malloc(sizeof(FrameArenaBlock) + need);
        if (!b) return NULL;
        b->cap = need;
        b->pooled = 0;
    }
    b->used = 0;
    b->next = NULL;
    return b;
}

void *frame_arena_alloc(FrameArena *a, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    FrameArenaBlock *b = a->head;
    if (!b || b->used + n > b->cap) {
        FrameArenaBlock *nb = block_new(n);
        if (!nb) return NULL;
        if (b && !nb->pooled) {
            /* Oversized block: keep the current block in front. */
            nb->next = b->next;
            b->next = nb;
            nb->used = n;
            a->bytes += n;
            return nb->data;
        }
        nb->next = b;
        a->head = b = nb;
    }
    void *p = b->data + b->used;
    b->used += n;
    a->bytes += n;
    return p;
}

void *frame_arena_calloc(FrameArena *a, size_t n)
{
    void *p = frame_arena_alloc(a, n);
    if (p) memset(p, 0, n);
    return p;
}

char *frame_arena_strndup(FrameArena *a, const char *s, size_t n)
{
    char *p = frame_arena_alloc(a, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *frame_arena_strdup(FrameArena *a, const char *s)
{
    return frame_arena_strndup(a, s, strlen(s));
}

void frame_arena_absorb(FrameArena *dst, FrameArena *src)
{
    if (!src->head) return;
    FrameArenaBlock *last = src->head;
    while (last->next) last = last->next;
    /* Keep dst's current block in front so its free space stays usable. */
    if (dst->head) {
        last->next = dst->head->next;
        dst->head->next = src->head;
    } else {
        dst->head = src->head;
    }
    dst->bytes += src->bytes;
    src->head = NULL;
    src->bytes = 0;
}

void frame_arena_reset(FrameArena *a)
{
    FrameArenaBlock *b = a->head;
    while (b) {
        FrameArenaBlock *next = b->next;
        if (b->pooled) pool_push(b);
        else free(b);
        b = next;
    }
    a->head = NULL;
    a->bytes = 0;
}
