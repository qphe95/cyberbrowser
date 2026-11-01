/* glyph_atlas.c - one glyph atlas for a tab's text (glyph_atlas.h).
 *
 * Each page is cut, top to bottom, into bands: shelves holding glyphs of one
 * height class side by side, and free bands.  A glyph goes on the first
 * shelf of its class with room, else on a new shelf cut from the top of the
 * smallest free band that holds it.  Row 0 and column 0 of a page stay
 * empty, and each glyph has an empty column right of it and row below (the
 * shelf's last), so every glyph is framed by empty texels: sampling just
 * past its edge reads nothing, wherever it is.  Evicting a shelf frees its whole band
 * (merged with free neighbours); its entries' generations move on, so refs
 * to them stop resolving. */
#include "glyph_atlas.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int      page;
    int      y, h;        /* rows [y, y + h) of the page */
    int      x;           /* next free column (a shelf) */
    bool     used;        /* a shelf; else a free band */
    uint32_t stamp;       /* newest stamp of its entries */
    int      first;       /* its entries, through Entry.next */
    int      next_free;   /* the pool's free list */
} Shelf;

typedef struct {
    int32_t  shelf;       /* -1: free */
    uint16_t x, y, w, h;
    uint32_t stamp;
    uint32_t gen;
    int32_t  next;        /* in its shelf, or the free list */
} Entry;

typedef struct {
    uint8_t *px;
    int     *order;       /* shelf ids by y, covering the page */
    int      n, cap;
    int      dx0, dy0, dx1, dy1;   /* dirty; dx1 <= dx0: none */
} Page;

struct GlyphAtlas {
    int      size, budget;
    Page     pages[GLYPH_ATLAS_MAX_PAGES];
    int      npages;
    Shelf   *shelves;
    int      nshelves, cap_shelves, free_shelf;
    Entry   *entries;
    int      nentries, cap_entries, free_entry;
    uint32_t frame, last_sent;
    uint64_t evictions;
};

GlyphAtlas *glyph_atlas_create(int page_size, int budget)
{
    if (page_size < 16 || page_size > 65535) return NULL;
    if (budget < 1) budget = 1;
    if (budget > GLYPH_ATLAS_MAX_PAGES) budget = GLYPH_ATLAS_MAX_PAGES;
    GlyphAtlas *a = (GlyphAtlas *)calloc(1, sizeof(GlyphAtlas));
    if (!a) return NULL;
    a->size = page_size;
    a->budget = budget;
    a->free_shelf = -1;
    a->free_entry = -1;
    a->frame = 1;
    return a;
}

void glyph_atlas_destroy(GlyphAtlas *a)
{
    if (!a) return;
    for (int i = 0; i < a->npages; i++) {
        free(a->pages[i].px);
        free(a->pages[i].order);
    }
    free(a->shelves);
    free(a->entries);
    free(a);
}

static int shelf_new(GlyphAtlas *a, int page, int y, int h, bool used)
{
    int id = a->free_shelf;
    if (id >= 0) {
        a->free_shelf = a->shelves[id].next_free;
    } else {
        if (a->nshelves == a->cap_shelves) {
            int cap = a->cap_shelves ? a->cap_shelves * 2 : 64;
            Shelf *ns = (Shelf *)realloc(a->shelves, (size_t)cap * sizeof(Shelf));
            if (!ns) return -1;
            a->shelves = ns;
            a->cap_shelves = cap;
        }
        id = a->nshelves++;
    }
    Shelf *s = &a->shelves[id];
    memset(s, 0, sizeof(*s));
    s->page = page;
    s->y = y;
    s->h = h;
    s->used = used;
    s->first = -1;
    s->next_free = -1;
    return id;
}

static void shelf_release(GlyphAtlas *a, int id)
{
    a->shelves[id].next_free = a->free_shelf;
    a->free_shelf = id;
}

static bool order_insert(Page *p, int at, int id)
{
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 32;
        int *no = (int *)realloc(p->order, (size_t)cap * sizeof(int));
        if (!no) return false;
        p->order = no;
        p->cap = cap;
    }
    memmove(&p->order[at + 1], &p->order[at], (size_t)(p->n - at) * sizeof(int));
    p->order[at] = id;
    p->n++;
    return true;
}

static void order_remove(Page *p, int at)
{
    memmove(&p->order[at], &p->order[at + 1], (size_t)(p->n - at - 1) * sizeof(int));
    p->n--;
}

static void mark_dirty(Page *p, int x0, int y0, int x1, int y1)
{
    if (x1 <= x0 || y1 <= y0) return;
    if (p->dx1 <= p->dx0) {
        p->dx0 = x0; p->dy0 = y0; p->dx1 = x1; p->dy1 = y1;
        return;
    }
    if (x0 < p->dx0) p->dx0 = x0;
    if (y0 < p->dy0) p->dy0 = y0;
    if (x1 > p->dx1) p->dx1 = x1;
    if (y1 > p->dy1) p->dy1 = y1;
}

static bool page_add(GlyphAtlas *a)
{
    if (a->npages >= a->budget) return false;
    Page *p = &a->pages[a->npages];
    memset(p, 0, sizeof(*p));
    p->px = (uint8_t *)calloc(1, (size_t)a->size * (size_t)a->size);
    if (!p->px) return false;
    int id = shelf_new(a, a->npages, 1, a->size - 1, false);
    if (id < 0 || !order_insert(p, 0, id)) {
        free(p->px);
        free(p->order);
        memset(p, 0, sizeof(*p));
        return false;
    }
    a->npages++;
    return true;
}

/* Shelf heights come in steps of 4 rows (a row of gutter included), so a
 * freed shelf suits the glyphs of nearby sizes. */
static int height_class(int h)
{
    return (h + 1 + 3) & ~3;
}

/* An entry for w x h at column x of shelf id. */
static int entry_new(GlyphAtlas *a, int shelf_id, int w, int h)
{
    int id = a->free_entry;
    if (id >= 0) {
        a->free_entry = a->entries[id].next;
    } else {
        if (a->nentries == a->cap_entries) {
            int cap = a->cap_entries ? a->cap_entries * 2 : 256;
            Entry *ne = (Entry *)realloc(a->entries, (size_t)cap * sizeof(Entry));
            if (!ne) return -1;
            a->entries = ne;
            a->cap_entries = cap;
        }
        id = a->nentries++;
        a->entries[id].gen = 0;
    }
    Shelf *s = &a->shelves[shelf_id];
    Entry *e = &a->entries[id];
    e->shelf = shelf_id;
    e->x = (uint16_t)s->x;
    e->y = (uint16_t)s->y;
    e->w = (uint16_t)w;
    e->h = (uint16_t)h;
    e->stamp = a->frame;
    e->next = s->first;
    s->first = id;
    s->x += w + 1;
    s->stamp = a->frame;
    return id;
}

/* A shelf of class H with room for w columns. */
static int find_shelf(GlyphAtlas *a, int w, int H)
{
    for (int pg = 0; pg < a->npages; pg++) {
        Page *p = &a->pages[pg];
        for (int i = 0; i < p->n; i++) {
            Shelf *s = &a->shelves[p->order[i]];
            if (s->used && s->h == H && s->x + w + 1 <= a->size) return p->order[i];
        }
    }
    return -1;
}

/* A new shelf of height H cut from the smallest free band that holds it. */
static int cut_shelf(GlyphAtlas *a, int H)
{
    int best = -1, best_pg = -1, best_at = -1;
    for (int pg = 0; pg < a->npages; pg++) {
        Page *p = &a->pages[pg];
        for (int i = 0; i < p->n; i++) {
            Shelf *s = &a->shelves[p->order[i]];
            if (!s->used && s->h >= H && (best < 0 || s->h < a->shelves[best].h)) {
                best = p->order[i];
                best_pg = pg;
                best_at = i;
            }
        }
    }
    if (best < 0) return -1;
    Shelf *s = &a->shelves[best];
    if (s->h > H) {
        int rest = shelf_new(a, best_pg, s->y + H, s->h - H, false);
        if (rest < 0) return -1;
        s = &a->shelves[best];                    /* (the pool may have moved) */
        if (!order_insert(&a->pages[best_pg], best_at + 1, rest)) {
            shelf_release(a, rest);
            return -1;
        }
        s->h = H;
    }
    s->used = true;
    s->x = 1;
    s->first = -1;
    s->stamp = a->frame;
    return best;
}

/* Shelf at position `at` of page pg becomes a free band, its pixels cleared
 * (and dirty) and its entries gone; merged with free neighbours. */
static void evict(GlyphAtlas *a, int pg, int at)
{
    Page *p = &a->pages[pg];
    int id = p->order[at];
    Shelf *s = &a->shelves[id];
    for (int e = s->first; e >= 0;) {
        Entry *en = &a->entries[e];
        int next = en->next;
        en->shelf = -1;
        en->gen++;
        en->next = a->free_entry;
        a->free_entry = e;
        e = next;
    }
    int cols = s->x < a->size ? s->x : a->size;
    for (int y = s->y; y < s->y + s->h; y++) memset(p->px + (size_t)y * a->size, 0, (size_t)cols);
    mark_dirty(p, 0, s->y, cols, s->y + s->h);
    s->used = false;
    s->first = -1;
    s->x = 1;
    a->evictions++;
    if (at + 1 < p->n && !a->shelves[p->order[at + 1]].used) {
        s->h += a->shelves[p->order[at + 1]].h;
        shelf_release(a, p->order[at + 1]);
        order_remove(p, at + 1);
    }
    if (at > 0 && !a->shelves[p->order[at - 1]].used) {
        a->shelves[p->order[at - 1]].h += s->h;
        shelf_release(a, id);
        order_remove(p, at);
    }
}

/* Evict the least recently used shelf that no frame on screen can need:
 * stamped before the last frame sent.  False when there is none. */
static bool evict_lru(GlyphAtlas *a)
{
    int best_pg = -1, best_at = -1;
    uint32_t best_stamp = 0;
    for (int pg = 0; pg < a->npages; pg++) {
        Page *p = &a->pages[pg];
        for (int i = 0; i < p->n; i++) {
            Shelf *s = &a->shelves[p->order[i]];
            if (s->used && s->stamp < a->last_sent && (best_pg < 0 || s->stamp < best_stamp)) {
                best_pg = pg;
                best_at = i;
                best_stamp = s->stamp;
            }
        }
    }
    if (best_pg < 0) return false;
    evict(a, best_pg, best_at);
    return true;
}

bool glyph_atlas_alloc(GlyphAtlas *a, int w, int h, GaRef *ref, GaRect *out)
{
    if (!a || w <= 0 || h <= 0 || w + 2 > a->size || h + 2 > a->size) return false;
    int H = height_class(h);
    if (H > a->size - 1) H = a->size - 1;
    int shelf = find_shelf(a, w, H);
    if (shelf < 0) shelf = cut_shelf(a, H);
    while (shelf < 0 && evict_lru(a)) shelf = cut_shelf(a, H);
    if (shelf < 0 && page_add(a)) shelf = cut_shelf(a, H);
    if (shelf < 0) return false;
    int id = entry_new(a, shelf, w, h);
    if (id < 0) return false;
    const Shelf *s = &a->shelves[shelf];
    const Entry *e = &a->entries[id];
    Page *p = &a->pages[s->page];
    for (int y = e->y; y < e->y + h; y++) memset(p->px + (size_t)y * a->size + e->x, 0, (size_t)w);
    mark_dirty(p, e->x, e->y, e->x + w, e->y + h);
    ref->id = id;
    ref->gen = e->gen;
    out->page = s->page;
    out->x = e->x;
    out->y = e->y;
    out->w = w;
    out->h = h;
    return true;
}

bool glyph_atlas_get(GlyphAtlas *a, GaRef ref, GaRect *out)
{
    if (!a || ref.id < 0 || ref.id >= a->nentries) return false;
    Entry *e = &a->entries[ref.id];
    if (e->shelf < 0 || e->gen != ref.gen) return false;
    Shelf *s = &a->shelves[e->shelf];
    e->stamp = a->frame;
    s->stamp = a->frame;
    out->page = s->page;
    out->x = e->x;
    out->y = e->y;
    out->w = e->w;
    out->h = e->h;
    return true;
}

void glyph_atlas_touch(GlyphAtlas *a, int page, int y)
{
    if (!a || page < 0 || page >= a->npages || y < 0 || y >= a->size) return;
    Page *p = &a->pages[page];
    int lo = 0, hi = p->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        Shelf *s = &a->shelves[p->order[mid]];
        if (y < s->y) hi = mid - 1;
        else if (y >= s->y + s->h) lo = mid + 1;
        else {
            if (s->used) s->stamp = a->frame;
            return;
        }
    }
}

void glyph_atlas_frame_sent(GlyphAtlas *a)
{
    if (!a) return;
    a->last_sent = a->frame;
    a->frame++;
}

uint32_t glyph_atlas_frame(const GlyphAtlas *a) { return a ? a->frame : 0; }
int glyph_atlas_page_size(const GlyphAtlas *a) { return a ? a->size : 0; }
int glyph_atlas_page_count(const GlyphAtlas *a) { return a ? a->npages : 0; }
uint64_t glyph_atlas_evictions(const GlyphAtlas *a) { return a ? a->evictions : 0; }

uint8_t *glyph_atlas_page_pixels(const GlyphAtlas *a, int page)
{
    return a && page >= 0 && page < a->npages ? a->pages[page].px : NULL;
}

int glyph_atlas_page_used_rows(const GlyphAtlas *a, int page)
{
    if (!a || page < 0 || page >= a->npages) return 0;
    const Page *p = &a->pages[page];
    for (int i = p->n - 1; i >= 0; i--) {
        const Shelf *s = &a->shelves[p->order[i]];
        if (s->used) return s->y + s->h;
    }
    return 0;
}

bool glyph_atlas_take_dirty(GlyphAtlas *a, int page, int *x0, int *y0, int *x1, int *y1)
{
    if (!a || page < 0 || page >= a->npages) return false;
    Page *p = &a->pages[page];
    if (p->dx1 <= p->dx0) return false;
    *x0 = p->dx0;
    *y0 = p->dy0;
    *x1 = p->dx1;
    *y1 = p->dy1;
    p->dx0 = p->dx1 = p->dy0 = p->dy1 = 0;
    return true;
}
