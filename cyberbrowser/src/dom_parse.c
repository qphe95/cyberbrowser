/* HTML tokenizer and tree builder over the node store.  See dom_parse.h. */
#include "dom_parse.h"
#include "cyber_env.h"
#include "job_system.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ============================================================================
 * Tokens
 * ============================================================================ */

enum {
    TK_START = 1,       /* start tag: tag, attributes, flags */
    TK_END,             /* "</...": src = '<', end = after the first '>' (or EOF) */
    TK_TEXT,            /* text run, collapsed and decoded */
    TK_RAW,             /* raw content of the preceding raw-text start tag */
    TK_MALFORMED,       /* "<" without a tag name, skipped */
};
enum { TKF_SELF_CLOSED = 1 };

typedef struct DomTok {
    uint8_t kind;
    uint8_t flags;
    uint16_t nattr;
    CssAtom tag;
    uint32_t src, end;          /* source span */
    DomAttr *attrs;             /* START: arena array (block attr_blk) */
    uint32_t attr_blk;
    DomStr text;                /* TEXT / RAW */
} DomTok;

typedef struct TokChunk {
    const char *html;
    size_t len;
    size_t start, limit;        /* tokenize tokens that start in [start, limit) */
    size_t end;                 /* out: position after the last token */
    bool doc_start;             /* skip a doctype at `start` */
    bool atoms_full;            /* out: the atom table ran out (redo serially) */
    bool overrun;               /* out: a speculative chunk ran past its slack */
    size_t full_len;            /* the real input length (len may be cut short) */
    DomArena arena;
    DomTok *toks;
    size_t n, cap;
    DomAttr *scratch;           /* attribute scratch */
    size_t scratch_cap;
} TokChunk;

static DomParseStats g_stats;
void dom_parse_last_stats(DomParseStats *out) { *out = g_stats; }

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static inline bool c_isspace(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static DomTok *tok_push(TokChunk *c) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 16;
        c->toks = realloc(c->toks, c->cap * sizeof(DomTok));
    }
    DomTok *t = &c->toks[c->n++];
    memset(t, 0, sizeof *t);
    return t;
}

/* Intern a name; serial tokenizers grow the atom table as they go, jobs rely
 * on the reservation made before dispatch and flag the chunk when it ran out. */
static CssAtom tok_atom(TokChunk *c, const char *s, size_t n, bool serial) {
    if (serial) css_atom_reserve(1);
    CssAtom a = css_atom_intern(s, n);
    if (a == CSS_ATOM_NONE && n) c->atoms_full = true;
    return a;
}

/* html_decode_entities() of the legacy parser, in place on a NUL-terminated
 * buffer.  Returns the new length. */
static uint32_t decode_entities(char *str, size_t len) {
    char *dest = str;
    const char *src = str, *end = str + len;
    while (*src && src < end) {
        if (*src == '&') {
            if (strncmp(src, "&lt;", 4) == 0) { *dest++ = '<'; src += 4; }
            else if (strncmp(src, "&gt;", 4) == 0) { *dest++ = '>'; src += 4; }
            else if (strncmp(src, "&amp;", 5) == 0) { *dest++ = '&'; src += 5; }
            else if (strncmp(src, "&quot;", 6) == 0) { *dest++ = '"'; src += 6; }
            else if (strncmp(src, "&apos;", 6) == 0) { *dest++ = '\''; src += 6; }
            else if (strncmp(src, "&nbsp;", 6) == 0) { *dest++ = ' '; src += 6; }
            else if (src[1] == '#' && isdigit((unsigned char)src[2])) {
                src += 2;
                unsigned val = 0;
                while (isdigit((unsigned char)*src)) { val = val * 10u + (unsigned)(*src - '0'); src++; }
                if (*src == ';') src++;
                *dest++ = (char)(val > 0 && val < 256 ? val : '?');
            } else {
                *dest++ = *src++;
            }
        } else {
            *dest++ = *src++;
        }
    }
    *dest = '\0';
    return (uint32_t)(dest - str);
}

/* Text: collapse whitespace runs to one space (keeping single leading and
 * trailing spaces; an all-space run becomes " "), then decode entities. */
static DomStr tok_text(TokChunk *c, const char *s, size_t n) {
    DomStr r;
    r.s = dom_arena_alloc(&c->arena, n + 2, &r.blk);
    size_t cl = 0;
    bool pending = false;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (c_isspace(ch)) { pending = true; continue; }
        if (pending) r.s[cl++] = ' ';
        pending = false;
        r.s[cl++] = (char)ch;
    }
    if (pending && cl > 0) r.s[cl++] = ' ';
    if (cl == 0) r.s[cl++] = ' ';
    r.s[cl] = '\0';
    r.len = decode_entities(r.s, cl);
    return r;
}

static DomStr tok_attr_value(TokChunk *c, const char *s, size_t n) {
    if (n == 0) return (DomStr){ NULL, 0, 0 };
    DomStr r = dom_arena_str(&c->arena, s, n);
    r.len = decode_entities(r.s, strlen(r.s));
    if (r.len == 0) { dom_str_release(&r); return (DomStr){ NULL, 0, 0 }; }
    return r;
}

/* Raw-text tag atoms, interned before any job runs (dom_tag_flags() keeps an
 * engine-thread cache, so jobs must not call it). */
static CssAtom g_raw_atoms[4];
static void init_raw_atoms(void) {
    if (g_raw_atoms[0]) return;
    static const char *const names[4] = { "script", "style", "textarea", "title" };
    css_atom_reserve(4);
    for (int i = 0; i < 4; i++) g_raw_atoms[i] = css_atom_intern(names[i], strlen(names[i]));
}
static inline bool is_raw_atom(CssAtom a) {
    return a == g_raw_atoms[0] || a == g_raw_atoms[1] || a == g_raw_atoms[2] || a == g_raw_atoms[3];
}

static bool is_tag_delim(char ch) {
    return ch == '>' || ch == '/' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

/* Tokenize a start tag at c->html[pos] == '<' (not followed by '/').
 * Returns the position after it (and after the raw content of raw tags). */
static size_t tok_start_tag(TokChunk *c, size_t pos, bool serial) {
    const char *h = c->html;
    size_t len = c->len, p = pos + 1, src = pos;
    while (p < len && c_isspace((unsigned char)h[p])) p++;
    size_t nb = p;
    while (p < len && !is_tag_delim(h[p])) p++;
    if (p == nb) {
        /* Malformed: skip to '>' inclusive; the child loop then skips one more
         * character (the legacy recovery). */
        while (p < len && h[p] != '>') p++;
        if (p < len) p++;
        DomTok *t = tok_push(c);
        t->kind = TK_MALFORMED;
        t->src = (uint32_t)src;
        t->end = (uint32_t)p;
        return p + 1 <= len ? p + 1 : len;
    }
    CssAtom tag = tok_atom(c, h + nb, p - nb, serial);
    size_t na = 0;
    uint8_t flags = 0;
    for (;;) {
        while (p < len && c_isspace((unsigned char)h[p])) p++;
        if (p >= len) break;
        if (h[p] == '>') { p++; break; }
        if (h[p] == '/' && p + 1 < len && h[p + 1] == '>') { p += 2; flags |= TKF_SELF_CLOSED; break; }
        /* attribute name */
        size_t an = p;
        while (p < len) {
            char ch = h[p];
            if (ch == '=' || ch == '>' || ch == '/' || c_isspace((unsigned char)ch)) break;
            p++;
        }
        if (p == an) {
            /* Not an attribute: skip one character ('/' or '='). */
            if (p < len && h[p] != '>' && !(h[p] == '/' && p + 1 < len && h[p + 1] == '>')) { p++; continue; }
            break;
        }
        size_t an_end = p;
        while (p < len && c_isspace((unsigned char)h[p])) p++;
        size_t vb = 0, ve = 0;
        if (p < len && h[p] == '=') {
            p++;
            while (p < len && c_isspace((unsigned char)h[p])) p++;
            if (p < len) {
                char q = h[p];
                if (q == '"' || q == '\'') {
                    p++;
                    vb = p;
                    const char *qe = memchr(h + p, q, len - p);
                    p = qe ? (size_t)(qe - h) : len;
                    ve = p;
                    if (p < len) p++;
                } else {
                    vb = p;
                    while (p < len) {
                        char ch = h[p];
                        if (ch == '>' || ch == '/' || c_isspace((unsigned char)ch)) break;
                        p++;
                    }
                    ve = p;
                }
            }
        }
        if (na == c->scratch_cap) {
            c->scratch_cap = c->scratch_cap ? c->scratch_cap * 2 : 16;
            c->scratch = realloc(c->scratch, c->scratch_cap * sizeof(DomAttr));
        }
        c->scratch[na].name = tok_atom(c, h + an, an_end - an, serial);
        c->scratch[na].value = tok_attr_value(c, h + vb, ve - vb);
        na++;
    }
    DomTok *t = tok_push(c);
    t->kind = TK_START;
    t->flags = flags;
    t->tag = tag;
    t->src = (uint32_t)src;
    if (na) {
        if (na > 0xFFFF) na = 0xFFFF;
        t->attrs = dom_arena_alloc(&c->arena, na * sizeof(DomAttr), &t->attr_blk);
        memcpy(t->attrs, c->scratch, na * sizeof(DomAttr));
        t->nattr = (uint16_t)na;
    }
    if (!(flags & TKF_SELF_CLOSED) && is_raw_atom(tag)) {
        /* Raw content up to "</" + tag (prefix, case-insensitive). */
        const char *tn = css_atom_str(tag);
        size_t tl = strlen(tn);
        size_t rb = p;
        while (p < len) {
            const char *lt = memchr(h + p, '<', len - p);
            if (!lt) { p = len; break; }
            p = (size_t)(lt - h);
            if (p + 1 < len && h[p + 1] == '/' && p + 2 + tl <= len &&
                strncasecmp(h + p + 2, tn, tl) == 0) break;
            p++;
        }
        DomTok *r = tok_push(c);
        t = &c->toks[c->n - 2];
        r->kind = TK_RAW;
        r->src = (uint32_t)rb;
        r->text = dom_arena_str(&c->arena, h + rb, p - rb);
        /* Skip the exact closing tag "</tag>". */
        if (p + tl + 3 <= len && h[p] == '<' && h[p + 1] == '/' &&
            strncasecmp(h + p + 2, tn, tl) == 0 && h[p + 2 + tl] == '>')
            p += tl + 3;
        r->end = (uint32_t)p;
    }
    t->end = (uint32_t)p;
    return p;
}

/* Tokenize from c->start: every token that starts before c->limit. */
static void tokenize(TokChunk *c, bool serial) {
    const char *h = c->html;
    size_t len = c->len, pos = c->start;
    if (c->doc_start && pos + 9 <= len && strncasecmp(h + pos, "<!doctype", 9) == 0) {
        pos += 9;
        const char *gt = memchr(h + pos, '>', len - pos);
        pos = gt ? (size_t)(gt - h) + 1 : len;
    }
    while (pos < len && pos < c->limit) {
        if (h[pos] == '<') {
            if (pos + 4 <= len && memcmp(h + pos, "<!--", 4) == 0) {
                pos += 4;
                for (;;) {
                    const char *d = pos < len ? memchr(h + pos, '-', len - pos) : NULL;
                    if (!d) { pos = len; break; }
                    pos = (size_t)(d - h);
                    if (pos + 3 <= len && d[1] == '-' && d[2] == '>') { pos += 3; break; }
                    pos++;
                }
                continue;
            }
            if (pos + 1 < len && h[pos + 1] == '/') {
                size_t e = pos + 2;
                const char *gt = e < len ? memchr(h + e, '>', len - e) : NULL;
                e = gt ? (size_t)(gt - h) + 1 : len;
                DomTok *t = tok_push(c);
                t->kind = TK_END;
                t->src = (uint32_t)pos;
                t->end = (uint32_t)e;
                pos = e;
                continue;
            }
            pos = tok_start_tag(c, pos, serial);
            continue;
        }
        const char *lt = memchr(h + pos, '<', len - pos);
        size_t e = lt ? (size_t)(lt - h) : len;
        DomTok *t = tok_push(c);
        t->kind = TK_TEXT;
        t->src = (uint32_t)pos;
        t->end = (uint32_t)e;
        t->text = tok_text(c, h + pos, e - pos);
        pos = e;
    }
    c->end = pos;
}

static void chunk_release_tokens(TokChunk *c, size_t from) {
    for (size_t i = from; i < c->n; i++) {
        DomTok *t = &c->toks[i];
        if (t->kind == TK_START) {
            for (int a = 0; a < t->nattr; a++) dom_str_release(&t->attrs[a].value);
            dom_block_release(t->attr_blk);
        } else if (t->kind == TK_TEXT || t->kind == TK_RAW) {
            dom_str_release(&t->text);
        }
    }
    c->n = from;
}

static void chunk_free(TokChunk *c) {
    chunk_release_tokens(c, 0);
    dom_arena_finish(&c->arena);
    free(c->toks);
    free(c->scratch);
    c->toks = NULL;
    c->scratch = NULL;
    c->n = c->cap = c->scratch_cap = 0;
}

/* ============================================================================
 * Prescan and parallel chunked tokenizing (§5.2)
 * ============================================================================ */

static size_t env_size(const char *name, size_t def) {
    const char *v = cyber_getenv(name);
    if (!v || !*v) return def;
    long long x = atoll(v);
    return x > 0 ? (size_t)x : def;
}

/* Skip a raw-text element or comment starting at h[p] == '<', if it is one.
 * Returns the position after it, or p when it is something else. */
static size_t prescan_skip(const char *h, size_t len, size_t p) {
    if (p + 4 <= len && memcmp(h + p, "<!--", 4) == 0) {
        const char *e = NULL;
        for (size_t q = p + 4; q + 3 <= len; ) {
            const char *d = memchr(h + q, '-', len - q);
            if (!d || (size_t)(d - h) + 3 > len) break;
            if (d[1] == '-' && d[2] == '>') { e = d + 3; break; }
            q = (size_t)(d - h) + 1;
        }
        return e ? (size_t)(e - h) : len;
    }
    static const char *const raw[] = { "script", "style", "textarea", "title" };
    for (int i = 0; i < 4; i++) {
        size_t tl = strlen(raw[i]);
        if (p + 1 + tl < len && strncasecmp(h + p + 1, raw[i], tl) == 0 &&
            (h[p + 1 + tl] == '>' || c_isspace((unsigned char)h[p + 1 + tl]) || h[p + 1 + tl] == '/')) {
            const char *gt = memchr(h + p, '>', len - p);
            if (!gt) return len;
            size_t q = (size_t)(gt - h) + 1;
            while (q < len) {
                const char *lt = memchr(h + q, '<', len - q);
                if (!lt) return len;
                q = (size_t)(lt - h);
                if (q + 2 + tl <= len && h[q + 1] == '/' && strncasecmp(h + q + 2, raw[i], tl) == 0) {
                    const char *g2 = memchr(h + q, '>', len - q);
                    return g2 ? (size_t)(g2 - h) + 1 : len;
                }
                q++;
            }
            return len;
        }
    }
    return p;
}

/* Chunk boundaries: about every `chunk` bytes, at a '<' that starts a tag,
 * never inside a raw-text element or comment the prescan recognised.  Also
 * counts '<' and '=' for the atom-table reservation. */
static size_t prescan(const char *h, size_t len, size_t chunk, bool force,
                      size_t **bounds_out, size_t *n_lt) {
    size_t cap = len / chunk + 2, n = 0;
    size_t *b = malloc(cap * sizeof(size_t));
    b[n++] = 0;
    size_t lt_count = 0;
    if (force) {
        for (size_t p = chunk; p < len; p += chunk) { if (n == cap) break; b[n++] = p; }
        for (size_t p = 0; p < len; p++) if (h[p] == '<' || h[p] == '=') lt_count++;
    } else {
        size_t next = chunk, p = 0;
        while (p < len) {
            const char *lt = memchr(h + p, '<', len - p);
            if (!lt) break;
            p = (size_t)(lt - h);
            lt_count++;
            size_t q = prescan_skip(h, len, p);
            if (q != p) { p = q; continue; }
            if (p >= next && p + 1 < len && (isalpha((unsigned char)h[p + 1]) || h[p + 1] == '/')) {
                if (n < cap) b[n++] = p;
                next = p + chunk;
            }
            p++;
        }
        for (size_t q = 0; q < len; q++) if (h[q] == '=') lt_count++;
    }
    *bounds_out = b;
    *n_lt = lt_count;
    return n;
}

typedef struct ParseJobCtx {
    TokChunk *chunks;
    size_t nchunks;
} ParseJobCtx;

/* A speculative chunk sees the input cut `slack` bytes after its limit, so a
 * chunk that started in the wrong state (inside a script, say) cannot scan
 * megabytes.  A token that reached the cut may have been shaped by it: the
 * chunk is then treated as invalid and redone serially. */
static void tokenize_job(Job *job) {
    ParseJobCtx *pc = job->ctx;
    TokChunk *c = &pc->chunks[job->a];
    tokenize(c, false);
    if (c->len < c->full_len && c->end >= c->len) c->overrun = true;
}

static void validate_chain(TokChunk *chunks, size_t n);

static void validate_cont(Job *job) {
    ParseJobCtx *pc = job->ctx;
    validate_chain(pc->chunks, pc->nchunks);
}

/* Root of the tokenize graph: one job per chunk, then the validation
 * continuation (run by whichever thread finishes the last chunk). */
static void tokenize_root_job(Job *job) {
    ParseJobCtx *pc = job->ctx;
    for (size_t k = 0; k < pc->nchunks; k++)
        job_spawn(job_new(tokenize_job, job, pc, (uint32_t)k, 0));
    job_set_continuation(job, validate_cont);
}

/* Validate the chain (continuation of the tokenize jobs): chunk k must start
 * where chunk k-1 ended.  A chunk that does not is re-tokenized from the true
 * position; that repairs the rest of the chain. */
static void validate_chain(TokChunk *chunks, size_t n) {
    for (size_t k = 0; k < n; k++) {
        TokChunk *c = &chunks[k];
        size_t truth = k == 0 ? 0 : chunks[k - 1].end;
        if (c->atoms_full || c->overrun || c->start != truth) {
            chunk_release_tokens(c, 0);
            c->atoms_full = false;
            c->overrun = false;
            c->len = c->full_len;
            c->start = truth;
            if (c->start > c->limit) c->limit = c->start;   /* tokenize nothing new, keep end = start */
            if (truth >= c->limit) { c->end = truth; continue; }
            tokenize(c, true);
            g_stats.chunks_redone++;
        }
    }
}

static size_t g_chunk_override;
static int g_force_override = -1;

void dom_parse_set_chunking(size_t chunk, int force) {
    g_chunk_override = chunk;
    g_force_override = force;
}

static void tokenize_all(const char *h, size_t len, TokChunk **out, size_t *nout) {
    size_t chunk = g_chunk_override ? g_chunk_override : env_size("DOM_CHUNK", 1024 * 1024);
    bool force = g_force_override >= 0 ? g_force_override != 0
               : (cyber_getenv("DOM_CHUNK_FORCE") && cyber_getenv("DOM_CHUNK_FORCE")[0] == '1');
    double t0 = now_ms();
    init_raw_atoms();
    size_t *bounds = NULL, nlt = 0, nb = 1;
    bool parallel = job_thread_count() > 1 && (len >= 2 * chunk || force);
    if (parallel) nb = prescan(h, len, chunk, force, &bounds, &nlt);
    if (nb < 2) parallel = false;
    g_stats.prescan_ms = now_ms() - t0;
    t0 = now_ms();
    if (!parallel) {
        TokChunk *c = calloc(1, sizeof(TokChunk));
        c->html = h; c->len = c->full_len = len; c->start = 0; c->limit = len; c->doc_start = true;
        tokenize(c, true);
        free(bounds);
        *out = c; *nout = 1;
        g_stats.chunks = 1;
        g_stats.tokenize_ms = now_ms() - t0;
        return;
    }
    TokChunk *cs = calloc(nb, sizeof(TokChunk));
    for (size_t k = 0; k < nb; k++) {
        size_t span = (k + 1 < nb ? bounds[k + 1] : len) - bounds[k];
        cs[k].arena.next_size = span * 2 + 256 < 64 * 1024 ? span * 2 + 256 : 64 * 1024;
        cs[k].html = h; cs[k].full_len = len;
        cs[k].start = bounds[k];
        cs[k].limit = k + 1 < nb ? bounds[k + 1] : len;
        size_t slack = 4 * chunk > 1024 ? 4 * chunk : 1024;
        cs[k].len = cs[k].limit + slack < len ? cs[k].limit + slack : len;
        cs[k].doc_start = k == 0;
    }
    free(bounds);
    /* Jobs intern without growing the atom table: reserve an upper bound of
     * the names they can meet. */
    css_atom_reserve(nlt + 64);
    ParseJobCtx pc = { cs, nb };
    job_graph_run(job_new(tokenize_root_job, NULL, &pc, 0, 0));
    *out = cs; *nout = nb;
    g_stats.chunks = (uint32_t)nb;
    g_stats.tokenize_ms = now_ms() - t0;
}

/* ============================================================================
 * Tree builder
 * ============================================================================ */

#define MAX_NESTING 256

typedef struct Builder {
    const char *html;
    size_t len;
    uint32_t frag;              /* container slot */
    uint32_t owner;
    uint32_t *stack;            /* open elements (slots) */
    uint32_t sp;
    CssAtom id_name;
} Builder;

static uint32_t build_element(Builder *b, DomTok *t, uint32_t parent) {
    uint32_t s = dom_alloc_slot(DOM_K_ELEMENT, dom_id_of(b->owner));
    g_dom.tag[s] = t->tag;
    g_dom.flags[s] = dom_tag_flags(t->tag) | ((t->flags & TKF_SELF_CLOSED) ? DOMF_SELF_CLOSED : 0);
    if (t->tag == g_raw_atoms[0]) g_dom.flags[s] |= DOMF_PARSER_SCRIPT;
    if (t->nattr) {
        g_dom.attrs[s] = (DomAttrList){ t->attrs, t->nattr, t->nattr, t->attr_blk };
        t->attrs = NULL; t->nattr = 0; t->attr_blk = 0;      /* ownership moved */
        DomAttrList *l = &g_dom.attrs[s];
        for (int i = 0; i < l->n; i++)
            if (l->a[i].name == b->id_name) {
                if (l->a[i].value.len) {
                    css_atom_reserve(1);
                    g_dom.id_atom[s] = css_atom_intern(l->a[i].value.s, l->a[i].value.len);
                    dom_id_index_note(s);
                }
                break;
            }
    }
    dom_link_append(dom_id_of(parent), dom_id_of(s));
    if (g_dom.flags[s] & DOMF_CE_CANDIDATE) {
        for (uint32_t p = parent; p; p = g_dom.parent[p]) {
            if (g_dom.flags[p] & DOMF_CE_DESC) break;
            g_dom.flags[p] |= DOMF_CE_DESC;
        }
    }
    return s;
}

static void build_text(Builder *b, DomTok *t, uint32_t parent) {
    uint32_t s = dom_alloc_slot(DOM_K_TEXT, dom_id_of(b->owner));
    g_dom.text[s] = t->text;
    t->text = (DomStr){ NULL, 0, 0 };
    dom_link_append(dom_id_of(parent), dom_id_of(s));
}

/* Feed tokens [*i, n) of `c`.  Returns false when the builder needs the rest
 * of the input re-tokenized from *detour (a top-level end tag that the legacy
 * parser resumed from inside). */
static bool build_tokens(Builder *b, TokChunk *c, size_t *pi, size_t *detour) {
    for (size_t i = *pi; i < c->n; i++) {
        DomTok *t = &c->toks[i];
        uint32_t top = b->sp ? b->stack[b->sp - 1] : 0;
        switch (t->kind) {
        case TK_START: {
            uint32_t parent = top ? top : b->frag;
            uint32_t s = build_element(b, t, parent);
            uint16_t f = g_dom.flags[s];
            if (f & DOMF_SELF_CLOSED) break;
            if (f & DOMF_RAW_TEXT) {
                if (i + 1 < c->n && c->toks[i + 1].kind == TK_RAW) {
                    DomTok *r = &c->toks[++i];
                    if (r->text.len) build_text(b, r, s);
                }
                break;
            }
            if (f & DOMF_VOID) break;
            if (b->sp + 1 < MAX_NESTING) b->stack[b->sp++] = s;
            break;
        }
        case TK_END:
            if (top) {
                const char *tn = css_atom_str(g_dom.tag[top]);
                size_t tl = strlen(tn);
                if (t->src + 2 + tl <= b->len && strncasecmp(b->html + t->src + 2, tn, tl) == 0)
                    b->sp--;
            } else {
                /* Top level: the legacy parser skipped the '<' and discarded
                 * text up to the next '<'.  If that '<' lies inside this
                 * token, re-tokenize from it. */
                size_t from = t->src + 1;
                const char *lt = memchr(b->html + from, '<', t->end - from);
                if (lt) { *pi = i + 1; *detour = (size_t)(lt - b->html); return false; }
            }
            break;
        case TK_TEXT:
            if (top) build_text(b, t, top);
            break;
        default:
            break;
        }
    }
    *pi = c->n;
    return true;
}

static void parse_into(const char *html, size_t len, uint32_t frag, uint32_t owner) {
    memset(&g_stats, 0, sizeof g_stats);
    double t_all = now_ms();
    TokChunk *chunks = NULL;
    size_t nchunks = 0;
    tokenize_all(html, len, &chunks, &nchunks);
    double t0 = now_ms();
    Builder b = { html, len, frag, owner, malloc(MAX_NESTING * sizeof(uint32_t)), 0, 0 };
    css_atom_reserve(1);
    b.id_name = css_atom_intern("id", 2);
    uint32_t before = g_dom.live;
    for (size_t k = 0; k < nchunks; k++) {
        size_t i = 0, detour = 0;
        g_stats.tokens += (uint32_t)chunks[k].n;
        if (build_tokens(&b, &chunks[k], &i, &detour)) continue;
        /* Detour: drop the remaining tokens, re-tokenize the rest serially. */
        for (size_t r = k; r < nchunks; r++) chunk_release_tokens(&chunks[r], r == k ? i : 0);
        for (;;) {
            g_stats.detours++;
            TokChunk d = { 0 };
            d.html = html; d.len = d.full_len = len; d.start = detour; d.limit = len;
            tokenize(&d, true);
            size_t j = 0;
            bool ok = build_tokens(&b, &d, &j, &detour);
            chunk_release_tokens(&d, j);
            chunk_free(&d);
            if (ok) break;
        }
        break;
    }
    free(b.stack);
    g_stats.nodes = g_dom.live - before;
    for (size_t k = 0; k < nchunks; k++) chunk_free(&chunks[k]);
    free(chunks);
    g_stats.build_ms = now_ms() - t0;
    if (cyber_getenv("DOM_LOG"))
        fprintf(stderr, "[dom-parse] %zu bytes: %u chunks (%u redone, %u detours), %u tokens, %u nodes | "
                "prescan %.2f tokenize %.2f build %.2f total %.2f ms\n",
                len, g_stats.chunks, g_stats.chunks_redone, g_stats.detours, g_stats.tokens,
                g_stats.nodes, g_stats.prescan_ms, g_stats.tokenize_ms, g_stats.build_ms,
                now_ms() - t_all);
}

NodeId dom_parse_html(const char *html, size_t len, NodeId owner_doc) {
    NodeId frag = dom_create_fragment(owner_doc);
    if (html && len) parse_into(html, len, dom_slot(frag), dom_valid(owner_doc) ? dom_slot(owner_doc) : 0);
    return frag;
}

void dom_parse_html_into(const char *html, size_t len, NodeId parent, NodeId owner_doc) {
    if (!html || !len || !dom_valid(parent)) return;
    parse_into(html, len, dom_slot(parent), dom_valid(owner_doc) ? dom_slot(owner_doc) : 0);
}

DomParseRoots dom_parse_roots(NodeId fragment) {
    DomParseRoots r = { DOM_NULL, DOM_NULL, DOM_NULL };
    if (!dom_valid(fragment)) return r;
    static CssAtom a_head, a_body, a_html;
    if (!a_head) {
        css_atom_reserve(3);
        a_head = css_atom_intern("head", 4);
        a_body = css_atom_intern("body", 4);
        a_html = css_atom_intern("html", 4);
    }
    for (uint32_t s = g_dom.first[dom_slot(fragment)]; s; s = g_dom.next[s]) {
        if (g_dom.kind[s] != DOM_K_ELEMENT) continue;
        if (!r.root) r.root = dom_id_of(s);
        CssAtom t = g_dom.tag[s];
        if (t == a_head) r.head = dom_id_of(s);
        else if (t == a_body) r.body = dom_id_of(s);
        else if (t == a_html) {
            for (uint32_t c = g_dom.first[s]; c; c = g_dom.next[c]) {
                if (g_dom.kind[c] != DOM_K_ELEMENT) continue;
                if (g_dom.tag[c] == a_head) r.head = dom_id_of(c);
                else if (g_dom.tag[c] == a_body) r.body = dom_id_of(c);
            }
        }
    }
    return r;
}

NodeId dom_parse_fragment_source(NodeId fragment) {
    DomParseRoots r = dom_parse_roots(fragment);
    NodeId first = dom_first_child(r.body);
    if (first) return first;
    return dom_first_child(fragment);
}
