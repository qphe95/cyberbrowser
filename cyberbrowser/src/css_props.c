/*
 * css_props.c - the CSS property table and its value grammar matcher
 * (css_props.h).
 *
 *   tokens      CSS Syntax 3 tokens of a value, blocks and functions as
 *               nested ranges, whitespace as a flag on the next token;
 *   grammars    the value definition syntax compiled once into node trees
 *               (properties' grammars, and the <type>s of css_props_types.h);
 *   matching    backtracking over the tokens in continuation-passing style,
 *               recording the nodes on the successful path;
 *   serializing the recorded match: keywords lower-cased, numbers short,
 *               colors as rgb(), || and && groups in grammar order.
 *
 * The property and type tables are in css_props_table.h.
 */
#include "css_props.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ============================================================================
 * Small helpers
 * ============================================================================ */

typedef struct Buf { char *s; size_t n, cap; } Buf;

static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->n + n + 1) cap *= 2;
        char *ns = realloc(b->s, cap);
        if (!ns) return;
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = '\0';
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_ch(Buf *b, char c) { buf_add(b, &c, 1); }
static char *buf_take(Buf *b) {
    if (!b->s) return strdup("");
    char *s = b->s;
    b->s = NULL;
    b->n = b->cap = 0;
    return s;
}

static bool ieq(const char *a, size_t an, const char *b) {
    size_t bn = strlen(b);
    return an == bn && strncasecmp(a, b, an) == 0;
}

/* A number in CSSOM's form: shortest, no exponent, at most 6 decimals. */
static void fmt_number(Buf *b, double v) {
    char tmp[64];
    if (!isfinite(v)) { buf_str(b, v != v ? "NaN" : v > 0 ? "infinity" : "-infinity"); return; }
    if (v == 0) v = 0;  /* (-0) */
    if (fabs(v - round(v)) < 1e-9 && fabs(v) < 1e15) {
        snprintf(tmp, sizeof tmp, "%.0f", round(v));
    } else {
        snprintf(tmp, sizeof tmp, "%.6f", v);
        char *e = tmp + strlen(tmp) - 1;
        while (e > tmp && *e == '0') *e-- = '\0';
        if (*e == '.') *e = '\0';
        if (strcmp(tmp, "-0") == 0) strcpy(tmp, "0");
    }
    buf_str(b, tmp);
}

/* ============================================================================
 * Tokens
 * ============================================================================ */

enum {
    T_IDENT, T_FUNC, T_AT, T_HASH, T_STRING, T_URL, T_NUMBER, T_PERCENT, T_DIM,
    T_DELIM, T_COMMA, T_COLON, T_SEMI, T_PAREN, T_BRACKET, T_BRACE, T_BAD,
};

typedef struct Tok {
    unsigned char type;
    unsigned char ws;       /* whitespace before it */
    unsigned char is_int;   /* a number written without '.' or exponent */
    unsigned char hash_id;  /* a hash that would be an identifier */
    char delim;
    double num;
    char *str;              /* ident / function name / string / url / hash / unit (unescaped) */
    int first, end;         /* function and blocks: children [first, end) */
} Tok;

typedef struct Toks { Tok *t; int n, cap; } Toks;

static void toks_free(Toks *ts) {
    for (int i = 0; i < ts->n; i++) free(ts->t[i].str);
    free(ts->t);
    ts->t = NULL;
    ts->n = ts->cap = 0;
}

static int toks_push(Toks *ts) {
    if (ts->n == ts->cap) {
        int cap = ts->cap ? ts->cap * 2 : 16;
        Tok *nt = realloc(ts->t, (size_t)cap * sizeof(Tok));
        if (!nt) return -1;
        ts->t = nt;
        ts->cap = cap;
    }
    memset(&ts->t[ts->n], 0, sizeof(Tok));
    return ts->n++;
}

typedef struct Lex { const char *s; size_t i, n; } Lex;

static bool is_name_start(unsigned char c) { return isalpha(c) || c == '_' || c >= 0x80; }
static bool is_name(unsigned char c) { return is_name_start(c) || isdigit(c) || c == '-'; }
static bool valid_escape(const Lex *L, size_t i) {
    return i + 1 < L->n + 1 && L->s[i] == '\\' && i + 1 < L->n && L->s[i + 1] != '\n';
}
static bool starts_ident(const Lex *L, size_t i) {
    if (i >= L->n) return false;
    unsigned char c = (unsigned char)L->s[i];
    if (c == '-') {
        if (i + 1 >= L->n) return false;
        unsigned char d = (unsigned char)L->s[i + 1];
        return is_name_start(d) || d == '-' || valid_escape(L, i + 1);
    }
    if (is_name_start(c)) return true;
    return c == '\\' && valid_escape(L, i);
}

/* Append the UTF-8 of code point cp. */
static void put_cp(Buf *b, unsigned cp) {
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
    char u[4];
    if (cp < 0x80) { buf_ch(b, (char)cp); return; }
    if (cp < 0x800) { u[0] = (char)(0xC0 | cp >> 6); u[1] = (char)(0x80 | (cp & 63)); buf_add(b, u, 2); return; }
    if (cp < 0x10000) {
        u[0] = (char)(0xE0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 63)); u[2] = (char)(0x80 | (cp & 63));
        buf_add(b, u, 3);
        return;
    }
    u[0] = (char)(0xF0 | cp >> 18); u[1] = (char)(0x80 | ((cp >> 12) & 63));
    u[2] = (char)(0x80 | ((cp >> 6) & 63)); u[3] = (char)(0x80 | (cp & 63));
    buf_add(b, u, 4);
}

static void lex_escape(Lex *L, Buf *b) {
    L->i++;     /* '\\' */
    if (L->i >= L->n) { put_cp(b, 0xFFFD); return; }
    if (isxdigit((unsigned char)L->s[L->i])) {
        unsigned cp = 0;
        int k = 0;
        while (k < 6 && L->i < L->n && isxdigit((unsigned char)L->s[L->i])) {
            char c = L->s[L->i++];
            cp = cp * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0' : (tolower((unsigned char)c) - 'a' + 10));
            k++;
        }
        if (L->i < L->n && isspace((unsigned char)L->s[L->i])) L->i++;
        put_cp(b, cp);
        return;
    }
    buf_ch(b, L->s[L->i++]);
}

static char *lex_name(Lex *L) {
    Buf b = { 0 };
    while (L->i < L->n) {
        unsigned char c = (unsigned char)L->s[L->i];
        if (is_name(c)) { buf_ch(&b, (char)c); L->i++; }
        else if (c == '\\' && valid_escape(L, L->i)) lex_escape(L, &b);
        else break;
    }
    return buf_take(&b);
}

static bool starts_number(const Lex *L, size_t i) {
    if (i >= L->n) return false;
    char c = L->s[i];
    if (c == '+' || c == '-') { i++; if (i >= L->n) return false; c = L->s[i]; }
    if (isdigit((unsigned char)c)) return true;
    return c == '.' && i + 1 < L->n && isdigit((unsigned char)L->s[i + 1]);
}

/* Tokenize [L->i, L->n) up to the closing `close` (0: the end) into ts;
 * returns false on tokens that make a value invalid (bad strings/urls). */
static bool lex_list(Lex *L, Toks *ts, char close, bool *ok_end);

static int lex_one(Lex *L, Toks *ts, bool ws) {
    const char *s = L->s;
    unsigned char c = (unsigned char)s[L->i];
    int k;
    if (c == '"' || c == '\'') {
        char q = (char)c;
        L->i++;
        Buf b = { 0 };
        bool bad = false;
        while (L->i < L->n && s[L->i] != q) {
            if (s[L->i] == '\n') { bad = true; break; }
            if (s[L->i] == '\\') {
                if (L->i + 1 < L->n && s[L->i + 1] == '\n') { L->i += 2; continue; }
                lex_escape(L, &b);
                continue;
            }
            buf_ch(&b, s[L->i++]);
        }
        if (L->i < L->n && s[L->i] == q) L->i++;
        if ((k = toks_push(ts)) < 0) { free(b.s); return -1; }
        ts->t[k].type = bad ? T_BAD : T_STRING;
        ts->t[k].str = buf_take(&b);
        ts->t[k].ws = ws;
        return k;
    }
    if (starts_number(L, L->i)) {
        char *end = NULL;
        double v = strtod(s + L->i, &end);
        size_t j = L->i;
        if (s[j] == '+' || s[j] == '-') j++;
        bool is_int = true;
        for (const char *p = s + j; p < end; p++) if (*p == '.' || *p == 'e' || *p == 'E') is_int = false;
        /* strtod reads hex and "inf": CSS numbers are decimal only */
        size_t len = (size_t)(end - (s + L->i));
        const char *p = s + j;
        size_t m = 0;
        while (p + m < end && (isdigit((unsigned char)p[m]) || p[m] == '.' || p[m] == 'e' || p[m] == 'E' ||
                               ((p[m] == '+' || p[m] == '-') && m && (p[m - 1] == 'e' || p[m - 1] == 'E')))) m++;
        if (p + m != end) { end = (char *)(p + m); v = strtod(s + L->i, NULL); len = (size_t)(end - (s + L->i)); }
        /* "1e" with no exponent digits is the number 1 and the unit "e" */
        if (len >= 1 && (end[-1] == 'e' || end[-1] == 'E')) { end--; len--; }
        L->i += len;
        if ((k = toks_push(ts)) < 0) return -1;
        Tok *t = &ts->t[k];
        t->num = v;
        t->is_int = is_int;
        t->ws = ws;
        if (L->i < L->n && s[L->i] == '%') { L->i++; t->type = T_PERCENT; }
        else if (starts_ident(L, L->i)) { t->type = T_DIM; t->str = lex_name(L); t = &ts->t[k]; }
        else t->type = T_NUMBER;
        return k;
    }
    if (starts_ident(L, L->i)) {
        char *name = lex_name(L);
        if (L->i < L->n && s[L->i] == '(') {
            L->i++;
            if (strcasecmp(name, "url") == 0) {
                size_t j = L->i;
                while (j < L->n && isspace((unsigned char)s[j])) j++;
                if (j >= L->n || (s[j] != '"' && s[j] != '\'')) {
                    /* an unquoted url() */
                    L->i = j;
                    Buf b = { 0 };
                    bool bad = false;
                    while (L->i < L->n && s[L->i] != ')') {
                        char ch = s[L->i];
                        if (isspace((unsigned char)ch)) {
                            while (L->i < L->n && isspace((unsigned char)s[L->i])) L->i++;
                            if (L->i < L->n && s[L->i] != ')') bad = true;
                            continue;
                        }
                        if (ch == '"' || ch == '\'' || ch == '(') bad = true;
                        if (ch == '\\') { lex_escape(L, &b); continue; }
                        buf_ch(&b, ch);
                        L->i++;
                    }
                    if (L->i < L->n) L->i++;
                    free(name);
                    if ((k = toks_push(ts)) < 0) { free(b.s); return -1; }
                    ts->t[k].type = bad ? T_BAD : T_URL;
                    ts->t[k].str = buf_take(&b);
                    ts->t[k].ws = ws;
                    return k;
                }
            }
            if ((k = toks_push(ts)) < 0) { free(name); return -1; }
            ts->t[k].type = T_FUNC;
            ts->t[k].str = name;
            ts->t[k].ws = ws;
            ts->t[k].first = ts->n;
            bool ok = true;
            if (!lex_list(L, ts, ')', &ok)) return -1;
            ts->t[k].end = ts->n;
            return k;
        }
        if ((k = toks_push(ts)) < 0) { free(name); return -1; }
        ts->t[k].type = T_IDENT;
        ts->t[k].str = name;
        ts->t[k].ws = ws;
        return k;
    }
    if (c == '#' && L->i + 1 < L->n && (is_name((unsigned char)s[L->i + 1]) || valid_escape(L, L->i + 1))) {
        L->i++;
        bool id = starts_ident(L, L->i);
        char *name = lex_name(L);
        if ((k = toks_push(ts)) < 0) { free(name); return -1; }
        ts->t[k].type = T_HASH;
        ts->t[k].str = name;
        ts->t[k].hash_id = id;
        ts->t[k].ws = ws;
        return k;
    }
    if (c == '@' && starts_ident(L, L->i + 1)) {
        L->i++;
        char *name = lex_name(L);
        if ((k = toks_push(ts)) < 0) { free(name); return -1; }
        ts->t[k].type = T_AT;
        ts->t[k].str = name;
        ts->t[k].ws = ws;
        return k;
    }
    if (c == '(' || c == '[' || c == '{') {
        L->i++;
        if ((k = toks_push(ts)) < 0) return -1;
        ts->t[k].type = c == '(' ? T_PAREN : c == '[' ? T_BRACKET : T_BRACE;
        ts->t[k].ws = ws;
        ts->t[k].first = ts->n;
        bool ok = true;
        if (!lex_list(L, ts, c == '(' ? ')' : c == '[' ? ']' : '}', &ok)) return -1;
        ts->t[k].end = ts->n;
        return k;
    }
    L->i++;
    if ((k = toks_push(ts)) < 0) return -1;
    Tok *t = &ts->t[k];
    t->ws = ws;
    if (c == ',') t->type = T_COMMA;
    else if (c == ':') t->type = T_COLON;
    else if (c == ';') t->type = T_SEMI;
    else if (c == ')' || c == ']' || c == '}') t->type = T_BAD;   /* unmatched */
    else { t->type = T_DELIM; t->delim = (char)c; }
    return k;
}

static bool lex_list(Lex *L, Toks *ts, char close, bool *ok_end) {
    (void)ok_end;
    /* Children are laid out after the parent token: a nested block's own
     * children follow it, so each list is contiguous only at its top level.
     * Parents point at [first, end) and walkers skip nested ranges. */
    bool ws = false;
    while (L->i < L->n) {
        unsigned char c = (unsigned char)L->s[L->i];
        if (isspace(c)) { ws = true; L->i++; continue; }
        if (c == '/' && L->i + 1 < L->n && L->s[L->i + 1] == '*') {
            const char *e = strstr(L->s + L->i + 2, "*/");
            L->i = e && (size_t)(e - L->s) < L->n ? (size_t)(e - L->s) + 2 : L->n;
            ws = true;
            continue;
        }
        if (close && (char)c == close) { L->i++; return true; }
        if (lex_one(L, ts, ws) < 0) return false;
        ws = false;
    }
    return true;
}

/* The next sibling of token i (skipping a function's or block's children). */
static int tok_next(const Toks *ts, int i) {
    const Tok *t = &ts->t[i];
    if (t->type == T_FUNC || t->type == T_PAREN || t->type == T_BRACKET || t->type == T_BRACE) return t->end;
    return i + 1;
}

static bool tokenize(const char *s, size_t n, Toks *ts) {
    Lex L = { s, 0, n };
    memset(ts, 0, sizeof(*ts));
    bool ok = true;
    if (!lex_list(&L, ts, 0, &ok)) { toks_free(ts); return false; }
    return true;
}

/* ============================================================================
 * Grammar nodes
 * ============================================================================ */

enum { G_KW, G_TYPE, G_PROP, G_FUNC, G_LIT, G_SEQ, G_ALT, G_ALL, G_ANY, G_BLOCK };

typedef struct GNode {
    unsigned char kind;
    unsigned char comma;        /* the # multiplier */
    unsigned char required;     /* the ! multiplier */
    unsigned short min, max;    /* repetitions (1,1 none) */
    char lit;                   /* G_LIT: ',' '/' ... */
    char *text;                 /* keyword, type, property or function name */
    double rmin, rmax;          /* <type [min,max]> */
    unsigned char has_range;
    int idx;                    /* the node's position among its parent's children */
    struct GNode **kids;
    int nk;
} GNode;

#define REP_INF 0xFFFF

typedef struct GParse { const char *s; size_t i; bool err; } GParse;

static GNode *gnode(int kind) {
    GNode *g = calloc(1, sizeof(GNode));
    if (g) { g->kind = (unsigned char)kind; g->min = g->max = 1; g->rmin = -INFINITY; g->rmax = INFINITY; }
    return g;
}
static void gadd(GNode *p, GNode *c) {
    GNode **nk = realloc(p->kids, (size_t)(p->nk + 1) * sizeof(GNode *));
    if (!nk) return;
    p->kids = nk;
    c->idx = p->nk;
    p->kids[p->nk++] = c;
}
static void gws(GParse *P) { while (P->s[P->i] && isspace((unsigned char)P->s[P->i])) P->i++; }

static GNode *g_alt(GParse *P);

static double g_num(GParse *P) {
    gws(P);
    if (P->s[P->i] == '-' && strncmp(P->s + P->i, "-\xE2\x88\x9E", 4) == 0) { P->i += 4; return -INFINITY; }
    if (strncmp(P->s + P->i, "\xE2\x88\x9E", 3) == 0) { P->i += 3; return INFINITY; }
    if (strncmp(P->s + P->i, "inf", 3) == 0) { P->i += 3; return INFINITY; }
    char *e = NULL;
    double v = strtod(P->s + P->i, &e);
    if (e == P->s + P->i) { P->err = true; return 0; }
    P->i = (size_t)(e - P->s);
    while (isalpha((unsigned char)P->s[P->i])) P->i++;     /* a unit: [0px,inf] */
    return v;
}

static void g_mult(GParse *P, GNode *g) {
    for (;;) {
        char c = P->s[P->i];
        if (c == '?') { g->min = 0; g->max = 1; P->i++; }
        else if (c == '*') { g->min = 0; g->max = REP_INF; P->i++; }
        else if (c == '+') { g->min = 1; g->max = REP_INF; P->i++; }
        else if (c == '#') {
            g->comma = 1; P->i++;
            if (P->s[P->i] != '{') { g->min = 1; g->max = REP_INF; }
        }
        else if (c == '!') { g->required = 1; P->i++; }
        else if (c == '{') {
            P->i++;
            int a = (int)strtol(P->s + P->i, NULL, 10);
            while (isdigit((unsigned char)P->s[P->i])) P->i++;
            int b = a;
            if (P->s[P->i] == ',') {
                P->i++;
                if (isdigit((unsigned char)P->s[P->i])) {
                    b = (int)strtol(P->s + P->i, NULL, 10);
                    while (isdigit((unsigned char)P->s[P->i])) P->i++;
                } else b = REP_INF;
            }
            if (P->s[P->i] == '}') P->i++;
            g->min = (unsigned short)a;
            g->max = (unsigned short)b;
        }
        else break;
    }
}

static GNode *g_term(GParse *P) {
    gws(P);
    const char *s = P->s;
    char c = s[P->i];
    GNode *g = NULL;
    if (c == '[') {
        P->i++;
        g = g_alt(P);
        gws(P);
        if (s[P->i] == ']') P->i++; else P->err = true;
        if (g && g->min == 1 && g->max == 1 && !g->comma && g->kind != G_KW) {
            /* wrap so the group's own multipliers apply to the whole */
            GNode *w = gnode(G_SEQ);
            gadd(w, g);
            g = w;
        }
    } else if (c == '<') {
        P->i++;
        if (s[P->i] == '\'') {
            P->i++;
            size_t b = P->i;
            while (s[P->i] && s[P->i] != '\'') P->i++;
            g = gnode(G_PROP);
            g->text = strndup(s + b, P->i - b);
            if (s[P->i] == '\'') P->i++;
        } else {
            size_t b = P->i;
            while (s[P->i] && (isalnum((unsigned char)s[P->i]) || s[P->i] == '-' || s[P->i] == '(' || s[P->i] == ')')) P->i++;
            g = gnode(G_TYPE);
            g->text = strndup(s + b, P->i - b);
            gws(P);
            if (s[P->i] == '[') {
                P->i++;
                g->rmin = g_num(P);
                gws(P);
                if (s[P->i] == ',') P->i++;
                g->rmax = g_num(P);
                gws(P);
                if (s[P->i] == ']') P->i++;
                g->has_range = 1;
            }
        }
        gws(P);
        if (s[P->i] == '>') P->i++; else P->err = true;
    } else if (isalpha((unsigned char)c) || c == '-' || c == '_') {
        size_t b = P->i;
        while (s[P->i] && (isalnum((unsigned char)s[P->i]) || s[P->i] == '-' || s[P->i] == '_')) P->i++;
        if (s[P->i] == '(') {
            g = gnode(G_FUNC);
            g->text = strndup(s + b, P->i - b);
            P->i++;
            gws(P);
            if (s[P->i] == ')') { P->i++; }
            else {
                GNode *in = g_alt(P);
                if (in) gadd(g, in);
                gws(P);
                if (s[P->i] == ')') P->i++; else P->err = true;
            }
        } else {
            g = gnode(G_KW);
            g->text = strndup(s + b, P->i - b);
            for (char *q = g->text; *q; q++) *q = (char)tolower((unsigned char)*q);
        }
    } else if (c == '\'' ) {
        /* a quoted literal: ',' or '/' or '(' ... */
        P->i++;
        g = gnode(G_LIT);
        g->lit = s[P->i];
        while (s[P->i] && s[P->i] != '\'') P->i++;
        if (s[P->i] == '\'') P->i++;
    } else if (c == ',' || c == '/' || c == ':' || c == '=' || c == '+' || c == '*') {
        P->i++;
        g = gnode(G_LIT);
        g->lit = c;
    } else {
        P->err = true;
        return NULL;
    }
    if (g) g_mult(P, g);
    return g;
}

static bool g_at_end(GParse *P) {
    gws(P);
    char c = P->s[P->i];
    return !c || c == ']' || c == ')' || c == '|' || (c == '&' && P->s[P->i + 1] == '&');
}

static GNode *g_seq(GParse *P) {
    GNode *seq = gnode(G_SEQ);
    while (!g_at_end(P) && !P->err) {
        GNode *t = g_term(P);
        if (!t) break;
        gadd(seq, t);
    }
    if (seq->nk == 1) { GNode *only = seq->kids[0]; free(seq->kids); free(seq); only->idx = 0; return only; }
    return seq;
}

static GNode *g_all(GParse *P) {
    GNode *first = g_seq(P);
    gws(P);
    if (!(P->s[P->i] == '&' && P->s[P->i + 1] == '&')) return first;
    GNode *all = gnode(G_ALL);
    gadd(all, first);
    while (P->s[P->i] == '&' && P->s[P->i + 1] == '&') {
        P->i += 2;
        gadd(all, g_seq(P));
        gws(P);
    }
    return all;
}

static GNode *g_any(GParse *P) {
    GNode *first = g_all(P);
    gws(P);
    if (!(P->s[P->i] == '|' && P->s[P->i + 1] == '|')) return first;
    GNode *any = gnode(G_ANY);
    gadd(any, first);
    while (P->s[P->i] == '|' && P->s[P->i + 1] == '|') {
        P->i += 2;
        gadd(any, g_all(P));
        gws(P);
    }
    return any;
}

static GNode *g_alt(GParse *P) {
    GNode *first = g_any(P);
    gws(P);
    if (!(P->s[P->i] == '|' && P->s[P->i + 1] != '|')) return first;
    GNode *alt = gnode(G_ALT);
    gadd(alt, first);
    while (P->s[P->i] == '|' && P->s[P->i + 1] != '|') {
        P->i++;
        gadd(alt, g_any(P));
        gws(P);
    }
    return alt;
}

static GNode *g_compile(const char *src) {
    GParse P = { src, 0, false };
    GNode *g = g_alt(&P);
    gws(&P);
    if (P.err || P.s[P.i]) {
        fprintf(stderr, "[css_props] bad grammar at %zu: %s\n", P.i, src);
    }
    return g;
}

/* ============================================================================
 * Tables
 * ============================================================================ */

typedef struct CssTypeDef { const char *name, *grammar; } CssTypeDef;

#include "css_props_table.h"

#define NPROPS ((int)(sizeof(g_props) / sizeof(g_props[0])))
#define NTYPES ((int)(sizeof(g_types) / sizeof(g_types[0])))

static GNode *g_prop_grammar[NPROPS];
static GNode *g_type_grammar[NTYPES];

static int prop_cmp(const void *a, const void *b) {
    return strcmp(((const CssPropDef *)a)->name, ((const CssPropDef *)b)->name);
}
/* The table sorted by name (once, before any grammar is compiled). */
static void props_init(void) {
    static int done;
    if (done) return;
    qsort(g_props, NPROPS, sizeof g_props[0], prop_cmp);
    done = 1;
}

int css_prop_count(void) { return NPROPS; }
const CssPropDef *css_prop_at(int i) { props_init(); return i >= 0 && i < NPROPS ? &g_props[i] : NULL; }

static int prop_index(const char *name, size_t len) {
    props_init();
    int lo = 0, hi = NPROPS - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *m = g_props[mid].name;
        int c = strncasecmp(name, m, len);
        if (c == 0) c = m[len] ? -1 : 0;
        if (c == 0) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

const CssPropDef *css_prop_find(const char *name, size_t len) {
    if (!name) return NULL;
    int i = prop_index(name, len);
    if (i >= 0) return &g_props[i];
    for (size_t k = 0; k < sizeof(g_aliases) / sizeof(g_aliases[0]); k++)
        if (ieq(name, len, g_aliases[k][0])) return css_prop_find(g_aliases[k][1], strlen(g_aliases[k][1]));
    return NULL;
}

const char *css_prop_alias_at(int i, const CssPropDef **prop) {
    if (i < 0 || i >= (int)(sizeof g_aliases / sizeof g_aliases[0])) return NULL;
    if (prop) *prop = css_prop_find(g_aliases[i][1], strlen(g_aliases[i][1]));
    return g_aliases[i][0];
}

char *css_prop_camel(const char *name) {
    Buf b = { 0 };
    bool up = false;
    for (const char *p = name; *p; p++) {
        if (*p == '-') { up = b.n > 0; continue; }
        buf_ch(&b, up ? (char)toupper((unsigned char)*p) : *p);
        up = false;
    }
    return buf_take(&b);
}

const CssPropDef *css_prop_find_idl(const char *name, size_t len) {
    if (!name || !len) return NULL;
    if (ieq(name, len, "cssFloat")) return css_prop_find("float", 5);
    /* the dashed attribute: the name itself */
    for (size_t i = 0; i < len; i++) if (name[i] == '-') return css_prop_find(name, len);
    /* camelCase / webkitCased: each capital is "-" + its lower case */
    char tmp[128];
    size_t o = 0;
    if (len > 6 && strncmp(name, "webkit", 6) == 0 && isupper((unsigned char)name[6])) {
        memcpy(tmp, "-webkit", 7);
        o = 7;
        name += 6;
        len -= 6;
    }
    for (size_t i = 0; i < len && o + 2 < sizeof tmp; i++) {
        char c = name[i];
        if (isupper((unsigned char)c)) { tmp[o++] = '-'; tmp[o++] = (char)tolower((unsigned char)c); }
        else tmp[o++] = c;
    }
    if (o >= sizeof tmp - 1) return NULL;
    tmp[o] = '\0';
    /* an all-lower-case IDL name is only the dashed attribute of a one-word
     * property: "color" yes, "backgroundcolor" no */
    return css_prop_find(tmp, o);
}

bool css_prop_is_custom(const char *name, size_t len) {
    return len >= 2 && name[0] == '-' && name[1] == '-';
}

bool css_is_wide_keyword(const char *v, size_t n) {
    while (n && isspace((unsigned char)*v)) { v++; n--; }
    while (n && isspace((unsigned char)v[n - 1])) n--;
    return ieq(v, n, "initial") || ieq(v, n, "inherit") || ieq(v, n, "unset") || ieq(v, n, "revert") ||
           ieq(v, n, "revert-layer");
}

static GNode *prop_grammar(int i) {
    if (!g_prop_grammar[i]) g_prop_grammar[i] = g_compile(g_props[i].grammar);
    return g_prop_grammar[i];
}

static int type_index(const char *name) {
    for (int i = 0; i < NTYPES; i++) if (strcmp(g_types[i].name, name) == 0) return i;
    return -1;
}

static GNode *type_grammar(int i) {
    if (!g_type_grammar[i]) g_type_grammar[i] = g_compile(g_types[i].grammar);
    return g_type_grammar[i];
}

/* ============================================================================
 * Matching
 * ============================================================================ */

/* A node on the successful path: the tokens [start, end) it matched (as
 * sibling positions in the token array). */
typedef struct Hit {
    const GNode *node;
    int start, end;
    int parent;         /* index of the enclosing hit, -1 */
    int prop;           /* G_PROP: the property index */
} Hit;

typedef struct Matcher {
    const Toks *ts;
    Hit *hits;
    int nh, caph;
    int cur;            /* the open hit */
    int depth;
    long steps;         /* a bound on backtracking */
} Matcher;

typedef bool (*Cont)(Matcher *m, int pos, int end, void *ud);

static int hit_open(Matcher *m, const GNode *g, int start) {
    if (m->nh == m->caph) {
        int cap = m->caph ? m->caph * 2 : 64;
        Hit *nh = realloc(m->hits, (size_t)cap * sizeof(Hit));
        if (!nh) return -1;
        m->hits = nh;
        m->caph = cap;
    }
    Hit *h = &m->hits[m->nh];
    h->node = g;
    h->start = start;
    h->end = -1;
    h->parent = m->cur;
    h->prop = -1;
    m->cur = m->nh;
    return m->nh++;
}

static bool match_node(Matcher *m, const GNode *g, int pos, int end, Cont k, void *ud);

/* ---- numeric tokens and math functions ---- */

enum { D_NUMBER = 1, D_PERCENT = 2, D_LENGTH = 4, D_ANGLE = 8, D_TIME = 16, D_FREQ = 32, D_RES = 64, D_FLEX = 128 };

static int unit_dim(const char *u, double *scale) {
    static const struct { const char *u; int d; double s; } units[] = {
        { "px", D_LENGTH, 1 }, { "em", D_LENGTH, 0 }, { "rem", D_LENGTH, 0 }, { "ex", D_LENGTH, 0 },
        { "rex", D_LENGTH, 0 }, { "ch", D_LENGTH, 0 }, { "rch", D_LENGTH, 0 }, { "cap", D_LENGTH, 0 },
        { "rcap", D_LENGTH, 0 }, { "ic", D_LENGTH, 0 }, { "ric", D_LENGTH, 0 }, { "lh", D_LENGTH, 0 },
        { "rlh", D_LENGTH, 0 }, { "vw", D_LENGTH, 0 }, { "vh", D_LENGTH, 0 }, { "vi", D_LENGTH, 0 },
        { "vb", D_LENGTH, 0 }, { "vmin", D_LENGTH, 0 }, { "vmax", D_LENGTH, 0 }, { "svw", D_LENGTH, 0 },
        { "svh", D_LENGTH, 0 }, { "lvw", D_LENGTH, 0 }, { "lvh", D_LENGTH, 0 }, { "dvw", D_LENGTH, 0 },
        { "dvh", D_LENGTH, 0 }, { "svi", D_LENGTH, 0 }, { "svb", D_LENGTH, 0 }, { "lvi", D_LENGTH, 0 },
        { "lvb", D_LENGTH, 0 }, { "dvi", D_LENGTH, 0 }, { "dvb", D_LENGTH, 0 }, { "svmin", D_LENGTH, 0 },
        { "svmax", D_LENGTH, 0 }, { "lvmin", D_LENGTH, 0 }, { "lvmax", D_LENGTH, 0 }, { "dvmin", D_LENGTH, 0 },
        { "dvmax", D_LENGTH, 0 }, { "cqw", D_LENGTH, 0 }, { "cqh", D_LENGTH, 0 }, { "cqi", D_LENGTH, 0 },
        { "cqb", D_LENGTH, 0 }, { "cqmin", D_LENGTH, 0 }, { "cqmax", D_LENGTH, 0 },
        { "cm", D_LENGTH, 96.0 / 2.54 }, { "mm", D_LENGTH, 96.0 / 25.4 }, { "q", D_LENGTH, 96.0 / 101.6 },
        { "in", D_LENGTH, 96 }, { "pt", D_LENGTH, 96.0 / 72 }, { "pc", D_LENGTH, 16 },
        { "deg", D_ANGLE, 1 }, { "grad", D_ANGLE, 0.9 }, { "rad", D_ANGLE, 180 / M_PI }, { "turn", D_ANGLE, 360 },
        { "s", D_TIME, 1000 }, { "ms", D_TIME, 1 }, { "hz", D_FREQ, 1 }, { "khz", D_FREQ, 1000 },
        { "dpi", D_RES, 1.0 / 96 }, { "dpcm", D_RES, 2.54 / 96 }, { "dppx", D_RES, 1 }, { "x", D_RES, 1 },
        { "fr", D_FLEX, 1 },
    };
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++)
        if (strcasecmp(u, units[i].u) == 0) { if (scale) *scale = units[i].s; return units[i].d; }
    return 0;
}

static bool is_math_fn(const char *n) {
    static const char *const fns[] = { "calc", "min", "max", "clamp", "round", "mod", "rem", "abs", "sign",
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp",
        "-webkit-calc", "calc-size", NULL };
    for (int i = 0; fns[i]; i++) if (strcasecmp(n, fns[i]) == 0) return true;
    return false;
}

/* The type of a calc expression over tokens [b, e): a D_* mask with a
 * percent that may resolve against `pct` (0 when the context has none);
 * -1 when it is not a valid expression. */
static int calc_sum(const Toks *ts, int b, int e, int pct);

static int calc_value(const Toks *ts, int i, int pct) {
    const Tok *t = &ts->t[i];
    switch (t->type) {
    case T_NUMBER: return D_NUMBER;
    case T_PERCENT: return pct ? pct : D_PERCENT;
    case T_DIM: { int d = unit_dim(t->str, NULL); return d ? d : -1; }
    case T_IDENT:
        if (strcasecmp(t->str, "e") == 0 || strcasecmp(t->str, "pi") == 0 || strcasecmp(t->str, "infinity") == 0 ||
            strcasecmp(t->str, "-infinity") == 0 || strcasecmp(t->str, "nan") == 0) return D_NUMBER;
        return -1;
    case T_PAREN: return calc_sum(ts, t->first, t->end, pct);
    case T_FUNC: {
        if (!is_math_fn(t->str)) return -1;
        const char *f = t->str;
        if (strcasecmp(f, "calc") == 0 || strcasecmp(f, "-webkit-calc") == 0) return calc_sum(ts, t->first, t->end, pct);
        /* comma-separated arguments */
        int args[4], na = 0, s = t->first;
        for (int j = t->first; j <= t->end; j = j < t->end ? tok_next(ts, j) : j + 1) {
            if (j == t->end || ts->t[j].type == T_COMMA) {
                if (na == 4) return -1;
                if (j == s) {
                    if (strcasecmp(f, "round") == 0 && na == 0) { args[na++] = 0; s = j + 1; continue; }
                    return -1;
                }
                /* round()'s optional rounding strategy */
                if (strcasecmp(f, "round") == 0 && na == 0 && ts->t[s].type == T_IDENT && tok_next(ts, s) == j &&
                    (strcasecmp(ts->t[s].str, "nearest") == 0 || strcasecmp(ts->t[s].str, "up") == 0 ||
                     strcasecmp(ts->t[s].str, "down") == 0 || strcasecmp(ts->t[s].str, "to-zero") == 0)) {
                    s = j + 1;
                    continue;
                }
                int d = calc_sum(ts, s, j, pct);
                if (d < 0) return -1;
                args[na++] = d;
                s = j + 1;
            }
            if (j == t->end) break;
        }
        if (!na) return -1;
        if (strcasecmp(f, "sign") == 0) return D_NUMBER;
        if (!strcasecmp(f, "sin") || !strcasecmp(f, "cos") || !strcasecmp(f, "tan")) return (args[0] & (D_NUMBER | D_ANGLE)) ? D_NUMBER : -1;
        if (!strcasecmp(f, "asin") || !strcasecmp(f, "acos") || !strcasecmp(f, "atan") || !strcasecmp(f, "atan2")) return D_ANGLE;
        if (!strcasecmp(f, "pow") || !strcasecmp(f, "sqrt") || !strcasecmp(f, "log") || !strcasecmp(f, "exp")) return args[0] == D_NUMBER ? D_NUMBER : -1;
        int d = 0;
        for (int a = 0; a < na; a++) if (args[a]) { if (d && (d & ~D_PERCENT) && (args[a] & ~D_PERCENT) && (d & ~D_PERCENT) != (args[a] & ~D_PERCENT)) return -1; d |= args[a]; }
        if (strcasecmp(f, "clamp") == 0 && na != 3) return -1;
        return d;
    }
    default: return -1;
    }
}

static int calc_product(const Toks *ts, int b, int e, int pct) {
    int d = -1;
    bool divide = false, first = true;
    for (int i = b; i < e; ) {
        if (!first) {
            const Tok *op = &ts->t[i];
            if (op->type != T_DELIM || (op->delim != '*' && op->delim != '/')) return -1;
            divide = op->delim == '/';
            i++;
            if (i >= e) return -1;
        }
        int v = calc_value(ts, i, pct);
        if (v < 0) return -1;
        if (first) d = v;
        else if (divide) { if (v != D_NUMBER) return -1; }
        else if (d == D_NUMBER) d = v;
        else if (v != D_NUMBER) return -1;
        first = false;
        i = tok_next(ts, i);
    }
    return d;
}

static int calc_sum(const Toks *ts, int b, int e, int pct) {
    if (b >= e) return -1;
    int d = -1, s = b;
    for (int i = b; i <= e; i = i < e ? tok_next(ts, i) : i + 1) {
        bool op = i < e && i > s && ts->t[i].type == T_DELIM && (ts->t[i].delim == '+' || ts->t[i].delim == '-');
        /* + and - must have whitespace on both sides */
        if (op && (!ts->t[i].ws || i + 1 >= e || !ts->t[i + 1].ws)) return -1;
        if (i == e || op) {
            int v = calc_product(ts, s, i, pct);
            if (v < 0) return -1;
            if (d < 0) d = v;
            else {
                int a = d & ~D_PERCENT, c = v & ~D_PERCENT;
                if (a && c && a != c) return -1;
                if ((d == D_NUMBER) != (v == D_NUMBER)) return -1;
                d |= v;
            }
            s = i + 1;
        }
        if (i == e) break;
    }
    return d;
}

/* ---- the builtin types ---- */

static bool css_color_named(const char *name, const unsigned char **rgb);
static bool css_color_system(const char *name);
static bool color_tok_valid(const Toks *ts, int i);

static bool tok_ident(const Tok *t, const char *kw) { return t->type == T_IDENT && strcasecmp(t->str, kw) == 0; }

static const char *const g_css_wide[] = { "initial", "inherit", "unset", "revert", "revert-layer", "default", NULL };

static bool custom_ident_ok(const Tok *t) {
    if (t->type != T_IDENT) return false;
    for (int i = 0; g_css_wide[i]; i++) if (strcasecmp(t->str, g_css_wide[i]) == 0) return false;
    return true;
}

/* Builtin single-token types: does token i match <name [min,max]>? */
static int builtin_type(const GNode *g, const Toks *ts, int i) {
    const Tok *t = &ts->t[i];
    const char *n = g->text;
    double v = t->num;
    bool range = g->has_range;
    if (t->type == T_FUNC && is_math_fn(t->str)) {
        int want = 0, pct = 0;
        if (!strcmp(n, "number") || !strcmp(n, "integer") || !strcmp(n, "alpha-value")) want = D_NUMBER;
        else if (!strcmp(n, "length")) want = D_LENGTH;
        else if (!strcmp(n, "percentage")) want = D_PERCENT;
        else if (!strcmp(n, "length-percentage")) { want = D_LENGTH; pct = D_LENGTH; }
        else if (!strcmp(n, "angle")) want = D_ANGLE;
        else if (!strcmp(n, "angle-percentage")) { want = D_ANGLE; pct = D_ANGLE; }
        else if (!strcmp(n, "time")) want = D_TIME;
        else if (!strcmp(n, "time-percentage")) { want = D_TIME; pct = D_TIME; }
        else if (!strcmp(n, "frequency")) want = D_FREQ;
        else if (!strcmp(n, "resolution")) want = D_RES;
        else if (!strcmp(n, "flex")) want = D_FLEX;
        else if (!strcmp(n, "number-percentage")) { want = D_NUMBER; pct = D_NUMBER; }
        else return -1;
        int d = calc_value(ts, i, pct);
        if (d < 0) return 0;
        if (!strcmp(n, "alpha-value") && d == D_PERCENT) return 1;
        return d == want || (pct && d == (want | pct)) ? 1 : 0;
    }
    if (!strcmp(n, "number")) {
        if (t->type != T_NUMBER) return 0;
        return !range || (v >= g->rmin && v <= g->rmax);
    }
    if (!strcmp(n, "alpha-value")) return t->type == T_NUMBER || t->type == T_PERCENT;
    if (!strcmp(n, "integer")) {
        if (t->type != T_NUMBER || !t->is_int) return 0;
        return !range || (v >= g->rmin && v <= g->rmax);
    }
    if (!strcmp(n, "percentage")) {
        if (t->type != T_PERCENT) return 0;
        return !range || (v >= g->rmin && v <= g->rmax);
    }
    if (!strcmp(n, "length") || !strcmp(n, "length-percentage")) {
        bool ok = (t->type == T_DIM && unit_dim(t->str, NULL) == D_LENGTH) ||
                  (t->type == T_NUMBER && v == 0) ||
                  (t->type == T_PERCENT && n[6] == '-');
        if (!ok) return 0;
        return !range || (v >= g->rmin && v <= g->rmax);
    }
    if (!strcmp(n, "number-percentage")) {
        if (t->type != T_NUMBER && t->type != T_PERCENT) return 0;
        return !range || (v >= g->rmin && v <= g->rmax);
    }
    static const struct { const char *name; int d; } dims[] = {
        { "angle", D_ANGLE }, { "time", D_TIME }, { "frequency", D_FREQ }, { "resolution", D_RES }, { "flex", D_FLEX },
    };
    for (size_t k = 0; k < sizeof dims / sizeof dims[0]; k++) {
        if (strcmp(n, dims[k].name) == 0) {
            bool ok = t->type == T_DIM && unit_dim(t->str, NULL) == dims[k].d;
            if (!ok) return 0;
            return !range || (v >= g->rmin && v <= g->rmax);
        }
    }
    if (!strcmp(n, "angle-percentage")) {
        if (!((t->type == T_DIM && unit_dim(t->str, NULL) == D_ANGLE) || t->type == T_PERCENT)) return 0;
        return !range || (v >= g->rmin && v <= g->rmax);
    }
    if (!strcmp(n, "zero")) return (t->type == T_NUMBER && v == 0) ? 1 : 0;   /* a unitless 0 (as an <angle> in transforms) */
    if (!strcmp(n, "ident")) return t->type == T_IDENT;
    if (!strcmp(n, "custom-ident")) return custom_ident_ok(t);
    if (!strcmp(n, "dashed-ident")) return t->type == T_IDENT && t->str[0] == '-' && t->str[1] == '-';
    if (!strcmp(n, "string")) return t->type == T_STRING;
    if (!strcmp(n, "url")) {
        if (t->type == T_URL) return 1;
        if (t->type == T_FUNC && (strcasecmp(t->str, "url") == 0 || strcasecmp(t->str, "src") == 0)) {
            int j = t->first;
            return j < t->end && ts->t[j].type == T_STRING;
        }
        return 0;
    }
    if (!strcmp(n, "hex-color")) {
        if (t->type != T_HASH) return 0;
        size_t l = strlen(t->str);
        if (l != 3 && l != 4 && l != 6 && l != 8) return 0;
        for (size_t k = 0; k < l; k++) if (!isxdigit((unsigned char)t->str[k])) return 0;
        return 1;
    }
    if (!strcmp(n, "hash-token")) return t->type == T_HASH;
    if (!strcmp(n, "grid-ident")) return custom_ident_ok(t) && !tok_ident(t, "span") && !tok_ident(t, "auto");
    if (!strcmp(n, "nonzero-integer")) return t->type == T_NUMBER && t->is_int && v != 0;
    if (!strcmp(n, "line-names")) {
        /* [ <custom-ident>* ] (not span or auto) */
        if (t->type != T_BRACKET) return 0;
        for (int j = t->first; j < t->end; j++)
            if (!custom_ident_ok(&ts->t[j]) || tok_ident(&ts->t[j], "span") || tok_ident(&ts->t[j], "auto")) return 0;
        return 1;
    }
    if (!strcmp(n, "named-color")) return t->type == T_IDENT && css_color_named(t->str, NULL);
    if (!strcmp(n, "system-color")) return t->type == T_IDENT && css_color_system(t->str);
    if (!strcmp(n, "relative-color"))
        return t->type == T_FUNC && t->first < t->end && tok_ident(&ts->t[t->first], "from") &&
               color_tok_valid(ts, (int)(t - ts->t));
    if (!strcmp(n, "any-value")) return t->type != T_BAD && t->type != T_SEMI;
    return -1;      /* not a builtin */
}

/* ---- the matcher ---- */

typedef struct SeqUd { const GNode *g; int i; Cont k; void *ud; int hit; int saved_cur; } SeqUd;

static bool close_and_continue(Matcher *m, int hit, int pos, int end, Cont k, void *ud) {
    int saved_cur = m->cur;
    m->hits[hit].end = pos;
    m->cur = m->hits[hit].parent;
    if (k(m, pos, end, ud)) return true;
    m->hits[hit].end = -1;
    m->cur = saved_cur;
    return false;
}

/* Close hit, then continue with k (for nodes that hand their continuation on). */
typedef struct CloseUd { int hit; Cont k; void *ud; } CloseUd;
static bool close_cont(Matcher *m, int pos, int end, void *vud) {
    CloseUd *c = vud;
    return close_and_continue(m, c->hit, pos, end, c->k, c->ud);
}

/* One occurrence of g (without its multiplier). */
static bool match_once(Matcher *m, const GNode *g, int pos, int end, Cont k, void *ud);

typedef struct RepUd { const GNode *g; int count; Cont k; void *ud; int last; } RepUd;

static bool rep_step(Matcher *m, int pos, int end, void *vud);

static bool rep_try(Matcher *m, const GNode *g, int count, int pos, int end, Cont k, void *ud) {
    if (++m->steps > 200000) return false;
    /* more first (greedy), then stop if enough */
    if (count < g->max) {
        int p = pos;
        bool ok = true;
        if (count > 0 && g->comma) {
            if (p < end && m->ts->t[p].type == T_COMMA) p++;
            else ok = false;
        }
        if (ok && p < end) {
            RepUd r = { g, count + 1, k, ud, pos };
            if (match_once(m, g, p, end, rep_step, &r)) return true;
        }
    }
    if (count >= g->min) {
        if (g->required && count == 0) return false;
        return k(m, pos, end, ud);
    }
    return false;
}

static bool rep_step(Matcher *m, int pos, int end, void *vud) {
    RepUd *r = vud;
    if (pos == r->last && r->count > 1) return false;     /* no progress */
    return rep_try(m, r->g, r->count, pos, end, r->k, r->ud);
}

static bool match_node(Matcher *m, const GNode *g, int pos, int end, Cont k, void *ud) {
    if (g->min == 1 && g->max == 1 && !g->comma) return match_once(m, g, pos, end, k, ud);
    return rep_try(m, g, 0, pos, end, k, ud);
}

static bool seq_step(Matcher *m, int pos, int end, void *vud);

static bool seq_from(Matcher *m, const GNode *g, int i, int pos, int end, Cont k, void *ud, int hit) {
    if (i == g->nk) return close_and_continue(m, hit, pos, end, k, ud);
    SeqUd s = { g, i, k, ud, hit, 0 };
    return match_node(m, g->kids[i], pos, end, seq_step, &s);
}

static bool seq_step(Matcher *m, int pos, int end, void *vud) {
    SeqUd *s = vud;
    return seq_from(m, s->g, s->i + 1, pos, end, s->k, s->ud, s->hit);
}

typedef struct SetUd { const GNode *g; unsigned used; Cont k; void *ud; int hit; } SetUd;

static bool set_from(Matcher *m, const GNode *g, unsigned used, int pos, int end, Cont k, void *ud, int hit);

static bool set_step(Matcher *m, int pos, int end, void *vud) {
    SetUd *s = vud;
    return set_from(m, s->g, s->used, pos, end, s->k, s->ud, s->hit);
}

/* && (all, any order) and || (one or more, any order) */
static bool set_from(Matcher *m, const GNode *g, unsigned used, int pos, int end, Cont k, void *ud, int hit) {
    if (++m->steps > 200000) return false;
    unsigned all = g->nk >= 32 ? 0xFFFFFFFFu : ((1u << g->nk) - 1);
    for (int i = 0; i < g->nk; i++) {
        if (used & (1u << i)) continue;
        SetUd s = { g, used | (1u << i), k, ud, hit };
        int h0 = m->nh, c0 = m->cur;
        if (match_node(m, g->kids[i], pos, end, set_step, &s)) return true;
        m->nh = h0;
        m->cur = c0;
    }
    if (g->kind == G_ALL) {
        /* the members that may match nothing (a? or b*) count as there */
        for (int i = 0; i < g->nk; i++) if (!(used & (1u << i)) && g->kids[i]->min > 0) return false;
        (void)all;
        return close_and_continue(m, hit, pos, end, k, ud);
    }
    if (!used) return false;
    return close_and_continue(m, hit, pos, end, k, ud);
}

/* The end of a function's or block's arguments: all of them consumed. */
static bool at_end(Matcher *m, int pos, int end, void *ud) { (void)m; (void)ud; return pos == end; }

static bool match_once(Matcher *m, const GNode *g, int pos, int end, Cont k, void *ud) {
    if (++m->steps > 200000) return false;
    int h0 = m->nh, c0 = m->cur;
    int hit = hit_open(m, g, pos);
    if (hit < 0) return false;
    bool ok = false;
    const Toks *ts = m->ts;
    switch (g->kind) {
    case G_KW:
        if (pos < end && tok_ident(&ts->t[pos], g->text)) ok = close_and_continue(m, hit, pos + 1, end, k, ud);
        break;
    case G_LIT:
        if (pos < end) {
            const Tok *t = &ts->t[pos];
            bool lit = (g->lit == ',' && t->type == T_COMMA) || (g->lit == ':' && t->type == T_COLON) ||
                       (t->type == T_DELIM && t->delim == g->lit);
            if (lit) ok = close_and_continue(m, hit, pos + 1, end, k, ud);
        }
        break;
    case G_FUNC:
        if (pos < end && ts->t[pos].type == T_FUNC && strcasecmp(ts->t[pos].str, g->text) == 0) {
            const Tok *t = &ts->t[pos];
            bool inner;
            if (!g->nk) inner = t->first == t->end;
            else inner = match_node(m, g->kids[0], t->first, t->end, at_end, NULL);
            if (inner) ok = close_and_continue(m, hit, t->end, end, k, ud);
        }
        break;
    case G_BLOCK:
        break;
    case G_TYPE: {
        if (pos >= end) break;
        int b = builtin_type(g, ts, pos);
        if (b >= 0) {
            if (b) ok = close_and_continue(m, hit, tok_next(ts, pos), end, k, ud);
            break;
        }
        int ti = type_index(g->text);
        if (ti >= 0) {
            CloseUd c = { hit, k, ud };
            ok = match_node(m, type_grammar(ti), pos, end, close_cont, &c);
            break;
        }
        fprintf(stderr, "[css_props] unknown type <%s>\n", g->text);
        break;
    }
    case G_PROP: {
        int pi = prop_index(g->text, strlen(g->text));
        if (pi < 0) { fprintf(stderr, "[css_props] unknown property <'%s'>\n", g->text); break; }
        m->hits[hit].prop = pi;
        CloseUd c = { hit, k, ud };
        ok = match_node(m, prop_grammar(pi), pos, end, close_cont, &c);
        break;
    }
    case G_SEQ:
        ok = seq_from(m, g, 0, pos, end, k, ud, hit);
        break;
    case G_ALT:
        for (int i = 0; i < g->nk && !ok; i++) {
            int h1 = m->nh, c1 = m->cur;
            CloseUd c = { hit, k, ud };
            ok = match_node(m, g->kids[i], pos, end, close_cont, &c);
            if (!ok) { m->nh = h1; m->cur = c1; }
        }
        break;
    case G_ALL:
    case G_ANY:
        ok = set_from(m, g, 0, pos, end, k, ud, hit);
        break;
    }
    if (!ok) { m->nh = h0; m->cur = c0; }
    return ok;
}

/* ============================================================================
 * Colors
 * ============================================================================ */

static const struct { const char *name; unsigned char rgb[3]; } g_named_colors[] = {
    { "aliceblue", {240,248,255} }, { "antiquewhite", {250,235,215} }, { "aqua", {0,255,255} },
    { "aquamarine", {127,255,212} }, { "azure", {240,255,255} }, { "beige", {245,245,220} },
    { "bisque", {255,228,196} }, { "black", {0,0,0} }, { "blanchedalmond", {255,235,205} },
    { "blue", {0,0,255} }, { "blueviolet", {138,43,226} }, { "brown", {165,42,42} },
    { "burlywood", {222,184,135} }, { "cadetblue", {95,158,160} }, { "chartreuse", {127,255,0} },
    { "chocolate", {210,105,30} }, { "coral", {255,127,80} }, { "cornflowerblue", {100,149,237} },
    { "cornsilk", {255,248,220} }, { "crimson", {220,20,60} }, { "cyan", {0,255,255} },
    { "darkblue", {0,0,139} }, { "darkcyan", {0,139,139} }, { "darkgoldenrod", {184,134,11} },
    { "darkgray", {169,169,169} }, { "darkgreen", {0,100,0} }, { "darkgrey", {169,169,169} },
    { "darkkhaki", {189,183,107} }, { "darkmagenta", {139,0,139} }, { "darkolivegreen", {85,107,47} },
    { "darkorange", {255,140,0} }, { "darkorchid", {153,50,204} }, { "darkred", {139,0,0} },
    { "darksalmon", {233,150,122} }, { "darkseagreen", {143,188,143} }, { "darkslateblue", {72,61,139} },
    { "darkslategray", {47,79,79} }, { "darkslategrey", {47,79,79} }, { "darkturquoise", {0,206,209} },
    { "darkviolet", {148,0,211} }, { "deeppink", {255,20,147} }, { "deepskyblue", {0,191,255} },
    { "dimgray", {105,105,105} }, { "dimgrey", {105,105,105} }, { "dodgerblue", {30,144,255} },
    { "firebrick", {178,34,34} }, { "floralwhite", {255,250,240} }, { "forestgreen", {34,139,34} },
    { "fuchsia", {255,0,255} }, { "gainsboro", {220,220,220} }, { "ghostwhite", {248,248,255} },
    { "gold", {255,215,0} }, { "goldenrod", {218,165,32} }, { "gray", {128,128,128} },
    { "green", {0,128,0} }, { "greenyellow", {173,255,47} }, { "grey", {128,128,128} },
    { "honeydew", {240,255,240} }, { "hotpink", {255,105,180} }, { "indianred", {205,92,92} },
    { "indigo", {75,0,130} }, { "ivory", {255,255,240} }, { "khaki", {240,230,140} },
    { "lavender", {230,230,250} }, { "lavenderblush", {255,240,245} }, { "lawngreen", {124,252,0} },
    { "lemonchiffon", {255,250,205} }, { "lightblue", {173,216,230} }, { "lightcoral", {240,128,128} },
    { "lightcyan", {224,255,255} }, { "lightgoldenrodyellow", {250,250,210} }, { "lightgray", {211,211,211} },
    { "lightgreen", {144,238,144} }, { "lightgrey", {211,211,211} }, { "lightpink", {255,182,193} },
    { "lightsalmon", {255,160,122} }, { "lightseagreen", {32,178,170} }, { "lightskyblue", {135,206,250} },
    { "lightslategray", {119,136,153} }, { "lightslategrey", {119,136,153} }, { "lightsteelblue", {176,196,222} },
    { "lightyellow", {255,255,224} }, { "lime", {0,255,0} }, { "limegreen", {50,205,50} },
    { "linen", {250,240,230} }, { "magenta", {255,0,255} }, { "maroon", {128,0,0} },
    { "mediumaquamarine", {102,205,170} }, { "mediumblue", {0,0,205} }, { "mediumorchid", {186,85,211} },
    { "mediumpurple", {147,112,219} }, { "mediumseagreen", {60,179,113} }, { "mediumslateblue", {123,104,238} },
    { "mediumspringgreen", {0,250,154} }, { "mediumturquoise", {72,209,204} }, { "mediumvioletred", {199,21,133} },
    { "midnightblue", {25,25,112} }, { "mintcream", {245,255,250} }, { "mistyrose", {255,228,225} },
    { "moccasin", {255,228,181} }, { "navajowhite", {255,222,173} }, { "navy", {0,0,128} },
    { "oldlace", {253,245,230} }, { "olive", {128,128,0} }, { "olivedrab", {107,142,35} },
    { "orange", {255,165,0} }, { "orangered", {255,69,0} }, { "orchid", {218,112,214} },
    { "palegoldenrod", {238,232,170} }, { "palegreen", {152,251,152} }, { "paleturquoise", {175,238,238} },
    { "palevioletred", {219,112,147} }, { "papayawhip", {255,239,213} }, { "peachpuff", {255,218,185} },
    { "peru", {205,133,63} }, { "pink", {255,192,203} }, { "plum", {221,160,221} },
    { "powderblue", {176,224,230} }, { "purple", {128,0,128} }, { "rebeccapurple", {102,51,153} },
    { "red", {255,0,0} }, { "rosybrown", {188,143,143} }, { "royalblue", {65,105,225} },
    { "saddlebrown", {139,69,19} }, { "salmon", {250,128,114} }, { "sandybrown", {244,164,96} },
    { "seagreen", {46,139,87} }, { "seashell", {255,245,238} }, { "sienna", {160,82,45} },
    { "silver", {192,192,192} }, { "skyblue", {135,206,235} }, { "slateblue", {106,90,205} },
    { "slategray", {112,128,144} }, { "slategrey", {112,128,144} }, { "snow", {255,250,250} },
    { "springgreen", {0,255,127} }, { "steelblue", {70,130,180} }, { "tan", {210,180,140} },
    { "teal", {0,128,128} }, { "thistle", {216,191,216} }, { "tomato", {255,99,71} },
    { "turquoise", {64,224,208} }, { "violet", {238,130,238} }, { "wheat", {245,222,179} },
    { "white", {255,255,255} }, { "whitesmoke", {245,245,245} }, { "yellow", {255,255,0} },
    { "yellowgreen", {154,205,50} },
};

static bool css_color_named(const char *name, const unsigned char **rgb) {
    int lo = 0, hi = (int)(sizeof g_named_colors / sizeof g_named_colors[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcasecmp(name, g_named_colors[mid].name);
        if (!c) { if (rgb) *rgb = g_named_colors[mid].rgb; return true; }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return false;
}

static bool css_color_system(const char *name) {
    static const char *const sys[] = { "accentcolor", "accentcolortext", "activetext", "buttonborder",
        "buttonface", "buttontext", "canvas", "canvastext", "field", "fieldtext", "graytext", "highlight",
        "highlighttext", "linktext", "mark", "marktext", "selecteditem", "selecteditemtext", "visitedtext",
        /* deprecated, still valid */
        "activeborder", "activecaption", "appworkspace", "background", "buttonhighlight", "buttonshadow",
        "captiontext", "inactiveborder", "inactivecaption", "inactivecaptiontext", "infobackground",
        "infotext", "menu", "menutext", "scrollbar", "threeddarkshadow", "threedface", "threedhighlight",
        "threedlightshadow", "threedshadow", "window", "windowframe", "windowtext", NULL };
    for (int i = 0; sys[i]; i++) if (strcasecmp(name, sys[i]) == 0) return true;
    return false;
}

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }


static void fmt_alpha(Buf *b, double a) {
    /* the shortest decimal that keeps the 8-bit alpha */
    int a8 = (int)lround(clamp01(a) * 255);
    char tmp[32];
    for (int digits = 1; digits <= 6; digits++) {
        snprintf(tmp, sizeof tmp, "%.*f", digits, a8 / 255.0);
        if ((int)lround(strtod(tmp, NULL) * 255) == a8) break;
    }
    char *e = tmp + strlen(tmp) - 1;
    while (e > tmp && *e == '0') *e-- = '\0';
    if (*e == '.') *e = '\0';
    buf_str(b, tmp);
}

static void fmt_rgba(Buf *b, const double rgba[4]) {
    char tmp[64];
    int r = (int)lround(rgba[0]), g = (int)lround(rgba[1]), bl = (int)lround(rgba[2]);
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    bl = bl < 0 ? 0 : bl > 255 ? 255 : bl;
    if (lround(clamp01(rgba[3]) * 255) == 255) {
        snprintf(tmp, sizeof tmp, "rgb(%d, %d, %d)", r, g, bl);
        buf_str(b, tmp);
    } else {
        snprintf(tmp, sizeof tmp, "rgba(%d, %d, %d, ", r, g, bl);
        buf_str(b, tmp);
        fmt_alpha(b, rgba[3]);
        buf_ch(b, ')');
    }
}

char *css_color_serialize_rgba(const double rgba[4]) {
    Buf b = { 0 };
    fmt_rgba(&b, rgba);
    return buf_take(&b);
}

/* ============================================================================
 * Serializing a match
 * ============================================================================ */

typedef struct Ser {
    const Toks *ts;
    const Hit *hits;
    int nh;
    int **kids;         /* direct children of each hit */
    int *nkids;
} Ser;

static void ser_hit(Ser *S, int h, Buf *out);

static void ser_ident(Buf *b, const char *s) {
    /* CSSOM "serialize an identifier" */
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        bool first = i == 0, second = i == 1;
        if (isdigit(c) && (first || (second && s[0] == '-'))) {
            char tmp[8];
            snprintf(tmp, sizeof tmp, "\\%x ", c);
            buf_str(b, tmp);
        } else if (c >= 0x80 || isalnum(c) || c == '-' || c == '_') {
            if (first && c == '-' && !s[1]) buf_str(b, "\\-");
            else buf_ch(b, (char)c);
        } else if (c < 0x20 || c == 0x7f) {
            char tmp[8];
            snprintf(tmp, sizeof tmp, "\\%x ", c);
            buf_str(b, tmp);
        } else {
            buf_ch(b, '\\');
            buf_ch(b, (char)c);
        }
    }
}

static void ser_string(Buf *b, const char *s) {
    buf_ch(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { buf_ch(b, '\\'); buf_ch(b, (char)*p); }
        else if (*p < 0x20 || *p == 0x7f) { char tmp[8]; snprintf(tmp, sizeof tmp, "\\%x ", *p); buf_str(b, tmp); }
        else buf_ch(b, (char)*p);
    }
    buf_ch(b, '"');
}

/* Serialize the sibling tokens [b, e), with the hits in kids (sorted by
 * start) standing for their spans. */
static void ser_range(Ser *S, int b, int e, const int *kids, int nk, Buf *out);

static bool calc_serialize(const Toks *ts, int i, Buf *b);
static bool calc_unit_canon(const char *u, double *scale, char *out, size_t cap);
/* Set while computing a value (css_prop_compute): relative lengths resolve. */
static __thread const CssComputeEnv *g_env;

static void ser_tok(Ser *S, int i, Buf *b, const int *kids, int nk) {
    const Tok *t = &S->ts->t[i];
    switch (t->type) {
    case T_IDENT: ser_ident(b, t->str); break;
    case T_FUNC: {
        if (is_math_fn(t->str) && calc_serialize(S->ts, i, b)) break;
        char *lower = strdup(t->str);
        for (char *p = lower; *p; p++) *p = (char)tolower((unsigned char)*p);
        buf_str(b, lower);
        free(lower);
        buf_ch(b, '(');
        ser_range(S, t->first, t->end, kids, nk, b);
        buf_ch(b, ')');
        break;
    }
    case T_AT: buf_ch(b, '@'); buf_str(b, t->str); break;
    case T_HASH: buf_ch(b, '#'); buf_str(b, t->str); break;
    case T_STRING: ser_string(b, t->str); break;
    case T_URL: buf_str(b, "url("); ser_string(b, t->str); buf_ch(b, ')'); break;
    case T_NUMBER: fmt_number(b, t->num); break;
    case T_PERCENT: fmt_number(b, t->num); buf_ch(b, '%'); break;
    case T_DIM: {
        double sc;
        char u[12];
        if (g_env && calc_unit_canon(t->str, &sc, u, sizeof u)) {
            /* a computed dimension: in its canonical unit */
            fmt_number(b, t->num * sc);
            buf_str(b, u);
            break;
        }
        fmt_number(b, t->num);
        for (const char *p = t->str; *p; p++) buf_ch(b, (char)tolower((unsigned char)*p));
        break;
    }
    case T_DELIM: buf_ch(b, t->delim); break;
    case T_COMMA: buf_ch(b, ','); break;
    case T_COLON: buf_ch(b, ':'); break;
    case T_SEMI: buf_ch(b, ';'); break;
    case T_PAREN: buf_ch(b, '('); ser_range(S, t->first, t->end, kids, nk, b); buf_ch(b, ')'); break;
    case T_BRACKET: buf_ch(b, '['); ser_range(S, t->first, t->end, kids, nk, b); buf_ch(b, ']'); break;
    case T_BRACE: buf_ch(b, '{'); ser_range(S, t->first, t->end, kids, nk, b); buf_ch(b, '}'); break;
    default: break;
    }
}

static void ser_range(Ser *S, int b, int e, const int *kids, int nk, Buf *out) {
    bool first = true;
    int k = 0;
    for (int i = b; i < e; ) {
        while (k < nk && S->hits[kids[k]].start < i) k++;
        const Tok *t = &S->ts->t[i];
        bool comma = t->type == T_COMMA;
        bool slash = t->type == T_DELIM && t->delim == '/';
        (void)slash;
        if (!first && !comma) buf_ch(out, ' ');
        first = false;
        if (k < nk && S->hits[kids[k]].start == i && S->hits[kids[k]].end > i) {
            int h = kids[k++];
            ser_hit(S, h, out);
            i = S->hits[h].end;
            continue;
        }
        ser_tok(S, i, out, kids + k, nk - k);
        i = tok_next(S->ts, i);
    }
}

static bool hit_is_type(const Hit *h, const char *name) {
    return h->node->kind == G_TYPE && strcmp(h->node->text, name) == 0;
}

/* ---- calc() simplification (CSS Values 4 §10.10): a sum of terms, each
 * a number times a canonical unit ("" number, "%", "px", "deg", ...) ---- */

typedef struct CalcTerm { double v; char unit[12]; } CalcTerm;
typedef struct CalcSum { CalcTerm t[16]; int n; } CalcSum;


/* The px of one relative length unit u under g_env (0 when not one). */
static double relative_px(const char *u) {
    const CssComputeEnv *e = g_env;
    double em = e->font_size, vw = e->vw / 100, vh = e->vh / 100;
    static const struct { const char *u; int k; double f; } rel[] = {
        { "em", 0, 1 }, { "rem", 1, 1 }, { "ex", 0, 0.5 }, { "rex", 1, 0.5 }, { "ch", 0, 0.5 }, { "rch", 1, 0.5 },
        { "cap", 0, 0.7 }, { "rcap", 1, 0.7 }, { "ic", 0, 1 }, { "ric", 1, 1 }, { "lh", 0, 1.2 }, { "rlh", 1, 1.2 },
        { "vw", 2, 1 }, { "svw", 2, 1 }, { "lvw", 2, 1 }, { "dvw", 2, 1 }, { "vi", 2, 1 }, { "svi", 2, 1 },
        { "lvi", 2, 1 }, { "dvi", 2, 1 }, { "cqw", 2, 1 }, { "cqi", 2, 1 },
        { "vh", 3, 1 }, { "svh", 3, 1 }, { "lvh", 3, 1 }, { "dvh", 3, 1 }, { "vb", 3, 1 }, { "svb", 3, 1 },
        { "lvb", 3, 1 }, { "dvb", 3, 1 }, { "cqh", 3, 1 }, { "cqb", 3, 1 },
        { "vmin", 4, 1 }, { "svmin", 4, 1 }, { "lvmin", 4, 1 }, { "dvmin", 4, 1 }, { "cqmin", 4, 1 },
        { "vmax", 5, 1 }, { "svmax", 5, 1 }, { "lvmax", 5, 1 }, { "dvmax", 5, 1 }, { "cqmax", 5, 1 },
    };
    for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        if (strcasecmp(u, rel[i].u)) continue;
        double base = rel[i].k == 0 ? em : rel[i].k == 1 ? e->root_font_size : rel[i].k == 2 ? vw :
                      rel[i].k == 3 ? vh : rel[i].k == 4 ? (vw < vh ? vw : vh) : (vw > vh ? vw : vh);
        return base * rel[i].f;
    }
    return 0;
}

static bool calc_unit_canon(const char *u, double *scale, char *out, size_t cap) {
    double s = 0;
    int d = unit_dim(u, &s);
    if (!d) return false;
    if (s == 0 && g_env && d == D_LENGTH) {     /* computing: relative lengths in px */
        snprintf(out, cap, "px");
        *scale = relative_px(u);
        return true;
    }
    if (s == 0) {   /* relative: kept as written (lower case) */
        snprintf(out, cap, "%s", u);
        for (char *p = out; *p; p++) *p = (char)tolower((unsigned char)*p);
        *scale = 1;
        return true;
    }
    const char *canon = d == D_LENGTH ? "px" : d == D_ANGLE ? "deg" : d == D_TIME ? "s" : d == D_FREQ ? "hz" :
                        d == D_RES ? "dppx" : "fr";
    snprintf(out, cap, "%s", canon);
    *scale = d == D_TIME ? s / 1000 : s;
    return true;
}

static bool calc_add(CalcSum *s, double v, const char *unit) {
    for (int i = 0; i < s->n; i++) if (!strcmp(s->t[i].unit, unit)) { s->t[i].v += v; return true; }
    if (s->n == 16) return false;
    s->t[s->n].v = v;
    snprintf(s->t[s->n].unit, sizeof s->t[s->n].unit, "%s", unit);
    s->n++;
    return true;
}

static bool calc_simplify_sum(const Toks *ts, int b, int e, CalcSum *out);
static bool color_var(const char *name, double *v);

static bool calc_math_function(const Toks *ts, const Tok *f, CalcSum *out);

static bool calc_simplify_value(const Toks *ts, int i, CalcSum *out) {
    const Tok *t = &ts->t[i];
    memset(out, 0, sizeof *out);
    switch (t->type) {
    case T_NUMBER: return calc_add(out, t->num, "");
    case T_PERCENT: return calc_add(out, t->num, "%");
    case T_DIM: {
        double sc;
        char u[12];
        if (!calc_unit_canon(t->str, &sc, u, sizeof u)) return false;
        return calc_add(out, t->num * sc, u);
    }
    case T_PAREN: return calc_simplify_sum(ts, t->first, t->end, out);
    case T_IDENT:
        /* the constants (CSS Values 4 §10.7.1) */
        if (!strcasecmp(t->str, "e")) return calc_add(out, M_E, "");
        if (!strcasecmp(t->str, "pi")) return calc_add(out, M_PI, "");
        if (!strcasecmp(t->str, "infinity")) return calc_add(out, INFINITY, "");
        if (!strcasecmp(t->str, "-infinity")) return calc_add(out, -INFINITY, "");
        if (!strcasecmp(t->str, "nan")) return calc_add(out, NAN, "");
        {
            double v;
            if (color_var(t->str, &v)) return calc_add(out, v, "");    /* a relative color's channel */
        }
        return false;
    case T_FUNC:
        if (strcasecmp(t->str, "calc") == 0) return calc_simplify_sum(ts, t->first, t->end, out);
        return calc_math_function(ts, t, out);
    default: return false;
    }
}

/* One term (a sum that simplified to a single value), or false. */
static bool calc_one(const CalcSum *s, double *v, const char **unit) {
    if (s->n != 1) return false;
    *v = s->t[0].v;
    *unit = s->t[0].unit;
    return true;
}

/* A math function other than calc() whose arguments resolve (CSS Values 4
 * §10.3-10.8): its value as a one-term sum. */
static bool calc_math_function(const Toks *ts, const Tok *f, CalcSum *out) {
    const char *name = f->str;
    /* the comma-separated arguments; round()'s first may be a strategy */
    CalcSum args[8];
    int na = 0, strategy = 0;   /* 0 nearest, 1 up, 2 down, 3 to-zero */
    int s = f->first;
    for (int j = f->first; ; j = j < f->end ? tok_next(ts, j) : j + 1) {
        if (j >= f->end || ts->t[j].type == T_COMMA) {
            if (j == s || na == 8) return false;
            if (!strcasecmp(name, "round") && na == 0 && tok_next(ts, s) == j && ts->t[s].type == T_IDENT &&
                (tok_ident(&ts->t[s], "nearest") || tok_ident(&ts->t[s], "up") || tok_ident(&ts->t[s], "down") ||
                 tok_ident(&ts->t[s], "to-zero"))) {
                strategy = tok_ident(&ts->t[s], "up") ? 1 : tok_ident(&ts->t[s], "down") ? 2 :
                           tok_ident(&ts->t[s], "to-zero") ? 3 : 0;
            } else if (!calc_simplify_sum(ts, s, j, &args[na++])) return false;
            s = j + 1;
            if (j >= f->end) break;
        }
    }
    if (!na) return false;
    double v[8];
    const char *u[8];
    for (int i = 0; i < na; i++) if (!calc_one(&args[i], &v[i], &u[i])) return false;
    /* functions over values of one type take arguments of the same unit */
    #define SAME_UNITS() do { for (int i = 1; i < na; i++) if (strcmp(u[i], u[0])) return false; } while (0)
    memset(out, 0, sizeof *out);
    double r;
    const char *unit = u[0];
    if (!strcasecmp(name, "min") || !strcasecmp(name, "max")) {
        SAME_UNITS();
        r = v[0];
        for (int i = 1; i < na; i++) {
            if (isnan(v[i])) { r = NAN; break; }
            r = !strcasecmp(name, "min") ? (v[i] < r ? v[i] : r) : (v[i] > r ? v[i] : r);
        }
    } else if (!strcasecmp(name, "clamp")) {
        if (na != 3) return false;
        SAME_UNITS();
        r = v[1] < v[0] ? v[0] : v[1];
        if (r > v[2]) r = v[2];
        if (v[0] > v[2]) r = v[0];
        if (isnan(v[0]) || isnan(v[1]) || isnan(v[2])) r = NAN;
    } else if (!strcasecmp(name, "abs")) {
        if (na != 1) return false;
        r = fabs(v[0]);
    } else if (!strcasecmp(name, "sign")) {
        if (na != 1) return false;
        r = v[0] > 0 ? 1 : v[0] < 0 ? -1 : v[0];   /* (keeps -0, 0 and NaN) */
        unit = "";
    } else if (!strcasecmp(name, "round") || !strcasecmp(name, "mod") || !strcasecmp(name, "rem")) {
        double a = v[0], b = na > 1 ? v[1] : 1;
        if (na > 2 || (na == 1 && (u[0][0] || strcasecmp(name, "round")))) return false;
        if (!strcasecmp(name, "round")) b = fabs(b);    /* (the step's sign does not matter) */
        if (na == 2) SAME_UNITS();
        if (!strcasecmp(name, "round")) {
            if (b == 0 || isnan(a) || isnan(b)) r = NAN;
            else if (isinf(b)) r = isinf(a) ? NAN : (strategy == 1 ? (a > 0 ? INFINITY : -0.0) :
                                                   strategy == 2 ? (a < 0 ? -INFINITY : 0.0) : (a < 0 ? -0.0 : 0.0));
            else if (isinf(a)) r = a;
            else {
                double q = a / b, lo = floor(q) * b, hi = ceil(q) * b;
                switch (strategy) {
                case 1: r = hi; break;
                case 2: r = lo; break;
                case 3: r = fabs(lo) < fabs(hi) ? lo : hi; break;
                default: r = (a - lo) < (hi - a) ? lo : hi; break;
                }
                if (r == 0 && a < 0) r = -0.0;
            }
        } else {
            if (b == 0 || isinf(a) || isnan(a) || isnan(b)) r = NAN;
            else if (isinf(b)) r = !strcasecmp(name, "rem") || (a >= 0) == (b > 0) ? a : NAN;
            else r = !strcasecmp(name, "mod") ? a - b * floor(a / b) : a - b * trunc(a / b);
        }
    } else if (!strcasecmp(name, "sin") || !strcasecmp(name, "cos") || !strcasecmp(name, "tan")) {
        if (na != 1 || (u[0][0] && strcmp(u[0], "deg"))) return false;
        double x = u[0][0] ? v[0] * M_PI / 180 : v[0];
        r = !strcasecmp(name, "sin") ? sin(x) : !strcasecmp(name, "cos") ? cos(x) : tan(x);
        if (!strcasecmp(name, "tan") && u[0][0] && fmod(fabs(v[0]) - 90, 180) == 0) r = v[0] > 0 ? INFINITY : -INFINITY;
        unit = "";
    } else if (!strcasecmp(name, "asin") || !strcasecmp(name, "acos") || !strcasecmp(name, "atan")) {
        if (na != 1 || u[0][0]) return false;
        r = (!strcasecmp(name, "asin") ? asin(v[0]) : !strcasecmp(name, "acos") ? acos(v[0]) : atan(v[0])) * 180 / M_PI;
        unit = "deg";
    } else if (!strcasecmp(name, "atan2")) {
        if (na != 2) return false;
        SAME_UNITS();
        r = atan2(v[0], v[1]) * 180 / M_PI;
        unit = "deg";
    } else if (!strcasecmp(name, "pow")) {
        if (na != 2 || u[0][0] || u[1][0]) return false;
        r = pow(v[0], v[1]);
    } else if (!strcasecmp(name, "sqrt")) {
        if (na != 1 || u[0][0]) return false;
        r = sqrt(v[0]);
    } else if (!strcasecmp(name, "hypot")) {
        SAME_UNITS();
        r = 0;
        for (int i = 0; i < na; i++) r += v[i] * v[i];
        r = sqrt(r);
        for (int i = 0; i < na; i++) if (isinf(v[i])) r = INFINITY;
    } else if (!strcasecmp(name, "log")) {
        if (na > 2 || u[0][0] || (na == 2 && u[1][0])) return false;
        r = na == 2 ? log(v[0]) / log(v[1]) : log(v[0]);
    } else if (!strcasecmp(name, "exp")) {
        if (na != 1 || u[0][0]) return false;
        r = exp(v[0]);
    } else return false;
    #undef SAME_UNITS
    /* (a percentage or relative length the function compared or combined
     * cannot be resolved here: only units that are the same everywhere) */
    if (!strcmp(u[0], "%") && (!strcasecmp(name, "abs") || !strcasecmp(name, "sign"))) return false;
    out->n = 1;
    out->t[0].v = r;
    snprintf(out->t[0].unit, sizeof out->t[0].unit, "%s", unit);
    return true;
}

static bool calc_simplify_product(const Toks *ts, int b, int e, CalcSum *out) {
    bool first = true;
    for (int i = b; i < e; ) {
        bool divide = false;
        if (!first) {
            const Tok *op = &ts->t[i];
            if (op->type != T_DELIM || (op->delim != '*' && op->delim != '/')) return false;
            divide = op->delim == '/';
            if (++i >= e) return false;
        }
        CalcSum v;
        if (!calc_simplify_value(ts, i, &v)) return false;
        if (first) *out = v;
        else {
            /* one side must be a plain number */
            bool vnum = v.n == 1 && !v.t[0].unit[0], onum = out->n == 1 && !out->t[0].unit[0];
            if (divide) {
                if (!vnum || v.t[0].v == 0) return false;
                for (int k = 0; k < out->n; k++) out->t[k].v /= v.t[0].v;
            } else if (vnum) {
                for (int k = 0; k < out->n; k++) out->t[k].v *= v.t[0].v;
            } else if (onum) {
                double f = out->t[0].v;
                *out = v;
                for (int k = 0; k < out->n; k++) out->t[k].v *= f;
            } else return false;
        }
        first = false;
        i = tok_next(ts, i);
    }
    return !first;
}

static bool calc_simplify_sum(const Toks *ts, int b, int e, CalcSum *out) {
    memset(out, 0, sizeof *out);
    int s = b;
    double sign = 1;
    for (int i = b; i <= e; i = i < e ? tok_next(ts, i) : i + 1) {
        bool op = i < e && i > s && ts->t[i].type == T_DELIM && (ts->t[i].delim == '+' || ts->t[i].delim == '-');
        if (i == e || op) {
            CalcSum p;
            if (!calc_simplify_product(ts, s, i, &p)) return false;
            for (int k = 0; k < p.n; k++) if (!calc_add(out, sign * p.t[k].v, p.t[k].unit)) return false;
            if (op) sign = ts->t[i].delim == '-' ? -1 : 1;
            s = i + 1;
        }
        if (i == e) break;
    }
    return true;
}

static int calc_term_cmp(const void *a, const void *b) {
    const CalcTerm *x = a, *y = b;
    int rx = !x->unit[0] ? 0 : !strcmp(x->unit, "%") ? 1 : 2, ry = !y->unit[0] ? 0 : !strcmp(y->unit, "%") ? 1 : 2;
    if (rx != ry) return rx - ry;
    return strcasecmp(x->unit, y->unit);
}

/* calc(...) at token i, simplified into b; false to keep it as written. */
static bool calc_serialize(const Toks *ts, int i, Buf *b) {
    CalcSum s;
    if (!calc_simplify_value(ts, i, &s)) return false;
    /* (zero terms of a sum with others go) */
    int n = 0;
    for (int k = 0; k < s.n; k++) if (s.t[k].v != 0 || s.n == 1) s.t[n++] = s.t[k];
    if (!n) { s.n = 1; n = 1; }
    s.n = n;
    qsort(s.t, (size_t)s.n, sizeof s.t[0], calc_term_cmp);
    if (g_env && s.n == 1 && strcmp(s.t[0].unit, "%")) {
        /* a computed calc() of one term is that value; NaN is censored to
         * 0 and infinities clamped to the largest finite values */
        double v = s.t[0].v;
        if (isnan(v)) v = 0;
        else if (isinf(v)) v = v > 0 ? 3.4028234663852886e38 : -3.4028234663852886e38;
        fmt_number(b, v);
        buf_str(b, s.t[0].unit);
        return true;
    }
    buf_str(b, "calc(");
    for (int k = 0; k < s.n; k++) {
        double v = s.t[k].v;
        if (k) { buf_str(b, v < 0 ? " - " : " + "); v = fabs(v); }
        if (!isfinite(v)) {
            /* infinity and NaN: a keyword times one of the unit */
            buf_str(b, isnan(v) ? "NaN" : v > 0 ? "infinity" : "-infinity");
            if (s.t[k].unit[0]) { buf_str(b, " * 1"); buf_str(b, s.t[k].unit); }
            continue;
        }
        fmt_number(b, v);
        buf_str(b, s.t[k].unit);
    }
    buf_ch(b, ')');
    return true;
}

/* ============================================================================
 * Colors (CSS Color 4 and 5): every syntax as a color space and its channels,
 * conversions between the spaces, relative colors and color-mix()
 * ============================================================================ */

enum { CS_SRGB, CS_SRGB_LINEAR, CS_P3, CS_P3_LINEAR, CS_A98, CS_PROPHOTO, CS_REC2020,
       CS_XYZ_D50, CS_XYZ_D65, CS_LAB, CS_LCH, CS_OKLAB, CS_OKLCH, CS_HSL, CS_HWB, CS_N };
static const char *const cs_name[CS_N] = {
    "srgb", "srgb-linear", "display-p3", "display-p3-linear", "a98-rgb", "prophoto-rgb", "rec2020",
    "xyz-d50", "xyz-d65", "lab", "lch", "oklab", "oklch", "hsl", "hwb",
};

typedef struct Color {
    int cs;
    double c[3], alpha;
    unsigned none;      /* bit i: channel i missing; bit 3: the alpha */
    bool legacy;        /* hex, named, rgb(), hsl(), hwb(): serialized as rgb()/rgba() */
    bool derived;       /* relative or color-mix(): specified values keep their form */
} Color;

static int cs_hue(int cs) { return cs == CS_HSL || cs == CS_HWB ? 0 : cs == CS_LCH || cs == CS_OKLCH ? 2 : -1; }

static double norm_hue(double h) {
    if (!isfinite(h)) return 0;
    h = fmod(h, 360);
    return h < 0 ? h + 360 : h;
}

static void mat3(const double m[9], const double v[3], double o[3]) {
    double r[3];
    for (int i = 0; i < 3; i++) r[i] = m[3 * i] * v[0] + m[3 * i + 1] * v[1] + m[3 * i + 2] * v[2];
    memcpy(o, r, sizeof r);
}

static void inv3(const double m[9], double o[9]) {
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    double A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
    double det = a * A + b * B + c * C;
    o[0] = A / det; o[1] = -(b * i - c * h) / det; o[2] = (b * f - c * e) / det;
    o[3] = B / det; o[4] = (a * i - c * g) / det; o[5] = -(a * f - c * d) / det;
    o[6] = C / det; o[7] = -(a * h - b * g) / det; o[8] = (a * e - b * d) / det;
}

/* Linear RGB to XYZ (D65, except ProPhoto's D50), from CSS Color 4 §18. */
static const double M_SRGB[9] = { 506752.0 / 1228815, 87881.0 / 245763, 12673.0 / 70218,
                                  87098.0 / 409605, 175762.0 / 245763, 12673.0 / 175545,
                                  7918.0 / 409605, 87881.0 / 737289, 1001167.0 / 1053270 };
static const double M_P3[9] = { 608311.0 / 1250200, 189793.0 / 714400, 198249.0 / 1000160,
                                35783.0 / 156275, 247089.0 / 357200, 198249.0 / 2500400,
                                0, 32229.0 / 714400, 5220557.0 / 5000800 };
static const double M_A98[9] = { 573536.0 / 994567, 263643.0 / 1420810, 187206.0 / 994567,
                                 591459.0 / 1989134, 6239551.0 / 9945670, 374412.0 / 4972835,
                                 53769.0 / 1989134, 351524.0 / 4972835, 4929758.0 / 4972835 };
static const double M_PROPHOTO[9] = { 0.7977666449006423, 0.13518129740053308, 0.0313477341283922,
                                      0.2880748288194013, 0.711835234241873, 0.00008993693872564,
                                      0, 0, 0.8251046025104602 };
static const double M_REC2020[9] = { 63426534.0 / 99577255, 20160776.0 / 139408157, 47086771.0 / 278816314,
                                     26158966.0 / 99577255, 472592308.0 / 697040785, 8267143.0 / 139408157,
                                     0, 19567812.0 / 697040785, 295819943.0 / 278816314 };
static const double M_D50_D65[9] = { 0.955473421488075, -0.02309845494876471, 0.06325924320057072,
                                     -0.0283697093338637, 1.0099953980813041, 0.021041441191917323,
                                     0.012314014864481998, -0.020507649298898964, 1.330365926242124 };
static const double M_XYZ_LMS[9] = { 0.8190224379967030, 0.3619062600528904, -0.1288737815209879,
                                     0.0329836539323885, 0.9292868615863434, 0.0361446663506424,
                                     0.0481771893596242, 0.2642395317527308, 0.6335478284694309 };
static const double M_LMS_OKLAB[9] = { 0.2104542683093140, 0.7936177747023054, -0.0040720430116193,
                                       1.9779985324311684, -2.4285922420485799, 0.4505937096174110,
                                       0.0259040424655478, 0.7827717124575296, -0.8086757549230774 };
static const double WHITE_D50[3] = { 0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585 };

typedef struct ColorMats {
    double srgb[9], p3[9], a98[9], prophoto[9], rec2020[9], d65_d50[9], lms_xyz[9], oklab_lms[9];
} ColorMats;
static const ColorMats *color_mats(void) {
    static ColorMats m;
    static volatile int ready;
    if (!ready) {   /* idempotent: racing threads write the same values */
        inv3(M_SRGB, m.srgb); inv3(M_P3, m.p3); inv3(M_A98, m.a98); inv3(M_PROPHOTO, m.prophoto);
        inv3(M_REC2020, m.rec2020); inv3(M_D50_D65, m.d65_d50); inv3(M_XYZ_LMS, m.lms_xyz);
        inv3(M_LMS_OKLAB, m.oklab_lms);
        ready = 1;
    }
    return &m;
}

/* Transfer functions: encoded <-> linear, sign-preserving. */
static double tf_srgb_lin(double c) { double a = fabs(c); return a <= 0.04045 ? c / 12.92 : copysign(pow((a + 0.055) / 1.055, 2.4), c); }
static double tf_srgb_enc(double c) { double a = fabs(c); return a > 0.0031308 ? copysign(1.055 * pow(a, 1 / 2.4) - 0.055, c) : 12.92 * c; }
static double tf_a98_lin(double c) { return copysign(pow(fabs(c), 563.0 / 256), c); }
static double tf_a98_enc(double c) { return copysign(pow(fabs(c), 256.0 / 563), c); }
static double tf_pp_lin(double c) { double a = fabs(c); return a <= 16.0 / 512 ? c / 16 : copysign(pow(a, 1.8), c); }
static double tf_pp_enc(double c) { double a = fabs(c); return a >= 1.0 / 512 ? copysign(pow(a, 1 / 1.8), c) : 16 * c; }
static const double R20_A = 1.09929682680944, R20_B = 0.018053968510807;
static double tf_r20_lin(double c) { double a = fabs(c); return a < R20_B * 4.5 ? c / 4.5 : copysign(pow((a + R20_A - 1) / R20_A, 1 / 0.45), c); }
static double tf_r20_enc(double c) { double a = fabs(c); return a > R20_B ? copysign(R20_A * pow(a, 0.45) - (R20_A - 1), c) : 4.5 * c; }

/* sRGB (0-1) <-> HSL (degrees, 0-100, 0-100) and HWB; an achromatic color's
 * hue is powerless (reported in *hue_none). */
static void srgb_to_hsl(const double c[3], double o[3], bool *hue_none) {
    double mx = fmax(c[0], fmax(c[1], c[2])), mn = fmin(c[0], fmin(c[1], c[2]));
    double l = (mx + mn) / 2, d = mx - mn, h = 0, s = 0;
    *hue_none = true;
    if (d != 0) {
        s = (l == 0 || l == 1) ? 0 : (mx - l) / fmin(l, 1 - l);
        if (mx == c[0]) h = (c[1] - c[2]) / d + (c[1] < c[2] ? 6 : 0);
        else if (mx == c[1]) h = (c[2] - c[0]) / d + 2;
        else h = (c[0] - c[1]) / d + 4;
        h *= 60;
        *hue_none = false;
    }
    if (s < 0) { h += 180; s = fabs(s); }
    o[0] = norm_hue(h); o[1] = s * 100; o[2] = l * 100;
}

static void hsl_to_srgb(const double hsl[3], double o[3]) {
    double h = norm_hue(hsl[0]), s = hsl[1] / 100, l = hsl[2] / 100;
    for (int i = 0; i < 3; i++) {
        double n = i == 0 ? 0 : i == 1 ? 8 : 4;
        double k = fmod(n + h / 30, 12), a = s * fmin(l, 1 - l);
        o[i] = l - a * fmax(-1, fmin(fmin(k - 3, 9 - k), 1));
    }
}

static void hwb_to_srgb(const double hwb[3], double o[3]) {
    double w = hwb[1] / 100, b = hwb[2] / 100;
    if (w + b >= 1) { o[0] = o[1] = o[2] = w / (w + b); return; }
    double full[3] = { hwb[0], 100, 50 };
    hsl_to_srgb(full, o);
    for (int i = 0; i < 3; i++) o[i] = o[i] * (1 - w - b) + w;
}

static void srgb_to_hwb(const double c[3], double o[3], bool *hue_none) {
    double hsl[3];
    srgb_to_hsl(c, hsl, hue_none);
    double mx = fmax(c[0], fmax(c[1], c[2])), mn = fmin(c[0], fmin(c[1], c[2]));
    o[0] = hsl[0]; o[1] = mn * 100; o[2] = (1 - mx) * 100;
    if (o[1] + o[2] >= 100) *hue_none = true;
}

static void lab_to_xyz50(const double lab[3], double o[3]) {
    const double k = 24389.0 / 27, e = 216.0 / 24389;
    double f1 = (lab[0] + 16) / 116, f0 = lab[1] / 500 + f1, f2 = f1 - lab[2] / 200;
    double x = pow(f0, 3) > e ? pow(f0, 3) : (116 * f0 - 16) / k;
    double y = lab[0] > k * e ? pow(f1, 3) : lab[0] / k;
    double z = pow(f2, 3) > e ? pow(f2, 3) : (116 * f2 - 16) / k;
    o[0] = x * WHITE_D50[0]; o[1] = y * WHITE_D50[1]; o[2] = z * WHITE_D50[2];
}

static void xyz50_to_lab(const double xyz[3], double o[3]) {
    const double k = 24389.0 / 27, e = 216.0 / 24389;
    double f[3];
    for (int i = 0; i < 3; i++) {
        double v = xyz[i] / WHITE_D50[i];
        f[i] = v > e ? cbrt(v) : (k * v + 16) / 116;
    }
    o[0] = 116 * f[1] - 16; o[1] = 500 * (f[0] - f[1]); o[2] = 200 * (f[1] - f[2]);
}

static void polar_to_rect(const double p[3], double o[3]) {
    double h = p[2] * M_PI / 180;
    o[0] = p[0]; o[1] = p[1] * cos(h); o[2] = p[1] * sin(h);
}

static void rect_to_polar(const double r[3], double o[3], double eps, bool *hue_none) {
    double c = sqrt(r[1] * r[1] + r[2] * r[2]);
    o[0] = r[0]; o[1] = c; o[2] = norm_hue(atan2(r[2], r[1]) * 180 / M_PI);
    *hue_none = c <= eps;
}

/* Channels of space cs (missing ones as 0) to XYZ D65. */
static void to_xyz65(int cs, const double in[3], double o[3]) {
    const ColorMats *m = color_mats();
    double v[3];
    memcpy(v, in, sizeof v);
    switch (cs) {
    case CS_HSL: hsl_to_srgb(in, v); /* fall through */
    case CS_SRGB: for (int i = 0; i < 3; i++) v[i] = tf_srgb_lin(v[i]); mat3(M_SRGB, v, o); return;
    case CS_HWB: hwb_to_srgb(in, v); for (int i = 0; i < 3; i++) v[i] = tf_srgb_lin(v[i]); mat3(M_SRGB, v, o); return;
    case CS_SRGB_LINEAR: mat3(M_SRGB, v, o); return;
    case CS_P3: for (int i = 0; i < 3; i++) v[i] = tf_srgb_lin(v[i]); mat3(M_P3, v, o); return;
    case CS_P3_LINEAR: mat3(M_P3, v, o); return;
    case CS_A98: for (int i = 0; i < 3; i++) v[i] = tf_a98_lin(v[i]); mat3(M_A98, v, o); return;
    case CS_PROPHOTO: for (int i = 0; i < 3; i++) v[i] = tf_pp_lin(v[i]); mat3(M_PROPHOTO, v, v); mat3(M_D50_D65, v, o); return;
    case CS_REC2020: for (int i = 0; i < 3; i++) v[i] = tf_r20_lin(v[i]); mat3(M_REC2020, v, o); return;
    case CS_XYZ_D50: mat3(M_D50_D65, v, o); return;
    case CS_XYZ_D65: memcpy(o, v, sizeof v); return;
    case CS_LCH: polar_to_rect(in, v); /* fall through */
    case CS_LAB: lab_to_xyz50(v, v); mat3(M_D50_D65, v, o); return;
    case CS_OKLCH: polar_to_rect(in, v); /* fall through */
    case CS_OKLAB:
        mat3(m->oklab_lms, v, v);
        for (int i = 0; i < 3; i++) v[i] = v[i] * v[i] * v[i];
        mat3(m->lms_xyz, v, o);
        return;
    }
}

/* XYZ D65 to space cs; *hue_none when the result's hue is powerless. */
static void from_xyz65(int cs, const double in[3], double o[3], bool *hue_none) {
    const ColorMats *m = color_mats();
    double v[3];
    *hue_none = false;
    switch (cs) {
    case CS_SRGB: case CS_HSL: case CS_HWB:
        mat3(m->srgb, in, v);
        for (int i = 0; i < 3; i++) v[i] = tf_srgb_enc(v[i]);
        if (cs == CS_HSL) srgb_to_hsl(v, o, hue_none);
        else if (cs == CS_HWB) srgb_to_hwb(v, o, hue_none);
        else memcpy(o, v, sizeof v);
        return;
    case CS_SRGB_LINEAR: mat3(m->srgb, in, o); return;
    case CS_P3: mat3(m->p3, in, o); for (int i = 0; i < 3; i++) o[i] = tf_srgb_enc(o[i]); return;
    case CS_P3_LINEAR: mat3(m->p3, in, o); return;
    case CS_A98: mat3(m->a98, in, o); for (int i = 0; i < 3; i++) o[i] = tf_a98_enc(o[i]); return;
    case CS_PROPHOTO: mat3(m->d65_d50, in, v); mat3(m->prophoto, v, o); for (int i = 0; i < 3; i++) o[i] = tf_pp_enc(o[i]); return;
    case CS_REC2020: mat3(m->rec2020, in, o); for (int i = 0; i < 3; i++) o[i] = tf_r20_enc(o[i]); return;
    case CS_XYZ_D50: mat3(m->d65_d50, in, o); return;
    case CS_XYZ_D65: memcpy(o, in, sizeof v); return;
    case CS_LAB: case CS_LCH:
        mat3(m->d65_d50, in, v);
        xyz50_to_lab(v, v);
        if (cs == CS_LCH) rect_to_polar(v, o, 0.0015, hue_none);
        else memcpy(o, v, sizeof v);
        return;
    case CS_OKLAB: case CS_OKLCH:
        mat3(M_XYZ_LMS, in, v);
        for (int i = 0; i < 3; i++) v[i] = cbrt(v[i]);
        mat3(M_LMS_OKLAB, v, v);
        if (cs == CS_OKLCH) rect_to_polar(v, o, 0.000004, hue_none);
        else memcpy(o, v, sizeof v);
        return;
    }
}

/* Which kind of channel i of space cs is, for carrying missing channels
 * between spaces (CSS Color 4 §12.2): 1 red, 2 green, 3 blue, 4 lightness,
 * 5 colorfulness, 6 hue, 7 opposing a, 8 opposing b; 0 none of these. */
static int channel_kind(int cs, int i) {
    switch (cs) {
    case CS_HSL: return i == 0 ? 6 : i == 1 ? 5 : 4;
    case CS_HWB: return i == 0 ? 6 : 0;
    case CS_LAB: case CS_OKLAB: return i == 0 ? 4 : i == 1 ? 7 : 8;
    case CS_LCH: case CS_OKLCH: return i == 0 ? 4 : i == 1 ? 5 : 6;
    case CS_XYZ_D50: case CS_XYZ_D65: return 0;
    default: return i + 1;
    }
}

/* c converted to space cs: missing channels count as 0 for the conversion
 * and stay missing in an analogous channel; a powerless hue is missing. */
static Color color_convert(const Color *c, int cs) {
    Color r = *c;
    r.legacy = false;
    if (c->cs == cs) return r;
    double v[3], xyz[3];
    for (int i = 0; i < 3; i++) v[i] = (c->none >> i & 1) ? 0 : c->c[i];
    bool hue_none = false;
    if ((c->cs == CS_SRGB || c->cs == CS_HSL || c->cs == CS_HWB) && (cs == CS_SRGB || cs == CS_HSL || cs == CS_HWB)) {
        double s[3];
        if (c->cs == CS_HSL) hsl_to_srgb(v, s); else if (c->cs == CS_HWB) hwb_to_srgb(v, s); else memcpy(s, v, sizeof s);
        if (cs == CS_HSL) srgb_to_hsl(s, r.c, &hue_none); else if (cs == CS_HWB) srgb_to_hwb(s, r.c, &hue_none); else memcpy(r.c, s, sizeof s);
    } else {
        to_xyz65(c->cs, v, xyz);
        from_xyz65(cs, xyz, r.c, &hue_none);
    }
    r.cs = cs;
    r.none = c->none & 8;
    for (int i = 0; i < 3; i++) {
        int k = channel_kind(cs, i);
        for (int j = 0; k && j < 3; j++)
            if ((c->none >> j & 1) && channel_kind(c->cs, j) == k) r.none |= 1u << i;
    }
    if (hue_none && cs_hue(cs) >= 0) r.none |= 1u << cs_hue(cs);
    return r;
}

/* The color as sRGB 0-255 and alpha 0-1, clipped to the gamut. */
static void color_to_rgba(const Color *c, double rgba[4]) {
    Color s = color_convert(c, CS_SRGB);
    /* (snapped so that 127.4999999 from float noise rounds as 127.5) */
    for (int i = 0; i < 3; i++) rgba[i] = round(((s.none >> i & 1) ? 0 : clamp01(s.c[i])) * 255 * 1e6) / 1e6;
    rgba[3] = (c->none & 8) ? 0 : clamp01(c->alpha);
}

/* Channel keywords of a relative color, visible to calc() while its
 * channels are evaluated. */
typedef struct ColorVars { const char *name[4]; double v[4]; unsigned none; } ColorVars;
static __thread const ColorVars *t_color_vars;

/* A channel keyword's value (0 for a missing channel, inside calc()). */
static bool color_var(const char *name, double *v) {
    const ColorVars *cv = t_color_vars;
    for (int i = 0; cv && i < 4; i++)
        if (cv->name[i] && strcasecmp(cv->name[i], name) == 0) { *v = cv->v[i]; return true; }
    return false;
}

/* Is a bare channel keyword the origin's missing channel? */
static bool color_var_none(const char *name) {
    const ColorVars *cv = t_color_vars;
    for (int i = 0; cv && i < 4; i++)
        if (cv->name[i] && strcasecmp(cv->name[i], name) == 0) return cv->none >> i & 1;
    return false;
}

static bool calc_simplify_value(const Toks *ts, int i, CalcSum *out);

/* One channel: a number, a percentage of `ref`, an angle (hues) or none
 * (not in legacy syntax).  kind 'n' number/percentage, 'h' hue, 'a' alpha,
 * 'N' number only, 'P' percentage only. */
static bool color_channel(const Toks *ts, int i, char kind, double ref, bool legacy, double *v, bool *none) {
    const Tok *t = &ts->t[i];
    *none = false;
    if (tok_ident(t, "none")) { if (legacy) return false; *none = true; *v = 0; return true; }
    double num = 0;
    const char *unit = "";
    if (t->type == T_IDENT && color_var(t->str, &num)) {
        if (color_var_none(t->str)) { *none = true; *v = 0; return true; }
    }
    else if (t->type == T_NUMBER) num = t->num;
    else if (t->type == T_PERCENT) { num = t->num; unit = "%"; }
    else if (t->type == T_DIM || (t->type == T_FUNC && is_math_fn(t->str))) {
        CalcSum s;
        if (t->type == T_DIM) {
            double sc = 0;
            if (unit_dim(t->str, &sc) != D_ANGLE) return false;
            num = t->num * sc;
            unit = "deg";
        } else {
            if (!calc_simplify_value(ts, i, &s) || !calc_one(&s, &num, &unit)) return false;
        }
    } else return false;
    if (!isfinite(num)) num = isnan(num) ? 0 : num;
    if (!strcmp(unit, "")) {
        if (kind == 'P') return false;
        *v = num;
    } else if (!strcmp(unit, "%")) {
        if (kind == 'h' || kind == 'N') return false;
        *v = num / 100 * ref;
    } else if (!strcmp(unit, "deg")) {
        if (kind != 'h') return false;
        *v = num;
    } else return false;
    return true;
}

static int color_eval(const Toks *ts, int i, Color *out, int depth);

/* The arguments of a color function: token indices, with where the '/' is
 * and whether they were comma-separated. */
typedef struct FnArgs { int v[8]; int n, slash; bool commas, bad; } FnArgs;

static void fn_args(const Toks *ts, const Tok *f, int from, FnArgs *a) {
    memset(a, 0, sizeof *a);
    a->slash = -1;
    int commas = 0;
    bool last_comma = false;
    for (int i = from; i < f->end; i = tok_next(ts, i)) {
        const Tok *t = &ts->t[i];
        if (t->type == T_COMMA) { commas++; if (last_comma || !a->n) a->bad = true; last_comma = true; continue; }
        last_comma = false;
        if (t->type == T_DELIM && t->delim == '/') { if (a->slash >= 0) a->bad = true; a->slash = a->n; continue; }
        if (a->n == 8) { a->bad = true; break; }
        a->v[a->n++] = i;
    }
    if (last_comma) a->bad = true;
    a->commas = commas > 0;
    if (a->commas && (a->slash >= 0 || commas != a->n - 1)) a->bad = true;
}

static int cs_from_ident(const Tok *t) {
    if (t->type != T_IDENT) return -1;
    if (!strcasecmp(t->str, "xyz")) return CS_XYZ_D65;
    for (int k = 0; k < CS_N; k++) if (!strcasecmp(t->str, cs_name[k])) return k;
    return -1;
}

/* rgb() hsl() hwb() lab() lch() oklab() oklch() color(), absolute or relative. */
static int color_fn(const Toks *ts, const Tok *f, Color *out, int depth) {
    const char *n = f->str;
    int cs = !strcasecmp(n, "rgb") || !strcasecmp(n, "rgba") ? CS_SRGB :
             !strcasecmp(n, "hsl") || !strcasecmp(n, "hsla") ? CS_HSL :
             !strcasecmp(n, "hwb") ? CS_HWB : !strcasecmp(n, "lab") ? CS_LAB : !strcasecmp(n, "lch") ? CS_LCH :
             !strcasecmp(n, "oklab") ? CS_OKLAB : !strcasecmp(n, "oklch") ? CS_OKLCH :
             !strcasecmp(n, "color") ? -2 : -1;
    if (cs == -1) return 0;
    int i = f->first, res = 1;
    ColorVars vars;
    memset(&vars, 0, sizeof vars);
    bool relative = i < f->end && tok_ident(&ts->t[i], "from");
    Color origin;
    if (relative) {
        i++;
        if (i >= f->end) return 0;
        res = color_eval(ts, i, &origin, depth + 1);
        if (!res) return 0;
        i = tok_next(ts, i);
    }
    if (cs == -2) {
        if (i >= f->end || (cs = cs_from_ident(&ts->t[i])) < 0 || cs >= CS_LAB) return 0;
        i++;
    }
    bool is_color_fn = f->str[0] == 'c' || f->str[0] == 'C';
    FnArgs a;
    fn_args(ts, f, i, &a);
    if (a.bad) return 0;
    /* legacy (comma) syntax: rgb() and hsl() only, absolute, 3 or 4 values */
    bool legacy = a.commas;
    if (legacy && (relative || is_color_fn || (cs != CS_SRGB && cs != CS_HSL) || (a.n != 3 && a.n != 4))) return 0;
    if (!legacy && !(a.slash < 0 ? a.n == 3 : a.slash == 3 && a.n == 4)) return 0;
    static const char *const names[][3] = {
        [CS_SRGB] = { "r", "g", "b" }, [CS_HSL] = { "h", "s", "l" }, [CS_HWB] = { "h", "w", "b" },
        [CS_LAB] = { "l", "a", "b" }, [CS_LCH] = { "l", "c", "h" }, [CS_OKLAB] = { "l", "a", "b" },
        [CS_OKLCH] = { "l", "c", "h" },
    };
    double scale = cs == CS_SRGB && !is_color_fn ? 255 : 1;
    if (relative) {
        /* the origin's channels (0 when it can't be resolved here: only
         * validity matters then) */
        static const char *const xyz[3] = { "x", "y", "z" }, *const rgb[3] = { "r", "g", "b" };
        Color o = res == 1 ? color_convert(&origin, cs) : (Color){ .cs = cs };
        for (int k = 0; k < 3; k++) {
            vars.name[k] = is_color_fn ? (cs == CS_XYZ_D50 || cs == CS_XYZ_D65 ? xyz[k] : rgb[k]) : names[cs][k];
            vars.v[k] = (o.none >> k & 1) ? 0 : o.c[k] * scale;
        }
        vars.name[3] = "alpha";
        vars.v[3] = (o.none & 8) ? 0 : o.alpha;
        vars.none = o.none;
    }
    const ColorVars *saved = t_color_vars;
    if (relative) t_color_vars = &vars;
    memset(out, 0, sizeof *out);
    out->cs = cs;
    out->alpha = 1;
    out->legacy = !relative && !is_color_fn && (cs == CS_SRGB || cs == CS_HSL || cs == CS_HWB);
    out->derived = relative;
    bool ok = true;
    int pct_kind = -1;      /* legacy rgb(): all numbers or all percentages */
    for (int k = 0; k < 3 && ok; k++) {
        double ref = 1;
        char kind = 'n';
        if (is_color_fn) ref = 1;
        else switch (cs) {
        case CS_SRGB: ref = 255; break;
        case CS_HSL: case CS_HWB: if (k == 0) kind = 'h'; else { ref = 100; if (legacy) kind = 'P'; } break;
        case CS_LAB: ref = k == 0 ? 100 : 125; break;
        case CS_LCH: if (k == 2) kind = 'h'; else ref = k == 0 ? 100 : 150; break;
        case CS_OKLAB: ref = k == 0 ? 1 : 0.4; break;
        case CS_OKLCH: if (k == 2) kind = 'h'; else ref = k == 0 ? 1 : 0.4; break;
        }
        bool none;
        double v;
        const Tok *t = &ts->t[a.v[k]];
        if (legacy && cs == CS_SRGB) {
            int p = t->type == T_PERCENT ? 1 : t->type == T_NUMBER ? 0 : -1;
            if (p >= 0 && pct_kind >= 0 && p != pct_kind) { ok = false; break; }
            if (p >= 0) pct_kind = p;
        }
        if (!color_channel(ts, a.v[k], kind, ref, legacy, &v, &none)) { ok = false; break; }
        if (none) out->none |= 1u << k;
        out->c[k] = v / scale;
    }
    if (ok && a.n == 4) {
        bool none;
        double v;
        if (!color_channel(ts, a.v[3], 'a', 1, legacy, &v, &none)) ok = false;
        else if (none) out->none |= 8;
        else out->alpha = clamp01(v);
    } else if (ok && relative) {
        out->alpha = vars.v[3];
        out->none |= vars.none & 8;
    }
    t_color_vars = saved;
    if (!ok) return 0;
    /* parse-time clamps (CSS Color 4) */
    switch (cs) {
    case CS_LAB: case CS_LCH: out->c[0] = fmax(0, fmin(100, out->c[0])); break;
    case CS_OKLAB: case CS_OKLCH: out->c[0] = fmax(0, fmin(1, out->c[0])); break;
    case CS_HSL: if (!relative) { out->c[1] = fmax(0, out->c[1]); out->c[2] = fmax(0, fmin(100, out->c[2])); } break;
    default: break;
    }
    if (cs == CS_LCH || cs == CS_OKLCH) out->c[1] = fmax(0, out->c[1]);
    if (cs_hue(cs) >= 0 && !(out->none >> cs_hue(cs) & 1)) out->c[cs_hue(cs)] = norm_hue(out->c[cs_hue(cs)]);
    return res;
}

/* color-mix() (CSS Color 5 §2): two colors (or one) mixed in a space. */
static int color_mix(const Toks *ts, const Tok *f, Color *out, int depth) {
    int cs = CS_OKLAB, hue_method = 0;      /* shorter, longer, increasing, decreasing */
    int i = f->first;
    if (i < f->end && tok_ident(&ts->t[i], "in")) {
        if (++i >= f->end || (cs = cs_from_ident(&ts->t[i])) < 0) return 0;
        i++;
        if (i < f->end && ts->t[i].type == T_IDENT && cs_hue(cs) >= 0) {
            static const char *const m[] = { "shorter", "longer", "increasing", "decreasing" };
            hue_method = -1;
            for (int k = 0; k < 4; k++) if (tok_ident(&ts->t[i], m[k])) hue_method = k;
            if (hue_method < 0 || i + 1 >= f->end || !tok_ident(&ts->t[i + 1], "hue")) return 0;
            i += 2;
        }
        if (i < f->end && ts->t[i].type == T_COMMA) i++;
    }
    Color c[2];
    double p[2];
    bool has_p[2] = { false, false };
    int nc = 0, res = 1;
    while (i < f->end) {
        if (nc == 2) return -1;     /* more than two: valid, not mixed here */
        bool got = false;
        for (int part = 0; part < 2 && i < f->end && ts->t[i].type != T_COMMA; part++) {
            const Tok *t = &ts->t[i];
            if (t->type == T_PERCENT || (t->type == T_FUNC && is_math_fn(t->str))) {
                CalcSum s;
                double v;
                const char *u;
                if (has_p[nc]) return 0;
                if (t->type == T_PERCENT) v = t->num;
                else if (!calc_simplify_value(ts, i, &s) || !calc_one(&s, &v, &u) || strcmp(u, "%")) return 0;
                if (v < 0 || v > 100) return 0;
                p[nc] = v;
                has_p[nc] = true;
            } else {
                if (got) return 0;
                int r = color_eval(ts, i, &c[nc], depth + 1);
                if (!r) return 0;
                if (r < 0) res = -1;
                got = true;
            }
            i = tok_next(ts, i);
        }
        if (!got) return 0;
        nc++;
        if (i < f->end) { if (ts->t[i].type != T_COMMA) return 0; i++; if (i >= f->end) return 0; }
    }
    if (nc == 0) return 0;
    if (nc == 1) { has_p[1] = false; p[1] = 0; }
    if (!has_p[0] && !has_p[1]) p[0] = p[1] = nc == 2 ? 50 : 100;
    else if (!has_p[0]) p[0] = 100 - p[1];
    else if (!has_p[1]) p[1] = nc == 2 ? 100 - p[0] : 0;
    double sum = p[0] + (nc == 2 ? p[1] : 0);
    if (sum <= 0) return 0;
    if (res < 0) return -1;
    double mult = sum < 100 ? sum / 100 : 1;
    Color a = color_convert(&c[0], cs);
    Color b = nc == 2 ? color_convert(&c[1], cs) : a;
    double t = nc == 2 ? p[1] / sum : 0;
    memset(out, 0, sizeof *out);
    out->cs = cs;
    out->derived = true;
    /* missing channels take the other color's */
    for (int k = 0; k < 4; k++) {
        bool an = a.none >> k & 1, bn = b.none >> k & 1;
        double *av = k < 3 ? &a.c[k] : &a.alpha, *bv = k < 3 ? &b.c[k] : &b.alpha;
        if (an && bn) { out->none |= 1u << k; *av = *bv = 0; }
        else if (an) *av = *bv;
        else if (bn) *bv = *av;
    }
    double alpha = a.alpha * (1 - t) + b.alpha * t;
    int hk = cs_hue(cs);
    if (hk >= 0 && !(out->none >> hk & 1)) {
        double h1 = norm_hue(a.c[hk]), h2 = norm_hue(b.c[hk]), d = h2 - h1;
        switch (hue_method) {
        case 0: if (d > 180) h1 += 360; else if (d < -180) h2 += 360; break;
        case 1: if (d > 0 && d < 180) h1 += 360; else if (d > -180 && d <= 0) h2 += 360; break;
        case 2: if (h2 < h1) h2 += 360; break;
        case 3: if (h1 < h2) h1 += 360; break;
        }
        out->c[hk] = norm_hue(h1 * (1 - t) + h2 * t);
    }
    for (int k = 0; k < 3; k++) {
        if (k == hk || (out->none >> k & 1)) continue;
        double v = a.c[k] * a.alpha * (1 - t) + b.c[k] * b.alpha * t;
        out->c[k] = alpha != 0 ? v / alpha : v;
    }
    out->alpha = (out->none & 8) ? 0 : alpha * mult;
    return 1;
}

static const struct { const char *n; double c[3]; } g_system_colors[] = {
    { "canvas", {255,255,255} }, { "field", {255,255,255} }, { "buttonface", {239,239,239} },
    { "linktext", {0,0,238} }, { "visitedtext", {85,26,139} }, { "activetext", {255,0,0} },
    { "graytext", {109,109,109} }, { "highlight", {181,213,255} }, { "mark", {255,255,0} },
    { "buttonborder", {118,118,118} }, { "accentcolor", {0,117,255} }, { "selecteditem", {0,117,255} },
    { "accentcolortext", {255,255,255} }, { "selecteditemtext", {255,255,255} },
};

/* Any <color> token: 1 resolved into *out, -1 valid but not resolvable here
 * (currentcolor outside a computation), 0 not a color. */
static int color_eval(const Toks *ts, int i, Color *out, int depth) {
    const Tok *t = &ts->t[i];
    memset(out, 0, sizeof *out);
    out->cs = CS_SRGB;
    out->alpha = 1;
    out->legacy = true;
    if (depth > 16) return 0;
    if (t->type == T_HASH) {
        const char *h = t->str;
        size_t l = strlen(h);
        unsigned v[8];
        if (l != 3 && l != 4 && l != 6 && l != 8) return 0;
        for (size_t k = 0; k < l; k++) {
            if (!isxdigit((unsigned char)h[k])) return 0;
            v[k] = (unsigned)(isdigit((unsigned char)h[k]) ? h[k] - '0' : tolower((unsigned char)h[k]) - 'a' + 10);
        }
        for (int k = 0; k < 3; k++) out->c[k] = (l <= 4 ? v[k] * 17 : v[2 * k] * 16 + v[2 * k + 1]) / 255.0;
        if (l == 4) out->alpha = v[3] * 17 / 255.0;
        if (l == 8) out->alpha = (v[6] * 16 + v[7]) / 255.0;
        return 1;
    }
    if (t->type == T_IDENT) {
        const unsigned char *rgb;
        if (css_color_named(t->str, &rgb)) { for (int k = 0; k < 3; k++) out->c[k] = rgb[k] / 255.0; return 1; }
        if (!strcasecmp(t->str, "transparent")) { out->alpha = 0; return 1; }
        if (!strcasecmp(t->str, "currentcolor")) {
            if (!g_env) return -1;
            for (int k = 0; k < 3; k++) out->c[k] = g_env->color[k] / 255;
            out->alpha = g_env->color[3];
            return 1;
        }
        if (css_color_system(t->str)) {
            for (size_t k = 0; k < sizeof g_system_colors / sizeof g_system_colors[0]; k++)
                if (!strcasecmp(t->str, g_system_colors[k].n))
                    for (int j = 0; j < 3; j++) out->c[j] = g_system_colors[k].c[j] / 255;
            return 1;
        }
        return 0;
    }
    if (t->type != T_FUNC) return 0;
    if (!strcasecmp(t->str, "color-mix")) return color_mix(ts, t, out, depth);
    if (!strcasecmp(t->str, "light-dark")) {
        int j = t->first;
        if (j >= t->end) return 0;
        int r = color_eval(ts, j, out, depth + 1);
        int k = tok_next(ts, j);
        if (k >= t->end || ts->t[k].type != T_COMMA || ++k >= t->end || tok_next(ts, k) != t->end) return 0;
        Color dark;
        int r2 = color_eval(ts, k, &dark, depth + 1);
        if (!r || !r2) return 0;
        out->derived = true;
        return r < 0 || r2 < 0 ? -1 : r;
    }
    return color_fn(ts, t, out, depth);
}

/* A color in its computed form (CSS Color 4 §15). */
static void color_serialize(Buf *b, const Color *c) {
    if (c->legacy && !c->none) {
        double rgba[4];
        color_to_rgba(c, rgba);
        fmt_rgba(b, rgba);
        return;
    }
    Color s = *c;
    if ((s.cs == CS_HSL || s.cs == CS_HWB) && (s.none & 7)) {
        /* missing channels keep hsl() / hwb() */
        buf_str(b, cs_name[s.cs]);
        buf_ch(b, '(');
        for (int k = 0; k < 3; k++) {
            if (k) buf_ch(b, ' ');
            if (s.none >> k & 1) { buf_str(b, "none"); continue; }
            fmt_number(b, s.c[k]);
            if (k) buf_ch(b, '%');
        }
        if (s.none & 8) buf_str(b, " / none");
        else if (s.alpha < 1) { buf_str(b, " / "); fmt_number(b, clamp01(s.alpha)); }
        buf_ch(b, ')');
        return;
    }
    if (s.cs == CS_HSL || s.cs == CS_HWB) {
        unsigned keep = s.none & 8;
        s = color_convert(c, CS_SRGB);
        s.none = keep;
    }
    bool fn = s.cs < CS_LAB;
    if (fn) { buf_str(b, "color("); buf_str(b, cs_name[s.cs]); buf_ch(b, ' '); }
    else { buf_str(b, cs_name[s.cs]); buf_ch(b, '('); }
    for (int k = 0; k < 3; k++) {
        if (k) buf_ch(b, ' ');
        if (s.none >> k & 1) buf_str(b, "none");
        else fmt_number(b, s.c[k]);
    }
    if (s.none & 8) buf_str(b, " / none");
    else if (s.alpha < 1) { buf_str(b, " / "); fmt_number(b, clamp01(s.alpha)); }
    buf_ch(b, ')');
}

static bool has_math_arg(const Toks *ts, const Tok *f) {
    for (int i = f->first; i < f->end; i = tok_next(ts, i))
        if (ts->t[i].type == T_FUNC && is_math_fn(ts->t[i].str)) return true;
    return false;
}

static void color_spec(Buf *b, const Toks *ts, int i);

/* color-mix()'s specified form: the default hue method dropped, and the
 * percentages normalized when they're plain (50%/50% is none). */
static void color_mix_spec(Buf *b, const Toks *ts, const Tok *f) {
    buf_str(b, "color-mix(");
    int i = f->first;
    if (i < f->end && tok_ident(&ts->t[i], "in")) {
        buf_str(b, "in ");
        for (i++; i < f->end && ts->t[i].type == T_IDENT; i++) {
            if (tok_ident(&ts->t[i], "shorter") && i + 1 < f->end && tok_ident(&ts->t[i + 1], "hue")) { i++; continue; }
            if (b->n && b->s[b->n - 1] != ' ') buf_ch(b, ' ');
            ser_ident(b, ts->t[i].str);
        }
        if (i < f->end && ts->t[i].type == T_COMMA) i++;
        buf_str(b, ", ");
    }
    int col[2] = { -1, -1 }, pct[2] = { -1, -1 }, n = 0;
    bool plain = true;
    for (; i < f->end && n < 2; n++) {
        for (; i < f->end && ts->t[i].type != T_COMMA; i = tok_next(ts, i)) {
            if (ts->t[i].type == T_PERCENT) pct[n] = i;
            else if (ts->t[i].type == T_FUNC && is_math_fn(ts->t[i].str)) { pct[n] = i; plain = false; }
            else col[n] = i;
        }
        if (i < f->end) i++;
    }
    double p[2] = { pct[0] >= 0 ? ts->t[pct[0]].num : -1, pct[1] >= 0 ? ts->t[pct[1]].num : -1 };
    bool show = !plain || n != 2 || !((p[0] < 0 || p[0] == 50) && (p[1] < 0 || p[1] == 50));
    if (plain && n == 2 && show) {
        if (p[0] < 0) p[0] = 100 - p[1];
        if (p[1] < 0) p[1] = 100 - p[0];
    }
    Ser plain_ser = { ts, NULL, 0, NULL, NULL };
    for (int k = 0; k < n; k++) {
        if (k) buf_str(b, ", ");
        if (col[k] >= 0) color_spec(b, ts, col[k]);
        if (!show) continue;
        if (plain && n == 2) { buf_ch(b, ' '); fmt_number(b, p[k]); buf_ch(b, '%'); }
        else if (pct[k] >= 0) { buf_ch(b, ' '); ser_tok(&plain_ser, pct[k], b, NULL, 0); }
    }
    buf_ch(b, ')');
}

/* A color's specified form (CSS Color 4 §15): absolute colors canonical;
 * relative colors, color-mix() and light-dark() as written, their colors
 * canonical; calc() in a non-legacy color kept. */
static void color_spec(Buf *b, const Toks *ts, int i) {
    const Tok *t = &ts->t[i];
    Ser plain = { ts, NULL, 0, NULL, NULL };
    Color c;
    int r = color_eval(ts, i, &c, 0);
    if (t->type == T_IDENT || (t->type != T_FUNC && r != 1)) { ser_tok(&plain, i, b, NULL, 0); return; }
    if (r == 1 && !c.derived && (c.legacy || !has_math_arg(ts, t))) { color_serialize(b, &c); return; }
    if (t->type != T_FUNC) { ser_tok(&plain, i, b, NULL, 0); return; }
    if (!strcasecmp(t->str, "color-mix")) { color_mix_spec(b, ts, t); return; }
    char name[16];
    snprintf(name, sizeof name, "%s", t->str);
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!strcmp(name, "rgba") || !strcmp(name, "hsla")) name[3] = '\0';
    buf_str(b, name);
    buf_ch(b, '(');
    bool space = false, from = false;
    for (int j = t->first; j < t->end; j = tok_next(ts, j)) {
        const Tok *a = &ts->t[j];
        if (a->type == T_COMMA) { buf_str(b, ", "); space = false; continue; }
        if (a->type == T_DELIM && a->delim == '/') { buf_str(b, " / "); space = false; continue; }
        if (space) buf_ch(b, ' ');
        space = true;
        if (from || !strcmp(name, "light-dark")) { color_spec(b, ts, j); from = false; continue; }
        if (tok_ident(a, "from")) from = true;
        ser_tok(&plain, j, b, NULL, 0);
    }
    buf_ch(b, ')');
}

static bool color_tok_valid(const Toks *ts, int i) {
    Color c;
    return color_eval(ts, i, &c, 0) != 0;
}

/* The token's color as sRGB 0-255 and alpha. */
static bool color_from_tok(const Toks *ts, const Tok *t, double rgba[4]) {
    Color c;
    if (color_eval(ts, (int)(t - ts->t), &c, 0) != 1) return false;
    color_to_rgba(&c, rgba);
    return true;
}

bool css_color_parse(const char *value, double rgba[4]) {
    Toks ts;
    if (!value || !tokenize(value, strlen(value), &ts)) return false;
    bool ok = ts.n > 0 && tok_next(&ts, 0) == ts.n && color_from_tok(&ts, &ts.t[0], rgba);
    toks_free(&ts);
    return ok;
}

/* Two equal keywords of a pair as one (border-image-repeat, repeat-style),
 * and repeat-style's repeat-x / repeat-y. */
static void collapse_pair(Buf *b, size_t start) {
    char *s = b->s + start;
    char *sp = strchr(s, ' ');
    if (!sp || strchr(sp + 1, ' ')) return;
    if (!strncmp(s, sp + 1, (size_t)(sp - s)) && strlen(sp + 1) == (size_t)(sp - s)) { *sp = '\0'; b->n = start + strlen(s); return; }
    if (!strcmp(s, "repeat no-repeat")) { b->n = start; buf_str(b, "repeat-x"); }
    else if (!strcmp(s, "no-repeat repeat")) { b->n = start; buf_str(b, "repeat-y"); }
}

/* ---- computed positions: keywords as percentages ---- */

/* One axis from its keyword (token kw, -1 none) and offset (token off). */
static void axis_computed(Ser *S, int kw, int off, Buf *out) {
    const Tok *k = kw >= 0 ? &S->ts->t[kw] : NULL;
    bool far = k && (tok_ident(k, "right") || tok_ident(k, "bottom") || tok_ident(k, "x-end") || tok_ident(k, "y-end"));
    if (off < 0) {
        buf_str(out, !k || tok_ident(k, "center") ? "50%" : far ? "100%" : "0%");
        return;
    }
    Buf o = { 0 };
    const Tok *ot = &S->ts->t[off];
    if (ot->type == T_NUMBER && ot->num == 0) buf_str(&o, "0px");
    else ser_tok(S, off, &o, NULL, 0);
    char *os = buf_take(&o);
    if (!far) buf_str(out, os);
    else if (ot->type == T_PERCENT) { fmt_number(out, 100 - ot->num); buf_ch(out, '%'); }
    else if (!strncmp(os, "calc(", 5)) { buf_str(out, "calc(100% - "); buf_str(out, os); buf_ch(out, ')'); }
    else if (os[0] == '-') { buf_str(out, "calc(100% + "); buf_str(out, os + 1); buf_ch(out, ')'); }
    else { buf_str(out, "calc(100% - "); buf_str(out, os); buf_ch(out, ')'); }
    free(os);
}

/* A <position> / <bg-position> over tokens [b, e) as "x y". */
static void position_computed(Ser *S, int b, int e, Buf *out) {
    int t[4], n = 0;
    for (int i = b; i < e && n < 4; i = tok_next(S->ts, i)) t[n++] = i;
    const Toks *ts = S->ts;
#define IS_KW(i) (ts->t[i].type == T_IDENT)
#define IS_Y(i) (tok_ident(&ts->t[i], "top") || tok_ident(&ts->t[i], "bottom"))
#define IS_X(i) (tok_ident(&ts->t[i], "left") || tok_ident(&ts->t[i], "right"))
    int xk = -1, xo = -1, yk = -1, yo = -1;
    if (n == 1) {
        if (IS_Y(t[0])) yk = t[0];
        else if (IS_KW(t[0])) xk = t[0];
        else xo = t[0];
    } else if (n == 2 && !(IS_KW(t[0]) && !IS_KW(t[1]) && !IS_Y(t[0]) && 0)) {
        bool swap = IS_Y(t[0]) || IS_X(t[1]);
        int a = swap ? t[1] : t[0], c = swap ? t[0] : t[1];
        if (IS_KW(a)) xk = a; else xo = a;
        if (IS_KW(c)) yk = c; else yo = c;
    } else {
        int center = -1;
        for (int i = 0; i < n; ) {
            int k = t[i], o = i + 1 < n && !IS_KW(t[i + 1]) ? t[i + 1] : -1;
            if (IS_X(k)) { xk = k; xo = o; }
            else if (IS_Y(k)) { yk = k; yo = o; }
            else center = k;
            i += o >= 0 ? 2 : 1;
        }
        if (xk < 0 && xo < 0) xk = center;
        else if (yk < 0 && yo < 0) yk = center;
    }
#undef IS_KW
#undef IS_Y
#undef IS_X
    axis_computed(S, xk, xo, out);
    buf_ch(out, ' ');
    axis_computed(S, yk, yo, out);
}

/* The hits under h (depth first) of <type> name. */
static int hits_of_type(Ser *S, int h, const char *name, int *out, int max) {
    int n = 0;
    for (int i = 0; i < S->nkids[h] && n < max; i++) {
        int k = S->kids[h][i];
        if (hit_is_type(&S->hits[k], name)) out[n++] = k;
        else n += hits_of_type(S, k, name, out + n, max - n);
    }
    return n;
}

static void ser_hit(Ser *S, int h, Buf *out) {
    const Hit *H = &S->hits[h];
    const GNode *g = H->node;
    if (g->kind == G_KW) { buf_str(out, g->text); return; }
    if (g_env) {
        if (hit_is_type(H, "position") || hit_is_type(H, "bg-position")) {
            position_computed(S, H->start, H->end, out);
            return;
        }
        if (g->kind == G_PROP && (!strcmp(g->text, "background-position-x") || !strcmp(g->text, "background-position-y"))) {
            /* items: center, a keyword, a keyword and an offset, an offset */
            bool first = true;
            for (int i = H->start; i < H->end; ) {
                int kw = -1, off = -1;
                if (S->ts->t[i].type == T_IDENT) { kw = i; i = tok_next(S->ts, i); }
                if (i < H->end && S->ts->t[i].type != T_COMMA) { off = i; i = tok_next(S->ts, i); }
                if (!first) buf_str(out, ", ");
                first = false;
                axis_computed(S, kw, off, out);
                if (i < H->end && S->ts->t[i].type == T_COMMA) i++;
            }
            return;
        }
        if (hit_is_type(H, "shadow") || hit_is_type(H, "shadow-t")) {
            /* color x y blur [spread] [inset], all there */
            int c[1], l[4];
            int nc = hits_of_type(S, h, "color", c, 1), nl = hits_of_type(S, h, "length", l, 4);
            if (nc) ser_hit(S, c[0], out); else fmt_rgba(out, g_env->color);
            int want = hit_is_type(H, "shadow") ? 4 : 3;
            for (int i = 0; i < want; i++) {
                buf_ch(out, ' ');
                if (i < nl) ser_hit(S, l[i], out); else buf_str(out, "0px");
            }
            for (int i = H->start; i < H->end; i = tok_next(S->ts, i))
                if (tok_ident(&S->ts->t[i], "inset")) { buf_str(out, " inset"); break; }
            return;
        }
        if (g->kind == G_TYPE && g->has_range && H->end == H->start + 1 && S->ts->t[H->start].type == T_FUNC) {
            /* a computed math function is clamped to the type's range */
            Buf tmp = { 0 };
            ser_tok(S, H->start, &tmp, NULL, 0);
            char *v = buf_take(&tmp);
            char *end = NULL;
            double x = strtod(v, &end);
            if (end != v && !strchr(v, '(')) {
                if (x < g->rmin) x = g->rmin;
                if (x > g->rmax) x = g->rmax;
                fmt_number(out, x);
                buf_str(out, end);
            } else buf_str(out, v);
            free(v);
            return;
        }
    }
    if ((hit_is_type(H, "length") || hit_is_type(H, "length-percentage")) && H->end == H->start + 1 &&
        S->ts->t[H->start].type == T_NUMBER) {
        buf_str(out, "0px");    /* a unitless zero length */
        return;
    }
    if (hit_is_type(H, "color") && tok_next(S->ts, H->start) == H->end) {
        /* sRGB syntaxes (hex, rgb(), hsl(), hwb()) as rgb()/rgba(), the
         * others in their own form (CSS Color 4 §15).  Specified values keep
         * named colors and keywords (lower-cased), relative colors and
         * color-mix(); computed ones resolve all of them. */
        const Tok *t = &S->ts->t[H->start];
        Color c;
        if (g_env && color_eval(S->ts, H->start, &c, 0) == 1) { color_serialize(out, &c); return; }
        if (!g_env && t->type != T_IDENT && color_tok_valid(S->ts, H->start)) { color_spec(out, S->ts, H->start); return; }
        if (t->type == T_IDENT) {
            for (const char *p = t->str; *p; p++) buf_ch(out, (char)tolower((unsigned char)*p));
            return;
        }
    }
    if (hit_is_type(H, "image-repeat-pair") || hit_is_type(H, "repeat-style") || hit_is_type(H, "bg-size")) {
        size_t start = out->n;
        ser_range(S, H->start, H->end, S->kids[h], S->nkids[h], out);
        if (hit_is_type(H, "bg-size")) {
            /* one size is "x auto"; "auto auto" is "auto" */
            if (!strcmp(out->s + start, "auto auto")) { out->n = start + 4; out->s[out->n] = 0; }
            else if (!strchr(out->s + start, ' ') && strcmp(out->s + start, "cover") && strcmp(out->s + start, "contain") &&
                     strcmp(out->s + start, "auto")) buf_str(out, " auto");
        } else collapse_pair(out, start);
        return;
    }
    if (g->kind == G_ANY || g->kind == G_ALL) {
        /* the members in the grammar's order */
        int nk = S->nkids[h];
        int *order = malloc((size_t)(nk ? nk : 1) * sizeof(int));
        memcpy(order, S->kids[h], (size_t)nk * sizeof(int));
        for (int i = 1; i < nk; i++)
            for (int j = i; j > 0 && S->hits[order[j]].node->idx < S->hits[order[j - 1]].node->idx; j--) {
                int x = order[j]; order[j] = order[j - 1]; order[j - 1] = x;
            }
        bool first = true;
        for (int i = 0; i < nk; i++) {
            if (S->hits[order[i]].end <= S->hits[order[i]].start) continue;
            if (!first) buf_ch(out, ' ');
            first = false;
            ser_hit(S, order[i], out);
        }
        free(order);
        return;
    }
    if (g->kind == G_FUNC) {
        const Tok *t = &S->ts->t[H->start];
        ser_tok(S, H->start, out, S->kids[h], S->nkids[h]);
        (void)t;
        return;
    }
    ser_range(S, H->start, H->end, S->kids[h], S->nkids[h], out);
}

static void ser_init(Ser *S, const Matcher *m) {
    S->ts = m->ts;
    S->hits = m->hits;
    S->nh = m->nh;
    S->kids = calloc((size_t)(m->nh ? m->nh : 1), sizeof(int *));
    S->nkids = calloc((size_t)(m->nh ? m->nh : 1), sizeof(int));
    for (int i = 0; i < m->nh; i++) {
        int p = m->hits[i].parent;
        if (p < 0) continue;
        int *nk = realloc(S->kids[p], (size_t)(S->nkids[p] + 1) * sizeof(int));
        if (!nk) continue;
        S->kids[p] = nk;
        S->kids[p][S->nkids[p]++] = i;
    }
}

static void ser_free(Ser *S) {
    for (int i = 0; i < S->nh; i++) free(S->kids[i]);
    free(S->kids);
    free(S->nkids);
}

/* ============================================================================
 * Parsing values
 * ============================================================================ */

static bool has_substitution(const char *v) {
    for (const char *p = v; *p; p++) {
        if ((p == v || !isalnum((unsigned char)p[-1])) &&
            (strncasecmp(p, "var(", 4) == 0 || strncasecmp(p, "env(", 4) == 0 || strncasecmp(p, "attr(", 5) == 0))
            return true;
    }
    return false;
}

static char *trim_dup(const char *v) {
    while (*v && isspace((unsigned char)*v)) v++;
    size_t n = strlen(v);
    while (n && isspace((unsigned char)v[n - 1])) n--;
    return strndup(v, n);
}

/* Match value against property pi's grammar.  On success the matcher holds
 * the hits (hit 0 is the property's root). */
static bool match_value(int pi, const Toks *ts, Matcher *m) {
    memset(m, 0, sizeof *m);
    m->ts = ts;
    m->cur = -1;
    if (!ts->n) return false;
    for (int i = 0; i < ts->n; i++) if (ts->t[i].type == T_BAD) return false;
    /* (a top-level ';' or '!' ends a declaration: not part of a value) */
    for (int i = 0; i < ts->n; i = tok_next(ts, i)) {
        if (ts->t[i].type == T_SEMI || (ts->t[i].type == T_DELIM && ts->t[i].delim == '!')) return false;
        if (ts->t[i].type == T_BRACE) return false;
    }
    /* (the root outlives the match: the hits point at it) */
    static GNode roots[NPROPS];
    GNode *root = &roots[pi];
    root->kind = G_PROP;
    root->min = root->max = 1;
    root->text = (char *)g_props[pi].name;
    return match_node(m, root, 0, ts->n, at_end, NULL);
}

char *css_prop_parse(const CssPropDef *p, const char *value) {
    if (!p || !value) return NULL;
    char *v = trim_dup(value);
    if (!v[0]) { free(v); return NULL; }
    if (css_is_wide_keyword(v, strlen(v))) {
        for (char *q = v; *q; q++) *q = (char)tolower((unsigned char)*q);
        return v;
    }
    if (has_substitution(v)) return v;
    if (p->shorthand != CSS_SH_NONE) {
        /* checked by expanding it; serialized back from the longhands */
        char **vals = NULL;
        int n = css_prop_expand(p, v, &vals);
        if (n < 0) { free(v); return NULL; }
        char *out = css_prop_shorthand_serialize(p, (const char *const *)vals);
        for (int i = 0; i < n; i++) free(vals[i]);
        free(vals);
        if (!out) return v;
        free(v);
        return out;
    }
    int pi = (int)(p - g_props);
    Toks ts;
    if (!tokenize(v, strlen(v), &ts)) { free(v); return NULL; }
    Matcher m;
    char *out = NULL;
    if (match_value(pi, &ts, &m)) {
        Ser S;
        ser_init(&S, &m);
        if (getenv("CSS_PROPS_DEBUG"))
            for (int i = 0; i < m.nh; i++)
                fprintf(stderr, "  hit %d kind %d text %s [%d,%d) parent %d\n", i, m.hits[i].node->kind,
                        m.hits[i].node->text ? m.hits[i].node->text : "-", m.hits[i].start, m.hits[i].end, m.hits[i].parent);
        Buf b = { 0 };
        ser_hit(&S, 0, &b);
        out = buf_take(&b);
        ser_free(&S);
    }
    free(m.hits);
    toks_free(&ts);
    free(v);
    return out;
}

char *css_prop_compute(const CssPropDef *p, const char *value, const CssComputeEnv *env) {
    if (!p || !value || !env || p->longhands) return NULL;
    char *v = trim_dup(value);
    int pi = (int)(p - g_props);
    Toks ts;
    char *out = NULL;
    if (v[0] && tokenize(v, strlen(v), &ts)) {
        Matcher m;
        if (match_value(pi, &ts, &m)) {
            Ser S;
            ser_init(&S, &m);
            g_env = env;
            Buf b = { 0 };
            ser_hit(&S, 0, &b);
            g_env = NULL;
            out = buf_take(&b);
            ser_free(&S);
        }
        free(m.hits);
        toks_free(&ts);
    }
    free(v);
    return out;
}

/* ============================================================================
 * Shorthands
 * ============================================================================ */

/* (Built on first use; layout threads call this too, so a list is complete
 * before it is published, and a racing duplicate is harmless.) */
int css_prop_longhands(const CssPropDef *p, const char ***names) {
    typedef struct { int n; const char *names[64]; } Longhands;
    static Longhands *lists[512];
    int pi = (int)(p - g_props);
    if (!p->longhands || pi < 0 || pi >= 512) { *names = NULL; return 0; }
    Longhands *l = __atomic_load_n(&lists[pi], __ATOMIC_ACQUIRE);
    if (!l) {
        l = calloc(1, sizeof *l);
        char *copy = strdup(p->longhands);
        for (char *s = copy; *s && l->n < 64; ) {
            while (*s == ' ') *s++ = '\0';
            if (!*s) break;
            l->names[l->n++] = s;
            while (*s && *s != ' ') s++;
        }
        Longhands *expected = NULL;
        if (!__atomic_compare_exchange_n(&lists[pi], &expected, l, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            free(copy);     /* (names point into it) */
            free(l);
            l = expected;
        }
    }
    *names = l->names;
    return l->n;
}

static int lh_index(const char **names, int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(names[i], name) == 0) return i;
    return -1;
}

int css_prop_shorthands_of(const CssPropDef *p, const CssPropDef *const **out) {
    static const CssPropDef **lists[NPROPS];
    static int counts[NPROPS];
    static int built;
    props_init();
    if (!built) {
        for (int s = 0; s < NPROPS; s++) {
            const CssPropDef *sh = &g_props[s];
            if (!sh->longhands) continue;
            const char **names;
            int n = css_prop_longhands(sh, &names);
            for (int i = 0; i < n; i++) {
                int li = prop_index(names[i], strlen(names[i]));
                if (li < 0) continue;
                const CssPropDef **l = realloc(lists[li], (size_t)(counts[li] + 1) * sizeof(*l));
                if (!l) continue;
                l[counts[li]++] = sh;
                lists[li] = l;
            }
        }
        built = 1;
    }
    int i = (int)(p - g_props);
    if (i < 0 || i >= NPROPS) { *out = NULL; return 0; }
    *out = (const CssPropDef *const *)lists[i];
    return counts[i];
}

static char *initial_of(const char *name) {
    const CssPropDef *d = css_prop_find(name, strlen(name));
    return strdup(d && d->initial ? d->initial : "initial");
}

/* The <'longhand'> hits under hit h that belong to the shorthand, in order. */
static void collect_props(Ser *S, int h, const char **names, int n, int *found, int *nf, int max) {
    const Hit *H = &S->hits[h];
    if (H->node->kind == G_PROP && H->prop >= 0 && h != 0 && lh_index(names, n, g_props[H->prop].name) >= 0) {
        if (*nf < max) found[(*nf)++] = h;
        return;
    }
    for (int i = 0; i < S->nkids[h]; i++) collect_props(S, S->kids[h][i], names, n, found, nf, max);
}

static int expand_special(const CssPropDef *p, const char *v, char **out, const char **names, int n);

int css_prop_expand(const CssPropDef *p, const char *value, char ***out_vals) {
    *out_vals = NULL;
    if (!p || !p->longhands || !value) return -1;
    const char **names;
    int n = css_prop_longhands(p, &names);
    char **out = calloc((size_t)n, sizeof(char *));
    char *v = trim_dup(value);
    if (!v[0]) { free(v); free(out); return -1; }
    if (css_is_wide_keyword(v, strlen(v)) || has_substitution(v)) {
        bool wide = css_is_wide_keyword(v, strlen(v));
        for (char *q = v; wide && *q; q++) *q = (char)tolower((unsigned char)*q);
        for (int i = 0; i < n; i++) out[i] = strdup(wide ? v : "");
        free(v);
        *out_vals = out;
        return n;
    }
    if (p->shorthand == CSS_SH_SPECIAL) {
        int r = expand_special(p, v, out, names, n);
        free(v);
        if (r < 0) { for (int i = 0; i < n; i++) free(out[i]); free(out); return -1; }
        *out_vals = out;
        return n;
    }
    int pi = (int)(p - g_props);
    Toks ts;
    if (!tokenize(v, strlen(v), &ts)) { free(v); free(out); return -1; }
    Matcher m;
    int result = -1;
    if (match_value(pi, &ts, &m)) {
        Ser S;
        ser_init(&S, &m);
        int found[64], nf = 0;
        collect_props(&S, 0, names, n, found, &nf, 64);
        char *vals[64] = { 0 };
        for (int i = 0; i < nf; i++) {
            Buf b = { 0 };
            ser_hit(&S, found[i], &b);
            vals[i] = buf_take(&b);
        }
        if (p->shorthand == CSS_SH_BOX && n == 4 && nf >= 1 && nf <= 4) {
            int map[4][4] = { {0,0,0,0}, {0,1,0,1}, {0,1,2,1}, {0,1,2,3} };
            for (int i = 0; i < 4; i++) out[i] = strdup(vals[map[nf - 1][i]]);
            result = n;
        } else if (p->shorthand == CSS_SH_PAIR && n == 2 && nf >= 1 && nf <= 2) {
            out[0] = strdup(vals[0]);
            out[1] = strdup(vals[nf == 2 ? 1 : 0]);
            result = n;
        } else if (p->shorthand == CSS_SH_GRAMMAR) {
            for (int i = 0; i < nf; i++) {
                int li = lh_index(names, n, g_props[S.hits[found[i]].prop].name);
                if (li >= 0 && !out[li]) out[li] = strdup(vals[i]);
            }
            for (int i = 0; i < n; i++) if (!out[i]) out[i] = initial_of(names[i]);
            result = n;
        }
        for (int i = 0; i < nf; i++) free(vals[i]);
        ser_free(&S);
    }
    free(m.hits);
    toks_free(&ts);
    free(v);
    if (result < 0) { for (int i = 0; i < n; i++) free(out[i]); free(out); return -1; }
    *out_vals = out;
    return result;
}

static int serialize_special(const CssPropDef *p, const char *const *vals, const char **names, int n, Buf *b);

static bool same_value(const char *name, const char *spec, const char *computed);
char *css_prop_shorthand_serialize(const CssPropDef *p, const char *const *vals) {
    if (!p || !p->longhands) return NULL;
    const char **names;
    int n = css_prop_longhands(p, &names);
    int wide = 0;
    for (int i = 0; i < n; i++) {
        if (!vals[i]) return NULL;
        if (!vals[i][0]) return NULL;      /* pending substitution */
        if (css_is_wide_keyword(vals[i], strlen(vals[i]))) wide++;
    }
    if (wide) {
        if (wide != n) return NULL;
        for (int i = 1; i < n; i++) if (strcmp(vals[i], vals[0]) != 0) return NULL;
        return strdup(vals[0]);
    }
    Buf b = { 0 };
    if (p->shorthand == CSS_SH_BOX && n == 4) {
        const char *t = vals[0], *r = vals[1], *bo = vals[2], *l = vals[3];
        buf_str(&b, t);
        if (strcmp(l, r) || strcmp(bo, t) || strcmp(r, t)) {
            buf_ch(&b, ' '); buf_str(&b, r);
            if (strcmp(l, r) || strcmp(bo, t)) {
                buf_ch(&b, ' '); buf_str(&b, bo);
                if (strcmp(l, r)) { buf_ch(&b, ' '); buf_str(&b, l); }
            }
        }
    } else if (p->shorthand == CSS_SH_PAIR && n == 2) {
        buf_str(&b, vals[0]);
        if (strcmp(vals[0], vals[1])) { buf_ch(&b, ' '); buf_str(&b, vals[1]); }
    } else if (p->shorthand == CSS_SH_SPECIAL) {
        if (serialize_special(p, vals, names, n, &b) < 0) { free(b.s); return NULL; }
    } else {
        /* the longhands that are not initial, in order (or the first) */
        for (int i = 0; i < n; i++) {
            const CssPropDef *d = css_prop_find(names[i], strlen(names[i]));
            if (d && d->initial && strcmp(vals[i], d->initial) == 0) continue;
            if (b.n) buf_ch(&b, ' ');
            buf_str(&b, vals[i]);
        }
        if (!b.n) buf_str(&b, vals[0]);
    }
    char *s = buf_take(&b);
    /* it must give back the same longhands */
    char **back = NULL;
    int bn = css_prop_expand(p, s, &back);
    bool same = bn == n;
    for (int i = 0; same && i < n; i++) same = strcmp(back[i], vals[i]) == 0 || same_value(names[i], back[i], vals[i]);
    for (int i = 0; i < bn; i++) free(back[i]);
    free(back);
    if (!same) { free(s); return NULL; }
    return s;
}

/* ============================================================================
 * @supports
 * ============================================================================ */

bool css_supports_property(const char *prop, const char *value) {
    if (!prop || !value) return false;
    char *name = trim_dup(prop);
    bool ok = false;
    size_t nl = strlen(name);
    if (css_prop_is_custom(name, nl)) {
        char *v = trim_dup(value);
        ok = true;
        free(v);
    } else {
        const CssPropDef *p = css_prop_find(name, nl);
        if (p) {
            char *s = css_prop_parse(p, value);
            ok = s != NULL;
            free(s);
        }
    }
    free(name);
    return ok;
}

/* condition: not <in-parens> | <in-parens> [and <in-parens>]* | <in-parens> [or <in-parens>]* */
static int sup_cond(const Toks *ts, int b, int e, const char *src);

static int sup_in_parens(const Toks *ts, int i, const char *src) {
    const Tok *t = &ts->t[i];
    if (t->type == T_FUNC) {
        if (strcasecmp(t->str, "selector") == 0) return 1;     /* (the matcher's selectors are not checked here) */
        return 0;   /* font-tech(), font-format(), unknown functions */
    }
    if (t->type != T_PAREN) return 0;
    if (t->first >= t->end) return 0;
    /* ( condition ) or ( property: value ) */
    const Tok *f = &ts->t[t->first];
    if (f->type == T_IDENT && tok_next(ts, t->first) < t->end && ts->t[tok_next(ts, t->first)].type == T_COLON) {
        /* re-serialize the value tokens as the declaration's value */
        Buf vb = { 0 };
        Ser S = { ts, NULL, 0, NULL, NULL };
        int vs = tok_next(ts, t->first) + 1;
        ser_range(&S, vs, t->end, NULL, 0, &vb);
        bool important = false;
        if (vb.s) {
            char *bang = strstr(vb.s, "! important");
            if (!bang) bang = strstr(vb.s, "!important");
            if (bang) { *bang = '\0'; important = true; }
        }
        (void)important;
        bool ok = vb.s && css_supports_property(f->str, vb.s);
        free(vb.s);
        return ok;
    }
    int r = sup_cond(ts, t->first, t->end, src);
    return r < 0 ? 0 : r;
}

static int sup_cond(const Toks *ts, int b, int e, const char *src) {
    if (b >= e) return -1;
    if (tok_ident(&ts->t[b], "not")) {
        int n = tok_next(ts, b);
        if (n >= e || tok_next(ts, n) != e) return -1;
        return !sup_in_parens(ts, n, src);
    }
    int r = sup_in_parens(ts, b, src);
    int op = 0;
    for (int i = tok_next(ts, b); i < e; ) {
        int o = tok_ident(&ts->t[i], "and") ? 1 : tok_ident(&ts->t[i], "or") ? 2 : 0;
        if (!o || (op && o != op)) return -1;
        op = o;
        i = tok_next(ts, i);
        if (i >= e) return -1;
        int x = sup_in_parens(ts, i, src);
        r = o == 1 ? (r && x) : (r || x);
        i = tok_next(ts, i);
    }
    return r;
}

bool css_supports_condition(const char *cond) {
    Toks ts;
    if (!cond || !tokenize(cond, strlen(cond), &ts)) return false;
    int r = sup_cond(&ts, 0, ts.n, cond);
    toks_free(&ts);
    return r > 0;
}

/* ============================================================================
 * Special shorthands
 * ============================================================================ */

/* A value matched against a grammar of its own (a <type> or a property). */
typedef struct Parsed { Toks ts; Matcher m; Ser S; bool ok; } Parsed;

static bool parse_type(const char *type, const char *v, Parsed *P) {
    memset(P, 0, sizeof *P);
    int ti = type_index(type);
    if (ti < 0 || !tokenize(v, strlen(v), &P->ts)) return false;
    P->m.ts = &P->ts;
    P->m.cur = -1;
    for (int i = 0; i < P->ts.n; i++) if (P->ts.t[i].type == T_BAD) return false;
    if (!P->ts.n) return false;
    static GNode roots[NTYPES];
    GNode *root = &roots[ti];
    root->kind = G_TYPE;
    root->min = root->max = 1;
    root->text = (char *)g_types[ti].name;
    P->ok = match_node(&P->m, root, 0, P->ts.n, at_end, NULL);
    if (P->ok) ser_init(&P->S, &P->m);
    return P->ok;
}

static bool parse_prop(const char *prop, const char *v, Parsed *P) {
    memset(P, 0, sizeof *P);
    int pi = prop_index(prop, strlen(prop));
    if (pi < 0 || !tokenize(v, strlen(v), &P->ts)) return false;
    P->ok = match_value(pi, &P->ts, &P->m);
    if (P->ok) ser_init(&P->S, &P->m);
    return P->ok;
}

static void parsed_free(Parsed *P) {
    if (P->ok) ser_free(&P->S);
    free(P->m.hits);
    toks_free(&P->ts);
}

static char *hit_text(Parsed *P, int h) {
    Buf b = { 0 };
    ser_hit(&P->S, h, &b);
    return buf_take(&b);
}

/* The first hit under h (depth-first, h itself included) that is the
 * <type> or <'property'> name; -1 when none. */
static int find_named(Parsed *P, int h, const char *name) {
    const Hit *H = &P->m.hits[h];
    if ((H->node->kind == G_TYPE || H->node->kind == G_PROP) && strcmp(H->node->text, name) == 0) return h;
    for (int i = 0; i < P->S.nkids[h]; i++) {
        int r = find_named(P, P->S.kids[h][i], name);
        if (r >= 0) return r;
    }
    return -1;
}

/* All the hits of <type> name directly repeated under h (the layers of a
 * list), in order. */
static int find_all_named(Parsed *P, int h, const char *name, int *out, int max) {
    int n = 0;
    const Hit *H = &P->m.hits[h];
    if (H->node->kind == G_TYPE && strcmp(H->node->text, name) == 0 && h != 0) { out[0] = h; return 1; }
    for (int i = 0; i < P->S.nkids[h] && n < max; i++) n += find_all_named(P, P->S.kids[h][i], name, out + n, max - n);
    return n;
}

/* The members of the || group of <type> hit h: hit[i] for member i of the
 * grammar (-1 when not given). */
static void members(Parsed *P, int h, int *hit, int n) {
    for (int i = 0; i < n; i++) hit[i] = -1;
    int g = h;
    /* down to the || node */
    while (g >= 0 && P->m.hits[g].node->kind != G_ANY && P->S.nkids[g] == 1) g = P->S.kids[g][0];
    if (g < 0 || P->m.hits[g].node->kind != G_ANY) return;
    for (int i = 0; i < P->S.nkids[g]; i++) {
        int k = P->S.kids[g][i];
        int idx = P->m.hits[k].node->idx;
        if (idx >= 0 && idx < n && hit[idx] < 0) hit[idx] = k;
    }
}

static void set_out(char **out, int i, const char *v) { free(out[i]); out[i] = strdup(v); }

/* The longhand named name's slot. */
static int slot(const char **names, int n, const char *name) { return lh_index(names, n, name); }

/* Split a value at its top-level commas (each part trimmed). */
static int split_commas(const char *v, char ***parts) {
    Toks ts;
    *parts = NULL;
    if (!tokenize(v, strlen(v), &ts)) return 0;
    int n = 1;
    for (int i = 0; i < ts.n; i = tok_next(&ts, i)) if (ts.t[i].type == T_COMMA) n++;
    toks_free(&ts);
    char **p = calloc((size_t)n, sizeof(char *));
    int k = 0, depth = 0;
    const char *s = v;
    char q = 0;
    for (const char *c = v; ; c++) {
        if (q) { if (*c == '\\' && c[1]) c++; else if (*c == q) q = 0; if (*c) continue; }
        if (*c == '"' || *c == '\'') { q = *c; continue; }
        if (*c == '(' || *c == '[') depth++;
        else if ((*c == ')' || *c == ']') && depth) depth--;
        if (!*c || (*c == ',' && !depth)) {
            if (k < n) { char *raw = strndup(s, (size_t)(c - s)); p[k++] = trim_dup(raw); free(raw); }
            if (!*c) break;
            s = c + 1;
        }
    }
    *parts = p;
    return k;
}

/* "a, b, c" from parts. */
static char *join(char **parts, int n, const char *sep) {
    Buf b = { 0 };
    for (int i = 0; i < n; i++) { if (i) buf_str(&b, sep); buf_str(&b, parts[i] ? parts[i] : ""); }
    return buf_take(&b);
}

static void free_parts(char **p, int n) { for (int i = 0; i < n; i++) free(p[i]); free(p); }

/* ---- background-position: x and y of a <bg-position> ---- */

static bool tok_is(const Tok *t, const char *a, const char *b) { return tok_ident(t, a) || (b && tok_ident(t, b)); }

static void bg_position_xy(Parsed *P, int h, char **x, char **y) {
    const Hit *H = &P->m.hits[h];
    const Toks *ts = &P->ts;
    int idx[4], n = 0;
    for (int i = H->start; i < H->end && n < 4; i = tok_next(ts, i)) idx[n++] = i;
    char *tx[4];
    for (int i = 0; i < n; i++) {
        Buf b = { 0 };
        const Tok *t = &ts->t[idx[i]];
        if (t->type == T_NUMBER && t->num == 0) buf_str(&b, "0px");
        else ser_tok(&P->S, idx[i], &b, NULL, 0);
        tx[i] = buf_take(&b);
        for (char *q = tx[i]; t->type == T_IDENT && *q; q++) *q = (char)tolower((unsigned char)*q);
    }
    const Tok *t0 = n > 0 ? &ts->t[idx[0]] : NULL, *t1 = n > 1 ? &ts->t[idx[1]] : NULL;
    if (n == 1) {
        if (tok_is(t0, "top", "bottom")) { *x = strdup("center"); *y = strdup(tx[0]); }
        else if (tok_is(t0, "left", "right")) { *x = strdup(tx[0]); *y = strdup("center"); }
        else if (tok_ident(t0, "center")) { *x = strdup("center"); *y = strdup("center"); }
        else { *x = strdup(tx[0]); *y = strdup("center"); }
    } else if (n == 2) {
        bool swap = tok_is(t0, "top", "bottom") || tok_is(t1, "left", "right");
        *x = strdup(tx[swap ? 1 : 0]);
        *y = strdup(tx[swap ? 0 : 1]);
    } else {
        /* keyword groups, each with an optional offset */
        char *gx = NULL, *gy = NULL, *gc = NULL;
        for (int i = 0; i < n; ) {
            Buf b = { 0 };
            buf_str(&b, tx[i]);
            bool off = i + 1 < n && ts->t[idx[i + 1]].type != T_IDENT;
            if (off) { buf_ch(&b, ' '); buf_str(&b, tx[i + 1]); }
            char *g = buf_take(&b);
            const Tok *k = &ts->t[idx[i]];
            if (tok_is(k, "left", "right")) gx = g;
            else if (tok_is(k, "top", "bottom")) gy = g;
            else gc = g;
            i += off ? 2 : 1;
        }
        if (!gx) gx = gc ? gc : strdup("center");
        else if (!gy) gy = gc ? gc : strdup("center");
        else free(gc);
        if (!gy) gy = strdup("center");
        *x = gx;
        *y = gy;
    }
    for (int i = 0; i < n; i++) free(tx[i]);
}

/* ---- the expanders ---- */

static int expand_border_sides(const char *v, char **out, const char **names, int n, const char *one_side,
                               const char *const *sides, int nsides) {
    char **vals = NULL;
    const CssPropDef *side = css_prop_find(one_side, strlen(one_side));
    if (css_prop_expand(side, v, &vals) != 3) return -1;
    static const char *const parts[] = { "width", "style", "color" };
    for (int s = 0; s < nsides; s++)
        for (int k = 0; k < 3; k++) {
            char name[64];
            snprintf(name, sizeof name, "border-%s-%s", sides[s], parts[k]);
            int i = slot(names, n, name);
            if (i >= 0) set_out(out, i, vals[k]);
        }
    for (int k = 0; k < 3; k++) free(vals[k]);
    free(vals);
    for (int i = 0; i < n; i++) if (!out[i]) out[i] = initial_of(names[i]);
    return n;
}

static int expand_border_radius(const char *v, char **out) {
    Parsed P;
    if (!parse_prop("border-radius", v, &P)) { parsed_free(&P); return -1; }
    /* the lengths before and after '/' */
    char *h[4] = { 0 }, *w[4] = { 0 };
    int nh = 0, nv = 0;
    bool after = false;
    for (int i = 0; i < P.ts.n; i = tok_next(&P.ts, i)) {
        const Tok *t = &P.ts.t[i];
        if (t->type == T_DELIM && t->delim == '/') { after = true; continue; }
        Buf b = { 0 };
        if (t->type == T_NUMBER) buf_str(&b, "0px"); else ser_tok(&P.S, i, &b, NULL, 0);
        if (after) { if (nv < 4) w[nv++] = buf_take(&b); else free(b.s); }
        else { if (nh < 4) h[nh++] = buf_take(&b); else free(b.s); }
    }
    static const int map[4][4] = { {0,0,0,0}, {0,1,0,1}, {0,1,2,1}, {0,1,2,3} };
    for (int c = 0; c < 4; c++) {
        const char *hv = h[map[nh - 1][c]];
        const char *vv = nv ? w[map[nv - 1][c]] : hv;
        Buf b = { 0 };
        buf_str(&b, hv);
        if (strcmp(hv, vv)) { buf_ch(&b, ' '); buf_str(&b, vv); }
        out[c] = buf_take(&b);
    }
    for (int i = 0; i < nh; i++) free(h[i]);
    for (int i = 0; i < nv; i++) free(w[i]);
    parsed_free(&P);
    return 4;
}

static int expand_flex(const char *v, char **out) {
    if (strcasecmp(v, "none") == 0) {
        out[0] = strdup("0"); out[1] = strdup("0"); out[2] = strdup("auto");
        return 3;
    }
    Parsed P;
    if (!parse_prop("flex", v, &P)) { parsed_free(&P); return -1; }
    int g = find_named(&P, 0, "flex-grow"), s = find_named(&P, 0, "flex-shrink"), b = find_named(&P, 0, "flex-basis");
    out[0] = g >= 0 ? hit_text(&P, g) : strdup("1");
    out[1] = s >= 0 ? hit_text(&P, s) : strdup("1");
    out[2] = b >= 0 ? hit_text(&P, b) : strdup("0%");
    parsed_free(&P);
    return 3;
}

static int expand_list_style(const char *v, char **out) {
    Parsed P;
    if (!parse_prop("list-style", v, &P)) { parsed_free(&P); return -1; }
    int pos = find_named(&P, 0, "list-style-position"), img = find_named(&P, 0, "list-style-image"),
        typ = find_named(&P, 0, "list-style-type");
    int nones = 0;
    for (int i = 0; i < P.ts.n; i = tok_next(&P.ts, i)) if (tok_ident(&P.ts.t[i], "none")) nones++;
    out[0] = pos >= 0 ? hit_text(&P, pos) : strdup("outside");
    out[1] = img >= 0 ? hit_text(&P, img) : NULL;
    out[2] = typ >= 0 ? hit_text(&P, typ) : NULL;
    /* "none" sets whichever of image and type is not otherwise given */
    if (nones) {
        if (!out[1]) out[1] = strdup("none");
        if (!out[2]) out[2] = strdup("none");
    }
    if (!out[1]) out[1] = strdup("none");
    if (!out[2]) out[2] = strdup("disc");
    parsed_free(&P);
    return 3;
}

/* grid-row / grid-column / grid-area: <grid-line> values split at '/'. */
static bool grid_ident(const char *s) {
    Toks ts;
    if (!tokenize(s, strlen(s), &ts)) return false;
    bool r = ts.n == 1 && ts.t[0].type == T_IDENT && !tok_ident(&ts.t[0], "auto");
    toks_free(&ts);
    return r;
}

static int expand_grid_lines(const char *v, char **out, int n) {
    /* split at top-level '/' */
    char *parts[4] = { 0 };
    int np = 0;
    const char *s = v;
    for (const char *c = v; ; c++) {
        if (!*c || *c == '/') {
            if (np == 4) { for (int i = 0; i < np; i++) free(parts[i]); return -1; }
            char *raw = strndup(s, (size_t)(c - s));
            parts[np++] = trim_dup(raw);
            free(raw);
            if (!*c) break;
            s = c + 1;
        }
    }
    int max = n == 4 ? 4 : 2;
    if (np > max) { for (int i = 0; i < np; i++) free(parts[i]); return -1; }
    char *vals[4] = { 0 };
    for (int i = 0; i < np; i++) {
        vals[i] = css_prop_parse(css_prop_find("grid-row-start", 14), parts[i]);
        free(parts[i]);
        if (!vals[i]) { for (int k = 0; k < i; k++) free(vals[k]); for (int k = i + 1; k < np; k++) free(parts[k]); return -1; }
    }
    if (n == 2) {
        out[0] = vals[0];
        out[1] = np > 1 ? vals[1] : strdup(grid_ident(vals[0]) ? vals[0] : "auto");
        return 2;
    }
    /* grid-area: row-start, column-start, row-end, column-end */
    out[0] = vals[0];
    out[1] = np > 1 ? vals[1] : strdup(grid_ident(out[0]) ? out[0] : "auto");
    out[2] = np > 2 ? vals[2] : strdup(grid_ident(out[0]) ? out[0] : "auto");
    out[3] = np > 3 ? vals[3] : strdup(grid_ident(out[1]) ? out[1] : "auto");
    return 4;
}

static int expand_background(const char *v, char **out, const char **names, int n) {
    Parsed P;
    if (!parse_type("background-syntax", v, &P)) { parsed_free(&P); return -1; }
    int layers[64];
    int nl = find_all_named(&P, 0, "bg-layer", layers, 63);
    int fl = find_named(&P, 0, "final-bg-layer");
    if (fl < 0) { parsed_free(&P); return -1; }
    layers[nl++] = fl;
    char **image = calloc((size_t)nl, sizeof(char *)), **px = calloc((size_t)nl, sizeof(char *)),
         **py = calloc((size_t)nl, sizeof(char *)), **size = calloc((size_t)nl, sizeof(char *)),
         **rep = calloc((size_t)nl, sizeof(char *)), **att = calloc((size_t)nl, sizeof(char *)),
         **org = calloc((size_t)nl, sizeof(char *)), **clip = calloc((size_t)nl, sizeof(char *));
    char *color = NULL;
    for (int l = 0; l < nl; l++) {
        int m[7];
        members(&P, layers[l], m, 7);
        image[l] = m[0] >= 0 ? hit_text(&P, m[0]) : strdup("none");
        if (m[1] >= 0) {
            int pos = find_named(&P, m[1], "bg-position"), sz = find_named(&P, m[1], "bg-size");
            bg_position_xy(&P, pos, &px[l], &py[l]);
            size[l] = sz >= 0 ? hit_text(&P, sz) : strdup("auto");
        } else {
            px[l] = strdup("0%"); py[l] = strdup("0%"); size[l] = strdup("auto");
        }
        rep[l] = m[2] >= 0 ? hit_text(&P, m[2]) : strdup("repeat");
        att[l] = m[3] >= 0 ? hit_text(&P, m[3]) : strdup("scroll");
        if (m[4] >= 0 && m[5] >= 0) {
            /* the first box given is the origin, the second the clip */
            int a = P.m.hits[m[4]].start < P.m.hits[m[5]].start ? m[4] : m[5], b = a == m[4] ? m[5] : m[4];
            org[l] = hit_text(&P, a);
            clip[l] = hit_text(&P, b);
        } else if (m[4] >= 0 || m[5] >= 0) {
            org[l] = hit_text(&P, m[4] >= 0 ? m[4] : m[5]);
            clip[l] = strdup(org[l]);
        } else {
            org[l] = strdup("padding-box");
            clip[l] = strdup("border-box");
        }
        if (l == nl - 1) {
            /* (the final layer's 7th member) */
            color = m[6] >= 0 ? hit_text(&P, m[6]) : strdup("transparent");
        }
    }
    char **lists[] = { image, px, py, size, rep, att, org, clip };
    static const char *const lnames[] = { "background-image", "background-position-x", "background-position-y",
        "background-size", "background-repeat", "background-attachment", "background-origin", "background-clip" };
    for (int k = 0; k < 8; k++) {
        int i = slot(names, n, lnames[k]);
        if (i >= 0) out[i] = join(lists[k], nl, ", ");
        free_parts(lists[k], nl);
    }
    int ci = slot(names, n, "background-color");
    if (ci >= 0) out[ci] = color; else free(color);
    parsed_free(&P);
    return n;
}

static int expand_background_position(const char *v, char **out) {
    char **parts;
    int np = split_commas(v, &parts);
    char **xs = calloc((size_t)(np ? np : 1), sizeof(char *)), **ys = calloc((size_t)(np ? np : 1), sizeof(char *));
    int ok = np > 0;
    for (int i = 0; i < np && ok; i++) {
        Parsed P;
        if (!parts[i][0] || !parse_type("bg-position", parts[i], &P)) ok = 0;
        else bg_position_xy(&P, 0, &xs[i], &ys[i]);
        parsed_free(&P);
    }
    if (ok) { out[0] = join(xs, np, ", "); out[1] = join(ys, np, ", "); }
    free_parts(xs, np);
    free_parts(ys, np);
    free_parts(parts, np);
    return ok ? 2 : -1;
}

/* transition and animation: per comma-separated layer, the members of
 * <single-transition> / <single-animation> into their longhands' lists. */
static int expand_layers(const char *v, char **out, const char **names, int n, const char *type,
                         const char *const *member_props, const char *const *defaults, int nm) {
    char **parts;
    int np = split_commas(v, &parts);
    char ***cols = calloc((size_t)nm, sizeof(char **));
    for (int k = 0; k < nm; k++) cols[k] = calloc((size_t)(np ? np : 1), sizeof(char *));
    bool ok = np > 0;
    for (int l = 0; l < np && ok; l++) {
        Parsed P;
        if (!parts[l][0] || !parse_type(type, parts[l], &P)) { ok = false; parsed_free(&P); break; }
        int m[16];
        members(&P, 0, m, nm);
        for (int k = 0; k < nm; k++) cols[k][l] = m[k] >= 0 ? hit_text(&P, m[k]) : strdup(defaults[k]);
        parsed_free(&P);
    }
    /* "none" as a transition-property only alone */
    for (int k = 0; k < n; k++) out[k] = NULL;
    if (ok) {
        for (int k = 0; k < nm; k++) {
            int i = slot(names, n, member_props[k]);
            if (i >= 0) out[i] = join(cols[k], np, ", ");
        }
        for (int i = 0; i < n; i++) if (!out[i]) out[i] = initial_of(names[i]);
    }
    for (int k = 0; k < nm; k++) free_parts(cols[k], np);
    free(cols);
    free_parts(parts, np);
    return ok ? n : -1;
}

static int expand_font(const char *v, char **out, const char **names, int n) {
    Parsed P;
    if (!parse_type("font-syntax", v, &P)) { parsed_free(&P); return -1; }
    int st = find_named(&P, 0, "font-style"), va = find_named(&P, 0, "font-variant-css2"),
        we = find_named(&P, 0, "font-weight"), sr = find_named(&P, 0, "font-stretch-css3"),
        sz = find_named(&P, 0, "font-size"), lh = find_named(&P, 0, "line-height"),
        fa = find_named(&P, 0, "font-family");
    if (sz < 0) { parsed_free(&P); return -1; }    /* (system fonts are not kept) */
    const struct { const char *name; int hit; const char *dflt; } m[] = {
        { "font-style", st, "normal" }, { "font-variant-caps", va, "normal" }, { "font-weight", we, "normal" },
        { "font-stretch", sr, "normal" }, { "font-size", sz, "medium" }, { "line-height", lh, "normal" },
        { "font-family", fa, "serif" },
    };
    for (size_t k = 0; k < sizeof m / sizeof m[0]; k++) {
        int i = slot(names, n, m[k].name);
        if (i >= 0) out[i] = m[k].hit >= 0 ? hit_text(&P, m[k].hit) : strdup(m[k].dflt);
    }
    for (int i = 0; i < n; i++) if (!out[i]) out[i] = initial_of(names[i]);
    parsed_free(&P);
    return n;
}

static int expand_font_variant(const char *v, char **out, const char **names, int n) {
    for (int i = 0; i < n; i++) out[i] = strdup("normal");
    if (strcasecmp(v, "normal") == 0) return n;
    if (strcasecmp(v, "none") == 0) { set_out(out, slot(names, n, "font-variant-ligatures"), "none"); return n; }
    Parsed P;
    if (!parse_prop("font-variant", v, &P)) { parsed_free(&P); return -1; }
    /* each keyword to the longhand whose grammar takes it, in that grammar's order */
    static const char *const lh[] = { "font-variant-ligatures", "font-variant-alternates", "font-variant-caps",
        "font-variant-numeric", "font-variant-east-asian", "font-variant-position", "font-variant-emoji" };
    Buf acc[7] = { { 0 } };
    for (int i = 0; i < P.ts.n; i = tok_next(&P.ts, i)) {
        const Tok *t = &P.ts.t[i];
        if (t->type != T_IDENT) continue;
        for (int k = 0; k < 7; k++) {
            const CssPropDef *d = css_prop_find(lh[k], strlen(lh[k]));
            char *one = d ? css_prop_parse(d, t->str) : NULL;
            if (!one) continue;
            if (acc[k].n) buf_ch(&acc[k], ' ');
            buf_str(&acc[k], one);
            free(one);
            break;
        }
    }
    for (int k = 0; k < 7; k++) {
        if (!acc[k].n) continue;
        char *raw = buf_take(&acc[k]);
        const CssPropDef *d = css_prop_find(lh[k], strlen(lh[k]));
        char *canon = css_prop_parse(d, raw);    /* (in the longhand's canonical order) */
        int i = slot(names, n, lh[k]);
        if (i >= 0) set_out(out, i, canon ? canon : raw);
        free(canon);
        free(raw);
    }
    parsed_free(&P);
    return n;
}

static int expand_font_synthesis(const char *v, char **out, const char **names, int n) {
    Parsed P;
    if (!parse_prop("font-synthesis", v, &P)) { parsed_free(&P); return -1; }
    static const char *const kw[] = { "weight", "style", "small-caps", "position" };
    for (int k = 0; k < 4; k++) {
        bool on = false;
        for (int i = 0; i < P.ts.n; i++) if (tok_ident(&P.ts.t[i], kw[k])) on = true;
        char name[64];
        snprintf(name, sizeof name, "font-synthesis-%s", kw[k]);
        int i = slot(names, n, name);
        if (i >= 0) out[i] = strdup(on ? "auto" : "none");
    }
    parsed_free(&P);
    return n;
}

static int expand_special(const CssPropDef *p, const char *v, char **out, const char **names, int n) {
    const char *nm = p->name;
    if (!strcmp(nm, "border")) {
        static const char *const s[] = { "top", "right", "bottom", "left" };
        return expand_border_sides(v, out, names, n, "border-top", s, 4);
    }
    if (!strcmp(nm, "border-block")) {
        static const char *const s[] = { "block-start", "block-end" };
        return expand_border_sides(v, out, names, n, "border-block-start", s, 2);
    }
    if (!strcmp(nm, "border-inline")) {
        static const char *const s[] = { "inline-start", "inline-end" };
        return expand_border_sides(v, out, names, n, "border-inline-start", s, 2);
    }
    if (!strcmp(nm, "border-radius")) return expand_border_radius(v, out);
    if (!strcmp(nm, "flex")) return expand_flex(v, out);
    if (!strcmp(nm, "list-style")) return expand_list_style(v, out);
    if (!strcmp(nm, "grid-row") || !strcmp(nm, "grid-column")) return expand_grid_lines(v, out, 2);
    if (!strcmp(nm, "grid-area")) return expand_grid_lines(v, out, 4);
    if (!strcmp(nm, "background")) return expand_background(v, out, names, n);
    if (!strcmp(nm, "background-position")) return expand_background_position(v, out);
    if (!strcmp(nm, "font")) return expand_font(v, out, names, n);
    if (!strcmp(nm, "font-variant")) return expand_font_variant(v, out, names, n);
    if (!strcmp(nm, "font-synthesis")) return expand_font_synthesis(v, out, names, n);
    if (!strcmp(nm, "marker")) {
        char *one = css_prop_parse(css_prop_find("marker-start", 12), v);
        if (!one) return -1;
        for (int i = 0; i < n; i++) out[i] = strdup(one);
        free(one);
        return n;
    }
    if (!strcmp(nm, "transition")) {
        static const char *const props[] = { "transition-property", "transition-duration", "transition-timing-function",
                                             "transition-delay", "transition-behavior" };
        static const char *const dflt[] = { "all", "0s", "ease", "0s", "normal" };
        return expand_layers(v, out, names, n, "single-transition", props, dflt, 5);
    }
    if (!strcmp(nm, "animation")) {
        static const char *const props[] = { "animation-duration", "animation-timing-function", "animation-delay",
            "animation-iteration-count", "animation-direction", "animation-fill-mode", "animation-play-state",
            "animation-name" };
        static const char *const dflt[] = { "0s", "ease", "0s", "1", "normal", "none", "running", "none" };
        return expand_layers(v, out, names, n, "single-animation", props, dflt, 8);
    }
    return -1;
}

/* ---- serializing them back ---- */

static const char *val_of(const char *const *vals, const char **names, int n, const char *name) {
    int i = lh_index(names, n, name);
    return i >= 0 ? vals[i] : NULL;
}

/* Specified keywords equal to computed values (font: normal is weight 400,
 * a stretch keyword its percentage), for shorthand round trips. */
static bool same_value(const char *name, const char *spec, const char *computed) {
    static const char *const kw[][2] = {
        { "font-weight", "normal" }, { "font-weight", "bold" },
        { "font-stretch", "ultra-condensed" }, { "font-stretch", "extra-condensed" }, { "font-stretch", "condensed" },
        { "font-stretch", "semi-condensed" }, { "font-stretch", "normal" }, { "font-stretch", "semi-expanded" },
        { "font-stretch", "expanded" }, { "font-stretch", "extra-expanded" }, { "font-stretch", "ultra-expanded" },
    };
    static const char *const val[] = { "400", "700", "50%", "62.5%", "75%", "87.5%", "100%", "112.5%", "125%", "150%", "200%" };
    for (size_t k = 0; k < sizeof val / sizeof val[0]; k++)
        if (!strcmp(name, kw[k][0]) && !strcmp(spec, kw[k][1]) && !strcmp(computed, val[k])) return true;
    return false;
}

static bool is_initial(const char *name, const char *v) {
    const CssPropDef *d = css_prop_find(name, strlen(name));
    return d && d->initial && v && strcmp(d->initial, v) == 0;
}

/* Box-collapse four values into b. */
static void box4(Buf *b, const char *t, const char *r, const char *bo, const char *l) {
    buf_str(b, t);
    if (strcmp(l, r) || strcmp(bo, t) || strcmp(r, t)) {
        buf_ch(b, ' '); buf_str(b, r);
        if (strcmp(l, r) || strcmp(bo, t)) {
            buf_ch(b, ' '); buf_str(b, bo);
            if (strcmp(l, r)) { buf_ch(b, ' '); buf_str(b, l); }
        }
    }
}

static int serialize_layers(const char *const *vals, const char **names, int n, const char *const *props,
                            int nm, Buf *b, bool transition) {
    char **cols[16];
    int len = -1;
    for (int k = 0; k < nm; k++) {
        const char *v = val_of(vals, names, n, props[k]);
        int c = split_commas(v ? v : "", &cols[k]);
        if (len >= 0 && c != len) { for (int j = 0; j <= k; j++) free_parts(cols[j], j == k ? c : len); return -1; }
        len = c;
    }
    /* the other longhands (timeline, range, composition) must be initial */
    for (int i = 0; i < n; i++) {
        bool member = false;
        for (int k = 0; k < nm; k++) if (!strcmp(names[i], props[k])) member = true;
        if (!member && !is_initial(names[i], vals[i])) { for (int k = 0; k < nm; k++) free_parts(cols[k], len); return -1; }
    }
    for (int l = 0; l < len; l++) {
        if (l) buf_str(b, ", ");
        size_t start = b->n;
        if (transition) {
            /* property duration easing delay behavior, the initial ones left out
             * (the duration kept when the delay is given) */
            const char *pr = cols[0][l], *du = cols[1][l], *ea = cols[2][l], *de = cols[3][l], *be = cols[4][l];
            bool sp = false;
            #define PUT(x) do { if (sp) buf_ch(b, ' '); buf_str(b, x); sp = true; } while (0)
            if (strcmp(pr, "all")) PUT(pr);
            if (strcmp(du, "0s") || strcmp(de, "0s")) PUT(du);
            if (strcmp(ea, "ease")) PUT(ea);
            if (strcmp(de, "0s")) PUT(de);
            if (strcmp(be, "normal")) PUT(be);
            if (b->n == start) buf_str(b, "all");
            #undef PUT
        } else {
            /* animation: every member */
            for (int k = 0; k < nm; k++) { if (k) buf_ch(b, ' '); buf_str(b, cols[k][l]); }
        }
    }
    for (int k = 0; k < nm; k++) free_parts(cols[k], len);
    return 0;
}

static int serialize_special(const CssPropDef *p, const char *const *vals, const char **names, int n, Buf *b) {
    const char *nm = p->name;
    if (!strcmp(nm, "border") || !strcmp(nm, "border-block") || !strcmp(nm, "border-inline")) {
        /* every side the same, and border-image initial */
        const char *w = NULL, *s = NULL, *c = NULL;
        for (int i = 0; i < n; i++) {
            const char *nmi = names[i];
            if (!strncmp(nmi, "border-image", 12)) { if (!is_initial(nmi, vals[i])) return -1; continue; }
            const char **slotp = strstr(nmi, "-width") ? &w : strstr(nmi, "-style") ? &s : &c;
            if (*slotp && strcmp(*slotp, vals[i])) return -1;
            *slotp = vals[i];
        }
        bool sp = false;
        if (strcmp(w, "medium")) { buf_str(b, w); sp = true; }
        if (strcmp(s, "none")) { if (sp) buf_ch(b, ' '); buf_str(b, s); sp = true; }
        if (strcmp(c, "currentcolor")) { if (sp) buf_ch(b, ' '); buf_str(b, c); sp = true; }
        if (!sp) buf_str(b, "none");
        return 0;
    }
    if (!strcmp(nm, "border-radius")) {
        char h[4][128], v[4][128];
        for (int c = 0; c < 4; c++) {
            const char *s = vals[c];
            const char *sp = strchr(s, ' ');
            if (sp) { snprintf(h[c], sizeof h[c], "%.*s", (int)(sp - s), s); snprintf(v[c], sizeof v[c], "%s", sp + 1); }
            else { snprintf(h[c], sizeof h[c], "%s", s); snprintf(v[c], sizeof v[c], "%s", s); }
        }
        box4(b, h[0], h[1], h[2], h[3]);
        if (strcmp(h[0], v[0]) || strcmp(h[1], v[1]) || strcmp(h[2], v[2]) || strcmp(h[3], v[3])) {
            buf_str(b, " / ");
            box4(b, v[0], v[1], v[2], v[3]);
        }
        return 0;
    }
    if (!strcmp(nm, "flex")) {
        buf_str(b, vals[0]); buf_ch(b, ' '); buf_str(b, vals[1]); buf_ch(b, ' '); buf_str(b, vals[2]);
        return 0;
    }
    if (!strcmp(nm, "list-style")) {
        bool sp = false;
        const char *pos = vals[0], *img = vals[1], *typ = vals[2];
        if (!strcmp(img, "none") && !strcmp(typ, "none")) {
            if (strcmp(pos, "outside")) { buf_str(b, pos); buf_ch(b, ' '); }
            buf_str(b, "none");
            return 0;
        }
        if (strcmp(pos, "outside")) { buf_str(b, pos); sp = true; }
        if (strcmp(img, "none")) { if (sp) buf_ch(b, ' '); buf_str(b, img); sp = true; }
        if (strcmp(typ, "disc")) { if (sp) buf_ch(b, ' '); buf_str(b, typ); sp = true; }
        if (!sp) buf_str(b, "outside");
        return 0;
    }
    if (!strcmp(nm, "grid-row") || !strcmp(nm, "grid-column")) {
        buf_str(b, vals[0]);
        bool omit = grid_ident(vals[0]) ? !strcmp(vals[1], vals[0]) : !strcmp(vals[1], "auto");
        if (!omit) { buf_str(b, " / "); buf_str(b, vals[1]); }
        return 0;
    }
    if (!strcmp(nm, "grid-area")) {
        /* drop trailing values that equal what they would default to */
        int keep = 4;
        if (keep == 4 && !strcmp(vals[3], grid_ident(vals[1]) ? vals[1] : "auto")) keep = 3;
        if (keep == 3 && !strcmp(vals[2], grid_ident(vals[0]) ? vals[0] : "auto")) keep = 2;
        if (keep == 2 && !strcmp(vals[1], grid_ident(vals[0]) ? vals[0] : "auto")) keep = 1;
        for (int i = 0; i < keep; i++) { if (i) buf_str(b, " / "); buf_str(b, vals[i]); }
        return 0;
    }
    if (!strcmp(nm, "background-position")) {
        char **xs, **ys;
        int nx = split_commas(vals[0], &xs), ny = split_commas(vals[1], &ys);
        if (nx != ny) { free_parts(xs, nx); free_parts(ys, ny); return -1; }
        for (int i = 0; i < nx; i++) {
            if (i) buf_str(b, ", ");
            buf_str(b, xs[i]); buf_ch(b, ' '); buf_str(b, ys[i]);
        }
        free_parts(xs, nx);
        free_parts(ys, ny);
        return 0;
    }
    if (!strcmp(nm, "background")) {
        static const char *const lnames[] = { "background-image", "background-position-x", "background-position-y",
            "background-size", "background-repeat", "background-attachment", "background-origin", "background-clip" };
        char **cols[8];
        int len = -1;
        for (int k = 0; k < 8; k++) {
            const char *v = val_of(vals, names, n, lnames[k]);
            int c = split_commas(v ? v : "", &cols[k]);
            if (len >= 0 && c != len) { for (int j = 0; j <= k; j++) free_parts(cols[j], j == k ? c : len); return -1; }
            len = c;
        }
        const char *color = val_of(vals, names, n, "background-color");
        for (int l = 0; l < len; l++) {
            if (l) buf_str(b, ", ");
            size_t start = b->n;
            bool sp = false;
            #define PUT(x) do { if (sp) buf_ch(b, ' '); buf_str(b, x); sp = true; } while (0)
            if (strcmp(cols[0][l], "none")) PUT(cols[0][l]);
            bool pos = strcmp(cols[1][l], "0%") || strcmp(cols[2][l], "0%"), size = strcmp(cols[3][l], "auto");
            if (pos || size) {
                if (sp) buf_ch(b, ' ');
                buf_str(b, cols[1][l]); buf_ch(b, ' '); buf_str(b, cols[2][l]);
                if (size) { buf_str(b, " / "); buf_str(b, cols[3][l]); }
                sp = true;
            }
            if (strcmp(cols[4][l], "repeat")) PUT(cols[4][l]);
            if (strcmp(cols[5][l], "scroll")) PUT(cols[5][l]);
            bool org = strcmp(cols[6][l], "padding-box"), clip = strcmp(cols[7][l], "border-box");
            if (org || clip) {
                PUT(cols[6][l]);
                if (strcmp(cols[6][l], cols[7][l])) PUT(cols[7][l]);
            }
            if (l == len - 1 && color && strcmp(color, "transparent")) PUT(color);
            if (b->n == start) buf_str(b, "none");
            #undef PUT
        }
        for (int k = 0; k < 8; k++) free_parts(cols[k], len);
        return 0;
    }
    if (!strcmp(nm, "font")) {
        /* the reset-only longhands must be initial */
        static const char *const shown[] = { "font-style", "font-variant-caps", "font-weight", "font-stretch",
                                             "font-size", "line-height", "font-family" };
        for (int i = 0; i < n; i++) {
            bool s = false;
            for (int k = 0; k < 7; k++) if (!strcmp(names[i], shown[k])) s = true;
            if (!s && !is_initial(names[i], vals[i])) return -1;
        }
        const char *caps = val_of(vals, names, n, "font-variant-caps");
        const char *stretch = val_of(vals, names, n, "font-stretch");
        if (strcmp(caps, "normal") && strcmp(caps, "small-caps")) return -1;
        if (strchr(stretch, '%')) {
            /* a computed stretch: only the keywords' percentages fit */
            static const struct { double pct; const char *kw; } sk[] = {
                { 50, "ultra-condensed" }, { 62.5, "extra-condensed" }, { 75, "condensed" },
                { 87.5, "semi-condensed" }, { 100, "normal" }, { 112.5, "semi-expanded" },
                { 125, "expanded" }, { 150, "extra-expanded" }, { 200, "ultra-expanded" },
            };
            double pv = strtod(stretch, NULL);
            const char *kw = NULL;
            for (size_t k = 0; k < sizeof sk / sizeof sk[0]; k++) if (sk[k].pct == pv) kw = sk[k].kw;
            if (!kw) return -1;
            stretch = kw;
        }
        bool sp = false;
        #define PUT(x) do { if (sp) buf_ch(b, ' '); buf_str(b, x); sp = true; } while (0)
        const char *st = val_of(vals, names, n, "font-style"), *we = val_of(vals, names, n, "font-weight");
        if (strcmp(st, "normal")) PUT(st);
        if (strcmp(caps, "normal")) PUT(caps);
        if (strcmp(we, "normal") && strcmp(we, "400")) PUT(we);     /* (400: computed normal) */
        if (strcmp(stretch, "normal")) PUT(stretch);
        PUT(val_of(vals, names, n, "font-size"));
        const char *lh = val_of(vals, names, n, "line-height");
        if (strcmp(lh, "normal")) { buf_str(b, " / "); buf_str(b, lh); }
        PUT(val_of(vals, names, n, "font-family"));
        #undef PUT
        return 0;
    }
    if (!strcmp(nm, "font-variant")) {
        bool all_normal = true, lig_none = false;
        for (int i = 0; i < n; i++) {
            if (strcmp(vals[i], "normal")) {
                if (!strcmp(names[i], "font-variant-ligatures") && !strcmp(vals[i], "none")) lig_none = true;
                else all_normal = false;
            }
        }
        if (all_normal) { buf_str(b, lig_none ? "none" : "normal"); return 0; }
        if (lig_none) return -1;
        bool sp = false;
        for (int i = 0; i < n; i++) if (strcmp(vals[i], "normal")) { if (sp) buf_ch(b, ' '); buf_str(b, vals[i]); sp = true; }
        return 0;
    }
    if (!strcmp(nm, "font-synthesis")) {
        static const char *const kw[] = { "weight", "style", "small-caps", "position" };
        bool sp = false;
        for (int k = 0; k < 4; k++) {
            char name[64];
            snprintf(name, sizeof name, "font-synthesis-%s", kw[k]);
            const char *v = val_of(vals, names, n, name);
            if (v && !strcmp(v, "auto")) { if (sp) buf_ch(b, ' '); buf_str(b, kw[k]); sp = true; }
        }
        if (!sp) buf_str(b, "none");
        return 0;
    }
    if (!strcmp(nm, "marker")) {
        for (int i = 1; i < n; i++) if (strcmp(vals[i], vals[0])) return -1;
        buf_str(b, vals[0]);
        return 0;
    }
    if (!strcmp(nm, "transition")) {
        static const char *const props[] = { "transition-property", "transition-duration", "transition-timing-function",
                                             "transition-delay", "transition-behavior" };
        return serialize_layers(vals, names, n, props, 5, b, true);
    }
    if (!strcmp(nm, "animation")) {
        static const char *const props[] = { "animation-duration", "animation-timing-function", "animation-delay",
            "animation-iteration-count", "animation-direction", "animation-fill-mode", "animation-play-state",
            "animation-name" };
        return serialize_layers(vals, names, n, props, 8, b, false);
    }
    return -1;
}
