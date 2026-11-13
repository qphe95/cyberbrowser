/*
 * css_counter_style.c - counter styles (CSS Counter Styles 3).
 *
 * A counter style is a row of g_cs: its system, symbols (one buffer of
 * NUL-ended strings, sym_off into it), additive weights, negative, prefix,
 * suffix, pad, range and fallback.  The predefined styles are written as
 * @counter-style rules (k_predefined) and read by the same parser as the
 * page's; the CJK long forms and ethiopic-numeric have algorithms of their
 * own (special).  Rows 0..g_npre-1 are the predefined, the page's after.
 */
#include "css_counter_style.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { CS_CYCLIC, CS_NUMERIC, CS_ALPHABETIC, CS_SYMBOLIC, CS_ADDITIVE, CS_FIXED, CS_SPECIAL };
enum { SP_NONE, SP_SIMP_INF, SP_SIMP_FORM, SP_TRAD_INF, SP_TRAD_FORM, SP_JA_INF, SP_JA_FORM, SP_KO_HANGUL,
       SP_KO_HANJA_INF, SP_KO_HANJA_FORM, SP_ETHIOPIC };
/* (which descriptors a rule set: what `extends` doesn't take from its base) */
enum { D_SYSTEM = 1, D_SYMBOLS = 2, D_ADDITIVE = 4, D_NEGATIVE = 8, D_PREFIX = 16, D_SUFFIX = 32, D_PAD = 64,
       D_RANGE = 128, D_FALLBACK = 256 };

enum { CS_MAX = 512, CS_SYMS = 64, CS_BUF = 1024, CS_RANGES = 8, CS_NAME = 64 };

typedef struct CStyle {
    char name[CS_NAME];
    char extends[CS_NAME];          /* system: extends <name> (resolved after the scan) */
    char fallback[CS_NAME];
    unsigned char system, special;
    unsigned char disc;             /* (1 disclosure-open, 2 disclosure-closed: its triangle by the flow) */
    bool range_auto;
    unsigned set;                   /* D_* */
    int first;                      /* fixed: the first symbol's value */
    int nsym, nadd, nrange, pad;
    unsigned short sym_off[CS_SYMS], add_off[CS_SYMS];
    long long add_w[CS_SYMS];
    long long lo[CS_RANGES], hi[CS_RANGES];
    unsigned short neg_off[2], prefix_off, suffix_off, pad_off;
    bool has_neg2;
    unsigned short used;
    char buf[CS_BUF];
} CStyle;

static CStyle g_cs[CS_MAX];
static int g_n, g_npre;

/* ---- text: symbols as written (strings, identifiers, CSS escapes) ---- */

static void put_utf8(char *out, size_t cap, size_t *o, uint32_t c)
{
    char t[4];
    int n;
    if (c < 0x80) { t[0] = (char)c; n = 1; }
    else if (c < 0x800) { t[0] = (char)(0xC0 | (c >> 6)); t[1] = (char)(0x80 | (c & 63)); n = 2; }
    else if (c < 0x10000) { t[0] = (char)(0xE0 | (c >> 12)); t[1] = (char)(0x80 | ((c >> 6) & 63)); t[2] = (char)(0x80 | (c & 63)); n = 3; }
    else { t[0] = (char)(0xF0 | (c >> 18)); t[1] = (char)(0x80 | ((c >> 12) & 63)); t[2] = (char)(0x80 | ((c >> 6) & 63)); t[3] = (char)(0x80 | (c & 63)); n = 4; }
    if (*o + (size_t)n >= cap) return;
    memcpy(out + *o, t, (size_t)n);
    *o += (size_t)n;
}

/* (a CSS escape at p, after its backslash: the character, p past it) */
static uint32_t css_escape(const char **pp, const char *end)
{
    const char *p = *pp;
    if (p >= end) return 0xFFFD;
    if (isxdigit((unsigned char)*p)) {
        uint32_t c = 0;
        int n = 0;
        while (p < end && n < 6 && isxdigit((unsigned char)*p)) {
            c = c * 16 + (uint32_t)(isdigit((unsigned char)*p) ? *p - '0' : (tolower((unsigned char)*p) - 'a' + 10));
            p++, n++;
        }
        if (p < end && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
        *pp = p;
        return c == 0 || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF) ? 0xFFFD : c;
    }
    uint32_t c = (unsigned char)*p;
    *pp = p + 1;
    return c;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (isspace((unsigned char)*p) || (*p == '/' && p + 1 < end && p[1] == '*'))) {
        if (*p == '/') {
            const char *c = strstr(p + 2, "*/");
            p = c && c < end ? c + 2 : end;
        } else {
            p++;
        }
    }
    return p;
}

/* One symbol at *pp: a string or an identifier (decoded into out); false
 * when there is none (an image, a number, punctuation). */
static bool read_symbol(const char **pp, const char *end, char *out, size_t cap)
{
    const char *p = skip_ws(*pp, end);
    size_t o = 0;
    out[0] = 0;
    if (p >= end) return false;
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        while (p < end && *p != q) {
            if (*p == '\\') {
                p++;
                if (p < end && *p == '\n') { p++; continue; }
                put_utf8(out, cap, &o, css_escape(&p, end));
                continue;
            }
            if (o + 1 < cap) out[o++] = *p;
            p++;
        }
        if (p >= end) return false;
        p++;
        out[o] = 0;
        *pp = p;
        return true;
    }
    /* an identifier (custom-ident) */
    const char *s = p;
    if (*p == '-') p++;
    if (p < end && (isalpha((unsigned char)*p) || *p == '_' || (unsigned char)*p >= 0x80 || *p == '\\' || (*p == '-' && p == s + 1))) {
        while (p < end && (isalnum((unsigned char)*p) || *p == '-' || *p == '_' || (unsigned char)*p >= 0x80 || *p == '\\')) {
            if (*p == '\\') {
                p++;
                put_utf8(out, cap, &o, css_escape(&p, end));
                continue;
            }
            if (o + 1 < cap) out[o++] = *p;
            p++;
        }
        out[o] = 0;
        if (p < end && *p == '(') return false;     /* (a function: an image) */
        *pp = p;
        return o > 0;
    }
    return false;
}

static bool read_int(const char **pp, const char *end, long long *v)
{
    const char *p = skip_ws(*pp, end);
    if (p >= end) return false;
    const char *s = p;
    if (*p == '+' || *p == '-') p++;
    if (p >= end || !isdigit((unsigned char)*p)) return false;
    while (p < end && isdigit((unsigned char)*p)) p++;
    if (p < end && (*p == '.' || *p == 'e' || *p == 'E' || isalpha((unsigned char)*p) || *p == '%')) return false;
    char tmp[32];
    size_t n = (size_t)(p - s) < sizeof tmp - 1 ? (size_t)(p - s) : sizeof tmp - 1;
    memcpy(tmp, s, n);
    tmp[n] = 0;
    *v = strtoll(tmp, NULL, 10);
    *pp = p;
    return true;
}

static bool word_is(const char **pp, const char *end, const char *w)
{
    const char *p = skip_ws(*pp, end);
    size_t n = strlen(w);
    if ((size_t)(end - p) < n || strncasecmp(p, w, n)) return false;
    if (p + n < end && (isalnum((unsigned char)p[n]) || p[n] == '-' || p[n] == '_')) return false;
    *pp = p + n;
    return true;
}

static bool at_end(const char *p, const char *end) { return skip_ws(p, end) >= end; }

/* (a string into the row's buffer: its offset, 0 when it doesn't fit; offset 0 is "") */
static unsigned short cs_store(CStyle *c, const char *s)
{
    size_t n = strlen(s);
    if (!c->used) { c->buf[0] = 0; c->used = 1; }
    if (c->used + n + 1 > CS_BUF) return 0;
    unsigned short o = c->used;
    memcpy(c->buf + o, s, n + 1);
    c->used = (unsigned short)(c->used + n + 1);
    return o;
}
#define CS_STR(c, off) ((c)->buf + (off))

/* ---- descriptors ---- */

static bool name_ok(const char *n)
{
    /* (none, and the CSS-wide keywords, are not names) */
    static const char *const bad[] = { "none", "inherit", "initial", "unset", "default", "revert", "revert-layer" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        if (!strcasecmp(n, bad[i])) return false;
    return n[0] != 0;
}

static void desc(CStyle *c, const char *name, size_t nl, const char *v, const char *end)
{
    char sym[256];
    const char *p = v;
#define IS(s) (nl == strlen(s) && !strncasecmp(name, s, nl))
    if (IS("system")) {
        static const char *const k[] = { "cyclic", "numeric", "alphabetic", "symbolic", "additive", "fixed" };
        for (int i = 0; i < 6; i++) {
            const char *q = p;
            if (!word_is(&q, end, k[i])) continue;
            int first = 1;
            if (i == CS_FIXED) {
                long long f;
                if (!at_end(q, end)) { if (!read_int(&q, end, &f)) return; first = (int)f; }
            }
            if (!at_end(q, end)) return;
            c->system = (unsigned char)i;
            c->first = first;
            c->set |= D_SYSTEM;
            return;
        }
        const char *q = p;
        if (word_is(&q, end, "extends")) {
            if (!read_symbol(&q, end, sym, sizeof sym) || !at_end(q, end) || !name_ok(sym)) return;
            q = skip_ws(p, end);
            if (*q == '"' || *q == '\'') return;
            snprintf(c->extends, sizeof c->extends, "%s", sym);
            c->set |= D_SYSTEM;
            return;
        }
        if (word_is(&q, end, "-cb-special")) { c->system = CS_SPECIAL; c->set |= D_SYSTEM; }
        return;
    }
    if (IS("symbols")) {
        int n = 0;
        unsigned short off[CS_SYMS];
        CStyle tmp = *c;
        while (!at_end(p, end)) {
            const char *q0 = skip_ws(p, end);
            bool quoted = *q0 == '"' || *q0 == '\'';
            if (n == CS_SYMS || !read_symbol(&p, end, sym, sizeof sym)) return;
            if (!quoted && strcasecmp(sym, "none") && !name_ok(sym)) return;   /* (a CSS-wide keyword is no symbol) */
            off[n++] = cs_store(&tmp, sym);
        }
        if (!n) return;
        *c = tmp;
        memcpy(c->sym_off, off, (size_t)n * sizeof off[0]);
        c->nsym = n;
        c->set |= D_SYMBOLS;
        return;
    }
    if (IS("additive-symbols")) {
        int n = 0;
        CStyle tmp = *c;
        long long w[CS_SYMS];
        unsigned short off[CS_SYMS];
        while (!at_end(p, end)) {
            if (n == CS_SYMS) return;
            long long x;
            bool got_w = read_int(&p, end, &x), got_s = read_symbol(&p, end, sym, sizeof sym);
            if (!got_w && got_s) got_w = read_int(&p, end, &x);
            if (!got_w || !got_s || x < 0) return;
            if (n && x >= w[n - 1]) return;      /* (weights strictly descending) */
            w[n] = x;
            off[n++] = cs_store(&tmp, sym);
            p = skip_ws(p, end);
            if (p < end) { if (*p != ',') return; p++; if (at_end(p, end)) return; }
        }
        if (!n) return;
        *c = tmp;
        memcpy(c->add_w, w, (size_t)n * sizeof w[0]);
        memcpy(c->add_off, off, (size_t)n * sizeof off[0]);
        c->nadd = n;
        c->set |= D_ADDITIVE;
        return;
    }
    if (IS("negative")) {
        char sym2[256];
        if (!read_symbol(&p, end, sym, sizeof sym)) return;
        bool two = !at_end(p, end);
        if (two && (!read_symbol(&p, end, sym2, sizeof sym2) || !at_end(p, end))) return;
        c->neg_off[0] = cs_store(c, sym);
        c->neg_off[1] = two ? cs_store(c, sym2) : 0;
        c->has_neg2 = two;
        c->set |= D_NEGATIVE;
        return;
    }
    if (IS("prefix") || IS("suffix")) {
        if (!read_symbol(&p, end, sym, sizeof sym) || !at_end(p, end)) return;
        if (IS("prefix")) { c->prefix_off = cs_store(c, sym); c->set |= D_PREFIX; }
        else { c->suffix_off = cs_store(c, sym); c->set |= D_SUFFIX; }
        return;
    }
    if (IS("pad")) {
        long long x;
        bool got_w = read_int(&p, end, &x), got_s = read_symbol(&p, end, sym, sizeof sym);
        if (!got_w && got_s) got_w = read_int(&p, end, &x);
        if (!got_w || !got_s || x < 0 || !at_end(p, end)) return;
        c->pad = (int)(x > 64 ? 64 : x);
        c->pad_off = cs_store(c, sym);
        c->set |= D_PAD;
        return;
    }
    if (IS("range")) {
        const char *q = p;
        if (word_is(&q, end, "auto") && at_end(q, end)) { c->range_auto = true; c->nrange = 0; c->set |= D_RANGE; return; }
        int n = 0;
        long long lo[CS_RANGES], hi[CS_RANGES];
        while (!at_end(p, end)) {
            if (n == CS_RANGES) return;
            long long a, b;
            if (word_is(&p, end, "infinite")) a = INT64_MIN; else if (!read_int(&p, end, &a)) return;
            if (word_is(&p, end, "infinite")) b = INT64_MAX; else if (!read_int(&p, end, &b)) return;
            if (a > b) return;
            lo[n] = a; hi[n++] = b;
            p = skip_ws(p, end);
            if (p < end) { if (*p != ',') return; p++; if (at_end(p, end)) return; }
        }
        if (!n) return;
        memcpy(c->lo, lo, (size_t)n * sizeof lo[0]);
        memcpy(c->hi, hi, (size_t)n * sizeof hi[0]);
        c->nrange = n;
        c->range_auto = false;
        c->set |= D_RANGE;
        return;
    }
    if (IS("fallback")) {
        const char *q = skip_ws(p, end);
        if (q < end && (*q == '"' || *q == '\'')) return;
        if (!read_symbol(&p, end, sym, sizeof sym) || !at_end(p, end) || !name_ok(sym)) return;
        snprintf(c->fallback, sizeof c->fallback, "%s", sym);
        c->set |= D_FALLBACK;
        return;
    }
#undef IS
}

/* Whether a rule's descriptors make a counter style (Counter Styles 3 3.1.1). */
static bool cs_valid(const CStyle *c)
{
    if (c->extends[0]) return !(c->set & (D_SYMBOLS | D_ADDITIVE));
    switch (c->system) {
    case CS_CYCLIC: case CS_FIXED: case CS_SYMBOLIC: return c->nsym >= 1 && (c->set & D_SYMBOLS);
    case CS_NUMERIC: case CS_ALPHABETIC: return c->nsym >= 2 && (c->set & D_SYMBOLS);
    case CS_ADDITIVE: return c->nadd >= 1 && (c->set & D_ADDITIVE);
    case CS_SPECIAL: return true;
    }
    return false;
}

static int cs_find_exact(const char *name)
{
    for (int i = g_n - 1; i >= 0; i--)
        if (!strcmp(g_cs[i].name, name)) return i;
    return -1;
}

/* The style a name means: the page's rule of that name (exact), else a
 * predefined one (any case); -1: none. */
static int cs_lookup(const char *name)
{
    /* (a predefined name in any case: that name in lower case, a page's rule of it first) */
    for (int k = g_npre - 1; k >= 0; k--)
        if (!strcasecmp(g_cs[k].name, name)) {
            int j = cs_find_exact(g_cs[k].name);
            return j >= 0 ? j : k;
        }
    return cs_find_exact(name);
}

/* One @counter-style block: its name and body text. */
static void cs_rule(const char *name, const char *body, const char *bend, bool predefined)
{
    if (!name_ok(name) || g_n >= CS_MAX - 1) return;
    /* (these can't be redefined) */
    static const char *const fixed_names[] = { "decimal", "disc", "square", "circle", "disclosure-open", "disclosure-closed" };
    if (!predefined)
        for (size_t i = 0; i < sizeof fixed_names / sizeof fixed_names[0]; i++)
            if (!strcasecmp(name, fixed_names[i])) return;
    CStyle *c = &g_cs[g_n];
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "%s", name);
    c->system = CS_SYMBOLIC;
    c->range_auto = true;
    c->first = 1;
    c->buf[0] = 0;
    c->used = 1;
    c->neg_off[0] = cs_store(c, "-");
    const char *p = body;
    while (p < bend) {
        p = skip_ws(p, bend);
        const char *ns = p;
        while (p < bend && *p != ':' && *p != ';') p++;
        const char *ne = p;
        while (ne > ns && isspace((unsigned char)ne[-1])) ne--;
        if (p >= bend || *p == ';') { p++; continue; }
        const char *vs = ++p;
        int depth = 0;
        char q = 0;
        while (p < bend && (q || depth || *p != ';')) {
            if (q) { if (*p == '\\' && p + 1 < bend) p++; else if (*p == q) q = 0; }
            else if (*p == '"' || *p == '\'') q = *p;
            else if (*p == '(') depth++;
            else if (*p == ')' && depth) depth--;
            p++;
        }
        const char *ve = p;
        while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
        /* (!important makes a descriptor invalid) */
        if (ve - vs >= 10 && memchr(vs, '!', (size_t)(ve - vs))) { p++; continue; }
        desc(c, ns, (size_t)(ne - ns), vs, ve);
        p++;
    }
    if (!(c->set & D_SYSTEM)) c->system = CS_SYMBOLIC;
    if (predefined && c->system == CS_SPECIAL) {
        static const struct { const char *n; unsigned char sp; } k[] = {
            { "simp-chinese-informal", SP_SIMP_INF }, { "simp-chinese-formal", SP_SIMP_FORM },
            { "trad-chinese-informal", SP_TRAD_INF }, { "trad-chinese-formal", SP_TRAD_FORM },
            { "japanese-informal", SP_JA_INF }, { "japanese-formal", SP_JA_FORM },
            { "korean-hangul-formal", SP_KO_HANGUL }, { "korean-hanja-informal", SP_KO_HANJA_INF },
            { "korean-hanja-formal", SP_KO_HANJA_FORM }, { "ethiopic-numeric", SP_ETHIOPIC } };
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
            if (!strcmp(name, k[i].n)) c->special = k[i].sp;
    }
    if (!cs_valid(c)) return;
    g_n++;
}

/* (`extends`: the base's descriptors under the ones the rule set; a cycle or
 * an unknown base makes it extend decimal) */
static void cs_resolve(int i, int depth)
{
    CStyle *c = &g_cs[i];
    if (!c->extends[0]) return;
    int b = depth < 16 ? cs_lookup(c->extends) : -1;
    if (b == i) b = -1;
    /* (i in a cycle: decimal its base) */
    for (int j = b, k = 0; j >= 0 && k < 64 && g_cs[j].extends[0]; j = cs_lookup(g_cs[j].extends), k++)
        if (j == i) { b = -1; break; }
    if (b >= 0 && g_cs[b].extends[0]) cs_resolve(b, depth + 1);
    if (b < 0 || g_cs[b].extends[0]) b = cs_lookup("decimal");
    CStyle own = *c, *base = &g_cs[b];
    *c = *base;
    memcpy(c->name, own.name, sizeof c->name);
    c->extends[0] = 0;
    c->set = base->set | own.set;
    if (own.set & D_SYMBOLS) c->disc = 0;
    /* (strings: the base's buffer, the rule's own appended) */
#define TAKE(flag, field) if (own.set & flag) c->field = cs_store(c, CS_STR(&own, own.field))
    TAKE(D_PREFIX, prefix_off);
    TAKE(D_SUFFIX, suffix_off);
    if (own.set & D_NEGATIVE) {
        c->neg_off[0] = cs_store(c, CS_STR(&own, own.neg_off[0]));
        c->neg_off[1] = own.has_neg2 ? cs_store(c, CS_STR(&own, own.neg_off[1])) : 0;
        c->has_neg2 = own.has_neg2;
    }
    if (own.set & D_PAD) { c->pad = own.pad; c->pad_off = cs_store(c, CS_STR(&own, own.pad_off)); }
#undef TAKE
    if (own.set & D_RANGE) {
        c->range_auto = own.range_auto;
        c->nrange = own.nrange;
        memcpy(c->lo, own.lo, sizeof c->lo);
        memcpy(c->hi, own.hi, sizeof c->hi);
    }
    if (own.set & D_FALLBACK) memcpy(c->fallback, own.fallback, sizeof c->fallback);
}

/* The predefined styles (Counter Styles 3 section 6 and 7). */
static const char k_predefined[] =
    "@counter-style decimal { system: numeric; symbols: '0' '1' '2' '3' '4' '5' '6' '7' '8' '9'; }"
    "@counter-style decimal-leading-zero { system: extends decimal; pad: 2 '0'; }"
    "@counter-style arabic-indic { system: numeric; symbols: '\\660' '\\661' '\\662' '\\663' '\\664' '\\665' '\\666' '\\667' '\\668' '\\669'; }"
    "@counter-style armenian { system: additive; range: 1 9999; additive-symbols: 9000 \\554, 8000 \\553, 7000 \\552,"
    " 6000 \\551, 5000 \\550, 4000 \\54F, 3000 \\54E, 2000 \\54D, 1000 \\54C, 900 \\54B, 800 \\54A, 700 \\549, 600 \\548,"
    " 500 \\547, 400 \\546, 300 \\545, 200 \\544, 100 \\543, 90 \\542, 80 \\541, 70 \\540, 60 \\53F, 50 \\53E, 40 \\53D,"
    " 30 \\53C, 20 \\53B, 10 \\53A, 9 \\539, 8 \\538, 7 \\537, 6 \\536, 5 \\535, 4 \\534, 3 \\533, 2 \\532, 1 '\\531'; }"
    "@counter-style upper-armenian { system: extends armenian; }"
    "@counter-style lower-armenian { system: additive; range: 1 9999; additive-symbols: 9000 \\584, 8000 \\583, 7000 \\582,"
    " 6000 \\581, 5000 \\580, 4000 \\57F, 3000 \\57E, 2000 \\57D, 1000 \\57C, 900 \\57B, 800 \\57A, 700 \\579, 600 \\578,"
    " 500 \\577, 400 \\576, 300 \\575, 200 \\574, 100 \\573, 90 \\572, 80 \\571, 70 \\570, 60 \\56F, 50 \\56E, 40 \\56D,"
    " 30 \\56C, 20 \\56B, 10 \\56A, 9 \\569, 8 \\568, 7 \\567, 6 \\566, 5 \\565, 4 \\564, 3 \\563, 2 \\562, 1 '\\561'; }"
    "@counter-style bengali { system: numeric; symbols: '\\9E6' '\\9E7' '\\9E8' '\\9E9' '\\9EA' '\\9EB' '\\9EC' '\\9ED' '\\9EE' '\\9EF'; }"
    "@counter-style cambodian { system: numeric; symbols: '\\17E0' '\\17E1' '\\17E2' '\\17E3' '\\17E4' '\\17E5' '\\17E6' '\\17E7' '\\17E8' '\\17E9'; }"
    "@counter-style khmer { system: extends cambodian; }"
    "@counter-style cjk-decimal { system: numeric; range: 0 infinite; symbols: '\\3007' '\\4E00' '\\4E8C' '\\4E09' '\\56DB' '\\4E94'"
    " '\\516D' '\\4E03' '\\516B' '\\4E5D'; suffix: '\\3001'; }"
    "@counter-style devanagari { system: numeric; symbols: '\\966' '\\967' '\\968' '\\969' '\\96A' '\\96B' '\\96C' '\\96D' '\\96E' '\\96F'; }"
    "@counter-style georgian { system: additive; range: 1 19999; additive-symbols: 10000 \\10F5, 9000 \\10F0, 8000 \\10EF,"
    " 7000 \\10F4, 6000 \\10EE, 5000 \\10ED, 4000 \\10EC, 3000 \\10EB, 2000 \\10EA, 1000 \\10E9, 900 \\10E8, 800 \\10E7,"
    " 700 \\10E6, 600 \\10E5, 500 \\10E4, 400 \\10F3, 300 \\10E2, 200 \\10E1, 100 \\10E0, 90 \\10DF, 80 \\10DE, 70 \\10DD,"
    " 60 \\10F2, 50 \\10DC, 40 \\10DB, 30 \\10DA, 20 \\10D9, 10 \\10D8, 9 \\10D7, 8 \\10F1, 7 \\10D6, 6 \\10D5, 5 \\10D4,"
    " 4 \\10D3, 3 \\10D2, 2 \\10D1, 1 '\\10D0'; }"
    "@counter-style gujarati { system: numeric; symbols: '\\AE6' '\\AE7' '\\AE8' '\\AE9' '\\AEA' '\\AEB' '\\AEC' '\\AED' '\\AEE' '\\AEF'; }"
    "@counter-style gurmukhi { system: numeric; symbols: '\\A66' '\\A67' '\\A68' '\\A69' '\\A6A' '\\A6B' '\\A6C' '\\A6D' '\\A6E' '\\A6F'; }"
    "@counter-style hebrew { system: additive; range: 1 10999; additive-symbols: 10000 '\\5D9\\5F3', 9000 '\\5D8\\5F3',"
    " 8000 '\\5D7\\5F3', 7000 '\\5D6\\5F3', 6000 '\\5D5\\5F3', 5000 '\\5D4\\5F3', 4000 '\\5D3\\5F3', 3000 '\\5D2\\5F3',"
    " 2000 '\\5D1\\5F3', 1000 '\\5D0\\5F3', 400 \\5EA, 300 \\5E9, 200 \\5E8, 100 \\5E7, 90 \\5E6, 80 \\5E4, 70 \\5E2,"
    " 60 \\5E1, 50 \\5E0, 40 \\5DE, 30 \\5DC, 20 \\5DB, 19 '\\5D9\\5D8', 18 '\\5D9\\5D7', 17 '\\5D9\\5D6', 16 '\\5D8\\5D6',"
    " 15 '\\5D8\\5D5', 10 \\5D9, 9 \\5D8, 8 \\5D7, 7 \\5D6, 6 \\5D5, 5 \\5D4, 4 \\5D3, 3 \\5D2, 2 \\5D1, 1 '\\5D0'; }"
    "@counter-style kannada { system: numeric; symbols: '\\CE6' '\\CE7' '\\CE8' '\\CE9' '\\CEA' '\\CEB' '\\CEC' '\\CED' '\\CEE' '\\CEF'; }"
    "@counter-style lao { system: numeric; symbols: '\\ED0' '\\ED1' '\\ED2' '\\ED3' '\\ED4' '\\ED5' '\\ED6' '\\ED7' '\\ED8' '\\ED9'; }"
    "@counter-style malayalam { system: numeric; symbols: '\\D66' '\\D67' '\\D68' '\\D69' '\\D6A' '\\D6B' '\\D6C' '\\D6D' '\\D6E' '\\D6F'; }"
    "@counter-style mongolian { system: numeric; symbols: '\\1810' '\\1811' '\\1812' '\\1813' '\\1814' '\\1815' '\\1816' '\\1817' '\\1818' '\\1819'; }"
    "@counter-style myanmar { system: numeric; symbols: '\\1040' '\\1041' '\\1042' '\\1043' '\\1044' '\\1045' '\\1046' '\\1047' '\\1048' '\\1049'; }"
    "@counter-style oriya { system: numeric; symbols: '\\B66' '\\B67' '\\B68' '\\B69' '\\B6A' '\\B6B' '\\B6C' '\\B6D' '\\B6E' '\\B6F'; }"
    "@counter-style persian { system: numeric; symbols: '\\6F0' '\\6F1' '\\6F2' '\\6F3' '\\6F4' '\\6F5' '\\6F6' '\\6F7' '\\6F8' '\\6F9'; }"
    "@counter-style lower-roman { system: additive; range: 1 3999; additive-symbols: 1000 m, 900 cm, 500 d, 400 cd,"
    " 100 c, 90 xc, 50 l, 40 xl, 10 x, 9 ix, 5 v, 4 iv, 1 i; }"
    "@counter-style upper-roman { system: additive; range: 1 3999; additive-symbols: 1000 M, 900 CM, 500 D, 400 CD,"
    " 100 C, 90 XC, 50 L, 40 XL, 10 X, 9 IX, 5 V, 4 IV, 1 I; }"
    "@counter-style tamil { system: numeric; symbols: '\\BE6' '\\BE7' '\\BE8' '\\BE9' '\\BEA' '\\BEB' '\\BEC' '\\BED' '\\BEE' '\\BEF'; }"
    "@counter-style telugu { system: numeric; symbols: '\\C66' '\\C67' '\\C68' '\\C69' '\\C6A' '\\C6B' '\\C6C' '\\C6D' '\\C6E' '\\C6F'; }"
    "@counter-style thai { system: numeric; symbols: '\\E50' '\\E51' '\\E52' '\\E53' '\\E54' '\\E55' '\\E56' '\\E57' '\\E58' '\\E59'; }"
    "@counter-style tibetan { system: numeric; symbols: '\\F20' '\\F21' '\\F22' '\\F23' '\\F24' '\\F25' '\\F26' '\\F27' '\\F28' '\\F29'; }"
    "@counter-style lower-alpha { system: alphabetic; symbols: a b c d e f g h i j k l m n o p q r s t u v w x y z; }"
    "@counter-style lower-latin { system: extends lower-alpha; }"
    "@counter-style upper-alpha { system: alphabetic; symbols: A B C D E F G H I J K L M N O P Q R S T U V W X Y Z; }"
    "@counter-style upper-latin { system: extends upper-alpha; }"
    "@counter-style lower-greek { system: alphabetic; symbols: '\\3B1' '\\3B2' '\\3B3' '\\3B4' '\\3B5' '\\3B6' '\\3B7' '\\3B8' '\\3B9' '\\3BA'"
    " '\\3BB' '\\3BC' '\\3BD' '\\3BE' '\\3BF' '\\3C0' '\\3C1' '\\3C3' '\\3C4' '\\3C5' '\\3C6' '\\3C7' '\\3C8' '\\3C9'; }"
    "@counter-style hiragana { system: alphabetic; symbols: '\\3042' '\\3044' '\\3046' '\\3048' '\\304A' '\\304B' '\\304D' '\\304F' '\\3051'"
    " '\\3053' '\\3055' '\\3057' '\\3059' '\\305B' '\\305D' '\\305F' '\\3061' '\\3064' '\\3066' '\\3068' '\\306A' '\\306B' '\\306C' '\\306D' '\\306E' '\\306F'"
    " '\\3072' '\\3075' '\\3078' '\\307B' '\\307E' '\\307F' '\\3080' '\\3081' '\\3082' '\\3084' '\\3086' '\\3088' '\\3089' '\\308A' '\\308B' '\\308C' '\\308D'"
    " '\\308F' '\\3090' '\\3091' '\\3092' '\\3093'; suffix: '\\3001'; }"
    "@counter-style hiragana-iroha { system: alphabetic; symbols: '\\3044' '\\308D' '\\306F' '\\306B' '\\307B' '\\3078' '\\3068' '\\3061'"
    " '\\308A' '\\306C' '\\308B' '\\3092' '\\308F' '\\304B' '\\3088' '\\305F' '\\308C' '\\305D' '\\3064' '\\306D' '\\306A' '\\3089' '\\3080' '\\3046' '\\3090'"
    " '\\306E' '\\304A' '\\304F' '\\3084' '\\307E' '\\3051' '\\3075' '\\3053' '\\3048' '\\3066' '\\3042' '\\3055' '\\304D' '\\3086' '\\3081' '\\307F' '\\3057'"
    " '\\3091' '\\3072' '\\3082' '\\305B' '\\3059'; suffix: '\\3001'; }"
    "@counter-style katakana { system: alphabetic; symbols: '\\30A2' '\\30A4' '\\30A6' '\\30A8' '\\30AA' '\\30AB' '\\30AD' '\\30AF' '\\30B1'"
    " '\\30B3' '\\30B5' '\\30B7' '\\30B9' '\\30BB' '\\30BD' '\\30BF' '\\30C1' '\\30C4' '\\30C6' '\\30C8' '\\30CA' '\\30CB' '\\30CC' '\\30CD' '\\30CE' '\\30CF'"
    " '\\30D2' '\\30D5' '\\30D8' '\\30DB' '\\30DE' '\\30DF' '\\30E0' '\\30E1' '\\30E2' '\\30E4' '\\30E6' '\\30E8' '\\30E9' '\\30EA' '\\30EB' '\\30EC' '\\30ED'"
    " '\\30EF' '\\30F0' '\\30F1' '\\30F2' '\\30F3'; suffix: '\\3001'; }"
    "@counter-style katakana-iroha { system: alphabetic; symbols: '\\30A4' '\\30ED' '\\30CF' '\\30CB' '\\30DB' '\\30D8' '\\30C8' '\\30C1'"
    " '\\30EA' '\\30CC' '\\30EB' '\\30F2' '\\30EF' '\\30AB' '\\30E8' '\\30BF' '\\30EC' '\\30BD' '\\30C4' '\\30CD' '\\30CA' '\\30E9' '\\30E0' '\\30A6' '\\30F0'"
    " '\\30CE' '\\30AA' '\\30AF' '\\30E4' '\\30DE' '\\30B1' '\\30D5' '\\30B3' '\\30A8' '\\30C6' '\\30A2' '\\30B5' '\\30AD' '\\30E6' '\\30E1' '\\30DF' '\\30B7'"
    " '\\30F1' '\\30D2' '\\30E2' '\\30BB' '\\30B9'; suffix: '\\3001'; }"
    "@counter-style cjk-earthly-branch { system: fixed; symbols: '\\5B50' '\\4E11' '\\5BC5' '\\536F' '\\8FB0' '\\5DF3' '\\5348'"
    " '\\672A' '\\7533' '\\9149' '\\620C' '\\4EA5'; suffix: '\\3001'; fallback: cjk-decimal; }"
    "@counter-style cjk-heavenly-stem { system: fixed; symbols: '\\7532' '\\4E59' '\\4E19' '\\4E01' '\\620A' '\\5DF1' '\\5E9A'"
    " '\\8F9B' '\\58EC' '\\7678'; suffix: '\\3001'; fallback: cjk-decimal; }"
    "@counter-style disc { system: cyclic; symbols: '\\2022'; suffix: ' '; }"
    "@counter-style circle { system: cyclic; symbols: '\\25E6'; suffix: ' '; }"
    "@counter-style square { system: cyclic; symbols: '\\25AA'; suffix: ' '; }"
    "@counter-style disclosure-open { system: cyclic; symbols: '\\25BE'; suffix: ' '; }"
    "@counter-style disclosure-closed { system: cyclic; symbols: '\\25B8'; suffix: ' '; }"
    "@counter-style simp-chinese-informal { system: -cb-special; range: -9999999999999999 9999999999999999; negative: \\8D1F; suffix: '\\3001'; fallback: cjk-decimal; }"
    "@counter-style simp-chinese-formal { system: -cb-special; range: -9999999999999999 9999999999999999; negative: \\8D1F; suffix: '\\3001'; fallback: cjk-decimal; }"
    "@counter-style trad-chinese-informal { system: -cb-special; range: -9999999999999999 9999999999999999; negative: \\8CA0; suffix: '\\3001'; fallback: cjk-decimal; }"
    "@counter-style trad-chinese-formal { system: -cb-special; range: -9999999999999999 9999999999999999; negative: \\8CA0; suffix: '\\3001'; fallback: cjk-decimal; }"
    "@counter-style japanese-informal { system: -cb-special; range: -9999999999999999 9999999999999999; negative: '\\30DE\\30A4\\30CA\\30B9'; suffix: '\\3001'; fallback: cjk-decimal; }"
    "@counter-style japanese-formal { system: -cb-special; range: -9999999999999999 9999999999999999; negative: '\\30DE\\30A4\\30CA\\30B9'; suffix: '\\3001'; fallback: cjk-decimal; }"
    "@counter-style korean-hangul-formal { system: -cb-special; range: -9999999999999999 9999999999999999; negative: '\\B9C8\\C774\\B108\\C2A4  '; suffix: ', '; fallback: cjk-decimal; }"
    "@counter-style korean-hanja-informal { system: -cb-special; range: -9999999999999999 9999999999999999; negative: '\\B9C8\\C774\\B108\\C2A4  '; suffix: ', '; fallback: cjk-decimal; }"
    "@counter-style korean-hanja-formal { system: -cb-special; range: -9999999999999999 9999999999999999; negative: '\\B9C8\\C774\\B108\\C2A4  '; suffix: ', '; fallback: cjk-decimal; }"
    "@counter-style ethiopic-numeric { system: -cb-special; range: 1 infinite; suffix: '/ '; }";

/* (is at inside an @media or @supports block of css whose condition does not hold? as laid out, 800 by 600) */
extern bool css_media_query_matches(const char *query, double viewport_width, double viewport_height);
extern bool css_supports_condition(const char *cond);
static bool cs_in_false_block(const char *css, const char *at)
{
    bool st[64];
    int d = 0;
    char quote = 0;
    for (const char *p = css; p < at; p++) {
        if (quote) { if (*p == '\\' && p + 1 < at) p++; else if (*p == quote) quote = 0; continue; }
        if (*p == '"' || *p == '\'') { quote = *p; continue; }
        if (*p == '/' && p + 1 < at && p[1] == '*') { const char *e = strstr(p + 2, "*/"); if (!e || e >= at) return false; p = e + 1; continue; }
        if (*p == '@' && (!strncasecmp(p, "@media", 6) || !strncasecmp(p, "@supports", 9))) {
            bool media = p[1] == 'm' || p[1] == 'M';
            const char *q = p + (media ? 6 : 9), *b = q;
            while (b < at && *b != '{' && *b != ';') b++;
            if (b >= at || *b != '{') continue;
            char cond[512];
            size_t n = (size_t)(b - q) < sizeof cond - 1 ? (size_t)(b - q) : sizeof cond - 1;
            memcpy(cond, q, n);
            cond[n] = 0;
            bool on = media ? css_media_query_matches(cond, 800.0, 600.0) : css_supports_condition(cond);
            if (d < 64) st[d] = !on;
            d++;
            p = b;
            continue;
        }
        if (*p == '{') { if (d < 64) st[d] = false; d++; }
        else if (*p == '}' && d > 0) d--;
    }
    for (int k = 0; k < d && k < 64; k++) if (st[k]) return true;
    return false;
}

/* @counter-style rules in css text (the predefined ones too). */
static void cs_scan(const char *css, size_t len, bool predefined)
{
    const char *end = css + len, *p = css;
    while (p < end) {
        const char *at = strstr(p, "@counter-style");
        if (!at || at >= end) break;
        const char *q = at + 14;
        p = q;
        if (q < end && !isspace((unsigned char)*q) && *q != '/') continue;
        char name[CS_NAME * 2];
        const char *n0 = skip_ws(q, end);
        if (n0 < end && (*n0 == '"' || *n0 == '\'')) continue;      /* (a string is no name) */
        if (!read_symbol(&q, end, name, sizeof name)) continue;
        q = skip_ws(q, end);
        if (q >= end || *q != '{') continue;
        const char *body = q + 1, *b = body;
        int depth = 1;
        char quote = 0;
        while (b < end && depth) {
            if (quote) { if (*b == '\\' && b + 1 < end) b++; else if (*b == quote) quote = 0; }
            else if (*b == '"' || *b == '\'') quote = *b;
            else if (*b == '{') depth++;
            else if (*b == '}') depth--;
            if (depth) b++;
        }
        if (predefined || !cs_in_false_block(css, at)) cs_rule(name, body, b, predefined);
        p = b < end ? b + 1 : end;
    }
}

static void cs_init(void)
{
    if (g_npre) return;
    g_n = 0;
    cs_scan(k_predefined, sizeof k_predefined - 1, true);
    for (int i = 0; i < g_n; i++) {
        if (!strcmp(g_cs[i].name, "disclosure-open")) g_cs[i].disc = 1;
        else if (!strcmp(g_cs[i].name, "disclosure-closed")) g_cs[i].disc = 2;
    }
    for (int i = 0; i < g_n; i++) cs_resolve(i, 0);
    g_npre = g_n;
}

void cstyle_forget(void)
{
    cs_init();
    g_n = g_npre;
}

void cstyle_scan_css(const char *css, size_t len)
{
    cs_init();
    int from = g_n;
    cs_scan(css, len, false);
    /* (those in an extends cycle: each extends decimal, found before any is resolved) */
    unsigned char cyc[256] = { 0 };
    for (int i = from; i < g_n && i - from < 256; i++)
        for (int j = g_cs[i].extends[0] ? cs_lookup(g_cs[i].extends) : -1, k = 0; j >= 0 && k < 64 && g_cs[j].extends[0]; j = cs_lookup(g_cs[j].extends), k++)
            if (j == i) { cyc[i - from] = 1; break; }
    for (int i = from; i < g_n && i - from < 256; i++) if (cyc[i - from]) snprintf(g_cs[i].extends, sizeof g_cs[i].extends, "%s", "decimal");
    for (int i = from; i < g_n; i++) cs_resolve(i, 0);
}

bool cstyle_known(const char *style)
{
    cs_init();
    return cs_lookup(style) >= 0;
}

/* ---- the algorithms ---- */

typedef struct Out { char *s; size_t cap, n; bool ok; } Out;
static void out_add(Out *o, const char *t)
{
    size_t l = strlen(t);
    if (o->n + l + 1 > o->cap) { o->ok = false; return; }
    memcpy(o->s + o->n, t, l + 1);
    o->n += l;
}
/* (prepend) */
static void out_pre(Out *o, const char *t)
{
    size_t l = strlen(t);
    if (o->n + l + 1 > o->cap) { o->ok = false; return; }
    memmove(o->s + l, o->s, o->n + 1);
    memcpy(o->s, t, l);
    o->n += l;
}

static size_t cps(const char *s)
{
    /* (grapheme clusters, near enough: code points less the combining marks) */
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = *p;
        int l = 1;
        if (cp >= 0xF0 && p[1] && p[2] && p[3]) { cp = ((cp & 7u) << 18) | ((p[1] & 63u) << 12) | ((p[2] & 63u) << 6) | (p[3] & 63u); l = 4; }
        else if (cp >= 0xE0 && p[1] && p[2]) { cp = ((cp & 15u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u); l = 3; }
        else if (cp >= 0xC0 && p[1]) { cp = ((cp & 31u) << 6) | (p[1] & 63u); l = 2; }
        bool mark = (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
                    (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0x200D || (cp >= 0xFE00 && cp <= 0xFE0F);
        if (!mark) n++;
        p += l;
    }
    return n;
}

/* CJK long forms (Counter Styles 3 7.1.2) for 0 < v <= 9999. */
static void cjk(const CStyle *c, long long v, Out *o)
{
    static const char *const d_simp_inf[] = { "\xe9\x9b\xb6", "\xe4\xb8\x80", "\xe4\xba\x8c", "\xe4\xb8\x89", "\xe5\x9b\x9b", "\xe4\xba\x94", "\xe5\x85\xad", "\xe4\xb8\x83", "\xe5\x85\xab", "\xe4\xb9\x9d" };
    static const char *const d_simp_form[] = { "\xe9\x9b\xb6", "\xe5\xa3\xb9", "\xe8\xb4\xb0", "\xe5\x8f\x81", "\xe8\x82\x86", "\xe4\xbc\x8d", "\xe9\x99\x86", "\xe6\x9f\x92", "\xe6\x8d\x8c", "\xe7\x8e\x96" };
    static const char *const d_trad_form[] = { "\xe9\x9b\xb6", "\xe5\xa3\xb9", "\xe8\xb2\xb3", "\xe5\x8f\x83", "\xe8\x82\x86", "\xe4\xbc\x8d", "\xe9\x99\xb8", "\xe6\x9f\x92", "\xe6\x8d\x8c", "\xe7\x8e\x96" };
    static const char *const d_ja_inf[] = { "\xe3\x80\x87", "\xe4\xb8\x80", "\xe4\xba\x8c", "\xe4\xb8\x89", "\xe5\x9b\x9b", "\xe4\xba\x94", "\xe5\x85\xad", "\xe4\xb8\x83", "\xe5\x85\xab", "\xe4\xb9\x9d" };
    static const char *const d_ja_form[] = { "\xe9\x9b\xb6", "\xe5\xa3\xb1", "\xe5\xbc\x90", "\xe5\x8f\x82", "\xe5\x9b\x9b", "\xe4\xbc\x8d", "\xe5\x85\xad", "\xe4\xb8\x83", "\xe5\x85\xab", "\xe4\xb9\x9d" };
    static const char *const d_ko_hangul[] = { "\xec\x98\x81", "\xec\x9d\xbc", "\xec\x9d\xb4", "\xec\x82\xbc", "\xec\x82\xac", "\xec\x98\xa4", "\xec\x9c\xa1", "\xec\xb9\xa0", "\xed\x8c\x94", "\xea\xb5\xac" };
    static const char *const d_ko_hanja_form[] = { "\xe9\x9b\xb6", "\xe5\xa3\xb9", "\xe8\xb2\xb3", "\xe5\x8f\x83", "\xe5\x9b\x9b", "\xe4\xba\x94", "\xe5\x85\xad", "\xe4\xb8\x83", "\xe5\x85\xab", "\xe4\xb9\x9d" };
    static const char *const m_inf[] = { "\xe5\x8d\x81", "\xe7\x99\xbe", "\xe5\x8d\x83" };            /* 十百千 */
    static const char *const m_form_cn[] = { "\xe6\x8b\xbe", "\xe4\xbd\xb0", "\xe4\xbb\x9f" };       /* 拾佰仟 */
    static const char *const m_ja_form[] = { "\xe6\x8b\xbe", "\xe7\x99\xbe", "\xe9\x98\xa1" };       /* 拾百阡 */
    static const char *const m_ko_hangul[] = { "\xec\x8b\xad", "\xeb\xb0\xb1", "\xec\xb2\x9c" };     /* 십백천 */
    static const char *const m_ko_hanja_form[] = { "\xe6\x8b\xbe", "\xe7\x99\xbe", "\xe4\xbb\x9f" }; /* 拾百仟 */
    const char *const *d = d_simp_inf, *const *m = m_inf;
    bool chinese = false, drop_one = false, informal_ten = false;
    switch (c->special) {
    case SP_SIMP_INF: d = d_simp_inf; m = m_inf; chinese = informal_ten = true; break;
    case SP_SIMP_FORM: d = d_simp_form; m = m_form_cn; chinese = true; break;
    case SP_TRAD_INF: d = d_simp_inf; m = m_inf; chinese = informal_ten = true; break;
    case SP_TRAD_FORM: d = d_trad_form; m = m_form_cn; chinese = true; break;
    case SP_JA_INF: d = d_ja_inf; m = m_inf; drop_one = true; break;
    case SP_JA_FORM: d = d_ja_form; m = m_ja_form; break;
    case SP_KO_HANGUL: d = d_ko_hangul; m = m_ko_hangul; break;
    case SP_KO_HANJA_INF: d = d_simp_inf; m = m_inf; drop_one = true; break;
    case SP_KO_HANJA_FORM: d = d_ko_hanja_form; m = m_ko_hanja_form; break;
    }
    if (v == 0) { out_add(o, d[0]); return; }
    int dig[4] = { (int)(v / 1000 % 10), (int)(v / 100 % 10), (int)(v / 10 % 10), (int)(v % 10) };
    bool zero_pending = false, any = false;
    for (int i = 0; i < 4; i++) {
        int x = dig[i], place = 3 - i;      /* (3 thousands .. 0 ones) */
        if (x == 0) {
            if (chinese && any) zero_pending = true;
            continue;
        }
        if (zero_pending) { out_add(o, d[0]); zero_pending = false; }
        /* (informal: 十 alone for 10-19; japanese-informal and korean-hanja-informal drop a 1 before any marker) */
        bool skip_digit = x == 1 && place > 0 && (drop_one || (informal_ten && place == 1 && !any));
        if (!skip_digit) out_add(o, d[x]);
        if (place > 0) out_add(o, m[place - 1]);
        any = true;
    }
}

/* CJK long forms past 9999: four-digit groups, each with its group marker
 * (Chinese: one zero before a group with leading zeros or after empty
 * ones; Korean: groups apart by a space; korean-hanja-informal: no one
 * before the ten-thousands marker) */
static void cjk_ext(const CStyle *c, long long v, Out *o)
{
    if (v <= 9999) { cjk(c, v, o); return; }
    static const char *const g_simp[] = { "\xe4\xb8\x87", "\xe4\xba\xbf", "\xe4\xb8\x87\xe4\xba\xbf" };   /* (wan, yi, wan-yi) */
    static const char *const g_trad[] = { "\xe8\x90\xac", "\xe5\x84\x84", "\xe5\x85\x86" };   /* (wan, yi, zhao) */
    static const char *const g_ja_inf[] = { "\xe4\xb8\x87", "\xe5\x84\x84", "\xe5\x85\x86" };   /* (man, oku, cho) */
    static const char *const g_ko[] = { "\xeb\xa7\x8c", "\xec\x96\xb5", "\xec\xa1\xb0" };   /* (man, eok, jo) */
    const char *const *gm = g_trad;
    bool chinese = false, korean = false;
    switch (c->special) {
    case SP_SIMP_INF: case SP_SIMP_FORM: gm = g_simp; chinese = true; break;
    case SP_TRAD_INF: case SP_TRAD_FORM: gm = g_trad; chinese = true; break;
    case SP_JA_INF: gm = g_ja_inf; break;
    case SP_JA_FORM: gm = g_trad; break;
    case SP_KO_HANGUL: gm = g_ko; korean = true; break;
    case SP_KO_HANJA_INF: case SP_KO_HANJA_FORM: gm = g_trad; korean = true; break;
    }
    int g[4];
    for (int i = 0; i < 4; i++) { g[i] = (int)(v % 10000); v /= 10000; }
    int top = 3;
    while (top > 0 && !g[top]) top--;
    bool zero_pending = false, first = true;
    char buf[128];
    for (int i = top; i >= 0; i--) {
        if (!g[i]) { if (chinese) zero_pending = true; continue; }
        if (chinese && i < top && g[i] < 1000) zero_pending = true;
        if (zero_pending) { out_add(o, "\xe9\x9b\xb6"); zero_pending = false; }
        if (korean && !first) out_add(o, " ");
        if (!(c->special == SP_KO_HANJA_INF && i == 1 && g[i] == 1)) {
            Out t = { buf, sizeof buf, 0, true };
            buf[0] = 0;
            cjk(c, g[i], &t);
            out_add(o, buf);
        }
        if (i > 0) out_add(o, gm[i - 1]);
        first = false;
    }
}

/* ethiopic-numeric (Counter Styles 3 7.1.3), v >= 1. */
static void ethiopic(long long v, Out *o)
{
    if (v == 1) { put_utf8(o->s, o->cap, &o->n, 0x1369); o->s[o->n] = 0; return; }
    int groups[16], ng = 0;
    for (long long x = v; x > 0 && ng < 16; x /= 100) groups[ng++] = (int)(x % 100);
    for (int gi = ng - 1; gi >= 0; gi--) {
        int g = groups[gi];
        bool remove = g == 0 || (gi == ng - 1 && g == 1) || ((gi & 1) && g == 1);
        if (!remove) {
            if (g / 10) put_utf8(o->s, o->cap, &o->n, 0x1372 + (uint32_t)(g / 10) - 1);
            if (g % 10) put_utf8(o->s, o->cap, &o->n, 0x1369 + (uint32_t)(g % 10) - 1);
        }
        if ((gi & 1) && g != 0) put_utf8(o->s, o->cap, &o->n, 0x137B);
        else if (!(gi & 1) && gi > 0) put_utf8(o->s, o->cap, &o->n, 0x137C);
        o->s[o->n] = 0;
    }
}

static bool cs_in_range(const CStyle *c, long long v)
{
    if (!c->range_auto) {
        for (int i = 0; i < c->nrange; i++)
            if (v >= c->lo[i] && v <= c->hi[i]) return true;
        return false;
    }
    switch (c->system) {
    case CS_ALPHABETIC: case CS_SYMBOLIC: return v >= 1;
    case CS_ADDITIVE: return v >= 0;
    default: return true;
    }
}

static bool uses_negative(const CStyle *c)
{
    return c->system == CS_SYMBOLIC || c->system == CS_ALPHABETIC || c->system == CS_NUMERIC ||
           c->system == CS_ADDITIVE || (c->system == CS_SPECIAL && c->special != SP_ETHIOPIC);
}

/* The representation of v (Counter Styles 3 3.1), *used: the row whose
 * prefix and suffix go with it. */
static void cs_rep(int si, long long v, Out *o, int depth, int *used)
{
    const CStyle *c = &g_cs[si];
    *used = si;
    o->n = 0;
    o->s[0] = 0;
    o->ok = true;
    if (depth < 8 && cs_in_range(c, v)) {
        long long a = v < 0 && uses_negative(c) ? -v : v;
        bool ok = true;
        switch (c->system) {
        case CS_CYCLIC: {
            long long k = ((a - 1) % c->nsym + c->nsym) % c->nsym;
            out_add(o, CS_STR(c, c->sym_off[k]));
            break;
        }
        case CS_FIXED:
            if (a < c->first || a - c->first >= c->nsym) { ok = false; break; }
            out_add(o, CS_STR(c, c->sym_off[a - c->first]));
            break;
        case CS_SYMBOLIC: {
            if (a < 1) { ok = false; break; }
            long long reps = (a + c->nsym - 1) / c->nsym;
            if (reps > 60) { ok = false; break; }
            const char *s = CS_STR(c, c->sym_off[(a - 1) % c->nsym]);
            for (long long r = 0; r < reps && o->ok; r++) out_add(o, s);
            break;
        }
        case CS_ALPHABETIC:
            if (a < 1) { ok = false; break; }
            while (a > 0 && o->ok) { a--; out_pre(o, CS_STR(c, c->sym_off[a % c->nsym])); a /= c->nsym; }
            break;
        case CS_NUMERIC:
            if (a == 0) { out_add(o, CS_STR(c, c->sym_off[0])); break; }
            while (a > 0 && o->ok) { out_pre(o, CS_STR(c, c->sym_off[a % c->nsym])); a /= c->nsym; }
            break;
        case CS_ADDITIVE:
            if (a == 0) {
                ok = false;
                for (int i = 0; i < c->nadd; i++)
                    if (c->add_w[i] == 0) { out_add(o, CS_STR(c, c->add_off[i])); ok = true; break; }
                break;
            }
            for (int i = 0; i < c->nadd && a > 0 && o->ok; i++) {
                if (c->add_w[i] == 0) continue;
                long long times = a / c->add_w[i];
                if (times > 1000) { ok = false; break; }
                for (long long t = 0; t < times && o->ok; t++) out_add(o, CS_STR(c, c->add_off[i]));
                a -= times * c->add_w[i];
            }
            if (a != 0) ok = false;
            break;
        case CS_SPECIAL:
            if (c->special == SP_ETHIOPIC) ethiopic(a, o);
            else if (a <= 9999999999999999LL) cjk_ext(c, a, o);
            else ok = false;
            break;
        }
        if (ok && o->ok) {
            bool neg = v < 0 && uses_negative(c);
            if (c->pad > 0) {
                /* (the negative sign's length counts, the padding goes inside it) */
                size_t have = cps(o->s) + (neg ? cps(CS_STR(c, c->neg_off[0])) + (c->has_neg2 ? cps(CS_STR(c, c->neg_off[1])) : 0) : 0);
                for (size_t k = have; k < (size_t)c->pad && o->ok; k++) out_pre(o, CS_STR(c, c->pad_off));
            }
            if (neg) {
                out_pre(o, CS_STR(c, c->neg_off[0]));
                if (c->has_neg2) out_add(o, CS_STR(c, c->neg_off[1]));
            }
            if (o->ok) return;
        }
    }
    /* (the fallback, and decimal when that can't either) */
    int fb = c->fallback[0] ? cs_lookup(c->fallback) : -1;
    if (fb < 0) fb = cs_lookup("decimal");
    if (depth >= 8 || fb == si) fb = cs_lookup("decimal");
    if (fb == si) { o->n = (size_t)snprintf(o->s, o->cap, "%lld", v); return; }
    cs_rep(fb, v, o, depth + 1, used);
}

/* symbols(<type>? <string>+): an anonymous style (Counter Styles 3 6). */
static int cs_symbols_fn(const char *style)
{
    static CStyle anon;
    const char *p = style + 8, *end = style + strlen(style);
    while (end > p && (isspace((unsigned char)end[-1]))) end--;
    if (end > p && end[-1] == ')') end--;
    memset(&anon, 0, sizeof anon);
    snprintf(anon.name, sizeof anon.name, "%s", "");
    anon.system = CS_SYMBOLIC;
    anon.range_auto = true;
    anon.first = 1;
    anon.buf[0] = 0;
    anon.used = 1;
    anon.neg_off[0] = cs_store(&anon, "-");
    static const char *const k[] = { "cyclic", "numeric", "alphabetic", "symbolic", "", "fixed" };
    for (int i = 0; i < 6; i++)
        if (k[i][0] && word_is(&p, end, k[i])) { anon.system = (unsigned char)i; break; }
    char sym[256];
    while (!at_end(p, end)) {
        const char *q = skip_ws(p, end);
        if (*q != '"' && *q != '\'') return -1;
        if (anon.nsym == CS_SYMS || !read_symbol(&p, end, sym, sizeof sym)) return -1;
        anon.sym_off[anon.nsym++] = cs_store(&anon, sym);
    }
    anon.set = D_SYMBOLS | D_SYSTEM;
    if (!cs_valid(&anon)) return -1;
    anon.suffix_off = cs_store(&anon, " ");
    anon.set |= D_SUFFIX;
    /* (a row of its own at the end of the table, for cs_rep's index) */
    if (g_n >= CS_MAX) return -1;
    g_cs[CS_MAX - 1] = anon;
    return CS_MAX - 1;
}

static int cs_style_index(const char *style)
{
    cs_init();
    if (!style || !*style) return cs_lookup("decimal");
    if (!strncasecmp(style, "symbols(", 8)) {
        int i = cs_symbols_fn(style);
        return i >= 0 ? i : cs_lookup("decimal");
    }
    int i = cs_lookup(style);
    return i >= 0 ? i : cs_lookup("decimal");
}

/* (the flow the next formats are for: writing mode (0 horizontal, 1 vertical-rl, 2 vertical-lr, 3 sideways-rl,
 * 4 sideways-lr) and right to left) */
static __thread int t_cs_wm, t_cs_rtl;
void cstyle_set_flow(int wm, int rtl) { t_cs_wm = wm; t_cs_rtl = rtl; }
/* (a disclosure triangle in out turned for the flow) */
static void cs_disc_turn(int disc, char *out)
{
    if (!disc) return;
    /* U+25B8 right, U+25C2 left, U+25BE down, U+25B4 up */
    static const char *const R8 = "\xE2\x96\xB8", *const L8 = "\xE2\x97\x82", *const D8 = "\xE2\x96\xBE", *const U8 = "\xE2\x96\xB4";
    const char *from = disc == 2 ? R8 : D8, *to;
    bool vert = t_cs_wm != 0;
    if (disc == 2) to = vert ? (t_cs_rtl ? U8 : D8) : (t_cs_rtl ? L8 : R8);
    else to = !vert ? D8 : (t_cs_wm == 1 || t_cs_wm == 3) ? L8 : R8;
    if (t_cs_wm == 4 && disc == 2) to = t_cs_rtl ? D8 : U8;   /* (sideways-lr: the inline end is up) */
    if (to == from) return;
    for (char *p = strstr(out, from); p; p = strstr(p + 3, from)) memcpy(p, to, 3);
}

void cstyle_format(const char *style, long long v, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if (style && !strcasecmp(style, "none")) return;
    int si = cs_style_index(style), used = si;
    Out o = { out, cap, 0, true };
    cs_rep(si, v, &o, 0, &used);
    if (si >= 0) cs_disc_turn(g_cs[si].disc, out);
}

void cstyle_marker(const char *style, long long v, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if (style && !strcasecmp(style, "none")) return;
    int si = cs_style_index(style), used = si;
    char rep[512];
    Out o = { rep, sizeof rep, 0, true };
    cs_rep(si, v, &o, 0, &used);
    const CStyle *c = &g_cs[si];
    const char *prefix = c->set & D_PREFIX || c->prefix_off ? CS_STR(c, c->prefix_off) : "";
    const char *suffix = (c->set & D_SUFFIX) || c->suffix_off ? CS_STR(c, c->suffix_off) : ". ";
    snprintf(out, cap, "%s%s%s", prefix, rep, suffix);
    cs_disc_turn(c->disc, out);
}
