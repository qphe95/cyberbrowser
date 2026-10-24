/*
 * Structure index of a script: one scan of the whole script that makes, for
 * every function body, the decisions js_lazy_scan_body (quickjs.c) makes when
 * it scans that body alone.  See js_struct_index.h.
 *
 * The serial scan of a body starts at its '{' with an empty context and
 * reads to the matching '}'.  Here the script is read once from its start;
 * each body that opens gets its own context (the serial scan's declaration
 * state, its names and flags), and tokens update the innermost one.  The
 * outer contexts would not change inside a nested body anyway: their
 * declaration state only reacts at their own nesting level.  When a body
 * closes, its names join its parent's (the parent's serial scan reads the
 * nested body too), as do its this / super / new flags.  Where the serial
 * scan of a body would give up (direct eval, an HTML-like comment, nesting
 * too deep) every open body is marked eager; where the source cannot be
 * scanned further with confidence (an escaped or non-ASCII identifier,
 * mismatched brackets, an unterminated literal) the scan stops, and bodies
 * still open are left unclosed (the parser scans those serially).
 */
#include "js_struct_index.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LAZY_MAX_NESTING 1024   /* as in quickjs.c */

/* classes of the previous token, as js_lazy_scan_body's */
enum {
    LZ_START, LZ_PUNCT, LZ_OPERAND, LZ_IDENT, LZ_KW_EXPR, LZ_KW_CTRL, LZ_KW_BLOCK,
    LZ_CLOSE_CTRL, LZ_CLOSE_PAREN, LZ_CLOSE_BRACKET, LZ_CLOSE_BLOCK, LZ_CLOSE_OBJECT,
    LZ_INCDEC, LZ_DOT, LZ_ARROW, LZ_COLON,
};

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static inline bool is_ident_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* One UTF-8 sequence at p: its length (0 if malformed) and code point. */
static int utf8_decode(const uint8_t *p, const uint8_t *end, uint32_t *cp)
{
    int n = *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : *p >= 0xC0 ? 2 : 0;
    if (!n || end - p < n) return 0;
    uint32_t v = *p & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (p[i] & 0x3F);
    }
    *cp = v;
    return n;
}

/* White space (Zs and BOM) and line terminators beyond ASCII. */
static bool is_unicode_space(uint32_t cp)
{
    return cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
           cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

static bool is_line_terminator(uint32_t cp)
{
    return cp == 0x2028 || cp == 0x2029;
}

/* The rest of an identifier from p: ASCII identifier characters, \u escapes
 * and other non-ASCII characters (not spaces or line terminators). */
static const uint8_t *skip_ident(const uint8_t *p, const uint8_t *end)
{
    while (p < end) {
        uint32_t cp;
        int n;
        if (is_ident_char(*p)) {
            p++;
        } else if (*p == '\\' && p + 1 < end && p[1] == 'u') {
            p += 2;
            if (p < end && *p == '{') {
                while (p < end && *p != '}')
                    p++;
                p++;
            } else {
                p += 4;
            }
        } else if (*p >= 0x80 && (n = utf8_decode(p, end, &cp)) > 0 &&
                   !is_unicode_space(cp) && !is_line_terminator(cp)) {
            p += n;
        } else {
            break;
        }
    }
    return p < end ? p : end;
}

static inline bool kw(const uint8_t *p, size_t len, const char *k)
{
    return strlen(k) == len && !memcmp(p, k, len);
}

static inline uint64_t name_hash(const uint8_t *p, size_t len)
{
    uint64_t h = 1469598103934665603ull;
    while (len--)
        h = (h ^ *p++) * 1099511628211ull;
    return h;
}

/* ---- Growable arrays ---- */

typedef struct { JSStructName *v; uint32_t n, cap; } NameVec;

static bool namevec_push(NameVec *a, JSStructName e)
{
    if (a->n == a->cap) {
        uint32_t cap = a->cap ? a->cap * 2 : 16;
        JSStructName *v = (JSStructName *)realloc(a->v, (size_t)cap * sizeof(*v));
        if (!v) return false;
        a->v = v;
        a->cap = cap;
    }
    a->v[a->n++] = e;
    return true;
}

/* A set of names (distinct bytes), in insertion order. */
typedef struct {
    NameVec v;
    uint32_t *tab;              /* index + 1 into v, open addressing */
    uint32_t tcap;
} NameSet;

static void nameset_clear(NameSet *s)
{
    s->v.n = 0;
    if (s->tab) memset(s->tab, 0, (size_t)s->tcap * sizeof(uint32_t));
}

static void nameset_free(NameSet *s)
{
    free(s->v.v);
    free(s->tab);
    memset(s, 0, sizeof(*s));
}

static bool nameset_add(NameSet *s, const uint8_t *base, JSStructName e)
{
    if ((s->v.n + 1) * 2 > s->tcap) {
        uint32_t cap = s->tcap ? s->tcap * 2 : 64;
        uint32_t *t = (uint32_t *)calloc(cap, sizeof(uint32_t));
        if (!t) return false;
        for (uint32_t i = 0; i < s->v.n; i++) {
            uint32_t j = (uint32_t)s->v.v[i].hash & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = i + 1;
        }
        free(s->tab);
        s->tab = t;
        s->tcap = cap;
    }
    uint32_t j = (uint32_t)e.hash & (s->tcap - 1);
    while (s->tab[j]) {
        const JSStructName *o = &s->v.v[s->tab[j] - 1];
        if (o->hash == e.hash && o->len == e.len && !memcmp(base + o->off, base + e.off, e.len))
            return true;
        j = (j + 1) & (s->tcap - 1);
    }
    if (!namevec_push(&s->v, e)) return false;
    s->tab[j] = s->v.n;
    return true;
}

/* ---- The scan ---- */

/* An open body: the serial scan's state for it. */
typedef struct {
    uint32_t rec;               /* record index; UINT32_MAX for the top level */
    int base;                   /* stack index of the body's '{' (the serial scan's level 0) */
    int decl_level;             /* absolute stack level of the declaration list, or -1 */
    bool expect_binding, fdecl_pending;
    uint32_t flags;
    NameSet names;
    NameVec decls;
} Ctx;

typedef struct {
    const uint8_t *base, *end;
    char *stack;                /* bracket kinds, as the serial scan's */
    int level, stack_cap;
    Ctx *ctx;                   /* ctx[0]: the top level */
    int nctx, ctx_cap;
    JSStructRecord *recs;
    uint32_t n_recs, recs_cap;
    NameVec names, decls;       /* finished records' */
    const uint8_t *pos;         /* the token being read (where a stop happened) */
    bool oom;
} Scan;

static bool stack_push(Scan *s, char kind)
{
    if (s->level == s->stack_cap) {
        int cap = s->stack_cap ? s->stack_cap * 2 : 256;
        char *st = (char *)realloc(s->stack, (size_t)cap);
        if (!st) { s->oom = true; return false; }
        s->stack = st;
        s->stack_cap = cap;
    }
    s->stack[s->level++] = kind;
    return true;
}

/* The serial scan of each open body refuses a push past LAZY_MAX_NESTING
 * levels of its own (outer bodies have more levels: they come first). */
static void check_nesting(Scan *s)
{
    for (int i = 1; i < s->nctx && s->level - s->ctx[i].base >= LAZY_MAX_NESTING; i++)
        s->ctx[i].flags |= JS_STRUCT_EAGER;
}

static void mark_all_eager(Scan *s)
{
    for (int i = 1; i < s->nctx; i++)
        s->ctx[i].flags |= JS_STRUCT_EAGER;
}

static void add_name(Scan *s, const uint8_t *p, size_t len)
{
    Ctx *c = &s->ctx[s->nctx - 1];
    if (c->rec == UINT32_MAX) return;
    JSStructName e = { (uint32_t)(p - s->base), (uint32_t)len, name_hash(p, len) };
    if (!nameset_add(&c->names, s->base, e)) s->oom = true;
}

/* A name for the enclosing body only (not the innermost one). */
static void add_name_outer(Scan *s, const uint8_t *p, size_t len)
{
    if (s->nctx < 3) return;
    Ctx *c = &s->ctx[s->nctx - 2];
    JSStructName e = { (uint32_t)(p - s->base), (uint32_t)len, name_hash(p, len) };
    if (!nameset_add(&c->names, s->base, e)) s->oom = true;
}

static void add_decl(Scan *s, const uint8_t *p, size_t len)
{
    Ctx *c = &s->ctx[s->nctx - 1];
    if (c->rec == UINT32_MAX) return;
    JSStructName e = { (uint32_t)(p - s->base), (uint32_t)len, name_hash(p, len) };
    if (!namevec_push(&c->decls, e)) s->oom = true;
}

/* A function body opens at '{' (stack index s->level, before the push). */
static void open_body(Scan *s, const uint8_t *brace)
{
    if (s->n_recs == s->recs_cap) {
        uint32_t cap = s->recs_cap ? s->recs_cap * 2 : 1024;
        JSStructRecord *r = (JSStructRecord *)realloc(s->recs, (size_t)cap * sizeof(*r));
        if (!r) { s->oom = true; return; }
        s->recs = r;
        s->recs_cap = cap;
    }
    if (s->nctx == s->ctx_cap) {
        int cap = s->ctx_cap * 2;
        Ctx *c = (Ctx *)realloc(s->ctx, (size_t)cap * sizeof(*c));
        if (!c) { s->oom = true; return; }
        memset(c + s->ctx_cap, 0, (size_t)(cap - s->ctx_cap) * sizeof(*c));
        s->ctx = c;
        s->ctx_cap = cap;
    }
    JSStructRecord *r = &s->recs[s->n_recs];
    memset(r, 0, sizeof(*r));
    r->open = (uint32_t)(brace - s->base);
    Ctx *c = &s->ctx[s->nctx++];
    c->rec = s->n_recs++;
    c->base = s->level;
    c->decl_level = -1;
    c->expect_binding = c->fdecl_pending = false;
    c->flags = 0;
    nameset_clear(&c->names);
    c->decls.n = 0;
}

static int decl_cmp(const void *a, const void *b)
{
    uint64_t x = ((const JSStructName *)a)->hash, y = ((const JSStructName *)b)->hash;
    return x < y ? -1 : x > y;
}

/* The innermost body closes; `after` is just past its '}'. */
static void close_body(Scan *s, const uint8_t *after)
{
    Ctx *c = &s->ctx[s->nctx - 1];
    JSStructRecord *r = &s->recs[c->rec];
    r->close = (uint32_t)(after - s->base);
    r->flags = c->flags;
    r->names = s->names.n;
    r->n_names = c->names.v.n;
    for (uint32_t i = 0; i < c->names.v.n; i++)
        if (!namevec_push(&s->names, c->names.v.v[i])) s->oom = true;
    r->decls = s->decls.n;
    r->n_decls = c->decls.n;
    for (uint32_t i = 0; i < c->decls.n; i++)
        if (!namevec_push(&s->decls, c->decls.v[i])) s->oom = true;
    /* by hash, for js_struct_index_declares */
    if (!s->oom && r->n_decls > 1)
        qsort(s->decls.v + r->decls, r->n_decls, sizeof(JSStructName), decl_cmp);
    s->nctx--;
    Ctx *parent = &s->ctx[s->nctx - 1];
    if (parent->rec != UINT32_MAX) {
        parent->flags |= c->flags;
        for (uint32_t i = 0; i < c->names.v.n; i++)
            if (!nameset_add(&parent->names, s->base, c->names.v.v[i])) s->oom = true;
    }
}

/* Whether it read the whole script (false: it stopped where it could not go on). */
static bool scan(Scan *s)
{
    const uint8_t *p = s->base, *end = s->end;
    int last = LZ_START, c;
    bool catch_pending = false;

    while (p < end && !s->oom) {
        Ctx *cx = &s->ctx[s->nctx - 1];
        s->pos = p;
        c = *p;
        switch (c) {
        case '\n': case '\r':
            if (cx->decl_level == s->level && !cx->expect_binding)
                cx->decl_level = -1;
            p++;
            continue;
        case ' ': case '\t': case '\v': case '\f':
            p++;
            continue;
        case '/':
            if (p[1] == '/') {
                while (p < end && *p != '\n' && *p != '\r')
                    p++;
                continue;
            }
            if (p[1] == '*') {
                const uint8_t *q = p + 2;
                while (q + 1 < end && !(q[0] == '*' && q[1] == '/'))
                    q++;
                if (q + 1 >= end)
                    return false;
                if (cx->decl_level == s->level && !cx->expect_binding && memchr(p, '\n', q - p))
                    cx->decl_level = -1;
                p = q + 2;
                continue;
            }
            if (last == LZ_START || last == LZ_PUNCT || last == LZ_KW_EXPR ||
                last == LZ_KW_CTRL || last == LZ_KW_BLOCK || last == LZ_ARROW ||
                last == LZ_COLON || last == LZ_CLOSE_CTRL || last == LZ_CLOSE_BLOCK) {
                bool in_class = false;
                p++;
                for (;;) {
                    if (p >= end || *p == '\n' || *p == '\r')
                        return false;
                    if (*p == '\\') {
                        p += 2;
                        continue;
                    }
                    if (*p == '[')
                        in_class = true;
                    else if (*p == ']')
                        in_class = false;
                    else if (*p == '/' && !in_class)
                        break;
                    p++;
                }
                p++;
                while (p < end && is_ident_char(*p))
                    p++;
                last = LZ_OPERAND;
            } else {
                p++;
                last = LZ_PUNCT;
            }
            continue;
        case '\'':
        case '"':
            p++;
            for (;;) {
                if (p >= end || *p == '\n' || *p == '\r')
                    return false;
                if (*p == '\\') {
                    p += 2;
                    continue;
                }
                if (*p == c)
                    break;
                p++;
            }
            p++;
            last = LZ_OPERAND;
            continue;
        case '`':
            p++;
        template_chars:
            for (;;) {
                if (p >= end)
                    return false;
                if (*p == '\\') {
                    p += 2;
                    continue;
                }
                if (*p == '`') {
                    p++;
                    last = LZ_OPERAND;
                    break;
                }
                if (*p == '$' && p[1] == '{') {
                    check_nesting(s);
                    if (!stack_push(s, '`'))
                        return false;
                    p += 2;
                    last = LZ_PUNCT;
                    break;
                }
                p++;
            }
            continue;
        case '{': {
            char kind;
            if (cx->expect_binding && cx->decl_level == s->level)
                cx->decl_level = -1;
            if (last == LZ_CLOSE_PAREN || last == LZ_ARROW || last == LZ_IDENT)
                kind = 'f';
            else if (last == LZ_START || last == LZ_CLOSE_CTRL ||
                     last == LZ_KW_BLOCK || last == LZ_CLOSE_BLOCK)
                kind = '{';
            else if (last == LZ_COLON)
                kind = s->level > 0 && s->stack[s->level - 1] == 'o' ? 'o' : '{';
            else
                kind = 'o';
            check_nesting(s);
            if (kind == 'f')
                open_body(s, p);
            if (!stack_push(s, kind))
                return false;
            p++;
            last = LZ_START;
            continue;
        }
        case '}': {
            if (s->level == 0)
                return false;
            char kind = s->stack[--s->level];
            p++;
            if (kind == 'f') {
                close_body(s, p);
                cx = &s->ctx[s->nctx - 1];
            }
            if (cx->decl_level > s->level)
                cx->decl_level = -1;
            if (kind == '`')
                goto template_chars;
            if (kind != 'f' && kind != '{' && kind != 'o')
                return false;
            last = (kind == 'o') ? LZ_CLOSE_OBJECT : LZ_CLOSE_BLOCK;
            continue;
        }
        case '(':
            check_nesting(s);
            if (!stack_push(s, (last == LZ_KW_CTRL || catch_pending) ? 'c' : '('))
                return false;
            catch_pending = false;
            p++;
            last = LZ_PUNCT;
            continue;
        case ')':
            if (s->level == 0 || (s->stack[s->level - 1] != '(' && s->stack[s->level - 1] != 'c'))
                return false;
            last = (s->stack[--s->level] == 'c') ? LZ_CLOSE_CTRL : LZ_CLOSE_PAREN;
            if (cx->decl_level > s->level)
                cx->decl_level = -1;
            p++;
            continue;
        case '[':
            if (cx->expect_binding && cx->decl_level == s->level)
                cx->decl_level = -1;
            check_nesting(s);
            if (!stack_push(s, '['))
                return false;
            p++;
            last = LZ_PUNCT;
            continue;
        case ']':
            if (s->level == 0 || s->stack[s->level - 1] != '[')
                return false;
            s->level--;
            if (cx->decl_level > s->level)
                cx->decl_level = -1;
            p++;
            last = LZ_CLOSE_BRACKET;
            continue;
        case ';':
            if (cx->decl_level == s->level)
                cx->decl_level = -1;
            p++;
            last = LZ_START;
            continue;
        case ',':
            if (cx->decl_level == s->level)
                cx->expect_binding = true;
            p++;
            last = LZ_PUNCT;
            continue;
        case ':':
            p++;
            last = LZ_COLON;
            continue;
        case '.':
            if (p[1] >= '0' && p[1] <= '9')
                goto number;
            if (p[1] == '.' && p[2] == '.') {
                p += 3;
                last = LZ_PUNCT;
            } else {
                p++;
                last = LZ_DOT;
            }
            continue;
        case '?':
            if (p[1] == '.' && !(p[2] >= '0' && p[2] <= '9')) {
                p += 2;
                last = LZ_DOT;
            } else {
                p++;
                last = LZ_PUNCT;
            }
            continue;
        case '=':
            if (p[1] == '>') {
                p += 2;
                last = LZ_ARROW;
            } else {
                p++;
                last = LZ_PUNCT;
            }
            continue;
        case '+':
        case '-':
            if (c == '-' && p[1] == '-' && p[2] == '>')
                return false;     /* possible HTML-like comment: the rest of the line may be one */
            if (p[1] == c) {
                p += 2;
                last = LZ_INCDEC;
            } else {
                p++;
                last = LZ_PUNCT;
            }
            continue;
        case '<':
            if (p[1] == '!' && p[2] == '-' && p[3] == '-') {
                /* an HTML-like comment (a line comment in a script); the
                   serial scan refuses the bodies around it */
                mark_all_eager(s);
                while (p < end && *p != '\n' && *p != '\r')
                    p++;
                continue;
            }
            p++;
            last = LZ_PUNCT;
            continue;
        case '*':
            p++;
            last = LZ_PUNCT;
            continue;
        case '#': {
            const uint8_t *start = p++;
            if (p >= end || !is_ident_char(*p) || (*p >= '0' && *p <= '9'))
                return false;
            while (p < end && is_ident_char(*p))
                p++;
            if (p < end && (*p == '\\' || *p >= 0x80))
                return false;
            add_name(s, start, p - start);
            last = LZ_IDENT;
            continue;
        }
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        number:
            p++;
            while (p < end && (is_ident_char(*p) || *p == '.' ||
                               ((*p == '+' || *p == '-') &&
                                (p[-1] == 'e' || p[-1] == 'E') &&
                                !(p[-2] == 'x' || p[-2] == 'X'))))
                p++;
            last = LZ_OPERAND;
            continue;
        default:
            if (last == LZ_DOT && (is_ident_char(c) || c >= 0x80 || c == '\\')) {
                while (p < end) {
                    if (is_ident_char(*p) || *p >= 0x80) {
                        p++;
                    } else if (*p == '\\' && p + 1 < end && p[1] == 'u') {
                        p += 2;
                        if (p < end && *p == '{') {
                            while (p < end && *p != '}')
                                p++;
                            p++;
                        } else {
                            p += 4;
                        }
                    } else {
                        break;
                    }
                }
                last = LZ_IDENT;
                continue;
            }
            if (c >= 0x80 || c == '\\') {
                uint32_t cp;
                int n = c >= 0x80 ? utf8_decode(p, end, &cp) : 0;
                if (n && is_line_terminator(cp)) {
                    if (cx->decl_level == s->level && !cx->expect_binding)
                        cx->decl_level = -1;
                    p += n;
                    continue;
                }
                if (n && is_unicode_space(cp)) {
                    p += n;
                    continue;
                }
                /* an escaped or non-ASCII identifier: the serial scan refuses
                   the bodies around it */
                const uint8_t *start = p;
                p = skip_ident(p, end);
                if (p == start)
                    return false;
                mark_all_eager(s);
                last = LZ_IDENT;
                continue;
            }
            if (is_ident_char(c)) {
                const uint8_t *start = p;
                size_t len;
                int prev = last;
                bool body_level = s->nctx > 1 && s->level == cx->base + 1 &&
                                  (prev == LZ_START || prev == LZ_CLOSE_BLOCK);
                while (p < end && is_ident_char(*p))
                    p++;
                if (p < end && (*p == '\\' || *p >= 0x80)) {
                    const uint8_t *q = skip_ident(p, end);
                    if (q != p) {
                        /* it goes on with escapes or non-ASCII characters */
                        p = q;
                        if (prev != LZ_DOT)
                            mark_all_eager(s);
                        last = LZ_IDENT;
                        continue;
                    }
                }
                len = p - start;
                if (prev == LZ_DOT) {
                    last = LZ_IDENT;
                    continue;
                }
                switch (start[0]) {
                case 'a':
                    if (kw(start, len, "await")) { last = LZ_KW_EXPR; goto add; }
                    break;
                case 'b':
                    if (kw(start, len, "break")) { last = LZ_IDENT; continue; }
                    break;
                case 'c':
                    if (kw(start, len, "case")) { last = LZ_KW_EXPR; continue; }
                    if (kw(start, len, "catch")) { catch_pending = true; last = LZ_KW_EXPR; continue; }
                    if (kw(start, len, "class")) {
                        if (body_level)
                            cx->fdecl_pending = true;
                        last = LZ_KW_EXPR;
                        continue;
                    }
                    if (kw(start, len, "const")) {
                        if (body_level) {
                            cx->decl_level = s->level;
                            cx->expect_binding = true;
                        }
                        last = LZ_KW_EXPR;
                        continue;
                    }
                    if (kw(start, len, "continue")) { last = LZ_IDENT; continue; }
                    break;
                case 'd':
                    if (kw(start, len, "do")) { last = LZ_KW_BLOCK; continue; }
                    if (kw(start, len, "delete") || kw(start, len, "default") ||
                        kw(start, len, "debugger")) { last = LZ_KW_EXPR; continue; }
                    break;
                case 'e':
                    if (kw(start, len, "else")) { last = LZ_KW_BLOCK; continue; }
                    if (kw(start, len, "eval")) {
                        /* direct eval sees the whole scope */
                        mark_all_eager(s);
                        break;
                    }
                    if (kw(start, len, "extends") || kw(start, len, "export") ||
                        kw(start, len, "enum")) { last = LZ_KW_EXPR; continue; }
                    break;
                case 'f':
                    if (kw(start, len, "for")) { last = LZ_KW_CTRL; continue; }
                    if (kw(start, len, "finally")) { last = LZ_KW_BLOCK; continue; }
                    if (kw(start, len, "false")) { last = LZ_OPERAND; continue; }
                    if (kw(start, len, "function")) {
                        if (body_level)
                            cx->fdecl_pending = true;
                        last = LZ_KW_EXPR;
                        continue;
                    }
                    break;
                case 'i':
                    if (kw(start, len, "if")) { last = LZ_KW_CTRL; continue; }
                    if (kw(start, len, "in") || kw(start, len, "instanceof") ||
                        kw(start, len, "import")) { last = LZ_KW_EXPR; continue; }
                    break;
                case 'l':
                    if (kw(start, len, "let") && body_level) {
                        cx->decl_level = s->level;
                        cx->expect_binding = true;
                        /* the serial scans of the enclosing bodies (not at
                           their own level) take it for a name */
                        add_name_outer(s, start, len);
                        last = LZ_KW_EXPR;
                        continue;
                    }
                    break;
                case 'n':
                    if (kw(start, len, "new")) { cx->flags |= JS_STRUCT_USES_NEW; last = LZ_KW_EXPR; continue; }
                    if (kw(start, len, "null")) { last = LZ_OPERAND; continue; }
                    break;
                case 'o':
                    if (kw(start, len, "of")) { last = LZ_KW_EXPR; goto add; }
                    break;
                case 'r':
                    if (kw(start, len, "return")) { last = LZ_KW_EXPR; continue; }
                    break;
                case 's':
                    if (kw(start, len, "super")) { cx->flags |= JS_STRUCT_USES_SUPER; last = LZ_OPERAND; continue; }
                    if (kw(start, len, "switch")) { last = LZ_KW_CTRL; continue; }
                    break;
                case 't':
                    if (kw(start, len, "this")) { cx->flags |= JS_STRUCT_USES_THIS; last = LZ_OPERAND; continue; }
                    if (kw(start, len, "true")) { last = LZ_OPERAND; continue; }
                    if (kw(start, len, "try")) { last = LZ_KW_BLOCK; continue; }
                    if (kw(start, len, "typeof") || kw(start, len, "throw")) { last = LZ_KW_EXPR; continue; }
                    break;
                case 'v':
                    if (kw(start, len, "var")) {
                        if (s->nctx > 1) {
                            cx->decl_level = s->level;
                            cx->expect_binding = true;
                        }
                        last = LZ_KW_EXPR;
                        continue;
                    }
                    if (kw(start, len, "void")) { last = LZ_KW_EXPR; continue; }
                    break;
                case 'w':
                    if (kw(start, len, "while") || kw(start, len, "with")) { last = LZ_KW_CTRL; continue; }
                    break;
                case 'y':
                    if (kw(start, len, "yield")) { last = LZ_KW_EXPR; goto add; }
                    break;
                }
                last = LZ_IDENT;
                if (cx->fdecl_pending) {
                    cx->fdecl_pending = false;
                    add_decl(s, start, len);
                } else if (cx->expect_binding && cx->decl_level == s->level) {
                    cx->expect_binding = false;
                    add_decl(s, start, len);
                }
            add:
                add_name(s, start, len);
                continue;
            }
            p++;
            last = LZ_PUNCT;
            continue;
        }
    }
    return !s->oom;
}

JSStructIndex *js_struct_index_build(const uint8_t *src, size_t len)
{
    double t0 = now_ms();
    if (len >= UINT32_MAX) return NULL;
    Scan s;
    memset(&s, 0, sizeof(s));
    s.base = src;
    s.end = src + len;
    s.ctx_cap = 64;
    s.ctx = (Ctx *)calloc((size_t)s.ctx_cap, sizeof(Ctx));
    JSStructIndex *ix = (JSStructIndex *)calloc(1, sizeof(*ix));
    if (!s.ctx || !ix) {
        free(s.ctx);
        free(ix);
        return NULL;
    }
    s.ctx[0].rec = UINT32_MAX;
    s.ctx[0].base = -1;
    s.ctx[0].decl_level = -1;
    s.nctx = 1;
    bool whole = scan(&s);
    /* CYBER_STRUCT_LOG=1: where a scan stopped, and what it read there */
    if (!whole && getenv("CYBER_STRUCT_LOG")) {
        size_t at = s.pos ? (size_t)(s.pos - src) : 0;
        char ctxt[81];
        size_t a = at > 40 ? at - 40 : 0, n = 0;
        for (size_t i = a; i < len && n < 80; i++, n++) ctxt[n] = (src[i] < 32 || src[i] > 126) ? '?' : (char)src[i];
        ctxt[n] = 0;
        fprintf(stderr, "[struct] %zu bytes: stopped at %zu (byte 0x%02x), %u records, level %d, open bodies %d: %s\n",
                len, at, at < len ? src[at] : 0, s.n_recs, s.level, s.nctx - 1, ctxt);
    }
    /* (where it stopped: bodies still open stay unclosed) */
    ix->src_len = len;
    ix->recs = s.recs;
    ix->n_recs = s.n_recs;
    ix->names = s.names.v;
    ix->n_names = s.names.n;
    ix->decls = s.decls.v;
    ix->n_decls = s.decls.n;
    ix->stop = whole ? (uint32_t)len : 0;
    for (int i = 0; i < s.ctx_cap; i++) {
        nameset_free(&s.ctx[i].names);
        free(s.ctx[i].decls.v);
    }
    free(s.ctx);
    free(s.stack);
    if (s.oom) {
        js_struct_index_free(ix);
        return NULL;
    }
    ix->scan_ms = now_ms() - t0;
    return ix;
}

void js_struct_index_free(JSStructIndex *ix)
{
    if (!ix) return;
    free(ix->recs);
    free(ix->names);
    free(ix->decls);
    free(ix);
}

const JSStructRecord *js_struct_index_find(const JSStructIndex *ix, uint32_t open)
{
    if (!ix) return NULL;
    uint32_t lo = 0, hi = ix->n_recs;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (ix->recs[mid].open < open) lo = mid + 1;
        else hi = mid;
    }
    if (lo < ix->n_recs && ix->recs[lo].open == open && ix->recs[lo].close)
        return &ix->recs[lo];
    return NULL;
}

bool js_struct_index_declares(const JSStructIndex *ix, const JSStructRecord *r,
                              const uint8_t *src, const JSStructName *n)
{
    const JSStructName *d = ix->decls + r->decls;
    uint32_t lo = 0, hi = r->n_decls;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (d[mid].hash < n->hash) lo = mid + 1;
        else hi = mid;
    }
    for (; lo < r->n_decls && d[lo].hash == n->hash; lo++)
        if (d[lo].len == n->len && !memcmp(src + d[lo].off, src + n->off, n->len))
            return true;
    return false;
}

/* ---- Registry ---- */

typedef struct Ticket {
    const char *data;
    size_t len;
    uint64_t hash;              /* of the bytes scanned: the claim checks them */
    JSStructIndex *ix;
    bool done;
    struct Ticket *next;
} Ticket;

/* A 64-bit hash of a whole buffer, eight bytes at a time. */
static uint64_t content_hash(const uint8_t *p, size_t len)
{
    uint64_t h = 0x9E3779B97F4A7C15ull ^ len;
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < len; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 29);
}

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_published = PTHREAD_COND_INITIALIZER;
static Ticket *g_tickets;
static JSStructIndexStats g_stats;

void *js_struct_index_announce(const char *data, size_t len)
{
    Ticket *t = (Ticket *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->data = data;
    t->len = len;
    t->hash = content_hash((const uint8_t *)data, len);
    pthread_mutex_lock(&g_mu);
    t->next = g_tickets;
    g_tickets = t;
    pthread_mutex_unlock(&g_mu);
    return t;
}

void js_struct_index_publish(void *ticket, JSStructIndex *ix)
{
    Ticket *t = (Ticket *)ticket;
    if (!t) {
        js_struct_index_free(ix);
        return;
    }
    pthread_mutex_lock(&g_mu);
    t->ix = ix;
    t->done = true;
    if (ix) {
        g_stats.built++;
        g_stats.scan_ms += ix->scan_ms;
        if (ix->stop != ix->src_len) g_stats.stopped_early++;
    }
    pthread_cond_broadcast(&g_published);
    pthread_mutex_unlock(&g_mu);
}

JSStructIndex *js_struct_index_claim(const char *data, size_t len)
{
    JSStructIndex *ix = NULL;
    uint64_t hash = 0;
    pthread_mutex_lock(&g_mu);
    Ticket **pt = &g_tickets;
    while (*pt && !((*pt)->data == data && (*pt)->len == len))
        pt = &(*pt)->next;
    Ticket *t = *pt;
    if (t) {
        if (!t->done) {
            double t0 = now_ms();
            while (!t->done)
                pthread_cond_wait(&g_published, &g_mu);
            g_stats.waited++;
            g_stats.wait_ms += now_ms() - t0;
        }
        /* the ticket may have moved in the list while we waited */
        pt = &g_tickets;
        while (*pt != t)
            pt = &(*pt)->next;
        *pt = t->next;
        ix = t->ix;
        hash = t->hash;
        free(t);
    }
    pthread_mutex_unlock(&g_mu);
    if (ix && content_hash((const uint8_t *)data, len) != hash) {
        /* not the bytes that were scanned (an earlier buffer at this address);
           the index is kept (a script stencil compile may be reading it) */
        ix = NULL;
    }
    if (ix) {
        pthread_mutex_lock(&g_mu);
        g_stats.claimed++;
        pthread_mutex_unlock(&g_mu);
    }
    return ix;
}

static void *(*g_pc_announce)(const char *data, size_t len);
static void (*g_pc_start)(void *ticket, uint8_t *copy, size_t len, JSStructIndex *ix);

void js_struct_index_set_precompile(void *(*announce)(const char *data, size_t len),
                                    void (*start)(void *ticket, uint8_t *copy, size_t len,
                                                  JSStructIndex *ix))
{
    g_pc_start = start;
    g_pc_announce = announce;
}

void js_struct_index_scan_fetched(const char *url, const char *data, size_t len,
                                  void (*release)(void *arg), void *arg)
{
    /* Scripts worth it: big enough, and not a stylesheet, font, image or
     * page by name or first byte. */
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("CYBER_STRUCT_INDEX");
        off = e && e[0] == '0';
    }
    const char *q = url ? strchr(url, '?') : NULL;
    size_t ul = url ? (q ? (size_t)(q - url) : strlen(url)) : 0;
    static const char *const skip_ext[] = { ".css", ".woff", ".woff2", ".ttf", ".otf", ".png",
                                            ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".ico", ".json" };
    bool skip = off || !data || len < 32 * 1024 || len >= UINT32_MAX;
    for (size_t i = 0; !skip && i < sizeof(skip_ext) / sizeof(skip_ext[0]); i++) {
        size_t el = strlen(skip_ext[i]);
        if (ul >= el && !memcmp(url + ul - el, skip_ext[i], el)) skip = true;
    }
    if (!skip) {
        size_t i = 0;
        while (i < len && (data[i] == ' ' || data[i] == '\n' || data[i] == '\r' || data[i] == '\t')) i++;
        if (i < len && (data[i] == '<' || data[i] == '@')) skip = true;
    }
    if (skip) {
        release(arg);
        return;
    }
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) {
        release(arg);
        return;
    }
    memcpy(copy, data, len);
    void *t = js_struct_index_announce(data, len);
    void *pc = g_pc_announce ? g_pc_announce(data, len) : NULL;
    release(arg);
    JSStructIndex *ix = js_struct_index_build(copy, len);
    js_struct_index_publish(t, ix);
    if (pc && ix)
        g_pc_start(pc, copy, len, ix);
    else {
        if (pc)
            g_pc_start(pc, NULL, 0, NULL);
        free(copy);
    }
}

void js_struct_index_stats(JSStructIndexStats *out)
{
    pthread_mutex_lock(&g_mu);
    *out = g_stats;
    pthread_mutex_unlock(&g_mu);
}
