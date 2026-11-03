/*
 * dom_range.c - the store of range boundary points (dom_range.h).
 *
 * A row's fields are in flat arrays (a structure of arrays); whether a row
 * is in use and whether it is live are bitsets.  Free rows are kept on a
 * free list through next[].  The hooks walk the rows in use: a page keeps
 * few ranges alive, and none costs nothing.
 */
#include "dom_range.h"

#include <stdlib.h>
#include <string.h>

static struct {
    int n, cap, free_head;      /* rows used (high water), allocated, first free + 1 */
    NodeId *sn, *en;
    uint32_t *so, *eo;
    int *next;
    uint64_t *used, *live;
} R;

uint32_t g_dom_ranges_live;

static inline bool bit(const uint64_t *b, int i) { return (b[i >> 6] >> (i & 63)) & 1; }
static inline void bset(uint64_t *b, int i, bool on)
{
    if (on) b[i >> 6] |= 1ull << (i & 63);
    else b[i >> 6] &= ~(1ull << (i & 63));
}

static bool grow(void)
{
    int cap = R.cap ? R.cap * 2 : 64;
#define G(f) do { void *q = realloc((void *)R.f, (size_t)cap * sizeof(*R.f)); if (!q) return false; R.f = q; } while (0)
    G(sn); G(en); G(so); G(eo); G(next);
#undef G
    size_t words = (size_t)cap / 64, old = (size_t)R.cap / 64;
    uint64_t **bits[] = { &R.used, &R.live };
    for (size_t i = 0; i < 2; i++) {
        uint64_t *q = (uint64_t *)realloc(*bits[i], words * sizeof(uint64_t));
        if (!q) return false;
        memset(q + old, 0, (words - old) * sizeof(uint64_t));
        *bits[i] = q;
    }
    R.cap = cap;
    return true;
}

int dom_range_new(NodeId n, uint32_t off, bool live)
{
    int r;
    if (R.free_head) {
        r = R.free_head - 1;
        R.free_head = R.next[r];
    } else {
        if (R.n == R.cap && !grow()) return -1;
        r = R.n++;
    }
    R.sn[r] = R.en[r] = n;
    R.so[r] = R.eo[r] = off;
    bset(R.used, r, true);
    bset(R.live, r, live);
    if (live) g_dom_ranges_live++;
    return r;
}

void dom_range_free(int r)
{
    if (r < 0 || r >= R.n || !bit(R.used, r)) return;
    if (bit(R.live, r)) g_dom_ranges_live--;
    bset(R.used, r, false);
    bset(R.live, r, false);
    R.next[r] = R.free_head;
    R.free_head = r + 1;
}

void dom_range_get(int r, NodeId *sn, uint32_t *so, NodeId *en, uint32_t *eo)
{
    bool ok = r >= 0 && r < R.n && bit(R.used, r);
    *sn = ok ? R.sn[r] : DOM_NULL;
    *so = ok ? R.so[r] : 0;
    *en = ok ? R.en[r] : DOM_NULL;
    *eo = ok ? R.eo[r] : 0;
}

void dom_range_set(int r, NodeId sn, uint32_t so, NodeId en, uint32_t eo)
{
    if (r < 0 || r >= R.n || !bit(R.used, r)) return;
    R.sn[r] = sn;
    R.so[r] = so;
    R.en[r] = en;
    R.eo[r] = eo;
}

bool dom_range_is_live(int r)
{
    return r >= 0 && r < R.n && bit(R.live, r);
}

uint32_t dom_utf16_len(const char *s, size_t n)
{
    uint32_t u = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) u++;
        if (c >= 0xF0) u++;     /* (a supplementary character: two units) */
    }
    return u;
}

/* ---- The hooks (DOM 4.2.3 insert, 4.2.3 remove, 4.10 replace data,
 * 4.11 split a Text node) ---- */

#define FOR_LIVE(r) for (int r = 0; r < R.n; r++) if (bit(R.live, r))

void dom_range_inserted(NodeId parent, uint32_t index, uint32_t count)
{
    FOR_LIVE(r) {
        if (R.sn[r] == parent && R.so[r] > index) R.so[r] += count;
        if (R.en[r] == parent && R.eo[r] > index) R.eo[r] += count;
    }
}

static bool inclusive_descendant(NodeId x, NodeId node)
{
    for (; x; x = dom_parent(x))
        if (x == node) return true;
    return false;
}

void dom_range_removed(NodeId parent, uint32_t index, NodeId node)
{
    FOR_LIVE(r) {
        if (inclusive_descendant(R.sn[r], node)) { R.sn[r] = parent; R.so[r] = index; }
        if (inclusive_descendant(R.en[r], node)) { R.en[r] = parent; R.eo[r] = index; }
        if (R.sn[r] == parent && R.so[r] > index) R.so[r]--;
        if (R.en[r] == parent && R.eo[r] > index) R.eo[r]--;
    }
}

void dom_range_replaced(NodeId node, uint32_t off, uint32_t count, uint32_t len)
{
    FOR_LIVE(r) {
        if (R.sn[r] == node) {
            if (R.so[r] > off && R.so[r] <= off + count) R.so[r] = off;
            else if (R.so[r] > off + count) R.so[r] = R.so[r] + len - count;
        }
        if (R.en[r] == node) {
            if (R.eo[r] > off && R.eo[r] <= off + count) R.eo[r] = off;
            else if (R.eo[r] > off + count) R.eo[r] = R.eo[r] + len - count;
        }
    }
}

void dom_range_split(NodeId node, uint32_t off, NodeId new_node, NodeId parent, uint32_t index)
{
    if (!parent) return;
    FOR_LIVE(r) {
        if (R.sn[r] == node && R.so[r] > off) { R.sn[r] = new_node; R.so[r] -= off; }
        if (R.en[r] == node && R.eo[r] > off) { R.en[r] = new_node; R.eo[r] -= off; }
        if (R.sn[r] == parent && R.so[r] == index) R.so[r]++;
        if (R.en[r] == parent && R.eo[r] == index) R.eo[r]++;
    }
}
