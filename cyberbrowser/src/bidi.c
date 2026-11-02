/* The Unicode Bidirectional Algorithm, UAX #9 (revision for Unicode 16+):
 * explicit levels and directions (X1 - X10), weak types (W1 - W7), paired
 * brackets (N0), neutrals (N1, N2), implicit levels (I1, I2), and per line
 * L1 and L2.  Classes, mirrors and brackets from src/bidi_tables.h
 * (tools/gen_bidi_tables.js). */
#include "bidi.h"
#include <stdlib.h>
#include <string.h>
#include "bidi_tables.h"

static BidiClass bidi_class_search(uint32_t cp)
{
    int lo = 0, hi = (int)(sizeof(k_bidi_ranges) / sizeof(k_bidi_ranges[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < k_bidi_ranges[mid].lo) hi = mid - 1;
        else if (cp > k_bidi_ranges[mid].hi) lo = mid + 1;
        else return (BidiClass)k_bidi_ranges[mid].cls;
    }
    return BIDI_L;
}

/* The classes of the BMP, one byte each, filled from the ranges at first
 * use (threads that race fill it with the same bytes). */
static uint8_t g_bmp_class[0x10000];
static volatile int g_bmp_ready;

static void bmp_class_fill(void)
{
    memset(g_bmp_class, BIDI_L, sizeof(g_bmp_class));
    for (size_t r = 0; r < sizeof(k_bidi_ranges) / sizeof(k_bidi_ranges[0]); r++) {
        if (k_bidi_ranges[r].lo > 0xFFFF) break;
        uint32_t hi = k_bidi_ranges[r].hi > 0xFFFF ? 0xFFFF : k_bidi_ranges[r].hi;
        memset(g_bmp_class + k_bidi_ranges[r].lo, k_bidi_ranges[r].cls, hi - k_bidi_ranges[r].lo + 1);
    }
    g_bmp_ready = 1;
}

BidiClass bidi_class(uint32_t cp)
{
    if (cp > 0xFFFF) return bidi_class_search(cp);
    if (!g_bmp_ready) bmp_class_fill();
    return (BidiClass)g_bmp_class[cp];
}

uint32_t bidi_mirror(uint32_t cp)
{
    int lo = 0, hi = (int)(sizeof(k_bidi_mirror) / sizeof(k_bidi_mirror[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < k_bidi_mirror[mid].cp) hi = mid - 1;
        else if (cp > k_bidi_mirror[mid].cp) lo = mid + 1;
        else return k_bidi_mirror[mid].to;
    }
    return cp;
}

/* cp's paired bracket type (1 open, 2 close, 0 none) and its pair. */
static int bracket_of(uint32_t cp, uint32_t *pair)
{
    if (cp < 0x80) {
        switch (cp) {
        case '(': *pair = ')'; return 1;
        case ')': *pair = '('; return 2;
        case '[': *pair = ']'; return 1;
        case ']': *pair = '['; return 2;
        case '{': *pair = '}'; return 1;
        case '}': *pair = '{'; return 2;
        default: return 0;
        }
    }
    int lo = 0, hi = (int)(sizeof(k_bidi_brackets) / sizeof(k_bidi_brackets[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < k_bidi_brackets[mid].cp) hi = mid - 1;
        else if (cp > k_bidi_brackets[mid].cp) lo = mid + 1;
        else {
            *pair = k_bidi_brackets[mid].pair;
            return k_bidi_brackets[mid].type;
        }
    }
    return 0;
}

/* (U+2329 / U+232A are canonically U+3008 / U+3009: one bracket) */
static uint32_t bracket_norm(uint32_t cp)
{
    return cp == 0x2329 ? 0x3008 : cp == 0x232A ? 0x3009 : cp;
}

static bool is_isolate_init(int c) { return c == BIDI_LRI || c == BIDI_RLI || c == BIDI_FSI; }

/* What X9 removes: embedding and override controls, and BN. */
static bool is_removed(int c)
{
    return c == BIDI_RLE || c == BIDI_LRE || c == BIDI_RLO || c == BIDI_LRO || c == BIDI_PDF || c == BIDI_BN;
}

/* BD9: each isolate initiator's matching PDI and each PDI's initiator
 * (-1: none), within the paragraph. */
static void match_isolates(const uint8_t *cls, int n, int *match, int *stack)
{
    int d = 0;
    for (int i = 0; i < n; i++) {
        match[i] = -1;
        if (is_isolate_init(cls[i])) {
            stack[d++] = i;
        } else if (cls[i] == BIDI_PDI && d > 0) {
            int j = stack[--d];
            match[j] = i;
            match[i] = j;
        } else if (cls[i] == BIDI_B) {
            d = 0;
        }
    }
}

/* P2 / P3 over [a, b): 0, 1, or -1. */
static int first_strong(const uint8_t *cls, const int *match, int a, int b)
{
    for (int i = a; i < b; i++) {
        int c = cls[i];
        if (c == BIDI_L) return 0;
        if (c == BIDI_R || c == BIDI_AL) return 1;
        if (c == BIDI_B) return -1;
        if (is_isolate_init(c)) {
            if (match[i] < 0) return -1;
            i = match[i];
        }
    }
    return -1;
}

int bidi_first_strong_level(const uint32_t *cps, int n)
{
    if (n <= 0) return -1;
    uint8_t *cls = malloc((size_t)n);
    int *match = malloc((size_t)n * sizeof(int)), *stack = malloc((size_t)n * sizeof(int));
    int r = -1;
    if (cls && match && stack) {
        for (int i = 0; i < n; i++) cls[i] = (uint8_t)bidi_class(cps[i]);
        match_isolates(cls, n, match, stack);
        r = first_strong(cls, match, 0, n);
    }
    free(cls);
    free(match);
    free(stack);
    return r;
}

/* For N0, N1, N2: a type as a direction (EN and AN count as R): L, R, or
 * -1 for a neutral. */
static int strong_dir(int t)
{
    if (t == BIDI_L) return BIDI_L;
    if (t == BIDI_R || t == BIDI_AL || t == BIDI_EN || t == BIDI_AN) return BIDI_R;
    return -1;
}

static bool is_ni(int t)
{
    return (1u << t) & ((1u << BIDI_B) | (1u << BIDI_S) | (1u << BIDI_WS) | (1u << BIDI_ON) | (1u << BIDI_LRI) |
                        (1u << BIDI_RLI) | (1u << BIDI_FSI) | (1u << BIDI_PDI));
}

typedef struct { int open, close; } BracketPair;

static int pair_cmp(const void *a, const void *b)
{
    return ((const BracketPair *)a)->open - ((const BracketPair *)b)->open;
}

#define HAS(k) (seen & (1u << (k)))

/* W1 - I2 on one isolating run sequence: pos[0, m) its characters, t
 * their types (changed in place), sos / eos its edge types; seen, a mask of
 * the classes that may be in it (a rule with nothing to act on is
 * skipped). */
static void resolve_sequence(const uint32_t *cps, const uint8_t *orig, const int *pos, uint8_t *t, int m, int sos,
                             int eos, uint8_t *levels, BracketPair *pairs, unsigned seen)
{
    int level = levels[pos[0]];
    int e = level & 1 ? BIDI_R : BIDI_L;

    /* W1: NSM takes the type before it (ON after an isolate control) */
    for (int k = 0; HAS(BIDI_NSM) && k < m; k++) {
        if (t[k] != BIDI_NSM) continue;
        if (k == 0) t[k] = (uint8_t)sos;
        else t[k] = is_isolate_init(t[k - 1]) || t[k - 1] == BIDI_PDI ? BIDI_ON : t[k - 1];
    }
    /* W2: EN after AL is AN; W3: AL is R */
    int last = sos;
    for (int k = 0; HAS(BIDI_AL) && HAS(BIDI_EN) && k < m; k++) {
        if (t[k] == BIDI_L || t[k] == BIDI_R || t[k] == BIDI_AL) last = t[k];
        else if (t[k] == BIDI_EN && last == BIDI_AL) t[k] = BIDI_AN;
    }
    for (int k = 0; HAS(BIDI_AL) && k < m; k++)
        if (t[k] == BIDI_AL) t[k] = BIDI_R;
    /* W4: one separator between two numbers of its kind */
    for (int k = 1; (HAS(BIDI_EN) || HAS(BIDI_AN)) && (HAS(BIDI_ES) || HAS(BIDI_CS)) && k + 1 < m; k++) {
        if (t[k] == BIDI_ES && t[k - 1] == BIDI_EN && t[k + 1] == BIDI_EN) t[k] = BIDI_EN;
        else if (t[k] == BIDI_CS && t[k - 1] == t[k + 1] && (t[k - 1] == BIDI_EN || t[k - 1] == BIDI_AN))
            t[k] = t[k - 1];
    }
    /* W5: terminators next to European numbers */
    for (int k = 0; HAS(BIDI_ET) && HAS(BIDI_EN) && k < m;) {
        if (t[k] != BIDI_ET) {
            k++;
            continue;
        }
        int j = k;
        while (j < m && t[j] == BIDI_ET) j++;
        if ((k > 0 && t[k - 1] == BIDI_EN) || (j < m && t[j] == BIDI_EN))
            for (int q = k; q < j; q++) t[q] = BIDI_EN;
        k = j;
    }
    /* W6: other separators and terminators are neutral */
    for (int k = 0; (HAS(BIDI_ES) || HAS(BIDI_ET) || HAS(BIDI_CS)) && k < m; k++)
        if (t[k] == BIDI_ES || t[k] == BIDI_ET || t[k] == BIDI_CS) t[k] = BIDI_ON;
    /* W7: EN after L is L */
    last = sos;
    for (int k = 0; HAS(BIDI_EN) && k < m; k++) {
        if (t[k] == BIDI_L || t[k] == BIDI_R) last = t[k];
        else if (t[k] == BIDI_EN && last == BIDI_L) t[k] = BIDI_L;
    }

    /* N0: paired brackets (BD16, a stack of 63) */
    int np = 0;
    {
        struct { uint32_t close; int k; } st[63];
        int sp = 0;
        for (int k = 0; HAS(BIDI_ON) && k < m; k++) {
            if (t[k] != BIDI_ON) continue;
            uint32_t pair;
            int type = bracket_of(cps[pos[k]], &pair);
            if (type == 1) {
                if (sp == 63) break;
                st[sp].close = bracket_norm(pair);
                st[sp].k = k;
                sp++;
            } else if (type == 2) {
                uint32_t c = bracket_norm(cps[pos[k]]);
                for (int s = sp - 1; s >= 0; s--) {
                    if (st[s].close != c) continue;
                    pairs[np].open = st[s].k;
                    pairs[np].close = k;
                    np++;
                    sp = s;
                    break;
                }
            }
        }
    }
    if (np > 1) qsort(pairs, (size_t)np, sizeof(BracketPair), pair_cmp);
    for (int p = 0; p < np; p++) {
        int o = pairs[p].open, c = pairs[p].close;
        bool found_e = false, found_opp = false;
        for (int k = o + 1; k < c; k++) {
            int d = strong_dir(t[k]);
            if (d == e) found_e = true;
            else if (d >= 0) found_opp = true;
        }
        int dir;
        if (found_e) {
            dir = e;
        } else if (found_opp) {
            int ctx = sos;
            for (int k = o - 1; k >= 0; k--) {
                int d = strong_dir(t[k]);
                if (d >= 0) {
                    ctx = d;
                    break;
                }
            }
            dir = ctx;   /* (the opposite direction only when the context has it too) */
        } else {
            continue;
        }
        t[o] = t[c] = (uint8_t)dir;
        /* (NSMs after a bracket that changed follow it) */
        for (int k = o + 1; k < m && orig[pos[k]] == BIDI_NSM; k++) t[k] = (uint8_t)dir;
        for (int k = c + 1; k < m && orig[pos[k]] == BIDI_NSM; k++) t[k] = (uint8_t)dir;
    }

    /* N1, N2: neutrals between two of a direction take it, else the
     * embedding direction */
    for (int k = 0; k < m;) {
        if (!is_ni(t[k])) {
            k++;
            continue;
        }
        int j = k;
        while (j < m && is_ni(t[j])) j++;
        int before = k == 0 ? sos : strong_dir(t[k - 1]);
        int after = j == m ? eos : strong_dir(t[j]);
        int d = before == after && before >= 0 ? before : e;
        for (int q = k; q < j; q++) t[q] = (uint8_t)d;
        k = j;
    }

    /* I1, I2 */
    for (int k = 0; k < m; k++) {
        int lv = levels[pos[k]];
        if (!(lv & 1)) {
            if (t[k] == BIDI_R) lv += 1;
            else if (t[k] == BIDI_AN || t[k] == BIDI_EN) lv += 2;
        } else if (t[k] == BIDI_L || t[k] == BIDI_EN || t[k] == BIDI_AN) {
            lv += 1;
        }
        levels[pos[k]] = (uint8_t)lv;
    }
}

/* The working arrays of bidi_resolve, carved from one block per thread
 * that grows as paragraphs do and is kept between calls. */
static _Thread_local unsigned char *t_scratch;
static _Thread_local size_t t_scratch_cap;

static void *scratch_get(size_t need)
{
    if (need > t_scratch_cap) {
        size_t cap = t_scratch_cap ? t_scratch_cap : 4096;
        while (cap < need) cap *= 2;
        unsigned char *p = (unsigned char *)realloc(t_scratch, cap);
        if (!p) return NULL;
        t_scratch = p;
        t_scratch_cap = cap;
    }
    return t_scratch;
}

static int bidi_resolve_one(const uint32_t *cps, int n, int base_level, uint8_t *levels);

int bidi_resolve(const uint32_t *cps, int n, int base_level, uint8_t *levels)
{
    /* P1: a paragraph ends after each paragraph separator; each is resolved
     * on its own (a neutral before one does not see past it). */
    int first = -1;
    for (int s = 0; s < n || first < 0;) {
        int e = s;
        while (e < n) {
            uint32_t c = cps[e++];
            if (c == 0x2029 || c == 0x0A || c == 0x0D || c == 0x1C || c == 0x1D || c == 0x1E || c == 0x85) break;
        }
        int para = bidi_resolve_one(cps + s, e - s, base_level, levels + s);
        if (para < 0) return para;
        if (first < 0) first = para;
        s = e;
        if (e >= n) break;
    }
    return first;
}

static int bidi_resolve_one(const uint32_t *cps, int n, int base_level, uint8_t *levels)
{
    if (n <= 0) return base_level < 0 ? 0 : base_level;
    size_t N = (size_t)n;
    unsigned char *mem = (unsigned char *)scratch_get(N * sizeof(BracketPair) + (6 * N + 1) * sizeof(int) + 5 * N + 1);
    if (!mem) return base_level < 0 ? 0 : base_level;
    BracketPair *pairs = (BracketPair *)mem;
    int *match = (int *)(pairs + N), *tmp = match + N, *idx = tmp + N, *run_of = idx + N, *seq = run_of + N;
    int *runs = seq + N;   /* (n + 1) */
    uint8_t *orig = (uint8_t *)(runs + N + 1), *cls = orig + N, *t = cls + N;
    uint8_t *elev = t + N;   /* (the explicit levels, X1 - X8: what sos / eos compare) */
    uint8_t *linked = elev + N;   /* (n + 1) */
    memset(linked, 0, N + 1);
    int para = -1;

    /* (explicit: an embedding, override or isolate control, or a paragraph
     * separator; without one every character is at the paragraph level and
     * the paragraph is one isolating run sequence) */
    unsigned explicit_mask = (1u << BIDI_RLE) | (1u << BIDI_LRE) | (1u << BIDI_RLO) | (1u << BIDI_LRO) |
                             (1u << BIDI_PDF) | (1u << BIDI_RLI) | (1u << BIDI_LRI) | (1u << BIDI_FSI) |
                             (1u << BIDI_PDI) | (1u << BIDI_B) | (1u << BIDI_BN);
    unsigned seen = 0;
    for (int i = 0; i < n; i++) {
        uint32_t c = cps[i];
        uint8_t k = c <= 0xFFFF && g_bmp_ready ? g_bmp_class[c] : (uint8_t)bidi_class(c);
        orig[i] = cls[i] = k;
        seen |= 1u << k;
    }
    if (!(seen & explicit_mask)) {
        para = base_level >= 0 ? base_level : -1;
        if (para < 0) {
            para = 0;
            for (int i = 0; i < n; i++) {
                if (orig[i] == BIDI_L) break;
                if (orig[i] == BIDI_R || orig[i] == BIDI_AL) { para = 1; break; }
            }
        }
        memset(levels, para, N);
        /* (nothing strong of the other direction, no number: every level is the paragraph's) */
        unsigned other = para & 1 ? (1u << BIDI_L) | (1u << BIDI_EN) | (1u << BIDI_AN)
                                   : (1u << BIDI_R) | (1u << BIDI_AL) | (1u << BIDI_EN) | (1u << BIDI_AN);
        if (!(seen & other)) return para;
        for (int i = 0; i < n; i++) seq[i] = i;
        memcpy(t, cls, N);
        int e = para & 1 ? BIDI_R : BIDI_L;
        resolve_sequence(cps, orig, seq, t, n, e, e, levels, pairs, seen);
        return para;
    }
    match_isolates(orig, n, match, tmp);
    para = base_level >= 0 ? base_level : first_strong(orig, match, 0, n);
    if (para < 0) para = 0;

    /* X1 - X8: the directional status stack */
    {
        struct { uint8_t level, over; bool isolate; } st[BIDI_MAX_DEPTH + 2];
        enum { NO_OVERRIDE = 0xFF };
        int sp = 1, ovf_iso = 0, ovf_emb = 0, valid_iso = 0;
        st[0].level = (uint8_t)para;
        st[0].over = NO_OVERRIDE;
        st[0].isolate = false;
        for (int i = 0; i < n; i++) {
            int c = orig[i];
            switch (c) {
            case BIDI_RLE: case BIDI_LRE: case BIDI_RLO: case BIDI_LRO: {
                int cur = st[sp - 1].level;
                int nl = c == BIDI_RLE || c == BIDI_RLO ? (cur + 1) | 1 : (cur + 2) & ~1;
                levels[i] = (uint8_t)cur;
                if (nl <= BIDI_MAX_DEPTH && !ovf_iso && !ovf_emb) {
                    st[sp].level = (uint8_t)nl;
                    st[sp].over = c == BIDI_RLO ? BIDI_R : c == BIDI_LRO ? BIDI_L : NO_OVERRIDE;
                    st[sp].isolate = false;
                    sp++;
                } else if (!ovf_iso) {
                    ovf_emb++;
                }
                break;
            }
            case BIDI_RLI: case BIDI_LRI: case BIDI_FSI: {
                int cur = st[sp - 1].level;
                levels[i] = (uint8_t)cur;
                if (st[sp - 1].over != NO_OVERRIDE) cls[i] = st[sp - 1].over;
                bool rtl = c == BIDI_RLI ||
                           (c == BIDI_FSI && first_strong(orig, match, i + 1, match[i] >= 0 ? match[i] : n) == 1);
                int nl = rtl ? (cur + 1) | 1 : (cur + 2) & ~1;
                if (nl <= BIDI_MAX_DEPTH && !ovf_iso && !ovf_emb) {
                    valid_iso++;
                    st[sp].level = (uint8_t)nl;
                    st[sp].over = NO_OVERRIDE;
                    st[sp].isolate = true;
                    sp++;
                } else {
                    ovf_iso++;
                }
                break;
            }
            case BIDI_PDI:
                if (ovf_iso) {
                    ovf_iso--;
                } else if (valid_iso) {
                    ovf_emb = 0;
                    while (!st[sp - 1].isolate) sp--;
                    sp--;
                    valid_iso--;
                }
                levels[i] = st[sp - 1].level;
                if (st[sp - 1].over != NO_OVERRIDE) cls[i] = st[sp - 1].over;
                break;
            case BIDI_PDF:
                levels[i] = st[sp - 1].level;
                if (ovf_iso) {
                } else if (ovf_emb) {
                    ovf_emb--;
                } else if (!st[sp - 1].isolate && sp >= 2) {
                    sp--;
                }
                break;
            case BIDI_B:
                levels[i] = (uint8_t)para;
                sp = 1;
                ovf_iso = ovf_emb = valid_iso = 0;
                break;
            case BIDI_BN:
                levels[i] = st[sp - 1].level;
                break;
            default:
                levels[i] = st[sp - 1].level;
                if (st[sp - 1].over != NO_OVERRIDE) cls[i] = st[sp - 1].over;
                break;
            }
        }
    }

    /* X9: what is left; X10: level runs, then isolating run sequences */
    memcpy(elev, levels, (size_t)n);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (!is_removed(orig[i])) idx[m++] = i;
    for (int k = 0; k < m; k++) tmp[idx[k]] = k;   /* (each kept character's place in idx) */
    int nruns = 0;
    for (int k = 0; k < m; k++) {
        if (k == 0 || levels[idx[k]] != levels[idx[k - 1]]) runs[nruns++] = k;
        run_of[idx[k]] = nruns - 1;
    }
    runs[nruns] = m;
    for (int r = 0; r < nruns; r++) {
        if (linked[r]) continue;
        int len = 0;
        for (int cur = r;;) {
            for (int k = runs[cur]; k < runs[cur + 1]; k++) seq[len++] = idx[k];
            int lastc = idx[runs[cur + 1] - 1];
            if (!is_isolate_init(orig[lastc]) || match[lastc] < 0) break;
            int pdi = match[lastc];
            if (is_removed(orig[pdi])) break;
            int r2 = run_of[pdi];
            if (idx[runs[r2]] != pdi) break;   /* (not starting its run) */
            linked[r2] = 1;
            cur = r2;
        }
        int first = seq[0], lastc = seq[len - 1];
        int lv = levels[first];
        /* sos / eos: against the levels beside the sequence */
        int k0 = tmp[first];
        int prev = k0 > 0 ? elev[idx[k0 - 1]] : para;
        int next = para;
        if (!is_isolate_init(orig[lastc])) {
            int kl = tmp[lastc];
            if (kl + 1 < m) next = elev[idx[kl + 1]];
        }
        int sos = (lv > prev ? lv : prev) & 1 ? BIDI_R : BIDI_L;
        int eos = (lv > next ? lv : next) & 1 ? BIDI_R : BIDI_L;
        for (int k = 0; k < len; k++) t[k] = cls[seq[k]];
        resolve_sequence(cps, orig, seq, t, len, sos, eos, levels, pairs, ~0u);
    }
    /* (what X9 removed: the level before it, for unbroken runs; an
     * embedding or override initiator the level of what it starts, the
     * next character's, so it goes with that, not before the run) */
    for (int i = 0; i < n; i++) {
        if (!is_removed(orig[i])) continue;
        int c = orig[i];
        if (c == BIDI_RLE || c == BIDI_LRE || c == BIDI_RLO || c == BIDI_LRO) {
            int j = i + 1;
            while (j < n && is_removed(orig[j])) j++;
            if (j < n && orig[j] != BIDI_B) { levels[i] = levels[j]; continue; }
        }
        levels[i] = i > 0 ? levels[i - 1] : (uint8_t)para;
    }
    return para;
}

void bidi_line_levels(const uint32_t *cps, int start, int end, int para_level, uint8_t *levels)
{
    int ws = -1;   /* the white space run being passed, or -1 */
    for (int i = start; i < end; i++) {
        int c = bidi_class(cps[i]);
        if (c == BIDI_WS || c == BIDI_LRI || c == BIDI_RLI || c == BIDI_FSI || c == BIDI_PDI || is_removed(c)) {
            if (ws < 0) ws = i;
        } else if (c == BIDI_S || c == BIDI_B) {
            for (int q = ws >= 0 ? ws : i; q <= i; q++) levels[q] = (uint8_t)para_level;
            ws = -1;
        } else {
            ws = -1;
        }
    }
    if (ws >= 0)
        for (int q = ws; q < end; q++) levels[q] = (uint8_t)para_level;
}

void bidi_reorder(const uint8_t *levels, int n, int *order)
{
    int hi = 0, lo_odd = 255;
    for (int i = 0; i < n; i++) {
        order[i] = i;
        if (levels[i] > hi) hi = levels[i];
        if ((levels[i] & 1) && levels[i] < lo_odd) lo_odd = levels[i];
    }
    for (int lv = hi; lv >= lo_odd; lv--) {
        for (int i = 0; i < n;) {
            if (levels[order[i]] < lv) {
                i++;
                continue;
            }
            int j = i;
            while (j < n && levels[order[j]] >= lv) j++;
            for (int a = i, b = j - 1; a < b; a++, b--) {
                int x = order[a];
                order[a] = order[b];
                order[b] = x;
            }
            i = j;
        }
    }
}
