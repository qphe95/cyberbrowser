/*
 * GC heap: regions, handle tables, allocation, registries.
 * Design: CONCURRENT_GC.md.  The collector itself is in gc_collector.c.
 *
 * Heap
 *   One reserved address range split into 1 MB regions.  Objects are bump
 *   allocated; each starts with an 8-byte header placed at 8 mod 16 so the
 *   payload is 16-byte aligned.  Regions keep side bitmaps: object starts
 *   and two mark bitmaps (cycle parity).
 *
 * Handles
 *   Two handle tables of the same size.  gc_deref reads the front table.
 *   The JS thread writes every entry change to both ("dual write"), front
 *   first.  The collector builds moved addresses in the back table and the
 *   JS thread swaps the tables at a tick boundary.
 *
 * Threads
 *   Only the JS thread allocates engine objects.  The GC thread allocates
 *   copies in its own regions.  Regions are claimed with CAS; nothing here
 *   takes a lock.
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#include "quickjs.h"
#include "quickjs_gc_unified.h"
#include "quickjs-internal.h"
#include "gc_platform.h"
#include "gc_core.h"

GCState g_gc;
uint32_t g_gc_epoch;
GCCore g_core;

/* ============================================================================
 * Bit helpers
 * ============================================================================ */

static inline size_t gc_align16(size_t n) { return (n + 15) & ~(size_t)15; }

/* ============================================================================
 * Handle tables
 * ============================================================================ */

#define HANDLE_COMMIT_CHUNK (64u * 1024u)   /* entries committed at a time */

void **gc_core_back_table(void) {
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    return f == g_gc.tables[0] ? g_gc.tables[1] : g_gc.tables[0];
}

static bool handle_tables_commit_to(uint32_t limit) {
    while (g_gc.committed_handles < limit) {
        uint32_t start = g_gc.committed_handles;
        size_t bytes = (size_t)HANDLE_COMMIT_CHUNK * sizeof(void *);
        if (start + HANDLE_COMMIT_CHUNK > GC_MAX_HANDLES) return false;
        for (int t = 0; t < 2; t++) {
            if (!gc_os_commit(&g_gc.tables[t][start], bytes)) return false;
        }
        g_gc.committed_handles = start + HANDLE_COMMIT_CHUNK;
    }
    return true;
}

/* Write an entry in both tables (JS thread).  Front first, then back: the
 * collector's back-table updates use CAS and rely on this order. */
void gc_core_handle_dual_write(GCHandle h, void *p) {
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    void **b = f == g_gc.tables[0] ? g_gc.tables[1] : g_gc.tables[0];
    atomic_store_explicit((_Atomic(void *) *)&f[h], p, memory_order_release);
    atomic_store_explicit((_Atomic(void *) *)&b[h], p, memory_order_release);
}

bool gc_core_clear_handle(GCHandle h, void *payload) {
    void **f = atomic_load_explicit(&g_gc.front, memory_order_acquire);
    void *expect = payload;
    if (!atomic_compare_exchange_strong_explicit((_Atomic(void *) *)&f[h], &expect, NULL,
                                                 memory_order_acq_rel, memory_order_acquire))
        return false;
    void **b = f == g_gc.tables[0] ? g_gc.tables[1] : g_gc.tables[0];
    /* The back entry normally equals the front one.  Between a flip and the
     * end of the back table's update it may still hold an older address;
     * either way nothing may keep pointing at a freed handle. */
    atomic_store_explicit((_Atomic(void *) *)&b[h], NULL, memory_order_release);
    return true;
}

GCHandleHistory *g_handle_history;
bool g_handle_no_reuse;
static size_t g_handle_history_committed;   /* entries */

static uint32_t cycles_done(void) {
    return (uint32_t)(g_core.stats.minor_cycles + g_core.stats.major_cycles);
}

void gc_core_note_freed(GCHandle h, int how) {
    if (h < g_handle_history_committed) {
        g_handle_history[h].free_cycle = cycles_done();
        g_handle_history[h].freed_by = (uint8_t)how;
    }
}

/* Print what is known about handle h (QJS_GC_HISTORY / QJS_GC_NO_REUSE). */
void gc_debug_report_stale_handle(GCHandle h, const char *where) {
    static const char *how[] = { "not freed", "gc_free from the JS thread",
                                 "stop-the-world sweep", "concurrent sweep" };
    fprintf(stderr, "[GC-STALE] %s: handle %u is %s", where, (unsigned)h, gc_deref(h) ? "live" : "dead");
    if (g_handle_history && h < g_handle_history_committed) {
        GCHandleHistory *e = &g_handle_history[h];
        fprintf(stderr, "; now type %u, allocated %s after %u cycles, %s after %u; object #%u on it",
                e->type, e->alloc_young ? "young" : "old", e->alloc_cycle,
                how[e->freed_by & 3], e->free_cycle, e->uses);
        if (e->uses > 1)
            fprintf(stderr, "; before that type %u, allocated %s after %u cycles, freed by %s after %u",
                    e->prev_type, e->prev_young ? "young" : "old", e->prev_alloc_cycle,
                    how[e->prev_freed_by & 3], e->prev_free_cycle);
        fprintf(stderr, "; %u cycles done (%llu minor, %llu major)", cycles_done(),
                (unsigned long long)g_core.stats.minor_cycles, (unsigned long long)g_core.stats.major_cycles);
    }
    fputc('\n', stderr);
    if (g_core.trace_handle) gc_core_trace_dump(h);
    fflush(stderr);
}

#if GC_STALE_CHECK
/* Distinct (call site) uses of dead handles: counted, the first handle kept,
 * printed at exit by gc_debug_stale_dump. */
#define STALE_SITES 4096
static struct { void *site; GCHandle first; uint32_t n, type, freed_by; } g_stale[STALE_SITES];
static atomic_flag g_stale_lock = ATOMIC_FLAG_INIT;

void gc_debug_stale_deref(GCHandle handle, void *site) {
    if (!g_handle_history || handle >= g_handle_history_committed) return;
    GCHandleHistory *e = &g_handle_history[handle];
    if (e->freed_by == GC_FREED_NOT) return;   /* not allocated yet, or in flux */
    while (atomic_flag_test_and_set_explicit(&g_stale_lock, memory_order_acquire)) {}
    size_t i = ((uintptr_t)site >> 2) % STALE_SITES;
    for (size_t k = 0; k < STALE_SITES; k++, i = (i + 1) % STALE_SITES) {
        if (g_stale[i].site == site) { g_stale[i].n++; break; }
        if (!g_stale[i].site) {
            g_stale[i].site = site; g_stale[i].first = handle; g_stale[i].n = 1;
            g_stale[i].type = e->type; g_stale[i].freed_by = e->freed_by;
            break;
        }
    }
    atomic_flag_clear_explicit(&g_stale_lock, memory_order_release);
}

/* Print the call sites (image-relative, for addr2line) at exit. */
static void gc_debug_stale_dump(void) {
    extern char __ImageBase;
    size_t n = 0;
    for (size_t i = 0; i < STALE_SITES; i++)
        if (g_stale[i].site && ++n)
            fprintf(stderr, "[GC-STALE-SITE] rva=0x%llx count=%u first=%u type=%u freed_by=%u\n",
                    (unsigned long long)((uintptr_t)g_stale[i].site - (uintptr_t)&__ImageBase),
                    g_stale[i].n, (unsigned)g_stale[i].first, g_stale[i].type, g_stale[i].freed_by);
    fprintf(stderr, "[GC-STALE-SUMMARY] %zu sites\n", n);
    fflush(stderr);
}
#endif

void gc_core_hcache_push(GCHandle h) {
    if (g_handle_no_reuse) return;  /* QJS_GC_NO_REUSE: never reuse */
    if (g_core.hcache_n == g_core.hcache_cap) {
        uint32_t cap = g_core.hcache_cap ? g_core.hcache_cap * 2 : 4096;
        GCHandle *n = (GCHandle *)realloc(g_core.hcache, cap * sizeof(GCHandle));
        if (!n) return;  /* leak the handle rather than fail */
        g_core.hcache = n;
        g_core.hcache_cap = cap;
    }
    g_core.hcache[g_core.hcache_n++] = h;
}

static GCHandle handle_allocate(void) {
    if (g_core.hcache_n == 0)
        gc_core_drain_freed_ring();
    if (g_core.hcache_n > 0) {
        GCHandle r = g_core.hcache[--g_core.hcache_n];
        GC_TRACE(r, "reused by allocation");
        return r;
    }
    uint32_t h = atomic_load_explicit(&g_gc.high_water, memory_order_relaxed);
    if (h >= GC_MAX_HANDLES) {
        fprintf(stderr, "[GC] out of handles\n");
        abort();
    }
    if (h + 1 > g_gc.committed_handles && !handle_tables_commit_to(h + 1)) {
        fprintf(stderr, "[GC] cannot commit handle table\n");
        abort();
    }
    if (g_handle_history && h >= g_handle_history_committed) {
        size_t chunk = (size_t)1 << 20;   /* entries */
        size_t upto = ((size_t)h / chunk + 1) * chunk;
        if (!gc_os_commit(g_handle_history + g_handle_history_committed,
                          (upto - g_handle_history_committed) * sizeof(GCHandleHistory))) {
            fprintf(stderr, "[GC] cannot commit handle history\n");
            abort();
        }
        g_handle_history_committed = upto;
    }
    atomic_store_explicit(&g_gc.high_water, h + 1, memory_order_release);
    return h;
}

/* ============================================================================
 * Regions
 * ============================================================================ */

#define BITMAP_WORDS ((GC_REGION_SIZE / GC_GRANULE) / 64)

static bool region_commit_metadata(GCRegion *r) {
    if (!r->line_first) {
        r->line_first = (uint8_t *)malloc(GC_LINES);
        r->mark_bits[0] = (uint64_t *)calloc(BITMAP_WORDS, sizeof(uint64_t));
        r->mark_bits[1] = (uint64_t *)calloc(BITMAP_WORDS, sizeof(uint64_t));
    }
    return r->line_first && r->mark_bits[0] && r->mark_bits[1];
}

/* Bring a claimed region's memory and bitmaps to a clean state. */
static bool region_prepare(GCRegion *r, bool fresh) {
    uint8_t *base = gc_core_region_start(r);
    if (fresh || r->decommitted) {
        if (!gc_os_commit(base, GC_REGION_SIZE)) return false;
        r->decommitted = false;
        if (fresh) atomic_fetch_add(&g_core.regions_committed, 1);
    } else if (r->used_before && !r->zeroed) {
        memset(base, 0, GC_REGION_SIZE);
    }
    r->zeroed = false;
    atomic_store_explicit(&r->recycle_state, GC_RECYCLE_IDLE, memory_order_relaxed);
    if (r->free_lines) memset(r->free_lines, 0, GC_LINE_WORDS * sizeof(uint64_t));
    if (!region_commit_metadata(r)) return false;
    memset(r->line_first, GC_LINE_CONT, GC_LINES);
    memset(r->mark_bits[0], 0, BITMAP_WORDS * sizeof(uint64_t));
    memset(r->mark_bits[1], 0, BITMAP_WORDS * sizeof(uint64_t));
    return true;
}

static void region_activate(GCRegion *r, uint32_t space, uint32_t head, uint32_t count) {
    uint8_t *base = gc_core_region_start(r);
    r->used_before = true;
    r->space = space;
    r->alloc_owner = GC_OWNER_NONE;
    r->live_bytes = 0;
    r->tams = (uintptr_t)base;
    r->safe_top = (uintptr_t)base + GC_HEADER_SIZE;
    r->scan_top = (uintptr_t)base + GC_HEADER_SIZE;
    r->in_cset = false;
    r->collecting = false;
    r->keep = false;
    r->pinned = false;
    r->permanent = false;
    r->humongous_head = head;
    r->humongous_count = count;
    atomic_store_explicit(&r->top, (uintptr_t)base + GC_HEADER_SIZE, memory_order_release);
    atomic_store_explicit(&r->state, space, memory_order_release);
    atomic_fetch_add(&g_core.regions_in_use, 1);
}

/* Claim a region for allocation.  Lock-free: CAS a FREE region, or take a
 * never-used one by bumping the high-water mark. */
GCRegion *gc_core_region_acquire(uint32_t space) {
    uint32_t hwm = atomic_load_explicit(&g_core.region_hwm, memory_order_acquire);
    uint32_t hint = atomic_load_explicit((_Atomic uint32_t *)&g_core.region_hint, memory_order_relaxed);
    if (hint >= hwm) hint = 0;
    for (uint32_t n = 0; n < hwm; n++) {
        uint32_t i = hint + n;
        if (i >= hwm) i -= hwm;
        GCRegion *r = &g_core.regions[i];
        if (atomic_load_explicit(&r->state, memory_order_relaxed) != GC_RS_FREE) continue;
        uint32_t expect = GC_RS_FREE;
        if (!atomic_compare_exchange_strong(&r->state, &expect, GC_RS_CLAIMED)) continue;
        atomic_store_explicit((_Atomic uint32_t *)&g_core.region_hint, i + 1, memory_order_relaxed);
        if (!region_prepare(r, false)) {
            atomic_store(&r->state, GC_RS_FREE);
            return NULL;
        }
        region_activate(r, space, i, 1);
        return r;
    }
    uint32_t i = atomic_fetch_add(&g_core.region_hwm, 1);
    if (i >= GC_MAX_REGIONS) {
        fprintf(stderr, "[GC] heap reservation exhausted\n");
        abort();
    }
    GCRegion *r = &g_core.regions[i];
    atomic_store(&r->state, GC_RS_CLAIMED);
    if (!region_prepare(r, true)) {
        fprintf(stderr, "[GC] cannot commit region\n");
        abort();
    }
    region_activate(r, space, i, 1);
    return r;
}

/* Claim n contiguous regions for a humongous object. */
static GCRegion *region_acquire_humongous(uint32_t n) {
    uint32_t hwm = atomic_load_explicit(&g_core.region_hwm, memory_order_acquire);
    for (uint32_t i = 0; i + n <= hwm; i++) {
        uint32_t k;
        for (k = 0; k < n; k++) {
            uint32_t expect = GC_RS_FREE;
            if (!atomic_compare_exchange_strong(&g_core.regions[i + k].state, &expect, GC_RS_CLAIMED))
                break;
        }
        if (k < n) {
            while (k > 0) {
                k--;
                atomic_store(&g_core.regions[i + k].state, GC_RS_FREE);
            }
            continue;
        }
        for (k = 0; k < n; k++) {
            if (!region_prepare(&g_core.regions[i + k], false)) {
                fprintf(stderr, "[GC] cannot commit region\n");
                abort();
            }
        }
        for (k = 0; k < n; k++)
            region_activate(&g_core.regions[i + k], k == 0 ? GC_RS_HUMONGOUS : GC_RS_HUMONGOUS_CONT,
                            i, k == 0 ? n : 0);
        return &g_core.regions[i];
    }
    uint32_t i = atomic_fetch_add(&g_core.region_hwm, n);
    if (i + n > GC_MAX_REGIONS) {
        fprintf(stderr, "[GC] heap reservation exhausted\n");
        abort();
    }
    for (uint32_t k = 0; k < n; k++) {
        GCRegion *r = &g_core.regions[i + k];
        atomic_store(&r->state, GC_RS_CLAIMED);
        if (!region_prepare(r, true)) {
            fprintf(stderr, "[GC] cannot commit region\n");
            abort();
        }
    }
    for (uint32_t k = 0; k < n; k++)
        region_activate(&g_core.regions[i + k], k == 0 ? GC_RS_HUMONGOUS : GC_RS_HUMONGOUS_CONT,
                        i, k == 0 ? n : 0);
    return &g_core.regions[i];
}

/* Return a region (or a whole humongous run) to the free pool. */
void gc_core_region_release(GCRegion *r) {
    uint32_t first = (uint32_t)(r - g_core.regions);
    uint32_t count = atomic_load(&r->state) == GC_RS_HUMONGOUS ? r->humongous_count : 1;
    for (uint32_t k = 0; k < count; k++) {
        GCRegion *x = &g_core.regions[first + k];
        x->in_cset = false;
        x->collecting = false;
        x->keep = false;
        x->pinned = false;
        x->permanent = false;
        x->live_bytes = 0;
        x->alloc_owner = GC_OWNER_NONE;
        if (g_core.poison) {
            gc_os_decommit(gc_core_region_start(x), GC_REGION_SIZE);
            x->decommitted = true;
        } else if (x->used_before) {
            /* Zero what was used now, on the thread that releases it (the
             * collector's, when concurrent), instead of the whole region
             * on the allocating thread when it is claimed again.  A
             * humongous continuation's top says nothing: all of it. */
            uint8_t *base = gc_core_region_start(x);
            uintptr_t top = atomic_load_explicit(&x->top, memory_order_acquire);
            size_t used = k == 0 && top > (uintptr_t)base && top <= (uintptr_t)base + GC_REGION_SIZE
                              ? (size_t)(top - (uintptr_t)base) : GC_REGION_SIZE;
            memset(base, 0, used);
            x->zeroed = true;
        }
        atomic_store_explicit(&x->state, GC_RS_FREE, memory_order_release);
        atomic_fetch_sub(&g_core.regions_in_use, 1);
    }
}

/* ============================================================================
 * Allocation
 * ============================================================================ */

/* ---- Holes: reusing free lines of old regions without moving objects ----
 *
 * A line is free when no current object overlaps it.  After a major sweep,
 * each run of free lines below the region's top becomes a hole: it is
 * zeroed, and a dead object crossing either edge is cut at the edge (the
 * part past the hole's end gets a filler header), so walking the region
 * header by header still works: a zero header is free space, skipped with
 * the line bytes (gc_core_skip_free).  Allocating into a hole is a bump,
 * as in a fresh region. */

/* A dead (non-current) header of `total` bytes at p. */
static inline void write_filler(uintptr_t p, size_t total) {
    GCHeader *h = (GCHeader *)p;
    h->handle = GC_HANDLE_NULL;
    h->info = (uint32_t)JS_GC_OBJ_TYPE_DATA | (uint32_t)((total / GC_GRANULE) << GC_INFO_SIZE_SHIFT);
}

static inline uint32_t line_of(uintptr_t lb, uintptr_t p) { return (uint32_t)((p - lb) >> GC_LINE_SHIFT); }

typedef struct { uintptr_t start, end; bool live; } HoleExtent;
static HoleExtent *g_hole_ext;   /* sweeper-private scratch */
static uint32_t g_hole_ext_cap;

size_t gc_core_format_holes(GCRegion *r) {
    uintptr_t lb = gc_core_line_base(r);
    uintptr_t top = atomic_load_explicit(&r->top, memory_order_acquire);
    if (top <= lb) return 0;
    uint32_t nlines = line_of(lb, top);   /* lines wholly below top */
    if (nlines > GC_LINES) nlines = GC_LINES;
    if (!nlines) return 0;
    if (!r->free_lines && !(r->free_lines = (uint64_t *)calloc(GC_LINE_WORDS, sizeof(uint64_t))))
        return 0;
    uint64_t live[GC_LINE_WORDS];
    memset(live, 0, sizeof(live));
    /* Object extents in address order, and the lines live objects touch. */
    uint32_t n = 0;
    for (uintptr_t p = lb; p < top;) {
        GCHeader *h = (GCHeader *)p;
        size_t total = gc_header_total_size(h);
        if (!total) { p = gc_core_skip_free(r, p, top); continue; }
        if (n == g_hole_ext_cap) {
            uint32_t cap = g_hole_ext_cap ? g_hole_ext_cap * 2 : 4096;
            HoleExtent *ne = (HoleExtent *)realloc(g_hole_ext, cap * sizeof(HoleExtent));
            if (!ne) return 0;
            g_hole_ext = ne;
            g_hole_ext_cap = cap;
        }
        bool cur = gc_core_header_is_current(h);
        g_hole_ext[n++] = (HoleExtent){ p, p + total, cur };
        if (cur) {
            uint32_t l0 = line_of(lb, p), l1 = line_of(lb, p + total - 1);
            for (uint32_t l = l0; l <= l1 && l < GC_LINES; l++) live[l >> 6] |= (uint64_t)1 << (l & 63);
        }
        p += total;
    }
    memset(r->free_lines, 0, GC_LINE_WORDS * sizeof(uint64_t));
    size_t free_bytes = 0;
    uint32_t k = 0;
    for (uint32_t i = 0; i < nlines;) {
        if ((live[i >> 6] >> (i & 63)) & 1) { i++; continue; }
        uint32_t j = i;
        while (j < nlines && !((live[j >> 6] >> (j & 63)) & 1)) j++;
        uintptr_t hs = lb + ((uintptr_t)i << GC_LINE_SHIFT), he = lb + ((uintptr_t)j << GC_LINE_SHIFT);
        /* A (dead) object crossing the start: cut it there. */
        while (k < n && g_hole_ext[k].end <= hs) k++;
        if (k < n && g_hole_ext[k].start < hs) {
            GCHeader *h = (GCHeader *)g_hole_ext[k].start;
            h->info = (h->info & GC_INFO_TYPE_MASK) |
                      (uint32_t)(((hs - g_hole_ext[k].start) / GC_GRANULE) << GC_INFO_SIZE_SHIFT);
        }
        /* A (dead) object crossing the end: its tail becomes a filler. */
        uint32_t k2 = k;
        while (k2 < n && g_hole_ext[k2].end <= he) k2++;
        if (k2 < n && g_hole_ext[k2].start < he && g_hole_ext[k2].end > he) {
            write_filler(he, g_hole_ext[k2].end - he);
            r->line_first[j] = 0;
        }
        /* Allocations come zeroed: so does the hole, here on the sweeping
         * thread rather than the allocating one. */
        memset((void *)hs, 0, he - hs);
        r->line_first[i] = 0;
        memset(r->line_first + i + 1, GC_LINE_CONT, j - i - 1);
        for (uint32_t l = i; l < j; l++) r->free_lines[l >> 6] |= (uint64_t)1 << (l & 63);
        free_bytes += he - hs;
        k = k2;
        i = j;
    }
    return free_bytes;
}

/* Offer region r (its holes formatted) to the recycling contexts. */
void gc_core_offer_recyclable(GCRegion *r) {
    while (atomic_flag_test_and_set_explicit(&g_core.recyclable_lock, memory_order_acquire)) {}
    if (g_core.recyclable_n == g_core.recyclable_cap) {
        uint32_t cap = g_core.recyclable_cap ? g_core.recyclable_cap * 2 : 256;
        uint32_t *nr = (uint32_t *)realloc(g_core.recyclable, cap * sizeof(uint32_t));
        if (nr) { g_core.recyclable = nr; g_core.recyclable_cap = cap; }
    }
    if (!r->in_recyclable && g_core.recyclable_n < g_core.recyclable_cap) {
        g_core.recyclable[g_core.recyclable_n++] = (uint32_t)(r - g_core.regions);
        r->in_recyclable = true;
    }
    atomic_flag_clear_explicit(&g_core.recyclable_lock, memory_order_release);
}

/* Take an AVAILABLE region with holes, or NULL. */
static GCRegion *take_recyclable(void) {
    GCRegion *got = NULL;
    while (atomic_flag_test_and_set_explicit(&g_core.recyclable_lock, memory_order_acquire)) {}
    while (g_core.recyclable_n && !got) {
        GCRegion *r = &g_core.regions[g_core.recyclable[--g_core.recyclable_n]];
        r->in_recyclable = false;
        uint32_t expect = GC_RECYCLE_AVAILABLE;
        if (atomic_load_explicit(&r->state, memory_order_acquire) == GC_RS_OLD &&
            atomic_compare_exchange_strong(&r->recycle_state, &expect, GC_RECYCLE_OWNED))
            got = r;
    }
    atomic_flag_clear_explicit(&g_core.recyclable_lock, memory_order_release);
    return got;
}

/* Move a recycling context to its region's next hole; false when none. */
static bool next_hole(GCAllocCtx *a) {
    GCRegion *r = a->region;
    uint32_t i = a->next_line, w = i >> 6;
    if (w >= GC_LINE_WORDS) return false;
    /* First free line at or after i, then the first live one after it. */
    uint64_t v = r->free_lines[w] & (~(uint64_t)0 << (i & 63));
    while (!v) {
        if (++w >= GC_LINE_WORDS) return false;
        v = r->free_lines[w];
    }
    i = (w << 6) + (uint32_t)__builtin_ctzll(v);
    if (i >= GC_LINES) return false;
    v = ~r->free_lines[w] & (~(uint64_t)0 << (i & 63));
    while (!v && ++w < GC_LINE_WORDS) v = ~r->free_lines[w];
    uint32_t j = v ? (w << 6) + (uint32_t)__builtin_ctzll(v) : GC_LINES;
    if (j > GC_LINES) j = GC_LINES;
    a->top = (uint8_t *)(gc_core_line_base(r) + ((uintptr_t)i << GC_LINE_SHIFT));
    a->end = (uint8_t *)(gc_core_line_base(r) + ((uintptr_t)j << GC_LINE_SHIFT));
    a->line_end = a->top;
    a->next_line = j;
    a->in_hole = true;
    return true;
}

/* Give up the context's region (not its overflow one). */
static void ctx_retire_region(GCAllocCtx *a) {
    if (a->region) {
        if (a->in_hole) {
            /* Holes lie below top; the region's top does not move.  The rest
             * of the hole stays zero: free space. */
            atomic_store_explicit(&a->region->recycle_state, GC_RECYCLE_IDLE, memory_order_release);
        } else {
            atomic_store_explicit(&a->region->top, (uintptr_t)a->top, memory_order_release);
        }
        a->region->alloc_owner = GC_OWNER_NONE;
    }
    a->region = NULL;
    a->top = a->end = a->line_end = NULL;
    a->in_hole = false;
}

void gc_core_alloc_ctx_retire(GCAllocCtx *a) {
    if (a->overflow) ctx_retire_region(a->overflow);
    ctx_retire_region(a);
}

/* Objects up to this size may go into holes. */
#define GC_HOLE_MAX_OBJECT ((size_t)8 << 10)

/* Take [top, top + total), which reaches past the current line: record
 * where headers start in the lines it enters (the lines it covers whole
 * are continued already: fresh regions and holes start out that way). */
static inline GCHeader *alloc_cross(GCAllocCtx *a, size_t total, uint32_t info) {
    GCRegion *r = a->region;
    uintptr_t lb = gc_core_line_base(r), p = (uintptr_t)a->top, end = p + total;
    uint32_t lp = line_of(lb, p), le = line_of(lb, end);
    if (((p - lb) & (GC_LINE_SIZE - 1)) == 0) r->line_first[lp] = 0;
    /* The next header goes at end, the first in its line unless that is
     * p's own; at a hole's end one is there already. */
    if (le != lp && end < (uintptr_t)a->end)
        r->line_first[le] = (uint8_t)(((end - lb) & (GC_LINE_SIZE - 1)) >> 4);
    uintptr_t next = lb + ((uintptr_t)(le + 1) << GC_LINE_SHIFT);
    a->line_end = (uint8_t *)(next < (uintptr_t)a->end ? next : (uintptr_t)a->end);
    a->top = (uint8_t *)end;
    GCHeader *hdr = (GCHeader *)p;
    hdr->info = info;
    if (!a->in_hole) atomic_store_explicit(&r->top, end, memory_order_release);
    return hdr;
}

/* Bump-allocate in the context's fresh region, taking a new one when the
 * object does not fit. */
static GCHeader *bump_alloc(GCAllocCtx *a, size_t total, uint32_t info) {
    if (!a->top || a->top + total > a->end || a->in_hole) {
        ctx_retire_region(a);
        GCRegion *r = gc_core_region_acquire(a->space);
        if (!r) return NULL;
        r->alloc_owner = a->owner;
        r->permanent = a->permanent;
        a->region = r;
        a->top = (uint8_t *)atomic_load_explicit(&r->top, memory_order_relaxed);
        a->end = gc_core_region_start(r) + GC_LINES_END_OFFSET;
        a->line_end = a->top;
    }
    return alloc_cross(a, total, info);
}

/* A recycling context's allocation from holes: the current hole (or, for an
 * object bigger than a line that does not fit there, the overflow region),
 * the region's later holes, then other recyclable regions.  NULL when none
 * fits, or while a major cycle runs (the caller bump-allocates as usual). */
static GCHeader *hole_alloc(GCAllocCtx *a, size_t total, uint32_t info) {
    if (atomic_load_explicit(&g_core.holes_paused, memory_order_acquire)) {
        if (a->in_hole) ctx_retire_region(a);
        return NULL;
    }
    if (a->in_hole) {
        if (a->top + total <= a->end) return alloc_cross(a, total, info);
        /* Skipping the rest of the hole would waste it: small objects fill
         * it later. */
        if (total > GC_LINE_SIZE && a->overflow) {
            GCAllocCtx *o = a->overflow;
            if (o->top && o->top + total < o->line_end) {
                GCHeader *hdr = (GCHeader *)o->top;
                o->top += total;
                hdr->info = info;
                atomic_store_explicit(&o->region->top, (uintptr_t)o->top, memory_order_release);
                return hdr;
            }
            return bump_alloc(o, total, info);
        }
        while (next_hole(a))
            if (a->top + total <= a->end) return alloc_cross(a, total, info);
        ctx_retire_region(a);
    }
    /* Only when the context needs new space: keep filling a fresh region it
     * already has. */
    if (a->region && a->top + total <= a->end) return NULL;
    GCRegion *r;
    while ((r = take_recyclable())) {
        if (a->region) ctx_retire_region(a);
        r->alloc_owner = a->owner;
        a->region = r;
        a->next_line = 0;
        a->in_hole = false;
        while (next_hole(a))
            if (a->top + total <= a->end) return alloc_cross(a, total, info);
        ctx_retire_region(a);
    }
    return NULL;
}

GCHeader *gc_core_alloc_object_memory(GCAllocCtx *a, size_t payload, JSGCObjectTypeEnum type) {
    size_t total = gc_align16(payload + GC_HEADER_SIZE);
    if (total < GC_GRANULE) total = GC_GRANULE;
    uint32_t info = (uint32_t)type | (uint32_t)((total / GC_GRANULE) << GC_INFO_SIZE_SHIFT);

    /* Fast path: the object ends inside the current line.  The memory is
     * zero (fresh regions and holes both are), so the header is all. */
    uint8_t *p = a->top;
    if (likely(p + total < a->line_end)) {
        a->top = p + total;
        GCHeader *hdr = (GCHeader *)p;
        hdr->info = info;
        /* Publish the new top so the collector can see the object. */
        if (!a->in_hole) atomic_store_explicit(&a->region->top, (uintptr_t)a->top, memory_order_release);
        return hdr;
    }

    if (total > ((size_t)(UINT32_MAX >> GC_INFO_SIZE_SHIFT)) * GC_GRANULE) {
        fprintf(stderr, "[GC] object too large (%zu bytes)\n", payload);
        abort();
    }
    if (total >= GC_HUMONGOUS_MIN) {
        uint32_t n = (uint32_t)((total + GC_HEADER_SIZE + GC_REGION_SIZE - 1) >> GC_REGION_SHIFT);
        GCRegion *r = region_acquire_humongous(n);
        GCHeader *hdr = (GCHeader *)(gc_core_region_start(r) + GC_HEADER_SIZE);
        hdr->info = info;
        atomic_store_explicit(&r->top, (uintptr_t)hdr + total, memory_order_release);
        return hdr;
    }

    if (a->recycles && g_core.holes && total <= GC_HOLE_MAX_OBJECT) {
        GCHeader *hdr = hole_alloc(a, total, info);
        if (hdr) return hdr;
    }

    return bump_alloc(a, total, info);
}

/* Types that live long from birth go straight to old space. */
static bool type_starts_old(JSGCObjectTypeEnum type) {
    switch (type) {
    case JS_GC_OBJ_TYPE_JS_RUNTIME:
    case JS_GC_OBJ_TYPE_JS_CONTEXT:
    case JS_GC_OBJ_TYPE_SHAPE:
    case JS_GC_OBJ_TYPE_FUNCTION_BYTECODE:
    case JS_GC_OBJ_TYPE_MODULE:
    case JS_GC_OBJ_TYPE_FUNCTION_DEF:
    case JS_GC_OBJ_TYPE_BYTES:   /* never moves: keep it out of the nursery */
        return true;
    default:
        return false;
    }
}

static GCHandle gc_alloc_in(size_t size, JSGCObjectTypeEnum type, bool old, bool permanent) {
    if (!g_gc.initialized && !gc_init()) return GC_HANDLE_NULL;
    bool young = !old && !permanent && g_core.nursery_enabled;
    GCAllocCtx *a = permanent ? &g_core.js_perm : young ? &g_core.js_young : &g_core.js_old;
    GCHeader *hdr = gc_core_alloc_object_memory(a, size, type);
    if (!hdr) {
        fprintf(stderr, "[GC] out of memory allocating %zu bytes\n", size);
        abort();
    }
    GCHandle h = handle_allocate();
    hdr->handle = h;
    if (g_handle_history) {
        GCHandleHistory *e = &g_handle_history[h];
        if (e->uses) {
            e->prev_alloc_cycle = e->alloc_cycle;
            e->prev_free_cycle = e->free_cycle;
            e->prev_type = e->type;
            e->prev_freed_by = e->freed_by;
            e->prev_young = e->alloc_young;
        }
        e->uses++;
        e->alloc_cycle = cycles_done();
        e->free_cycle = 0;
        e->type = (uint16_t)type;
        e->freed_by = GC_FREED_NOT;
        e->alloc_young = young;
    }
    gc_core_handle_dual_write(h, gc_header_payload(hdr));
    GC_TRACE(h, type == JS_GC_OBJ_TYPE_STACK_FRAME ? "allocated (frame)" : "allocated");
    if (type == JS_GC_OBJ_TYPE_JS_CONTEXT)
        gc_core_registry_add(&g_core.contexts, h);
    gc_core_note_allocation(gc_header_total_size(hdr), young);
    return h;
}

/* Debug (QJS_GC_ALLOC_STATS=1): allocations by calling site and by type,
 * printed at exit, to see what a page allocates. */
#define ALLOC_SITES 4096
static int g_alloc_stats = -1;
static struct { void *site; uint64_t n, bytes; uint32_t type; } g_asites[ALLOC_SITES];
static uint64_t g_atype_n[32], g_atype_bytes[32];

static void alloc_stats_dump(void) {
    extern char __ImageBase;
    uint64_t tn = 0, tb = 0;
    for (int t = 0; t < 32; t++) { tn += g_atype_n[t]; tb += g_atype_bytes[t]; }
    fprintf(stderr, "[ALLOC-STATS] %llu allocations, %llu bytes\n", (unsigned long long)tn, (unsigned long long)tb);
    for (int t = 0; t < 32; t++)
        if (g_atype_n[t])
            fprintf(stderr, "[ALLOC-STATS] type %d: %llu allocations, %llu bytes\n", t,
                    (unsigned long long)g_atype_n[t], (unsigned long long)g_atype_bytes[t]);
    uint32_t order[ALLOC_SITES], n = 0;
    for (uint32_t i = 0; i < ALLOC_SITES; i++) if (g_asites[i].site) order[n++] = i;
    for (uint32_t a = 0; a < n && a < 40; a++)
        for (uint32_t b = a + 1; b < n; b++)
            if (g_asites[order[b]].n > g_asites[order[a]].n) { uint32_t t = order[a]; order[a] = order[b]; order[b] = t; }
    for (uint32_t a = 0; a < n && a < 40; a++) {
        uint32_t i = order[a];
        fprintf(stderr, "[ALLOC-SITE] rva=0x%llx type %u: %llu allocations, %llu bytes\n",
                (unsigned long long)((uintptr_t)g_asites[i].site - (uintptr_t)&__ImageBase), g_asites[i].type,
                (unsigned long long)g_asites[i].n, (unsigned long long)g_asites[i].bytes);
    }
    fflush(stderr);
}

static inline void alloc_stats_note(void *site, JSGCObjectTypeEnum type, size_t size) {
    if (g_alloc_stats < 0) {
        g_alloc_stats = getenv("QJS_GC_ALLOC_STATS") != NULL;
        if (g_alloc_stats) atexit(alloc_stats_dump);
    }
    if (!g_alloc_stats) return;
    g_atype_n[type & 31]++;
    g_atype_bytes[type & 31] += size;
    size_t i = ((uintptr_t)site >> 2) % ALLOC_SITES;
    for (size_t k = 0; k < ALLOC_SITES; k++, i = (i + 1) % ALLOC_SITES) {
        if (g_asites[i].site == site) { g_asites[i].n++; g_asites[i].bytes += size; return; }
        if (!g_asites[i].site) {
            g_asites[i].site = site; g_asites[i].n = 1; g_asites[i].bytes = size; g_asites[i].type = type;
            return;
        }
    }
}

GCHandle gc_alloc(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    if (unlikely(g_alloc_stats)) alloc_stats_note(__builtin_return_address(0), gc_obj_type, size);
    return gc_alloc_in(size, gc_obj_type, type_starts_old(gc_obj_type), false);
}

GCHandle gc_alloc_old(size_t size, JSGCObjectTypeEnum gc_obj_type) {
    if (unlikely(g_alloc_stats)) alloc_stats_note(__builtin_return_address(0), gc_obj_type, size);
    return gc_alloc_in(size, gc_obj_type, true, false);
}

GCHandle gc_alloc_ex(size_t size, JSGCObjectTypeEnum gc_obj_type, GCHandleArrayType array_type) {
    /* Atoms are permanent: they go to regions no cycle looks at, so the
     * collector never has to mark them. */
    bool atom = array_type == GC_HANDLE_ARRAY_ATOM;
    GCHandle h = gc_alloc_in(size, gc_obj_type, atom || type_starts_old(gc_obj_type), atom);
    if (h == GC_HANDLE_NULL) return h;
    if (array_type == GC_HANDLE_ARRAY_ATOM)
        gc_handle_array_add(&g_gc.atom_handles, h);
    else if (array_type == GC_HANDLE_ARRAY_WEAKREF)
        gc_handle_array_add(&g_gc.weakref_handles, h);
    return h;
}

GCHandle gc_realloc(GCHandle handle, size_t new_size) {
    if (handle == GC_HANDLE_NULL)
        return gc_alloc(new_size, JS_GC_OBJ_TYPE_DATA);
    if (new_size == 0) return GC_HANDLE_NULL;
    void *old_ptr = gc_deref(handle);
    if (!old_ptr) return gc_alloc(new_size, JS_GC_OBJ_TYPE_DATA);
    GCHeader *old_hdr = gc_header(old_ptr);
    JSGCObjectTypeEnum type = gc_header_type(old_hdr);
    size_t old_size = gc_header_payload_size(old_hdr);
    GCRegion *orr = gc_core_region_of(old_hdr);
    bool young = g_core.nursery_enabled && orr && orr->space == GC_RS_NURSERY;
    /* Debug (QJS_GC_REALLOC_MOVES=1): return a new handle and free the old
     * one, as a realloc will once references are addresses, so a caller
     * that keeps using the old handle shows up (with QJS_GC_NO_REUSE the
     * old handle dereferences to NULL). */
    static int moves = -1;
    if (moves < 0) moves = getenv("QJS_GC_REALLOC_MOVES") != NULL;
    if (moves) {
        bool perm = orr && orr->permanent;
        GCHandle nh = gc_alloc_in(new_size, type, !young || perm, perm);
        void *np = gc_deref(nh);
        memcpy(np, old_ptr, old_size < new_size ? old_size : new_size);
        gc_dirty_range(np, old_size < new_size ? old_size : new_size);
        gc_free(handle);
        return nh;
    }
    GCAllocCtx *a = orr && orr->permanent ? &g_core.js_perm : young ? &g_core.js_young : &g_core.js_old;
    GCHeader *hdr = gc_core_alloc_object_memory(a, new_size, type);
    if (!hdr) {
        fprintf(stderr, "[GC] out of memory reallocating %zu bytes\n", new_size);
        abort();
    }
    void *new_ptr = gc_header_payload(hdr);
    size_t n = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, old_ptr, n);
    hdr->handle = handle;
    gc_core_handle_dual_write(handle, new_ptr);
    /* The copy held its references before it was current: a card rescan in
     * between skipped it (and the old copy, no longer current either), so
     * its cards must be dirty again now. */
    gc_dirty_range(new_ptr, n);
#if !GC_SOFTWARE_DIRTY
    for (uintptr_t a = (uintptr_t)new_ptr; a < (uintptr_t)new_ptr + n;
         a = (a & ~(uintptr_t)(GC_OS_PAGE_SIZE - 1)) + GC_OS_PAGE_SIZE)
        atomic_fetch_or_explicit((_Atomic uint32_t *)(a & ~(uintptr_t)3), 0, memory_order_relaxed);
#endif
    gc_core_note_allocation(gc_header_total_size(hdr), young);
    return handle;
}

GCHandle gc_realloc2(GCHandle handle, size_t new_size, size_t *pslack) {
    GCHandle h = gc_realloc(handle, new_size);
    if (pslack && h != GC_HANDLE_NULL) {
        size_t usable = gc_usable_size(h);
        *pslack = usable > new_size ? usable - new_size : 0;
    }
    return h;
}

void gc_free(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return;
    void *p = gc_deref(handle);
    if (!p) return;
    /* CAS: a concurrent sweep may be freeing the same (unreachable) handle. */
    if (gc_core_clear_handle(handle, p)) {
        GC_TRACE(handle, "gc_free");
        if (g_handle_history) gc_core_note_freed(handle, GC_FREED_JS);
        gc_core_free_handle_from_js(handle);    }
}

/* ============================================================================
 * Queries
 * ============================================================================ */

JSGCObjectTypeEnum gc_handle_get_type(GCHandle handle) {
    return gc_handle_get_type_inline(handle);
}

bool (gc_handle_is_valid)(GCHandle handle) {
    return gc_deref(handle) != NULL;
}

bool gc_ptr_in_heap(void *ptr) {
    uintptr_t off = (uintptr_t)ptr - (uintptr_t)g_gc.heap_base;
    return g_gc.heap_base && off < g_gc.heap_reserved;
}

bool gc_ptr_is_valid(void *ptr) {
    return gc_ptr_to_handle(ptr) != GC_HANDLE_NULL;
}

/* The object whose memory (header or payload) contains p, current or not. */
GCHeader *gc_core_object_containing(const void *p) {
    GCRegion *r = gc_core_region_of(p);
    if (!r) return NULL;
    uint32_t st = atomic_load_explicit(&r->state, memory_order_acquire);
    if (st == GC_RS_HUMONGOUS_CONT) {
        r = &g_core.regions[r->humongous_head];
        st = atomic_load_explicit(&r->state, memory_order_acquire);
    }
    if (!gc_core_region_in_use(st)) return NULL;
    uintptr_t a = (uintptr_t)p, lb = gc_core_line_base(r);
    uintptr_t top = atomic_load_explicit(&r->top, memory_order_acquire);
    if (a >= top || a < lb) return NULL;
    if (st == GC_RS_HUMONGOUS) {
        GCHeader *hdr = (GCHeader *)lb;
        size_t total = gc_header_total_size(hdr);
        return total && a < lb + total ? hdr : NULL;
    }
    /* Back to the last line where a header starts at or before p, then
     * forward header by header (a line's worth, for small objects). */
    uint32_t l = (uint32_t)((a - lb) >> GC_LINE_SHIFT);
    if (l >= GC_LINES) return NULL;
    uintptr_t h;
    for (;;) {
        uint8_t b = atomic_load_explicit((_Atomic uint8_t *)&r->line_first[l], memory_order_acquire);
        if (b != GC_LINE_CONT && (h = lb + ((uintptr_t)l << GC_LINE_SHIFT) + ((uintptr_t)b << 4)) <= a) break;
        if (l == 0) return NULL;
        l--;
    }
    for (;;) {
        size_t total = gc_header_total_size((GCHeader *)h);
        if (!total) return NULL;   /* p lies in free space */
        if (a < h + total) return (GCHeader *)h;
        h += total;
    }
}

uintptr_t gc_core_next_header(const GCRegion *r, uintptr_t p, uintptr_t limit) {
    uintptr_t lb = gc_core_line_base(r);
    uint32_t l = (uint32_t)((p - lb) >> GC_LINE_SHIFT);
    if (l < GC_LINES && r->line_first[l] != GC_LINE_CONT) {
        uintptr_t q = lb + ((uintptr_t)l << GC_LINE_SHIFT) + ((uintptr_t)r->line_first[l] << 4);
        if (q >= p) return q < limit ? q : limit;
    }
    return gc_core_skip_free(r, p, limit);
}

GCHandle gc_ptr_to_handle(void *ptr) {
    GCHeader *hdr = gc_core_object_containing(ptr);
    if (!hdr || !gc_core_header_is_current(hdr)) return GC_HANDLE_NULL;
    return hdr->handle;
}

void gc_dirty_range_slow(const void *p, size_t size) {
#if GC_SOFTWARE_DIRTY
    if (!size) return;
    uintptr_t off = (uintptr_t)p - (uintptr_t)g_gc.heap_base;
    if (off >= g_gc.heap_reserved) return;
    uintptr_t last = off + size - 1;
    for (uintptr_t c = off >> GC_CARD_SHIFT; c <= (last >> GC_CARD_SHIFT); c++)
        atomic_store_explicit((_Atomic uint8_t *)&g_gc.cards[c], 1, memory_order_release);
#else
    (void)p; (void)size;
#endif
}

/* ============================================================================
 * Registries: contexts, finalizers, roots (JS thread)
 * ============================================================================ */

/* ============================================================================
 * Retired storage
 * ============================================================================ */

static void **g_retired;
static uint32_t g_retired_n, g_retired_cap;

void gc_core_retire(void *p) {
    if (!p) return;
    if (g_retired_n == g_retired_cap) {
        uint32_t cap = g_retired_cap ? g_retired_cap * 2 : 64;
        void **n = (void **)realloc(g_retired, cap * sizeof(void *));
        if (!n) return;   /* leak rather than free under a reader */
        g_retired = n;
        g_retired_cap = cap;
    }
    g_retired[g_retired_n++] = p;
}

void gc_core_free_retired(void) {
    for (uint32_t i = 0; i < g_retired_n; i++) free(g_retired[i]);
    g_retired_n = 0;
}

/* Grow storage the GC thread may read: copy, publish, retire the old. */
static void *grow_shared(void *old, size_t old_bytes, size_t new_bytes) {
    void *n = calloc(1, new_bytes);
    if (!n) return NULL;
    if (old) memcpy(n, old, old_bytes);
    gc_core_retire(old);
    return n;
}

void gc_core_registry_add(GCRegistry *reg, GCHandle h) {
    if (reg->count == reg->cap) {
        uint32_t cap = reg->cap ? reg->cap * 2 : 16;
        GCRegistryEntry *n = (GCRegistryEntry *)grow_shared(reg->items, reg->cap * sizeof(*n),
                                                            cap * sizeof(*n));
        if (!n) return;
        atomic_store_explicit((_Atomic(GCRegistryEntry *) *)&reg->items, n, memory_order_release);
        reg->cap = cap;
    }
    reg->items[reg->count].handle = h;
    reg->items[reg->count].fn = NULL;
    atomic_store_explicit((_Atomic uint32_t *)&reg->count, reg->count + 1, memory_order_release);
}

void gc_set_handle_finalizer(GCHandle handle, GCFinalizerFunc *finalizer) {
    if (handle == GC_HANDLE_NULL) return;
    for (uint32_t i = 0; i < g_core.finalizers.count; i++) {
        if (g_core.finalizers.items[i].handle == handle) {
            g_core.finalizers.items[i].fn = finalizer;
            return;
        }
    }
    gc_core_registry_add(&g_core.finalizers, handle);
    if (g_core.finalizers.count)
        g_core.finalizers.items[g_core.finalizers.count - 1].fn = finalizer;
}

/* gc_set_handle_finalizer() for a handle known to have none (a new object):
 * no search of the registry. */
void gc_add_handle_finalizer(GCHandle handle, GCFinalizerFunc *finalizer) {
    if (handle == GC_HANDLE_NULL) return;
    gc_core_registry_add(&g_core.finalizers, handle);
    if (g_core.finalizers.count)
        g_core.finalizers.items[g_core.finalizers.count - 1].fn = finalizer;
}

GCFinalizerFunc *gc_get_handle_finalizer(GCHandle handle) {
    for (uint32_t i = 0; i < g_core.finalizers.count; i++)
        if (g_core.finalizers.items[i].handle == handle)
            return g_core.finalizers.items[i].fn;
    return NULL;
}

bool gc_add_root(GCHandle handle) {
    if (handle == GC_HANDLE_NULL) return true;
    gc_core_registry_add(&g_core.roots, handle);
    return true;
}

void gc_remove_root(GCHandle handle) {
    /* From the end: roots are mostly removed in the reverse order they were
     * added (the parser's scoped pairs), so the match is near the top. */
    for (uint32_t i = g_core.roots.count; i-- > 0;) {
        if (g_core.roots.items[i].handle == handle) {
            g_core.roots.items[i] = g_core.roots.items[--g_core.roots.count];
            return;
        }
    }
}

void gc_core_walk_region(GCRegion *r, uintptr_t from, uintptr_t to,
                         void (*fn)(GCHeader *hdr, void *opaque), void *opaque) {
    uint8_t *p = (uint8_t *)from;
    while ((uintptr_t)p < to) {
        GCHeader *hdr = (GCHeader *)p;
        size_t total = gc_header_total_size(hdr);
        if (!total) { p = (uint8_t *)gc_core_skip_free(r, (uintptr_t)p, to); continue; }
        fn(hdr, opaque);
        p += total;
    }
}

typedef struct TypeWalk {
    JSGCObjectTypeEnum type;
    GCTypeIteratorFunc func;
    void *user_data;
} TypeWalk;

static void type_walk_cb(GCHeader *hdr, void *opaque) {
    TypeWalk *w = (TypeWalk *)opaque;
    if (gc_header_type(hdr) == w->type && gc_core_header_is_current(hdr))
        w->func(hdr->handle, w->user_data);
}

void gc_for_each_object_of_type(JSGCObjectTypeEnum type, GCTypeIteratorFunc func, void *user_data) {
    if (!func || !g_gc.initialized) return;
    if (type == JS_GC_OBJ_TYPE_JS_CONTEXT) {
        for (uint32_t i = 0; i < g_core.contexts.count; i++) {
            GCHandle h = g_core.contexts.items[i].handle;
            if (gc_deref(h) && gc_handle_get_type_inline(h) == JS_GC_OBJ_TYPE_JS_CONTEXT)
                func(h, user_data);
        }
        return;
    }
    TypeWalk w = { type, func, user_data };
    uint32_t hwm = atomic_load_explicit(&g_core.region_hwm, memory_order_acquire);
    for (uint32_t i = 0; i < hwm; i++) {
        GCRegion *r = &g_core.regions[i];
        uint32_t st = atomic_load_explicit(&r->state, memory_order_acquire);
        if (!gc_core_region_in_use(st)) continue;
        gc_core_walk_region(r, (uintptr_t)gc_core_region_start(r) + GC_HEADER_SIZE,
                            atomic_load_explicit(&r->top, memory_order_acquire), type_walk_cb, &w);
    }
}

static void count_cb(GCHandle h, void *d) { (void)h; (*(uint32_t *)d)++; }

uint32_t gc_count_objects_of_type(JSGCObjectTypeEnum type) {
    uint32_t n = 0;
    gc_for_each_object_of_type(type, count_cb, &n);
    return n;
}

/* ============================================================================
 * Handle arrays (engine registries)
 * ============================================================================ */

int gc_handle_array_init(JSHandleArray *arr, uint32_t capacity) {
    if (!arr) return -1;
    if (capacity == 0) capacity = 16;
    arr->handles = (GCHandle *)calloc(capacity, sizeof(GCHandle));
    if (!arr->handles) return -1;
    arr->count = 0;
    arr->capacity = capacity;
    return 0;
}

void gc_handle_array_free(JSHandleArray *arr) {
    if (!arr) return;
    free(arr->handles);
    arr->handles = NULL;
    arr->count = 0;
    arr->capacity = 0;
}

int gc_handle_array_add(JSHandleArray *arr, GCHandle handle) {
    return gc_handle_array_push_with_index(arr, handle, NULL);
}

int gc_handle_array_push_with_index(JSHandleArray *arr, GCHandle handle, uint32_t *out_index) {
    if (!arr || handle == GC_HANDLE_NULL) return -1;
    if (arr->count >= arr->capacity) {
        /* The GC thread may be reading the array (weak processing). */
        uint32_t cap = arr->capacity ? arr->capacity * 2 : 16;
        GCHandle *n = (GCHandle *)grow_shared(arr->handles, arr->capacity * sizeof(GCHandle),
                                              cap * sizeof(GCHandle));
        if (!n) return -1;
        atomic_store_explicit((_Atomic(GCHandle *) *)&arr->handles, n, memory_order_release);
        arr->capacity = cap;
    }
    uint32_t index = arr->count;
    arr->handles[index] = handle;
    atomic_store_explicit((_Atomic uint32_t *)&arr->count, index + 1, memory_order_release);
    if (out_index) *out_index = index;
    return 0;
}

void gc_handle_array_remove(JSHandleArray *arr, GCHandle handle) {
    if (!arr || handle == GC_HANDLE_NULL) return;
    for (uint32_t i = 0; i < arr->count; i++) {
        if (arr->handles[i] == handle) {
            arr->handles[i] = GC_HANDLE_FREED;
            return;
        }
    }
}

void gc_handle_array_compact(JSHandleArray *arr) {
    if (!arr) return;
    uint32_t j = 0;
    for (uint32_t i = 0; i < arr->count; i++) {
        GCHandle h = arr->handles[i];
        if (!gc_handle_array_entry_is_valid(h) || !gc_deref(h)) continue;
        arr->handles[j++] = h;
    }
    arr->count = j;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

bool gc_init(void) {
    if (g_gc.initialized) return true;
    memset(&g_core, 0, sizeof(g_core));

    const char *lv = getenv("QJS_GC_LOG");
    g_core.log_level = lv ? atoi(lv) : 0;
    g_core.poison = getenv("QJS_GC_POISON") != NULL;
    {
        const char *hz = getenv("QJS_GC_HOLES");
        g_core.holes = !hz || atoi(hz) != 0;
    }
    g_core.trace_handle = getenv("QJS_GC_TRACE") != NULL;
    g_handle_no_reuse = getenv("QJS_GC_NO_REUSE") != NULL;
    if (g_handle_no_reuse || getenv("QJS_GC_HISTORY")) {
        g_handle_history = (GCHandleHistory *)gc_os_reserve((size_t)GC_MAX_HANDLES * sizeof(GCHandleHistory), false);
        g_handle_history_committed = 0;
        if (g_handle_no_reuse)
            fprintf(stderr, "[GC] QJS_GC_NO_REUSE: freed handles stay dead%s\n",
                    GC_STALE_CHECK ? ", dead dereferences are reported at exit" : "");
#if GC_STALE_CHECK
        if (g_handle_no_reuse) atexit(gc_debug_stale_dump);
#endif
    }

    /* Reserve one extra region so the base can be region-aligned. */
    size_t reserve = GC_HEAP_RESERVE + GC_REGION_SIZE;
    g_core.heap_reservation = gc_os_reserve(reserve, gc_os_has_write_watch());
    if (!g_core.heap_reservation) {
        fprintf(stderr, "[GC] cannot reserve %zu bytes of address space\n", reserve);
        return false;
    }
    uintptr_t b = (uintptr_t)g_core.heap_reservation;
    g_gc.heap_base = (uint8_t *)((b + GC_REGION_SIZE - 1) & ~((uintptr_t)GC_REGION_SIZE - 1));
    g_gc.heap_reserved = GC_HEAP_RESERVE;

    g_core.regions = (GCRegion *)calloc(GC_MAX_REGIONS, sizeof(GCRegion));
#if GC_SOFTWARE_DIRTY
    size_t card_bytes = GC_HEAP_RESERVE >> GC_CARD_SHIFT;
    g_gc.cards = (uint8_t *)gc_os_reserve(card_bytes, false);
    if (g_gc.cards && !gc_os_commit(g_gc.cards, card_bytes)) g_gc.cards = NULL;
#endif
    for (int t = 0; t < 2; t++)
        g_gc.tables[t] = (void **)gc_os_reserve((size_t)GC_MAX_HANDLES * sizeof(void *), false);
    if (!g_core.regions || !g_gc.tables[0] || !g_gc.tables[1]
#if GC_SOFTWARE_DIRTY
        || !g_gc.cards
#endif
        ) {
        fprintf(stderr, "[GC] initialization failed\n");
        return false;
    }
    atomic_store(&g_gc.front, g_gc.tables[0]);
    g_gc.committed_handles = 0;
    if (!handle_tables_commit_to(HANDLE_COMMIT_CHUNK)) return false;
    atomic_store(&g_gc.high_water, 1);  /* handle 0 is GC_HANDLE_NULL */

    g_core.js_young.space = GC_RS_NURSERY;
    g_core.js_young.owner = GC_OWNER_JS;
    g_core.js_old.space = GC_RS_OLD;
    g_core.js_old.recycles = true;
    g_core.js_old.owner = GC_OWNER_JS;
    g_core.js_perm.space = GC_RS_OLD;
    g_core.js_perm.owner = GC_OWNER_JS;
    g_core.js_perm.permanent = true;
    g_core.gc_copy.space = GC_RS_OLD;
    g_core.gc_copy.recycles = true;
    g_core.gc_copy.owner = GC_OWNER_GC;
    g_core.js_old_overflow.space = GC_RS_OLD;
    g_core.js_old_overflow.owner = GC_OWNER_JS;
    g_core.js_old.overflow = &g_core.js_old_overflow;
    g_core.gc_copy_overflow.space = GC_RS_OLD;
    g_core.gc_copy_overflow.owner = GC_OWNER_GC;
    g_core.gc_copy.overflow = &g_core.gc_copy_overflow;

    gc_handle_array_init(&g_gc.atom_handles, 1024);
    gc_handle_array_init(&g_gc.weakmap_handles, 64);
    gc_handle_array_init(&g_gc.weakref_handles, 64);
    gc_handle_array_init(&g_gc.finrec_handles, 64);

    g_core.js_thread_id = gc_os_thread_id();
    gc_core_config_defaults();
    g_gc_epoch++;
    g_gc.initialized = true;
    return true;
}

bool gc_is_initialized(void) { return g_gc.initialized; }

void gc_cleanup(void) {
    if (!g_gc.initialized) return;
    gc_core_stop_thread();
    gc_core_free_internal();
    gc_handle_array_free(&g_gc.atom_handles);
    gc_handle_array_free(&g_gc.weakmap_handles);
    gc_handle_array_free(&g_gc.weakref_handles);
    gc_handle_array_free(&g_gc.finrec_handles);
    for (uint32_t i = 0; i < GC_MAX_REGIONS; i++) {
        GCRegion *r = &g_core.regions[i];
        free(r->line_first);
        free(r->mark_bits[0]);
        free(r->mark_bits[1]);
    }
    free(g_core.regions);
    free(g_core.hcache);
    gc_core_free_retired();
    free(g_retired);
    g_retired = NULL;
    g_retired_cap = 0;
    free(g_core.contexts.items);
    free(g_core.finalizers.items);
    free(g_core.roots.items);
    gc_os_release(g_core.heap_reservation, GC_HEAP_RESERVE + GC_REGION_SIZE);
    for (int t = 0; t < 2; t++)
        gc_os_release(g_gc.tables[t], (size_t)GC_MAX_HANDLES * sizeof(void *));
#if GC_SOFTWARE_DIRTY
    gc_os_release(g_gc.cards, GC_HEAP_RESERVE >> GC_CARD_SHIFT);
#endif
    memset(&g_core, 0, sizeof(g_core));
    memset(&g_gc, 0, sizeof(g_gc));
}

/* The GC thread runs while a runtime is attached. */
void gc_set_runtime(JSRuntimeHandle rt) {
    if (rt == GC_HANDLE_NULL) {
        gc_core_stop_thread();
        g_gc.rt = GC_HANDLE_NULL;
        return;
    }
    g_gc.rt = rt;
    gc_core_start_thread();
}

void gc_background_stop(void) { gc_core_stop_thread(); }
JSRuntimeHandle gc_get_runtime(void) { return (JSRuntimeHandle)g_gc.rt; }

size_t gc_used_bytes(void) {
    GCStats s;
    gc_get_stats(&s);
    return s.heap_used;
}

uint32_t gc_get_handle_count(void) {
    return gc_handle_limit();
}

/* ============================================================================
 * GC-safe linked lists
 * ============================================================================ */

static inline GCListHead *gc_list_head_from_handle(GCHandle handle, size_t offset) {
    if (handle == GC_HANDLE_NULL) return NULL;
    void *ptr = gc_deref(handle);
    return ptr ? (GCListHead *)((uint8_t *)ptr + offset) : NULL;
}

void gc_list_add_between(GCHandle new_node, GCHandle prev, GCHandle next, size_t link_offset) {
    GCListHead *new_link = gc_list_head_from_handle(new_node, link_offset);
    if (!new_link) return;
    new_link->prev = prev;
    new_link->next = next;
    gc_dirty_slot(new_link);
    if (prev != GC_HANDLE_NULL) {
        GCListHead *prev_link = gc_list_head_from_handle(prev, link_offset);
        if (prev_link) {
            prev_link->next = new_node;
            gc_dirty_slot(&prev_link->next);
        }
    }
    if (next != GC_HANDLE_NULL) {
        GCListHead *next_link = gc_list_head_from_handle(next, link_offset);
        if (next_link) {
            next_link->prev = new_node;
            gc_dirty_slot(&next_link->prev);
        }
    }
}

void gc_list_add(GCHandle new_node, struct GCListHead *head, size_t link_offset) {
    if (!head || new_node == GC_HANDLE_NULL) return;
    GCHandle first = head->next;
    gc_list_add_between(new_node, GC_HANDLE_NULL, first, link_offset);
    head->next = new_node;
    if (head->prev == GC_HANDLE_NULL)
        head->prev = new_node;
    gc_dirty_slot(head);
}

void gc_list_add_tail(GCHandle new_node, struct GCListHead *head, size_t link_offset) {
    if (!head || new_node == GC_HANDLE_NULL) return;
    GCHandle last = head->prev;
    gc_list_add_between(new_node, last, GC_HANDLE_NULL, link_offset);
    head->prev = new_node;
    if (head->next == GC_HANDLE_NULL)
        head->next = new_node;
    gc_dirty_slot(head);
}

void gc_list_del(GCHandle node, struct GCListHead *head, size_t link_offset) {
    if (!head || node == GC_HANDLE_NULL) return;
    GCListHead *link = gc_list_head_from_handle(node, link_offset);
    if (!link) return;
    GCHandle prev = link->prev;
    GCHandle next = link->next;
    if (prev != GC_HANDLE_NULL) {
        GCListHead *prev_link = gc_list_head_from_handle(prev, link_offset);
        if (prev_link) {
            prev_link->next = next;
            gc_dirty_slot(&prev_link->next);
        }
    } else {
        head->next = next;
    }
    if (next != GC_HANDLE_NULL) {
        GCListHead *next_link = gc_list_head_from_handle(next, link_offset);
        if (next_link) {
            next_link->prev = prev;
            gc_dirty_slot(&next_link->prev);
        }
    } else {
        head->prev = prev;
    }
    gc_dirty_slot(head);
    link->prev = GC_HANDLE_NULL;
    link->next = GC_HANDLE_NULL;
}

void gc_list_add_tail_in_container(GCHandle new_node, GCHandle container,
                                   size_t list_offset, size_t link_offset) {
    if (container == GC_HANDLE_NULL || new_node == GC_HANDLE_NULL) return;
    GCListHead *head = (GCListHead *)((uint8_t *)gc_deref(container) + list_offset);
    gc_list_add_tail(new_node, head, link_offset);
}

void gc_list_del_in_container(GCHandle node, GCHandle container,
                              size_t list_offset, size_t link_offset) {
    if (container == GC_HANDLE_NULL || node == GC_HANDLE_NULL) return;
    GCListHead *head = (GCListHead *)((uint8_t *)gc_deref(container) + list_offset);
    gc_list_del(node, head, link_offset);
}

/* ============================================================================
 * GCDataArray (see quickjs_types.h)
 * ============================================================================ */

bool gc_data_array_resize(GCDataArray *arr, size_t elem_size, int new_count)
{
    if (new_count <= arr->capacity) {
        /* Zero new elements when growing within capacity */
        if (new_count > arr->count) {
            uint8_t *p = (uint8_t *)GC_DATA_ARRAY_PTR(*arr, void);
            if (p) memset(p + elem_size * arr->count, 0, elem_size * (new_count - arr->count));
        }
        arr->count = new_count;
        return true;
    }
    int new_cap = new_count > arr->capacity * 3 / 2 ? new_count : arr->capacity * 3 / 2;
    if (new_cap < 4) new_cap = 4;

    GCHandle new_handle = gc_alloc(elem_size * new_cap, JS_GC_OBJ_TYPE_DATA);
    if (new_handle == GC_HANDLE_NULL) return false;
    uint8_t *new_data = (uint8_t *)gc_deref(new_handle);
    if (!new_data) return false;

    uint8_t *old_data = (uint8_t *)GC_DATA_ARRAY_PTR(*arr, void);
    if (old_data && arr->count > 0) {
        memcpy(new_data, old_data, elem_size * arr->count);
        gc_dirty_range(new_data, elem_size * arr->count);
    }

    arr->handle = new_handle;
    gc_dirty_slot(&arr->handle);
    arr->capacity = new_cap;
    arr->count = new_count;
    return true;
}

void *gc_data_array_prepare_push(GCDataArray *arr, size_t elem_size)
{
    if (!gc_data_array_resize(arr, elem_size, arr->count + 1))
        return NULL;
    uint8_t *p = (uint8_t *)GC_DATA_ARRAY_PTR(*arr, void);
    return p ? p + elem_size * (arr->count - 1) : NULL;
}

bool gc_data_array_push(GCDataArray *arr, size_t elem_size, const void *elem)
{
    void *slot = gc_data_array_prepare_push(arr, elem_size);
    if (!slot) return false;
    memcpy(slot, elem, elem_size);
    gc_dirty_range(slot, elem_size);
    return true;
}
