/* The CSS Syntax Level 3 tokenizer (https://drafts.csswg.org/css-syntax-3/
 * section 4), over UTF-8 bytes: non-ASCII bytes are name code points as a
 * whole (every non-ASCII code point is), so only escapes decode UTF-8.  The
 * preprocessing of section 3.3 happens as the input is read: CR, CR LF and
 * FF are newlines, NUL is U+FFFD. */
#include "css_tokenizer.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    size_t n, i;
    CssTokens *out;
    bool oom;
} Tz;

static bool is_newline(char c) { return c == '\n' || c == '\r' || c == '\f'; }
static bool is_ws(char c) { return c == ' ' || c == '\t' || is_newline(c); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static bool is_name_start(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80 || c == 0; }
static bool is_name(unsigned char c) { return is_name_start(c) || is_digit((char)c) || c == '-'; }

static char at(const Tz *z, size_t k) { return z->i + k < z->n ? z->s[z->i + k] : 0; }
static bool more(const Tz *z, size_t k) { return z->i + k < z->n; }

/* Two code points starting a valid escape (section 4.3.8). */
static bool valid_escape(char a, char b, bool b_exists) { return a == '\\' && b_exists && !is_newline(b); }

static bool starts_ident(const Tz *z, size_t k)
{
    char a = at(z, k), b = at(z, k + 1);
    if (!more(z, k)) return false;
    if (a == '-') {
        return (more(z, k + 1) && (is_name_start((unsigned char)b) || b == '-')) ||
               valid_escape(b, at(z, k + 2), more(z, k + 2));
    }
    if (is_name_start((unsigned char)a)) return true;
    return valid_escape(a, b, more(z, k + 1));
}

static bool starts_number(const Tz *z, size_t k)
{
    char a = at(z, k), b = at(z, k + 1);
    if (!more(z, k)) return false;
    if (a == '+' || a == '-') return is_digit(b) || (b == '.' && is_digit(at(z, k + 2)));
    if (a == '.') return is_digit(b);
    return is_digit(a);
}

/* ---- the arena ---- */

static bool arena_put(Tz *z, const char *p, size_t len)
{
    CssTokens *o = z->out;
    if (o->an + len + 1 > o->acap) {
        size_t nc = o->acap ? o->acap * 2 : 1024;
        while (nc < o->an + len + 1) nc *= 2;
        char *na = (char *)realloc(o->arena, nc);
        if (!na) { z->oom = true; return false; }
        o->arena = na;
        o->acap = nc;
    }
    memcpy(o->arena + o->an, p, len);
    o->an += len;
    return true;
}

static void arena_cp(Tz *z, uint32_t cp)
{
    char b[4];
    size_t l;
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) { b[0] = (char)cp; l = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); l = 2; }
    else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F));
        l = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F));
        l = 4;
    }
    arena_put(z, b, l);
}

/* Consume an escape (the backslash consumed already; section 4.3.7) into
 * the arena. */
static void consume_escape(Tz *z)
{
    if (!more(z, 0)) { arena_cp(z, 0xFFFD); return; }
    char c = z->s[z->i];
    if (is_hex(c)) {
        uint32_t v = 0;
        int k = 0;
        while (k < 6 && more(z, 0) && is_hex(z->s[z->i])) {
            char h = z->s[z->i++];
            v = v * 16 + (uint32_t)(is_digit(h) ? h - '0' : (h | 0x20) - 'a' + 10);
            k++;
        }
        if (more(z, 0) && is_ws(z->s[z->i])) {
            if (z->s[z->i] == '\r' && at(z, 1) == '\n') z->i++;
            z->i++;
        }
        arena_cp(z, v);
        return;
    }
    /* (any other code point, whole) */
    size_t st = z->i++;
    while (more(z, 0) && ((unsigned char)z->s[z->i] & 0xC0) == 0x80) z->i++;
    if (z->s[st] == 0) arena_cp(z, 0xFFFD);
    else arena_put(z, z->s + st, z->i - st);
}

/* Consume a name (section 4.3.12): its value's place (source, or the arena
 * once an escape is met). */
static void consume_name(Tz *z, uint32_t *val, uint32_t *len, uint8_t *flags)
{
    size_t st = z->i;
    while (more(z, 0)) {
        unsigned char c = (unsigned char)z->s[z->i];
        if (is_name(c) && c != 0) { z->i++; continue; }
        if (valid_escape((char)c, at(z, 1), more(z, 1)) || c == 0) break;
        *val = (uint32_t)st;
        *len = (uint32_t)(z->i - st);
        return;
    }
    if (!more(z, 0)) {
        *val = (uint32_t)st;
        *len = (uint32_t)(z->i - st);
        return;
    }
    /* With escapes: copy what is plain so far, then go on in the arena. */
    size_t a0 = z->out->an;
    arena_put(z, z->s + st, z->i - st);
    while (more(z, 0)) {
        unsigned char c = (unsigned char)z->s[z->i];
        if (c == 0) { z->i++; arena_cp(z, 0xFFFD); continue; }
        if (is_name(c)) {
            size_t r = z->i;
            while (more(z, 0) && is_name((unsigned char)z->s[z->i]) && z->s[z->i] != 0) z->i++;
            arena_put(z, z->s + r, z->i - r);
            continue;
        }
        if (valid_escape((char)c, at(z, 1), more(z, 1))) {
            z->i++;
            consume_escape(z);
            continue;
        }
        break;
    }
    *val = (uint32_t)a0;
    *len = (uint32_t)(z->out->an - a0);
    *flags |= CT_F_ARENA;
}

static CssToken *emit(Tz *z, int type, size_t start)
{
    CssTokens *o = z->out;
    if (o->n == o->cap) {
        int nc = o->cap ? o->cap * 2 : 256;
        CssToken *nt = (CssToken *)realloc(o->t, (size_t)nc * sizeof(CssToken));
        if (!nt) { z->oom = true; return NULL; }
        o->t = nt;
        o->cap = nc;
    }
    CssToken *t = &o->t[o->n++];
    memset(t, 0, sizeof(*t));
    t->type = (uint8_t)type;
    t->start = (uint32_t)start;
    return t;
}

/* Consume a number (section 4.3.13): its value and whether it is an integer. */
static double consume_number(Tz *z, bool *integer)
{
    double sign = 1.0, v = 0.0;
    *integer = true;
    if (at(z, 0) == '+' || at(z, 0) == '-') { if (z->s[z->i] == '-') sign = -1.0; z->i++; }
    while (more(z, 0) && is_digit(z->s[z->i])) v = v * 10.0 + (z->s[z->i++] - '0');
    if (at(z, 0) == '.' && is_digit(at(z, 1))) {
        *integer = false;
        z->i++;
        double f = 0.1;
        while (more(z, 0) && is_digit(z->s[z->i])) { v += (z->s[z->i++] - '0') * f; f *= 0.1; }
    }
    char e = at(z, 0);
    if ((e == 'e' || e == 'E') &&
        (is_digit(at(z, 1)) || ((at(z, 1) == '+' || at(z, 1) == '-') && is_digit(at(z, 2))))) {
        *integer = false;
        z->i++;
        int es = 1, ev = 0;
        if (z->s[z->i] == '+' || z->s[z->i] == '-') { if (z->s[z->i] == '-') es = -1; z->i++; }
        while (more(z, 0) && is_digit(z->s[z->i])) { if (ev < 10000) ev = ev * 10 + (z->s[z->i] - '0'); z->i++; }
        double p = 1.0, b = 10.0;
        for (int k = ev; k > 0; k >>= 1) { if (k & 1) p *= b; b *= b; }
        v = es > 0 ? v * p : v / p;
    }
    return sign * v;
}

static void consume_numeric(Tz *z, size_t start)
{
    bool integer;
    double v = consume_number(z, &integer);
    if (starts_ident(z, 0)) {
        CssToken *t = emit(z, CT_DIMENSION, start);
        if (!t) return;
        t->num = v;
        t->flags = integer ? CT_F_INTEGER : 0;
        uint32_t val = 0, len = 0;
        uint8_t fl = 0;
        consume_name(z, &val, &len, &fl);
        t = &z->out->t[z->out->n - 1];
        t->val = val;
        t->val_len = len;
        t->flags |= fl;
        t->end = (uint32_t)z->i;
        return;
    }
    int type = CT_NUMBER;
    if (at(z, 0) == '%') { z->i++; type = CT_PERCENTAGE; }
    CssToken *t = emit(z, type, start);
    if (!t) return;
    t->num = v;
    t->flags = integer && type == CT_NUMBER ? CT_F_INTEGER : 0;
    t->end = (uint32_t)z->i;
}

/* Consume a string token (section 4.3.5), the quote consumed already. */
static void consume_string(Tz *z, char quote, size_t start)
{
    size_t st = z->i;
    bool arena = false;
    size_t a0 = 0;
    for (;;) {
        if (!more(z, 0)) break;   /* (EOF: a parse error, the string as it is) */
        char c = z->s[z->i];
        if (c == quote) break;
        if (is_newline(c)) {
            /* A bad string: the newline is not consumed. */
            CssToken *t = emit(z, CT_BAD_STRING, start);
            if (t) t->end = (uint32_t)z->i;
            return;
        }
        if (c == '\\' || c == 0) {
            if (!arena) { a0 = z->out->an; arena_put(z, z->s + st, z->i - st); arena = true; }
            z->i++;
            if (c == 0) { arena_cp(z, 0xFFFD); continue; }
            if (!more(z, 0)) continue;
            if (is_newline(z->s[z->i])) {
                if (z->s[z->i] == '\r' && at(z, 1) == '\n') z->i++;
                z->i++;
                continue;
            }
            consume_escape(z);
            continue;
        }
        if (arena) {
            size_t r = z->i;
            while (more(z, 0) && z->s[z->i] != quote && z->s[z->i] != '\\' && z->s[z->i] != 0 &&
                   !is_newline(z->s[z->i]))
                z->i++;
            arena_put(z, z->s + r, z->i - r);
            continue;
        }
        z->i++;
    }
    size_t vend = z->i;
    if (more(z, 0)) z->i++;   /* (the closing quote) */
    CssToken *t = emit(z, CT_STRING, start);
    if (!t) return;
    if (arena) {
        t->val = (uint32_t)a0;
        t->val_len = (uint32_t)(z->out->an - a0);
        t->flags = CT_F_ARENA;
    } else {
        t->val = (uint32_t)st;
        t->val_len = (uint32_t)(vend - st);
    }
    t->end = (uint32_t)z->i;
}

/* The remnants of a bad url (section 4.3.14). */
static void consume_bad_url(Tz *z)
{
    while (more(z, 0)) {
        char c = z->s[z->i];
        if (c == ')') { z->i++; return; }
        if (valid_escape(c, at(z, 1), more(z, 1))) {
            z->i++;
            size_t a0 = z->out->an;
            consume_escape(z);
            z->out->an = a0;   /* (not kept) */
            continue;
        }
        z->i++;
    }
}

/* Consume a url token (section 4.3.6), after "url(" and its white space. */
static void consume_url(Tz *z, size_t start)
{
    size_t st = z->i;
    bool arena = false;
    size_t a0 = 0;
    for (;;) {
        if (!more(z, 0)) break;
        char c = z->s[z->i];
        if (c == ')') break;
        if (is_ws(c)) {
            size_t ws = z->i;
            while (more(z, 0) && is_ws(z->s[z->i])) z->i++;
            if (!more(z, 0) || z->s[z->i] == ')') {
                if (!arena) { a0 = z->out->an; arena_put(z, z->s + st, ws - st); arena = true; }
                break;
            }
            consume_bad_url(z);
            CssToken *t = emit(z, CT_BAD_URL, start);
            if (t) t->end = (uint32_t)z->i;
            return;
        }
        /* (a quote, an opening parenthesis or a non-printable code point:
         * tabs and newlines are white space, handled above, NUL below) */
        bool nonprintable = ((unsigned char)c < 0x20 && c != 0) || c == 0x7F;
        if (c == '"' || c == '\'' || c == '(' || nonprintable) {
            consume_bad_url(z);
            CssToken *t = emit(z, CT_BAD_URL, start);
            if (t) t->end = (uint32_t)z->i;
            return;
        }
        if (c == '\\') {
            if (!valid_escape(c, at(z, 1), more(z, 1))) {
                consume_bad_url(z);
                CssToken *t = emit(z, CT_BAD_URL, start);
                if (t) t->end = (uint32_t)z->i;
                return;
            }
            if (!arena) { a0 = z->out->an; arena_put(z, z->s + st, z->i - st); arena = true; }
            z->i++;
            consume_escape(z);
            continue;
        }
        if (c == 0) {
            if (!arena) { a0 = z->out->an; arena_put(z, z->s + st, z->i - st); arena = true; }
            z->i++;
            arena_cp(z, 0xFFFD);
            continue;
        }
        if (arena) arena_put(z, z->s + z->i, 1);
        z->i++;
    }
    size_t vend = z->i;
    if (more(z, 0)) z->i++;   /* ) */
    CssToken *t = emit(z, CT_URL, start);
    if (!t) return;
    if (arena) {
        t->val = (uint32_t)a0;
        t->val_len = (uint32_t)(z->out->an - a0);
        t->flags = CT_F_ARENA;
    } else {
        t->val = (uint32_t)st;
        t->val_len = (uint32_t)(vend - st);
    }
    t->end = (uint32_t)z->i;
}

/* An ident-like token (section 4.3.4): ident, function or url. */
static void consume_ident_like(Tz *z, size_t start)
{
    uint32_t val = 0, len = 0;
    uint8_t fl = 0;
    consume_name(z, &val, &len, &fl);
    const char *v = (fl & CT_F_ARENA ? z->out->arena : z->s) + val;
    bool is_url = len == 3 && (v[0] | 0x20) == 'u' && (v[1] | 0x20) == 'r' && (v[2] | 0x20) == 'l';
    if (at(z, 0) == '(' && more(z, 0)) {
        z->i++;
        if (is_url) {
            /* url( then white space: a function when a quote follows. */
            size_t save = z->i;
            while (more(z, 0) && is_ws(z->s[z->i]) && more(z, 1) && is_ws(z->s[z->i + 1])) z->i++;
            char c0 = at(z, 0), c1 = at(z, 1);
            bool quoted = c0 == '"' || c0 == '\'' || (is_ws(c0) && (c1 == '"' || c1 == '\''));
            if (!quoted) {
                while (more(z, 0) && is_ws(z->s[z->i])) z->i++;
                consume_url(z, start);
                return;
            }
            (void)save;
        }
        CssToken *t = emit(z, CT_FUNCTION, start);
        if (!t) return;
        t->val = val;
        t->val_len = len;
        t->flags = fl;
        t->end = (uint32_t)z->i;
        return;
    }
    CssToken *t = emit(z, CT_IDENT, start);
    if (!t) return;
    t->val = val;
    t->val_len = len;
    t->flags = fl;
    t->end = (uint32_t)z->i;
}

static void simple(Tz *z, int type, size_t start, size_t len)
{
    z->i += len;
    CssToken *t = emit(z, type, start);
    if (t) t->end = (uint32_t)z->i;
}

static void delim(Tz *z, size_t start)
{
    /* (one code point, whole) */
    unsigned char c = (unsigned char)z->s[z->i];
    uint32_t cp = c;
    size_t l = 1;
    if (c >= 0xF0 && more(z, 3)) { cp = ((c & 7u) << 18) | ((z->s[z->i + 1] & 0x3Fu) << 12) | ((z->s[z->i + 2] & 0x3Fu) << 6) | (z->s[z->i + 3] & 0x3Fu); l = 4; }
    else if (c >= 0xE0 && more(z, 2)) { cp = ((c & 0xFu) << 12) | ((z->s[z->i + 1] & 0x3Fu) << 6) | (z->s[z->i + 2] & 0x3Fu); l = 3; }
    else if (c >= 0xC0 && more(z, 1)) { cp = ((c & 0x1Fu) << 6) | (z->s[z->i + 1] & 0x3Fu); l = 2; }
    z->i += l;
    CssToken *t = emit(z, CT_DELIM, start);
    if (!t) return;
    t->delim = cp;
    t->end = (uint32_t)z->i;
}

bool css_tokenize(const char *s, size_t n, CssTokens *out)
{
    Tz z = { s, n, 0, out, false };
    out->src = s;
    while (!z.oom) {
        /* Comments (section 4.3.2). */
        if (at(&z, 0) == '/' && at(&z, 1) == '*') {
            const char *e = NULL;
            for (size_t k = z.i + 2; k + 1 < n; k++)
                if (s[k] == '*' && s[k + 1] == '/') { e = s + k; break; }
            z.i = e ? (size_t)(e - s) + 2 : n;
            continue;
        }
        if (!more(&z, 0)) break;
        size_t start = z.i;
        char c = s[z.i];
        if (is_ws(c)) {
            while (more(&z, 0) && is_ws(s[z.i])) z.i++;
            CssToken *t = emit(&z, CT_WHITESPACE, start);
            if (t) t->end = (uint32_t)z.i;
            continue;
        }
        switch (c) {
        case '"':
        case '\'':
            z.i++;
            consume_string(&z, c, start);
            continue;
        case '#':
            if (more(&z, 1) && (is_name((unsigned char)at(&z, 1)) || valid_escape(at(&z, 1), at(&z, 2), more(&z, 2)))) {
                z.i++;
                uint8_t fl = starts_ident(&z, 0) ? CT_F_ID : 0;
                uint32_t val = 0, len = 0;
                consume_name(&z, &val, &len, &fl);
                CssToken *t = emit(&z, CT_HASH, start);
                if (!t) break;
                t->val = val;
                t->val_len = len;
                t->flags = fl;
                t->end = (uint32_t)z.i;
                continue;
            }
            delim(&z, start);
            continue;
        case '(': simple(&z, CT_LPAREN, start, 1); continue;
        case ')': simple(&z, CT_RPAREN, start, 1); continue;
        case '[': simple(&z, CT_LBRACKET, start, 1); continue;
        case ']': simple(&z, CT_RBRACKET, start, 1); continue;
        case '{': simple(&z, CT_LBRACE, start, 1); continue;
        case '}': simple(&z, CT_RBRACE, start, 1); continue;
        case ',': simple(&z, CT_COMMA, start, 1); continue;
        case ':': simple(&z, CT_COLON, start, 1); continue;
        case ';': simple(&z, CT_SEMICOLON, start, 1); continue;
        case '+':
        case '.':
            if (starts_number(&z, 0)) { consume_numeric(&z, start); continue; }
            delim(&z, start);
            continue;
        case '-':
            if (starts_number(&z, 0)) { consume_numeric(&z, start); continue; }
            if (at(&z, 1) == '-' && at(&z, 2) == '>') { simple(&z, CT_CDC, start, 3); continue; }
            if (starts_ident(&z, 0)) { consume_ident_like(&z, start); continue; }
            delim(&z, start);
            continue;
        case '<':
            if (at(&z, 1) == '!' && at(&z, 2) == '-' && at(&z, 3) == '-') { simple(&z, CT_CDO, start, 4); continue; }
            delim(&z, start);
            continue;
        case '@':
            if (starts_ident(&z, 1)) {
                z.i++;
                uint32_t val = 0, len = 0;
                uint8_t fl = 0;
                consume_name(&z, &val, &len, &fl);
                CssToken *t = emit(&z, CT_AT_KEYWORD, start);
                if (!t) break;
                t->val = val;
                t->val_len = len;
                t->flags = fl;
                t->end = (uint32_t)z.i;
                continue;
            }
            delim(&z, start);
            continue;
        case '\\':
            if (valid_escape(c, at(&z, 1), more(&z, 1))) { consume_ident_like(&z, start); continue; }
            delim(&z, start);   /* (a parse error) */
            continue;
        default:
            break;
        }
        if (is_digit(c)) { consume_numeric(&z, start); continue; }
        if (is_name_start((unsigned char)c)) { consume_ident_like(&z, start); continue; }
        delim(&z, start);
    }
    if (z.oom) return false;
    CssToken *t = emit(&z, CT_EOF, n);
    if (!t) return false;
    t->end = (uint32_t)n;
    return true;
}

void css_tokens_free(CssTokens *ts)
{
    if (!ts) return;
    free(ts->t);
    free(ts->arena);
    memset(ts, 0, sizeof(*ts));
}
