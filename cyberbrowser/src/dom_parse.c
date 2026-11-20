/* HTML tokenizer and tree builder over the node store.  See dom_parse.h. */
#include "dom_parse.h"
#include "cyber_env.h"
#include "job_system.h"
#include "dom_wrap.h"

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
    TK_COMMENT,         /* comment (or bogus comment): text = its data */
    TK_DOCTYPE,         /* "<!doctype ...>": text = what follows "doctype" */
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
    bool doc_start;             /* (the input's start) */
    bool atoms_full;            /* out: the atom table ran out (redo serially) */
    bool overrun;               /* out: a speculative chunk ran past its slack */
    size_t full_len;            /* the real input length (len may be cut short) */
    DomArena arena;
    DomTok *toks;
    size_t n, cap;
    DomAttr *scratch;           /* attribute scratch */
    size_t scratch_cap;
    int fdepth, fip;            /* (svg/math open, and HTML integration points in them: no raw text in foreign content) */
} TokChunk;

/* (foreign content's bounds as the tokenizer can see them: svg/math in, an integration point's HTML, a breakout tag out) */
static bool tok_name_is(const char *s, const char *const *names) {
    for (; *names; names++) if (!strcasecmp(s, *names)) return true;
    return false;
}
static const char *const k_ip_names[] = { "foreignobject", "desc", "title", "mi", "mo", "mn", "ms", "mtext", NULL };
static const char *const k_breakout[] = { "b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed",
    "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta", "nobr", "ol", "p", "pre", "ruby",
    "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u", "ul", "var", NULL };
static void tok_foreign_start(TokChunk *c, const char *tn, bool self_closed, bool ax_html) {
    if (!tn) return;
    if (c->fdepth > 0 && !self_closed && ax_html && !strcasecmp(tn, "annotation-xml")) { c->fip++; return; }
    bool sm = !strcasecmp(tn, "svg") || !strcasecmp(tn, "math");
    if (c->fdepth == 0) { if (sm && !self_closed) { c->fdepth = 1; c->fip = 0; } return; }
    if (c->fip == 0 && tok_name_is(tn, k_breakout)) { c->fdepth = 0; c->fip = 0; return; }
    if (self_closed) return;
    if (sm) c->fdepth++;
    else if (c->fip == 0 && tok_name_is(tn, k_ip_names)) c->fip++;
    else if (c->fip > 0 && tok_name_is(tn, k_ip_names)) c->fip++;
}
static void tok_foreign_end(TokChunk *c, const char *h, size_t p, size_t len) {
    if (c->fdepth == 0) return;
    char nm[32];
    size_t k = 0;
    while (p < len && k + 1 < sizeof nm && (isalnum((unsigned char)h[p]) || h[p] == '-')) nm[k++] = h[p++];
    nm[k] = 0;
    if (!strcasecmp(nm, "svg") || !strcasecmp(nm, "math")) { if (--c->fdepth <= 0) { c->fdepth = 0; c->fip = 0; } }
    else if (c->fip > 0 && (tok_name_is(nm, k_ip_names) || !strcasecmp(nm, "annotation-xml"))) c->fip--;
}

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

/* Append code point cp as UTF-8; returns the byte count. */
static int utf8_put(char *d, uint32_t cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) { d[0] = (char)cp; return 1; }
    if (cp < 0x800) { d[0] = (char)(0xC0 | (cp >> 6)); d[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        d[0] = (char)(0xE0 | (cp >> 12)); d[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        d[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    d[0] = (char)(0xF0 | (cp >> 18)); d[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    d[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); d[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

#include "html_entities.inc"

/* (its place in the table, -1 none) */
static int html_entity_find(const char *name, size_t len) {
    int lo = 0, hi = (int)(sizeof(k_html_entities) / sizeof(k_html_entities[0])) - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        const char *n = k_html_entities[m].name;
        size_t nl = strlen(n);
        int c = memcmp(name, n, len < nl ? len : nl);
        if (!c) c = len < nl ? -1 : len > nl ? 1 : 0;
        if (!c) return m;
        if (c < 0) hi = m - 1; else lo = m + 1;
    }
    return -1;
}

/* The code points of the named reference name (without '&' and ';'); 0 if not one.
 * A few are two: *second the other, else 0. */
uint32_t dom_parse_named_entity2(const char *name, size_t len, uint32_t *second) {
    int k = html_entity_find(name, len);
    if (second) *second = k >= 0 ? k_html_entities[k].cp2 : 0;
    return k >= 0 ? k_html_entities[k].cp : 0;
}

uint32_t dom_parse_named_entity(const char *name, size_t len) {
    return dom_parse_named_entity2(name, len, NULL);
}

/* Character references, in place on a NUL-terminated buffer, as HTML's tokenizer takes them:
 * numeric ones, and the longest name that matches (with its semicolon, or without one for
 * the legacy names; in an attribute's value not when '=' or a letter or digit follows).
 * Anything else is left as written.  Two references grow (&nGt;, &nLt;: 6 bytes for 5): the
 * buffer has room for that, see dom_decode_room.  Returns the new length. */
static uint32_t decode_entities(char *str, size_t len, bool attr) {
    char *dest = str, *end = str + len;
    const char *src = str;
    while (src < end) {   /* (a NUL in text stays, for the tree builder to drop or replace) */
        if (*src != '&') { *dest++ = *src++; continue; }
        if (src[1] == '#') {
            bool hex = src[2] == 'x' || src[2] == 'X';
            const char *q = src + (hex ? 3 : 2);
            uint32_t val = 0;
            int digits = 0;
            while (hex ? isxdigit((unsigned char)*q) : isdigit((unsigned char)*q)) {
                int dv = isdigit((unsigned char)*q) ? *q - '0' : (tolower((unsigned char)*q) - 'a' + 10);
                if (val < 0x110000) val = val * (hex ? 16u : 10u) + (uint32_t)dv;
                q++;
                digits++;
            }
            if (digits) {
                if (*q == ';') q++;
                /* Windows-1252 C1 remaps (&#150; and friends). */
                static const uint16_t c1[32] = {
                    0x20AC, 0x81, 0x201A, 0x192, 0x201E, 0x2026, 0x2020, 0x2021, 0x2C6, 0x2030,
                    0x160, 0x2039, 0x152, 0x8D, 0x17D, 0x8F, 0x90, 0x2018, 0x2019, 0x201C,
                    0x201D, 0x2022, 0x2013, 0x2014, 0x2DC, 0x2122, 0x161, 0x203A, 0x153, 0x9D,
                    0x17E, 0x178 };
                if (val >= 0x80 && val < 0xA0) val = c1[val - 0x80];
                /* (none, past Unicode, a surrogate: the replacement character) */
                if (val == 0 || val >= 0x110000 || (val >= 0xD800 && val <= 0xDFFF)) val = 0xFFFD;
                dest += utf8_put(dest, val);
                src = q;
                continue;
            }
        } else if (isalnum((unsigned char)src[1])) {
            const char *q = src + 1;
            while (isalnum((unsigned char)*q) && q - src <= 32) q++;
            size_t run = (size_t)(q - src - 1);
            int k = *q == ';' ? html_entity_find(src + 1, run) : -1;
            size_t used = run + 2;   /* (with '&' and ';') */
            if (k < 0) {
                /* (the longest legacy name it starts with) */
                for (size_t l = run; l >= 2 && k < 0; l--) {
                    int m = html_entity_find(src + 1, l);
                    if (m >= 0 && k_html_entities[m].legacy) { k = m; used = l + 1; }
                }
                if (k >= 0 && attr) {
                    char nx = src[used];
                    if (nx == '=' || isalnum((unsigned char)nx)) k = -1;
                }
            }
            if (k >= 0) {
                char *after = (char *)src + used;
                size_t need = (size_t)utf8_put((char[4]){0}, k_html_entities[k].cp) +
                              (k_html_entities[k].cp2 ? (size_t)utf8_put((char[4]){0}, k_html_entities[k].cp2) : 0);
                /* (what's left moved along when this one is longer than all that's free) */
                if (dest + need > after) {
                    size_t by = (size_t)(dest + need - after);
                    memmove(after + by, after, (size_t)(end - after) + 1);
                    after += by;
                    end += by;
                }
                dest += utf8_put(dest, k_html_entities[k].cp);
                if (k_html_entities[k].cp2) dest += utf8_put(dest, k_html_entities[k].cp2);
                src = after;
                continue;
            }
        }
        *dest++ = *src++;
    }
    *dest = '\0';
    return (uint32_t)(dest - str);
}

uint32_t dom_parse_decode_entities(char *str, size_t len) { return decode_entities(str, len, false); }

/* (what a buffer for n bytes of source should have, so a reference that grows fits) */
static inline size_t dom_decode_room(size_t n) { return n + n / 5 + 2; }

/* Text: kept as written (white-space processing belongs to CSS), with CR LF
 * and lone CR normalized to LF and character references decoded. */
static DomStr tok_text(TokChunk *c, const char *s, size_t n) {
    DomStr r;
    r.s = dom_arena_alloc(&c->arena, dom_decode_room(n), &r.blk);
    size_t cl = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') continue;
            r.s[cl++] = '\n';
        } else {
            r.s[cl++] = s[i];
        }
    }
    r.s[cl] = '\0';
    r.len = dom_parse_decode_entities(r.s, cl);
    return r;
}

static DomStr tok_attr_value(TokChunk *c, const char *s, size_t n) {
    if (n == 0) return (DomStr){ NULL, 0, 0 };
    DomStr r;
    r.s = dom_arena_alloc(&c->arena, dom_decode_room(n), &r.blk);
    memcpy(r.s, s, n);
    r.s[n] = 0;
    r.len = decode_entities(r.s, strlen(r.s), true);
    if (r.len == 0) { dom_str_release(&r); return (DomStr){ NULL, 0, 0 }; }
    return r;
}

/* Raw-text tag atoms, interned before any job runs (dom_tag_flags() keeps an
 * engine-thread cache, so jobs must not call it). */
static CssAtom g_raw_atoms[10];   /* (RCDATA: textarea, title; RAWTEXT the rest, noscript as scripting is on; plaintext: all the rest) */
/* The names parsed_ns() looks for. */
enum { FX_SVG, FX_MATH, FX_FOREIGNOBJECT, FX_DESC, FX_TITLE, FX_MI, FX_MO, FX_MN, FX_MS, FX_MTEXT, FX_ANNOTATION_XML, FX_N };
static CssAtom g_fx[FX_N];
static void init_raw_atoms(void) {
    if (g_raw_atoms[0]) return;
    static const char *const names[10] = { "script", "style", "textarea", "title", "xmp", "noembed", "noframes", "noscript", "plaintext", "iframe" };
    static const char *const fx[FX_N] = { "svg", "math", "foreignobject", "desc", "title", "mi", "mo", "mn", "ms",
                                          "mtext", "annotation-xml" };
    css_atom_reserve(10 + FX_N);
    for (int i = 0; i < FX_N; i++) g_fx[i] = css_atom_intern(fx[i], strlen(fx[i]));
    for (int i = 0; i < 10; i++) g_raw_atoms[i] = css_atom_intern(names[i], strlen(names[i]));
}
/* (a document whose scripting is off, a sandboxed frame's without allow-scripts: noscript is markup, HTML 13.2.6.4.4) */
bool g_dom_parse_noscript_plain = false;
static inline bool is_raw_atom(CssAtom a) {
    for (int i = 0; i < 10; i++) if (a == g_raw_atoms[i]) return !(i == 7 && g_dom_parse_noscript_plain);
    return false;
}

static bool is_tag_delim(char ch) {
    return ch == '>' || ch == '/' || ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

/* Tokenize a start tag at c->html[pos] == '<' (not followed by '/').
 * Returns the position after it (and after the raw content of raw tags). */
static size_t raw_text_end(const char *h, size_t len, size_t p, const char *tn, size_t tl, bool script);
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
    bool closed_ = false;
    for (;;) {
        while (p < len && c_isspace((unsigned char)h[p])) p++;
        if (p >= len) break;
        if (h[p] == '>') { p++; closed_ = true; break; }
        if (h[p] == '/' && p + 1 < len && h[p + 1] == '>') { p += 2; flags |= TKF_SELF_CLOSED; closed_ = true; break; }
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
                    /* Unquoted: up to a space or '>' ('/' is part of it). */
                    vb = p;
                    while (p < len) {
                        char ch = h[p];
                        if (ch == '>' || c_isspace((unsigned char)ch)) break;
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
        c->scratch[na].ns = c->scratch[na].qname = 0;
        na++;
    }
    /* (the input ending inside the tag: no tag, HTML 13.2.5.8 "EOF in tag"; a stream holds back what reaches its end) */
    if (!closed_ && p >= len && len >= c->full_len) return len;
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
    bool in_foreign = c->fdepth > 0 && c->fip == 0;
    bool ax_html = false;
    if (c->fdepth > 0)
        for (size_t q = 0; q < na; q++) {
            const char *an = css_atom_str(c->scratch[q].name);
            const DomStr *v = &c->scratch[q].value;
            if (an && !strcasecmp(an, "encoding") && v->s &&
                ((v->len == 9 && !strncasecmp(v->s, "text/html", 9)) || (v->len == 21 && !strncasecmp(v->s, "application/xhtml+xml", 21)))) ax_html = true;
        }
    tok_foreign_start(c, css_atom_str(tag), (flags & TKF_SELF_CLOSED) != 0, ax_html);
    if (in_foreign && c->fdepth == 0) in_foreign = false;   /* (a breakout tag: HTML again) */
    if (!(flags & TKF_SELF_CLOSED) && is_raw_atom(tag) && !in_foreign) {
        /* Raw content up to "</" + tag (prefix, case-insensitive). */
        const char *tn = css_atom_str(tag);
        size_t tl = strlen(tn);
        size_t rb = p;
        if (tag == g_raw_atoms[8]) p = len;   /* (plaintext: no end tag ends it) */
        else p = raw_text_end(h, len, p, tn, tl, tag == g_raw_atoms[0]);
        DomTok *r = tok_push(c);
        t = &c->toks[c->n - 2];
        r->kind = TK_RAW;
        r->src = (uint32_t)rb;
        r->text = tag == g_raw_atoms[2] || tag == g_raw_atoms[3] ? tok_text(c, h + rb, p - rb)   /* (RCDATA) */
                                                                 : dom_arena_str(&c->arena, h + rb, p - rb);
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
    while (pos < len && pos < c->limit) {
        if (h[pos] == '<') {
            if (pos + 4 <= len && memcmp(h + pos, "<!--", 4) == 0) {
                /* A comment: its data runs to the next "-->" (as the legacy
                 * parser's extent; HTML ends "<!-->" and "<!--->" at once). */
                size_t from = pos, data = pos + 4, data_end;
                /* ("<!-->" and "<!--->": an empty comment, ended at once) */
                if (data < len && h[data] == '>') { data_end = data; pos = data + 1; }
                else if (data + 1 < len && h[data] == '-' && h[data + 1] == '>') { data_end = data; pos = data + 2; }
                else {
                    pos = data;
                    for (;;) {
                        const char *d = pos < len ? memchr(h + pos, '-', len - pos) : NULL;
                        if (!d) { data_end = pos = len; break; }
                        pos = (size_t)(d - h);
                        if (pos + 3 <= len && d[1] == '-' && d[2] == '>') { data_end = pos; pos += 3; break; }
                        if (pos + 4 <= len && d[1] == '-' && d[2] == '!' && d[3] == '>') { data_end = pos; pos += 4; break; }   /* (--!>: an end too, HTML 13.2.5.52) */
                        pos++;
                    }
                }
                DomTok *t = tok_push(c);
                t->kind = TK_COMMENT;
                t->src = (uint32_t)from;
                t->end = (uint32_t)pos;
                t->text = dom_arena_str(&c->arena, h + data, data_end - data);
                continue;
            }
            /* ("<![CDATA[": to its "]]>" (or the end), as a CDATA section in foreign content is; the builder makes it a bogus
             * comment to the first '>' and text after that in HTML content.  Only when what lies between is plain text) */
            if (pos + 9 <= len && h[pos + 1] == '!' && !memcmp(h + pos + 2, "[CDATA[", 7)) {
                const char *gt = memchr(h + pos, '>', len - pos);
                const char *cd = NULL;
                for (size_t q = pos + 9; q + 3 <= len; q++) if (h[q] == ']' && h[q + 1] == ']' && h[q + 2] == '>') { cd = h + q; break; }
                size_t cend = cd ? (size_t)(cd - h) + 3 : len, gte = gt ? (size_t)(gt - h) : len;
                bool plain = true;
                for (size_t q = gte + 1; q < cend && plain; q++) if (h[q] == '<' || h[q] == '&') plain = false;
                if (plain && cend >= gte) {
                    size_t data = pos + 2, tend = cd ? (size_t)(cd - h) + 2 : len;
                    DomTok *t = tok_push(c);
                    t->kind = TK_COMMENT;
                    t->src = (uint32_t)pos;
                    t->end = (uint32_t)cend;
                    t->text = dom_arena_str(&c->arena, h + data, tend - data);
                    t->attr_blk = (uint32_t)(gte - data) + 1;   /* (the first '>', as an offset in the text, + 1) */
                    if (cd) t->flags |= 2;                      /* (its "]]>" found) */
                    pos = cend;
                    continue;
                }
            }
            if (pos + 1 < len && (h[pos + 1] == '!' || h[pos + 1] == '?')) {
                /* Any other "<!...>" and "<?...>" is a bogus comment up to its
                 * '>' (HTML §13.2.5.42); "<!doctype" is a doctype (the builder
                 * drops one past the start). */
                const char *gt = memchr(h + pos, '>', len - pos);
                size_t from = pos, e = gt ? (size_t)(gt - h) : len;
                pos = gt ? e + 1 : len;
                size_t data = from + (h[from + 1] == '!' ? 2 : 1);
                bool doctype = h[from + 1] == '!' && data + 7 <= len && strncasecmp(h + data, "doctype", 7) == 0;
                if (doctype) data += 7;
                DomTok *t = tok_push(c);
                t->kind = doctype ? TK_DOCTYPE : TK_COMMENT;
                t->src = (uint32_t)from;
                t->end = (uint32_t)pos;
                t->text = dom_arena_str(&c->arena, h + data, e > data ? e - data : 0);
                continue;
            }
            /* ("<" not before a letter, "!", "?" or "/": text; "</" not before a letter: "</>" nothing, else a bogus comment, or text at the end) */
            {
                unsigned char n1 = pos + 1 < len ? (unsigned char)h[pos + 1] : 0;
                bool alpha1 = (n1 >= 'a' && n1 <= 'z') || (n1 >= 'A' && n1 <= 'Z');
                if (n1 == '/') {
                    unsigned char n2 = pos + 2 < len ? (unsigned char)h[pos + 2] : 0;
                    bool alpha2 = (n2 >= 'a' && n2 <= 'z') || (n2 >= 'A' && n2 <= 'Z');
                    if (!alpha2 && pos + 2 < len) {
                        if (n2 == '>') { pos += 3; continue; }
                        const char *gt = memchr(h + pos + 2, '>', len - pos - 2);
                        size_t from = pos, e = gt ? (size_t)(gt - h) : len;
                        pos = gt ? e + 1 : len;
                        DomTok *t = tok_push(c);
                        t->kind = TK_COMMENT;
                        t->src = (uint32_t)from;
                        t->end = (uint32_t)pos;
                        t->text = dom_arena_str(&c->arena, h + from + 2, e - from - 2);
                        continue;
                    }
                    alpha1 = alpha2 || pos + 2 >= len;
                    if (pos + 2 >= len) alpha1 = false;
                    else alpha1 = true;
                }
                if (!alpha1 && n1 != '!' && n1 != '?') {
                    const char *lt = pos + 1 < len ? memchr(h + pos + 1, '<', len - pos - 1) : NULL;
                    size_t e = lt ? (size_t)(lt - h) : len;
                    DomTok *t = tok_push(c);
                    t->kind = TK_TEXT;
                    t->src = (uint32_t)pos;
                    t->end = (uint32_t)e;
                    t->text = tok_text(c, h + pos, e - pos);
                    pos = e;
                    continue;
                }
            }
            if (pos + 1 < len && h[pos + 1] == '/') {
                size_t e = pos + 2;
                tok_foreign_end(c, h, e, len);
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
        } else if (t->kind == TK_TEXT || t->kind == TK_RAW || t->kind == TK_COMMENT || t->kind == TK_DOCTYPE) {
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
/* (where raw text that started at p ends: at its "</tag" followed by white space, '/' or '>', or len.  A script's
 * text keeps "<!--...<script>...</script>...-->" inside it: the script data escape states, HTML 13.2.5.15-31) */
static bool raw_end_at(const char *h, size_t len, size_t q, const char *tn, size_t tl) {
    if (q + 2 + tl > len || h[q] != '<' || h[q + 1] != '/' || strncasecmp(h + q + 2, tn, tl) != 0) return false;
    if (q + 2 + tl == len) return false;
    char c = h[q + 2 + tl];
    return c == '>' || c == '/' || c_isspace((unsigned char)c);
}
static size_t raw_text_end(const char *h, size_t len, size_t p, const char *tn, size_t tl, bool script) {
    int esc = 0;   /* (0 script data; 1 escaped; 2 double escaped) */
    while (p < len) {
        if (!script) {
            const char *lt = memchr(h + p, '<', len - p);
            if (!lt) return len;
            p = (size_t)(lt - h);
            if (raw_end_at(h, len, p, tn, tl)) return p;
            p++;
            continue;
        }
        char ch = h[p];
        if (ch == '<') {
            if (esc != 2 && raw_end_at(h, len, p, tn, tl)) return p;
            if (esc == 0 && p + 4 <= len && !memcmp(h + p, "<!--", 4)) { esc = 1; p += 4; continue; }
            if (esc == 1 && p + 7 <= len && !strncasecmp(h + p + 1, "script", 6) &&
                (p + 7 == len || h[p + 7] == '>' || h[p + 7] == '/' || c_isspace((unsigned char)h[p + 7]))) { esc = 2; p += 7; continue; }
            if (esc == 2 && p + 8 <= len && h[p + 1] == '/' && !strncasecmp(h + p + 2, "script", 6) &&
                (p + 8 == len || h[p + 8] == '>' || h[p + 8] == '/' || c_isspace((unsigned char)h[p + 8]))) { esc = 1; p += 8; continue; }
        } else if (ch == '-' && esc && p + 3 <= len && h[p + 1] == '-' && h[p + 2] == '>') { esc = 0; p += 3; continue; }
        p++;
    }
    return len;
}

static size_t prescan_skip(const char *h, size_t len, size_t p) {
    if (p + 4 <= len && memcmp(h + p, "<!--", 4) == 0) {
        /* (as the tokenizer: "<!-->" and "<!--->" end at once) */
        if (p + 4 < len && h[p + 4] == '>') return p + 5;
        if (p + 5 < len && h[p + 4] == '-' && h[p + 5] == '>') return p + 6;
        const char *e = NULL;
        for (size_t q = p + 4; q + 3 <= len; ) {
            const char *d = memchr(h + q, '-', len - q);
            if (!d || (size_t)(d - h) + 3 > len) break;
            if (d[1] == '-' && d[2] == '>') { e = d + 3; break; }
            q = (size_t)(d - h) + 1;
        }
        return e ? (size_t)(e - h) : len;
    }
    if (p + 10 < len && strncasecmp(h + p + 1, "plaintext", 9) == 0 &&
        (h[p + 10] == '>' || c_isspace((unsigned char)h[p + 10]) || h[p + 10] == '/')) return len;
    static const char *const raw[] = { "script", "style", "textarea", "title", "xmp", "noembed", "noframes", "noscript", "iframe" };
    for (int i = 0; i < (g_dom_parse_noscript_plain ? 7 : 8); i++) {
        size_t tl = strlen(raw[i]);
        if (p + 1 + tl < len && strncasecmp(h + p + 1, raw[i], tl) == 0 &&
            (h[p + 1 + tl] == '>' || c_isspace((unsigned char)h[p + 1 + tl]) || h[p + 1 + tl] == '/')) {
            const char *gt = memchr(h + p, '>', len - p);
            if (!gt) return len;
            size_t q = raw_text_end(h, len, (size_t)(gt - h) + 1, raw[i], tl, i == 0);
            if (q >= len) return len;
            const char *g2 = memchr(h + q, '>', len - q);
            return g2 ? (size_t)(g2 - h) + 1 : len;
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

#define AFL_MAX 64
typedef struct Builder {
    const char *html;
    size_t len;
    uint32_t frag;              /* container slot */
    uint32_t owner;
    uint32_t *stack;            /* open elements (slots) */
    uint32_t sp;
    CssAtom id_name;
    bool keep_top_text;         /* fragment parsing: text outside any element is kept */
    /* HTML's implied end tags and end tags across the stack (page markup,
     * innerHTML, DOMParser).  Off for re-parsing a serialized DOM, whose
     * nesting scripts may have made any shape (html_parse's snapshots). */
    bool html_rules;
    /* Streaming (dom_stream_*): the tree is live while it is built. */
    bool streaming;
    uint8_t *fresh;             /* per open element: created in the current step */
    uint32_t *roots;            /* nodes the current step attached to older nodes */
    uint32_t nroots, roots_cap;
    uint32_t stopped_script;    /* the <script> the step stopped after */
    /* A whole document (streaming into the Document): html, head and body
     * are implied as the HTML tree builder's insertion modes do. */
    bool doc_mode;
    uint8_t phase;              /* DOC_* */
    uint32_t el_html, el_head, el_body;   /* their slots, once there */
    uint8_t after_body;         /* 1 after </body> ("after body"), 2 after </html> ("after after body"); 0 back in the body */
    uint32_t afl[AFL_MAX];      /* the list of active formatting elements (slots; AFL_MARKER a marker) */
    uint32_t afl_n;
    uint32_t form_ptr;          /* the form element pointer: the open form, outside templates */
    bool fs_bad;                /* the frameset-ok flag "not ok": a frameset no longer replaces the body */
    uint32_t tm_slot[32];       /* templates' contents insertion modes (TM_*), by template slot */
    uint8_t tm_mode[32];
    uint8_t tm_n;
} Builder;

/* A template's contents insertion mode (HTML 13.2.6.4.18), set by the first start tag with the template current */
enum { TM_NONE, TM_BODY, TM_TABLE, TM_TBODY, TM_ROW, TM_COLGROUP };
static int tmode_get(const Builder *b, uint32_t s) {
    for (uint32_t i = 0; i < b->tm_n; i++) if (b->tm_slot[i] == s) return b->tm_mode[i];
    return TM_NONE;
}
static void tmode_set(Builder *b, uint32_t s, int m) {
    for (uint32_t i = 0; i < b->tm_n; i++) if (b->tm_slot[i] == s) { b->tm_mode[i] = (uint8_t)m; return; }
    uint32_t i = b->tm_n < 32 ? b->tm_n++ : 31;
    b->tm_slot[i] = s;
    b->tm_mode[i] = (uint8_t)m;
}

/* The insertion modes before "in body" (HTML §13.2.6.4.1-6). */
enum { DOC_INITIAL, DOC_BEFORE_HEAD, DOC_IN_HEAD, DOC_AFTER_HEAD, DOC_IN_BODY, DOC_IN_FRAMESET };

static CssAtom g_template_atom;

/* Where a streamed node goes under `parent`: a template's children are its
 * content's (they are inert, so not roots); others are roots when the parent
 * is older than this step. */
static uint32_t stream_parent(Builder *b, uint32_t parent, bool parent_fresh, uint32_t child) {
    if (!b->streaming) return parent;
    if (g_dom.kind[parent] == DOM_K_ELEMENT && g_dom.tag[parent] == g_template_atom) {
        NodeId frag = dom_template_content(dom_id_of(parent));
        if (!frag) {
            frag = dom_create_fragment(dom_id_of(b->owner));
            dom_set_template_content(dom_id_of(parent), frag);
        }
        return dom_slot(frag);
    }
    if (!parent_fresh && !(g_dom.flags[parent] & DOMF_TEMPLATE_CONTENT)) {
        if (b->nroots == b->roots_cap) {
            b->roots_cap = b->roots_cap ? b->roots_cap * 2 : 64;
            b->roots = realloc(b->roots, b->roots_cap * sizeof(uint32_t));
        }
        b->roots[b->nroots++] = child;
    }
    return parent;
}

/* SVG's element names with upper case (HTML 13.2.6.5: the tokenizer
 * lower-cases them). */
static const char *const g_svg_names[] = {
    "altGlyph", "altGlyphDef", "altGlyphItem", "animateColor", "animateMotion", "animateTransform", "clipPath",
    "feBlend", "feColorMatrix", "feComponentTransfer", "feComposite", "feConvolveMatrix", "feDiffuseLighting",
    "feDisplacementMap", "feDistantLight", "feDropShadow", "feFlood", "feFuncA", "feFuncB", "feFuncG", "feFuncR",
    "feGaussianBlur", "feImage", "feMerge", "feMergeNode", "feMorphology", "feOffset", "fePointLight",
    "feSpecularLighting", "feSpotLight", "feTile", "feTurbulence", "foreignObject", "glyphRef", "linearGradient",
    "radialGradient", "textPath",
};

/* A parsed element's namespace: <svg> and <math> start foreign content, which
 * their descendants are in, but for the children of SVG's and MathML's HTML
 * integration and text integration points (HTML 13.2.6.5, simplified: no
 * breakout of HTML tags). */
static uint32_t parsed_ns(CssAtom tag, uint32_t parent) {
    CssAtom svg = g_fx[FX_SVG], math = g_fx[FX_MATH], fo = g_fx[FX_FOREIGNOBJECT], desc = g_fx[FX_DESC],
            title = g_fx[FX_TITLE], mi = g_fx[FX_MI], mo = g_fx[FX_MO], mn = g_fx[FX_MN], ms = g_fx[FX_MS],
            mtext = g_fx[FX_MTEXT], ax = g_fx[FX_ANNOTATION_XML];
    uint32_t pns = parent && g_dom.kind[parent] == DOM_K_ELEMENT ? g_dom.ns[parent] : DOM_NS_HTML;
    CssAtom pt = parent ? g_dom.tag[parent] : CSS_ATOM_NONE;
    if (pns == DOM_NS_SVG && pt != fo && pt != desc && pt != title) return DOM_NS_SVG;
    if (pns == DOM_NS_MATHML && pt != mi && pt != mo && pt != mn && pt != ms && pt != mtext && pt != ax)
        return tag == svg && pt == ax ? DOM_NS_SVG : DOM_NS_MATHML;
    /* (in a MathML text integration point, mglyph and malignmark stay MathML) */
    if (pns == DOM_NS_MATHML && (pt == mi || pt == mo || pt == mn || pt == ms || pt == mtext)) {
        static CssAtom mglyph, malignmark;
        if (!mglyph) { css_atom_reserve(2); mglyph = css_atom_intern("mglyph", 6); malignmark = css_atom_intern("malignmark", 10); }
        if (tag == mglyph || tag == malignmark) return DOM_NS_MATHML;
    }
    return tag == svg ? DOM_NS_SVG : tag == math ? DOM_NS_MATHML : DOM_NS_HTML;
}

/* (an element the parser is in foreign content under, but for its integration points) */
/* (an annotation-xml whose encoding is text/html or application/xhtml+xml, ASCII case-insensitively: an HTML integration point) */
static bool annotation_xml_html(uint32_t s) {
    const char *e = dom_get_attr_str(dom_id_of(s), "encoding");
    return e && (!strcasecmp(e, "text/html") || !strcasecmp(e, "application/xhtml+xml"));
}
static bool foreign_current(uint32_t s) {
    if (!s || g_dom.kind[s] != DOM_K_ELEMENT) return false;
    uint32_t ns = g_dom.ns[s];
    CssAtom a = g_dom.tag[s];
    if (ns == DOM_NS_SVG) return a != g_fx[FX_FOREIGNOBJECT] && a != g_fx[FX_DESC] && a != g_fx[FX_TITLE];
    if (ns == DOM_NS_MATHML) return a != g_fx[FX_MI] && a != g_fx[FX_MO] && a != g_fx[FX_MN] && a != g_fx[FX_MS] &&
                                    a != g_fx[FX_MTEXT] && !(a == g_fx[FX_ANNOTATION_XML] && annotation_xml_html(s));
    return false;
}
/* (HTML 13.2.6.5: a start tag that breaks out of foreign content) */
static bool foreign_breakout_tag(const DomTok *t) {
    static const char *const k[] = { "b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em",
        "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta", "nobr", "ol",
        "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u", "ul", "var" };
    const char *n = css_atom_str(t->tag);
    if (!n) return false;
    if (!strcmp(n, "font")) {
        for (uint16_t i = 0; i < t->nattr; i++) {
            const char *an = css_atom_str(t->attrs[i].name);
            if (an && (!strcmp(an, "color") || !strcmp(an, "face") || !strcmp(an, "size"))) return true;
        }
        return false;
    }
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(n, k[i])) return true;
    return false;
}

/* (foster parenting: the node the next element or text goes before, in the table's parent; 0 none) */
static uint32_t g_foster_ref;
static void link_child(uint32_t parent, uint32_t s) {
    if (g_foster_ref && g_dom.parent[g_foster_ref] == parent) dom_link_before(dom_id_of(parent), dom_id_of(s), dom_id_of(g_foster_ref));
    else dom_link_append(dom_id_of(parent), dom_id_of(s));
}

/* (innerHTML's context element, for the namespace of what goes at the top: 0 none) */
static uint32_t g_frag_context;
void dom_parse_set_context(NodeId el) { g_frag_context = dom_valid(el) && dom_kind(el) == DOM_K_ELEMENT ? dom_slot(el) : 0; }

static uint32_t build_element(Builder *b, DomTok *t, uint32_t parent, bool parent_fresh) {
    uint32_t s = dom_alloc_slot(DOM_K_ELEMENT, dom_id_of(b->owner));
    g_dom.tag[s] = t->tag;
    bool at_top = parent == b->frag && g_frag_context && b->keep_top_text;
    uint32_t ns = parsed_ns(t->tag, at_top ? g_frag_context : parent);
    if (at_top && ns != DOM_NS_HTML && foreign_current(g_frag_context) && foreign_breakout_tag(t)) ns = DOM_NS_HTML;
    if (ns != DOM_NS_HTML) {
        g_dom.ns[s] = ns;
        if (ns == DOM_NS_SVG) {
            const char *tn = css_atom_str(t->tag);
            for (size_t i = 0; tn && i < sizeof g_svg_names / sizeof g_svg_names[0]; i++)
                if (!strcasecmp(tn, g_svg_names[i])) {
                    g_dom.lname[s] = dom_name_intern(g_svg_names[i], strlen(g_svg_names[i]));
                    break;
                }
        }
    }
    g_dom.flags[s] = dom_tag_flags(t->tag) | ((t->flags & TKF_SELF_CLOSED) ? DOMF_SELF_CLOSED : 0);
    if (t->tag == g_raw_atoms[0]) g_dom.flags[s] |= DOMF_PARSER_SCRIPT;
    if (g_dom.flags[s] & DOMF_MEDIA) dom_media_note(s);
    if (t->nattr) {
        g_dom.attrs[s] = (DomAttrList){ t->attrs, t->nattr, t->nattr, t->attr_blk };
        t->attrs = NULL; t->nattr = 0; t->attr_blk = 0;      /* ownership moved */
        DomAttrList *l = &g_dom.attrs[s];
        /* (adjust foreign attributes, HTML 13.2.6.5: xlink:*, xml:* and
         * xmlns on SVG and MathML elements are in their namespaces) */
        /* (and their names' case, HTML 13.2.6.5: SVG's camel-cased ones, MathML's definitionURL) */
        static const char *const svg_attr_case[] = { "attributeName", "attributeType", "baseFrequency", "baseProfile", "calcMode", "clipPathUnits", "diffuseConstant", "edgeMode", "filterUnits", "glyphRef", "gradientTransform", "gradientUnits", "kernelMatrix", "kernelUnitLength", "keyPoints", "keySplines", "keyTimes", "lengthAdjust", "limitingConeAngle", "markerHeight", "markerUnits", "markerWidth", "maskContentUnits", "maskUnits", "numOctaves", "pathLength", "patternContentUnits", "patternTransform", "patternUnits", "pointsAtX", "pointsAtY", "pointsAtZ", "preserveAlpha", "preserveAspectRatio", "primitiveUnits", "refX", "refY", "repeatCount", "repeatDur", "requiredExtensions", "requiredFeatures", "specularConstant", "specularExponent", "spreadMethod", "startOffset", "stdDeviation", "stitchTiles", "surfaceScale", "systemLanguage", "tableValues", "targetX", "targetY", "textLength", "viewBox", "viewTarget", "xChannelSelector", "yChannelSelector", "zoomAndPan" };
        for (int i = 0; ns != DOM_NS_HTML && i < l->n; i++) {
            const char *an = css_atom_str(l->a[i].name);
            if (!an) continue;
            if (ns == DOM_NS_MATHML && !strcasecmp(an, "definitionurl")) { l->a[i].qname = dom_name_intern("definitionURL", 13); continue; }
            if (l->a[i].qname) continue;
            else if (ns == DOM_NS_SVG)
                for (size_t k = 0; k < sizeof svg_attr_case / sizeof svg_attr_case[0]; k++)
                    if (!strcasecmp(an, svg_attr_case[k])) { if (strcmp(an, svg_attr_case[k])) l->a[i].qname = dom_name_intern(svg_attr_case[k], strlen(svg_attr_case[k])); break; }
        }
        for (int i = 0; ns != DOM_NS_HTML && i < l->n; i++) {
            const char *an = css_atom_str(l->a[i].name);
            if (!an) continue;
            static const char *const xlink[] = { "actuate", "arcrole", "href", "role", "show", "title", "type" };
            if (!strncmp(an, "xlink:", 6)) {
                for (size_t k = 0; k < sizeof xlink / sizeof xlink[0]; k++)
                    if (!strcmp(an + 6, xlink[k])) l->a[i].ns = DOM_NS_XLINK;
            } else if (!strcmp(an, "xml:lang") || !strcmp(an, "xml:space")) {
                l->a[i].ns = DOM_NS_XML;
            } else if (!strcmp(an, "xmlns") || !strcmp(an, "xmlns:xlink")) {
                l->a[i].ns = DOM_NS_XMLNS;
            }
        }
        static CssAtom class_name;
        if (!class_name) { css_atom_reserve(1); class_name = css_atom_intern("class", 5); }
        bool seen_id = false, seen_class = false;
        for (int i = 0; i < l->n && !(seen_id && seen_class); i++) {
            if (l->a[i].name == b->id_name && !seen_id) {
                seen_id = true;
                if (l->a[i].value.len) {
                    css_atom_reserve(1);
                    g_dom.id_atom[s] = css_atom_intern(l->a[i].value.s, l->a[i].value.len);
                    dom_id_index_note(s);
                }
            } else if (l->a[i].name == class_name && !seen_class) {
                seen_class = true;
                dom_cls_note(s, l->a[i].value.s, l->a[i].value.len);
            }
        }
    }
    parent = stream_parent(b, parent, parent_fresh, s);
    link_child(parent, s);
    if (g_dom.flags[s] & DOMF_CE_CANDIDATE) {
        for (uint32_t p = parent; p; p = g_dom.parent[p]) {
            if (g_dom.flags[p] & DOMF_CE_DESC) break;
            g_dom.flags[p] |= DOMF_CE_DESC;
        }
    }
    return s;
}

static void build_text(Builder *b, DomTok *t, uint32_t parent, bool parent_fresh) {
    /* (NULs: dropped in HTML content, U+FFFD in foreign content) */
    if (t->text.len && memchr(t->text.s, 0, t->text.len)) {
        uint32_t pctx = parent == b->frag && g_frag_context && b->keep_top_text ? g_frag_context : parent;
        bool foreign = t->kind == TK_RAW || foreign_current(pctx);   /* (raw text, RCDATA, script data, plaintext: U+FFFD too) */
        size_t n = t->text.len, k = 0, nul = 0;
        for (size_t i = 0; i < n; i++) if (!t->text.s[i]) nul++;
        char *o = (char *)malloc(n + nul * 2 + 1);
        if (o) {
            for (size_t i = 0; i < n; i++) {
                char ch = t->text.s[i];
                if (ch) o[k++] = ch;
                else if (foreign) { o[k++] = (char)0xEF; o[k++] = (char)0xBF; o[k++] = (char)0xBD; }
            }
            o[k] = 0;
            dom_str_release(&t->text);
            t->text = dom_str_dup(o, k);
            free(o);
            if (!k) return;
        }
    }
    /* A newline right after <pre> / <listing> is not content (HTML §13.2.6.4.7). */
    if (!g_dom.first[parent] && t->text.len && t->text.s[0] == '\n') {
        const char *tn = css_atom_str(g_dom.tag[parent]);
        if (tn && (strcasecmp(tn, "pre") == 0 || strcasecmp(tn, "listing") == 0 || strcasecmp(tn, "textarea") == 0)) {
            memmove(t->text.s, t->text.s + 1, t->text.len);   /* keeps the NUL */
            t->text.len--;
            if (!t->text.len) return;
        }
    }
    uint32_t s = dom_alloc_slot(DOM_K_TEXT, dom_id_of(b->owner));
    g_dom.text[s] = t->text;
    t->text = (DomStr){ NULL, 0, 0 };
    parent = stream_parent(b, parent, parent_fresh, s);
    link_child(parent, s);
}

/* (a "<?target data>": a processing instruction when target is a name and what follows it fits; false: a bogus comment) */
static bool build_pi(Builder *b, DomTok *t, uint32_t parent, bool parent_fresh) {
    const char *d = t->text.s;
    size_t n = t->text.len;
    if (!d || n < 2 || d[0] != '?') return false;
    size_t i = 1;
    char c0 = d[i];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z') || c0 == '_')) return false;
    while (i < n && ((d[i] >= 'a' && d[i] <= 'z') || (d[i] >= 'A' && d[i] <= 'Z') || (d[i] >= '0' && d[i] <= '9') || d[i] == '_' || d[i] == '-')) i++;
    size_t tn = i - 1;
    if (i < n && !(c_isspace((unsigned char)d[i]) || d[i] == '?')) return false;
    /* (an "xml" target, any case, or one starting so: a bogus comment still) */
    if (tn >= 3 && !strncasecmp(d + 1, "xml", 3)) return false;
    while (i < n && c_isspace((unsigned char)d[i])) i++;
    size_t ds = i, de = n;
    if (de > ds && d[de - 1] == '?') de--;
    /* (its data: a CR, or CR LF, as LF) */
    char *buf = (char *)malloc(de - ds + 1);
    if (!buf) return false;
    size_t k = 0;
    for (size_t j = ds; j < de; j++) {
        if (d[j] == '\r') { if (j + 1 < de && d[j + 1] == '\n') continue; buf[k++] = '\n'; }
        else buf[k++] = d[j];
    }
    NodeId pi = dom_create_pi(d + 1, tn, buf, k, dom_id_of(b->owner));
    free(buf);
    uint32_t s = dom_slot(pi);
    parent = stream_parent(b, parent, parent_fresh, s);
    dom_link_append(dom_id_of(parent), pi);
    return true;
}

/* (a "<![CDATA[...]]>" where the current node is foreign: its text) */
static bool build_cdata(Builder *b, DomTok *t, uint32_t parent, bool parent_fresh) {
    if (!parent || g_dom.kind[parent] != DOM_K_ELEMENT || g_dom.ns[parent] == DOM_NS_HTML) return false;
    const char *d = t->text.s;
    size_t n = t->text.len;
    if (!d || n < 7 || strncmp(d, "[CDATA[", 7)) return false;
    size_t ds = 7, de = n;
    /* (its "]]": the end's, when the section had one; one at the input's end is text) */
    if ((!t->attr_blk || (t->flags & 2)) && de >= ds + 2 && d[de - 1] == ']' && d[de - 2] == ']') de -= 2;
    if (de <= ds) return true;
    char *buf = (char *)malloc(de - ds + 1);
    if (!buf) return false;
    size_t k = 0;
    size_t nuls = 0;
    for (size_t j = ds; j < de; j++) if (!d[j]) nuls++;
    if (nuls) { char *nb2 = (char *)realloc(buf, de - ds + nuls * 2 + 1); if (!nb2) { free(buf); return false; } buf = nb2; }
    for (size_t j = ds; j < de; j++) {
        if (d[j] == '\r') { if (j + 1 < de && d[j + 1] == '\n') continue; buf[k++] = '\n'; }
        else if (!d[j]) { buf[k++] = (char)0xEF; buf[k++] = (char)0xBF; buf[k++] = (char)0xBD; }   /* (U+0000: U+FFFD, in foreign content) */
        else buf[k++] = d[j];
    }
    NodeId tx = dom_create_chardata(DOM_K_TEXT, buf, k, dom_id_of(b->owner));
    free(buf);
    uint32_t s = dom_slot(tx);
    parent = stream_parent(b, parent, parent_fresh, s);
    dom_link_append(dom_id_of(parent), tx);
    return true;
}

static void build_comment(Builder *b, DomTok *t, uint32_t parent, bool parent_fresh) {
    /* (a "<?" the input ended in, no '>': nothing) */
    if (t->src + 2 < b->len && b->html[t->src] == '<' && b->html[t->src + 1] == '?' && t->end == b->len && b->html[t->end - 1] != '>' &&
        isalpha((unsigned char)b->html[t->src + 2])) return;   /* (a processing instruction's target cut short; '<?#' and the like stay bogus comments) */
    if (t->src + 1 < b->len && b->html[t->src] == '<') {
        if (b->html[t->src + 1] == '?' && build_pi(b, t, parent, parent_fresh)) return;
        if (b->html[t->src + 1] == '!' && build_cdata(b, t, parent, parent_fresh)) return;
    }
    /* (a "<![CDATA[" in HTML content: a bogus comment to its first '>', the rest text) */
    if (t->attr_blk && t->kind == TK_COMMENT && t->text.len >= 7 && !strncmp(t->text.s, "[CDATA[", 7) && t->attr_blk - 1 < t->text.len) {
        size_t cut = t->attr_blk - 1;
        size_t rest_from = t->src + 2 + cut + 1;
        DomStr full = t->text;
        uint32_t s0 = dom_alloc_slot(DOM_K_COMMENT, dom_id_of(b->owner));
        g_dom.text[s0] = dom_str_dup(full.s, cut);
        dom_str_release(&t->text);
        uint32_t p0 = stream_parent(b, parent, parent_fresh, s0);
        dom_link_append(dom_id_of(p0), dom_id_of(s0));
        if (rest_from < t->end && t->end <= b->len) {
            DomTok tt;
            memset(&tt, 0, sizeof tt);
            tt.kind = TK_TEXT;
            tt.src = (uint32_t)rest_from;
            tt.end = t->end;
            tt.text = dom_str_dup(b->html + rest_from, t->end - rest_from);
            build_text(b, &tt, parent, parent_fresh);
            dom_str_release(&tt.text);
        }
        return;
    }
    /* (U+0000 in a comment: U+FFFD) */
    if (t->text.len && memchr(t->text.s, 0, t->text.len)) {
        size_t n0 = t->text.len, nul = 0, k0 = 0;
        for (size_t q = 0; q < n0; q++) if (!t->text.s[q]) nul++;
        char *o = (char *)malloc(n0 + nul * 2 + 1);
        if (o) {
            for (size_t q = 0; q < n0; q++) {
                if (t->text.s[q]) o[k0++] = t->text.s[q];
                else { o[k0++] = (char)0xEF; o[k0++] = (char)0xBF; o[k0++] = (char)0xBD; }
            }
            o[k0] = 0;
            dom_str_release(&t->text);
            t->text = dom_str_dup(o, k0);
            free(o);
        }
    }
    uint32_t s = dom_alloc_slot(DOM_K_COMMENT, dom_id_of(b->owner));
    g_dom.text[s] = t->text;
    t->text = (DomStr){ NULL, 0, 0 };
    parent = stream_parent(b, parent, parent_fresh, s);
    dom_link_append(dom_id_of(parent), dom_id_of(s));
}

/* A doctype token's name, public and system ids (HTML 13.2.5.53-68,
 * simplified: quoted ids after PUBLIC or SYSTEM), as a DocumentType under
 * the document being built: only before anything but comments. */
static void build_doctype(Builder *b, DomTok *t) {
    uint32_t parent = b->frag;
    /* (streaming into a document: before the skeleton it may already have,
     * unless it has a doctype; parsed into a fragment: only after comments) */
    for (uint32_t c = g_dom.first[parent]; c; c = g_dom.next[c])
        if (b->doc_mode ? g_dom.kind[c] == DOM_K_DOCTYPE : g_dom.kind[c] != DOM_K_COMMENT) return;
    const char *s = t->text.s ? t->text.s : "";
    size_t n = t->text.len, i = 0;
#define WS(ch) ((ch) == ' ' || (ch) == '\t' || (ch) == '\n' || (ch) == '\f' || (ch) == '\r')
    while (i < n && WS(s[i])) i++;
    size_t ns = i;
    while (i < n && !WS(s[i]) && s[i] != '>') i++;
    char name[256];
    size_t nn = i - ns < sizeof name ? i - ns : sizeof name - 1;
    for (size_t k = 0; k < nn; k++) name[k] = (char)(s[ns + k] >= 'A' && s[ns + k] <= 'Z' ? s[ns + k] + 32 : s[ns + k]);
    const char *ids[2] = { "", "" };
    size_t idn[2] = { 0, 0 };
    while (i < n && WS(s[i])) i++;
    int which = -1;
    if (n - i >= 6 && !strncasecmp(s + i, "public", 6)) which = 0;
    else if (n - i >= 6 && !strncasecmp(s + i, "system", 6)) which = 1;
    if (which >= 0) {
        i += 6;
        for (int k = which; k < 2; k++) {
            while (i < n && WS(s[i])) i++;
            if (i >= n || (s[i] != '"' && s[i] != '\'')) break;
            char q = s[i++];
            const char *e = memchr(s + i, q, n - i);
            size_t l = e ? (size_t)(e - (s + i)) : n - i;
            ids[k] = s + i;
            idn[k] = l;
            i += l + (e ? 1 : 0);
        }
    }
#undef WS
    NodeId dt = dom_create_doctype(name, nn, ids[0], idn[0], ids[1], idn[1], dom_id_of(b->owner));
    uint32_t slot = dom_slot(dt);
    parent = stream_parent(b, parent, false, slot);
    uint32_t after = g_dom.first[parent];
    while (after && g_dom.kind[after] == DOM_K_COMMENT) after = g_dom.next[after];
    dom_link_before(dom_id_of(parent), dt, dom_id_of(after));
}

static CssAtom g_a_html, g_a_head, g_a_body, g_a_frameset, g_a_frame, g_a_noframes, g_head_content[11];

static void doc_atoms(void) {
    if (g_a_html) return;
    static const char *const hc[] = { "base", "basefont", "bgsound", "link", "meta", "noframes", "script",
                                      "style", "template", "title", "noscript" };
    css_atom_reserve(16);
    g_a_html = css_atom_intern("html", 4);
    g_a_head = css_atom_intern("head", 4);
    g_a_body = css_atom_intern("body", 4);
    g_a_frameset = css_atom_intern("frameset", 8);
    g_a_frame = css_atom_intern("frame", 5);
    g_a_noframes = css_atom_intern("noframes", 8);
    for (int i = 0; i < 11; i++) g_head_content[i] = css_atom_intern(hc[i], strlen(hc[i]));
}

static bool head_content(CssAtom tag) {
    for (int i = 0; i < 11; i++) if (g_head_content[i] == tag) return true;
    return false;
}

static void push_open(Builder *b, uint32_t s) {
    if (b->sp + 1 >= MAX_NESTING) return;
    if (b->streaming) b->fresh[b->sp] = 1;
    b->stack[b->sp++] = s;
}

/* An element the markup leaves out (html, head, body), opened under parent. */
/* (documents whose head had a meta named responsive-embedded-sizing when their body started: their html elements) */
static NodeId g_resp_html[64];
static int g_resp_n;
bool dom_responsive_opt(NodeId html) {
    for (int k = 0; k < g_resp_n; k++) if (g_resp_html[k] == html) return true;
    return false;
}
static bool resp_is_meta(NodeId c) {
    static CssAtom a_meta;
    if (!a_meta) a_meta = css_atom_intern("meta", 4);
    if (g_dom.tag[dom_slot(c)] != a_meta) return false;
    const char *nm = dom_get_attr_str(c, "name");
    return nm && !strcasecmp(nm, "responsive-embedded-sizing");
}
static void resp_add(uint32_t el_html) {
    NodeId h = dom_id_of(el_html);
    if (!dom_responsive_opt(h)) { if (g_resp_n < 64) g_resp_html[g_resp_n++] = h; else g_resp_html[63] = h; }
}
static void resp_body_note(uint32_t el_head, uint32_t el_html) {
    if (!el_html) return;
    for (int w = 0; w < 2; w++) {
        uint32_t p = w == 0 ? el_head : el_html;
        if (!p) continue;
        for (NodeId c = dom_first_child(dom_id_of(p)); c; c = dom_next_sibling(c))
            if (resp_is_meta(c)) { resp_add(el_html); return; }
    }
}

static uint32_t implied(Builder *b, CssAtom tag, uint32_t parent) {
    DomTok t;
    memset(&t, 0, sizeof t);
    t.kind = TK_START;
    t.tag = tag;
    bool fresh = false;
    for (uint32_t i = 0; i < b->sp; i++) if (b->stack[i] == parent) fresh = b->streaming && b->fresh[i];
    uint32_t s = build_element(b, &t, parent, fresh);
    push_open(b, s);
    return s;
}

/* A second <html> or <body>: its attributes the element does not have yet. */
static void merge_attrs(uint32_t s, const DomTok *t) {
    if (!s) return;
    const DomAttrList *l = &g_dom.attrs[s];
    for (int i = 0; i < t->nattr; i++) {
        bool has = false;
        for (int k = 0; k < l->n && !has; k++) has = l->a[k].name == t->attrs[i].name;
        if (!has) dom_set_attr(dom_id_of(s), t->attrs[i].name, t->attrs[i].value.s ? t->attrs[i].value.s : "",
                               t->attrs[i].value.len);
        l = &g_dom.attrs[s];
    }
}

/* Before a start tag (tag) or text (tag 0) of a whole document: imply what
 * the markup left out.  True when the token was taken (a second html or
 * body, a head after one), false to build it at the new top. */
static bool doc_before(Builder *b, const DomTok *t, CssAtom tag) {
    /* (a frame, frameset, html, head or body in a template: ignored) */
    if ((tag == g_a_frameset || tag == g_a_frame || tag == g_a_html || tag == g_a_head || tag == g_a_body) && b->phase != DOC_IN_FRAMESET)
        for (uint32_t i = 0; i < (uint32_t)b->sp; i++)
            if (g_dom.tag[b->stack[i]] == g_template_atom) return true;
    /* (in a template in the head: its content, whatever it is) */
    if (b->phase == DOC_IN_HEAD)
        for (uint32_t i = 0; i < (uint32_t)b->sp; i++)
            if (g_dom.tag[b->stack[i]] == g_template_atom) return false;
    for (;;) {
        switch (b->phase) {
        case DOC_INITIAL:
            if (tag == g_a_html) return false;          /* (built below, and noted) */
            b->el_html = implied(b, g_a_html, b->frag);
            b->phase = DOC_BEFORE_HEAD;
            continue;
        case DOC_BEFORE_HEAD:
            if (tag == g_a_html) { merge_attrs(b->el_html, t); return true; }
            if (tag == g_a_head) return false;
            b->el_head = implied(b, g_a_head, b->el_html);
            if (tag && head_content(tag)) { b->phase = DOC_IN_HEAD; return false; }
            b->sp--;                                    /* (an empty head) */
            b->phase = DOC_AFTER_HEAD;
            continue;
        case DOC_IN_HEAD:
            if (tag && head_content(tag)) return false;
            if (tag == g_a_html) { merge_attrs(b->el_html, t); return true; }
            if (tag == g_a_head) return true;
            if (b->sp && b->stack[b->sp - 1] == b->el_head) b->sp--;
            b->phase = DOC_AFTER_HEAD;
            continue;
        case DOC_AFTER_HEAD:
            if (tag == g_a_body) { b->fs_bad = true; return false; }
            /* (a frameset document: its frameset where the body would be) */
            if (tag == g_a_frameset) { b->phase = DOC_IN_FRAMESET; return false; }
            if (tag == g_a_html) { merge_attrs(b->el_html, t); return true; }
            if (tag == g_a_head) return true;
            /* (head content here: into the head after all) */
            if (tag && head_content(tag) && b->el_head) { push_open(b, b->el_head); b->phase = DOC_IN_HEAD; return false; }
            resp_body_note(b->el_head, b->el_html);
            b->el_body = implied(b, g_a_body, b->el_html);
            b->phase = DOC_IN_BODY;
            continue;
        case DOC_IN_FRAMESET:
            /* (in a frameset and after it: frames, framesets and noframes; the rest ignored) */
            if (tag == g_a_html) { merge_attrs(b->el_html, t); return true; }
            return !(tag == g_a_frameset || tag == g_a_frame || tag == g_a_noframes);
        default:
            if (tag == g_a_body) { b->fs_bad = true; merge_attrs(b->el_body, t); return true; }
            /* (a frameset in the body: in place of the body while nothing has made that not ok; otherwise ignored) */
            if (tag == g_a_frameset) {
                if (b->fs_bad || !b->el_body || b->sp < 2 || b->stack[1] != b->el_body) return true;
                dom_detach(dom_id_of(b->el_body), false);
                b->sp = 1;
                b->el_body = 0;
                b->afl_n = 0;
                b->phase = DOC_IN_FRAMESET;
                return false;
            }
            if (tag == g_a_frame) return true;
            if (tag == g_a_html) { merge_attrs(b->el_html, t); return true; }
            if (tag == g_a_head) return true;
            return false;
        }
    }
}

/* Is end tag t </name> (ASCII case-insensitive, then > or white space)? */
/* ---- implied end tags and end tags across the stack (HTML 13.2.6.4.7,
 * "in body", reduced to this builder: no adoption agency, no foster
 * parenting) ---- */

typedef struct TreeAtoms {
    CssAtom li, dd, dt, p, option, optgroup, tr, td, th, button, address, div, select;
    CssAtom rb, rt, rp, rtc, ruby, h[6];
    CssAtom table, tbody, thead, tfoot, caption, colgroup, col, html, tmpl;
    CssAtom special[96], scope[12], p_closer[48];
    int nspecial, nscope, np_closer;
} TreeAtoms;
static TreeAtoms g_t;

static void tree_atoms(void) {
    if (g_t.li) return;
    static const char *const special[] = {
        "address", "applet", "area", "article", "aside", "base", "basefont", "bgsound", "blockquote", "body", "br",
        "button", "caption", "center", "col", "colgroup", "dd", "details", "dir", "div", "dl", "dt", "embed",
        "fieldset", "figcaption", "figure", "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6",
        "head", "header", "hgroup", "hr", "html", "iframe", "img", "input", "keygen", "li", "link", "listing",
        "main", "marquee", "menu", "meta", "nav", "noembed", "noframes", "noscript", "object", "ol", "p", "param",
        "plaintext", "pre", "script", "search", "section", "select", "source", "style", "summary", "table",
        "tbody", "td", "template", "textarea", "tfoot", "th", "thead", "title", "tr", "track", "ul", "wbr", "xmp" };
    static const char *const scope[] = { "applet", "caption", "html", "table", "td", "th", "marquee", "object", "template" };
    static const char *const p_closer[] = {
        "address", "article", "aside", "blockquote", "center", "details", "dialog", "dir", "div", "dl", "fieldset",
        "figcaption", "figure", "footer", "header", "hgroup", "main", "menu", "nav", "ol", "p", "search", "section",
        "summary", "ul", "h1", "h2", "h3", "h4", "h5", "h6", "pre", "listing", "form", "table", "hr", "xmp", "li", "dd", "dt",
        "plaintext" };
    css_atom_reserve(160);
#define T(x) css_atom_intern(x, strlen(x))
    g_t.dd = T("dd"); g_t.dt = T("dt"); g_t.p = T("p"); g_t.option = T("option"); g_t.optgroup = T("optgroup");
    g_t.tr = T("tr"); g_t.td = T("td"); g_t.th = T("th"); g_t.button = T("button"); g_t.address = T("address");
    g_t.div = T("div"); g_t.select = T("select");
    g_t.rb = T("rb"); g_t.rt = T("rt"); g_t.rp = T("rp"); g_t.rtc = T("rtc"); g_t.ruby = T("ruby");
    g_t.h[0] = T("h1"); g_t.h[1] = T("h2"); g_t.h[2] = T("h3"); g_t.h[3] = T("h4"); g_t.h[4] = T("h5"); g_t.h[5] = T("h6");
    g_t.table = T("table"); g_t.tbody = T("tbody"); g_t.thead = T("thead"); g_t.tfoot = T("tfoot");
    g_t.caption = T("caption"); g_t.colgroup = T("colgroup"); g_t.col = T("col"); g_t.html = T("html");
    g_t.tmpl = T("template");
    for (size_t i = 0; i < sizeof special / sizeof special[0]; i++) g_t.special[g_t.nspecial++] = T(special[i]);
    for (size_t i = 0; i < sizeof scope / sizeof scope[0]; i++) g_t.scope[g_t.nscope++] = T(scope[i]);
    for (size_t i = 0; i < sizeof p_closer / sizeof p_closer[0]; i++) g_t.p_closer[g_t.np_closer++] = T(p_closer[i]);
    g_t.li = T("li");       /* (last: it marks the set as made) */
#undef T
}

static bool atom_in(CssAtom a, const CssAtom *set, int n) {
    for (int i = 0; i < n; i++) if (set[i] == a) return true;
    return false;
}

/* A declarative shadow root (HTML 13.2.6.4.7, a template start tag with
 * shadowrootmode): when the parser closes template t, its content becomes
 * its parent's shadow root (open or closed) and the template goes. */
static void declarative_shadow(Builder *b, uint32_t t) {
    if (!b->streaming || g_dom.kind[t] != DOM_K_ELEMENT || g_dom.tag[t] != g_template_atom) return;
    const char *mode = dom_get_attr_str(dom_id_of(t), "shadowrootmode");
    if (!mode) return;
    bool closed = !strcasecmp(mode, "closed");
    if (!closed && strcasecmp(mode, "open")) return;
    uint32_t host = g_dom.parent[t];
    if (!host || g_dom.kind[host] != DOM_K_ELEMENT || g_dom.shadow[host]) return;
    NodeId frag = dom_template_content(dom_id_of(t));
    if (!frag) frag = dom_create_fragment(dom_id_of(b->owner));
    uint32_t f = dom_slot(frag);
    g_dom.content[t] = 0;
    g_dom.host[f] = 0;
    g_dom.flags[f] &= (uint16_t)~DOMF_TEMPLATE_CONTENT;
    dom_attach_shadow(dom_id_of(host), frag);
    if (closed) g_dom.flags[f] |= DOMF_SHADOW_CLOSED;
    dom_detach(dom_id_of(t), false);
}

/* <template for=name> (WICG declarative partial updates): when it closes, its content goes in place of the
 * first <?marker name=...?>, or of a <?start name=...?> up to its <?end?> (or the end of its parent), and the
 * template goes. */
static bool pi_target_is(uint32_t s, const char *target) {
    if (g_dom.kind[s] != DOM_K_PI) return false;
    const char *t = dom_name_str(g_dom.lname[s]);
    return t && !strcmp(t, target);
}
static bool pi_name_is(uint32_t s, const char *want) {
    const char *d = g_dom.text[s].s;
    size_t n = g_dom.text[s].len, wl = strlen(want);
    for (size_t i = 0; d && i + 4 <= n; i++) {
        if ((i && !c_isspace((unsigned char)d[i - 1])) || strncmp(d + i, "name", 4)) continue;
        size_t p = i + 4;
        while (p < n && c_isspace((unsigned char)d[p])) p++;
        if (p >= n || d[p] != '=') continue;
        p++;
        while (p < n && c_isspace((unsigned char)d[p])) p++;
        size_t vs, ve;
        if (p < n && (d[p] == '"' || d[p] == '\'')) {
            char q = d[p++];
            vs = p;
            while (p < n && d[p] != q) p++;
            ve = p;
        } else {
            vs = p;
            while (p < n && !c_isspace((unsigned char)d[p])) p++;
            ve = p;
        }
        return ve - vs == wl && !memcmp(d + vs, want, wl);
    }
    return false;
}
static bool anc_or_self(uint32_t a, uint32_t d);
static void template_for(Builder *b, uint32_t t) {
    if (g_dom.kind[t] != DOM_K_ELEMENT || g_dom.tag[t] != g_template_atom || g_dom.ns[t] != DOM_NS_HTML || !g_dom.parent[t]) return;
    const char *name = dom_get_attr_str(dom_id_of(t), "for");
    if (!name || !*name) return;
    uint32_t root = t;
    while (g_dom.parent[root]) root = g_dom.parent[root];
    uint32_t m = 0;
    for (uint32_t s = g_dom.first[root]; s && !m; s = dom_next_in_subtree(s, root))
        if ((pi_target_is(s, "marker") || pi_target_is(s, "start")) && pi_name_is(s, name)) m = s;
    if (!m) return;
    uint32_t parent = g_dom.parent[m], ref = g_dom.next[m];
    if (!parent) return;
    if (pi_target_is(m, "start")) {
        /* (to its end: start and end nest; without one, the rest of the parent, up to the template) */
        int depth = 0;
        uint32_t x = ref;
        while (x) {
            if (anc_or_self(x, t)) break;
            uint32_t nx = g_dom.next[x];
            bool end = false;
            if (pi_target_is(x, "start")) depth++;
            else if (pi_target_is(x, "end")) { if (depth == 0) end = true; else depth--; }
            dom_detach(dom_id_of(x), false);
            x = nx;
            if (end) break;
        }
        ref = x;
    }
    dom_detach(dom_id_of(m), false);
    dom_detach(dom_id_of(t), false);
    /* (its content: the content fragment's children when streamed, else still its own) */
    NodeId content = dom_template_content(dom_id_of(t));
    if (!content || !dom_first_child(content)) content = dom_id_of(t);
    for (NodeId c = dom_first_child(content); c; c = dom_first_child(content)) {
        dom_detach(c, false);
        uint32_t cs = dom_slot(c);
        uint32_t pp = stream_parent(b, parent, false, cs);
        if (ref && g_dom.parent[ref] == pp) dom_link_before(dom_id_of(pp), c, dom_id_of(ref));
        else dom_link_append(dom_id_of(pp), c);
    }
}

/* Pop the open elements down to (and including) stack index k. */
static bool is_marker_tag(CssAtom a);
static void afl_clear_to_marker(Builder *b);
static bool is_object_marker(CssAtom a) {
    const char *n = css_atom_str(a);
    return n && (!strcmp(n, "applet") || !strcmp(n, "marquee") || !strcmp(n, "object"));
}
static void pop_to(Builder *b, uint32_t k) {
    for (uint32_t i = b->sp; i-- > k; ) {
        if (b->doc_mode && b->stack[i] == b->el_head && b->phase == DOC_IN_HEAD) b->phase = DOC_AFTER_HEAD;
        /* (a cell, caption or template closing: the list cleared to its marker; applet, marquee and object only by their end tags) */
        if (b->afl_n && g_dom.ns[b->stack[i]] == DOM_NS_HTML && is_marker_tag(g_dom.tag[b->stack[i]]) && !is_object_marker(g_dom.tag[b->stack[i]])) afl_clear_to_marker(b);
        if (g_dom.tag[b->stack[i]] == g_template_atom) { declarative_shadow(b, b->stack[i]); template_for(b, b->stack[i]); }
    }
    b->sp = k;
}

/* The index of the topmost open tag element "in scope" (no scope boundary
 * above it; button scope adds <button>), or -1. */
/* (an element a scope ends at: HTML's applet, caption, html, table, td, th, marquee, object, template;
 * MathML's mi, mo, mn, ms, mtext, annotation-xml; SVG's foreignObject, desc, title) */
static bool scope_stop(uint32_t s) {
    if (g_dom.ns[s] == DOM_NS_HTML) return atom_in(g_dom.tag[s], g_t.scope, g_t.nscope);
    const char *n = css_atom_str(g_dom.tag[s]);
    if (!n) return false;
    if (g_dom.ns[s] == DOM_NS_MATHML)
        return !strcmp(n, "mi") || !strcmp(n, "mo") || !strcmp(n, "mn") || !strcmp(n, "ms") || !strcmp(n, "mtext") || !strcmp(n, "annotation-xml");
    if (g_dom.ns[s] == DOM_NS_SVG) return !strcasecmp(n, "foreignObject") || !strcmp(n, "desc") || !strcmp(n, "title");
    return false;
}
static int in_scope(const Builder *b, CssAtom tag, bool button) {
    for (uint32_t i = b->sp; i-- > 0; ) {
        CssAtom a = g_dom.tag[b->stack[i]];
        if (a == tag && g_dom.ns[b->stack[i]] == DOM_NS_HTML) return (int)i;
        if (scope_stop(b->stack[i]) || (button && a == g_t.button && g_dom.ns[b->stack[i]] == DOM_NS_HTML)) return -1;
    }
    return -1;
}

static bool is_table_never(CssAtom a) { (void)a; return false; }
static bool is_section(CssAtom a) { return a == g_t.tbody || a == g_t.thead || a == g_t.tfoot; }
static bool is_table_part(CssAtom a) {
    return a == g_t.tr || a == g_t.td || a == g_t.th || is_section(a) || a == g_t.caption || a == g_t.colgroup;
}

/* The lowest of what a table's start or end tag closes within the current
 * table (table scope), keeping what stop says stays: its stack index, or -1. */
static int table_part_below(const Builder *b, bool (*closes)(CssAtom), bool (*stop)(CssAtom)) {
    int k = -1;
    for (uint32_t i = b->sp; i-- > 0; ) {
        CssAtom a = g_dom.tag[b->stack[i]];
        if (a == g_t.table || a == g_t.tmpl || a == g_t.html || stop(a)) break;
        if (closes(a)) k = (int)i;
    }
    return k;
}
static bool is_cell(CssAtom a) { return a == g_t.td || a == g_t.th; }
static bool is_cell_or_head(CssAtom a) { return is_cell(a) || a == g_t.caption || a == g_t.colgroup; }
static bool is_row_or_cell(CssAtom a) { return a == g_t.tr || is_cell_or_head(a); }
static bool is_row(CssAtom a) { return a == g_t.tr; }

/* (the current node a table, a row group or a row: what isn't a table's goes before the table, HTML 13.2.6.1) */
static bool table_context(uint32_t s) {
    if (!s || g_dom.kind[s] != DOM_K_ELEMENT || g_dom.ns[s] != DOM_NS_HTML) return false;
    CssAtom a = g_dom.tag[s];
    return a == g_t.table || is_section(a) || a == g_t.tr;
}
static bool table_allowed(const DomTok *t) {
    CssAtom a = t->tag;
    if (a == g_t.table || is_table_part(a) || a == g_t.td || a == g_t.th || a == g_t.col || a == g_t.tmpl) return true;
    const char *n = css_atom_str(a);
    if (!n) return false;
    if (!strcmp(n, "style") || !strcmp(n, "script") || !strcmp(n, "form")) return true;
    if (!strcmp(n, "input")) {
        for (uint16_t i = 0; i < t->nattr; i++) {
            const char *an = css_atom_str(t->attrs[i].name);
            if (an && !strcmp(an, "type") && t->attrs[i].value.s && !strcasecmp(t->attrs[i].value.s, "hidden")) return true;
        }
    }
    return false;
}
/* (the foster parent: the last table's parent, before it; or the element above it) */
static uint32_t foster_parent(Builder *b, uint32_t *ref) {
    *ref = 0;
    for (uint32_t i = b->sp; i-- > 0; ) {
        uint32_t s = b->stack[i];
        if (g_dom.tag[s] == g_t.tmpl) return s;
        if (g_dom.tag[s] == g_t.table) {
            if (g_dom.parent[s]) { *ref = s; return g_dom.parent[s]; }
            return i ? b->stack[i - 1] : 0;
        }
    }
    return 0;
}


/* ---- the list of active formatting elements, its reconstruction and the adoption agency (HTML 13.2.4.3, 13.2.6.4.7) ---- */
#define AFL_MARKER 0xFFFFFFFFu
static CssAtom g_fmt[14];
static int g_nfmt;
static void fmt_atoms(void) {
    if (g_nfmt) return;
    static const char *const k[] = { "a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u" };
    css_atom_reserve(14);
    for (int i = 0; i < 14; i++) g_fmt[i] = css_atom_intern(k[i], strlen(k[i]));
    g_nfmt = 14;
}
static bool is_formatting(CssAtom a) { fmt_atoms(); return atom_in(a, g_fmt, g_nfmt); }
static bool is_marker_tag(CssAtom a) {
    static CssAtom applet, marquee, object;
    if (!applet) { css_atom_reserve(3); applet = css_atom_intern("applet", 6); marquee = css_atom_intern("marquee", 7); object = css_atom_intern("object", 6); }
    return a == applet || a == marquee || a == object || a == g_t.td || a == g_t.th || a == g_t.caption || a == g_t.tmpl;
}
static void afl_push(Builder *b, uint32_t s) {
    if (b->afl_n >= AFL_MAX) { memmove(b->afl, b->afl + 1, (AFL_MAX - 1) * sizeof b->afl[0]); b->afl_n--; }
    b->afl[b->afl_n++] = s;
}
static void afl_remove_at(Builder *b, uint32_t i) {
    memmove(b->afl + i, b->afl + i + 1, (b->afl_n - i - 1) * sizeof b->afl[0]);
    b->afl_n--;
}
static int afl_index(const Builder *b, uint32_t s) {
    for (uint32_t i = b->afl_n; i-- > 0; ) if (b->afl[i] == s) return (int)i;
    return -1;
}
static void afl_clear_to_marker(Builder *b) {
    while (b->afl_n) { uint32_t e = b->afl[--b->afl_n]; if (e == AFL_MARKER) break; }
}
static int stack_index(const Builder *b, uint32_t s) {
    for (uint32_t i = b->sp; i-- > 0; ) if (b->stack[i] == s) return (int)i;
    return -1;
}
/* (two elements alike: tag, namespace and attributes, for the Noah's Ark clause) */
static bool same_element(uint32_t a, uint32_t c) {
    if (g_dom.tag[a] != g_dom.tag[c] || g_dom.ns[a] != g_dom.ns[c]) return false;
    DomAttrList *x = &g_dom.attrs[a], *y = &g_dom.attrs[c];
    if (x->n != y->n) return false;
    for (int i = 0; i < x->n; i++) {
        bool hit = false;
        for (int j = 0; j < y->n && !hit; j++)
            hit = x->a[i].name == y->a[j].name && x->a[i].ns == y->a[j].ns && x->a[i].value.len == y->a[j].value.len &&
                  (!x->a[i].value.len || !memcmp(x->a[i].value.s, y->a[j].value.s, x->a[i].value.len));
        if (!hit) return false;
    }
    return true;
}
static void afl_push_formatting(Builder *b, uint32_t s) {
    int same = 0, first = -1;
    for (uint32_t i = b->afl_n; i-- > 0; ) {
        uint32_t e = b->afl[i];
        if (e == AFL_MARKER) break;
        if (same_element(e, s)) { same++; first = (int)i; }
    }
    if (same >= 3 && first >= 0) afl_remove_at(b, (uint32_t)first);
    afl_push(b, s);
}
/* (an element made again for the one given: tag, namespace and attributes) */
static uint32_t clone_for(Builder *b, uint32_t src) {
    uint32_t d = dom_alloc_slot(DOM_K_ELEMENT, dom_id_of(b->owner));
    g_dom.tag[d] = g_dom.tag[src];
    g_dom.ns[d] = g_dom.ns[src];
    g_dom.lname[d] = g_dom.lname[src];
    g_dom.prefix[d] = g_dom.prefix[src];
    g_dom.flags[d] = (uint16_t)(g_dom.flags[src] & (DOMF_CE_CANDIDATE | DOMF_VOID));
    DomAttrList *l = &g_dom.attrs[src];
    for (int i = 0; i < l->n; i++) {
        DomStr v = l->a[i].value;
        const char *q = dom_attr_qualified_name(&l->a[i]);
        dom_set_attr_at(dom_id_of(d), -1, l->a[i].ns, q, strlen(q), v.s ? v.s : "", v.len);
    }
    return d;
}
static void stack_insert(Builder *b, uint32_t at, uint32_t s) {
    if (b->sp + 1 >= MAX_NESTING) return;
    memmove(b->stack + at + 1, b->stack + at, (b->sp - at) * sizeof b->stack[0]);
    if (b->streaming) memmove(b->fresh + at + 1, b->fresh + at, (b->sp - at));
    b->stack[at] = s;
    if (b->streaming) b->fresh[at] = 1;
    b->sp++;
}
static void stack_remove(Builder *b, uint32_t at) {
    memmove(b->stack + at, b->stack + at + 1, (b->sp - at - 1) * sizeof b->stack[0]);
    if (b->streaming) memmove(b->fresh + at, b->fresh + at + 1, (b->sp - at - 1));
    b->sp--;
}
/* (a an inclusive ancestor of d, across shadow hosts and template contents: a move of a under d would make a cycle) */
static bool anc_or_self(uint32_t a, uint32_t d) {
    for (uint32_t x = d, n = 0; x && n < 100000; n++) {
        if (x == a) return true;
        uint32_t p = g_dom.parent[x] ? g_dom.parent[x] : g_dom.host[x];
        if (!p && g_dom.kind[x] == DOM_K_FRAGMENT) break;
        x = p;
    }
    return false;
}
/* (child moved under parent, appended; nothing when that would make a cycle) */
static bool move_append(Builder *b, uint32_t parent, uint32_t child) {
    if (!parent || !child || anc_or_self(child, parent)) return false;
    if (g_dom.parent[child]) dom_detach(dom_id_of(child), false);
    uint32_t pp = stream_parent(b, parent, false, child);
    if (anc_or_self(child, pp)) return false;
    dom_link_append(dom_id_of(pp), dom_id_of(child));
    return true;
}

/* (append node s under parent: where a table would foster it, before the table) */
static void place_node(Builder *b, uint32_t parent, uint32_t s) {
    if (!parent || anc_or_self(s, parent)) return;
    if (g_dom.parent[s]) dom_detach(dom_id_of(s), false);
    if (table_context(parent)) {
        uint32_t ref = 0, fp = foster_parent(b, &ref);
        if (fp) {
            fp = stream_parent(b, fp, false, s);
            if (ref && g_dom.parent[ref] == fp) dom_link_before(dom_id_of(fp), dom_id_of(s), dom_id_of(ref));
            else dom_link_append(dom_id_of(fp), dom_id_of(s));
            return;
        }
    }
    parent = stream_parent(b, parent, false, s);
    dom_link_append(dom_id_of(parent), dom_id_of(s));
}
/* Reconstruct the active formatting elements: those closed before their time open again at the current node. */
static void afl_reconstruct(Builder *b) {
    if (!b->afl_n || (!b->sp && !b->frag)) return;
    uint32_t last = b->afl[b->afl_n - 1];
    if (last == AFL_MARKER || stack_index(b, last) >= 0) return;
    uint32_t i = b->afl_n - 1;
    while (i > 0) {
        uint32_t e = b->afl[i - 1];
        if (e == AFL_MARKER || stack_index(b, e) >= 0) break;
        i--;
    }
    for (; i < b->afl_n; i++) {
        uint32_t src = b->afl[i];
        uint32_t top = b->sp ? b->stack[b->sp - 1] : b->frag;
        uint32_t n = clone_for(b, src);
        place_node(b, top, n);
        if (b->sp + 1 < MAX_NESTING) {
            if (b->streaming) b->fresh[b->sp] = 1;
            b->stack[b->sp++] = n;
        }
        b->afl[i] = n;
    }
}
static bool is_special_slot(uint32_t s) {
    if (g_dom.ns[s] != DOM_NS_HTML) return foreign_current(s) == false && (g_dom.ns[s] == DOM_NS_SVG || g_dom.ns[s] == DOM_NS_MATHML);
    return atom_in(g_dom.tag[s], g_t.special, g_t.nspecial);
}
/* The adoption agency algorithm for an end tag (or an a/nobr start tag) of `tag`: true when it handled it,
 * false when it is to be the "any other end tag" steps. */
static bool adoption_agency(Builder *b, CssAtom tag) {
    if (b->sp) {
        uint32_t cur = b->stack[b->sp - 1];
        if (g_dom.ns[cur] == DOM_NS_HTML && g_dom.tag[cur] == tag && afl_index(b, cur) < 0) { pop_to(b, b->sp - 1); return true; }
    }
    for (int outer = 0; outer < 8; outer++) {
        int fi = -1;
        for (uint32_t i = b->afl_n; i-- > 0; ) {
            uint32_t e = b->afl[i];
            if (e == AFL_MARKER) break;
            if (g_dom.tag[e] == tag) { fi = (int)i; break; }
        }
        if (fi < 0) return outer > 0;
        uint32_t fe = b->afl[fi];
        int si = stack_index(b, fe);
        if (si < 0) { afl_remove_at(b, (uint32_t)fi); return true; }
        /* (in the stack but not in scope: ignored) */
        {
            bool scoped = false;
            for (uint32_t i = b->sp; i-- > 0; ) {
                if (b->stack[i] == fe) { scoped = true; break; }
                if (scope_stop(b->stack[i])) break;
            }
            if (!scoped) return true;
        }
        int fbi = -1;
        for (uint32_t i = (uint32_t)si + 1; i < b->sp; i++) if (is_special_slot(b->stack[i])) { fbi = (int)i; break; }
        if (fbi < 0) {
            pop_to(b, (uint32_t)si);
            int k = afl_index(b, fe);
            if (k >= 0) afl_remove_at(b, (uint32_t)k);
            return true;
        }
        uint32_t fb = b->stack[fbi];
        uint32_t common = si > 0 ? b->stack[si - 1] : b->frag;
        int bookmark = fi;
        uint32_t node = fb, last_node = fb;
        int ni = fbi;
        for (int inner = 1; ; inner++) {
            ni--;
            node = b->stack[ni];
            if (node == fe) break;
            int ai = afl_index(b, node);
            if (inner > 3 && ai >= 0) { afl_remove_at(b, (uint32_t)ai); if (ai < bookmark) bookmark--; ai = -1; }
            if (ai < 0) { stack_remove(b, (uint32_t)ni); continue; }
            uint32_t nn = clone_for(b, node);
            b->afl[ai] = nn;
            b->stack[ni] = nn;
            if (b->streaming) b->fresh[ni] = 1;
            node = nn;
            if (last_node == fb) bookmark = ai + 1;
            move_append(b, node, last_node);
            last_node = node;
        }
        place_node(b, common, last_node);
        uint32_t ne = clone_for(b, fe);
        while (g_dom.first[fb]) {
            uint32_t c = g_dom.first[fb];
            dom_detach(dom_id_of(c), false);
            dom_link_append(dom_id_of(ne), dom_id_of(c));
        }
        move_append(b, fb, ne);
        int k = afl_index(b, fe);
        if (k >= 0) { afl_remove_at(b, (uint32_t)k); if (k < bookmark) bookmark--; }
        if (bookmark > (int)b->afl_n) bookmark = (int)b->afl_n;
        if (b->afl_n < AFL_MAX) {
            memmove(b->afl + bookmark + 1, b->afl + bookmark, (b->afl_n - (uint32_t)bookmark) * sizeof b->afl[0]);
            b->afl[bookmark] = ne;
            b->afl_n++;
        }
        int fsi = stack_index(b, fe);
        if (fsi >= 0) stack_remove(b, (uint32_t)fsi);
        int fbi2 = stack_index(b, fb);
        if (fbi2 >= 0) stack_insert(b, (uint32_t)fbi2 + 1, ne);
    }
    return true;
}

/* (start tags that don't reconstruct the formatting elements first: blocks, table parts, the document's parts) */
static bool no_reconstruct(CssAtom tag) {
    if (atom_in(tag, g_t.p_closer, g_t.np_closer)) {
        const char *n = css_atom_str(tag);
        return !(n && !strcmp(n, "xmp"));
    }
    if (tag == g_t.table || is_table_part(tag) || tag == g_t.td || tag == g_t.th || tag == g_t.col || tag == g_t.html || tag == g_t.tmpl ||
        tag == g_t.rb || tag == g_t.rt || tag == g_t.rp || tag == g_t.rtc) return true;
    const char *n = css_atom_str(tag);
    static const char *const k[] = { "head", "body", "frameset", "frame", "noframes", "base", "basefont", "bgsound", "link", "meta",
        "script", "style", "title", "noscript", "iframe", "noembed", "textarea", "hr" };
    for (size_t i = 0; n && i < sizeof k / sizeof k[0]; i++) if (!strcmp(n, k[i])) return true;
    return false;
}

/* (the document in quirks mode, as far as its doctype goes: none, another name than html, or the public id "HTML") */
static bool builder_quirks(const Builder *b) {
    uint32_t d = b->doc_mode ? b->frag : b->owner;
    if (!d || g_dom.kind[d] != DOM_K_DOCUMENT) return false;
    for (uint32_t c = g_dom.first[d]; c; c = g_dom.next[c]) {
        if (g_dom.kind[c] != DOM_K_DOCTYPE) continue;
        const char *nm = dom_name_str(g_dom.lname[c]), *pub = dom_name_str(g_dom.prefix[c]);
        if (!nm || strcasecmp(nm, "html")) return true;
        if (pub && !strcasecmp(pub, "html")) return true;
        return false;
    }
    return b->doc_mode;
}

/* A start tag closes what it implies the end of. */
static void close_implied(Builder *b, CssAtom tag) {
    if (!b->sp) return;
    CssAtom top = g_dom.tag[b->stack[b->sp - 1]];
    /* (HTML 13.2.6.4.13-15: a table's parts end those before them, and
     * whatever is still open inside them) */
    if (tag == g_t.td || tag == g_t.th || tag == g_t.tr || is_section(tag) || tag == g_t.caption || tag == g_t.colgroup) {
        /* (a caption or colgroup ends at any of them) */
        int k = tag == g_t.td || tag == g_t.th ? table_part_below(b, is_cell_or_head, is_row)
              : tag == g_t.tr ? table_part_below(b, is_row_or_cell, is_section)
              : table_part_below(b, is_table_part, is_table_never);
        if (k >= 0) pop_to(b, (uint32_t)k);
        /* (the parts a table's row or cell implies) */
        top = b->sp ? g_dom.tag[b->stack[b->sp - 1]] : 0;
        if (b->sp && top == g_t.table && (tag == g_t.tr || tag == g_t.td || tag == g_t.th)) {
            implied(b, g_t.tbody, b->stack[b->sp - 1]);
            top = g_t.tbody;
        }
        if (b->sp && is_section(top) && (tag == g_t.td || tag == g_t.th)) implied(b, g_t.tr, b->stack[b->sp - 1]);
        return;
    }
    if (tag == g_t.col && top == g_t.table) { implied(b, g_t.colgroup, b->stack[b->sp - 1]); return; }
    if (tag == g_t.option) { if (top == g_t.option) pop_to(b, b->sp - 1); return; }
    /* (ruby's parts, with a ruby in scope: the implied end tags generated, an rt or rp staying in its rtc) */
    if (tag == g_t.rb || tag == g_t.rtc || tag == g_t.rt || tag == g_t.rp) {
        if (in_scope(b, g_t.ruby, false) < 0) return;
        bool in = tag == g_t.rt || tag == g_t.rp;
        while (b->sp) {
            CssAtom a = g_dom.tag[b->stack[b->sp - 1]];
            if (!(a == g_t.rb || a == g_t.rt || a == g_t.rp || (a == g_t.rtc && !in) || a == g_t.p || a == g_t.li ||
                  a == g_t.dd || a == g_t.dt || a == g_t.option || a == g_t.optgroup)) break;
            pop_to(b, b->sp - 1);
        }
        return;
    }
    if (tag == g_t.optgroup) {
        if (top == g_t.option) { pop_to(b, b->sp - 1); top = b->sp ? g_dom.tag[b->stack[b->sp - 1]] : 0; }
        if (top == g_t.optgroup) pop_to(b, b->sp - 1);
        return;
    }
    if (tag == g_t.li || tag == g_t.dd || tag == g_t.dt) {
        /* the nearest li (dd/dt) above any special element but address,
         * div and p */
        for (uint32_t i = b->sp; i-- > 0; ) {
            CssAtom a = g_dom.tag[b->stack[i]];
            if (tag == g_t.li ? a == g_t.li : (a == g_t.dd || a == g_t.dt)) { pop_to(b, i); break; }
            if (atom_in(a, g_t.special, g_t.nspecial) && a != g_t.address && a != g_t.div && a != g_t.p) break;
        }
    }
    if (atom_in(tag, g_t.p_closer, g_t.np_closer) && !(tag == g_t.table && builder_quirks(b))) {
        int k = in_scope(b, g_t.p, true);
        if (k >= 0) pop_to(b, (uint32_t)k);
        /* (a heading in a heading: the open one ends) */
        if (atom_in(tag, g_t.h, 6) && b->sp && atom_in(g_dom.tag[b->stack[b->sp - 1]], g_t.h, 6)) pop_to(b, b->sp - 1);
    }
}

/* An end tag: the matching open element, popped with those above it; or
 * ignored (a special one not in scope, or another blocked by a special
 * element above it). */
static void close_end_tag(Builder *b, const DomTok *t) {
    char name[256];             /* (custom element names run long) */
    size_t n = 0;
    bool whole = false;
    for (size_t i = t->src + 2; i < t->end && i < b->len; i++) {
        char ch = b->html[i];
        if (ch == '>' || ch == '/' || c_isspace((unsigned char)ch)) { whole = true; break; }
        if (n + 1 >= sizeof name) break;
        name[n++] = (char)tolower((unsigned char)ch);
    }
    if (!whole && n + 1 >= sizeof name) return;     /* (a name longer still: ignored) */
    CssAtom tag = n ? css_atom_find(name, n) : 0;
    if (!tag) return;
    /* (table scope: an open cell doesn't stop a row's or the table's end tag,
     * and what is left open in it ends with it) */
    if (tag == g_t.table || is_table_part(tag)) {
        for (uint32_t i = b->sp; i-- > 0; ) {
            CssAtom a = g_dom.tag[b->stack[i]];
            if (a == tag) { pop_to(b, i); return; }
            if (a == g_t.table || a == g_t.tmpl || a == g_t.html) return;
        }
        return;
    }
    /* (</dialog>: as the special elements' end tags, though it is not one) */
    {
        static CssAtom dialog_atom;
        if (!dialog_atom) { css_atom_reserve(1); dialog_atom = css_atom_intern("dialog", 6); }
        if (tag == dialog_atom) { int k = in_scope(b, tag, false); if (k >= 0) pop_to(b, (uint32_t)k); return; }
    }
    /* (a heading's end tag ends whichever heading is open in scope) */
    if (atom_in(tag, g_t.h, 6)) {
        for (uint32_t i = b->sp; i-- > 0; ) {
            CssAtom a = g_dom.tag[b->stack[i]];
            if (atom_in(a, g_t.h, 6)) { pop_to(b, i); return; }
            if (scope_stop(b->stack[i])) return;
        }
        return;
    }
    if (atom_in(tag, g_t.special, g_t.nspecial)) {
        int k = in_scope(b, tag, tag == g_t.p);
        if (tag == g_t.select) {        /* (select scope: anything but option/optgroup bounds it) */
            k = -1;
            for (uint32_t i = b->sp; i-- > 0; ) {
                CssAtom a = g_dom.tag[b->stack[i]];
                if (a == g_t.select) { k = (int)i; break; }
                if (a != g_t.option && a != g_t.optgroup) break;
            }
        }
        if (k >= 0) pop_to(b, (uint32_t)k);
        /* (a </p> with no p open makes an empty one, HTML 13.2.6.4.7) */
        else if (tag == g_t.p && (b->sp || b->frag)) {   /* (nothing open in a fragment: the fragment its parent) */
            implied(b, g_t.p, b->sp ? b->stack[b->sp - 1] : b->frag);
            b->sp--;
        }
        return;
    }
    for (uint32_t i = b->sp; i-- > 0; ) {
        CssAtom a = g_dom.tag[b->stack[i]];
        if (a == tag) { pop_to(b, i); return; }
        if (atom_in(a, g_t.special, g_t.nspecial)) return;
    }
}

static bool end_tag_is(const Builder *b, const DomTok *t, const char *name) {
    size_t n = strlen(name), at = t->src + 2;
    if (at + n > b->len || strncasecmp(b->html + at, name, n) != 0) return false;
    if (at + n == b->len) return true;
    char c = b->html[at + n];
    return c == '>' || c_isspace((unsigned char)c) || c == '/';
}

/* Whether text is white space only (it does not open a body). */
static bool all_space(const DomStr *d) {
    for (uint32_t i = 0; i < d->len; i++) if (!c_isspace((unsigned char)d->s[i])) return false;
    return true;
}

/* Feed tokens [*i, n) of `c`.  Returns false when the builder needs the rest
 * of the input re-tokenized from *detour (a top-level end tag that the legacy
 * parser resumed from inside). */
static bool build_tokens(Builder *b, TokChunk *c, size_t *pi, size_t *detour) {
    tree_atoms();
    for (size_t i = *pi; i < c->n; i++) {
        DomTok *t = &c->toks[i];
        if (b->doc_mode) {
            if (t->kind == TK_START && doc_before(b, t, t->tag)) continue;
            if (t->kind == TK_TEXT && b->phase != DOC_IN_BODY) {
                /* (white space before the head goes; in the head it is the
                 * head's (or its title's), after it the html element's) */
                bool ws = all_space(&t->text);
                if (ws && (b->phase == DOC_INITIAL || b->phase == DOC_BEFORE_HEAD)) continue;
                if (b->phase == DOC_IN_FRAMESET && !ws) continue;   /* (a frameset's text: only its white space) */
                if (!ws) {
                    /* (its leading white space: dropped before the head, the head's or the html element's after; the rest opens the body) */
                    size_t k = 0;
                    while (k < t->text.len && c_isspace((unsigned char)t->text.s[k])) k++;
                    if (k > 0) {
                        if ((b->phase == DOC_IN_HEAD || b->phase == DOC_AFTER_HEAD) && b->sp) {
                            DomTok tt;
                            memset(&tt, 0, sizeof tt);
                            tt.kind = TK_TEXT;
                            tt.src = t->src;
                            tt.end = t->src;
                            tt.text = dom_str_dup(t->text.s, k);
                            build_text(b, &tt, b->stack[b->sp - 1], b->streaming && b->fresh[b->sp - 1]);
                            dom_str_release(&tt.text);
                        }
                        DomStr rest = dom_str_dup(t->text.s + k, t->text.len - k);
                        dom_str_release(&t->text);
                        t->text = rest;
                    }
                    doc_before(b, t, 0);
                }
            }
            /* (the body stays open; what comes after it: comments go to the html element, or the document) */
            if (t->kind == TK_END && (end_tag_is(b, t, "body") || end_tag_is(b, t, "html"))) {
                bool tmpl_open = false;
                for (uint32_t k = 0; k < b->sp && !tmpl_open; k++) tmpl_open = g_dom.tag[b->stack[k]] == g_template_atom;
                if (!tmpl_open && b->phase < DOC_IN_BODY) doc_before(b, t, 0);
                if (b->phase == DOC_IN_FRAMESET && end_tag_is(b, t, "html") && b->sp <= 1) { b->after_body = 2; continue; }
                if (b->el_body && b->phase == DOC_IN_BODY) b->after_body = end_tag_is(b, t, "html") ? 2 : (b->after_body ? b->after_body : 1);
                continue;
            }
            if (b->after_body) {
                if (t->kind == TK_COMMENT) {
                    if (b->after_body == 1 && b->el_html) build_comment(b, t, b->el_html, false);
                    else build_comment(b, t, b->frag, false);
                    continue;
                }
                if (!(t->kind == TK_TEXT && all_space(&t->text)) && t->kind != TK_DOCTYPE) b->after_body = 0;
            }
            /* (an end tag before the body: ignored, but for head's, body's, html's, br's and template's) */
            if (t->kind == TK_END && b->phase != DOC_IN_BODY && !end_tag_is(b, t, "head") && !end_tag_is(b, t, "br") &&
                !end_tag_is(b, t, "template") && !(b->phase == DOC_IN_FRAMESET && (end_tag_is(b, t, "frameset") || end_tag_is(b, t, "noframes")))) {
                bool in_tmpl = false;
                for (uint32_t i = 0; i < b->sp && !in_tmpl; i++) in_tmpl = g_dom.tag[b->stack[i]] == g_template_atom;
                if (!in_tmpl) continue;
            }
        }
        /* (what makes the frameset-ok flag "not ok", in the body) */
        if (b->doc_mode && b->phase == DOC_IN_BODY && !b->fs_bad) {
            if (t->kind == TK_TEXT && !all_space(&t->text)) {
                /* (U+0000: ignored in the body, so not something that spoils it either) */
                bool other = false;
                for (size_t q = 0; q < t->text.len && !other; q++) {
                    char ch = t->text.s[q];
                    if (ch && ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f') other = true;
                }
                if (other) b->fs_bad = true;
            }
            else if (t->kind == TK_START) {
                static const char *const k[] = { "pre", "listing", "li", "dd", "dt", "button", "applet", "marquee", "object", "table",
                    "area", "br", "embed", "img", "keygen", "wbr", "hr", "textarea", "xmp", "iframe", "select", "template", "image", NULL };
                const char *tn = css_atom_str(t->tag);
                for (int k2 = 0; tn && k[k2] && !b->fs_bad; k2++) if (!strcmp(tn, k[k2])) b->fs_bad = true;
                if (tn && !strcmp(tn, "input")) {
                    bool hidden = false;
                    for (uint16_t a = 0; a < t->nattr; a++) {
                        const char *an = css_atom_str(t->attrs[a].name);
                        if (an && !strcmp(an, "type") && t->attrs[a].value.s && t->attrs[a].value.len == 6 && !strncasecmp(t->attrs[a].value.s, "hidden", 6)) hidden = true;
                    }
                    if (!hidden) b->fs_bad = true;
                }
            }
        }
        uint32_t top = b->sp ? b->stack[b->sp - 1] : 0;
        bool top_fresh = top && b->streaming && b->fresh[b->sp - 1];
        /* ("</br>" in HTML content: a <br> start tag, its attributes dropped, HTML 13.2.6.4.7) */
        if (t->kind == TK_END && b->html_rules && end_tag_is(b, t, "br")) {   /* (in foreign content too: a breakout start tag then) */
            static CssAtom br_atom;
            if (!br_atom) { css_atom_reserve(1); br_atom = css_atom_intern("br", 2); }
            t->kind = TK_START;
            t->tag = br_atom;
            t->nattr = 0;
            t->attrs = NULL;
            t->flags = 0;
        }
        switch (t->kind) {
        case TK_START: {
            /* ("image" in HTML content: "img") */
            if (b->html_rules && (!top || !foreign_current(top))) {
                static CssAtom a_image, a_img;
                if (!a_image) { css_atom_reserve(2); a_image = css_atom_intern("image", 5); a_img = css_atom_intern("img", 3); }
                if (t->tag == a_image) t->tag = a_img;
            }
            /* (an HTML start tag in foreign content: out of it, to the nearest HTML element or integration point) */
            if (top && b->html_rules && foreign_current(top) && foreign_breakout_tag(t)) {
                while (b->sp && foreign_current(b->stack[b->sp - 1])) pop_to(b, b->sp - 1);
                top = b->sp ? b->stack[b->sp - 1] : 0;
                top_fresh = top && b->streaming && b->fresh[b->sp - 1];
            }
            /* (in an open select: a select ends it and goes; an input, textarea or keygen ends it first; an hr ends an option or optgroup) */
            if (top && b->html_rules && t->kind == TK_START) {
                const char *tn_ = css_atom_str(t->tag);
                int sk = tn_ ? in_scope(b, g_t.select, false) : -1;
                if (sk >= 0) {
                    if (!strcmp(tn_, "select")) { pop_to(b, (uint32_t)sk); break; }
                    if (!strcmp(tn_, "input") || !strcmp(tn_, "textarea") || !strcmp(tn_, "keygen")) {
                        pop_to(b, (uint32_t)sk);
                        top = b->sp ? b->stack[b->sp - 1] : 0;
                        top_fresh = top && b->streaming && b->fresh[b->sp - 1];
                    } else if (!strcmp(tn_, "hr")) {
                        if (b->sp && g_dom.tag[b->stack[b->sp - 1]] == g_t.option) pop_to(b, b->sp - 1);
                        if (b->sp && g_dom.tag[b->stack[b->sp - 1]] == g_t.optgroup) pop_to(b, b->sp - 1);
                        top = b->sp ? b->stack[b->sp - 1] : 0;
                        top_fresh = top && b->streaming && b->fresh[b->sp - 1];
                    }
                }
            }
            /* (a form inside an open form, outside a template: ignored) */
            if (b->html_rules && b->form_ptr && t->kind == TK_START && css_atom_str(t->tag) && !strcmp(css_atom_str(t->tag), "form")) {
                bool tmpl = false;
                for (uint32_t k = 0; k < b->sp && !tmpl; k++) tmpl = g_dom.tag[b->stack[k]] == g_t.tmpl;
                if (!tmpl) break;
            }
            /* (a table's part with a select open: in a table, the select ends; not in one, the tag is ignored) */
            if (top && b->html_rules && (t->tag == g_t.table || is_table_part(t->tag) || t->tag == g_t.td || t->tag == g_t.th)) {
                int si = -1, ti = -1;
                for (uint32_t k = b->sp; k-- > 0 && si < 0; ) {
                    CssAtom a = g_dom.tag[b->stack[k]];
                    if (a == g_t.select) si = (int)k;
                    else if (a == g_t.table || a == g_t.tmpl) break;
                }
                if (si >= 0) {
                    for (int k = si; k-- > 0 && ti < 0; ) if (g_dom.tag[b->stack[k]] == g_t.table) ti = k;
                    if (ti < 0) break;
                    pop_to(b, (uint32_t)si);
                    top = b->sp ? b->stack[b->sp - 1] : 0;
                    top_fresh = top && b->streaming && b->fresh[b->sp - 1];
                }
            }
            /* (a fragment in a cell, row, table section or caption, with none of those open: the table parts its mode would
             * close it for are ignored, the fragment case of "in cell", "in row", "in table body" and "in caption") */
            if (b->html_rules && !b->doc_mode && t->kind == TK_START && g_frag_context && g_dom.ns[g_frag_context] == DOM_NS_HTML && (!top || !foreign_current(top))) {
                CssAtom cx = g_dom.tag[g_frag_context], a = t->tag;
                int want = cx == g_t.td || cx == g_t.th ? 1 : cx == g_t.tr ? 2 : is_section(cx) ? 3 : cx == g_t.caption ? 4 : 0;
                if (want) {
                    bool part = a == g_t.caption || a == g_t.col || a == g_t.colgroup || is_section(a) || a == g_t.tr ||
                                (want != 3 && want != 2 ? (a == g_t.td || a == g_t.th) : false);
                    if (want == 2 || want == 3) part = part || false;
                    if (want == 3) part = a == g_t.caption || a == g_t.col || a == g_t.colgroup || is_section(a);
                    if (want == 2) part = a == g_t.caption || a == g_t.col || a == g_t.colgroup || is_section(a) || a == g_t.tr;
                    if (part) {
                        bool open = false;
                        for (uint32_t k = 0; k < b->sp && !open; k++) {
                            CssAtom x = g_dom.tag[b->stack[k]];
                            if (g_dom.ns[b->stack[k]] != DOM_NS_HTML) continue;
                            open = want == 1 ? (x == g_t.td || x == g_t.th) : want == 2 ? x == g_t.tr : want == 3 ? is_section(x) : x == g_t.caption;
                        }
                        if (!open) break;
                    }
                }
            }
            /* (a fragment, but for the html element's: its html, head, body, frameset and frame start tags ignored) */
            if (b->html_rules && !b->doc_mode && b->keep_top_text && (!top || !foreign_current(top)) &&
                !(g_frag_context && g_dom.tag[g_frag_context] == g_t.html && g_dom.ns[g_frag_context] == DOM_NS_HTML)) {
                static CssAtom f_body, f_head, f_frameset, f_frame;
                if (!f_body) { css_atom_reserve(4); f_body = css_atom_intern("body", 4); f_head = css_atom_intern("head", 4); f_frameset = css_atom_intern("frameset", 8); f_frame = css_atom_intern("frame", 5); }
                CssAtom a = t->tag;
                if (a == g_t.html || a == f_body || a == f_head || a == f_frameset || a == f_frame) break;
            }
            /* (with a template current: its contents' insertion mode, from the first start tag; table parts out of place ignored, missing ones implied) */
            if (b->html_rules && (!top || !foreign_current(top))) {
                CssAtom a = t->tag;
                bool tpart = is_table_part(a) || a == g_t.col;
                /* (a button in an open button: that one closes; a table in a table, not in a cell or caption: that one closes) */
                if (a == g_t.button) { int bk = in_scope(b, g_t.button, false); if (bk >= 0) { pop_to(b, (uint32_t)bk); top = b->sp ? b->stack[b->sp - 1] : 0; top_fresh = top && b->streaming && b->fresh[b->sp - 1]; } }
                if (a == g_t.table) {
                    for (uint32_t k = b->sp; k-- > 0; ) {
                        CssAtom x = g_dom.tag[b->stack[k]];
                        if (g_dom.ns[b->stack[k]] != DOM_NS_HTML) continue;
                        if (x == g_t.table) { if (k + 1 == b->sp || table_context(b->stack[b->sp - 1]) || g_dom.tag[b->stack[b->sp - 1]] == g_t.colgroup) { pop_to(b, k); top = b->sp ? b->stack[b->sp - 1] : 0; top_fresh = top && b->streaming && b->fresh[b->sp - 1]; } break; }
                        if (is_cell(x) || x == g_t.caption || x == g_t.tmpl) break;
                    }
                }
                if (top && g_dom.tag[top] == g_t.tmpl && g_dom.ns[top] == DOM_NS_HTML) {
                    int m = tmode_get(b, top);
                    if (!m && !head_content(a)) {
                        m = a == g_t.caption || a == g_t.colgroup || is_section(a) ? TM_TABLE : a == g_t.col ? TM_COLGROUP :
                            a == g_t.tr ? TM_TBODY : is_cell(a) ? TM_ROW : TM_BODY;
                        tmode_set(b, top, m);
                    }
                    if (m == TM_BODY && tpart) break;
                    if (m == TM_TBODY && (a == g_t.caption || a == g_t.colgroup || a == g_t.col || is_section(a))) break;
                    if (m == TM_ROW && (a == g_t.tr || a == g_t.caption || a == g_t.colgroup || a == g_t.col || is_section(a))) break;
                    if (m == TM_COLGROUP && a != g_t.col && !head_content(a)) break;
                    if (m == TM_TABLE && (a == g_t.tr || is_cell(a))) implied(b, g_t.tbody, top);
                    if (m == TM_TABLE && a == g_t.col) implied(b, g_t.colgroup, top);
                    if ((m == TM_TABLE || m == TM_TBODY) && is_cell(a)) implied(b, g_t.tr, b->stack[b->sp - 1]);
                    top = b->stack[b->sp - 1];
                    top_fresh = b->streaming && b->fresh[b->sp - 1];
                } else {
                    int near = 0, nk = -1;
                    uint32_t ts = 0;
                    for (uint32_t k = b->sp; k-- > 0 && !near; ) {
                        CssAtom x = g_dom.tag[b->stack[k]];
                        if (g_dom.ns[b->stack[k]] != DOM_NS_HTML) continue;
                        if (x == g_t.table || x == g_t.caption || is_section(x) || x == g_t.tr || is_cell(x) || x == g_t.colgroup) { near = 1; nk = (int)k; }
                        else if (x == g_t.tmpl) { near = 2; ts = b->stack[k]; nk = (int)k; }
                    }
                    /* (in a table, a row group or a row with what was fostered open: back to it first) */
                    if (tpart && near == 1 && nk + 1 < (int)b->sp) {
                        CssAtom x = g_dom.tag[b->stack[nk]];
                        /* (but a form open in the table: what follows goes in it, its controls' owner) */
                        bool form_open = false;
                        for (uint32_t k = (uint32_t)nk + 1; k < b->sp && !form_open; k++)
                            form_open = g_dom.ns[b->stack[k]] == DOM_NS_HTML && css_atom_str(g_dom.tag[b->stack[k]]) && !strcmp(css_atom_str(g_dom.tag[b->stack[k]]), "form");
                        if (!form_open && (x == g_t.table || is_section(x) || x == g_t.tr)) {
                            pop_to(b, (uint32_t)nk + 1);
                            top = b->stack[b->sp - 1];
                            top_fresh = b->streaming && b->fresh[b->sp - 1];
                        }
                    }
                    /* (in the body, not in a table: a table's part is ignored) */
                    if (tpart && near == 2 && tmode_get(b, ts) == TM_BODY) break;
                    if (tpart && !near && b->doc_mode && b->phase == DOC_IN_BODY) break;
                    /* (a fragment: its context's insertion mode, HTML 13.2.4.1 "reset the insertion mode") */
                    uint32_t cx = g_frag_context;
                    if (!near && !b->doc_mode && b->keep_top_text && cx && g_dom.kind[cx] == DOM_K_ELEMENT && g_dom.ns[cx] == DOM_NS_HTML) {
                        CssAtom c = g_dom.tag[cx];
                        bool tctx = c == g_t.table || is_section(c) || c == g_t.tr || c == g_t.caption || c == g_t.colgroup;
                        if (!tctx && c != g_t.tmpl && c != g_t.html && c != g_t.select && !is_cell(c) && tpart) break;
                        if (c == g_t.caption && tpart) break;
                        if (c == g_t.colgroup && b->sp == 0 && a != g_t.col && a != g_t.tmpl) break;
                        if (c == g_t.table && a == g_t.table) break;
                        if ((c == g_t.table || is_section(c) || c == g_t.tr) && tpart) {
                            if (is_section(c) && (a == g_t.caption || a == g_t.colgroup || a == g_t.col || is_section(a))) break;
                            if (c == g_t.tr && !is_cell(a)) break;
                            if (b->sp) pop_to(b, 0);
                            if (c == g_t.table && (a == g_t.tr || is_cell(a))) implied(b, g_t.tbody, b->frag);
                            if (c == g_t.table && a == g_t.col) implied(b, g_t.colgroup, b->frag);
                            if ((c == g_t.table || is_section(c)) && is_cell(a)) implied(b, g_t.tr, b->sp ? b->stack[b->sp - 1] : b->frag);
                            top = b->sp ? b->stack[b->sp - 1] : 0;
                            top_fresh = top && b->streaming && b->fresh[b->sp - 1];
                        }
                    }
                }
            }
            if (top && b->html_rules) {
                close_implied(b, t->tag);
                top = b->sp ? b->stack[b->sp - 1] : 0;
                top_fresh = top && b->streaming && b->fresh[b->sp - 1];
            }
            /* (the formatting elements: an a inside an a, a nobr inside a nobr adopt first; then what was closed opens again) */
            if (b->html_rules && (!top || g_dom.ns[top] == DOM_NS_HTML || !foreign_current(top))) {
                bool fmt = is_formatting(t->tag);
                const char *tn = fmt ? css_atom_str(t->tag) : NULL;
                if (tn && !strcmp(tn, "a")) {
                    int ai = -1;
                    for (uint32_t k = b->afl_n; k-- > 0; ) { uint32_t e = b->afl[k]; if (e == AFL_MARKER) break; if (g_dom.tag[e] == t->tag) { ai = (int)k; break; } }
                    if (ai >= 0) {
                        uint32_t old = b->afl[ai];
                        adoption_agency(b, t->tag);
                        int k2 = afl_index(b, old);
                        if (k2 >= 0) afl_remove_at(b, (uint32_t)k2);
                        int s2 = stack_index(b, old);
                        if (s2 >= 0) stack_remove(b, (uint32_t)s2);
                    }
                }
                if (!no_reconstruct(t->tag)) afl_reconstruct(b);
                if (tn && !strcmp(tn, "nobr") && in_scope(b, t->tag, false) >= 0) { adoption_agency(b, t->tag); afl_reconstruct(b); }
                top = b->sp ? b->stack[b->sp - 1] : 0;
                top_fresh = top && b->streaming && b->fresh[b->sp - 1];
            }
            uint32_t parent = top ? top : b->frag;
            /* (foster parenting: before the table) */
            uint32_t fref = 0;
            if (top && b->html_rules && table_context(top) && !table_allowed(t)) {
                uint32_t fp = foster_parent(b, &fref);
                if (fp) { parent = fp; top_fresh = false; }
            }
            g_foster_ref = fref;
            uint32_t s = build_element(b, t, parent, top_fresh);
            g_foster_ref = 0;
            if (b->doc_mode) {
                if (t->tag == g_a_html && !b->el_html) { b->el_html = s; b->phase = DOC_BEFORE_HEAD; }
                else if (t->tag == g_a_head && !b->el_head) { b->el_head = s; b->phase = DOC_IN_HEAD; }
                else if (t->tag == g_a_body && !b->el_body) { resp_body_note(b->el_head, b->el_html); b->el_body = s; b->phase = DOC_IN_BODY; }
            }
            if (b->doc_mode && !b->el_body && b->el_html && parent == b->el_head && b->el_head && resp_is_meta(dom_id_of(s))) resp_add(b->el_html);
            uint16_t f = g_dom.flags[s];
            if ((f & DOMF_RAW_TEXT || t->tag == g_raw_atoms[8]) && !(f & DOMF_SELF_CLOSED)) {
                if (i + 1 < c->n && c->toks[i + 1].kind == TK_RAW) {
                    DomTok *r = &c->toks[++i];
                    if (r->text.len) build_text(b, r, s, true);
                }
            }
            /* A streamed script runs before the parser goes on. */
            if (b->streaming && t->tag == g_raw_atoms[0]) {
                b->stopped_script = s;
                *pi = i + 1;
                return true;
            }
            /* (a self-closing flag counts only in foreign content: an HTML <div/> is a <div>) */
            /* (a raw-text one only when HTML: an svg <style> or <script> holds its text as children) */
            if (((f & DOMF_RAW_TEXT) && (g_dom.ns[s] == DOM_NS_HTML || (i > 0 && c->toks[i].kind == TK_RAW))) || (f & DOMF_VOID) ||
                ((f & DOMF_SELF_CLOSED) && (g_dom.ns[s] != DOM_NS_HTML || !b->html_rules))) break;
            if (b->sp + 1 < MAX_NESTING) {
                if (b->streaming) b->fresh[b->sp] = 1;
                b->stack[b->sp++] = s;
            }
            if (b->html_rules && g_dom.ns[s] == DOM_NS_HTML) {
                if (is_formatting(t->tag)) afl_push_formatting(b, s);
                else if (is_marker_tag(t->tag)) afl_push(b, AFL_MARKER);
                else if (css_atom_str(t->tag) && !strcmp(css_atom_str(t->tag), "form")) {
                    bool tmpl = false;
                    for (uint32_t k = 0; k + 1 < b->sp && !tmpl; k++) tmpl = g_dom.tag[b->stack[k]] == g_t.tmpl;
                    if (!tmpl) b->form_ptr = s;
                }
            }
            break;
        }
        case TK_END:
            /* (foreign content: an end tag naming the current node, an svg <style> or <script> say, closes it) */
            if (top && b->html_rules && g_dom.ns[top] != DOM_NS_HTML && (g_dom.flags[top] & DOMF_RAW_TEXT) &&
                css_atom_str(g_dom.tag[top]) && end_tag_is(b, t, css_atom_str(g_dom.tag[top]))) { pop_to(b, b->sp - 1); break; }
            /* (</p> or </br> in foreign content: out of it first) */
            if (top && b->html_rules && foreign_current(top) && (end_tag_is(b, t, "p") || end_tag_is(b, t, "br"))) {
                while (b->sp && foreign_current(b->stack[b->sp - 1])) pop_to(b, b->sp - 1);
                top = b->sp ? b->stack[b->sp - 1] : 0;
            }
            if (top && b->html_rules && b->afl_n && !foreign_current(top)) {
                fmt_atoms();
                CssAtom ft = 0;
                for (int k = 0; k < g_nfmt && !ft; k++) if (end_tag_is(b, t, css_atom_str(g_fmt[k]))) ft = g_fmt[k];
                /* (a formatting element outside an open select: its end tag ignored in the select) */
                if (ft) {
                    int si_ = -1, fi_ = -1;
                    for (uint32_t k = 0; k < b->sp; k++) { if (g_dom.tag[b->stack[k]] == g_t.select) si_ = (int)k; if (g_dom.tag[b->stack[k]] == ft) fi_ = (int)k; }
                    if (si_ >= 0 && fi_ >= 0 && fi_ < si_) break;
                }
                if (ft && adoption_agency(b, ft)) break;
            }
            if (b->html_rules && end_tag_is(b, t, "form")) {
                bool tmpl_ = false;
                for (uint32_t k = 0; k < b->sp && !tmpl_; k++) tmpl_ = g_dom.tag[b->stack[k]] == g_t.tmpl;
                uint32_t fp_ = b->form_ptr;
                b->form_ptr = 0;
                if (!tmpl_) {
                    /* (no template open: the form pointer's form, if in scope, leaves the stack where it is) */
                    int fk = -1;
                    for (uint32_t k = b->sp; k-- > 0; ) { if (b->stack[k] == fp_) { fk = (int)k; break; } if (scope_stop(b->stack[k])) break; }
                    if (fp_ && fk >= 0) { stack_remove(b, (uint32_t)fk); break; }
                    if (fp_ || !b->doc_mode) { if (!fp_) { } else break; }
                }
            }
            bool obj_end = b->html_rules && b->afl_n && (end_tag_is(b, t, "applet") || end_tag_is(b, t, "marquee") || end_tag_is(b, t, "object"));
            if ((top || (b->frag && !b->doc_mode && end_tag_is(b, t, "p"))) && b->html_rules) {
                close_end_tag(b, t);
                if (obj_end) afl_clear_to_marker(b);
            } else if (top) {
                const char *tn = css_atom_str(g_dom.tag[top]);
                size_t tl = strlen(tn);
                if (t->src + 2 + tl <= b->len && strncasecmp(b->html + t->src + 2, tn, tl) == 0) {
                    b->sp--;
                    if (b->doc_mode && top == b->el_head && b->phase == DOC_IN_HEAD) b->phase = DOC_AFTER_HEAD;
                }
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
            /* (a template whose contents are a column group's: its text, but for white space, is nothing) */
            if (top && b->html_rules && g_dom.tag[top] == g_t.tmpl && tmode_get(b, top) == TM_COLGROUP && !all_space(&t->text)) break;
            if (top && b->html_rules && table_context(top) && !all_space(&t->text)) {
                uint32_t fref = 0, fp = foster_parent(b, &fref);
                g_foster_ref = fref;
                build_text(b, t, fp ? fp : top, fp ? false : top_fresh);
                g_foster_ref = 0;
            } else if (top) {
                if (b->html_rules && b->afl_n && g_dom.ns[top] == DOM_NS_HTML) {
                    afl_reconstruct(b);
                    top = b->stack[b->sp - 1];
                    top_fresh = b->streaming && b->fresh[b->sp - 1];
                }
                build_text(b, t, top, top_fresh);
            }
            else if (b->keep_top_text) build_text(b, t, b->frag, false);
            break;
        case TK_COMMENT:
            if (top) build_comment(b, t, top, top_fresh);
            else if (b->keep_top_text || (b->doc_mode && b->phase == DOC_INITIAL)) build_comment(b, t, b->frag, false);
            break;
        case TK_DOCTYPE:
            /* (a document's, not a fragment's; at the start) */
            if (!top && !b->keep_top_text && (!b->doc_mode || b->phase == DOC_INITIAL)) build_doctype(b, t);
            break;
        default:
            break;
        }
    }
    *pi = c->n;
    return true;
}

static void parse_into(const char *html, size_t len, uint32_t frag, uint32_t owner, bool fragment_mode) {
    memset(&g_stats, 0, sizeof g_stats);
    double t_all = now_ms();
    TokChunk *chunks = NULL;
    size_t nchunks = 0;
    tokenize_all(html, len, &chunks, &nchunks);
    double t0 = now_ms();
    Builder b = { .html = html, .len = len, .frag = frag, .owner = owner, .stack = malloc(MAX_NESTING * sizeof(uint32_t)) };
    b.keep_top_text = fragment_mode;
    b.html_rules = (owner || fragment_mode);
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
    /* (the end of the input: what is still open closes, a <template for>'s patch with it) */
    if (b.html_rules) for (uint32_t i = b.sp; i-- > 0; ) if (g_dom.tag[b.stack[i]] == g_t.tmpl) template_for(&b, b.stack[i]);
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
    /* (a UTF-8 byte order mark first: the encoding's, not the document's text) */
    if (html && len >= 3 && (unsigned char)html[0] == 0xEF && (unsigned char)html[1] == 0xBB && (unsigned char)html[2] == 0xBF) { html += 3; len -= 3; }
    NodeId frag = dom_create_fragment(owner_doc);
    if (html && len) parse_into(html, len, dom_slot(frag), dom_valid(owner_doc) ? dom_slot(owner_doc) : 0, false);
    return frag;
}

NodeId dom_parse_html_fragment(const char *html, size_t len, NodeId owner_doc) {
    NodeId frag = dom_create_fragment(owner_doc);
    if (html && len) parse_into(html, len, dom_slot(frag), dom_valid(owner_doc) ? dom_slot(owner_doc) : 0, true);
    return frag;
}

void dom_parse_html_into(const char *html, size_t len, NodeId parent, NodeId owner_doc) {
    if (!html || !len || !dom_valid(parent)) return;
    parse_into(html, len, dom_slot(parent), dom_valid(owner_doc) ? dom_slot(owner_doc) : 0, false);
}

/* ============================================================================
 * Streaming (dom_stream_*)
 * ============================================================================ */

struct DomStream {
    char *buf;
    size_t len, cap;
    size_t pos;                 /* where the next tokenizing starts */
    TokChunk cur;               /* tokens not built yet: [ci, cur.n) */
    size_t ci;
    bool have_cur;
    size_t insert_pos;          /* document.write's insertion point */
    size_t limit;               /* build no further than this (0: no limit) */
    Builder b;
};

DomStream *dom_stream_begin(NodeId parent, NodeId owner_doc) {
    if (!dom_valid(parent)) return NULL;
    init_raw_atoms();
    css_atom_reserve(2);
    if (!g_template_atom) g_template_atom = css_atom_intern("template", 8);
    DomStream *st = calloc(1, sizeof(DomStream));
    if (!st) return NULL;
    st->b.frag = dom_slot(parent);
    st->b.owner = dom_valid(owner_doc) ? dom_slot(owner_doc) : 0;
    st->b.stack = malloc(MAX_NESTING * sizeof(uint32_t));
    st->b.fresh = calloc(MAX_NESTING, 1);
    st->b.id_name = css_atom_intern("id", 2);
    st->b.streaming = true;
    st->b.html_rules = true;
    /* (a whole document: html, head and body implied) */
    if (g_dom.kind[st->b.frag] == DOM_K_DOCUMENT) { doc_atoms(); st->b.doc_mode = true; }
    return st;
}

void dom_stream_append(DomStream *st, const char *data, size_t n) {
    if (!st || !n) return;
    if (st->len + n + 1 > st->cap) {
        size_t cap = st->cap ? st->cap : 65536;
        while (st->len + n + 1 > cap) cap *= 2;
        char *nb = realloc(st->buf, cap);
        if (!nb) return;
        st->buf = nb;
        st->cap = cap;
    }
    memcpy(st->buf + st->len, data, n);
    st->len += n;
    st->buf[st->len] = '\0';
}

const char *dom_stream_source(DomStream *st, size_t *len) {
    if (len) *len = st ? st->len : 0;
    return st ? st->buf : NULL;
}

NodeId dom_stream_pump(DomStream *st, bool final) {
    if (!st) return DOM_NULL;
    Builder *b = &st->b;
    memset(b->fresh, 0, b->sp);
    b->nroots = 0;
    b->stopped_script = 0;
    b->html = st->buf;
    b->len = st->len;
    for (;;) {
        if (st->have_cur && st->ci < st->cur.n) {
            size_t detour = 0;
            if (!build_tokens(b, &st->cur, &st->ci, &detour)) {
                /* Re-tokenize from inside the stray end tag. */
                chunk_free(&st->cur);
                st->have_cur = false;
                st->pos = detour;
                continue;
            }
            if (b->stopped_script) {
                /* What the script writes goes right after it. */
                st->insert_pos = st->cur.toks[st->ci - 1].end;
                return dom_id_of(b->stopped_script);
            }
            continue;
        }
        if (st->have_cur) { chunk_free(&st->cur); st->have_cur = false; }
        size_t end = st->limit && st->limit < st->len ? st->limit : st->len;
        bool last = final && end == st->len;
        if (st->pos >= end) return DOM_NULL;
        TokChunk *c = &st->cur;
        memset(c, 0, sizeof *c);
        c->html = st->buf;
        c->len = end;
        c->full_len = st->len;
        c->start = st->pos;
        c->limit = end;
        c->doc_start = st->pos == 0;
        tokenize(c, true);
        /* Only tokens that have wholly arrived: one reaching the end of the
         * input may be cut short (a text run, a tag, a script's content). */
        size_t keep = c->n;
        if (!last) {
            /* Up to a limit (written input), a tag that ends right there is
             * whole; at the end of what has arrived it may be cut short. */
            keep = 0;
            while (keep < c->n && (c->toks[keep].end < end ||
                                   (end < st->len && c->toks[keep].kind != TK_TEXT &&
                                    c->toks[keep].end == end && st->buf[end - 1] == '>')))
                keep++;
        }
        size_t next = last ? c->end : keep ? c->toks[keep - 1].end : st->pos;
        chunk_release_tokens(c, keep);
        if (!keep) {
            chunk_free(c);
            if (last) st->pos = st->len;
            return DOM_NULL;
        }
        st->pos = next;
        st->have_cur = true;
        st->ci = 0;
    }
}

void dom_stream_insert(DomStream *st, const char *data, size_t n) {
    if (!st || !n) return;
    size_t at = st->insert_pos <= st->len ? st->insert_pos : st->len;
    /* Tokens read past the insertion point are read again after the
     * inserted input. */
    if (st->have_cur) {
        if (st->ci < st->cur.n && st->cur.toks[st->ci].src < st->pos) st->pos = st->cur.toks[st->ci].src;
        chunk_free(&st->cur);
        st->have_cur = false;
    }
    if (st->pos > at) st->pos = at;
    if (st->len + n + 1 > st->cap) {
        size_t cap = st->cap ? st->cap : 65536;
        while (st->len + n + 1 > cap) cap *= 2;
        char *nb = realloc(st->buf, cap);
        if (!nb) return;
        st->buf = nb;
        st->cap = cap;
    }
    memmove(st->buf + at + n, st->buf + at, st->len - at + 1);
    memcpy(st->buf + at, data, n);
    st->len += n;
    if (st->limit >= at && st->limit) st->limit += n;
    st->insert_pos = at + n;
}

size_t dom_stream_insertion_point(DomStream *st) { return st ? st->insert_pos : 0; }
void dom_stream_set_insertion_point(DomStream *st, size_t pos) { if (st) st->insert_pos = pos; }
void dom_stream_set_limit(DomStream *st, size_t limit) { if (st) st->limit = limit; }

const uint32_t *dom_stream_new_roots(DomStream *st, uint32_t *n) {
    *n = st ? st->b.nroots : 0;
    return st ? st->b.roots : NULL;
}

void dom_stream_end(DomStream *st) {
    if (!st) return;
    if (st->b.html_rules && g_template_atom)
        for (uint32_t i = st->b.sp; i-- > 0; ) if (g_dom.tag[st->b.stack[i]] == g_template_atom && dom_valid(dom_id_of(st->b.stack[i]))) template_for(&st->b, st->b.stack[i]);
    if (st->have_cur) chunk_free(&st->cur);
    free(st->b.stack);
    free(st->b.fresh);
    free(st->b.roots);
    free(st->buf);
    free(st);
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
