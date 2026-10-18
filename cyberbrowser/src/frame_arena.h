/*
 * Frame arenas (PARALLEL_STYLE_LAYOUT.md §5.4).
 *
 * A bump allocator over fixed-size blocks.  Blocks come from a process-wide
 * lock-free free list (a Treiber stack whose head carries a generation tag,
 * so ABA cannot happen) and go back to it when the arena is reset.  An arena
 * is used by one thread at a time; nothing allocated from it is freed
 * individually.
 */
#ifndef FRAME_ARENA_H
#define FRAME_ARENA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRAME_ARENA_BLOCK (256u * 1024u)

typedef struct FrameArenaBlock {
    struct FrameArenaBlock *next;   /* chain in an arena, or in the free list */
    size_t used;
    size_t cap;
    uint32_t pooled;                /* standard-size block (returns to the pool) */
    uint32_t pad;
    unsigned char data[];
} FrameArenaBlock;

typedef struct FrameArena {
    FrameArenaBlock *head;          /* current block first */
    size_t bytes;                   /* total allocated, for statistics */
} FrameArena;

void *frame_arena_alloc(FrameArena *a, size_t n);          /* 16-byte aligned */
void *frame_arena_calloc(FrameArena *a, size_t n);
char *frame_arena_strdup(FrameArena *a, const char *s);
char *frame_arena_strndup(FrameArena *a, const char *s, size_t n);
/* Move every block of `src` into `dst` (src is empty afterwards). */
void frame_arena_absorb(FrameArena *dst, FrameArena *src);
/* Return every block to the pool; the arena is empty afterwards. */
void frame_arena_reset(FrameArena *a);
/* Blocks currently sitting in the global pool (statistics). */
size_t frame_arena_pool_blocks(void);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_ARENA_H */
