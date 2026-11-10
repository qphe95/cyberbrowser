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
    int ti;                     /* G_TYPE: its index in g_types + 1 (-1: none; 0: not looked for) */
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
           ieq(v, n, "revert-layer") || ieq(v, n, "revert-rule");
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

static bool tok_ident(const Tok *t, const char *name);

/* (the property being read takes its percentages as numbers: they compare as they are) */
static __thread bool t_pct_numbers;
static bool pct_numbers_of(const char *n) {
    return n && (strstr(n, "opacity") || !strcmp(n, "scale") || !strcmp(n, "shape-image-threshold") || !strcmp(n, "font-stretch") ||
                 !strcmp(n, "font-width") || !strcmp(n, "zoom") || !strcmp(n, "font-size-adjust") || !strncmp(n, "--", 2) ||
                 strstr(n, "timing-function") || strstr(n, "color") || !strcmp(n, "font-palette") || !strcmp(n, "text-size-adjust"));
}

static bool is_math_fn(const char *n) {
    static const char *const fns[] = { "calc", "min", "max", "clamp", "round", "mod", "rem", "abs", "sign",
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp",
        "-webkit-calc", "calc-size", "progress", "random", "sibling-index", "sibling-count", NULL };
    for (int i = 0; fns[i]; i++) if (strcasecmp(n, fns[i]) == 0) return true;
    return false;
}

static bool anchor_fn_valid(const Toks *ts, int i, bool size_fn);

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
        if (t->str && !strcasecmp(t->str, "anchor")) return anchor_fn_valid(ts, i, false) ? D_LENGTH : -1;
        if (t->str && !strcasecmp(t->str, "anchor-size")) return anchor_fn_valid(ts, i, true) ? D_LENGTH : -1;
        if (!is_math_fn(t->str)) return -1;
        const char *f = t->str;
        if (strcasecmp(f, "calc") == 0 || strcasecmp(f, "-webkit-calc") == 0) return calc_sum(ts, t->first, t->end, pct);
        /* (what comes of the element's place: no arguments) */
        if (!strcasecmp(f, "sibling-index") || !strcasecmp(f, "sibling-count")) return t->first == t->end ? D_NUMBER : -1;
        /* comma-separated arguments */
        int args[4], na = 0, s = t->first;
        for (int j = t->first; j <= t->end; j = j < t->end ? tok_next(ts, j) : j + 1) {
            if (j == t->end || ts->t[j].type == T_COMMA) {
                if (na == 4) return -1;
                if (j == s) {
                    if (strcasecmp(f, "round") == 0 && na == 0) { args[na++] = 0; s = j + 1; continue; }
                    return -1;
                }
                /* (clamp(): no lower or upper bound) */
                if (strcasecmp(f, "clamp") == 0 && (na == 0 || na == 2) && ts->t[s].type == T_IDENT && tok_next(ts, s) == j &&
                    strcasecmp(ts->t[s].str, "none") == 0) {
                    args[na++] = 0;
                    s = j + 1;
                    continue;
                }
                /* random()'s caching options come first */
                if (strcasecmp(f, "random") == 0 && na == 0 && s == t->first && ts->t[s].type == T_IDENT && !tok_ident(&ts->t[s], "e") &&
                    !tok_ident(&ts->t[s], "pi") && !tok_ident(&ts->t[s], "infinity") && !tok_ident(&ts->t[s], "-infinity") && !tok_ident(&ts->t[s], "nan")) {
                    bool all_ok = true, fixed = false, elem = false, prop = false, dashed = false;
                    for (int q = s; q < j; q = tok_next(ts, q)) {
                        const Tok *o = &ts->t[q];
                        if (fixed) { if (o->type != T_NUMBER) all_ok = false; fixed = false; if (q != tok_next(ts, s) || tok_next(ts, q) != j) all_ok = false; continue; }
                        if (o->type != T_IDENT) { all_ok = false; break; }
                        if (tok_ident(o, "auto")) { if (q != s || tok_next(ts, s) != j) all_ok = false; }
                        else if (tok_ident(o, "fixed")) { if (q != s) all_ok = false; fixed = true; }
                        else if (tok_ident(o, "element-scoped")) { if (elem) all_ok = false; elem = true; }
                        else if (tok_ident(o, "property-scoped") || tok_ident(o, "property-index-scoped")) { if (prop) all_ok = false; prop = true; }
                        else if (o->str && !strncmp(o->str, "--", 2)) { if (dashed) all_ok = false; dashed = true; }
                        else if (o->str && !strncmp(o->str, "ua-", 3)) { if (prop) all_ok = false; prop = true; }
                        else all_ok = false;
                    }
                    if (!all_ok || fixed) return -1;
                    s = j + 1;
                    continue;
                }
                /* progress()'s clamping comes first, with no comma */
                if (strcasecmp(f, "progress") == 0 && na == 0 && ts->t[s].type == T_IDENT &&
                    (tok_ident(&ts->t[s], "no-clamp") || tok_ident(&ts->t[s], "clamp"))) s = tok_next(ts, s);
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
        if (strcasecmp(f, "progress") == 0) {
            if (na != 3 || !args[0] || (args[0] & ~args[1]) || (args[1] & ~args[2]) || (args[2] & ~args[0])) return -1;
            return D_NUMBER;
        }
        /* (one argument each; a percentage only where there's something it's of) */
        if ((!strcasecmp(f, "sign") || !strcasecmp(f, "abs")) && na != 1) return -1;
        /* (round: a value and the step, which only a number may leave out) */
        if (!strcasecmp(f, "round") && (na > 2 || (na == 1 && args[0] != D_NUMBER))) return -1;
        if (strcasecmp(f, "sign") == 0) return (args[0] & D_PERCENT) && !pct ? -1 : D_NUMBER;
        if (strcasecmp(f, "random") == 0 && (na < 2 || na > 3)) return -1;
        if (!strcasecmp(f, "sin") || !strcasecmp(f, "cos") || !strcasecmp(f, "tan")) return (args[0] & (D_NUMBER | D_ANGLE)) ? D_NUMBER : -1;
        /* (one number for these three; two of one type for atan2) */
        if (!strcasecmp(f, "asin") || !strcasecmp(f, "acos") || !strcasecmp(f, "atan")) return na == 1 && args[0] == D_NUMBER ? D_ANGLE : -1;
        if (!strcasecmp(f, "atan2")) {
            if (na != 2) return -1;
            int x = args[0] & ~D_PERCENT, y = args[1] & ~D_PERCENT;
            if (x && y && x != y) return -1;
            return D_ANGLE;
        }
        /* (numbers only, and as many as each takes) */
        if (!strcasecmp(f, "pow") || !strcasecmp(f, "sqrt") || !strcasecmp(f, "log") || !strcasecmp(f, "exp")) {
            int want_lo = !strcasecmp(f, "pow") ? 2 : 1, want_hi = !strcasecmp(f, "pow") || !strcasecmp(f, "log") ? 2 : 1;
            if (na < want_lo || na > want_hi) return -1;
            for (int a = 0; a < na; a++) if (args[a] != D_NUMBER) return -1;
            return D_NUMBER;
        }
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
        else if (divide) {
            /* (of the same kind: a number) */
            if (v != D_NUMBER) {
                if (((d | v) & D_PERCENT) && !pct) return -1;
                if ((d & ~D_PERCENT) != (v & ~D_PERCENT) && !((d & D_PERCENT) && (v & D_PERCENT))) return -1;
                if (d == D_NUMBER) return -1;
                d = D_NUMBER;
            }
        }
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

static const char *const g_css_wide[] = { "initial", "inherit", "unset", "revert", "revert-layer", "revert-rule", "default", NULL };

static bool custom_ident_ok(const Tok *t) {
    if (t->type != T_IDENT) return false;
    for (int i = 0; g_css_wide[i]; i++) if (strcasecmp(t->str, g_css_wide[i]) == 0) return false;
    return true;
}

/* ---- anchor() and anchor-size() (CSS Anchor Positioning) ---- */
static bool anchor_fn_valid(const Toks *ts, int i, bool size_fn);

static bool anchor_fallback_valid(const Toks *ts, int i, bool size_fn)
{
    const Tok *t = &ts->t[i];
    if (t->type == T_PERCENT) return true;
    if (t->type == T_NUMBER) return t->num == 0.0;
    if (t->type == T_DIM) return unit_dim(t->str, NULL) == D_LENGTH;
    if (t->type != T_FUNC) return false;
    if (strcasecmp(t->str, size_fn ? "anchor-size" : "anchor") == 0) return anchor_fn_valid(ts, i, size_fn);
    if (!strcasecmp(t->str, "anchor") || !strcasecmp(t->str, "anchor-size")) return false;
    return is_math_fn(t->str) && calc_value(ts, i, D_LENGTH) == D_LENGTH;
}

static bool anchor_fn_valid(const Toks *ts, int i, bool size_fn)
{
    static const char *const sides[] = { "inside", "outside", "top", "left", "right", "bottom", "start", "end", "self-start", "self-end", "center", NULL };
    static const char *const dims[] = { "width", "height", "block", "inline", "self-block", "self-inline", NULL };
    const Tok *t = &ts->t[i];
    if (t->type != T_FUNC || strcasecmp(t->str, size_fn ? "anchor-size" : "anchor")) return false;
    bool name = false, what = false;
    int j = t->first;
    for (; j < t->end && ts->t[j].type != T_COMMA; j = tok_next(ts, j)) {
        const Tok *a = &ts->t[j];
        if (a->type == T_IDENT && a->str[0] == '-' && a->str[1] == '-') {
            if (name) return false;
            name = true;
            continue;
        }
        if (what) return false;
        what = true;
        if (a->type == T_IDENT) {
            bool known = false;
            for (const char *const *k = size_fn ? dims : sides; *k && !known; k++) known = !strcasecmp(a->str, *k);
            if (!known) return false;
        } else if (size_fn) {
            return false;
        } else if (a->type != T_PERCENT && !(a->type == T_FUNC && is_math_fn(a->str) && calc_value(ts, j, D_PERCENT) == D_PERCENT)) {
            return false;
        }
    }
    if (!size_fn && !what) return false;
    if (j < t->end) {
        /* ", " and one value */
        if (j == t->first) return false;
        int f = tok_next(ts, j);
        if (f >= t->end || tok_next(ts, f) < t->end) return false;
        return anchor_fallback_valid(ts, f, size_fn);
    }
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
            if (!(j < t->end && ts->t[j].type == T_STRING)) return 0;
            /* (then request modifiers, each once, each of one right argument) */
            int seen = 0;
            for (int k = tok_next(ts, j); k < t->end; k = tok_next(ts, k)) {
                const Tok *m = &ts->t[k];
                if (m->type != T_FUNC) return 0;
                int a = m->first, which;
                if (!strcasecmp(m->str, "cross-origin")) which = 1;
                else if (!strcasecmp(m->str, "integrity")) which = 2;
                else if (!strcasecmp(m->str, "referrer-policy")) which = 4;
                else return 0;
                if (seen & which) return 0;
                seen |= which;
                if (a >= m->end || tok_next(ts, a) != m->end) return 0;
                const Tok *x = &ts->t[a];
                if (which == 2) { if (x->type != T_STRING) return 0; continue; }
                if (x->type != T_IDENT) return 0;
                if (which == 1 && strcasecmp(x->str, "anonymous") && strcasecmp(x->str, "use-credentials")) return 0;
                if (which == 4) {
                    static const char *const rp[] = { "no-referrer", "no-referrer-when-downgrade", "same-origin", "origin",
                        "strict-origin", "origin-when-cross-origin", "strict-origin-when-cross-origin", "unsafe-url" };
                    bool ok = false;
                    for (size_t q = 0; q < sizeof rp / sizeof rp[0] && !ok; q++) ok = !strcasecmp(x->str, rp[q]);
                    if (!ok) return 0;
                }
            }
            return 1;
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
        return t->type == T_FUNC && ((t->first < t->end && tok_ident(&ts->t[t->first], "from")) ||
                                     !strcasecmp(t->str, "color-layers") || !strcasecmp(t->str, "contrast-color")) &&
               color_tok_valid(ts, (int)(t - ts->t));
    if (!strcmp(n, "any-value")) return t->type != T_BAD && t->type != T_SEMI;
    if (!strcmp(n, "anchor-fn")) return anchor_fn_valid(ts, i, false);
    if (!strcmp(n, "anchor-size-fn")) return anchor_fn_valid(ts, i, true);
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
    if (pos == r->last && r->g->required) return false;   /* (!: not nothing) */
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
        if (!g->ti) { int found = type_index(g->text); ((GNode *)g)->ti = found >= 0 ? found + 1 : -1; }
        int ti = g->ti - 1;
        if (ti >= 0) {
            CloseUd c = { hit, k, ud };
            ok = match_node(m, type_grammar(ti), pos, end, close_cont, &c);
            break;
        }
        /* (a builtin type, as often as not, that didn't match here) */
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

static bool math_ser_top(const Toks *ts, int i, Buf *b);
static void ser_tok(Ser *S, int i, Buf *b, const int *kids, int nk) {
    const Tok *t = &S->ts->t[i];
    switch (t->type) {
    case T_IDENT: ser_ident(b, t->str); break;
    case T_FUNC: {
        if (is_math_fn(t->str) && calc_serialize(S->ts, i, b)) break;
        if (!g_env && is_math_fn(t->str) && math_ser_top(S->ts, i, b)) break;
        if (!strcasecmp(t->str, "anchor") || !strcasecmp(t->str, "anchor-size")) {
            const Toks *ts = S->ts;
            buf_str(b, !strcasecmp(t->str, "anchor") ? "anchor(" : "anchor-size(");
            int name = -1, what = -1, j = t->first;
            for (; j < t->end && ts->t[j].type != T_COMMA; j = tok_next(ts, j)) {
                if (ts->t[j].type == T_IDENT && ts->t[j].str[0] == '-' && ts->t[j].str[1] == '-') name = j;
                else what = j;
            }
            if (name >= 0) ser_tok(S, name, b, kids, nk);
            if (what >= 0) {
                if (name >= 0) buf_ch(b, ' ');
                ser_tok(S, what, b, kids, nk);
            }
            if (j < t->end) {
                int f = tok_next(ts, j);
                if (name >= 0 || what >= 0) buf_str(b, ", ");
                if (ts->t[f].type == T_NUMBER && ts->t[f].num == 0.0) buf_str(b, "0px");
                else ser_tok(S, f, b, kids, nk);
            }
            buf_ch(b, ')');
            break;
        }
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
        { "lvi", 2, 1 }, { "dvi", 2, 1 }, { "cqw", 6, 1 }, { "cqi", 6, 1 },
        { "vh", 3, 1 }, { "svh", 3, 1 }, { "lvh", 3, 1 }, { "dvh", 3, 1 }, { "vb", 3, 1 }, { "svb", 3, 1 },
        { "lvb", 3, 1 }, { "dvb", 3, 1 }, { "cqh", 7, 1 }, { "cqb", 7, 1 },
        { "vmin", 4, 1 }, { "svmin", 4, 1 }, { "lvmin", 4, 1 }, { "dvmin", 4, 1 }, { "cqmin", 8, 1 },
        { "vmax", 5, 1 }, { "svmax", 5, 1 }, { "lvmax", 5, 1 }, { "dvmax", 5, 1 }, { "cqmax", 9, 1 },
    };
    /* (container units: the query container's, else the viewport's) */
    double cw = e->cq_w > 0 ? e->cq_w / 100 : vw, ch = e->cq_h > 0 ? e->cq_h / 100 : vh;
    for (size_t i = 0; i < sizeof rel / sizeof rel[0]; i++) {
        if (strcasecmp(u, rel[i].u)) continue;
        int k = rel[i].k;
        /* (inline and block: the viewport's axes as the box's writing mode turns them) */
        size_t ul = strlen(u);
        if (e->vertical && (k == 2 || k == 3) && ul && (tolower((unsigned char)u[ul - 1]) == 'i' || tolower((unsigned char)u[ul - 1]) == 'b')) k = 5 - k;
        double base = k == 0 ? em : k == 1 ? e->root_font_size : k == 2 ? vw : k == 3 ? vh : k == 4 ? (vw < vh ? vw : vh) :
                      k == 5 ? (vw > vh ? vw : vh) : k == 6 ? cw : k == 7 ? ch : k == 8 ? (cw < ch ? cw : ch) : (cw > ch ? cw : ch);
        if (e->ch_ratio > 0 && !strcasecmp(u, "ch")) return em * e->ch_ratio;
        if (e->ex_ratio > 0 && !strcasecmp(u, "ex")) return em * e->ex_ratio;
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
    /* (random(fixed N, ...) is known now; another random() and the tree's aren't) */
    if (!strcasecmp(name, "sibling-index") || !strcasecmp(name, "sibling-count")) return false;
    if (!strcasecmp(name, "random")) {
        if (f->first >= f->end || !tok_ident(&ts->t[f->first], "fixed")) return false;
        int q = tok_next(ts, f->first);
        if (q >= f->end || ts->t[q].type != T_NUMBER) return false;
        double x = ts->t[q].num;
        int s0 = tok_next(ts, q);
        if (s0 >= f->end || ts->t[s0].type != T_COMMA) return false;
        CalcSum a[3];
        int nr = 0, s1 = s0 + 1;
        for (int j = s1; ; j = j < f->end ? tok_next(ts, j) : j + 1) {
            if (j >= f->end || ts->t[j].type == T_COMMA) {
                if (nr == 3 || !calc_simplify_sum(ts, s1, j, &a[nr++])) return false;
                s1 = j + 1;
                if (j >= f->end) break;
            }
        }
        double lo, hi;
        const char *ul, *uh;
        if (nr < 2 || !calc_one(&a[0], &lo, &ul) || !calc_one(&a[1], &hi, &uh) || strcmp(ul, uh)) return false;
        double st = 0;
        if (nr == 3) {
            const char *us;
            if (!calc_one(&a[2], &st, &us) || strcmp(us, ul)) return false;
        }
        /* (NaN anywhere is NaN; an infinite min is the value, an infinite max NaN) */
        double val;
        if (isnan(lo) || isnan(hi) || isnan(st)) val = NAN;
        else if (hi <= lo) val = lo;
        else if (isinf(lo) || isinf(hi)) val = NAN;
        else if (st > 0.0 && isinf(st)) val = lo;
        else if (st > 0.0) { val = lo + floor(x * (hi - lo + st) / st) * st; if (val > hi) val -= st; if (val < lo) val = lo; }
        else val = lo + x * (hi - lo);
        memset(out, 0, sizeof *out);
        return calc_add(out, val, ul);
    }
    /* the comma-separated arguments; round()'s first may be a strategy */
    CalcSum args[20];   /* (at least 20, css-values 4 10.1) */
    int na = 0, strategy = 0;   /* 0 nearest, 1 up, 2 down, 3 to-zero */
    int clamp_none = 0;
    int s = f->first;
    for (int j = f->first; ; j = j < f->end ? tok_next(ts, j) : j + 1) {
        if (j >= f->end || ts->t[j].type == T_COMMA) {
            if (j == s || na == 20) return false;
            if (!strcasecmp(name, "round") && na == 0 && tok_next(ts, s) == j && ts->t[s].type == T_IDENT &&
                (tok_ident(&ts->t[s], "nearest") || tok_ident(&ts->t[s], "up") || tok_ident(&ts->t[s], "down") ||
                 tok_ident(&ts->t[s], "to-zero"))) {
                strategy = tok_ident(&ts->t[s], "up") ? 1 : tok_ident(&ts->t[s], "down") ? 2 :
                           tok_ident(&ts->t[s], "to-zero") ? 3 : 0;
            } else if (!strcasecmp(name, "clamp") && (na == 0 || na == 2) && tok_next(ts, s) == j && ts->t[s].type == T_IDENT && tok_ident(&ts->t[s], "none")) {
                /* (the other bound's unit: filled in below) */
                clamp_none |= na == 0 ? 1 : 2;
                memset(&args[na], 0, sizeof args[na]);
                args[na].n = 1;
                args[na].t[0].v = na == 0 ? -INFINITY : INFINITY;
                na++;
            } else if (!strcasecmp(name, "progress") && na == 0 && ts->t[s].type == T_IDENT && (tok_ident(&ts->t[s], "no-clamp") || tok_ident(&ts->t[s], "clamp"))) {
                if (!calc_simplify_sum(ts, tok_next(ts, s), j, &args[na++])) return false;
            } else if (!calc_simplify_sum(ts, s, j, &args[na++])) return false;
            s = j + 1;
            if (j >= f->end) break;
        }
    }
    if (!na) return false;
    double v[20];
    const char *u[20];
    for (int i = 0; i < na; i++) if (!calc_one(&args[i], &v[i], &u[i])) return false;
    if (clamp_none && na == 3) {
        if (clamp_none & 1) u[0] = u[1];
        if (clamp_none & 2) u[2] = u[1];
    }
    /* functions over values of one type take arguments of the same unit */
    #define SAME_UNITS() do { for (int i = 1; i < na; i++) if (strcmp(u[i], u[0])) return false; } while (0)
    memset(out, 0, sizeof *out);
    double r;
    const char *unit = u[0];
    if (!strcasecmp(name, "min") || !strcasecmp(name, "max")) {
        SAME_UNITS();
        /* (percentages may come to less than nothing, the larger the less: kept until they're known) */
        if (na > 1 && !strcmp(unit, "%") && !t_pct_numbers) {
            bool finite = true;
            for (int i = 0; i < na; i++) if (!isfinite(v[i])) finite = false;
            if (finite) return false;
        }
        r = v[0];
        for (int i = 1; i < na; i++) {
            if (isnan(v[i])) { r = NAN; break; }
            r = !strcasecmp(name, "min") ? (v[i] < r ? v[i] : r) : (v[i] > r ? v[i] : r);
            /* (between zeros: min takes -0, max +0) */
            if (v[i] == 0.0 && r == 0.0) r = !strcasecmp(name, "min") ? (signbit(v[i]) || signbit(r) ? -0.0 : 0.0)
                                                                       : (!signbit(v[i]) || !signbit(r) ? 0.0 : -0.0);
        }
    } else if (!strcasecmp(name, "clamp")) {
        if (na != 3) return false;
        SAME_UNITS();
        if (!strcmp(unit, "%") && !clamp_none && !t_pct_numbers) return false;
        r = v[1] < v[0] ? v[0] : v[1];
        if (r == 0.0 && v[0] == 0.0 && v[1] == 0.0) r = signbit(v[0]) && signbit(v[1]) ? -0.0 : 0.0;
        if (r > v[2]) r = v[2];
        else if (r == 0.0 && v[2] == 0.0) r = signbit(r) || signbit(v[2]) ? -0.0 : 0.0;
        if (v[0] > v[2]) r = v[0];
        if (isnan(v[0]) || isnan(v[1]) || isnan(v[2])) r = NAN;
    } else if (!strcasecmp(name, "progress")) {
        if (na != 3) return false;
        SAME_UNITS();
        r = (v[0] - v[1]) / (v[2] - v[1]);
        if (!(f->first < f->end && tok_ident(&ts->t[f->first], "no-clamp"))) r = isnan(r) ? r : r < 0 ? 0 : r > 1 ? 1 : r;
        unit = "";
    } else if (!strcasecmp(name, "abs")) {
        if (na != 1) return false;
        r = fabs(v[0]);
    } else if (!strcasecmp(name, "random")) {
        return false;
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
            else if (isinf(b)) r = isinf(a) ? NAN : (strategy == 1 ? (a > 0 ? INFINITY : copysign(0.0, a)) :
                                                   strategy == 2 ? (a < 0 ? -INFINITY : copysign(0.0, a)) : copysign(0.0, a));
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
            else {
                /* (a zero takes the sign of the divisor for mod, of the dividend for rem) */
                r = fmod(a, b);
                if (!strcasecmp(name, "mod")) { if (r != 0.0 && (r < 0.0) != (b < 0.0)) r += b; if (r == 0.0) r = copysign(0.0, b); }
                else if (r == 0.0) r = copysign(0.0, a);
            }
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
        /* (NaN in, NaN out; one to an infinite power has no answer) */
        if (isnan(v[0]) || isnan(v[1]) || (fabs(v[0]) == 1.0 && isinf(v[1]))) r = NAN;
    } else if (!strcasecmp(name, "sqrt")) {
        if (na != 1 || u[0][0]) return false;
        r = sqrt(v[0]);
    } else if (!strcasecmp(name, "hypot")) {
        SAME_UNITS();
        r = 0;
        for (int i = 0; i < na; i++) r += v[i] * v[i];
        r = sqrt(r);
        for (int i = 0; i < na; i++) if (isinf(v[i])) r = INFINITY;
        for (int i = 0; i < na; i++) if (isnan(v[i])) r = NAN;
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
                /* (of one unit: a plain number) */
                if (!vnum && v.n == 1 && out->n == 1 && !strcmp(v.t[0].unit, out->t[0].unit)) {
                    out->t[0].v /= v.t[0].v;
                    out->t[0].unit[0] = 0;
                    first = false;
                    i = tok_next(ts, i);
                    continue;
                }
                if (!vnum) return false;     /* (by zero: an infinity, by its sign) */
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
    /* (computed, what has no end comes to the largest there is, or with another, to nothing) */
    if (g_env) {
        bool up = false, down = false, nan = false;
        for (int k = 0; k < s.n; k++) {
            if (isnan(s.t[k].v)) nan = true;
            else if (isinf(s.t[k].v)) { if (s.t[k].v > 0) up = true; else down = true; }
        }
        if (nan || (up && down)) { buf_str(b, "0px"); return true; }
        if (up || down) {
            fmt_number(b, up ? 3.4028234663852886e38 : -3.4028234663852886e38);
            for (int k = 0; k < s.n; k++) if (!isfinite(s.t[k].v)) { buf_str(b, s.t[k].unit); break; }
            return true;
        }
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

/* ---- math that can't be worked out yet, in its canonical form: sums sorted and
 * flat, the numeric factor first, what a function comes to without a calc() round it ---- */
typedef struct { char *text; int kind; double num; bool neg, product; char unit[12]; } MathTerm;   /* (kind: 0 number, 1 percent, 2 dimension, 3 other) */
typedef struct { MathTerm t[32]; int n; bool bad; } MathSum;

static void math_sum_tokens(const Toks *ts, int b, int e, MathSum *out, bool neg);
static char *math_sum_text(MathSum *s, bool wrap);

static void math_sum_free(MathSum *s) { for (int i = 0; i < s->n; i++) free(s->t[i].text); s->n = 0; }

/* A plain value's text, without its sign. */
static char *math_num_text(double v, const char *unit)
{
    Buf b = { 0 };
    v = fabs(v);
    if (isnan(v) || isinf(v)) {
        buf_str(&b, isnan(v) ? "NaN" : "infinity");
        if (unit && unit[0]) { buf_str(&b, " * 1"); buf_str(&b, unit); }
    } else {
        fmt_number(&b, v);
        if (unit) buf_str(&b, unit);
    }
    return buf_take(&b);
}

static bool math_is_plain_fn(const char *f)
{
    static const char *const fns[] = { "calc", "-webkit-calc", "min", "max", "clamp", "round", "mod", "rem", "abs", "sign",
        "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp", NULL };
    for (int i = 0; fns[i]; i++) if (!strcasecmp(f, fns[i])) return true;
    return false;
}

/* The text of a function's call, with its arguments in canonical form. */
static char *math_fn_text(const Toks *ts, int i)
{
    const Tok *t = &ts->t[i];
    char name[24];
    snprintf(name, sizeof name, "%s", t->str);
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    /* (the arguments) */
    int starts[8], ends[8], na = 0, s = t->first;
    for (int j = t->first; j <= t->end && na < 8; j = j < t->end ? tok_next(ts, j) : j + 1) {
        if (j == t->end || ts->t[j].type == T_COMMA) {
            starts[na] = s;
            ends[na++] = j;
            s = j + 1;
            if (j == t->end) break;
        }
    }
    /* (one argument, or a clamp with a bound of none: what it comes to) */
    bool drop_first = false, drop_last = false;
    if (!strcmp(name, "clamp") && na == 3) {
        drop_first = ends[0] == starts[0] + 1 && tok_ident(&ts->t[starts[0]], "none");
        drop_last = ends[2] == starts[2] + 1 && tok_ident(&ts->t[starts[2]], "none");
        if (drop_first && drop_last) { starts[0] = starts[1]; ends[0] = ends[1]; na = 1; snprintf(name, sizeof name, "calc"); }
        else if (drop_last) { na = 2; snprintf(name, sizeof name, "max"); }
        else if (drop_first) { starts[0] = starts[1]; ends[0] = ends[1]; starts[1] = starts[2]; ends[1] = ends[2]; na = 2; snprintf(name, sizeof name, "min"); }
    }
    if (na == 1 && (!strcmp(name, "min") || !strcmp(name, "max") || !strcmp(name, "calc") || !strcmp(name, "-webkit-calc"))) {
        MathSum m = { .n = 0 };
        math_sum_tokens(ts, starts[0], ends[0], &m, false);
        if (m.bad) { math_sum_free(&m); return NULL; }
        if (m.n == 1 && m.t[0].kind == 3 && !m.t[0].neg && !m.t[0].product &&
            (!strncmp(m.t[0].text, "min(", 4) || !strncmp(m.t[0].text, "max(", 4) || !strncmp(m.t[0].text, "clamp(", 6) || !strncmp(m.t[0].text, "calc(", 5))) {
            char *one = m.t[0].text;
            m.t[0].text = NULL;
            math_sum_free(&m);
            return one;
        }
        char *in = math_sum_text(&m, false);
        math_sum_free(&m);
        Buf b = { 0 };
        buf_str(&b, "calc(");
        buf_str(&b, in);
        buf_ch(&b, ')');
        free(in);
        return buf_take(&b);
    }
    Buf b = { 0 };
    buf_str(&b, name);
    buf_ch(&b, '(');
    for (int a = 0; a < na; a++) {
        if (a) buf_str(&b, ", ");
        MathSum m = { .n = 0 };
        math_sum_tokens(ts, starts[a], ends[a], &m, false);
        if (m.bad) { math_sum_free(&m); free(b.s); return NULL; }
        char *in = math_sum_text(&m, false);
        math_sum_free(&m);
        buf_str(&b, in);
        free(in);
    }
    buf_ch(&b, ')');
    return buf_take(&b);
}

static void math_sum_tokens(const Toks *ts, int b, int e, MathSum *out, bool neg)
{
    bool minus = neg;
    int s = b;
    for (int i = b; i <= e; i = i < e ? tok_next(ts, i) : i + 1) {
        bool op = i < e && i > s && ts->t[i].type == T_DELIM && (ts->t[i].delim == '+' || ts->t[i].delim == '-');
        if (i != e && !op) continue;
        /* (divided by more than a number: in the order it's written) */
        bool verbatim = false;
        for (int j = s; j < i; j = tok_next(ts, j)) {
            if (ts->t[j].type != T_DELIM || ts->t[j].delim != '/') continue;
            int d = tok_next(ts, j);
            if (d < i && ts->t[d].type != T_NUMBER) verbatim = true;
        }
        if (verbatim) {
            Buf vb = { 0 };
            for (int j = s; j < i; j = tok_next(ts, j)) {
                const Tok *x = &ts->t[j];
                if (x->type == T_DELIM) { buf_ch(&vb, ' '); buf_ch(&vb, x->delim); buf_ch(&vb, ' '); continue; }
                if (x->type == T_NUMBER) fmt_number(&vb, x->num);
                else if (x->type == T_PERCENT || x->type == T_DIM) {
                    fmt_number(&vb, x->num);
                    if (x->type == T_PERCENT) buf_ch(&vb, '%'); else buf_str(&vb, x->str);
                } else if (x->type == T_IDENT) buf_str(&vb, x->str);
                else if (x->type == T_FUNC && math_is_plain_fn(x->str)) {
                    char *tx = math_fn_text(ts, j);
                    if (!tx) { out->bad = true; break; }
                    buf_str(&vb, tx);
                    free(tx);
                } else if (x->type == T_PAREN) {
                    MathSum in = { .n = 0 };
                    math_sum_tokens(ts, x->first, x->end, &in, false);
                    char *tx = in.bad ? NULL : math_sum_text(&in, true);
                    math_sum_free(&in);
                    if (!tx) { out->bad = true; break; }
                    buf_str(&vb, tx);
                    free(tx);
                } else {
                    Ser plain = { ts, NULL, 0, NULL, NULL };
                    ser_tok(&plain, j, &vb, NULL, 0);
                }
            }
            if (!out->bad && out->n < 32) {
                MathTerm *m = &out->t[out->n++];
                memset(m, 0, sizeof *m);
                m->kind = 3;
                m->product = true;
                m->neg = minus;
                m->text = buf_take(&vb);
            } else free(vb.s);
            if (out->bad || i == e) break;
            minus = neg != (ts->t[i].delim == '-');
            s = i + 1;
            continue;
        }
        /* (the product from s to i) */
        double k = 1.0, kdiv = 1.0;
        bool f_is_sum = false;
        bool any_num = false, divided = false, after_nums = false;
        const char *unit = NULL;
        char ucanon[12];
        int nf = 0, only = -1;
        Buf f = { 0 }, after = { 0 };
        bool wanted_paren_sum = false;
        for (int j = s; j < i; j = tok_next(ts, j)) {
            const Tok *x = &ts->t[j];
            if (x->type == T_DELIM && x->delim == '*') continue;
            if (x->type == T_DELIM && x->delim == '/') {
                int d = tok_next(ts, j);
                if (d >= i) { out->bad = true; break; }
                const Tok *dv = &ts->t[d];
                if (dv->type == T_NUMBER) { kdiv *= dv->num; after_nums = true; j = d; continue; }
                buf_str(&after, " / ");
                {
                    char *tx = NULL;
                    if (dv->type == T_FUNC && math_is_plain_fn(dv->str)) tx = math_fn_text(ts, d);
                    else if (dv->type == T_DIM || dv->type == T_PERCENT) {
                        Buf q = { 0 };
                        fmt_number(&q, dv->num);
                        if (dv->type == T_PERCENT) buf_ch(&q, '%'); else buf_str(&q, dv->str);
                        tx = buf_take(&q);
                    } else if (dv->type == T_IDENT) tx = strdup(dv->str);
                    if (!tx) { out->bad = true; break; }
                    buf_str(&after, tx);
                    free(tx);
                }
                divided = true;
                j = d;
                continue;
            }
            if (x->type == T_NUMBER) { k *= x->num; any_num = true; continue; }
            if (x->type == T_IDENT) {
                double cv = 0.0;
                bool isc = true;
                if (!strcasecmp(x->str, "e")) cv = M_E;
                else if (!strcasecmp(x->str, "pi")) cv = M_PI;
                else if (!strcasecmp(x->str, "infinity")) cv = INFINITY;
                else if (!strcasecmp(x->str, "-infinity")) cv = -INFINITY;
                else if (!strcasecmp(x->str, "nan")) cv = NAN;
                else isc = false;
                if (isc) { k *= cv; any_num = true; continue; }
            }
            if ((x->type == T_PERCENT || x->type == T_DIM) && !unit) {
                k *= x->num;
                unit = x->type == T_PERCENT ? "%" : x->str;
                /* (in its canonical unit, as one that's worked out would be) */
                double sc = 1.0;
                if (x->type == T_DIM && (unit_dim(x->str, NULL) == D_ANGLE || unit_dim(x->str, NULL) == D_TIME) && calc_unit_canon(x->str, &sc, ucanon, sizeof ucanon)) { k *= sc; unit = ucanon; }
                continue;
            }
            if (nf) buf_str(&f, " * ");
            nf++;
            if (x->type == T_PERCENT || x->type == T_DIM) {
                fmt_number(&f, x->num);
                if (x->type == T_PERCENT) buf_ch(&f, '%'); else buf_str(&f, x->str);
            } else if (x->type == T_IDENT) {
                for (const char *p = x->str; *p; p++) buf_ch(&f, (char)tolower((unsigned char)*p));
            } else if (x->type == T_PAREN || (x->type == T_FUNC && (!strcasecmp(x->str, "calc") || !strcasecmp(x->str, "-webkit-calc")))) {
                MathSum in = { .n = 0 };
                math_sum_tokens(ts, x->first, x->end, &in, false);
                if (in.bad) { math_sum_free(&in); out->bad = true; break; }
                if (in.n > 1) f_is_sum = true;
                /* (a bracket that is all of the term goes into this sum) */
                if (tok_next(ts, s) == i && j == s) {
                    for (int q = 0; q < in.n && out->n < 32; q++) {
                        out->t[out->n] = in.t[q];
                        if (minus) { out->t[out->n].neg = !out->t[out->n].neg; out->t[out->n].num = -out->t[out->n].num; }
                        out->n++;
                    }
                    wanted_paren_sum = true;
                    break;
                }
                char *tx = math_sum_text(&in, in.n > 1);
                math_sum_free(&in);
                buf_str(&f, tx);
                free(tx);
            } else if (x->type == T_FUNC) {
                char *tx = math_is_plain_fn(x->str) ? math_fn_text(ts, j) : NULL;
                /* (what came to a calc() of its own, inside another: not twice) */
                if (tx && !strncmp(tx, "calc(", 5)) {
                    size_t tl = strlen(tx);
                    tx[tl - 1] = 0;
                    if (strstr(tx + 5, " + ") || strstr(tx + 5, " - ")) { buf_ch(&f, '('); buf_str(&f, tx + 5); buf_ch(&f, ')'); }
                    else buf_str(&f, tx + 5);
                    free(tx);
                }
                else if (tx) { buf_str(&f, tx); free(tx); }
                else {
                    Ser plain = { ts, NULL, 0, NULL, NULL };
                    ser_tok(&plain, j, &f, NULL, 0);
                }
            } else { out->bad = true; break; }
            only = j;
        }
        (void)only;
        if (!out->bad && !wanted_paren_sum && out->n < 32) {
            MathTerm *m = &out->t[out->n++];
            memset(m, 0, sizeof *m);
            Buf tb = { 0 };
            bool lone_factor = nf == 0;
            if (lone_factor) {
                /* (all numeric) */
                k /= kdiv;
                m->kind = unit ? (!strcmp(unit, "%") ? 1 : 2) : 0;
                m->num = minus ? -k : k;
                if (unit) snprintf(m->unit, sizeof m->unit, "%s", unit);
                { char *nt = math_num_text(k, unit); buf_str(&tb, nt); free(nt); }
                m->neg = minus != (k < 0);
                m->text = NULL;
                if (after.n) {
                    /* (with a divisor it stays as it's written) */
                    buf_str(&tb, after.s);
                    m->kind = 3;
                    m->product = true;
                }
                m->text = buf_take(&tb);
            } else {
                m->kind = 3;
                /* (a number divides the factor too, but for a whole sum) */
                if (after_nums && !f_is_sum) { k /= kdiv; any_num = true; after_nums = false; }
                bool hascoef = any_num || unit;
                if (after_nums) {
                    /* (a divisor stays where it's written) */
                    Buf tail = { 0 };
                    buf_str(&tail, " / ");
                    fmt_number(&tail, kdiv);
                    if (after.s) buf_str(&tail, after.s);
                    free(after.s);
                    after = tail;
                }
                if (hascoef) {
                    m->neg = minus != (k < 0);
                    fmt_number(&tb, fabs(k));
                    if (unit) buf_str(&tb, unit);
                    buf_str(&tb, " * ");
                    m->product = true;
                } else {
                    m->neg = minus;
                    if (nf > 1) m->product = true;
                }
                buf_str(&tb, f.s ? f.s : "");
                if (after.n) { buf_str(&tb, after.s); m->product = true; }
                m->text = buf_take(&tb);
            }
        }
        (void)divided;
        free(f.s);
        free(after.s);
        if (out->bad || i == e) break;
        minus = neg != (ts->t[i].delim == '-');
        s = i + 1;
    }
}

static int math_term_cmp(const void *a, const void *b)
{
    const MathTerm *x = (const MathTerm *)a, *y = (const MathTerm *)b;
    return x->kind - y->kind;
}

/* The terms of a sum as text, sorted (stable by insertion), brackets round products. */
static char *math_sum_text(MathSum *s, bool wrap)
{
    /* (like terms of a number or a dimension together) */
    for (int i = 0; i < s->n; i++) {
        if (s->t[i].kind == 3 || !s->t[i].text) continue;
        for (int j = i + 1; j < s->n; j++) {
            if (s->t[j].kind != s->t[i].kind || strcasecmp(s->t[j].unit, s->t[i].unit)) continue;
            s->t[i].num += s->t[j].num;
            free(s->t[j].text);
            memmove(&s->t[j], &s->t[j + 1], (size_t)(s->n - j - 1) * sizeof(MathTerm));
            s->n--;
            j--;
            free(s->t[i].text);
            s->t[i].text = math_num_text(s->t[i].num, s->t[i].unit);
            s->t[i].neg = s->t[i].num < 0;
        }
    }
    for (int i = 1; i < s->n; i++) {
        MathTerm t = s->t[i];
        int j = i;
        while (j > 0 && math_term_cmp(&s->t[j - 1], &t) > 0) { s->t[j] = s->t[j - 1]; j--; }
        s->t[j] = t;
    }
    Buf b = { 0 };
    if (wrap) buf_ch(&b, '(');
    for (int i = 0; i < s->n; i++) {
        const MathTerm *t = &s->t[i];
        bool neg = t->neg;
        bool paren = s->n > 1 && t->product;
        if (i) buf_str(&b, neg ? " - " : " + ");
        else if (neg && t->kind == 3 && !t->product) buf_str(&b, "-1 * ");
        else if (neg && !paren) buf_ch(&b, '-');
        if (paren) buf_ch(&b, '(');
        if (!i && neg && paren) buf_ch(&b, '-');
        buf_str(&b, t->text ? t->text : "");
        if (paren) buf_ch(&b, ')');
    }
    if (wrap) buf_ch(&b, ')');
    char *out = buf_take(&b);
    return out ? out : strdup("");
}

/* Math function token i, canonical: false to leave it as written. */
static bool math_ser_top(const Toks *ts, int i, Buf *b)
{
    const Tok *t = &ts->t[i];
    if (t->type != T_FUNC || !t->str || !math_is_plain_fn(t->str)) return false;
    char *s = math_fn_text(ts, i);
    if (!s) return false;
    buf_str(b, s);
    free(s);
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
    /* (gray but for conversion noise) */
    if (fabs(d) > 1e-7) {
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
    /* (within 1e-5 of the whole: gray) */
    if (w + b >= 1 - 1.000001e-5) { o[0] = o[1] = o[2] = w / (w + b); return; }
    double full[3] = { hwb[0], 100, 50 };
    hsl_to_srgb(full, o);
    for (int i = 0; i < 3; i++) o[i] = o[i] * (1 - w - b) + w;
}

static void srgb_to_hwb(const double c[3], double o[3], bool *hue_none) {
    double hsl[3];
    srgb_to_hsl(c, hsl, hue_none);
    double mx = fmax(c[0], fmax(c[1], c[2])), mn = fmin(c[0], fmin(c[1], c[2]));
    o[0] = hsl[0]; o[1] = mn * 100; o[2] = (1 - mx) * 100;
    /* (HSL's turn for a negative saturation isn't HWB's: its whiteness and
     * blackness keep the order the channels have) */
    double l = (mx + mn) / 2;
    if (fabs(mx - mn) > 1e-7 && l != 0 && l != 1 && (mx - l) / fmin(l, 1 - l) < 0) o[0] = norm_hue(hsl[0] - 180);
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
    if (*hue_none) o[1] = 0;
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
    case CS_HWB: return i == 0 ? 6 : 9 + i;   /* (10 white, 11 black) */
    case CS_LAB: case CS_OKLAB: return i == 0 ? 4 : i == 1 ? 7 : 8;
    case CS_LCH: case CS_OKLCH: return i == 0 ? 4 : i == 1 ? 5 : 6;
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
    /* (colorfulness within noise of none: gray, as it leaves) */
    if ((c->cs == CS_LCH && v[1] <= 0.0015) || (c->cs == CS_OKLCH && v[1] <= 0.000004) || (c->cs == CS_HSL && fabs(v[1]) <= 0.001)) v[1] = 0;
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
    /* (what the tests of CSS Color 4 12.2 carry beyond the plain analogues) */
    unsigned sn = c->none & 7;
    bool from_opposing = (c->cs == CS_LAB || c->cs == CS_OKLAB) && (sn & 6) == 6;
    bool from_chroma = ((c->cs == CS_LCH || c->cs == CS_OKLCH) && (sn & 2)) || (c->cs == CS_HSL && (sn & 2));
    bool from_whiteblack = c->cs == CS_HWB && (sn & 6) == 6;
    bool from_light_chroma = ((c->cs == CS_HSL && (sn & 6) == 6) || ((c->cs == CS_LCH || c->cs == CS_OKLCH) && (sn & 3) == 3));
    if (sn == 7) r.none |= 7;
    for (int i = 0; i < 3; i++) {
        int k = channel_kind(cs, i);
        if ((from_opposing || from_chroma) && (k == 5 || k == 6)) r.none |= 1u << i;
        if (from_chroma && (k == 7 || k == 8)) r.none |= 1u << i;
        if (from_whiteblack && (k == 4 || k == 5 || k == 10 || k == 11)) r.none |= 1u << i;
        if (from_light_chroma && (k == 10 || k == 11)) r.none |= 1u << i;
    }
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
    /* alpha(from <color> [ / <alpha-value> ]) (CSS Color 5) */
    if (cs == -1 && !strcasecmp(n, "alpha")) {
        int j = f->first;
        if (j >= f->end || !tok_ident(&ts->t[j], "from") || ++j >= f->end) return 0;
        Color o;
        int r = color_eval(ts, j, &o, depth + 1);
        if (!r) return 0;
        j = tok_next(ts, j);
        if (r == 1) { *out = o; out->legacy = false; out->derived = true; }
        else { memset(out, 0, sizeof *out); out->alpha = 1; out->derived = true; }
        if (j >= f->end) return 0;
        if (ts->t[j].type != T_DELIM || ts->t[j].delim != '/' || ++j >= f->end || tok_next(ts, j) < f->end) return 0;
        ColorVars av;
        memset(&av, 0, sizeof av);
        av.name[3] = "alpha";
        av.v[3] = out->none & 8 ? 0 : out->alpha;
        av.none = out->none;
        const ColorVars *was = t_color_vars;
        t_color_vars = &av;
        double v;
        bool none;
        bool ok = color_channel(ts, j, 'a', 1, false, &v, &none);
        t_color_vars = was;
        if (!ok) return 0;
        out->none &= 7;
        if (none) out->none |= 8; else out->alpha = clamp01(v);
        return r;
    }
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

/* Two colors (already in space cs) mixed, t of the second. */
static void color_mix2(Color a, Color b, double t, int cs, int hue_method, Color *out) {
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
    /* (no alpha on either: the channels aren't weighted by one) */
    if (out->none & 8) a.alpha = b.alpha = 1;
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
    out->alpha = (out->none & 8) ? 0 : alpha;
}

/* color-mix() (CSS Color 5 §2): one color or more mixed in a space, each by
 * its share (unsaid shares split what the said leave; under 100% in all, the
 * mix as transparent as what's missing). */
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
        /* (the method, then a comma: always) */
        if (i < f->end && ts->t[i].type == T_COMMA) i++;
        else return 0;
    }
    enum { MAXC = 16 };
    Color c[MAXC];
    double p[MAXC];
    bool has_p[MAXC];
    int nc = 0, res = 1;
    while (i < f->end) {
        if (nc == MAXC) return -1;     /* (more than kept here: valid, not mixed) */
        bool got = false;
        has_p[nc] = false;
        p[nc] = 0;
        for (int part = 0; part < 2 && i < f->end && ts->t[i].type != T_COMMA; part++) {
            const Tok *t = &ts->t[i];
            if (t->type == T_PERCENT || (t->type == T_FUNC && is_math_fn(t->str))) {
                CalcSum s;
                double v;
                const char *u;
                if (has_p[nc]) return 0;
                if (t->type == T_PERCENT) v = t->num;
                else if (!calc_simplify_value(ts, i, &s) || !calc_one(&s, &v, &u) || strcmp(u, "%")) return 0;
                if (t->type == T_PERCENT && (v < 0 || v > 100)) return 0;
                /* (a calc() out of range: clamped) */
                if (v < 0) v = 0;
                if (v > 100) v = 100;
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
    if (res < 0) return -1;
    /* (the unsaid share what the said leave) */
    double said = 0;
    int unsaid = 0;
    for (int k = 0; k < nc; k++) { if (has_p[k]) said += p[k]; else unsaid++; }
    double each = unsaid ? (100 - said) / unsaid : 0;
    if (each < 0) each = 0;
    double sum = 0;
    for (int k = 0; k < nc; k++) { if (!has_p[k]) p[k] = each; sum += p[k]; }
    /* (nothing of any: equal shares, and transparent) */
    double mult = sum < 100 ? sum / 100 : 1;
    if (sum <= 0) { for (int k = 0; k < nc; k++) p[k] = 1; sum = nc; mult = 0; }
    /* (each in turn into what's mixed so far, by its share of the whole so far) */
    Color acc = color_convert(&c[0], cs);
    double cum = p[0];
    if (nc == 1) color_mix2(acc, acc, 0, cs, hue_method, &acc);
    for (int k = 1; k < nc; k++) {
        Color b = color_convert(&c[k], cs);
        double t = cum + p[k] > 0 ? p[k] / (cum + p[k]) : 0;
        Color m;
        color_mix2(acc, b, t, cs, hue_method, &m);
        acc = m;
        cum += p[k];
    }
    *out = acc;
    out->cs = cs;
    out->derived = true;
    if (!(out->none & 8)) out->alpha *= mult;
    return 1;
}

static const struct { const char *n; double c[3]; } g_system_colors[] = {
    { "canvas", {255,255,255} }, { "field", {255,255,255} }, { "buttonface", {239,239,239} },
    { "linktext", {0,0,238} }, { "visitedtext", {85,26,139} }, { "activetext", {255,0,0} },
    { "graytext", {109,109,109} }, { "highlight", {181,213,255} }, { "mark", {255,255,0} },
    { "buttonborder", {118,118,118} }, { "accentcolor", {0,117,255} }, { "selecteditem", {0,117,255} },
    { "accentcolortext", {255,255,255} }, { "selecteditemtext", {255,255,255} },
    /* (deprecated ones, as CSS Color 4 maps them) */
    { "activeborder", {118,118,118} }, { "inactiveborder", {118,118,118} }, { "threeddarkshadow", {118,118,118} },
    { "threedhighlight", {118,118,118} }, { "threedlightshadow", {118,118,118} }, { "threedshadow", {118,118,118} },
    { "windowframe", {118,118,118} }, { "buttonhighlight", {239,239,239} }, { "buttonshadow", {239,239,239} },
    { "threedface", {239,239,239} }, { "activecaption", {255,255,255} }, { "appworkspace", {255,255,255} },
    { "background", {255,255,255} }, { "inactivecaption", {255,255,255} }, { "infobackground", {255,255,255} },
    { "menu", {255,255,255} }, { "scrollbar", {255,255,255} }, { "window", {255,255,255} },
    { "inactivecaptiontext", {109,109,109} },
};

/* Where a color-layers()'s colors start, past its blend mode; -1 if it has only that. */
static int color_layers_first(const Toks *ts, const Tok *f) {
    static const char *const modes[] = { "normal", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge",
        "color-burn", "hard-light", "soft-light", "difference", "exclusion", "hue", "saturation", "color", "luminosity" };
    int j = f->first;
    if (j >= f->end || ts->t[j].type != T_IDENT) return j;
    for (size_t k = 0; k < sizeof modes / sizeof modes[0]; k++)
        if (tok_ident(&ts->t[j], modes[k])) {
            j = tok_next(ts, j);
            if (j >= f->end || ts->t[j].type != T_COMMA) return -1;
            return j + 1;
        }
    return j;
}

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
            /* (the color in its own space, not its 8 bits) */
            if (g_env->color_text[0] && !strstr(g_env->color_text, "urrent")) {
                Toks ct;
                if (tokenize(g_env->color_text, strlen(g_env->color_text), &ct)) {
                    int r = ct.n > 0 ? color_eval(&ct, 0, out, depth + 1) : 0;
                    toks_free(&ct);
                    if (r == 1) return 1;
                }
                memset(out, 0, sizeof *out);
                out->cs = CS_SRGB;
                out->alpha = 1;
                out->legacy = true;
            }
            for (int k = 0; k < 3; k++) out->c[k] = g_env->color[k] / 255;
            out->alpha = g_env->color[3];
            return 1;
        }
        if (css_color_system(t->str)) {
            /* (a dark color scheme's own) */
            static const struct { const char *n; double c[3]; } dark[] = {
                { "canvas", {18,18,18} }, { "canvastext", {255,255,255} }, { "field", {59,59,59} }, { "fieldtext", {255,255,255} },
                { "buttontext", {255,255,255} }, { "visitedtext", {208,173,240} }, { "activetext", {255,158,158} },
                { "graytext", {128,128,128} }, { "menu", {59,59,59} }, { "menutext", {255,255,255} },
                { "window", {18,18,18} }, { "windowtext", {255,255,255} }, { "buttonhighlight", {107,107,107} },
                { "buttonshadow", {107,107,107} }, { "threedface", {107,107,107} }, { "infobackground", {59,59,59} },
                { "infotext", {255,255,255} },
            };
            if (g_env && g_env->dark)
                for (size_t k = 0; k < sizeof dark / sizeof dark[0]; k++)
                    if (!strcasecmp(t->str, dark[k].n)) {
                        for (int j = 0; j < 3; j++) out->c[j] = dark[k].c[j] / 255;
                        return 1;
                    }
            for (size_t k = 0; k < sizeof g_system_colors / sizeof g_system_colors[0]; k++)
                if (!strcasecmp(t->str, g_system_colors[k].n))
                    for (int j = 0; j < 3; j++) out->c[j] = g_system_colors[k].c[j] / 255;
            return 1;
        }
        return 0;
    }
    if (t->type != T_FUNC) return 0;
    if (!strcasecmp(t->str, "color-mix")) return color_mix(ts, t, out, depth);
    if (!strcasecmp(t->str, "contrast-color")) {
        /* (black or white, whichever stands out: WCAG luminance) */
        int j = t->first;
        Color in;
        if (j >= t->end || tok_next(ts, j) < t->end) return 0;
        int r = color_eval(ts, j, &in, depth + 1);
        if (!r) return 0;
        memset(out, 0, sizeof *out);
        out->cs = CS_SRGB;
        out->alpha = 1;
        out->legacy = true;
        if (r < 0) return -1;
        Color s = color_convert(&in, CS_SRGB);
        double lum = 0;
        static const double w[3] = { 0.2126, 0.7152, 0.0722 };
        for (int k = 0; k < 3; k++) {
            double v = (s.none >> k & 1) ? 0 : clamp01(s.c[k]);
            lum += w[k] * (v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
        }
        double on = lum > 0.179 ? 0 : 1;
        for (int k = 0; k < 3; k++) out->c[k] = on;
        return 1;
    }
    if (!strcasecmp(t->str, "color-layers")) {
        /* ([ <blend-mode> , ]? <color>#): valid, not flattened here */
        int j = color_layers_first(ts, t);
        if (j < 0) return 0;
        for (;;) {
            Color layer;
            if (j >= t->end || !color_eval(ts, j, &layer, depth + 1)) return 0;
            j = tok_next(ts, j);
            if (j >= t->end) return -1;
            if (ts->t[j].type != T_COMMA) return 0;
            j++;
        }
    }
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
    if ((s.cs == CS_HSL || s.cs == CS_HWB) && (s.none & 15)) {
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
    /* (the default space goes without saying) */
    bool default_space = i + 1 < f->end && tok_ident(&ts->t[i], "in") && tok_ident(&ts->t[i + 1], "oklab");
    if (default_space) {
        i += 2;
        if (i < f->end && ts->t[i].type == T_COMMA) i++;
    } else if (i < f->end && tok_ident(&ts->t[i], "in")) {
        buf_str(b, "in ");
        for (i++; i < f->end && ts->t[i].type == T_IDENT; i++) {
            if (tok_ident(&ts->t[i], "xyz")) { buf_str(b, "xyz-d65"); continue; }
            if (tok_ident(&ts->t[i], "shorter") && i + 1 < f->end && tok_ident(&ts->t[i + 1], "hue")) { i++; continue; }
            if (b->n && b->s[b->n - 1] != ' ') buf_ch(b, ' ');
            ser_ident(b, ts->t[i].str);
        }
        if (i < f->end && ts->t[i].type == T_COMMA) i++;
        buf_str(b, ", ");
    }
    int col[16], pct[16], n = 0;
    memset(col, -1, sizeof col);
    memset(pct, -1, sizeof pct);
    bool plain = true;
    for (; i < f->end && n < 16; n++) {
        for (; i < f->end && ts->t[i].type != T_COMMA; i = tok_next(ts, i)) {
            if (ts->t[i].type == T_PERCENT) pct[n] = i;
            else if (ts->t[i].type == T_FUNC && is_math_fn(ts->t[i].str)) { pct[n] = i; plain = false; }
            else col[n] = i;
        }
        if (i < f->end) i++;
    }
    double p[16];
    for (int k = 0; k < 16; k++) p[k] = pct[k] >= 0 ? ts->t[pct[k]].num : -1;
    bool show = !plain || n != 2 || !((p[0] < 0 || p[0] == 50) && (p[1] < 0 || p[1] == 50));
    /* (one alone at the whole of it: unsaid) */
    if (plain && n == 1 && (p[0] < 0 || p[0] == 100)) show = false;
    bool fill = plain && n == 2 && show;
    if (fill) {
        if (p[0] < 0) p[0] = 100 - p[1];
        if (p[1] < 0) p[1] = 100 - p[0];
    }
    /* (more: the unsaid share what the said leave, once any is said) */
    if (plain && n > 2) {
        double said = 0;
        int unsaid = 0;
        for (int k = 0; k < n; k++) { if (p[k] >= 0) said += p[k]; else unsaid++; }
        if (unsaid && unsaid < n) {
            double each = (100 - said) / unsaid;
            if (each < 0) each = 0;
            for (int k = 0; k < n; k++) if (p[k] < 0) p[k] = each;
            fill = true;
        }
    }
    Ser plain_ser = { ts, NULL, 0, NULL, NULL };
    for (int k = 0; k < n; k++) {
        if (k) buf_str(b, ", ");
        if (col[k] >= 0) color_spec(b, ts, col[k]);
        if (!show) continue;
        if (fill) { buf_ch(b, ' '); fmt_number(b, p[k]); buf_ch(b, '%'); }
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
    if (t->type == T_IDENT) {
        /* (keywords in lower case, except where a system color's is canonical as written) */
        for (const char *q = t->str; *q; q++) buf_ch(b, (char)tolower((unsigned char)*q));
        return;
    }
    if (t->type != T_FUNC && r != 1) { ser_tok(&plain, i, b, NULL, 0); return; }
    if (t->type == T_FUNC && !strcasecmp(t->str, "contrast-color") && t->first < t->end) {
        buf_str(b, "contrast-color(");
        color_spec(b, ts, t->first);
        buf_ch(b, ')');
        return;
    }
    if (r == 1 && !c.derived && (c.legacy || !has_math_arg(ts, t))) { color_serialize(b, &c); return; }
    if (t->type != T_FUNC) { ser_tok(&plain, i, b, NULL, 0); return; }
    if (!strcasecmp(t->str, "color-mix")) { color_mix_spec(b, ts, t); return; }
    if (!strcasecmp(t->str, "color-layers")) {
        buf_str(b, "color-layers(");
        int j = t->first, first = color_layers_first(ts, t);
        if (first > j && !tok_ident(&ts->t[j], "normal")) {
            ser_tok(&plain, j, b, NULL, 0);
            buf_str(b, ", ");
        }
        for (j = first; j < t->end; j = tok_next(ts, j)) {
            if (ts->t[j].type == T_COMMA) { buf_str(b, ", "); continue; }
            color_spec(b, ts, j);
        }
        buf_ch(b, ')');
        return;
    }
    char name[16];
    snprintf(name, sizeof name, "%s", t->str);
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!strcmp(name, "rgba") || !strcmp(name, "hsla")) name[3] = '\0';
    /* (with math in it, and no color it's from: its other channels as numbers) */
    bool relative = t->first < t->end && tok_ident(&ts->t[t->first], "from");
    int kind = !strcmp(name, "rgb") ? 1 : !strcmp(name, "hsl") || !strcmp(name, "hwb") ? 2 : !strcmp(name, "lab") ? 3 : !strcmp(name, "lch") ? 4 :
               !strcmp(name, "oklab") ? 5 : !strcmp(name, "oklch") ? 6 : !strcmp(name, "color") ? 7 : 0;
    if (!relative && kind) {
        buf_str(b, name);
        buf_ch(b, '(');
        int ch = 0;
        bool alpha = false, first = true;
        for (int j = t->first; j < t->end; j = tok_next(ts, j)) {
            const Tok *a = &ts->t[j];
            if (a->type == T_COMMA) { if (ch == 3) alpha = true; continue; }
            if (a->type == T_DELIM && a->delim == '/') { alpha = true; continue; }
            if (!first) buf_str(b, alpha ? " / " : " ");
            first = false;
            if (kind == 7 && ch == 0 && a->type == T_IDENT && !tok_ident(a, "none") && !alpha) {
                if (tok_ident(a, "xyz")) buf_str(b, "xyz-d65");
                else for (const char *q = a->str; *q; q++) buf_ch(b, (char)tolower((unsigned char)*q));
                kind = 8;
                continue;
            }
            if (a->type == T_FUNC && math_ser_top(ts, j, b)) { if (!alpha) ch++; continue; }
            if (a->type == T_IDENT) { for (const char *q = a->str; *q; q++) buf_ch(b, (char)tolower((unsigned char)*q)); if (!alpha) ch++; continue; }
            if (a->type != T_NUMBER && a->type != T_PERCENT && a->type != T_DIM) { ser_tok(&plain, j, b, NULL, 0); if (!alpha) ch++; continue; }
            double v = a->num;
            bool pct = a->type == T_PERCENT;
            if (alpha) { if (pct) v /= 100.0; v = v < 0 ? 0 : v > 1 ? 1 : v; }
            else if (a->type == T_DIM) {
                /* (an angle: in degrees) */
                double sc = 1.0;
                unit_dim(a->str, &sc);
                v = v * sc;
            }
            else switch (kind) {
            case 1: if (pct) v *= 2.55; v = v < 0 ? 0 : v > 255 ? 255 : v; break;
            case 2: if (ch > 0) v = v < 0 ? 0 : v > 100 ? 100 : v; break;
            case 3: if (ch == 0) v = v < 0 ? 0 : v > 100 ? 100 : v; else if (pct) v *= 1.25; break;
            case 4: if (ch == 0) v = v < 0 ? 0 : v > 100 ? 100 : v; else if (ch == 1) { if (pct) v *= 1.5; if (v < 0) v = 0; } break;
            case 5: if (ch == 0) { if (pct) v /= 100.0; v = v < 0 ? 0 : v > 1 ? 1 : v; } else if (pct) v *= 0.004; break;
            case 6: if (ch == 0) { if (pct) v /= 100.0; v = v < 0 ? 0 : v > 1 ? 1 : v; } else if (ch == 1) { if (pct) v *= 0.004; if (v < 0) v = 0; } break;
            case 8: if (pct) v /= 100.0; break;
            default: break;
            }
            fmt_number(b, v);
            ch++;
        }
        buf_ch(b, ')');
        return;
    }
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
        if (tok_ident(a, "xyz")) { buf_str(b, "xyz-d65"); continue; }
        if (a->type == T_FUNC && math_ser_top(ts, j, b)) continue;
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
        if (!g_env && t->type != T_IDENT && (color_tok_valid(S->ts, H->start) || (t->type == T_FUNC && has_math_arg(S->ts, t)))) { color_spec(out, S->ts, H->start); return; }
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

/* A dashed function's call at p, its arguments well formed: none, or each one
 * something, and a {} one nothing but that. */
static bool css_dashed_call(const char *p) {
    if (p[0] != '-' || p[1] != '-') return false;
    const char *q = p + 2;
    if (!*q || *q == '(') return false;
    while (isalnum((unsigned char)*q) || *q == '-' || *q == '_' || (unsigned char)*q >= 0x80) q++;
    if (*q != '(') return false;
    q++;
    int depth = 0, args = 0;
    bool blank = true, braced = false, after = false;
    for (;; q++) {
        char c = *q;
        if (!c) return false;
        if (depth == 0 && (c == ',' || c == ')')) {
            if (blank && (c == ',' || args)) return false;
            if (c == ')') return true;
            args++;
            blank = true; braced = false; after = false;
            continue;
        }
        if (depth == 0 && isspace((unsigned char)c)) continue;
        if (depth == 0) {
            if (c == '!' || c == ';' || c == '}' || after) return false;
            if (c == '{') { if (!blank) return false; braced = true; }
            blank = false;
        }
        if (c == '(' || c == '{' || c == '[') depth++;
        else if (c == ')' || c == '}' || c == ']') {
            if (--depth < 0) return false;
            if (depth == 0 && braced) after = true;
        }
    }
}

static bool has_substitution(const char *v) {
    for (const char *p = v; *p; p++) {
        if ((p == v || !isalnum((unsigned char)p[-1])) &&
            (strncasecmp(p, "var(", 4) == 0 || strncasecmp(p, "env(", 4) == 0 || strncasecmp(p, "attr(", 5) == 0 ||
             strncasecmp(p, "if(", 3) == 0 || strncasecmp(p, "ident(", 6) == 0 || strncasecmp(p, "random-item(", 12) == 0 ||
             (strncasecmp(p, "inherit(", 8) == 0 && p[8 + strspn(p + 8, " \t\n")] == '-' && p[9 + strspn(p + 8, " \t\n")] == '-') ||
             css_dashed_call(p)))
            return true;
    }
    return false;
}

/* Each ident( ... ) in v with something in it, no comma and no {} block at
 * its top level (what's in it is only checked once substituted). */
static const char *strcasestr_simple(const char *h, const char *n);
/* Each env( <custom-ident> <integer [0,inf]>* [, <fallback>]? ) in v. */
/* Each random-item( <key>, <item>#? ) in v: brackets that match, a key, a comma,
 * and no ! or ; outside a block. */
static bool random_item_calls_ok(const char *v) {
    for (const char *p = v; (p = strcasestr_simple(p, "random-item(")); p += 12) {
        if (p > v && (isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_')) continue;
        const char *a = p + 12;
        char st[64];
        int d = 0;
        bool comma = false, key = false, blk = false, other = false;
        const char *c = a;
        for (; *c; c++) {
            char ch = *c;
            if (ch == '"' || ch == '\'') { char q = ch; for (c++; *c && *c != q; c++) if (*c == '\\' && c[1]) c++; if (!*c) return false; if (!d && !comma) key = true; if (!d && comma) { if (blk) return false; other = true; } continue; }
            if (ch == '(' || ch == '[' || ch == '{') {
                if (d >= 64) return false;
                if (ch == '{' && !d && !comma) return false;
                /* (an item in {}: the block and nothing else) */
                if (!d && comma) { if (ch == '{' ? (blk || other) : blk) return false; if (ch == '{') blk = true; else other = true; }
                st[d++] = ch == '(' ? ')' : ch == '[' ? ']' : '}';
                if (!d || !comma) key = true;
                continue;
            }
            if (ch == ')' || ch == ']' || ch == '}') {
                if (!d) { if (ch == ')') break; return false; }
                if (st[--d] != ch) return false;
                continue;
            }
            if (!d && (ch == '!' || ch == ';')) return false;
            if (!d && ch == ',') { if (!comma && !key) return false; comma = true; blk = other = false; continue; }
            if (!d && !comma && !isspace((unsigned char)ch)) key = true;
            if (!d && comma && !isspace((unsigned char)ch)) { if (blk) return false; other = true; }
        }
        if (*c != ')' || !comma || !key) return false;
    }
    return true;
}
static bool env_calls_ok(const char *v) {
    for (const char *p = v; (p = strcasestr_simple(p, "env(")); p += 4) {
        if (p > v && (isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_')) continue;
        const char *a = p + 4;
        while (isspace((unsigned char)*a)) a++;
        /* (the name: an identifier, not a function's) */
        if (!(isalpha((unsigned char)*a) || *a == '-' || *a == '_' || (unsigned char)*a >= 0x80)) return false;
        if (*a == '-' && isdigit((unsigned char)a[1])) return false;
        while (isalnum((unsigned char)*a) || *a == '-' || *a == '_' || (unsigned char)*a >= 0x80) a++;
        if (*a == '(') return false;
        /* (then indices: non-negative integers) */
        for (;;) {
            const char *b = a;
            while (isspace((unsigned char)*a)) a++;
            if (*a == ',' || *a == ')') break;
            if (a == b || !isdigit((unsigned char)*a)) return false;
            while (isdigit((unsigned char)*a)) a++;
            if (*a && !isspace((unsigned char)*a) && *a != ',' && *a != ')') return false;
        }
    }
    return true;
}
/* Each var( name [, fallback] ): the name one thing, a custom property's name,
 * a var() that gives one, or a {} block of one. */
static bool var_calls_ok(const char *v) {
    for (const char *p = v; (p = strcasestr_simple(p, "var(")); p += 4) {
        if (p > v && (isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_')) continue;
        const char *a = p + 4, *e = a;
        int d = 0;
        for (; *e; e++) {
            if (*e == '(' || *e == '{' || *e == '[') d++;
            else if (*e == ')' || *e == '}' || *e == ']') { if (!d) break; d--; }
            else if (*e == ',' && !d) break;
        }
        const char *s = a, *t = e;
        while (s < t && isspace((unsigned char)*s)) s++;
        while (t > s && isspace((unsigned char)t[-1])) t--;
        if (s >= t) return false;
        /* (a {} block in it: the whole of it, with something in it) */
        bool block = false;
        int dd = 0;
        for (const char *q = s; q < t; q++) {
            if (*q == '(') dd++;
            else if (*q == ')') dd--;
            else if (*q == '{' && dd == 0) block = true;
        }
        if (!block) continue;
        if (*s != '{' || t[-1] != '}') return false;
        const char *bs = s + 1, *be = t - 1;
        while (bs < be && isspace((unsigned char)*bs)) bs++;
        if (bs >= be) return false;
        const char *k = t;
        if (k < t) return false;
    }
    return true;
}
/* Each --name( ... ) call's arguments: a named one with a value, a {} one
 * with something in it and no !important, no bad url. */
static bool dashed_calls_ok(const char *v) {
    for (const char *p = v; *p; p++) {
        if (!(p[0] == '-' && p[1] == '-') || (p > v && (isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_'))) continue;
        const char *n = p + 2;
        while (isalnum((unsigned char)*n) || *n == '-' || *n == '_' || (unsigned char)*n >= 0x80) n++;
        if (*n != '(') continue;
        /* (its arguments, comma by comma at its own level) */
        const char *a = n + 1;
        for (;;) {
            const char *e = a;
            int d = 0;
            char q = 0;
            for (; *e; e++) {
                if (q) { if (*e == '\\' && e[1]) e++; else if (*e == q) q = 0; continue; }
                if (*e == '"' || *e == '\'') { q = *e; continue; }
                if (*e == '(' || *e == '{' || *e == '[') d++;
                else if (*e == ')' || *e == '}' || *e == ']') { if (d == 0) break; d--; }
                else if (*e == ',' && d == 0) break;
            }
            const char *s = a, *t = e;
            while (s < t && isspace((unsigned char)*s)) s++;
            while (t > s && isspace((unsigned char)t[-1])) t--;
            if (s < t && *s == '{') {
                /* (a block: the whole argument, not empty, not important) */
                if (t[-1] != '}') return false;
                const char *bs = s + 1, *be = t - 1;
                while (bs < be && isspace((unsigned char)*bs)) bs++;
                if (bs >= be) return false;
                for (const char *k = bs; k + 1 < be; k++) if (*k == '!') return false;
            }
            if (s + 2 < t && s[0] == '-' && s[1] == '-') {
                const char *k = s + 2;
                while (k < t && (isalnum((unsigned char)*k) || *k == '-' || *k == '_' || (unsigned char)*k >= 0x80)) k++;
                while (k < t && isspace((unsigned char)*k)) k++;
                if (k < t && *k == ':') {
                    k++;
                    while (k < t && isspace((unsigned char)*k)) k++;
                    if (k >= t) return false;
                }
            }
            for (const char *k = s; k + 4 < t; k++)
                if (!strncasecmp(k, "url(", 4)) for (const char *u = k + 4; u < t && *u != ')'; u++) if (*u == '\\' && u[1] == '\n') return false;
            if (*e != ',') break;
            a = e + 1;
        }
    }
    return true;
}
static bool ident_calls_ok(const char *v) {
    for (const char *p = v; (p = strcasestr_simple(p, "ident(")); p += 6) {
        if (p > v && (isalnum((unsigned char)p[-1]) || p[-1] == '-' || p[-1] == '_')) continue;
        const char *a = p + 6;
        int d = 1;
        bool any = false;
        for (; *a && d; a++) {
            if (*a == '"' || *a == '\'') { char q = *a; for (a++; *a && *a != q; a++) if (*a == '\\' && a[1]) a++; if (!*a) break; any = true; continue; }
            if (*a == '(') d++;
            else if (*a == ')') { if (--d == 0) break; }
            else if (d == 1 && (*a == ',' || *a == '{' || *a == '}')) return false;
            if (!isspace((unsigned char)*a)) any = true;
        }
        if (!any) return false;
    }
    return true;
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

/* position-area (CSS Anchor Positioning): which group a keyword is of (0: none);
 * in a pair the first of the grammar goes first, and what adds nothing goes. */
static int area_group(const char *w, bool *neutral)
{
    static const struct { const char *base; int g; } k[] = {
        { "left", 1 }, { "right", 1 }, { "x-start", 1 }, { "x-end", 1 }, { "top", 2 }, { "bottom", 2 }, { "y-start", 2 }, { "y-end", 2 },
        { "block-start", 3 }, { "block-end", 3 }, { "inline-start", 4 }, { "inline-end", 4 },
        { "start", 5 }, { "end", 5 }, { NULL, 0 } };
    *neutral = !strcmp(w, "center") || !strcmp(w, "span-all");
    if (*neutral) return 0;
    const char *c = !strncmp(w, "span-", 5) ? w + 5 : w;
    bool self = !strncmp(c, "self-", 5);
    if (self) c += 5;
    for (int i = 0; k[i].base; i++) {
        if (strcmp(c, k[i].base)) continue;
        /* (self-: a group of its own, x and y with the rest) */
        return k[i].g <= 2 ? k[i].g + (self ? 10 : 0) : k[i].g + (self ? 10 : 0);
    }
    return -1;
}

static char *position_area_canon(const char *value)
{
    char a[2][40] = { "", "" }, extra[8] = "";
    int n = sscanf(value, " %39s %39s %7s", a[0], a[1], extra);
    if (n < 1 || n > 2) return NULL;
    for (int i = 0; i < n; i++) for (char *c = a[i]; *c; c++) *c = (char)tolower((unsigned char)*c);
    char out[96];
    if (n == 1 && !strcmp(a[0], "none")) return strdup("none");
    bool n0, n1 = false;
    int g0 = area_group(a[0], &n0), g1 = n == 2 ? area_group(a[1], &n1) : 0;
    if (g0 < 0 || g1 < 0) return NULL;
    if (n == 1) return strdup(a[0]);
    if (!strcmp(a[0], a[1])) {
        /* (the same word twice: only a word of no one axis) */
        if (n0 || g0 == 5 || g0 == 15) return strdup(a[0]);
        return NULL;
    }
    int base0 = g0 % 10, base1 = g1 % 10;
    bool self0 = g0 > 10, self1 = g1 > 10;
    /* start, end: either way round, as written */
    if (base0 == 5 || base1 == 5) {
        if ((n0 && base1 == 5) || (n1 && base0 == 5) || (base0 == 5 && base1 == 5 && self0 == self1)) {
            snprintf(out, sizeof out, "%s %s", a[0], a[1]);
            return strdup(out);
        }
        return NULL;
    }
    if (n0 && n1) { snprintf(out, sizeof out, "%s %s", a[0], a[1]); return strdup(out); }
    if (n0 || n1) {
        const char *word = n0 ? a[1] : a[0], *neutral = n0 ? a[0] : a[1];
        int b = n0 ? base1 : base0;
        if (!strcmp(neutral, "span-all")) return strdup(word);
        /* (x before y, block before inline) */
        if (b == 1 || b == 3) snprintf(out, sizeof out, "%s center", word);
        else snprintf(out, sizeof out, "center %s", word);
        return strdup(out);
    }
    if (self0 != self1) {
        /* (x, y: self- each on its own terms) */
        if (!((base0 <= 2 && base1 <= 2))) return NULL;
    }
    if ((base0 == 1 && base1 == 2) || (base0 == 3 && base1 == 4)) { snprintf(out, sizeof out, "%s %s", a[0], a[1]); return strdup(out); }
    if ((base0 == 2 && base1 == 1) || (base0 == 4 && base1 == 3)) { snprintf(out, sizeof out, "%s %s", a[1], a[0]); return strdup(out); }
    return NULL;
}

/* (set while a value that substitution made is read: what it came to is clamped later, not refused) */
bool g_css_parse_substituted;

static char *shape_fn_canon(const char *v);
static int canon_words(const char *v, char (*w)[160], int max);

/* Where calc-size()'s top-level comma is in s (which starts just past its
 * parenthesis), or NULL; *close set to its closing parenthesis. */
static const char *calc_size_split(const char *s, const char **close)
{
    int d = 0;
    const char *comma = NULL;
    for (const char *c = s; *c; c++) {
        if (*c == '(') d++;
        else if (*c == ')') { if (!d) { *close = c; return comma; } d--; }
        else if (*c == ',' && !d && !comma) comma = c;
    }
    *close = NULL;
    return NULL;
}

/* A calc-size() as written: a basis and a length its size can make (none with
 * any for a basis). */
static bool calc_size_ok(const char *v)
{
    while (isspace((unsigned char)*v)) v++;
    if (strncasecmp(v, "calc-size(", 10)) return true;
    const char *close = NULL, *comma = calc_size_split(v + 10, &close);
    if (!comma || !close) return false;
    for (const char *t = close + 1; *t; t++) if (!isspace((unsigned char)*t)) return false;
    const char *b = v + 10;
    while (isspace((unsigned char)*b)) b++;
    const char *be = comma;
    while (be > b && isspace((unsigned char)be[-1])) be--;
    if (be - b == 1 && *b == '0') return false;
    if (!strncasecmp(b, "calc-size(", 10)) {
        char *inner = strndup(b, (size_t)(be - b));
        bool ok = calc_size_ok(inner);
        free(inner);
        if (!ok) return false;
    }
    bool any = be - b == 3 && !strncasecmp(b, "any", 3);
    /* (size as a length for the check) */
    Buf e = { 0 };
    bool sized = false;
    buf_ch(&e, '(');
    for (const char *c = comma + 1; c < close; ) {
        bool edge = c == comma + 1 || !(isalnum((unsigned char)c[-1]) || c[-1] == '-' || c[-1] == '_');
        if (edge && !strncasecmp(c, "size", 4) && !(isalnum((unsigned char)c[4]) || c[4] == '-' || c[4] == '_' || c[4] == '(')) {
            buf_str(&e, "1px");
            c += 4;
            sized = true;
            continue;
        }
        buf_ch(&e, *c++);
    }
    buf_ch(&e, ')');
    if (any && sized) { free(e.s); return false; }
    Toks ts;
    bool ok = false;
    if (tokenize(e.s, e.n, &ts)) {
        ok = ts.n > 0 && calc_value(&ts, 0, D_LENGTH) == D_LENGTH;
        toks_free(&ts);
    }
    free(e.s);
    return ok;
}

/* calc-size()'s expression as it serializes: size's products with the number
 * first, a division as a product, the plain values sorted with percentages first. */
static char *calc_size_expr_canon(const char *x)
{
    Buf s = { 0 };
    int d = 0;
    for (const char *c = x; *c; c++) {
        if (*c == '(') d++; else if (*c == ')') d--;
        if (!d && (*c == '*' || *c == '/')) { buf_ch(&s, ' '); buf_ch(&s, *c); buf_ch(&s, ' '); }
        else buf_ch(&s, *c);
    }
    buf_ch(&s, 0);
    char w[24][160];
    int n = canon_words(s.s, w, 24);
    free(s.s);
    if (n < 1 || n >= 24) return NULL;
    for (int i = 0; i < n; i++) if (strchr(w[i], 40)) return NULL;
    /* (terms between + and -; only sums are sorted) */
    char terms[12][200];
    int nt = 0, st = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || !strcmp(w[i], "+") || !strcmp(w[i], "-")) {
            if (i < n && strcmp(w[i], "+")) return NULL;
            if (nt == 12 || i == st) return NULL;
            int k = i - st;
            if (k == 3 && (!strcmp(w[st + 1], "*") || !strcmp(w[st + 1], "/"))) {
                const char *a = w[st], *b = w[st + 2];
                bool div = !strcmp(w[st + 1], "/");
                char *e1 = NULL;
                if (!strcasecmp(a, "size") && (strtod(b, &e1), e1 != b && !*e1)) {
                    double f = strtod(b, NULL);
                    if (div) { if (f == 0) return NULL; f = 1.0 / f; }
                    snprintf(terms[nt], 200, "%.6g * size", f);
                } else if (!strcasecmp(b, "size") && !div && (strtod(a, &e1), e1 != a && !*e1)) {
                    snprintf(terms[nt], 200, "%s * size", a);
                } else snprintf(terms[nt], 200, "%s %s %s", a, w[st + 1], b);
            } else if (k == 1) snprintf(terms[nt], 200, "%s", w[st]);
            else return NULL;
            nt++;
            st = i + 1;
        }
    }
    Buf o = { 0 };
    int order[12], no = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < nt; i++) {
            const char *t = terms[i];
            bool plain = !strchr(t, ' ') && !strchr(t, '(') && strcasecmp(t, "size");
            bool pct = plain && t[strlen(t) - 1] == '%';
            if ((pass == 0 && pct) || (pass == 1 && plain && !pct) || (pass == 2 && !plain)) order[no++] = i;
        }
    for (int i = 0; i < no; i++) {
        const char *t = terms[order[i]];
        if (i) buf_str(&o, " + ");
        bool wrap = nt > 1 && strchr(t, ' ');
        if (wrap) buf_ch(&o, '(');
        buf_str(&o, t);
        if (wrap) buf_ch(&o, ')');
    }
    buf_ch(&o, 0);
    return o.s;
}

/* Each calc-size() in v with its expression canonical (nested ones too). */
static char *calc_size_canon(const char *v)
{
    const char *f = strstr(v, "calc-size(");
    if (!f) return NULL;
    const char *close = NULL, *comma = calc_size_split(f + 10, &close);
    if (!comma || !close) return NULL;
    Buf o = { 0 };
    buf_add(&o, v, (size_t)(f + 10 - v));
    char *basis = strndup(f + 10, (size_t)(comma - f - 10));
    char *nb = calc_size_canon(basis);
    buf_str(&o, nb ? nb : basis);
    free(nb);
    free(basis);
    buf_str(&o, ", ");
    char *x = strndup(comma + 1, (size_t)(close - comma - 1));
    char *cx = calc_size_expr_canon(x);
    const char *xs = x;
    while (isspace((unsigned char)*xs)) xs++;
    buf_str(&o, cx ? cx : xs);
    free(cx);
    free(x);
    buf_str(&o, close);
    buf_ch(&o, 0);
    if (!strcmp(o.s, v)) { free(o.s); return NULL; }
    return o.s;
}

/* SVG path data as path() serializes it: each command and its numbers one space
 * apart, Z upper case; NULL if it isn't path data. */
static char *path_data_canon(const char *d, size_t len)
{
    static const char cmds[] = "MmLlHhVvCcSsQqTtAaZz";
    static const int argn[] = { 2, 2, 2, 2, 1, 1, 1, 1, 6, 6, 4, 4, 4, 4, 2, 2, 7, 7, 0, 0 };
    Buf b = { 0 };
    const char *p = d, *e = d + len;
    char cmd = 0;
    bool first = true;
    while (1) {
        while (p < e && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (p >= e) break;
        const char *ci = strchr(cmds, *p);
        int n;
        if (ci && *p) {
            cmd = *p++;
            if (first && cmd != 'M' && cmd != 'm') { free(b.s); return NULL; }
            n = argn[ci - cmds];
            if (b.n) buf_ch(&b, ' ');
            buf_ch(&b, cmd == 'z' ? 'Z' : cmd);
            first = false;
            if (!n) continue;
        } else {
            /* (more numbers: the same command again; after a moveto, a lineto) */
            if (!cmd || cmd == 'Z' || cmd == 'z') { free(b.s); return NULL; }
            if (cmd == 'M') cmd = 'L'; else if (cmd == 'm') cmd = 'l';
            n = argn[strchr(cmds, cmd) - cmds];
            buf_ch(&b, ' ');
            buf_ch(&b, cmd);
        }
        for (int k = 0; k < n; k++) {
            while (p < e && (isspace((unsigned char)*p) || *p == ',')) p++;
            if (p >= e) { free(b.s); return NULL; }
            char nb[64];
            if ((cmd == 'A' || cmd == 'a') && (k == 3 || k == 4)) {
                if (*p != '0' && *p != '1') { free(b.s); return NULL; }
                nb[0] = *p++; nb[1] = 0;
            } else {
                char *end = NULL;
                double v = strtod(p, &end);
                if (end == p || end > e || !isfinite(v)) { free(b.s); return NULL; }
                p = end;
                /* (as short as gives the same number back, no exponent) */
                for (int pr = 1; pr <= 17; pr++) {
                    snprintf(nb, sizeof nb, "%.*g", pr, v);
                    if (strtod(nb, NULL) == v) break;
                }
                if (strchr(nb, 'e')) {
                    snprintf(nb, sizeof nb, "%.12f", v);
                    char *z = nb + strlen(nb);
                    while (z > nb && z[-1] == '0') *--z = 0;
                    if (z > nb && z[-1] == '.') z[-1] = 0;
                }
            }
            buf_ch(&b, ' ');
            buf_str(&b, nb);
        }
    }
    if (first) { free(b.s); return NULL; }
    buf_ch(&b, 0);
    return b.s;
}

/* Each path() in v with its data in canonical form, a default fill rule unsaid;
 * NULL if one isn't valid (or has a fill rule where none is allowed). */
static char *path_fn_canon(const char *v, bool rule_ok)
{
    Buf o = { 0 };
    const char *p = v;
    for (const char *f; (f = strstr(p, "path(")) != NULL; ) {
        if (f > v && (isalnum((unsigned char)f[-1]) || f[-1] == '-')) { buf_add(&o, p, (size_t)(f + 5 - p)); p = f + 5; continue; }
        buf_add(&o, p, (size_t)(f - p));
        const char *a = f + 5;
        while (isspace((unsigned char)*a)) a++;
        const char *rule = NULL;
        if (!strncasecmp(a, "nonzero", 7) || !strncasecmp(a, "evenodd", 7)) {
            rule = a;
            a += 7;
            while (isspace((unsigned char)*a)) a++;
            if (*a != ',') { free(o.s); return NULL; }
            a++;
            while (isspace((unsigned char)*a)) a++;
            if (!rule_ok) { free(o.s); return NULL; }
        }
        if (*a != '"' && *a != '\'') { free(o.s); return NULL; }
        char q = *a;
        const char *s = a + 1, *se = strchr(s, q);
        if (!se) { free(o.s); return NULL; }
        char *c = path_data_canon(s, (size_t)(se - s));
        if (!c) { free(o.s); return NULL; }
        const char *close = se + 1;
        while (isspace((unsigned char)*close)) close++;
        if (*close != ')') { free(c); free(o.s); return NULL; }
        buf_str(&o, "path(");
        if (rule && !strncasecmp(rule, "evenodd", 7)) buf_str(&o, "evenodd, ");
        buf_ch(&o, '"');
        buf_str(&o, c);
        buf_str(&o, "\")");
        free(c);
        p = close + 1;
    }
    buf_str(&o, p);
    buf_ch(&o, 0);
    return o.s;
}

/* dynamic-range-limit-mix( [ <'dynamic-range-limit'> && <percentage [0,100]> ]#{2,} ) at *s;
 * true and *s past it if it is one.  *nonzero: some percentage isn't 0 (or isn't known). */
static bool drl_mix(const char **s, bool *nonzero) {
    const char *p = *s;
    if (strncasecmp(p, "dynamic-range-limit-mix(", 24)) return false;
    p += 24;
    int items = 0;
    bool any = false;
    for (;;) {
        int kw = 0, pct = 0;
        for (int part = 0; part < 3; part++) {
            while (isspace((unsigned char)*p)) p++;
            if (*p == ',' || *p == ')') break;
            if (!strncasecmp(p, "dynamic-range-limit-mix(", 24)) {
                bool nz = false;
                if (kw || !drl_mix(&p, &nz)) return false;
                kw = 1;
                continue;
            }
            if (!strncasecmp(p, "standard", 8) || !strncasecmp(p, "no-limit", 8) || !strncasecmp(p, "constrained", 11)) {
                size_t l = !strncasecmp(p, "constrained", 11) ? 11 : 8;
                if (kw || isalnum((unsigned char)p[l]) || p[l] == '-') return false;
                kw = 1;
                p += l;
                continue;
            }
            if (!strncasecmp(p, "calc(", 5)) {
                if (pct) return false;
                int d = 0;
                const char *q = p;
                for (; *q; q++) { if (*q == '(') d++; else if (*q == ')' && --d == 0) { q++; break; } }
                if (d) return false;
                p = q;
                pct = 1;
                any = true;
                continue;
            }
            char *e = NULL;
            double v = strtod(p, &e);
            if (e == p || *e != '%' || pct) return false;
            if (v < 0 || v > 100) return false;
            if (v != 0) any = true;
            p = e + 1;
            pct = 1;
        }
        if (!kw || !pct) return false;
        items++;
        while (isspace((unsigned char)*p)) p++;
        if (*p == ',') { p++; continue; }
        if (*p == ')') { p++; break; }
        return false;
    }
    if (items < 2) return false;
    *nonzero = any;
    *s = p;
    return any;
}

static char *prop_parse_raw_in(const CssPropDef *p, const char *value);
static char *prop_parse_raw(const CssPropDef *p, const char *value) {
    bool held = t_pct_numbers;
    t_pct_numbers = p && pct_numbers_of(p->name);
    char *r = prop_parse_raw_in(p, value);
    t_pct_numbers = held;
    return r;
}
static char *prop_parse_raw_in(const CssPropDef *p, const char *value) {
    if (p && value && !strcmp(p->name, "dynamic-range-limit")) {
        const char *t = value;
        while (isspace((unsigned char)*t)) t++;
        if (!strncasecmp(t, "dynamic-range-limit-mix(", 24)) {
            const char *q = t;
            bool nz = false;
            if (!drl_mix(&q, &nz)) return NULL;
            while (isspace((unsigned char)*q)) q++;
            if (*q) return NULL;
            char *r = trim_dup(t);
            return r;
        }
    }
    if (p && value && !strcmp(p->name, "position-area") && strcasecmp(value, "inherit") && strcasecmp(value, "initial") &&
        strcasecmp(value, "unset") && strcasecmp(value, "revert") && strcasecmp(value, "revert-layer"))
        return position_area_canon(value);
    if (!p || !value) return NULL;
    char *v = trim_dup(value);
    if (!v[0]) { free(v); return NULL; }
    if (css_is_wide_keyword(v, strlen(v))) {
        for (char *q = v; *q; q++) *q = (char)tolower((unsigned char)*q);
        return v;
    }
    if (has_substitution(v)) {
        if (!ident_calls_ok(v) || !env_calls_ok(v) || !dashed_calls_ok(v) || !var_calls_ok(v) || !random_item_calls_ok(v)) { free(v); return NULL; }
        return v;
    }
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
        static int debug = -1;
        if (debug < 0) debug = getenv("CSS_PROPS_DEBUG") != NULL;
        if (debug)
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
    /* (what the grammar lets by: steps(n, jump-none) needs two steps; a keyword once) */
    if (out && !g_css_parse_substituted && (!strcmp(p->name, "animation-timing-function") || !strcmp(p->name, "transition-timing-function"))) {
        for (const char *q = out; (q = strstr(q, "steps(")); q += 6) {
            const char *e = strchr(q, ')'), *jn = strstr(q, "jump-none");
            if (!e || !jn || jn > e) continue;
            char *ne = NULL;
            double c = strtod(q + 6, &ne);
            if (ne != q + 6 ? !(c >= 2) : !strncmp(q + 6, "calc(NaN", 8)) { free(out); return NULL; }
        }
    }
    /* (path(): its data checked and in canonical form) */
    if (out && strstr(out, "path(") && (!strcmp(p->name, "clip-path") || !strcmp(p->name, "shape-outside") || !strcmp(p->name, "offset-path"))) {
        char *c = path_fn_canon(out, strcmp(p->name, "offset-path") != 0);
        free(out);
        out = c;
    }
    /* (calc-size(): checked as written, its expression canonical) */
    if (out && strstr(out, "calc-size(") && (strstr(p->name, "width") || strstr(p->name, "height") || strstr(p->name, "-size") || !strcmp(p->name, "flex-basis"))) {
        const char *rv = value;
        while (isspace((unsigned char)*rv)) rv++;
        if (!strncasecmp(rv, "calc-size(", 10) && !calc_size_ok(rv)) { free(out); return NULL; }
        char *c = calc_size_canon(out);
        if (c) { free(out); out = c; }
    }
    /* (shape(): what goes without saying unsaid, its positions in their order) */
    if (out && strstr(out, "shape(") && (!strcmp(p->name, "clip-path") || !strcmp(p->name, "shape-outside") || !strcmp(p->name, "offset-path"))) {
        char *c = shape_fn_canon(out);
        if (c) { free(out); out = c; }
    }
    /* (hanging-punctuation and image-resolution keep the order they were written in) */
    if (out && (!strcmp(p->name, "hanging-punctuation") || !strcmp(p->name, "image-resolution")) && !strchr(value, '(')) {
        Buf wb = { 0 };
        for (const char *q = value; *q; ) {
            while (isspace((unsigned char)*q)) q++;
            if (!*q) break;
            if (wb.n) buf_ch(&wb, ' ');
            while (*q && !isspace((unsigned char)*q)) buf_ch(&wb, (char)tolower((unsigned char)*q++));
        }
        buf_ch(&wb, 0);
        if (wb.s && wb.s[0]) { free(out); out = wb.s; } else free(wb.s);
    }
    /* (none only on its own: not a counter's name or one of a list of transitions) */
    if (out && (!strcmp(p->name, "counter-reset") || !strcmp(p->name, "counter-set") || !strcmp(p->name, "counter-increment") ||
                !strcmp(p->name, "transition-property"))) {
        bool list = strchr(out, ' ') || strchr(out, ',') || strchr(out, '(');
        for (const char *q = out; list && (q = strcasestr_simple(q, "none")); q += 4) {
            bool start = q == out || q[-1] == ' ' || q[-1] == ',' || q[-1] == '(';
            bool end = q[4] == 0 || q[4] == ' ' || q[4] == ',' || q[4] == ')';
            if (start && end) { free(out); return NULL; }
        }
    }
    if (out && !strcmp(p->name, "position-visibility")) {
        static const char *const kw[] = { "anchors-valid", "anchor-valid", "anchors-visible", "anchor-visible", "no-overflow" };
        for (size_t k = 0; k < sizeof kw / sizeof kw[0]; k++) {
            int seen = 0;
            size_t l = strlen(kw[k]);
            for (const char *q = out; (q = strstr(q, kw[k])); q += l)
                if ((q == out || q[-1] == ' ') && (q[l] == 0 || q[l] == ' ')) seen++;
            if (seen > 1) { free(out); return NULL; }
        }
    }
    return out;
}

/* What a relative color or color-mix() was written as, by its specified value:
 * that has its origins in 8 bits (rgb()), and computing goes by the original. */
enum { COLOR_RAW_N = 512 };
static char *g_color_raw[COLOR_RAW_N][2];
static int g_color_raw_next;

static void color_raw_note(const char *canon, const char *raw) {
    /* (the latest raw wins: hsl(0 0 50%) and hsl(180 0 50%) are one rgb()) */
    for (int i = 0; i < COLOR_RAW_N; i++)
        if (g_color_raw[i][0] && !strcmp(g_color_raw[i][0], canon)) {
            if (strcmp(g_color_raw[i][1], raw)) { free(g_color_raw[i][1]); g_color_raw[i][1] = strdup(raw); }
            return;
        }
    int k = g_color_raw_next++ % COLOR_RAW_N;
    free(g_color_raw[k][0]);
    free(g_color_raw[k][1]);
    g_color_raw[k][0] = strdup(canon);
    g_color_raw[k][1] = strdup(raw);
}

static const char *color_raw_of(const char *value) {
    if (!strstr(value, "from ") && !strstr(value, "color-mix(")) return value;
    for (int i = 0; i < COLOR_RAW_N; i++)
        if (g_color_raw[i][0] && !strcmp(g_color_raw[i][0], value)) return g_color_raw[i][1];
    return value;
}

static char *css_prop_canon(const CssPropDef *p, const char *v);
static int canon_words(const char *v, char (*w)[160], int max);
static __thread bool g_canon_computed;
static char *gradient_computed(const char *v);
char *css_gradient_computed(const char *v) { return gradient_computed(v); }
char *css_shape_computed(const char *n, const char *v);
static char *css_prop_compute_in(const CssPropDef *p, const char *value, const CssComputeEnv *env);
char *css_prop_compute(const CssPropDef *p, const char *value, const CssComputeEnv *env) {
    bool held = t_pct_numbers;
    t_pct_numbers = p && pct_numbers_of(p->name);
    char *r = css_prop_compute_in(p, value, env);
    t_pct_numbers = held;
    return r;
}
static char *css_prop_compute_in(const CssPropDef *p, const char *value, const CssComputeEnv *env) {
    if (!p || !value || !env || p->longhands) return NULL;
    char *v = trim_dup(color_raw_of(value));
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
    /* (an integer property's math: rounded to the nearest, halves up) */
    if (out && strchr(value, '(') && p->grammar && strstr(p->grammar, "<integer") && !strstr(p->grammar, "<number")) {
        bool wrap = !strncmp(out, "calc(", 5);
        const char *st = out + (wrap ? 5 : 0);
        char *e = NULL;
        double d = strtod(st, &e);
        if (e != st && (wrap ? e[0] == ')' && !e[1] : !e[0]) && isfinite(d) && fabs(d) < 2147483647.0) {
            char nb[32];
            snprintf(nb, sizeof nb, "%.0f", floor(d + 0.5));
            free(out);
            out = strdup(nb);
        }
    }
    /* (scale's percentages, computed: the numbers they are) */
    if (out && !strcmp(p->name, "scale") && strchr(out, '%')) {
        char w[3][160];
        int nw = canon_words(out, w, 3);
        Buf sb = { 0 };
        bool ok = nw > 0;
        for (int i = 0; i < nw && ok; i++) {
            const char *t = w[i];
            if (!strncasecmp(t, "calc(", 5)) t += 5;
            char *e = NULL;
            double d = strtod(t, &e);
            if (e == t) { ok = false; break; }
            if (*e == '%') d /= 100;
            if (i) buf_ch(&sb, ' ');
            fmt_number(&sb, d);
        }
        if (ok) { buf_ch(&sb, 0); free(out); out = sb.s; } else free(sb.s);
    }
    if (out && (!strcmp(p->name, "contain") || !strcmp(p->name, "text-fit") ||
                strstr(p->name, "timing-function") || !strncmp(p->name, "animation-range-", 16) ||
                !strcmp(p->name, "counter-increment") || !strcmp(p->name, "counter-set") || !strcmp(p->name, "font-weight") ||
                !strcmp(p->name, "font-width") || !strcmp(p->name, "font-stretch") || !strcmp(p->name, "font-style") ||
                !strncmp(p->name, "scroll-padding", 14) || !strcmp(p->name, "hyphenate-limit-chars") || !strcmp(p->name, "offset-rotate"))) {
        g_canon_computed = true;
        char *cc = css_prop_canon(p, out);
        g_canon_computed = false;
        if (cc && cc[0]) { free(out); out = cc; } else free(cc);
    }
    if (out && strstr(out, "gradient(") && (!strcmp(p->name, "background-image") || !strcmp(p->name, "mask-image") ||
                                             !strcmp(p->name, "list-style-image") || !strcmp(p->name, "border-image-source"))) {
        char *gc = gradient_computed(out);
        if (gc) { free(out); out = gc; }
    }
    if (out && strchr(out, '(') && (!strcmp(p->name, "clip-path") || !strcmp(p->name, "shape-outside") || (!strcmp(p->name, "offset-path") && !strstr(out, "ray(")))) {
        char *sc = css_shape_computed(p->name, out);
        if (sc) { free(out); out = sc; }
    }
    return out;
}

/* ============================================================================
 * Shorthands
 * ============================================================================ */


/* (Built on first use; layout threads call this too, so a list is complete
 * before it is published, and a racing duplicate is harmless.) */
int css_prop_longhands(const CssPropDef *p, const char ***names) {
    typedef struct { int n; const char *names[64]; } Longhands;
    static Longhands *lists[NPROPS];
    int pi = (int)(p - g_props);
    if (!p->longhands || pi < 0 || pi >= NPROPS) { *names = NULL; return 0; }
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

static int canon_words(const char *v, char (*w)[160], int max);

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
    /* (first baseline is what baseline is) */
    if (!strncmp(p->name, "place-", 6))
        for (char *fb; (fb = strstr(v, "first baseline")) != NULL; ) memmove(fb, fb + 6, strlen(fb + 6) + 1);
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
            /* (a baseline isn't a justification: start) */
            if (nf == 1 && !strcmp(p->name, "place-content") && strstr(vals[0], "baseline")) out[1] = strdup("start");
            else out[1] = strdup(vals[nf == 2 ? 1 : 0]);
            result = n;
        } else if (p->shorthand == CSS_SH_GRAMMAR) {
            for (int i = 0; i < nf; i++) {
                int li = lh_index(names, n, g_props[S.hits[found[i]].prop].name);
                if (li >= 0 && !out[li]) out[li] = strdup(vals[i]);
                /* (flex-wrap as it is on its own: balance's wrap unsaid) */
                if (li >= 0 && out[li] && !strcmp(names[li], "flex-wrap") && strstr(out[li], "balance")) {
                    char *c = css_prop_canon(&g_props[S.hits[found[i]].prop], out[li]);
                    if (c && c[0]) { free(out[li]); out[li] = c; } else free(c);
                }
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
    } else if (!strcmp(p->name, "columns") && n == 3) {
        /* width and count that aren't auto; a height after a slash */
        if (strcmp(vals[0], "auto")) buf_str(&b, vals[0]);
        if (strcmp(vals[1], "auto")) { if (b.n) buf_ch(&b, ' '); buf_str(&b, vals[1]); }
        if (!b.n) buf_str(&b, "auto");
        if (strcmp(vals[2], "auto")) { buf_str(&b, " / "); buf_str(&b, vals[2]); }
    } else if (!strcmp(p->name, "border-image") && n == 5) {
        /* source slice [/ width [/ outset]] repeat: what isn't initial; a slash
         * for each part after the slice that's said (an empty width between) */
        const char *src = NULL, *sl = NULL, *wd = NULL, *os = NULL, *rp = NULL;
        for (int i = 0; i < n; i++) {
            const char *nmi = names[i];
            if (!strcmp(nmi, "border-image-source")) src = vals[i];
            else if (!strcmp(nmi, "border-image-slice")) sl = vals[i];
            else if (!strcmp(nmi, "border-image-width")) wd = vals[i];
            else if (!strcmp(nmi, "border-image-outset")) os = vals[i];
            else if (!strcmp(nmi, "border-image-repeat")) rp = vals[i];
        }
        if (!src || !sl || !wd || !os || !rp) { free(b.s); return NULL; }
        bool has_w = strcmp(wd, "1") != 0, has_o = strcmp(os, "0") != 0;
        if (strcmp(src, "none")) buf_str(&b, src);
        if (strcmp(sl, "100%") || has_w || has_o) {
            if (b.n) buf_ch(&b, ' ');
            /* (the slice's four sides, as few as say them) */
            char w[5][160];
            int nw = canon_words(sl, w, 5);
            bool fill = nw > 0 && !strcmp(w[nw - 1], "fill");
            int ns = fill ? nw - 1 : nw;
            if (ns == 4) {
                if (!strcmp(w[1], w[3])) { ns = 3; if (!strcmp(w[0], w[2])) { ns = 2; if (!strcmp(w[0], w[1])) ns = 1; } }
                for (int i = 0; i < ns; i++) { if (i) buf_ch(&b, ' '); buf_str(&b, w[i]); }
                if (fill) buf_str(&b, " fill");
            } else buf_str(&b, sl);
        }
        if (has_w || has_o) { buf_str(&b, " /"); if (has_w) { buf_ch(&b, ' '); buf_str(&b, wd); } }
        if (has_o) { buf_str(&b, " / "); buf_str(&b, os); }
        if (strcmp(rp, "stretch")) { if (b.n) buf_ch(&b, ' '); buf_str(&b, rp); }
        if (!b.n) buf_str(&b, "none");
    } else {
        /* the longhands that are not initial, in order (or the first) */
        for (int i = 0; i < n; i++) {
            const CssPropDef *d = css_prop_find(names[i], strlen(names[i]));
            if (d && d->initial && strcmp(vals[i], d->initial) == 0) continue;
            if (b.n) buf_ch(&b, ' ');
            buf_str(&b, vals[i]);
        }
        /* (a border side with nothing but initial values: none) */
        if (!b.n) buf_str(&b, n == 3 && !strncmp(p->name, "border-", 7) && strstr(names[1], "-style") ? vals[1] : vals[0]);
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
        ok = nl > 2;   /* ("--" alone is no custom property) */
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

/* ---- Values layout should ignore ----
 * For the properties below, a value outside the property's grammar is
 * dropped (the declaration before it stays), as CSS 2.1 section 4.2 says.
 * Others are left to their layout parsers.  The common simple values are
 * recognised without the grammar matcher, which costs microseconds. */

typedef enum { VK_NONE, VK_COLOR, VK_LEN1, VK_LEN4, VK_GRAMMAR } ValueKind;

static ValueKind value_kind(const char *p)
{
    typedef struct { const char *name; ValueKind kind; } Named;
    static const Named k[] = {
        { "background", VK_GRAMMAR }, { "background-color", VK_COLOR }, { "background-image", VK_GRAMMAR },
        { "border-bottom-color", VK_COLOR }, { "border-bottom-style", VK_GRAMMAR }, { "border-bottom-width", VK_LEN1 },
        { "border-color", VK_GRAMMAR }, { "border-left-color", VK_COLOR }, { "border-left-style", VK_GRAMMAR },
        { "border-left-width", VK_LEN1 }, { "border-right-color", VK_COLOR }, { "border-right-style", VK_GRAMMAR },
        { "border-right-width", VK_LEN1 }, { "border-style", VK_GRAMMAR }, { "border-top-color", VK_COLOR },
        { "border-top-style", VK_GRAMMAR }, { "border-top-width", VK_LEN1 }, { "border-width", VK_LEN4 }, { "bottom", VK_LEN1 },
        { "caret-color", VK_COLOR }, { "color", VK_COLOR },
        { "font-family", VK_GRAMMAR }, { "font-size", VK_LEN1 }, { "height", VK_LEN1 }, { "left", VK_LEN1 },
        { "letter-spacing", VK_LEN1 }, { "line-height", VK_LEN1 }, { "margin", VK_LEN4 },
        { "margin-bottom", VK_LEN1 }, { "margin-left", VK_LEN1 }, { "margin-right", VK_LEN1 },
        { "margin-top", VK_LEN1 }, { "max-height", VK_LEN1 }, { "max-width", VK_LEN1 }, { "min-height", VK_LEN1 },
        { "min-width", VK_LEN1 }, { "outline-color", VK_COLOR }, { "padding", VK_LEN4 },
        { "padding-bottom", VK_LEN1 }, { "padding-left", VK_LEN1 }, { "padding-right", VK_LEN1 },
        { "padding-top", VK_LEN1 }, { "right", VK_LEN1 }, { "text-decoration-color", VK_COLOR },
        { "text-indent", VK_LEN1 }, { "top", VK_LEN1 }, { "width", VK_LEN1 }, { "word-spacing", VK_LEN1 },
    };
    /* (sorted by name: past p's first letter, no match) */
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (k[i].name[0] < p[0]) continue;
        if (k[i].name[0] > p[0]) break;
        if (!strcmp(p, k[i].name)) return k[i].kind;
    }
    return VK_NONE;
}

/* The end of a plain number at s (sign, digits, at most one '.', a digit
 * somewhere), or NULL. */
static const char *simple_number_end(const char *s, const char *e)
{
    const char *p = s;
    if (p < e && (*p == '+' || *p == '-')) p++;
    bool digit = false, dot = false;
    for (; p < e; p++) {
        if (isdigit((unsigned char)*p)) digit = true;
        else if (*p == '.' && !dot) dot = true;
        else break;
    }
    return digit ? p : NULL;
}

/* A plain length or percentage token [s, e) (a bare number too: quirks
 * pages write width: 100), or auto. */
static bool simple_length(const char *s, const char *e)
{
    static const char *const units[] = { "px", "em", "rem", "ex", "ch", "%", "vw", "vh", "vmin", "vmax", "cm",
                                         "mm", "in", "pt", "pc", "q", "lh", "rlh", "svh", "lvh", "dvh", "svw",
                                         "lvw", "dvw", "vi", "vb", NULL };
    size_t n = (size_t)(e - s);
    if (n == 4 && !strncasecmp(s, "auto", 4)) return true;
    const char *p = simple_number_end(s, e);
    if (!p) return false;
    if (p == e) return true;
    for (int i = 0; units[i]; i++)
        if ((size_t)(e - p) == strlen(units[i]) && !strncasecmp(p, units[i], (size_t)(e - p))) return true;
    return false;
}

/* The value is 1 (max 1) or 1 - 4 (max 4) plain lengths: true when it is
 * one of those, so known valid. */
static bool simple_lengths(const char *v, int max)
{
    int n = 0;
    const char *p = v;
    for (;;) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != ' ') p++;
        if (++n > max || !simple_length(s, p)) return false;
        /* (a bare number only as the one value: margin: 10 0 is not one) */
        if (max > 1 && simple_number_end(s, p) == p && strtod(s, NULL) != 0) return false;
    }
    return n > 0;
}

/* The keywords a one-length property takes besides auto: none (max-width,
 * max-height), normal (line-height, letter-spacing, word-spacing). */
static bool simple_length_keyword(const char *prop, const char *v)
{
    if (!strcasecmp(v, "none")) return !strncmp(prop, "max-", 4);
    if (!strcasecmp(v, "normal"))
        return !strcmp(prop, "line-height") || !strcmp(prop, "letter-spacing") || !strcmp(prop, "word-spacing");
    return false;
}

/* The value is one calc() / min() / max() / clamp() (its insides not
 * checked: the layout's math evaluator reads them). */
static bool one_math_function(const char *v)
{
    static const char *const fns[] = { "calc(", "min(", "max(", "clamp(", NULL };
    for (int i = 0; fns[i]; i++) {
        size_t l = strlen(fns[i]);
        if (strncasecmp(v, fns[i], l)) continue;
        int d = 1;
        const char *p = v + l;
        for (; *p && d; p++) d += *p == '(' ? 1 : *p == ')' ? -1 : 0;
        while (*p && isspace((unsigned char)*p)) p++;
        return d == 0 && !*p;
    }
    return false;
}

/* A font-family list of plain names: quoted without escapes, or identifiers
 * (none starting with a digit) with spaces between. */
static bool simple_families(const char *v)
{
    const char *p = v;
    for (;;) {
        while (*p == ' ') p++;
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            for (; *p != q; p++)
                if (!*p || *p == '\\' || *p == '\n') return false;
            p++;
        } else {
            bool any = false, word_start = true;
            for (; *p && *p != ','; p++) {
                unsigned char c = (unsigned char)*p;
                if (c == ' ') {
                    word_start = true;
                    continue;
                }
                if (!(isalpha(c) || c == '-' || c == '_' || c >= 0x80 || (isdigit(c) && !word_start))) return false;
                any = true;
                word_start = false;
            }
            if (!any) return false;
        }
        while (*p == ' ') p++;
        if (!*p) return true;
        if (*p != ',') return false;
        p++;
    }
}

bool css_value_invalid(const char *prop, const char *value)
{
    if (!prop || !value || (prop[0] == '-' && prop[1] == '-')) return false;
    ValueKind k = value_kind(prop);
    while (isspace((unsigned char)*value)) value++;
    if (css_is_wide_keyword(value, strlen(value)) || has_substitution(value)) return false;
    double rgba[4];
    if (k == VK_COLOR && css_color_parse(value, rgba)) return false;
    if (k == VK_LEN1 || k == VK_LEN4) {
        if (simple_lengths(value, k == VK_LEN1 ? 1 : 4)) return false;
        if (k == VK_LEN1 && simple_length_keyword(prop, value)) return false;
        if (k == VK_LEN1 && one_math_function(value)) return false;
    }
    if (!strcmp(prop, "font-family") && simple_families(value)) return false;
    if (!strcmp(prop, "background") && (!strcasecmp(value, "none") || css_color_parse(value, rgba))) return false;
    const CssPropDef *p = css_prop_find(prop, strlen(prop));
    if (!p) return false;
    char *s = css_prop_parse(p, value);
    bool ok = s != NULL;
    free(s);
    return !ok;
}

static __thread const char *t_sup_from;
static const char *strcasestr_simple(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) if (!strncasecmp(h, n, l)) return h;
    return NULL;
}

/* selector() in @supports: one complex selector, and nothing forgiven inside :is(),
 * :where(), :has() or an :nth-child()'s "of". */
/* (a '!' that is part of a <!-- token, spaces or not around it) */
static bool sup_cdo_bang(const char *b, const char *p)
{
    const char *q = p;
    while (q > b && q[-1] == ' ') q--;
    if (q > b && q[-1] == '<') return true;
    q = p + 1;
    while (*q == ' ') q++;
    return q[0] == '-' && q[1] == '-';
}
static bool sup_selector_branches(const char *s, size_t n, bool relative);
static bool sup_selector_one(const char *s, size_t n, bool relative)
{
    extern bool dom_selector_valid(const char *s);
    char *t = strndup(s, n);
    if (!t) return false;
    char *x = t;
    while (isspace((unsigned char)*x)) x++;
    size_t l = strlen(x);
    while (l && isspace((unsigned char)x[l - 1])) x[--l] = 0;
    if (!*x) { free(t); return false; }
    if (relative && (*x == '>' || *x == '+' || *x == '~')) { x++; while (isspace((unsigned char)*x)) x++; }
    /* (a column combinator, or a vendor pseudo-element nobody knows) */
    bool ok = !strstr(x, " | ") && !strstr(x, "||");
    for (const char *w = strstr(x, "::-webkit-"); ok && w; w = strstr(w + 1, "::-webkit-")) {
        static const char *const known[] = { "scrollbar", "resizer", "search-cancel-button", "inner-spin-button", "outer-spin-button",
            "input-placeholder", "file-upload-button", "slider-thumb", "slider-runnable-track", "details-marker", "progress-bar",
            "progress-value", "progress-inner-element", "meter-bar", "meter-optimum-value", "meter-suboptimum-value",
            "meter-even-less-good-value", "color-swatch", "color-swatch-wrapper", "calendar-picker-indicator", "datetime-edit" };
        const char *nm = w + 10;
        bool hit = false;
        for (size_t k = 0; k < sizeof known / sizeof known[0] && !hit; k++) hit = !strncasecmp(nm, known[k], strlen(known[k]));
        if (!hit) ok = false;
    }
    extern bool css_selector_text_invalid(const char *sel);
    (void)dom_selector_valid;
    if (ok) ok = !css_selector_text_invalid(x);
    /* (:host()'s is one compound, however deep: no combinator in it) */
    for (const char *h = x; ok && (h = strstr(h, ":host(")); h++) {
        const char *c = h + 6;
        int d = 1;
        char q = 0;
        for (; *c && d; c++) {
            if (q) { if (*c == q) q = 0; continue; }
            if (*c == '"' || *c == '\'') { q = *c; continue; }
            if (*c == '(') d++;
            else if (*c == ')') d--;
            else if (*c == '>' || *c == '+' || *c == '~') ok = false;
            else if (isspace((unsigned char)*c) && c[-1] != ',' && c[-1] != '(' && c[1] && !strchr(",)", c[1]) && !isspace((unsigned char)c[1])) ok = false;
        }
    }
    /* (what the lists inside say, each strictly) */
    for (const char *p = x; ok && (p = strchr(p, ':')); p++) {
        static const struct { const char *n; int kind; } fns[] = {
            { ":is(", 0 }, { ":where(", 0 }, { ":matches(", 0 }, { ":-webkit-any(", 0 }, { ":not(", 0 }, { ":has(", 1 },
            { ":nth-child(", 2 }, { ":nth-last-child(", 2 } };
        for (size_t k = 0; k < sizeof fns / sizeof fns[0]; k++) {
            size_t fl = strlen(fns[k].n);
            if (strncasecmp(p, fns[k].n, fl) || (p > x && p[-1] == ':')) continue;
            const char *a = p + fl, *e = a;
            int d = 1;
            for (; *e && d; e++) { if (*e == '(') d++; else if (*e == ')') d--; }
            if (d) { ok = false; break; }
            size_t an = (size_t)(e - 1 - a);
            if (fns[k].kind == 2) {
                const char *of = NULL;
                for (const char *c = a; c + 3 < a + an && !of; c++)
                    if (isspace((unsigned char)c[0]) && !strncasecmp(c + 1, "of", 2) && isspace((unsigned char)c[3])) of = c;
                const char *ie = of ? of : a + an;
                /* (An+B and nothing else) */
                for (const char *c = a; c < ie && ok; c++) {
                    if (isalpha((unsigned char)*c) && !strchr("nN", *c) && strncasecmp(c, "odd", 3) && strncasecmp(c, "even", 4)) ok = false;
                    if (!strncasecmp(c, "odd", 3)) c += 2;
                    else if (!strncasecmp(c, "even", 4)) c += 3;
                }
                if (ok) {
                    /* (two numbers in a row: "2n 4") */
                    bool num_end = false, gap = false;
                    for (const char *c = a; c < ie && ok; c++) {
                        if (isspace((unsigned char)*c)) { if (num_end) gap = true; continue; }
                        if (isdigit((unsigned char)*c) && gap) ok = false;
                        num_end = isdigit((unsigned char)*c) || *c == 'n' || *c == 'N';
                        if (*c == '+' || *c == '-') num_end = false;
                        gap = false;
                    }
                }
                if (ok && of) ok = sup_selector_branches(of + 4, (size_t)(a + an - of - 4), false);
            } else {
                /* (no pseudo-element in a list of elements) */
                for (size_t c = 0; c + 1 < an && ok; c++) if (a[c] == ':' && a[c + 1] == ':') ok = false;
                if (ok) ok = sup_selector_branches(a, an, fns[k].kind == 1);
            }
            break;
        }
    }
    free(t);
    return ok;
}

static bool sup_selector_branches(const char *s, size_t n, bool relative)
{
    size_t st = 0;
    int d = 0;
    for (size_t i = 0; i <= n; i++) {
        if (i < n && (s[i] == '(' || s[i] == '[')) d++;
        else if (i < n && (s[i] == ')' || s[i] == ']')) d--;
        if (i == n || (s[i] == ',' && !d)) {
            if (!sup_selector_one(s + st, i - st, relative)) return false;
            st = i + 1;
        }
    }
    return true;
}

static bool sup_selector(const char *s)
{
    int d = 0;
    for (const char *c = s; *c; c++) {
        if (*c == '(' || *c == '[') d++;
        else if (*c == ')' || *c == ']') d--;
        else if (*c == ',' && !d) return false;
    }
    return sup_selector_one(s, strlen(s), false);
}

/* condition: not <in-parens> | <in-parens> [and <in-parens>]* | <in-parens> [or <in-parens>]* */
static int sup_cond(const Toks *ts, int b, int e, const char *src);

static int sup_in_parens(const Toks *ts, int i, const char *src) {
    const Tok *t = &ts->t[i];
    if (t->type == T_FUNC) {
        if (strcasecmp(t->str, "selector") == 0) {
            /* (its text as written: the next selector( in the condition, in the order they're asked) */
            const char *at = src ? strcasestr_simple(t_sup_from ? t_sup_from : src, "selector(") : NULL;
            if (!at) return 0;
            const char *a = at + 9, *e = a;
            int d = 1;
            for (; *e && d; e++) { if (*e == '(') d++; else if (*e == ')') d--; }
            t_sup_from = e;
            char *txt = strndup(a, (size_t)(e - a - (d ? 0 : 1)));
            bool ok = txt && sup_selector(txt);
            free(txt);
            return ok;
        }
        if (strcasecmp(t->str, "at-rule") == 0) {
            /* (one at-keyword the engine knows) */
            int k = t->first;
            if (k >= t->end || ts->t[k].type != T_AT || tok_next(ts, k) != t->end) return 0;
            static const char *const known[] = { "media", "supports", "import", "namespace", "font-face", "keyframes", "page",
                "counter-style", "layer", "container", "property", "scope", "starting-style", "font-feature-values",
                "font-palette-values", "position-try", "view-transition", "swash", "annotation", "ornaments", "stylistic",
                "styleset", "character-variant", "historical-forms", "-webkit-keyframes" };
            for (size_t q = 0; q < sizeof known / sizeof known[0]; q++) if (!strcasecmp(ts->t[k].str, known[q])) return 1;
            return 0;
        }
        bool ftech = !strcasecmp(t->str, "font-tech"), ffmt = !strcasecmp(t->str, "font-format");
        if (ftech || ffmt) {
            /* (one keyword the font loader handles) */
            int k = t->first;
            if (k >= t->end || ts->t[k].type != T_IDENT || tok_next(ts, k) != t->end) return 0;
            static const char *const tech[] = { "features-opentype", "features-aat", "features-graphite", "color-COLRv0",
                "color-COLRv1", "color-SVG", "color-sbix", "color-CBDT", "variations", "palettes", "incremental" };
            static const char *const fmt[] = { "opentype", "truetype", "woff", "woff2", "collection" };
            const char *const *l = ftech ? tech : fmt;
            size_t nl = ftech ? sizeof tech / sizeof tech[0] : sizeof fmt / sizeof fmt[0];
            for (size_t q = 0; q < nl; q++) if (!strcasecmp(ts->t[k].str, l[q])) return 1;
            return 0;
        }
        return 0;   /* unknown functions */
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
            if (bang) {
                /* (nothing but space after the priority) */
                const char *q = bang + (bang[1] == ' ' ? 11 : 10);
                while (*q == ' ') q++;
                if (*q) { free(vb.s); return 0; }
                *bang = '\0';
                important = true;
            }
        }
        (void)important;
        /* (a '!' or ';' at the value's top level, or a var() fallback's, makes it
         * invalid; inside other blocks and strings it is fine; <!-- is a token) */
        if (vb.s) {
            char qc = 0, stk[64];
            int sp = 0;
            for (const char *p = vb.s; *p; p++) {
                if (qc) { if (*p == '\\' && p[1]) p++; else if (*p == qc) qc = 0; continue; }
                if (*p == '"' || *p == '\'') qc = *p;
                else if (!strncmp(p, "<!--", 4)) p += 3;
                else if (!strncmp(p, "-->", 3)) p += 2;
                else if (*p == '(' || *p == '[' || *p == '{') {
                    bool v = *p == '(' && p - vb.s >= 3 && !strncasecmp(p - 3, "var", 3);
                    if (sp < 64) stk[sp] = v ? 'v' : 'b';
                    sp++;
                }
                else if (*p == ')' || *p == ']' || *p == '}') { if (sp) sp--; }
                else if (((*p == '!' && !sup_cdo_bang(vb.s, p)) || *p == ';') && (!sp || (sp <= 64 && stk[sp - 1] == 'v'))) { free(vb.s); return 0; }
            }
        }
        /* (a custom property may be empty) */
        bool ok = vb.s ? css_supports_property(f->str, vb.s) : css_supports_property(f->str, "");
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
    t_sup_from = NULL;
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
            /* the first box given is the origin, the second the clip (a clip
             * that isn't a box, the clip wherever it is) */
            char *t5 = hit_text(&P, m[5]);
            bool box5 = !strcmp(t5, "border-box") || !strcmp(t5, "padding-box") || !strcmp(t5, "content-box");
            free(t5);
            int a = !box5 ? m[4] : P.m.hits[m[4]].start < P.m.hits[m[5]].start ? m[4] : m[5], b = a == m[4] ? m[5] : m[4];
            org[l] = hit_text(&P, a);
            clip[l] = hit_text(&P, b);
        } else if (m[4] >= 0 || m[5] >= 0) {
            char *one = hit_text(&P, m[4] >= 0 ? m[4] : m[5]);
            /* (a clip that isn't a box: border-area, text: the clip alone) */
            if (!strcmp(one, "border-box") || !strcmp(one, "padding-box") || !strcmp(one, "content-box")) { org[l] = one; clip[l] = strdup(one); }
            /* (border-area: the origin the border box) */
            else { org[l] = strdup(strstr(one, "border-area") ? "border-box" : "padding-box"); clip[l] = one; }
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

/* The values of v separated by spaces, up to cap: brackets, parentheses and
 * strings stay whole. */
static int gap_words_early(const char *v, char **out, int cap)
{
    int n = 0, depth = 0;
    char q = 0;
    const char *st = NULL;
    for (const char *p = v;; p++) {
        if (q && *p) { if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') q = *p;
        if (*p == '(' || *p == '[') depth++;
        if (*p == ')' || *p == ']') depth--;
        bool end = !*p || (!depth && isspace((unsigned char)*p));
        if (!end && !st) st = p;
        if (end && st) {
            if (n < cap) out[n++] = strndup(st, (size_t)(p - st));
            st = NULL;
        }
        if (!*p) break;
    }
    return n;
}

/* ---- grid-template and grid ---- */
static char *prop_parse_raw(const CssPropDef *p, const char *value);
static const char *val_of(const char *const *vals, const char **names, int n, const char *name);
/* s without its empty [] line names, spaces single. */
static char *grid_clean_tracks(const char *s)
{
    Buf b = { 0 };
    char *w[256];
    int n = gap_words_early(s, w, 256);
    for (int i = 0; i < n; i++) {
        bool empty = w[i][0] == '[';
        for (const char *q = w[i] + 1; empty && *q; q++) if (*q != ']' && !isspace((unsigned char)*q)) empty = false;
        if (!empty) {
            if (b.n) buf_ch(&b, ' ');
            buf_str(&b, w[i]);
        }
        free(w[i]);
    }
    buf_ch(&b, 0);
    return b.s;
}
static bool grid_track_ok(const char *v)
{
    if (!strcasecmp(v, "none")) return true;
    const CssPropDef *p = css_prop_find("grid-template-rows", 18);
    char *t = p ? prop_parse_raw(p, v) : NULL;
    bool ok = t != NULL;
    free(t);
    return ok;
}
/* The slash at depth 0 in v, or NULL. */
static const char *grid_slash(const char *v)
{
    int d = 0;
    char q = 0;
    for (const char *p = v; *p; p++) {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '(' || *p == '[') d++;
        else if (*p == ')' || *p == ']') d--;
        else if (*p == '/' && !d) return p;
    }
    return NULL;
}
/* grid-template's value into rows, columns and areas; false when it isn't one. */
static bool grid_template_split(const char *v, char **rows, char **cols, char **areas)
{
    *rows = *cols = *areas = NULL;
    if (!strcasecmp(v, "none")) {
        *rows = strdup("none"); *cols = strdup("none"); *areas = strdup("none");
        return true;
    }
    const char *sl = grid_slash(v);
    bool strings = false;
    {
        char q = 0;
        for (const char *p = v; *p && !strings && (!sl || p < sl); p++) {
            if (*p == '"' || *p == '\'') { strings = true; q = *p; }
        }
        (void)q;
    }
    char *left = sl ? strndup(v, (size_t)(sl - v)) : strdup(v);
    char *right = sl ? strdup(sl + 1) : NULL;
    bool ok = true;
    if (!strings) {
        if (!sl || grid_slash(right)) ok = false;
        if (ok) {
            char *l = grid_clean_tracks(left), *r = grid_clean_tracks(right);
            ok = l[0] && r[0] && grid_track_ok(l) && grid_track_ok(r);
            *rows = l; *cols = r;
            *areas = strdup("none");
        }
        free(left); free(right);
        return ok;
    }
    /* [names]? "string" track? [names]?, repeated */
    Buf rb = { 0 }, ab = { 0 };
    char *w[256];
    int n = gap_words_early(left, w, 256);
    int cols_in_row = -1;
    for (int i = 0; i < n && ok;) {
        bool lead = false;
        if (w[i][0] == '[') {
            char *cn = grid_clean_tracks(w[i]);
            if (cn[0]) {
                /* (after the last row's names: one set of them) */
                if (rb.n && rb.s[rb.n - 1] == ']' && cn[0] == '[') {
                    rb.n--;
                    rb.s[rb.n] = 0;
                    buf_ch(&rb, ' ');
                    buf_str(&rb, cn + 1);
                } else { if (rb.n) buf_ch(&rb, ' '); buf_str(&rb, cn); }
                lead = true;
            }
            free(cn);
            i++;
        }
        (void)lead;
        if (i >= n || (w[i][0] != '"' && w[i][0] != '\'')) { ok = false; break; }
        /* (each row of the template as long as the others) */
        {
            int cnt = 0;
            bool in = false;
            for (const char *q = w[i] + 1; *q && q[1]; q++) {
                bool sp = isspace((unsigned char)*q);
                if (!sp && !in) cnt++;
                in = !sp;
            }
            if (cols_in_row >= 0 && cnt != cols_in_row) ok = false;
            if (!cnt) ok = false;
            cols_in_row = cnt;
        }
        if (ab.n) buf_ch(&ab, ' ');
        buf_str(&ab, w[i]);
        i++;
        if (rb.n) buf_ch(&rb, ' ');
        if (i < n && w[i][0] != '[' && w[i][0] != '"' && w[i][0] != '\'') {
            if (!strcasecmp(w[i], "none")) ok = false;
            buf_str(&rb, w[i]);
            i++;
        } else {
            buf_str(&rb, "auto");
        }
        if (i < n && w[i][0] == '[') {
            char *cn = grid_clean_tracks(w[i]);
            if (cn[0]) { buf_ch(&rb, ' '); buf_str(&rb, cn); }
            free(cn);
            i++;
        }
    }
    for (int i = 0; i < n; i++) free(w[i]);
    buf_ch(&rb, 0);
    buf_ch(&ab, 0);
    if (ok) ok = grid_track_ok(rb.s) && !strstr(rb.s, "repeat(");
    char *c = right ? grid_clean_tracks(right) : strdup("none");
    /* (a bare 0 track: 0px) */
    if (c && !strcmp(c, "0")) { free(c); c = strdup("0px"); }
    if (ok && right) ok = c[0] && strcasecmp(c, "none") && grid_track_ok(c) && !strstr(c, "repeat(auto") && !strstr(c, "subgrid");
    *rows = rb.s; *areas = ab.s; *cols = c;
    free(left); free(right);
    return ok;
}
static int expand_grid(const char *nm, const char *v, char **out, const char **names, int n)
{
    char *rows = NULL, *cols = NULL, *areas = NULL;
    char *arows = NULL, *acols = NULL, *flow = NULL;
    bool ok = false;
    if (!strcmp(nm, "grid") && strstr(v, "auto-flow")) {
        const char *sl = grid_slash(v);
        char *left = sl ? strndup(v, (size_t)(sl - v)) : strdup(v);
        char *right = sl ? strdup(sl + 1) : strdup("");
        bool in_left = strstr(left, "auto-flow") != NULL;
        const char *f = in_left ? left : right;
        char *w[32];
        int cnt = gap_words_early(f, w, 32);
        Buf sz = { 0 };
        bool dense = false, flowed = false;
        ok = !(in_left && !sl);
        for (int i = 0; i < cnt; i++) {
            if (!strcasecmp(w[i], "auto-flow") && !flowed) flowed = true;
            else if (!strcasecmp(w[i], "dense") && !dense) dense = true;
            else { if (sz.n) buf_ch(&sz, ' '); buf_str(&sz, w[i]); }
            free(w[i]);
        }
        buf_ch(&sz, 0);
        const char *other = in_left ? right : left;
        char *ot = grid_clean_tracks(other);
        if (ok) ok = ot[0] && grid_track_ok(ot) && !grid_slash(in_left ? right : left) && (!in_left || !strstr(right, "auto-flow"));
        const CssPropDef *ap = css_prop_find("grid-auto-rows", 14);
        if (ok && sz.s[0]) { char *t = ap ? prop_parse_raw(ap, sz.s) : NULL; if (!t) ok = false; free(t); }
        if (ok) {
            rows = strdup(in_left ? "none" : ot);
            cols = strdup(in_left ? ot : "none");
            areas = strdup("none");
            arows = strdup(in_left && sz.s[0] ? sz.s : "auto");
            acols = strdup(!in_left && sz.s[0] ? sz.s : "auto");
            char fb[32];
            snprintf(fb, sizeof fb, "%s%s", in_left ? "row" : "column", dense ? " dense" : "");
            flow = strdup(fb);
        }
        free(ot); free(sz.s); free(left); free(right);
    } else {
        ok = grid_template_split(v, &rows, &cols, &areas);
        arows = strdup("auto"); acols = strdup("auto"); flow = strdup("row");
    }
    if (!ok) {
        free(rows); free(cols); free(areas); free(arows); free(acols); free(flow);
        return -1;
    }
    struct { const char *k; char *v; } m[] = { { "grid-template-rows", rows }, { "grid-template-columns", cols }, { "grid-template-areas", areas },
                                               { "grid-auto-rows", arows }, { "grid-auto-columns", acols }, { "grid-auto-flow", flow } };
    for (size_t k = 0; k < 6; k++) {
        int i = slot(names, n, m[k].k);
        if (i >= 0) out[i] = strdup(m[k].v);
        free(m[k].v);
    }
    return n;
}
/* corner and its sides: radius and shape, per corner, by slashes */
static bool corner_item(const char *t, char *radius, size_t rc, char *shape, size_t sc)
{
    const CssPropDef *rp = css_prop_find("border-top-left-radius", 22), *sp = css_prop_find("corner-top-left-shape", 21);
    char r[256] = "", s[128] = "";
    /* (normal: nothing rounded, in the round) */
    while (isspace((unsigned char)*t)) t++;
    size_t tl = strlen(t);
    while (tl && isspace((unsigned char)t[tl - 1])) tl--;
    if (tl == 6 && !strncasecmp(t, "normal", 6)) { snprintf(radius, rc, "0px"); snprintf(shape, sc, "round"); return true; }
    const char *p = t, *end = t + tl;
    while (p < end) {
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end) break;
        const char *q = p;
        int d = 0;
        while (q < end && (d || !isspace((unsigned char)*q))) { if (*q == '(') d++; else if (*q == ')') d--; q++; }
        char w[128];
        size_t l = (size_t)(q - p);
        if (l >= sizeof w) return false;
        memcpy(w, p, l);
        w[l] = 0;
        char *as_s = sp ? prop_parse_raw(sp, w) : NULL;
        if (as_s) {
            if (s[0]) { free(as_s); return false; }
            snprintf(s, sizeof s, "%s", as_s);
            free(as_s);
        } else {
            /* (the radii stay together) */
            if (r[0] && strchr(r, ' ')) return false;
            size_t have = strlen(r);
            snprintf(r + have, sizeof r - have, "%s%s", have ? " " : "", w);
        }
        p = q;
    }
    /* (it takes both) */
    if (!r[0] || !s[0]) return false;
    {
        char *ok = rp ? prop_parse_raw(rp, r) : NULL;
        if (!ok) return false;
        snprintf(radius, rc, "%s", ok);
        free(ok);
    }
    snprintf(shape, sc, "%s", s[0] ? s : "round");
    return true;
}

static int corner_count(const char *nm)
{
    static const struct { const char *n; int c; } k[] = {
        { "corner", 4 },
        { "corner-top", 2 },
        { "corner-bottom", 2 },
        { "corner-left", 2 },
        { "corner-right", 2 },
        { "corner-block-start", 2 },
        { "corner-block-end", 2 },
        { "corner-inline-start", 2 },
        { "corner-inline-end", 2 },
        { "corner-top-left", 1 },
        { "corner-top-right", 1 },
        { "corner-bottom-left", 1 },
        { "corner-bottom-right", 1 },
        { "corner-start-start", 1 },
        { "corner-start-end", 1 },
        { "corner-end-start", 1 },
        { "corner-end-end", 1 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(k[i].n, nm)) return k[i].c;
    return 0;
}

static int expand_corner(const char *nm, const char *v, char **out)
{
    int want = corner_count(nm);
    char item[4][256];
    int n = 0;
    const char *p = v;
    for (;;) {
        const char *q = p;
        int d = 0;
        while (*q && (d || *q != '/')) { if (*q == '(') d++; else if (*q == ')') d--; q++; }
        if (n == 4 || (size_t)(q - p) >= sizeof item[0]) return -1;
        memcpy(item[n], p, (size_t)(q - p));
        item[n][q - p] = 0;
        n++;
        if (!*q) break;
        p = q + 1;
    }
    if (n > want) return -1;
    static const int fill[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
    for (int i = 0; i < want; i++) {
        char r[256], s[128];
        int k = want == 4 ? fill[n - 1][i] : (i < n ? i : 0);
        if (!corner_item(item[k], r, sizeof r, s, sizeof s)) return -1;
        set_out(out, i * 2, r);
        set_out(out, i * 2 + 1, s);
    }
    return 0;
}

static void corner_text(const char *r, const char *s, char *o, size_t cap)
{
    if (!strcmp(r, "0px") && !strcmp(s, "round")) snprintf(o, cap, "normal");
    else snprintf(o, cap, "%s %s", r, s);
}

static int serialize_corner(const char *nm, const char *const *vals, int n, Buf *b)
{
    int want = corner_count(nm);
    if (n != want * 2) return -1;
    char t[4][400];
    for (int i = 0; i < want; i++) {
        if (!vals[i * 2] || !vals[i * 2 + 1]) return -1;
        corner_text(vals[i * 2], vals[i * 2 + 1], t[i], sizeof t[i]);
    }
    int show = want;
    if (want == 4) {
        if (!strcmp(t[3], t[1])) { show = 3; if (!strcmp(t[2], t[0])) { show = 2; if (!strcmp(t[1], t[0])) show = 1; } }
    } else if (want == 2 && !strcmp(t[0], t[1])) show = 1;
    for (int i = 0; i < show; i++) {
        if (i) buf_str(b, " / ");
        buf_str(b, t[i]);
    }
    return 0;
}

/* border-top-radius and the like: two corners' radii. */
static int expand_side_radius(const char *v, char **out)
{
    const CssPropDef *rp = css_prop_find("border-top-left-radius", 22);
    const char *sl = strchr(v, '/');
    char h[2][96] = { "", "" }, w[2][96] = { "", "" }, ex[8] = "";
    char left[256], right[256] = "";
    size_t ll = sl ? (size_t)(sl - v) : strlen(v);
    if (ll >= sizeof left) return -1;
    memcpy(left, v, ll);
    left[ll] = 0;
    if (sl) snprintf(right, sizeof right, "%s", sl + 1);
    int nh = sscanf(left, "%95s %95s %7s", h[0], h[1], ex);
    if (nh < 1 || nh > 2) return -1;
    int nv = sl ? sscanf(right, "%95s %95s %7s", w[0], w[1], ex) : 0;
    if (sl && (nv < 1 || nv > 2)) return -1;
    if (nh == 1) strcpy(h[1], h[0]);
    if (nv == 1) strcpy(w[1], w[0]);
    if (!nv) { strcpy(w[0], h[0]); strcpy(w[1], h[1]); }
    for (int i = 0; i < 2; i++) {
        char t[200];
        if (!strcmp(h[i], w[i])) snprintf(t, sizeof t, "%s", h[i]);
        else snprintf(t, sizeof t, "%s %s", h[i], w[i]);
        char *ok = rp ? prop_parse_raw(rp, t) : NULL;
        if (!ok) return -1;
        set_out(out, i, ok);
        free(ok);
    }
    return 0;
}

static int serialize_side_radius(const char *const *vals, int n, Buf *b)
{
    if (n != 2 || !vals[0] || !vals[1]) return -1;
    char h[2][96], w[2][96];
    for (int i = 0; i < 2; i++) {
        int c = sscanf(vals[i], "%95s %95s", h[i], w[i]);
        if (c < 1) return -1;
        if (c == 1) strcpy(w[i], h[i]);
    }
    buf_str(b, h[0]);
    if (strcmp(h[0], h[1])) { buf_ch(b, ' '); buf_str(b, h[1]); }
    if (strcmp(h[0], w[0]) || strcmp(h[1], w[1])) {
        buf_str(b, " / ");
        buf_str(b, w[0]);
        if (strcmp(w[0], w[1])) { buf_ch(b, ' '); buf_str(b, w[1]); }
    }
    return 0;
}

/* grid-lanes: its words in any order; a string, the direction, the rest the tracks. */
static int expand_grid_lanes(const char *v, char **out)
{
    size_t vl = strlen(v);
    char *tracks = calloc(vl + 2, 1);
    const char *str = NULL;
    size_t strl = 0;
    bool row = false, col = false, normal = false, fr = false, tr = false, bad = false;
    const char *p = v;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *s = p;
        int d = 0;
        char q = 0;
        for (; *p; p++) {
            if (q) { if (*p == q) q = 0; continue; }
            if (*p == '"' || *p == '\'') q = *p;
            else if (*p == '(' || *p == '[') d++;
            else if (*p == ')' || *p == ']') d--;
            else if (isspace((unsigned char)*p) && !d) break;
        }
        size_t l = (size_t)(p - s);
        #define IS(w) (l == strlen(w) && !strncasecmp(s, w, l))
        if (*s == '"' || *s == '\'') { if (str) bad = true; str = s; strl = l; }
        else if (IS("row")) { if (row) bad = true; row = true; }
        else if (IS("column")) { if (col) bad = true; col = true; }
        else if (IS("normal")) { if (normal) bad = true; normal = true; }
        else if (IS("fill-reverse")) { if (fr) bad = true; fr = true; }
        else if (IS("track-reverse")) { if (tr) bad = true; tr = true; }
        else {
            if (tracks[0]) strcat(tracks, " ");
            strncat(tracks, s, l);
        }
        #undef IS
    }
    if ((row && col) || (normal && (row || col || fr || tr))) bad = true;
    const CssPropDef *tp = css_prop_find("grid-template-columns", 21), *ap = css_prop_find("grid-template-areas", 19);
    char *tv = NULL, *av = NULL;
    if (!bad && tracks[0]) {
        tv = tp ? prop_parse_raw(tp, tracks) : NULL;
        if (!tv) bad = true;
    }
    if (!bad && str) {
        char *t = strndup(str, strl);
        av = ap ? prop_parse_raw(ap, t) : NULL;
        free(t);
        if (!av) bad = true;
    }
    if (bad || (!tracks[0] && !str && !row && !col && !normal && !fr && !tr)) {
        free(tracks); free(tv); free(av);
        return -1;
    }
    /* (going in rows: its names one to a row) */
    if (row && av && strcmp(av, "none")) {
        Buf nb = {0};
        bool in = false;
        for (const char *a = av; *a; a++) {
            if (*a == '"') continue;
            if (isspace((unsigned char)*a)) { if (in) buf_str(&nb, "\" "); in = false; continue; }
            if (!in) { if (nb.n && nb.s[nb.n - 1] != ' ') buf_str(&nb, " "); buf_str(&nb, "\""); in = true; }
            char one[2] = { *a, 0 };
            buf_str(&nb, one);
        }
        if (in) buf_str(&nb, "\"");
        free(av);
        av = nb.s ? strndup(nb.s, nb.n) : strdup("none");
        free(nb.s);
    }
    char dir[48];
    snprintf(dir, sizeof dir, "%s%s%s", normal ? "normal" : row ? "row" : "column", fr ? " fill-reverse" : "", tr ? " track-reverse" : "");
    set_out(out, 0, row && tv ? tv : "none");
    set_out(out, 1, !row && tv ? tv : "none");
    set_out(out, 2, av ? av : "none");
    set_out(out, 3, dir);
    free(tracks); free(tv); free(av);
    return 0;
}

static int serialize_grid_lanes(const char *const *vals, const char **names, int n, Buf *b)
{
    const char *rows = val_of(vals, names, n, "grid-template-rows"), *cols = val_of(vals, names, n, "grid-template-columns");
    const char *areas = val_of(vals, names, n, "grid-template-areas"), *dir = val_of(vals, names, n, "grid-lanes-direction");
    if (!rows || !cols || !areas || !dir) return -1;
    bool row = !strncmp(dir, "row", 3);
    const char *tracks = row ? rows : cols;
    bool any = false;
    if (strcmp(areas, "none")) {
        /* (one string: the rows' names, in rows, run together) */
        int strs = 0, words = 0;
        bool in = false;
        for (const char *a = areas; *a; a++) if (*a == '"') { strs++; in = !in; }
        strs /= 2;
        in = false;
        for (const char *a = areas; *a; a++) {
            bool w = *a != '"' && !isspace((unsigned char)*a);
            if (w && !in) words++;
            in = w;
        }
        if (row ? strs != words : strs != 1) return -1;
        buf_str(b, "\"");
        bool first = true;
        in = false;
        for (const char *a = areas; *a; a++) {
            bool w = *a != '"' && !isspace((unsigned char)*a);
            if (w) {
                if (!in && !first) buf_str(b, " ");
                char one[2] = { *a, 0 };
                buf_str(b, one);
                first = false;
            }
            in = w;
        }
        buf_str(b, "\"");
        any = true;
    }
    if (strcmp(tracks, "none")) {
        if (any) buf_str(b, " ");
        buf_str(b, tracks);
        any = true;
    }
    if (any) buf_str(b, " ");
    buf_str(b, dir);
    return 0;
}

static int serialize_grid(const char *nm, const char *const *vals, const char **names, int n, Buf *b)
{
    const char *rows = val_of(vals, names, n, "grid-template-rows"), *cols = val_of(vals, names, n, "grid-template-columns");
    const char *areas = val_of(vals, names, n, "grid-template-areas");
    if (!rows || !cols || !areas) return -1;
    if (!strcmp(nm, "grid")) {
        const char *ar = val_of(vals, names, n, "grid-auto-rows"), *ac = val_of(vals, names, n, "grid-auto-columns"), *fl = val_of(vals, names, n, "grid-auto-flow");
        bool plain = ar && ac && fl && !strcmp(ar, "auto") && !strcmp(ac, "auto") && !strcmp(fl, "row");
        if (!plain) {
            if (strcmp(areas, "none") || !ar || !ac || !fl) return -1;
            bool column = !strncmp(fl, "column", 6), dense = strstr(fl, "dense") != NULL;
            if (column) {
                if (strcmp(cols, "none") || strcmp(ar, "auto")) return -1;
                buf_str(b, rows); buf_str(b, " / auto-flow");
                if (dense) buf_str(b, " dense");
                if (strcmp(ac, "auto")) { buf_ch(b, ' '); buf_str(b, ac); }
            } else {
                if (strcmp(rows, "none") || strcmp(ac, "auto")) return -1;
                buf_str(b, "auto-flow");
                if (dense) buf_str(b, " dense");
                if (strcmp(ar, "auto")) { buf_ch(b, ' '); buf_str(b, ar); }
                buf_str(b, " / "); buf_str(b, cols);
            }
            return 0;
        }
    }
    if (!strcmp(areas, "none")) {
        if (!strcmp(rows, "none") && !strcmp(cols, "none")) { buf_str(b, "none"); return 0; }
        buf_str(b, rows); buf_str(b, " / "); buf_str(b, cols);
        return 0;
    }
    /* with areas: each string by its row */
    if (strstr(rows, "repeat(") || strstr(cols, "repeat(")) return -1;
    char *as[256], *rs[256];
    int na = gap_words_early(areas, as, 256), nr = gap_words_early(rows, rs, 256), ri = 0;
    int rc = 0;
    for (int i = 0; i < na && !rc; i++) {
        if (i) buf_ch(b, ' ');
        if (ri < nr && rs[ri][0] == '[') { buf_str(b, rs[ri++]); buf_ch(b, ' '); }
        buf_str(b, as[i]);
        if (ri >= nr || rs[ri][0] == '[') { rc = -1; break; }
        if (strcmp(rs[ri], "auto")) { buf_ch(b, ' '); buf_str(b, rs[ri]); }
        ri++;
        if (ri < nr && rs[ri][0] == '[') { buf_ch(b, ' '); buf_str(b, rs[ri++]); }
    }
    if (ri != nr) rc = -1;
    if (!rc && strcmp(cols, "none")) { buf_str(b, " / "); buf_str(b, cols); }
    for (int i = 0; i < na; i++) free(as[i]);
    for (int i = 0; i < nr; i++) free(rs[i]);
    return rc;
}

/* ---- gap decorations ---- */
/* Shorthands giving every longhand the one value. */
static bool gap_same(const char *nm)
{
    static const char *const k[] = { "rule-color", "rule-style", "rule-width", "rule-break", "rule-visibility-items", "column-rule-inset-start",
        "column-rule-inset-end", "row-rule-inset-start", "row-rule-inset-end", "rule-inset-start", "rule-inset-end" };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(nm, k[i])) return true;
    return false;
}
/* start end pairs, of one or both directions */
static bool gap_pairs(const char *nm)
{
    static const char *const k[] = { "corner-top-shape", "corner-bottom-shape", "corner-left-shape", "corner-right-shape",
        "corner-block-start-shape", "corner-block-end-shape", "corner-inline-start-shape", "corner-inline-end-shape", "column-rule-inset-cap", "column-rule-inset-junction", "row-rule-inset-cap",
        "row-rule-inset-junction", "rule-inset-cap", "rule-inset-junction" };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!strcmp(nm, k[i])) return true;
    return false;
}
static bool gap_insets(const char *nm) { return !strcmp(nm, "column-rule-inset") || !strcmp(nm, "row-rule-inset") || !strcmp(nm, "rule-inset"); }
static bool gap_rule(const char *nm) { return !strcmp(nm, "column-rule") || !strcmp(nm, "row-rule") || !strcmp(nm, "rule"); }

/* The space-separated values of v, up to cap. */
static int gap_words(const char *v, char **out, int cap)
{
    int n = 0, depth = 0;
    const char *st = NULL;
    for (const char *p = v;; p++) {
        if (*p == '(') depth++;
        if (*p == ')') depth--;
        bool end = !*p || (!depth && isspace((unsigned char)*p));
        if (!end && !st) st = p;
        if (end && st) {
            if (n < cap) out[n++] = strndup(st, (size_t)(p - st));
            st = NULL;
        }
        if (!*p) break;
    }
    return n;
}

/* One <gap-rule> into its three channels. */
static bool gap_rule_parts(const char *v, char *ch[3])
{
    Parsed P;
    if (!parse_type("gap-rule", v, &P)) { parsed_free(&P); return false; }
    int m[3];
    members(&P, 0, m, 3);
    ch[0] = m[0] >= 0 ? hit_text(&P, m[0]) : strdup("medium");
    ch[1] = m[1] >= 0 ? hit_text(&P, m[1]) : strdup("none");
    ch[2] = m[2] >= 0 ? hit_text(&P, m[2]) : strdup("currentcolor");
    parsed_free(&P);
    return true;
}

static const char *gap_zero(const char *w) { return !strcmp(w, "0") ? "0px" : w; }

static int expand_gap(const char *nm, const char *v, char **out, const char **names, int n)
{
    (void)names;
    /* (corner-shape: as a box's sides, top left going round) */
    if (!strcmp(nm, "corner-shape")) {
        char *w[5] = { NULL, NULL, NULL, NULL, NULL };
        int c = gap_words(v, w, 5);
        Parsed Q4;
        bool ok4 = c >= 1 && c <= 4;
        for (int i = 0; i < c && i < 4 && ok4; i++) {
            ok4 = parse_type("corner-shape-value", w[i], &Q4);
            parsed_free(&Q4);
        }
        if (ok4) {
            const char *a = w[0], *b = c > 1 ? w[1] : a, *d = c > 2 ? w[2] : a, *e = c > 3 ? w[3] : b;
            out[0] = strdup(a); out[1] = strdup(b); out[2] = strdup(d); out[3] = strdup(e);
        }
        for (int i = 0; i < 5; i++) free(w[i]);
        return ok4 ? n : -1;
    }
    const char *ty = gap_rule(nm) ? "gap-rule-list" : gap_insets(nm) ? "gap-insets" : !strncmp(nm, "corner-", 7) ? "corner-shape-pair" : gap_pairs(nm) ? "gap-inset-pair" :
                     !strcmp(nm, "rule-color") ? "gap-color-list" : !strcmp(nm, "rule-style") ? "gap-style-list" :
                     !strcmp(nm, "rule-width") ? "gap-width-list" : !strcmp(nm, "rule-break") ? "gap-break" :
                     !strcmp(nm, "rule-visibility-items") ? "gap-visibility" : "gap-inset";
    Parsed Q;
    bool fits = parse_type(ty, v, &Q);
    parsed_free(&Q);
    if (!fits) return -1;
    if (gap_same(nm)) {
        for (int i = 0; i < n; i++) out[i] = strdup(gap_zero(v));
        return n;
    }
    if (gap_pairs(nm)) {
        char *w[2] = { NULL, NULL };
        int c = gap_words(v, w, 2);
        if (c < 1) return -1;
        for (int i = 0; i < n; i++) out[i] = strdup(gap_zero(i % 2 && c > 1 ? w[1] : w[0]));
        free(w[0]); free(w[1]);
        return n;
    }
    if (gap_insets(nm)) {
        char *left = strdup(v), *right = NULL, *slash = strchr(left, '/');
        int depth = 0;
        slash = NULL;
        for (char *q = left; *q; q++) {
            if (*q == '(') depth++;
            if (*q == ')') depth--;
            if (*q == '/' && !depth) { slash = q; break; }
        }
        if (slash) { *slash = 0; right = slash + 1; }
        char *a[2] = { NULL, NULL }, *b[2] = { NULL, NULL };
        int ca = gap_words(left, a, 2), cb = right ? gap_words(right, b, 2) : 0;
        if (ca < 1) { free(left); return -1; }
        const char *vals4[4] = { a[0], ca > 1 ? a[1] : a[0], cb ? b[0] : a[0], cb ? (cb > 1 ? b[1] : b[0]) : (ca > 1 ? a[1] : a[0]) };
        for (int i = 0; i < n; i++) out[i] = strdup(gap_zero(vals4[i % 4]));
        free(a[0]); free(a[1]); free(b[0]); free(b[1]); free(left);
        return n;
    }
    /* column-rule, row-rule, rule: each item's channels; repeat()s stay around them */
    char **items;
    int ni = split_commas(v, &items);
    Buf ch[3] = { { 0 }, { 0 }, { 0 } };
    bool ok = ni > 0;
    for (int i = 0; i < ni && ok; i++) {
        for (int c = 0; c < 3; c++) if (i) buf_str(&ch[c], ", ");
        const char *it = items[i];
        if (!strncasecmp(it, "repeat(", 7)) {
            size_t len = strlen(it);
            char *inner = strndup(it + 7, len - 8);
            char **rp;
            int nr = split_commas(inner, &rp);
            ok = nr >= 2;
            for (int c = 0; c < 3 && ok; c++) { buf_str(&ch[c], "repeat("); buf_str(&ch[c], rp[0]); }
            for (int r = 1; r < nr && ok; r++) {
                char *pt[3];
                ok = gap_rule_parts(rp[r], pt);
                for (int c = 0; c < 3 && ok; c++) { buf_str(&ch[c], ", "); buf_str(&ch[c], pt[c]); free(pt[c]); }
            }
            for (int c = 0; c < 3 && ok; c++) buf_ch(&ch[c], ')');
            free_parts(rp, nr);
            free(inner);
        } else {
            char *pt[3];
            ok = gap_rule_parts(it, pt);
            for (int c = 0; c < 3 && ok; c++) { buf_str(&ch[c], pt[c]); free(pt[c]); }
        }
    }
    free_parts(items, ni);
    if (!ok) { for (int c = 0; c < 3; c++) free(ch[c].s); return -1; }
    for (int c = 0; c < 3; c++) buf_ch(&ch[c], 0);
    for (int i = 0; i < n; i++) out[i] = strdup(ch[i % 3].s);
    for (int c = 0; c < 3; c++) free(ch[c].s);
    return n;
}

/* (a repeat()'s inside: its count and items) */
static bool gap_split_repeat(const char *it, char ***rp, int *nr)
{
    if (strncasecmp(it, "repeat(", 7)) return false;
    char *inner = strndup(it + 7, strlen(it) - 8);
    *nr = split_commas(inner, rp);
    free(inner);
    return true;
}

static void gap_rule_text(Buf *b, const char *w, const char *s, const char *c)
{
    bool sp = false;
    if (strcmp(w, "medium")) { buf_str(b, w); sp = true; }
    if (strcmp(s, "none")) { if (sp) buf_ch(b, ' '); buf_str(b, s); sp = true; }
    if (strcmp(c, "currentcolor")) { if (sp) buf_ch(b, ' '); buf_str(b, c); sp = true; }
    if (!sp) buf_str(b, "medium");
}

static int serialize_gap(const char *nm, const char *const *vals, int n, Buf *b)
{
    if (!strcmp(nm, "corner-shape")) {
        const char *x = vals[0], *y = vals[1], *z = vals[2], *t = vals[3];
        buf_str(b, x);
        if (strcmp(y, t)) { buf_ch(b, ' '); buf_str(b, y); buf_ch(b, ' '); buf_str(b, z); buf_ch(b, ' '); buf_str(b, t); }
        else if (strcmp(x, z)) { buf_ch(b, ' '); buf_str(b, y); buf_ch(b, ' '); buf_str(b, z); }
        else if (strcmp(x, y)) { buf_ch(b, ' '); buf_str(b, y); }
        return 0;
    }
    if (gap_same(nm)) {
        for (int i = 1; i < n; i++) if (strcmp(vals[i], vals[0])) return -1;
        buf_str(b, vals[0]);
        return 0;
    }
    if (gap_pairs(nm)) {
        for (int i = 2; i < n; i++) if (strcmp(vals[i], vals[i - 2])) return -1;
        buf_str(b, vals[0]);
        if (strcmp(vals[0], vals[1])) { buf_ch(b, ' '); buf_str(b, vals[1]); }
        return 0;
    }
    if (gap_insets(nm)) {
        for (int i = 4; i < n; i++) if (strcmp(vals[i], vals[i - 4])) return -1;
        if (!strcmp(vals[0], vals[1]) && !strcmp(vals[0], vals[2]) && !strcmp(vals[0], vals[3])) { buf_str(b, vals[0]); return 0; }
        buf_str(b, vals[0]); buf_ch(b, ' '); buf_str(b, vals[1]); buf_str(b, " / ");
        buf_str(b, vals[2]); buf_ch(b, ' '); buf_str(b, vals[3]);
        return 0;
    }
    for (int i = 3; i < n; i++) if (strcmp(vals[i], vals[i - 3])) return -1;
    char **col[3];
    int len[3];
    for (int c = 0; c < 3; c++) len[c] = split_commas(vals[c], &col[c]);
    int rc = len[0] == len[1] && len[0] == len[2] ? 0 : -1;
    size_t mark = b->n;
    for (int i = 0; i < len[0] && !rc; i++) {
        if (i) buf_str(b, ", ");
        char **rp[3];
        int nr[3];
        int reps = 0;
        for (int c = 0; c < 3; c++) if (gap_split_repeat(col[c][i], &rp[c], &nr[c])) reps |= 1 << c;
        if (reps == 7 && nr[0] == nr[1] && nr[0] == nr[2] && !strcasecmp(rp[0][0], rp[1][0]) && !strcasecmp(rp[0][0], rp[2][0])) {
            buf_str(b, "repeat("); buf_str(b, rp[0][0]);
            for (int r = 1; r < nr[0]; r++) { buf_str(b, ", "); gap_rule_text(b, rp[0][r], rp[1][r], rp[2][r]); }
            buf_ch(b, ')');
        } else if (!reps) {
            gap_rule_text(b, col[0][i], col[1][i], col[2][i]);
        } else {
            rc = -1;
        }
        for (int c = 0; c < 3; c++) if (reps & (1 << c)) free_parts(rp[c], nr[c]);
    }
    if (rc) b->n = mark;
    for (int c = 0; c < 3; c++) free_parts(col[c], len[c]);
    return rc;
}

/* mask: its layers into the longhands' lists; it resets mask-border. */
static int expand_mask(const char *v, char **out, const char **names, int n)
{
    Parsed P;
    if (!parse_type("mask-syntax", v, &P)) { parsed_free(&P); return -1; }
    int layers[64];
    int nl = find_all_named(&P, 0, "mask-layer", layers, 64);
    if (nl <= 0) { parsed_free(&P); return -1; }
    enum { IMG, POS, SIZE, REP, ORG, CLIP, COMP, MODE, NCOL };
    char **col[NCOL];
    for (int k = 0; k < NCOL; k++) col[k] = calloc((size_t)nl, sizeof(char *));
    for (int l = 0; l < nl; l++) {
        int m[7];
        members(&P, layers[l], m, 7);
        col[IMG][l] = m[0] >= 0 ? hit_text(&P, m[0]) : strdup("none");
        int pos = m[1] >= 0 ? find_named(&P, m[1], "position") : -1, sz = m[1] >= 0 ? find_named(&P, m[1], "bg-size") : -1;
        col[POS][l] = pos >= 0 ? hit_text(&P, pos) : strdup("0% 0%");
        col[SIZE][l] = sz >= 0 ? hit_text(&P, sz) : strdup("auto");
        col[REP][l] = m[2] >= 0 ? hit_text(&P, m[2]) : strdup("repeat");
        if (m[3] >= 0 && m[4] >= 0) {
            /* (no-clip is the clip; of two boxes, the first the origin) */
            char *a = hit_text(&P, P.m.hits[m[3]].start < P.m.hits[m[4]].start ? m[3] : m[4]);
            char *b = hit_text(&P, P.m.hits[m[3]].start < P.m.hits[m[4]].start ? m[4] : m[3]);
            if (!strcmp(a, "no-clip")) { char *t = a; a = b; b = t; }
            col[ORG][l] = a;
            col[CLIP][l] = b;
        } else if (m[3] >= 0 || m[4] >= 0) {
            char *a = hit_text(&P, m[3] >= 0 ? m[3] : m[4]);
            col[ORG][l] = strdup(strcmp(a, "no-clip") ? a : "border-box");
            col[CLIP][l] = a;
        } else {
            col[ORG][l] = strdup("border-box");
            col[CLIP][l] = strdup("border-box");
        }
        col[COMP][l] = m[5] >= 0 ? hit_text(&P, m[5]) : strdup("add");
        col[MODE][l] = m[6] >= 0 ? hit_text(&P, m[6]) : strdup("match-source");
    }
    static const char *const lnames[NCOL] = { "mask-image", "mask-position", "mask-size", "mask-repeat",
                                              "mask-origin", "mask-clip", "mask-composite", "mask-mode" };
    for (int k = 0; k < NCOL; k++) {
        int i = slot(names, n, lnames[k]);
        if (i >= 0) out[i] = join(col[k], nl, ", ");
        free_parts(col[k], nl);
    }
    static const struct { const char *name, *init; } reset[] = {
        { "mask-border-source", "none" }, { "mask-border-outset", "0" }, { "mask-border-repeat", "stretch" },
        { "mask-border-slice", "0" }, { "mask-border-width", "auto" },
    };
    for (size_t k = 0; k < sizeof reset / sizeof reset[0]; k++) {
        int i = slot(names, n, reset[k].name);
        if (i >= 0) out[i] = strdup(reset[k].init);
    }
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
    static const char *const sys[] = { "caption", "icon", "menu", "message-box", "small-caption", "status-bar" };
    for (size_t k = 0; k < sizeof sys / sizeof sys[0]; k++) {
        if (strcasecmp(v, sys[k])) continue;
        for (int i = 0; i < n; i++) {
            if (!strcmp(names[i], "font-size")) out[i] = strdup("13.3333px");
            else if (!strcmp(names[i], "font-family")) out[i] = strdup("system-ui");
            else out[i] = initial_of(names[i]);
        }
        return n;
    }
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
    /* (the alternates' functions, as written: to font-variant-alternates) */
    for (const char *p = v; *p; p++) {
        static const char *const fns[] = { "stylistic(", "styleset(", "character-variant(", "swash(", "ornaments(", "annotation(" };
        if (p > v && !isspace((unsigned char)p[-1])) continue;
        for (int k = 0; k < 6; k++) {
            size_t fl = strlen(fns[k]);
            if (strncasecmp(p, fns[k], fl)) continue;
            const char *e = strchr(p + fl, ')');
            if (!e) break;
            if (acc[1].n) buf_ch(&acc[1], ' ');
            buf_add(&acc[1], p, (size_t)(e + 1 - p));
            p = e;
            break;
        }
    }
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
        bool on = false, oblique = false;
        for (int i = 0; i < P.ts.n; i++) {
            if (tok_ident(&P.ts.t[i], kw[k])) on = true;
            if (k == 1 && tok_ident(&P.ts.t[i], "oblique-only")) oblique = true;
        }
        char name[64];
        snprintf(name, sizeof name, "font-synthesis-%s", kw[k]);
        int i = slot(names, n, name);
        if (i >= 0) out[i] = strdup(oblique ? "oblique-only" : on ? "auto" : "none");
    }
    parsed_free(&P);
    return n;
}

/* One side of an animation-range at *pos in its words: normal, a length, or a
 * range's name and maybe a length. */
static char *range_side(char (*w)[128], int nw, int *pos, const char *longhand, const char *dflt_at) {
    int i = *pos;
    if (i >= nw) return NULL;
    bool len0 = isdigit((unsigned char)w[i][0]) || w[i][0] == '.' || w[i][0] == '-' || w[i][0] == '+' || strchr(w[i], '(');
    char piece[260];
    if (!len0 && strcasecmp(w[i], "normal") && i + 1 < nw &&
        (isdigit((unsigned char)w[i + 1][0]) || w[i + 1][0] == '.' || w[i + 1][0] == '-' || w[i + 1][0] == '+' || strchr(w[i + 1], '('))) {
        /* (the side's own end of the range goes without saying) */
        if (!strcmp(w[i + 1], dflt_at)) snprintf(piece, sizeof piece, "%s", w[i]);
        else snprintf(piece, sizeof piece, "%s %s", w[i], w[i + 1]);
        i += 2;
    } else {
        snprintf(piece, sizeof piece, "%s", w[i]);
        i++;
    }
    const CssPropDef *d = css_prop_find(longhand, strlen(longhand));
    char *ok = d ? css_prop_parse(d, piece) : NULL;
    *pos = i;
    return ok;
}

static int expand_animation_range(const char *v, char **out) {
    char **parts = NULL;
    int np = split_commas(v, &parts);
    if (np <= 0) return -1;
    Buf s = {0}, e = {0};
    bool bad = false;
    for (int k = 0; k < np && !bad; k++) {
        char w[5][128];
        int nw = 0;
        const char *p = parts[k];
        while (*p && nw < 5) {
            while (isspace((unsigned char)*p)) p++;
            if (!*p) break;
            size_t len = 0;
            int depth = 0;
            while (*p && (depth || !isspace((unsigned char)*p))) {
                if (*p == '(') depth++; else if (*p == ')' && depth) depth--;
                if (len < 127) w[nw][len++] = *p;
                p++;
            }
            w[nw++][len] = 0;
        }
        int at = 0;
        char *a = range_side(w, nw, &at, "animation-range-start", "0%");
        char *b = NULL;
        if (a && at < nw) {
            b = range_side(w, nw, &at, "animation-range-end", "100%");
            if (b && !strcmp(b, "100%")) { free(b); b = strdup("normal"); }
        } else if (a) {
            /* (only a start: a range's end is its own) */
            bool named = !(isdigit((unsigned char)a[0]) || a[0] == '.' || a[0] == '-' || a[0] == '+' || strchr(a, '(') == a) && strcasecmp(a, "normal");
            if (named) { size_t l = strcspn(a, " "); b = strndup(a, l); }
            else b = strdup("normal");
        }
        if (!a || !b || at < nw) bad = true;
        else {
            if (k) { buf_str(&s, ", "); buf_str(&e, ", "); }
            buf_str(&s, a);
            buf_str(&e, b);
        }
        free(a);
        free(b);
    }
    for (int k = 0; k < np; k++) free(parts[k]);
    free(parts);
    if (bad) { free(s.s); free(e.s); return -1; }
    out[0] = s.s;
    out[1] = e.s;
    return 0;
}

static int canon_words(const char *v, char (*w)[160], int max);

/* text-box: normal, or a trim and an edge, either left to its default. */
static int expand_text_box(const char *v, char **out) {
    char w[4][160];
    int n = canon_words(v, w, 4);
    const CssPropDef *tp = css_prop_find("text-box-trim", 13), *ep = css_prop_find("text-box-edge", 13);
    if (n == 1 && !strcasecmp(w[0], "normal")) { out[0] = strdup("none"); out[1] = strdup("auto"); return 0; }
    Buf e = { 0 };
    const char *trim = NULL;
    for (int i = 0; i < n; i++) {
        char *ok = css_prop_parse(tp, w[i]);
        /* (a trim goes before or after the edge, not in it) */
        if (ok && i > 0 && i < n - 1) { free(ok); free(e.s); return -1; }
        if (ok && !trim) trim = w[i];
        else if (ok && trim) { free(ok); free(e.s); return -1; }
        else { if (e.n) buf_ch(&e, ' '); buf_str(&e, w[i]); }
        free(ok);
    }
    char *edge = NULL;
    if (e.n) {
        buf_ch(&e, 0);
        edge = css_prop_parse(ep, e.s);
        free(e.s);
        if (!edge) return -1;
    }
    out[0] = strdup(trim ? trim : "trim-both");
    out[1] = edge ? edge : strdup("auto");
    return 0;
}

/* offset: position, path, distance and rotate, then / anchor. */
static int expand_offset(const char *v, char **out) {
    char w[24][160];
    int n = canon_words(v, w, 24);
    if (n < 1) return -1;
    int slash = -1;
    for (int i = 0; i < n; i++) if (!strcmp(w[i], "/")) { slash = i; break; }
    int main_n = slash >= 0 ? slash : n;
    if (main_n < 1) return -1;
    int path_at = -1;
    for (int i = 0; i < main_n && path_at < 0; i++)
        if (!strcasecmp(w[i], "none") || strchr(w[i], '(') && strncasecmp(w[i], "calc(", 5) && strncasecmp(w[i], "min(", 4) &&
            strncasecmp(w[i], "max(", 4) && strncasecmp(w[i], "clamp(", 6)) path_at = i;
    Buf pos = { 0 }, path = { 0 }, anchor = { 0 };
    const char *dist = NULL, *rot_kw = NULL, *rot_ang = NULL;
    int pos_end = path_at >= 0 ? path_at : main_n;
    for (int i = 0; i < pos_end; i++) { if (pos.n) buf_ch(&pos, ' '); buf_str(&pos, w[i]); }
    int i = path_at >= 0 ? path_at + 1 : main_n;
    if (path_at >= 0) buf_str(&path, w[path_at]);
    /* (a box may be with the path) */
    while (i < main_n && (!strcasecmp(w[i], "border-box") || !strcasecmp(w[i], "padding-box") || !strcasecmp(w[i], "content-box") ||
                          !strcasecmp(w[i], "fill-box") || !strcasecmp(w[i], "stroke-box") || !strcasecmp(w[i], "view-box"))) {
        buf_ch(&path, ' ');
        buf_str(&path, w[i++]);
    }
    bool bad = false;
    for (; i < main_n; i++) {
        const char *t = w[i];
        char *e = NULL;
        double d = strtod(t, &e);
        (void)d;
        if (!strcasecmp(t, "auto") || !strcasecmp(t, "reverse")) { if (rot_kw) bad = true; rot_kw = t; }
        else if (e != t && (!strcasecmp(e, "deg") || !strcasecmp(e, "rad") || !strcasecmp(e, "grad") || !strcasecmp(e, "turn"))) { if (rot_ang) bad = true; rot_ang = t; }
        else { if (dist) bad = true; dist = t; }
    }
    for (int k = slash >= 0 ? slash + 1 : n; k < n; k++) { if (anchor.n) buf_ch(&anchor, ' '); buf_str(&anchor, w[k]); }
    if (slash >= 0 && !anchor.n) bad = true;
    buf_ch(&pos, 0); buf_ch(&path, 0); buf_ch(&anchor, 0);
    char rot[340];
    snprintf(rot, sizeof rot, "%s%s%s", rot_kw ? rot_kw : "", rot_kw && rot_ang ? " " : "", rot_ang ? rot_ang : "");
    static const char *const nm[5] = { "offset-position", "offset-path", "offset-distance", "offset-rotate", "offset-anchor" };
    const char *val[5] = { pos.s && pos.s[0] ? pos.s : NULL, path.s && path.s[0] ? path.s : NULL, dist, rot[0] ? rot : NULL, anchor.s && anchor.s[0] ? anchor.s : NULL };
    static const char *const dflt[5] = { "normal", "none", "0px", "auto", "auto" };
    if (!val[0] && !val[1]) bad = true;
    for (int k = 0; k < 5 && !bad; k++) {
        const CssPropDef *lp = css_prop_find(nm[k], strlen(nm[k]));
        out[k] = lp && val[k] ? css_prop_parse(lp, val[k]) : strdup(dflt[k]);
        if (!out[k]) bad = true;
    }
    free(pos.s); free(path.s); free(anchor.s);
    return bad ? -1 : 0;
}

static int expand_special(const CssPropDef *p, const char *v, char **out, const char **names, int n) {
    const char *nm = p->name;
    /* (the legacy page-break-*: always is page, the rest as they are) */
    if (!strncmp(nm, "page-break-", 11)) {
        static const char *const ok[] = { "auto", "always", "avoid", "left", "right", "recto", "verso", NULL };
        bool inside = !strcmp(nm, "page-break-inside");
        for (int i = 0; ok[i]; i++) {
            if (inside && strcmp(ok[i], "auto") && strcmp(ok[i], "avoid")) continue;
            if (!strcasecmp(v, ok[i])) { out[0] = strdup(!strcmp(ok[i], "always") ? "page" : ok[i]); return 1; }
        }
        return -1;
    }
    if (!strcmp(nm, "animation-range")) return expand_animation_range(v, out);
    if (!strcmp(nm, "text-box")) return expand_text_box(v, out);
    if (!strcmp(nm, "container")) {
        const char *sl = strchr(v, '/');
        char *name = sl ? strndup(v, (size_t)(sl - v)) : strdup(v);
        const CssPropDef *nd = css_prop_find("container-name", 14), *td = css_prop_find("container-type", 14);
        char *a = css_prop_parse(nd, name);
        free(name);
        char *t = sl ? css_prop_parse(td, sl + 1) : strdup("normal");
        if (!a || !t) { free(a); free(t); return -1; }
        out[0] = a;
        out[1] = t;
        return 0;
    }
    if (!strcmp(nm, "position-try")) {
        char w[2][160];
        int nw = canon_words(v, w, 1);
        const char *rest = v;
        const CssPropDef *od = css_prop_find("position-try-order", 18), *fd = css_prop_find("position-try-fallbacks", 22);
        char *ord = nw ? css_prop_parse(od, w[0]) : NULL;
        if (ord) {
            while (isspace((unsigned char)*rest)) rest++;
            rest += strlen(w[0]);
            while (isspace((unsigned char)*rest)) rest++;
        }
        char *fb = *rest ? css_prop_parse(fd, rest) : NULL;
        if (!fb) { free(ord); return -1; }
        out[0] = ord ? ord : strdup("normal");
        out[1] = fb;
        return 0;
    }
    if (!strcmp(nm, "offset")) return expand_offset(v, out);
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
    if (!strcmp(nm, "mask")) return expand_mask(v, out, names, n);
    if (!strcmp(nm, "grid-template") || !strcmp(nm, "grid")) return expand_grid(nm, v, out, names, n);
    if (!strcmp(nm, "grid-lanes")) return expand_grid_lanes(v, out);
    if (corner_count(nm)) return expand_corner(nm, v, out);
    if (!strncmp(nm, "border-", 7) && strcmp(nm, "border-radius") && !strcmp(nm + strlen(nm) - 7, "-radius")) return expand_side_radius(v, out);
    if (gap_same(nm) || gap_pairs(nm) || gap_insets(nm) || gap_rule(nm) || !strcmp(nm, "corner-shape")) return expand_gap(nm, v, out, names, n);
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
            /* animation: the initial ones left out (the duration kept when the delay
             * is given); every member when the name could be read as one of them */
            static const char *const kws[] = { "none", "ease", "linear", "ease-in", "ease-out", "ease-in-out", "step-start", "step-end",
                "infinite", "normal", "reverse", "alternate", "alternate-reverse", "forwards", "backwards", "both", "running", "paused", "auto" };
            static const char *const dflt[] = { "0s", "ease", "0s", "1", "normal", "none", "running", "none" };
            const char *name = cols[7][l];
            bool clash = false;
            for (size_t q = 1; q < sizeof kws / sizeof kws[0]; q++) if (!strcasecmp(name, kws[q])) clash = true;
            if (clash) {
                for (int k = 0; k < nm; k++) { if (k) buf_ch(b, ' '); buf_str(b, cols[k][l]); }
            } else {
                bool sp = false, delay = strcmp(cols[2][l], "0s") != 0;
                for (int k = 0; k < nm; k++) {
                    const char *v = cols[k][l];
                    bool initial = !strcmp(v, dflt[k]) || (k == 0 && !strcmp(v, "auto"));
                    if (initial && !(k == 0 && delay)) continue;
                    if (sp) buf_ch(b, ' ');
                    buf_str(b, v);
                    sp = true;
                }
                if (b->n == start) buf_str(b, "none");
            }
        }
    }
    for (int k = 0; k < nm; k++) free_parts(cols[k], len);
    return 0;
}

static int serialize_special(const CssPropDef *p, const char *const *vals, const char **names, int n, Buf *b) {
    if (!strncmp(p->name, "page-break-", 11) && n == 1) {
        static const char *const ok[] = { "auto", "avoid", "left", "right", "recto", "verso", NULL };
        bool inside = !strcmp(p->name, "page-break-inside");
        if (!inside && !strcmp(vals[0], "page")) { buf_str(b, "always"); return 0; }
        for (int i = 0; ok[i]; i++) {
            if (inside && strcmp(ok[i], "auto") && strcmp(ok[i], "avoid")) continue;
            if (!strcmp(vals[0], ok[i])) { buf_str(b, ok[i]); return 0; }
        }
        return -1;
    }
    const char *nm = p->name;
    if (!strcmp(nm, "position-try")) {
        if (strcmp(vals[0], "normal")) { buf_str(b, vals[0]); buf_ch(b, ' '); }
        buf_str(b, vals[1]);
        return 0;
    }
    if (!strcmp(nm, "container")) {
        buf_str(b, vals[0]);
        if (strcmp(vals[1], "normal")) { buf_str(b, " / "); buf_str(b, vals[1]); }
        return 0;
    }
    if (!strcmp(nm, "text-box")) {
        const char *t = vals[0], *e = vals[1];
        if (!strcmp(e, "auto")) { buf_str(b, !strcmp(t, "none") ? "normal" : t); return 0; }
        if (strcmp(t, "trim-both")) { buf_str(b, t); buf_ch(b, ' '); }
        buf_str(b, e);
        return 0;
    }
    if (!strcmp(nm, "offset")) {
        const char *pos = vals[0], *path = vals[1], *dist = vals[2], *rot = vals[3], *anchor = vals[4];
        bool sp = false;
        #define OFF_PUT(s) do { if (sp) buf_ch(b, ' '); buf_str(b, s); sp = true; } while (0)
        bool has_dist = strcmp(dist, "0px") && strcmp(dist, "0"), has_rot = strcmp(rot, "auto") != 0;
        if (strcmp(pos, "normal")) OFF_PUT(pos);
        if (strcmp(path, "none") || has_dist || has_rot || !sp) OFF_PUT(path);
        if (has_dist) OFF_PUT(dist);
        if (has_rot) OFF_PUT(rot);
        if (strcmp(anchor, "auto")) { buf_str(b, " / "); buf_str(b, anchor); }
        #undef OFF_PUT
        return 0;
    }
    if (!strcmp(nm, "animation-range")) {
        char **ps = NULL, **pe = NULL;
        int ns = split_commas(vals[0], &ps), ne = split_commas(vals[1], &pe);
        int r = ns == ne ? 0 : -1;
        for (int k = 0; r == 0 && k < ns; k++) {
            const char *s = ps[k], *e = pe[k];
            if (k) buf_str(b, ", ");
            buf_str(b, s);
            bool named = !(isdigit((unsigned char)s[0]) || s[0] == '.' || s[0] == '-' || s[0] == '+' || strchr(s, '(') == s) && strcasecmp(s, "normal");
            size_t l = strcspn(s, " ");
            bool same = named ? strlen(e) == l && !strncmp(s, e, l) : !strcmp(e, "normal");
            if (!same) { buf_ch(b, ' '); buf_str(b, e); }
        }
        for (int k = 0; k < ns; k++) free(ps[k]);
        for (int k = 0; k < ne; k++) free(pe[k]);
        free(ps);
        free(pe);
        return r;
    }
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
                /* (an edge and its offset with a lone other: the lone one from its own start edge) */
                bool x2 = strchr(cols[1][l], ' ') != NULL, y2 = strchr(cols[2][l], ' ') != NULL;
                if (y2 && !x2) buf_str(b, "left ");
                buf_str(b, cols[1][l]); buf_ch(b, ' ');
                if (x2 && !y2) buf_str(b, "top ");
                buf_str(b, cols[2][l]);
                if (size) { buf_str(b, " / "); buf_str(b, cols[3][l]); }
                sp = true;
            }
            if (strcmp(cols[4][l], "repeat")) PUT(cols[4][l]);
            if (strcmp(cols[5][l], "scroll")) PUT(cols[5][l]);
            bool org = strcmp(cols[6][l], "padding-box"), clip = strcmp(cols[7][l], "border-box");
            /* (border-area's own origin, the border box, goes without saying) */
            if (strstr(cols[7][l], "border-area") && !strcmp(cols[6][l], "border-box")) PUT(cols[7][l]);
            else if (org || clip) {
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
    if (!strcmp(nm, "grid-template") || !strcmp(nm, "grid")) return serialize_grid(nm, vals, names, n, b);
    if (!strcmp(nm, "grid-lanes")) return serialize_grid_lanes(vals, names, n, b);
    if (corner_count(nm)) return serialize_corner(nm, vals, n, b);
    if (!strncmp(nm, "border-", 7) && !strcmp(nm + strlen(nm) - 7, "-radius") && n == 2) return serialize_side_radius(vals, n, b);
    if (gap_same(nm) || gap_pairs(nm) || gap_insets(nm) || gap_rule(nm) || !strcmp(nm, "corner-shape")) return serialize_gap(nm, vals, n, b);
    if (!strcmp(nm, "mask")) {
        static const char *const lnames[] = { "mask-image", "mask-position", "mask-size", "mask-repeat",
                                              "mask-origin", "mask-clip", "mask-composite", "mask-mode" };
        static const char *const resets[] = { "mask-border-source", "mask-border-outset", "mask-border-repeat",
                                              "mask-border-slice", "mask-border-width" };
        for (int k = 0; k < 5; k++) {
            const char *v = val_of(vals, names, n, resets[k]);
            if (v && !is_initial(resets[k], v)) return -1;
        }
        char **cols[8];
        int len = -1;
        for (int k = 0; k < 8; k++) {
            const char *v = val_of(vals, names, n, lnames[k]);
            int c = split_commas(v ? v : "", &cols[k]);
            if (len >= 0 && c != len) { for (int j = 0; j <= k; j++) free_parts(cols[j], j == k ? c : len); return -1; }
            len = c;
        }
        for (int l = 0; l < len; l++) {
            if (l) buf_str(b, ", ");
            size_t start = b->n;
            bool sp = false;
            #define PUT(x) do { if (sp) buf_ch(b, ' '); buf_str(b, x); sp = true; } while (0)
            if (strcmp(cols[0][l], "none")) PUT(cols[0][l]);
            bool size = strcmp(cols[2][l], "auto");
            if (strcmp(cols[1][l], "0% 0%") || size) {
                PUT(cols[1][l]);
                if (size) { buf_str(b, " / "); buf_str(b, cols[2][l]); }
            }
            if (strcmp(cols[3][l], "repeat")) PUT(cols[3][l]);
            bool org = strcmp(cols[4][l], "border-box"), clip = strcmp(cols[5][l], "border-box");
            if (org || clip) {
                if (org) PUT(cols[4][l]);
                if (strcmp(cols[4][l], cols[5][l])) PUT(cols[5][l]);
            }
            if (strcmp(cols[6][l], "add")) PUT(cols[6][l]);
            if (strcmp(cols[7][l], "match-source")) PUT(cols[7][l]);
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
            else if (v && k == 1 && !strcmp(v, "oblique-only")) { if (sp) buf_ch(b, ' '); buf_str(b, "oblique-only"); sp = true; }
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

/* ---- gradients: their canonical form (CSS Images 4: an interpolation method
 * is left out where it is the default; it follows the size and position) ---- */
static void gradient_canon_one(const char *arg, const char *end, Buf *b)
{
    int depth = 0;
    const char *comma = NULL;
    for (const char *q = arg; q < end; q++) {
        if (*q == '(') depth++;
        else if (*q == ')') depth--;
        else if (*q == ',' && !depth) { comma = q; break; }
    }
    if (!comma) { buf_add(b, arg, (size_t)(end - arg)); return; }
    char pre[256];
    size_t pl = (size_t)(comma - arg);
    if (pl >= sizeof pre) { buf_add(b, arg, (size_t)(end - arg)); return; }
    memcpy(pre, arg, pl);
    pre[pl] = 0;
    char *w[32];
    int nw = gap_words(pre, w, 32);
    int in = -1;
    for (int i = 0; i < nw; i++) if (!strcasecmp(w[i], "in")) in = i;
    bool ellipse = nw > 1 && !strcasecmp(w[0], "ellipse") && strcasecmp(w[1], "at") && in != 1;
    if (in < 0 && !ellipse) {
        for (int i = 0; i < nw; i++) free(w[i]);
        buf_add(b, arg, (size_t)(end - arg));
        return;
    }
    bool legacy = true;
    static const char *const modern[] = { "color(", "lab(", "lch(" };
    for (int k = 0; k < 3; k++) {
        const char *hit = strstr(comma, modern[k]);
        if (hit && hit < end) legacy = false;
    }
    Buf rest = { 0 };
    int mend = in;
    if (in >= 0) mend = in + 2 < nw && in + 3 < nw && !strcasecmp(w[in + 3], "hue") ? in + 3 : in + 1;
    for (int i = ellipse ? 1 : 0; i < nw; i++) {
        if (in >= 0 && i >= in && i <= mend) continue;
        if (rest.n) buf_ch(&rest, ' ');
        buf_str(&rest, w[i]);
    }
    char method[64] = "";
    if (in >= 0 && in + 1 < nw) {
        snprintf(method, sizeof method, "%s", !strcasecmp(w[in + 1], "xyz") ? "xyz-d65" : w[in + 1]);
        if (mend == in + 3 && strcasecmp(w[in + 2], "shorter")) {
            size_t ml = strlen(method);
            snprintf(method + ml, sizeof method - ml, " %s hue", w[in + 2]);
        }
        if (legacy ? !strcasecmp(method, "srgb") : !strcasecmp(method, "oklab")) method[0] = 0;
    }
    if (rest.n) buf_add(b, rest.s, rest.n);
    if (method[0]) {
        if (rest.n) buf_ch(b, ' ');
        buf_str(b, "in ");
        buf_str(b, method);
    }
    free(rest.s);
    if (b->n && b->s[b->n - 1] != '(') buf_ch(b, ',');
    else {
        /* (nothing is left before the stops) */
        comma++;
        while (comma < end && *comma == ' ') comma++;
        comma--;
    }
    buf_add(b, comma + 1, (size_t)(end - comma - 1));
    for (int i = 0; i < nw; i++) free(w[i]);
}

/* Words of a value at its top level: returns how many, into w (at most max). */
static int canon_words(const char *v, char (*w)[160], int max)
{
    int n = 0;
    const char *p = v;
    while (*p && n < max) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        size_t l = 0;
        int d = 0;
        char q = 0;
        while (*p && (d || q || !isspace((unsigned char)*p))) {
            if (q) { if (*p == q) q = 0; }
            else if (*p == '"' || *p == '\'') q = *p;
            else if (*p == '(') d++;
            else if (*p == ')' && d) d--;
            if (l < 159) w[n][l++] = *p;
            p++;
        }
        w[n++][l] = 0;
    }
    return n;
}

/* A display's keywords in their shortest form. */
static char *canon_display(const char *v)
{
    while (isspace((unsigned char)*v)) v++;
    if (!strncasecmp(v, "layout(", 7) || !strncasecmp(v, "inline-layout(", 14)) {
        /* (a custom layout: the function as written, spaces dropped) */
        char b[160];
        size_t o = 0;
        for (const char *p = v; *p && o + 1 < sizeof b; p++) if (!isspace((unsigned char)*p)) b[o++] = *p;
        b[o] = 0;
        return strdup(b);
    }
    char w[4][160];
    int n = canon_words(v, w, 4);
    if (n < 1 || n > 3 || (n == 1 && strcasecmp(w[0], "flow"))) return NULL;
    const char *outer = NULL, *inner = NULL;
    bool list = false;
    for (int i = 0; i < n; i++) {
        const char *t = w[i];
        if (!strcasecmp(t, "block") || !strcasecmp(t, "inline") || !strcasecmp(t, "run-in")) outer = t;
        else if (!strcasecmp(t, "list-item")) list = true;
        else if (!strcasecmp(t, "flow") || !strcasecmp(t, "flow-root") || !strcasecmp(t, "table") || !strcasecmp(t, "flex") ||
                 !strcasecmp(t, "grid") || !strcasecmp(t, "ruby") || !strcasecmp(t, "grid-lanes") || !strcasecmp(t, "math")) inner = t;
        else if (!strcasecmp(t, "math")) inner = t;
        else return NULL;
    }
    char b[80];
    bool in_flow = !inner || !strcasecmp(inner, "flow");
    if (inner && !outer && !strcasecmp(inner, "math")) return NULL;
    if (list) {
        snprintf(b, sizeof b, "%s%s%s%slist-item", outer && strcasecmp(outer, "block") ? outer : "", outer && strcasecmp(outer, "block") ? " " : "",
                 in_flow ? "" : inner, in_flow ? "" : " ");
        return strdup(b);
    }
    const char *o = outer ? outer : "block";
    /* (math and ruby are inline unless they say) */
    if (inner && !strcasecmp(inner, "math") && !strcasecmp(o, "inline")) return strdup("math");
    if (inner && !strcasecmp(inner, "math") && !strcasecmp(o, "block")) return strdup("block math");
    bool ruby = inner && !strcasecmp(inner, "ruby");
    if (ruby && !outer) o = "inline";
    if (in_flow) return strdup(o);
    if (ruby) {
        if (!strcasecmp(o, "inline")) return strdup("ruby");
        snprintf(b, sizeof b, "%s ruby", o);
        return strdup(b);
    }
    if (!strcasecmp(o, "block")) return strdup(inner);
    if (!strcasecmp(o, "inline")) {
        if (!strcasecmp(inner, "flow-root")) return strdup("inline-block");
        /* (the newer ones have no one-word form) */
        snprintf(b, sizeof b, strcasecmp(inner, "grid-lanes") ? "inline-%s" : "inline %s", inner);
        return strdup(b);
    }
    snprintf(b, sizeof b, "%s %s", o, inner);
    return strdup(b);
}

/* white-space's parts, as the old words where there is one. */
static char *canon_white_space(const char *v)
{
    char w[4][160];
    int n = canon_words(v, w, 4);
    if (n < 1) return NULL;
    const char *mode = NULL, *wrap = NULL, *rest = NULL;
    for (int i = 0; i < n; i++) {
        const char *t = w[i];
        if (!strcasecmp(t, "collapse") || !strcasecmp(t, "preserve") || !strcasecmp(t, "preserve-breaks") ||
            !strcasecmp(t, "preserve-spaces") || !strcasecmp(t, "break-spaces")) mode = t;
        else if (!strcasecmp(t, "wrap") || !strcasecmp(t, "nowrap")) wrap = t;
        else rest = t;
    }
    if (rest) return NULL;
    bool nowrap = wrap && !strcasecmp(wrap, "nowrap");
    const char *m = mode ? mode : "collapse";
    if (!strcasecmp(m, "collapse")) return strdup(nowrap ? "nowrap" : "normal");
    if (!strcasecmp(m, "preserve")) return strdup(nowrap ? "pre" : "pre-wrap");
    if (!nowrap) {
        if (!strcasecmp(m, "preserve-breaks")) return strdup("pre-line");
        return strdup(m);
    }
    char b[64];
    snprintf(b, sizeof b, "%s nowrap", m);
    return strdup(b);
}

/* text-box-edge's two, as few as say the same. */
static char *canon_box_edge(const char *v)
{
    char w[3][160];
    int n = canon_words(v, w, 3);
    if (n != 2) return NULL;
    /* (text under cap or ex: what one of them alone means) */
    if (!strcasecmp(w[1], "text") && (!strcasecmp(w[0], "cap") || !strcasecmp(w[0], "ex") || !strcasecmp(w[0], "text"))) return strdup(w[0]);
    if (!strcasecmp(w[0], w[1])) return strdup(w[0]);
    if (!strcasecmp(w[0], "text") && !strcasecmp(w[1], "alphabetic")) return strdup("alphabetic");
    return NULL;
}

/* offset-rotate: the keyword first, an angle that adds nothing gone. */
static char *canon_offset_rotate(const char *v)
{
    char w[3][160];
    int n = canon_words(v, w, 3);
    /* (computed, reverse is auto and half a turn) */
    if (g_canon_computed && n == 1 && !strcasecmp(w[0], "reverse")) return strdup("auto 180deg");
    if (n != 2) return NULL;
    bool kw0 = !strcasecmp(w[0], "auto") || !strcasecmp(w[0], "reverse");
    const char *k = kw0 ? w[0] : w[1], *a = kw0 ? w[1] : w[0];
    char b[200];
    if (g_canon_computed && !strcasecmp(k, "reverse")) {
        char *e = NULL;
        double d = strtod(a, &e);
        if (e && e != a && !strcasecmp(e, "deg")) {
            snprintf(b, sizeof b, "auto %.6gdeg", d + 180.0);
            return strdup(b);
        }
    }
    if (!strcasecmp(k, "auto") && strtod(a, NULL) == 0.0 && (isdigit((unsigned char)a[0]) || a[0] == '.' || a[0] == '-')) return strdup("auto");
    snprintf(b, sizeof b, "%s %s", k, a);
    return strdup(b);
}

/* ray(): angle, size if not closest-side, contain, position. */
static char *canon_ray(const char *v)
{
    const char *r = strstr(v, "ray(");
    if (!r) return NULL;
    const char *close = strrchr(v, ')');
    if (!close || r != v) return NULL;
    char inner[300];
    size_t il = (size_t)(close - r - 4);
    if (il >= sizeof inner) return NULL;
    memcpy(inner, r + 4, il);
    inner[il] = 0;
    char w[12][160];
    int n = canon_words(inner, w, 12);
    const char *angle = NULL, *size = NULL;
    bool contain = false;
    Buf at = { 0 };
    for (int i = 0; i < n; i++) {
        if (!strcasecmp(w[i], "at")) {
            for (int k = i + 1; k < n; k++) { if (at.n) buf_ch(&at, ' '); buf_str(&at, w[k]); }
            break;
        }
        if (!strcasecmp(w[i], "contain")) contain = true;
        else if (!strncasecmp(w[i], "closest-", 8) || !strncasecmp(w[i], "farthest-", 9) || !strcasecmp(w[i], "sides")) size = w[i];
        else if (!angle) angle = w[i];
        else { free(at.s); return NULL; }
    }
    if (!angle) { free(at.s); return NULL; }
    Buf b = { 0 };
    buf_str(&b, "ray(");
    buf_str(&b, angle);
    if (size && strcasecmp(size, "closest-side")) { buf_ch(&b, ' '); buf_str(&b, size); }
    if (contain) buf_str(&b, " contain");
    if (at.n) {
        buf_ch(&at, 0);
        buf_str(&b, " at ");
        buf_str(&b, at.s);
    }
    free(at.s);
    buf_str(&b, ")");
    buf_str(&b, close + 1);
    buf_ch(&b, 0);
    return b.s;
}

/* inset(), rect(), xywh()'s corners: each list as short as it goes, and the
 * second if it says the same as the first gone. */
static void radii_list(char *text, Buf *out)
{
    char w[4][160];
    int n = canon_words(text, w, 4);
    if (n < 1) return;
    for (int i = 0; i < n; i++) if (w[i][0] == '+') memmove(w[i], w[i] + 1, strlen(w[i]));
    if (n == 3) { snprintf(w[3], 160, "%s", w[1]); n = 4; }
    if (n == 2) { snprintf(w[2], 160, "%s", w[0]); snprintf(w[3], 160, "%s", w[1]); n = 4; }
    if (n == 1) { for (int i = 1; i < 4; i++) snprintf(w[i], 160, "%s", w[0]); n = 4; }
    int keep = 4;
    if (!strcmp(w[3], w[1])) keep = 3;
    if (keep == 3 && !strcmp(w[2], w[0])) keep = 2;
    if (keep == 2 && !strcmp(w[1], w[0])) keep = 1;
    for (int i = 0; i < keep; i++) { if (i) buf_ch(out, ' '); buf_str(out, w[i]); }
}

static char *canon_box_round(const char *v)
{
    if (!strstr(v, " round ")) return NULL;
    if (strncasecmp(v, "inset(", 6) && strncasecmp(v, "rect(", 5) && strncasecmp(v, "xywh(", 5)) return NULL;
    const char *r = strstr(v, " round ");
    const char *close = r;
    int d = 0;
    for (; *close; close++) {
        if (*close == '(') d++;
        else if (*close == ')') { if (!d) break; d--; }
    }
    if (!*close) return NULL;
    char *body = strndup(r + 7, (size_t)(close - r - 7));
    char *slash = strchr(body, '/');
    Buf a = { 0 }, c = { 0 }, b = { 0 };
    if (slash) *slash = 0;
    radii_list(body, &a);
    if (slash) radii_list(slash + 1, &c);
    buf_ch(&a, 0);
    buf_ch(&c, 0);
    buf_add(&b, v, (size_t)(r - v));
    buf_str(&b, " round ");
    buf_str(&b, a.s);
    if (slash && strcmp(a.s, c.s)) { buf_str(&b, " / "); buf_str(&b, c.s); }
    buf_str(&b, close);
    buf_ch(&b, 0);
    free(a.s);
    free(c.s);
    free(body);
    return b.s;
}

static __thread bool g_shape_computed;   /* (a shape being put in its computed form) */
__thread bool g_css_shape_path_normal;

/* A computed length-percentage as percent and px: "1%", "2px", "calc(1% + 2px)". */
static bool lin_parse(const char *t, double *pct, double *px, bool *has_pct)
{
    *pct = *px = 0.0;
    *has_pct = false;
    const char *p = t;
    bool in_calc = !strncasecmp(p, "calc(", 5);
    if (in_calc) p += 5;
    double sign = 1.0;
    for (;;) {
        while (isspace((unsigned char)*p)) p++;
        char *e = NULL;
        double v = strtod(p, &e);
        if (e == p) return false;
        if (*e == '%') { *pct += sign * v; *has_pct = true; e++; }
        else if (!strncasecmp(e, "px", 2)) { *px += sign * v; e += 2; }
        else if (v == 0.0 && (!*e || *e == ')' || isspace((unsigned char)*e))) { }
        else return false;
        p = e;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '+') sign = 1.0;
        else if (*p == '-') sign = -1.0;
        else break;
        p++;
    }
    if (in_calc && *p == ')') p++;
    return *p == 0;
}

static void lin_text(Buf *b, double pct, double px, bool has_pct)
{
    if (fabs(pct) < 1e-9) pct = 0.0;
    if (fabs(px) < 1e-9) px = 0.0;
    if (!has_pct) { fmt_number(b, px); buf_str(b, "px"); return; }
    if (px == 0.0) { fmt_number(b, pct); buf_ch(b, '%'); return; }
    buf_str(b, "calc(");
    fmt_number(b, pct);
    buf_str(b, px < 0 ? "% - " : "% + ");
    fmt_number(b, fabs(px));
    buf_str(b, "px)");
}

/* A value in its simplest computed form, into w. */
static void lin_simplify(char *w, size_t cap)
{
    double pct, px;
    bool hp;
    if (!lin_parse(w, &pct, &px, &hp)) return;
    Buf b = { 0 };
    lin_text(&b, pct, px, hp);
    buf_ch(&b, 0);
    snprintf(w, cap, "%s", b.s);
    free(b.s);
}

/* Computed xywh() or rect() as the inset() they come to; NULL if not worked out. */
static char *shape_box_inset(int fn, const char *inner)
{
    char w[4][160];
    if (canon_words(inner, w, 4) != 4) return NULL;
    double p[4], x[4];
    bool hp[4];
    for (int i = 0; i < 4; i++) {
        if (fn == 6 && !strcasecmp(w[i], "auto")) {
            /* (an edge at its own side) */
            p[i] = i == 1 || i == 2 ? 100.0 : 0.0;
            x[i] = 0.0;
            hp[i] = true;
        } else if (!lin_parse(w[i], &p[i], &x[i], &hp[i])) return NULL;
    }
    double tp = fn == 5 ? p[1] : p[0], tx = fn == 5 ? x[1] : x[0];
    double lp = fn == 5 ? p[0] : p[3], lx = fn == 5 ? x[0] : x[3];
    double rp = fn == 5 ? 100.0 - (p[0] + p[2]) : 100.0 - p[1], rx = fn == 5 ? -(x[0] + x[2]) : -x[1];
    double bp = fn == 5 ? 100.0 - (p[1] + p[3]) : 100.0 - p[2], bx = fn == 5 ? -(x[1] + x[3]) : -x[2];
    Buf b = { 0 };
    lin_text(&b, tp, tx, fn == 5 ? hp[1] : hp[0]);
    buf_ch(&b, ' ');
    lin_text(&b, rp, rx, true);
    buf_ch(&b, ' ');
    lin_text(&b, bp, bx, true);
    buf_ch(&b, ' ');
    lin_text(&b, lp, lx, fn == 5 ? hp[0] : hp[3]);
    return buf_take(&b);
}

/* Path data with every command absolute and the numbers spaced, in quotes. */
static bool path_normal(const char *d, Buf *out)
{
    double cx = 0, cy = 0, sx = 0, sy = 0;
    const char *p = d;
    char cmd = 0;
    bool any = false;
    for (;;) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        if (isalpha((unsigned char)*p)) cmd = *p++;
        else if (!cmd) return false;
        else if (cmd == 'M') cmd = 'L';
        else if (cmd == 'm') cmd = 'l';
        bool rel = cmd >= 'a';
        char up = (char)toupper((unsigned char)cmd);
        static const char *const names = "MLHVCSQTAZ";
        static const int counts[] = { 2, 2, 1, 1, 6, 4, 4, 2, 7, 0 };
        const char *f = strchr(names, up);
        if (!f) return false;
        int cnt = counts[f - names];
        double a[7];
        for (int i = 0; i < cnt; i++) {
            while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
            char *e = NULL;
            if (up == 'A' && (i == 3 || i == 4)) {
                if (*p != '0' && *p != '1') return false;
                a[i] = *p == '1';
                p++;
                continue;
            }
            a[i] = strtod(p, &e);
            if (e == p) return false;
            p = e;
        }
        if (any) buf_ch(out, ' ');
        any = true;
        buf_ch(out, up);
        if (up == 'Z') { cx = sx; cy = sy; cmd = 0; continue; }
        for (int i = 0; i < cnt; i++) {
            double v = a[i];
            if (rel) {
                if (up == 'H') v += cx;
                else if (up == 'V') v += cy;
                else if (up == 'A') { if (i == 5) v += cx; else if (i == 6) v += cy; }
                else v += i % 2 ? cy : cx;
            }
            a[i] = v;
            buf_ch(out, ' ');
            fmt_number(out, v);
        }
        if (up == 'H') cx = a[0];
        else if (up == 'V') cy = a[0];
        else { cx = a[cnt - 2]; cy = a[cnt - 1]; }
        if (up == 'M') { sx = cx; sy = cy; }
    }
    return any;
}

/* A position in a shape: both axes, x first; the computed form in percentages. */
static void shape_position(const char *text, Buf *b)
{
    char w[4][160];
    int n = canon_words(text, w, 4);
    bool computed = g_shape_computed != NULL;
    if (n == 1) {
        if (!strcasecmp(w[0], "top") || !strcasecmp(w[0], "bottom")) { snprintf(w[1], 160, "%s", w[0]); snprintf(w[0], 160, "center"); }
        else snprintf(w[1], 160, "center");
        n = 2;
    } else if (n == 2) {
        bool first_y = !strcasecmp(w[0], "top") || !strcasecmp(w[0], "bottom");
        bool second_x = !strcasecmp(w[1], "left") || !strcasecmp(w[1], "right");
        if (first_y || second_x) { char t[160]; memcpy(t, w[0], 160); memcpy(w[0], w[1], 160); memcpy(w[1], t, 160); }
    }
    /* (from the sides, when it's worked out: from the top left) */
    if (computed && n == 4) {
        bool first_y = !strcasecmp(w[0], "top") || !strcasecmp(w[0], "bottom");
        const char *ex = first_y ? w[2] : w[0], *vx = first_y ? w[3] : w[1];
        const char *ey = first_y ? w[0] : w[2], *vy = first_y ? w[1] : w[3];
        double p1, x1, p2, x2;
        bool h1, h2;
        if (lin_parse(vx, &p1, &x1, &h1) && lin_parse(vy, &p2, &x2, &h2)) {
            if (!strcasecmp(ex, "right")) { p1 = 100.0 - p1; x1 = -x1; h1 = true; }
            if (!strcasecmp(ey, "bottom")) { p2 = 100.0 - p2; x2 = -x2; h2 = true; }
            lin_text(b, p1, x1, h1);
            buf_ch(b, ' ');
            lin_text(b, p2, x2, h2);
            return;
        }
    }
    for (int i = 0; i < n; i++) {
        const char *o = w[i];
        if (computed && n == 2) {
            if (!strcasecmp(o, "left") || !strcasecmp(o, "top")) o = "0%";
            else if (!strcasecmp(o, "right") || !strcasecmp(o, "bottom")) o = "100%";
            else if (!strcasecmp(o, "center")) o = "50%";
            else lin_simplify(w[i], sizeof w[i]);
        }
        if (i) buf_ch(b, ' ');
        buf_str(b, o);
    }
}

/* clip-path and shape-outside: the shape first, what's implied gone, each function short. */
static char *canon_basic_shape(const char *n, const char *v)
{
    const char *open = strchr(v, '(');
    if (!open) return NULL;
    const char *head = open;
    while (head > v && (isalnum((unsigned char)head[-1]) || head[-1] == '-')) head--;
    size_t fl = (size_t)(open - head);
    static const char *const fns[] = { "circle", "ellipse", "polygon", "path", "inset", "xywh", "rect" };
    int fn = -1;
    for (int i = 0; i < 7; i++) if (strlen(fns[i]) == fl && !strncasecmp(head, fns[i], fl)) fn = i;
    if (fn < 0) return NULL;
    int d = 0;
    const char *close = open;
    char q = 0;
    for (; *close; close++) {
        if (q) { if (*close == q) q = 0; continue; }
        if (*close == '"' || *close == '\'') q = *close;
        else if (*close == '(') d++;
        else if (*close == ')' && --d == 0) break;
    }
    if (!*close) return NULL;
    /* (a box on either side) */
    char box[32] = "";
    size_t pl = (size_t)(head - v);
    while (pl > 0 && isspace((unsigned char)v[pl - 1])) pl--;
    const char *after = close + 1;
    while (isspace((unsigned char)*after)) after++;
    if (pl && *after) return NULL;
    if (pl >= sizeof box || strlen(after) >= sizeof box) return NULL;
    if (pl) { memcpy(box, v, pl); box[pl] = 0; }
    else snprintf(box, sizeof box, "%s", after);
    size_t bl = strlen(box);
    while (bl > 0 && isspace((unsigned char)box[bl - 1])) box[--bl] = 0;
    char *inner = strndup(open + 1, (size_t)(close - open - 1));
    /* (computed, these two are insets) */
    if ((fn == 5 || fn == 6) && g_shape_computed) {
        char *rd0 = strstr(inner, " round ");
        char *box0 = rd0 ? strndup(inner, (size_t)(rd0 - inner)) : strdup(inner);
        char *ins = shape_box_inset(fn, box0);
        free(box0);
        if (ins) {
            Buf ni = { 0 };
            buf_str(&ni, ins);
            if (rd0) buf_str(&ni, rd0);
            free(ins);
            free(inner);
            inner = buf_take(&ni);
            fn = 4;
        }
    }
    Buf b = { 0 };
    buf_str(&b, fns[fn]);
    buf_ch(&b, '(');
    if (fn == 0 || fn == 1) {
        /* (radii up to "at"; the closest side is what's meant without) */
        char *at = NULL;
        d = 0;
        for (char *c = inner; *c; c++) {
            if (*c == '(') d++;
            else if (*c == ')') d--;
            else if (!d && !strncasecmp(c, "at", 2) && (c == inner || isspace((unsigned char)c[-1])) && isspace((unsigned char)c[2])) { at = c; break; }
        }
        if (at) *at = 0;
        char w[3][160];
        int nr = canon_words(inner, w, 3);
        bool sp = false;
        /* (computed, a radius is never below nothing) */
        if (g_shape_computed) for (int i = 0; i < nr; i++) if (w[i][0] == '-' && strtod(w[i], NULL) < 0) snprintf(w[i], 160, "0px");
        if (fn == 0) {
            if (nr == 1 && strcasecmp(w[0], "closest-side")) { buf_str(&b, w[0]); sp = true; }
        } else if (nr == 2 && (strcasecmp(w[0], "closest-side") || strcasecmp(w[1], "closest-side"))) {
            buf_str(&b, w[0]); buf_ch(&b, ' '); buf_str(&b, w[1]);
            sp = true;
        }
        if (at) {
            if (sp) buf_ch(&b, ' ');
            buf_str(&b, "at ");
            shape_position(at + 2, &b);
        }
    } else if (fn == 2 || fn == 3) {
        /* (the fill rule that's implied, and no rounding, unsaid) */
        d = 0;
        const char *comma = NULL;
        q = 0;
        for (const char *c = inner; *c && !comma; c++) {
            if (q) { if (*c == q) q = 0; continue; }
            if (*c == '"' || *c == '\'') q = *c;
            else if (*c == '(') d++;
            else if (*c == ')') d--;
            else if (*c == ',' && !d) comma = c;
        }
        const char *rest = inner;
        if (comma) {
            char *first = strndup(inner, (size_t)(comma - inner));
            char w[4][160];
            int nf = canon_words(first, w, 4);
            free(first);
            bool rule = nf >= 1 && (!strcasecmp(w[0], "nonzero") || !strcasecmp(w[0], "evenodd"));
            int ri = rule ? 1 : 0;
            bool round = nf == ri + 2 && !strcasecmp(w[ri], "round");
            if (rule || round) {
                rest = comma + 1;
                while (isspace((unsigned char)*rest)) rest++;
                bool zero = round && strtod(w[ri + 1], NULL) == 0.0 && !strchr(w[ri + 1], '(');
                bool even = rule && !strcasecmp(w[0], "evenodd");
                if (even) buf_str(&b, "evenodd");
                if (round && !zero) { if (even) buf_ch(&b, ' '); buf_str(&b, "round "); buf_str(&b, w[ri + 1]); }
                if (even || (round && !zero)) buf_str(&b, ", ");
            }
        }
        /* (computed, path data as absolute commands) */
        Buf pn = { 0 };
        size_t rl = strlen(rest);
        if (fn == 3 && g_shape_computed && g_css_shape_path_normal && strcmp(n, "clip-path") && rl >= 2 && (rest[0] == '"' || rest[0] == '\'')) {
            char *data = strndup(rest + 1, rl - 2);
            bool ok = path_normal(data, &pn);
            free(data);
            if (ok) { buf_ch(&b, '"'); buf_add(&b, pn.s, pn.n); buf_ch(&b, '"'); rest = ""; }
            free(pn.s);
        }
        buf_str(&b, rest);
    } else {
        /* (inset, xywh, rect: no rounding unsaid; inset's sides as short as they go) */
        char *rd = strstr(inner, " round ");
        char *rt = NULL;
        if (rd) { *rd = 0; rt = rd + 7; }
        if (fn == 4) {
            char w[4][160];
            int ns = canon_words(inner, w, 4);
            if (ns < 1) { free(inner); free(b.s); return NULL; }
            if (ns == 3) { snprintf(w[3], 160, "%s", w[1]); ns = 4; }
            if (ns == 2) { snprintf(w[2], 160, "%s", w[0]); snprintf(w[3], 160, "%s", w[1]); ns = 4; }
            if (ns == 1) { for (int i = 1; i < 4; i++) snprintf(w[i], 160, "%s", w[0]); ns = 4; }
            if (g_shape_computed) for (int i = 0; i < 4; i++) lin_simplify(w[i], sizeof w[i]);
            int keep = 4;
            if (!strcmp(w[3], w[1])) keep = 3;
            if (keep == 3 && !strcmp(w[2], w[0])) keep = 2;
            if (keep == 2 && !strcmp(w[1], w[0])) keep = 1;
            for (int i = 0; i < keep; i++) { if (i) buf_ch(&b, ' '); buf_str(&b, w[i]); }
        } else {
            buf_str(&b, inner);
        }
        if (rt) {
            while (isspace((unsigned char)*rt)) rt++;
            bool zero = !strchr(rt, ' ') && !strchr(rt, '(') && strtod(rt, NULL) == 0.0;
            if (!zero && g_shape_computed) {
                /* (each radius as simple as it goes) */
                char rw[9][160];
                int nw = canon_words(rt, rw, 9);
                buf_str(&b, " round");
                for (int i = 0; i < nw; i++) {
                    if (strcmp(rw[i], "/")) lin_simplify(rw[i], sizeof rw[i]);
                    buf_ch(&b, ' ');
                    buf_str(&b, rw[i]);
                }
            } else if (!zero) { buf_str(&b, " round "); buf_str(&b, rt); }
        }
    }
    buf_ch(&b, ')');
    bool implied = ((!strcmp(n, "clip-path") || !strcmp(n, "offset-path")) && !strcasecmp(box, "border-box")) || (!strcmp(n, "shape-outside") && !strcasecmp(box, "margin-box"));
    if (box[0] && !implied) { buf_ch(&b, ' '); buf_str(&b, box); }
    free(inner);
    buf_ch(&b, 0);
    if (!strcmp(b.s, v)) { free(b.s); return NULL; }
    return b.s;
}

/* Computed radial and conic gradients: what goes without saying, unsaid (an
 * ellipse to the farthest corner, at the centre, from 0deg); a radius below
 * nothing, none. */
static char *gradient_computed(const char *v)
{
    if (!strstr(v, "radial-gradient(") && !strstr(v, "conic-gradient(")) return NULL;
    Buf b = { 0 };
    bool changed = false;
    for (const char *p = v; *p; ) {
        const char *fn = NULL;
        bool radial = false;
        if (!strncmp(p, "radial-gradient(", 16) || !strncmp(p, "repeating-radial-gradient(", 26)) { fn = strchr(p, '('); radial = true; }
        else if (!strncmp(p, "conic-gradient(", 15) || !strncmp(p, "repeating-conic-gradient(", 25)) fn = strchr(p, '(');
        if (!fn || (p > v && (isalnum((unsigned char)p[-1]) || p[-1] == '-'))) { buf_ch(&b, *p++); continue; }
        /* (its first argument, if it isn't a color stop) */
        const char *a = fn + 1, *c = a;
        int d = 0;
        for (; *c && !(d == 0 && (*c == ',' || *c == ')')); c++) { if (*c == '(') d++; else if (*c == ')') d--; }
        char *first = strndup(a, (size_t)(c - a));
        char w[12][160];
        int nw = canon_words(first, w, 12);
        bool config = nw > 0 && (!strcmp(w[0], "at") || !strcmp(w[0], "from") || !strcmp(w[0], "in") || !strcmp(w[0], "circle") ||
                                 !strcmp(w[0], "ellipse") || !strncmp(w[0], "closest", 7) || !strncmp(w[0], "farthest", 8) ||
                                 isdigit((unsigned char)w[0][0]) || w[0][0] == '-' || !strncmp(w[0], "calc(", 5));
        if (!config || *c != ',') { free(first); buf_add(&b, p, (size_t)(a - p)); p = a; continue; }
        Buf o = { 0 };
        int i = 0;
        bool ellipse_kw = false, sized = false;
        int nsize = 0;
        for (; i < nw && strcmp(w[i], "at") && strcmp(w[i], "in") && strcmp(w[i], "from"); i++) {
            if (!strcmp(w[i], "ellipse")) { ellipse_kw = true; continue; }
            if (!strcmp(w[i], "farthest-corner")) continue;
            if (!strcmp(w[i], "circle")) { if (o.n) buf_ch(&o, ' '); buf_str(&o, "circle"); continue; }
            sized = true;
            nsize++;
            if (o.n) buf_ch(&o, ' ');
            /* (a radius is never below nothing) */
            if (w[i][0] == '-' && strtod(w[i], NULL) < 0) buf_str(&o, "0px"); else buf_str(&o, w[i]);
        }
        (void)ellipse_kw; (void)sized; (void)nsize;
        for (; i < nw; ) {
            if (!strcmp(w[i], "from") && i + 1 < nw) {
                double ang = strtod(w[i + 1], NULL);
                if (!(ang == 0.0 && (!strcmp(w[i + 1], "0deg") || !strcmp(w[i + 1], "0rad") || !strcmp(w[i + 1], "0turn") || !strcmp(w[i + 1], "0grad")))) {
                    if (o.n) buf_ch(&o, ' ');
                    buf_str(&o, "from ");
                    buf_str(&o, w[i + 1]);
                }
                i += 2;
            } else if (!strcmp(w[i], "at") && i + 2 < nw + 1) {
                bool center = i + 2 < nw && !strcmp(w[i + 1], "50%") && !strcmp(w[i + 2], "50%");
                int j = i + 1;
                Buf pos = { 0 };
                for (; j < nw && strcmp(w[j], "in") && strcmp(w[j], "from"); j++) { if (pos.n) buf_ch(&pos, ' '); buf_str(&pos, w[j]); }
                if (!center && pos.n) { if (o.n) buf_ch(&o, ' '); buf_str(&o, "at "); buf_add(&o, pos.s, pos.n); }
                free(pos.s);
                i = j;
            } else {
                if (o.n) buf_ch(&o, ' ');
                buf_str(&o, w[i]);
                i++;
            }
        }
        free(first);
        (void)radial;
        buf_add(&b, p, (size_t)(a - p));
        if (o.n) { buf_add(&b, o.s, o.n); buf_ch(&b, ','); p = c + 1; }
        else { p = c + 1; while (*p == ' ') p++; }
        free(o.s);
        changed = true;
    }
    buf_ch(&b, 0);
    if (!changed || !strcmp(b.s, v)) { free(b.s); return NULL; }
    return b.s;
}

/* Words w[a..b) joined with spaces onto o. */
static void words_out(Buf *o, char (*w)[160], int a, int b)
{
    for (int i = a; i < b; i++) { if (i > a) buf_ch(o, ' '); buf_str(o, w[i]); }
}

/* A position in a shape, from words w[a..b). */
static void shape_pos_words(Buf *o, char (*w)[160], int a, int b)
{
    Buf t = { 0 };
    words_out(&t, w, a, b);
    buf_ch(&t, 0);
    shape_position(t.s, o);
    free(t.s);
}

/* A control point from words w[a..b): a position, or a pair with what it's from
 * (unsaid when it's the default: the start for by, the origin for to). */
static void shape_cp_words(Buf *o, char (*w)[160], int a, int b, bool by)
{
    int f = -1;
    for (int i = a; i < b; i++) if (!strcasecmp(w[i], "from")) f = i;
    if (f < 0) { shape_pos_words(o, w, a, b); return; }
    for (int i = a; i < f; i++) {
        if (i > a) buf_ch(o, ' ');
        if (g_shape_computed) lin_simplify(w[i], sizeof w[i]);
        buf_str(o, w[i]);
    }
    if (f + 1 < b && !strcasecmp(w[f + 1], by ? "start" : "origin")) return;
    buf_ch(o, ' ');
    words_out(o, w, f, b);
}

/* One shape() command, canonical. */
static void shape_cmd(Buf *o, const char *seg, bool first)
{
    /* (a slash its own word) */
    Buf s = { 0 };
    for (const char *c = seg; *c; c++) { if (*c == '/') buf_str(&s, " / "); else buf_ch(&s, *c); }
    buf_ch(&s, 0);
    char w[32][160];
    int n = canon_words(s.s, w, 32);
    free(s.s);
    if (n < 1) return;
    if (first) {
        int i = 0;
        if (!strcasecmp(w[0], "nonzero")) i = 1;
        else if (!strcasecmp(w[0], "evenodd")) { buf_str(o, "evenodd "); i = 1; }
        if (i < n && !strcasecmp(w[i], "from")) { buf_str(o, "from "); shape_pos_words(o, w, i + 1, n); }
        else words_out(o, w, i, n);
        return;
    }
    const char *cmd = w[0];
    bool to = n > 1 && !strcasecmp(w[1], "to"), by = n > 1 && !strcasecmp(w[1], "by");
    if (!to && !by) { words_out(o, w, 0, n); return; }
    buf_str(o, cmd);
    buf_ch(o, ' ');
    buf_str(o, w[1]);
    buf_ch(o, ' ');
    if (!strcasecmp(cmd, "hline") || !strcasecmp(cmd, "vline")) {
        if (to && g_shape_computed && n == 3) {
            const char *k = w[2];
            if (!strcasecmp(k, "left") || !strcasecmp(k, "top") || !strcasecmp(k, "x-start") || !strcasecmp(k, "y-start")) { buf_str(o, "0%"); return; }
            if (!strcasecmp(k, "right") || !strcasecmp(k, "bottom") || !strcasecmp(k, "x-end") || !strcasecmp(k, "y-end")) { buf_str(o, "100%"); return; }
            if (!strcasecmp(k, "center")) { buf_str(o, "50%"); return; }
        }
        words_out(o, w, 2, n);
        return;
    }
    /* (where the end point's words stop) */
    int e = 2;
    while (e < n && strcasecmp(w[e], "with") && strcasecmp(w[e], "of")) e++;
    if (to) shape_pos_words(o, w, 2, e);
    else {
        for (int i = 2; i < e; i++) { if (i > 2) buf_ch(o, ' '); if (g_shape_computed) lin_simplify(w[i], sizeof w[i]); buf_str(o, w[i]); }
    }
    if (e >= n) return;
    if (!strcasecmp(w[e], "with")) {
        buf_str(o, " with ");
        int sl = e + 1;
        while (sl < n && strcmp(w[sl], "/")) sl++;
        shape_cp_words(o, w, e + 1, sl, by);
        if (sl < n) { buf_str(o, " / "); shape_cp_words(o, w, sl + 1, n, by); }
        return;
    }
    /* (arc: its radii, then sweep, size and rotation, the defaults unsaid) */
    int r = e + 1;
    const char *sweep = NULL, *size = NULL, *rot = NULL;
    buf_str(o, " of");
    for (; r < n; r++) {
        if (!strcasecmp(w[r], "cw") || !strcasecmp(w[r], "ccw")) sweep = w[r];
        else if (!strcasecmp(w[r], "large") || !strcasecmp(w[r], "small")) size = w[r];
        else if (!strcasecmp(w[r], "rotate") && r + 1 < n) rot = w[++r];
        else { buf_ch(o, ' '); buf_str(o, w[r]); }
    }
    if (sweep && !strcasecmp(sweep, "cw")) buf_str(o, " cw");
    if (size && !strcasecmp(size, "large")) buf_str(o, " large");
    if (rot) {
        char *ue = NULL;
        double a = strtod(rot, &ue);
        bool zero = ue != rot && a == 0.0 && (!strcmp(ue, "deg") || !strcmp(ue, "rad") || !strcmp(ue, "grad") || !strcmp(ue, "turn") || !*ue);
        if (!zero) { buf_str(o, " rotate "); buf_str(o, rot); }
    }
}

/* Each shape() in v canonical; NULL if there's none to change. */
static char *shape_fn_canon(const char *v)
{
    Buf b = { 0 };
    const char *p = v;
    bool any = false;
    for (const char *f; (f = strstr(p, "shape(")) != NULL; ) {
        if (f > v && (isalnum((unsigned char)f[-1]) || f[-1] == '-')) { buf_add(&b, p, (size_t)(f + 6 - p)); p = f + 6; continue; }
        buf_add(&b, p, (size_t)(f + 6 - p));
        const char *c = f + 6, *seg = c;
        int d = 1;
        bool firstseg = true;
        for (; *c && d > 0; c++) {
            if (*c == '(') d++;
            else if (*c == ')') d--;
            if ((*c == ',' && d == 1) || d == 0) {
                char *sg = strndup(seg, (size_t)(c - seg));
                if (!firstseg) buf_str(&b, ", ");
                shape_cmd(&b, sg, firstseg);
                free(sg);
                firstseg = false;
                seg = c + 1;
                if (d == 0) { buf_ch(&b, ')'); c++; break; }
            }
        }
        if (d) { free(b.s); return NULL; }
        any = true;
        p = c;
    }
    if (!any) { free(b.s); return NULL; }
    buf_str(&b, p);
    buf_ch(&b, 0);
    if (!strcmp(b.s, v)) { free(b.s); return NULL; }
    return b.s;
}

char *css_shape_computed(const char *n, const char *v)
{
    g_shape_computed = true;
    char *r = strstr(v, "shape(") ? shape_fn_canon(v) : canon_basic_shape(n, v);
    g_shape_computed = false;
    return r;
}

/* grid-template-areas: its strings' cells spaced, "" if not rows of one length with
 * every name a rectangle. */
static char *canon_grid_areas(const char *v)
{
    enum { MAX_CELLS = 400, MAX_NAME = 48 };
    static __thread char cells[MAX_CELLS][MAX_NAME];
    int rows = 0, cols = -1, nc = 0;
    Buf b = { 0 };
    for (const char *q = v; *q; ) {
        if (isspace((unsigned char)*q)) { q++; continue; }
        if (*q != '"') { free(b.s); return strdup(""); }
        const char *e = strchr(q + 1, '"');
        if (!e) { free(b.s); return strdup(""); }
        int here = 0;
        if (rows) buf_ch(&b, ' ');
        buf_ch(&b, '"');
        for (const char *c = q + 1; c < e; ) {
            if (isspace((unsigned char)*c)) { c++; continue; }
            /* (white space written as an escape: still white space) */
            if (*c == 92 && c + 1 < e) {
                const char *h = c + 1, *he = h;
                while (he < e && he - h < 6 && isxdigit((unsigned char)*he)) he++;
                long cp = he > h ? strtol(h, NULL, 16) : -1;
                if (cp == 9 || cp == 10 || cp == 12 || cp == 13 || cp == 32) {
                    c = he;
                    if (c < e && *c == ' ') c++;
                    continue;
                }
            }
            char name[MAX_NAME];
            size_t l = 0;
            if (*c == '.') {
                while (c < e && *c == '.') c++;
                name[l++] = '.';
            } else {
                const char *st = c;
                while (c < e && (isalnum((unsigned char)*c) || *c == '-' || *c == '_' || (unsigned char)*c >= 0x80 || *c == 92)) c++;
                if (c == st || (size_t)(c - st) >= MAX_NAME) { free(b.s); return c == st ? strdup("") : NULL; }
                l = (size_t)(c - st);
                memcpy(name, st, l);
            }
            name[l] = 0;
            if (nc >= MAX_CELLS) { free(b.s); return NULL; }
            memcpy(cells[nc++], name, l + 1);
            if (here++) buf_ch(&b, ' ');
            buf_str(&b, name);
        }
        buf_ch(&b, '"');
        if (!here || (cols >= 0 && here != cols)) { free(b.s); return strdup(""); }
        cols = here;
        rows++;
        q = e + 1;
    }
    if (!rows) { free(b.s); return strdup(""); }
    /* (each name fills its bounding box) */
    for (int i = 0; i < nc; i++) {
        if (cells[i][0] == '.') continue;
        bool first = true;
        for (int j = 0; j < i && first; j++) if (!strcmp(cells[i], cells[j])) first = false;
        if (!first) continue;
        int r0 = i / cols, r1 = r0, c0 = i % cols, c1 = c0, count = 0;
        for (int j = 0; j < nc; j++) {
            if (strcmp(cells[i], cells[j])) continue;
            int r = j / cols, c = j % cols;
            if (r < r0) r0 = r;
            if (r > r1) r1 = r;
            if (c < c0) c0 = c;
            if (c > c1) c1 = c;
            count++;
        }
        if (count != (r1 - r0 + 1) * (c1 - c0 + 1)) { free(b.s); return strdup(""); }
    }
    buf_ch(&b, 0);
    if (!strcmp(b.s, v)) { free(b.s); return NULL; }
    return b.s;
}

/* position-try-fallbacks: "" if invalid. */
static char *canon_try_fallbacks(const char *v)
{
    char **parts = NULL;
    int np = split_commas(v, &parts);
    Buf b = { 0 };
    bool bad = np < 1;
    for (int k = 0; k < np && !bad; k++) {
        char w[8][160];
        int n = canon_words(parts[k], w, 8);
        if (n < 1) { bad = true; break; }
        if (n == 1 && !strcasecmp(w[0], "none")) { if (np > 1) bad = true; else buf_str(&b, "none"); continue; }
        const char *dashed = NULL;
        int nf = 0, flips[8];
        bool others = false;
        for (int i = 0; i < n; i++) {
            if (!strncmp(w[i], "--", 2)) { if (dashed || (i != 0 && i != n - 1)) bad = true; dashed = w[i]; }
            else if (!strcasecmp(w[i], "flip-block") || !strcasecmp(w[i], "flip-inline") || !strcasecmp(w[i], "flip-start") || !strcasecmp(w[i], "flip-x") || !strcasecmp(w[i], "flip-y")) {
                for (int q = 0; q < nf; q++) if (!strcasecmp(w[flips[q]], w[i])) bad = true;
                flips[nf++] = i;
            } else others = true;
        }
        if (k) buf_str(&b, ", ");
        if (others) {
            if (dashed || nf) { bad = true; break; }
            char *pa = position_area_canon(parts[k]);
            if (!pa || !strcasecmp(pa, "none")) { free(pa); bad = true; break; }
            buf_str(&b, pa);
            free(pa);
            continue;
        }
        bool sp = false;
        if (dashed) { buf_str(&b, dashed); sp = true; }
        for (int q = 0; q < nf; q++) { if (sp) buf_ch(&b, ' '); buf_str(&b, w[flips[q]]); sp = true; }
    }
    for (int k = 0; k < np; k++) free(parts[k]);
    free(parts);
    if (bad) { free(b.s); return strdup(""); }
    buf_ch(&b, 0);
    return b.s;
}

/* grid-template-*: what the grammar's looseness lets through. true: invalid. */
static bool grid_tracks_bad(const char *v)
{
    if (!strcasecmp(v, "none") || !strncasecmp(v, "subgrid", 7)) return false;
    int autos = 0;
    bool has_track = false;
    const char *p = v;
    /* (fit-content without its length, a negative, names that can't be) */
    char *copy = strdup(v);
    bool bad = false;
    for (char *q = copy; *q; q++) {
        if (*q == '[') {
            char *e = strchr(q, ']');
            if (!e) break;
            char name[64];
            size_t l = (size_t)(e - q - 1);
            if (l < sizeof name) {
                memcpy(name, q + 1, l);
                name[l] = 0;
                char w[8][160];
                int n = canon_words(name, w, 8);
                for (int i = 0; i < n; i++) if (!strcasecmp(w[i], "auto") || !strcasecmp(w[i], "span")) bad = true;
            }
            memset(q, ' ', (size_t)(e - q + 1));
        }
    }
    int in_calc = 0;
    for (p = copy; *p; p++) {
        bool edge = p == copy || !(isalnum((unsigned char)p[-1]) || p[-1] == '-');
        if (edge && !strncasecmp(p, "repeat(auto-fi", 14)) autos++;
        if (edge && (!strncasecmp(p, "calc(", 5) || !strncasecmp(p, "min(", 4) || !strncasecmp(p, "max(", 4) || !strncasecmp(p, "clamp(", 6)) && !in_calc) in_calc = 1;
        else if (in_calc && *p == '(') in_calc++;
        else if (in_calc && *p == ')') in_calc--;
        if (!in_calc && *p == '-' && edge && (isdigit((unsigned char)p[1]) || p[1] == '.')) bad = true;
        if (edge && !strncasecmp(p, "fit-content", 11) && p[11] != '(') bad = true;
        if (edge && !strncasecmp(p, "minmax(fit-content", 18)) bad = true;
        if (!isspace((unsigned char)*p)) has_track = true;
    }
    if (!has_track || autos > 1) bad = true;
    if (autos == 1 && !bad) {
        /* (an auto repeat's own may be intrinsic; beside one, every track has a fixed size) */
        char *ar = strstr(copy, "repeat(auto-fi");
        bool own_intrinsic = false;
        if (ar) {
            char *e = ar + 7;
            int d = 1;
            for (; *e && d; e++) { if (*e == '(') d++; else if (*e == ')') d--; }
            for (char *k = ar + 7; k < e; k++) if (!strncasecmp(k, "content", 7) || (!strncasecmp(k, "auto", 4) && strncasecmp(k, "auto-fi", 7))) own_intrinsic = true;
            memset(ar, ' ', (size_t)(e - ar));
        }
        (void)own_intrinsic;
        for (p = copy; *p; p++) {
            bool edge = p == copy || !(isalnum((unsigned char)p[-1]) || p[-1] == '-');
            if (!edge) continue;
            if (!strncasecmp(p, "minmax(", 7)) {
                const char *c = p + 7, *e = c;
                int d = 1, comma = -1;
                for (; *e && d; e++) {
                    if (*e == '(') d++;
                    else if (*e == ')') d--;
                    else if (*e == ',' && d == 1) comma = (int)(e - c);
                }
                if (comma < 0) continue;
                #define GT_FIXED(s, n) ({ char t_[64]; size_t l_ = (size_t)(n) < 63 ? (size_t)(n) : 63; memcpy(t_, s, l_); t_[l_] = 0; !(strstr(t_, "auto") || strstr(t_, "content") || strstr(t_, "fr")); })
                bool first_fixed = GT_FIXED(c, comma), second_fixed = GT_FIXED(c + comma + 1, (e - 1) - (c + comma + 1));
                #undef GT_FIXED
                if (!first_fixed && !second_fixed) bad = true;
                p = e - 1;
                continue;
            }
            if (!strncasecmp(p, "min-content", 11) || !strncasecmp(p, "max-content", 11) || !strncasecmp(p, "fit-content", 11)) bad = true;
            if (!strncasecmp(p, "auto", 4) && strncasecmp(p, "auto-fi", 7)) bad = true;
        }
        if (strstr(copy, "fr") && !strstr(copy, "minmax(")) {
            for (p = copy; (p = strstr(p, "fr")) != NULL; p += 2) if (p > copy && (isdigit((unsigned char)p[-1]) || p[-1] == '.')) bad = true;
        }
    }
    free(copy);
    return bad;
}

/* One math argument in canonical form, as calc() would have it (without
 * calc() round a single term); NULL when it won't go. */
static char *math_arg_canon(const char *x, size_t n)
{
    Buf in = { 0 };
    buf_str(&in, "calc(");
    buf_add(&in, x, n);
    buf_ch(&in, ')');
    buf_ch(&in, 0);
    Toks ts;
    char *out = NULL;
    if (tokenize(in.s, strlen(in.s), &ts)) {
        int i = 0;
        while (i < ts.n && ts.t[i].type != T_FUNC) i++;
        if (i < ts.n) out = math_fn_text(&ts, i);
        toks_free(&ts);
    }
    free(in.s);
    if (out && !strncmp(out, "calc(", 5)) {
        size_t l = strlen(out);
        const char *b = out + 5;
        int d = 0;
        bool sum = false;
        for (const char *c = b; c < out + l - 1; c++) {
            if (*c == '(') d++; else if (*c == ')') d--;
            else if (!d && (*c == ' ') && (c[1] == '+' || c[1] == '-') && c[2] == ' ') sum = true;
        }
        if (!sum && !strchr(b, ' ')) { memmove(out, b, l - 5); out[l - 6] = 0; }
    }
    return out;
}

/* random()'s options as they serialize, knowing the property. */
static char *canon_random_args(const char *v);
static char *canon_random_keys(const char *prop, const char *v);
static char *canon_random(const char *prop, const char *v)
{
    char *o = canon_random_keys(prop, v);
    char *a = canon_random_args(o ? o : v);
    if (a) { free(o); return a; }
    return o;
}
/* (each argument after the options: in canonical form) */
static char *canon_random_args(const char *v)
{
    if (!strstr(v, "random(")) return NULL;
    Buf b = { 0 };
    bool changed = false;
    const char *q = v;
    for (const char *r; (r = strstr(q, "random(")); ) {
        const char *open = r + 7, *e = open;
        int d = 1;
        for (; *e; e++) { if (*e == '(') d++; else if (*e == ')' && !--d) break; }
        if (!*e) break;
        buf_add(&b, q, (size_t)(open - q));
        /* (the arguments, split at the top level) */
        const char *s = open;
        int k = 0;
        for (const char *c = open; ; c++) {
            if (c == e || (*c == ',' && !d)) {
                const char *a0 = s, *a1 = c;
                while (a0 < a1 && isspace((unsigned char)*a0)) a0++;
                while (a1 > a0 && isspace((unsigned char)a1[-1])) a1--;
                if (k) buf_str(&b, ", ");
                bool opt = k == 0 && a1 > a0 && (isalpha((unsigned char)*a0) || (*a0 == '-' && a0[1] == '-')) &&
                           strncasecmp(a0, "calc(", 5) && strncasecmp(a0, "min(", 4) && strncasecmp(a0, "max(", 4) &&
                           strncasecmp(a0, "e ", 2) && strncasecmp(a0, "pi", 2) && strncasecmp(a0, "infinity", 8);
                char *cv = !opt && a1 > a0 && !memchr(a0, '(', (size_t)(a1 - a0)) ? math_arg_canon(a0, (size_t)(a1 - a0)) : NULL;
                if (cv && strncmp(cv, "calc(", 5) && (strlen(cv) != (size_t)(a1 - a0) || memcmp(cv, a0, (size_t)(a1 - a0)))) { buf_str(&b, cv); changed = true; }
                else buf_add(&b, a0, (size_t)(a1 - a0));
                free(cv);
                k++;
                s = c + 1;
                if (c == e) break;
                continue;
            }
            if (*c == '(') d++;
            else if (*c == ')') d--;
        }
        buf_ch(&b, ')');
        q = e + 1;
    }
    buf_str(&b, q);
    buf_ch(&b, 0);
    if (!changed) { free(b.s); return NULL; }
    return b.s;
}
static char *canon_random_keys(const char *prop, const char *v)
{
    if (!strstr(v, "random(")) return NULL;
    Buf b = { 0 };
    const char *q = v;
    for (const char *r; (r = strstr(q, "random(")); ) {
        const char *open = r + 7, *e = open;
        int d = 1;
        for (; *e; e++) {
            if (*e == '(') d++;
            else if (*e == ')' && !--d) break;
            else if (*e == ',' && d == 1) break;
        }
        buf_add(&b, q, (size_t)(open - q));
        q = open;
        if (*e != ',') continue;
        {
            const char *f0 = open;
            while (isspace((unsigned char)*f0)) f0++;
            if (isdigit((unsigned char)*f0) || *f0 == '.' || *f0 == '+' || (*f0 == '-' && f0[1] != '-') || !strncasecmp(f0, "calc(", 5) || !strncasecmp(f0, "min(", 4) || !strncasecmp(f0, "max(", 4)) {
                buf_str(&b, "element-scoped ua-");
                buf_str(&b, prop);
                buf_str(&b, "-1, ");
                continue;
            }
        }
        char opt[200];
        size_t ol = (size_t)(e - open);
        if (ol >= sizeof opt) continue;
        memcpy(opt, open, ol);
        opt[ol] = 0;
        char w[6][160];
        int n = canon_words(opt, w, 6);
        if (n < 1 || (!isalpha((unsigned char)w[0][0]) && w[0][0] != '-') || !strcasecmp(w[0], "fixed") || !strcasecmp(w[0], "e") || !strcasecmp(w[0], "pi") || !strcasecmp(w[0], "infinity")) continue;
        if (!strncasecmp(w[0], "calc", 4) || strchr(w[0], '(')) continue;
        const char *dashed = NULL, *ua = NULL;
        bool elem = false, pscoped = false, pindex = false, aut = false;
        for (int i = 0; i < n; i++) {
            if (!strncmp(w[i], "--", 2)) dashed = w[i];
            else if (!strncasecmp(w[i], "ua-", 3)) ua = w[i];
            else if (!strcasecmp(w[i], "element-scoped")) elem = true;
            else if (!strcasecmp(w[i], "property-scoped")) pscoped = true;
            else if (!strcasecmp(w[i], "property-index-scoped")) pindex = true;
            else if (!strcasecmp(w[i], "auto")) aut = true;
        }
        if (aut) { elem = true; pindex = true; }
        bool sp = false;
        if (dashed) { buf_str(&b, dashed); sp = true; }
        if (elem) { if (sp) buf_ch(&b, ' '); buf_str(&b, "element-scoped"); sp = true; }
        if (ua) { if (sp) buf_ch(&b, ' '); buf_str(&b, ua); sp = true; }
        else if (pscoped || pindex) {
            if (sp) buf_ch(&b, ' ');
            buf_str(&b, "ua-");
            buf_str(&b, prop);
            if (pindex) buf_str(&b, "-1");
        }
        q = e;
    }
    buf_str(&b, q);
    buf_ch(&b, 0);
    return b.s;
}

/* image-set(): each image a url, each with its resolution. */
static char *canon_image_set(const char *v)
{
    const char *at = strstr(v, "image-set(");
    if (!at) return NULL;
    Buf b = { 0 };
    const char *q = v;
    while (at) {
        const char *open = at + 10, *e = open;
        int d = 1;
        char qc = 0;
        for (; *e; e++) {
            if (qc) { if (*e == qc) qc = 0; continue; }
            if (*e == '"' || *e == '\'') qc = *e;
            else if (*e == '(') d++;
            else if (*e == ')' && !--d) break;
        }
        if (!*e) { free(b.s); return NULL; }
        buf_add(&b, q, (size_t)(open - q));
        char *inner = strndup(open, (size_t)(e - open));
        char **parts = NULL;
        int np = split_commas(inner, &parts);
        free(inner);
        for (int k = 0; k < np; k++) {
            char w[6][160];
            int n = canon_words(parts[k], w, 6);
            const char *img = NULL, *res = NULL, *type = NULL;
            for (int i = 0; i < n; i++) {
                if (w[i][0] == '"' || w[i][0] == '\'' || !strncasecmp(w[i], "url(", 4) || strstr(w[i], "gradient(") || !strncasecmp(w[i], "image(", 6)) img = w[i];
                else if (!strncasecmp(w[i], "type(", 5)) type = w[i];
                else res = w[i];
            }
            /* (a resolution below nothing: no such option) */
            if (res && res[0] == '-' && (isdigit((unsigned char)res[1]) || res[1] == '.')) {
                for (int j = k; j < np; j++) free(parts[j]);
                free(parts);
                free(b.s);
                return strdup("");
            }
            if (k) buf_str(&b, ", ");
            if (!img) { for (int i = 0; i < n; i++) { if (i) buf_ch(&b, ' '); buf_str(&b, w[i]); } free(parts[k]); continue; }
            size_t il = strlen(img);
            if (img[0] == '"' || img[0] == '\'') { buf_str(&b, "url(\""); buf_add(&b, img + 1, il - 2); buf_str(&b, "\")"); }
            else if (!strncasecmp(img, "url(", 4) && img[4] != '"' && img[4] != '\'') { buf_str(&b, "url(\""); buf_add(&b, img + 4, il - 5); buf_str(&b, "\")"); }
            else if (!strncasecmp(img, "url('", 5)) { buf_str(&b, "url(\""); buf_add(&b, img + 5, il - 7); buf_str(&b, "\")"); }
            else buf_str(&b, img);
            buf_ch(&b, ' ');
            buf_str(&b, res ? res : "1x");
            if (type) {
                buf_ch(&b, ' ');
                buf_str(&b, "type(\"");
                const char *ts = type + 5;
                size_t tl = strlen(ts) - 1;
                if (tl >= 2 && (ts[0] == '"' || ts[0] == '\'')) { ts++; tl -= 2; }
                buf_add(&b, ts, tl);
                buf_str(&b, "\")");
            }
            free(parts[k]);
        }
        free(parts);
        q = e;
        at = strstr(q, "image-set(");
    }
    buf_str(&b, q);
    buf_ch(&b, 0);
    return b.s;
}

/* palette-mix(): the default space gone, the second percentage filled in; "" if invalid. */
/* linear() easing in a timing function list: two stops or more; computed,
 * each stop one point with its position (the unsaid spread evenly between
 * the said, the first 0%, the last 100%, none before the one before it).
 * NULL: as it is; "": invalid. */
static void lin_num(Buf *b, double v) {
    char t[48];
    snprintf(t, sizeof t, "%.6f", v);
    char *e = t + strlen(t) - 1;
    while (e > t && *e == '0') *e-- = 0;
    if (*e == '.') *e = 0;
    if (!strcmp(t, "-0")) strcpy(t, "0");
    buf_str(b, t);
}
static double lin_val(const char *w) {
    if (!strncasecmp(w, "calc(", 5)) w += 5;
    return strtod(w, NULL);
}
static char *canon_linear(const char *v, bool computed)
{
    if (!strstr(v, "linear(")) return NULL;
    Buf b = { 0 };
    bool changed = false;
    for (const char *p = v; *p; ) {
        if (strncasecmp(p, "linear(", 7) || (p > v && (isalnum((unsigned char)p[-1]) || p[-1] == '-'))) { buf_ch(&b, *p++); continue; }
        const char *a = p + 7, *e = a;
        int d = 1;
        for (; *e && d; e++) { if (*e == '(') d++; else if (*e == ')' && --d == 0) break; }
        if (!*e) { buf_ch(&b, *p++); continue; }
        char *inner = strndup(a, (size_t)(e - a));
        char **parts = NULL;
        int np = split_commas(inner, &parts);
        free(inner);
        if (np < 2) { for (int k = 0; k < np; k++) free(parts[k]); free(parts); free(b.s); return strdup(""); }
        if (!computed) {
            for (int k = 0; k < np; k++) free(parts[k]);
            free(parts);
            buf_add(&b, p, (size_t)(e + 1 - p));
            p = e + 1;
            continue;
        }
        /* (the points: an output and a position, or none yet) */
        int cap = np * 2, n = 0;
        double *out = calloc((size_t)cap, sizeof(double)), *pos = calloc((size_t)cap, sizeof(double));
        bool *has = calloc((size_t)cap, sizeof(bool));
        for (int k = 0; k < np; k++) {
            char w[3][160];
            int nw = canon_words(parts[k], w, 3);
            double o = 0;
            double ps[2];
            int npc = 0;
            for (int i = 0; i < nw; i++) {
                if (strchr(w[i], '%')) { if (npc < 2) ps[npc++] = lin_val(w[i]); }
                else { o = lin_val(w[i]); if (o != o) o = 0; }
            }
            if (npc == 0) { out[n] = o; has[n] = false; n++; }
            for (int i = 0; i < npc; i++) { out[n] = o; pos[n] = ps[i]; has[n] = true; n++; }
        }
        for (int k = 0; k < np; k++) free(parts[k]);
        free(parts);
        if (!has[0]) { pos[0] = 0; has[0] = true; }
        if (!has[n - 1]) { pos[n - 1] = 100; has[n - 1] = true; }
        /* (never before the largest so far) */
        double mx = pos[0];
        for (int i = 0; i < n; i++) if (has[i]) { if (pos[i] < mx) pos[i] = mx; mx = pos[i]; }
        for (int i = 0; i < n; ) {
            if (has[i]) { i++; continue; }
            int j = i;
            while (!has[j]) j++;
            double from = pos[i - 1], to = pos[j];
            for (int k = i; k < j; k++) pos[k] = from + (to - from) * (k - i + 1) / (j - i + 1);
            i = j;
        }
        buf_str(&b, "linear(");
        for (int i = 0; i < n; i++) {
            if (i) buf_str(&b, ", ");
            lin_num(&b, out[i]);
            buf_ch(&b, ' ');
            lin_num(&b, pos[i]);
            buf_ch(&b, '%');
        }
        buf_ch(&b, ')');
        free(out); free(pos); free(has);
        changed = true;
        p = e + 1;
    }
    buf_ch(&b, 0);
    if (!changed || !strcmp(b.s, v)) { free(b.s); return NULL; }
    return b.s;
}

static char *canon_palette_mix(const char *v)
{
    size_t vl = strlen(v);
    if (strncasecmp(v, "palette-mix(", 12) || vl < 13 || v[vl - 1] != ')') return strdup("");
    char *inner = strndup(v + 12, vl - 13);
    char **parts = NULL;
    int np = split_commas(inner, &parts);
    free(inner);
    char *out = NULL;
    Buf b = { 0 };
    bool bad = np < 1;
    int first = 0;
    char space[120] = "";
    if (!bad) {
        char w[5][160];
        int n = canon_words(parts[0], w, 5);
        if (n && !strcasecmp(w[0], "in")) {
            first = 1;
            static const char *const rect[] = { "srgb", "srgb-linear", "display-p3", "display-p3-linear", "a98-rgb", "prophoto-rgb", "rec2020", "lab", "oklab", "xyz", "xyz-d50", "xyz-d65" };
            static const char *const polar[] = { "hsl", "hwb", "lch", "oklch" };
            bool is_rect = false, is_polar = false;
            for (size_t i = 0; n > 1 && i < sizeof rect / sizeof rect[0]; i++) if (!strcasecmp(w[1], rect[i])) is_rect = true;
            for (size_t i = 0; n > 1 && i < sizeof polar / sizeof polar[0]; i++) if (!strcasecmp(w[1], polar[i])) is_polar = true;
            if (is_rect && n != 2) bad = true;
            if (!is_rect && !is_polar) bad = true;
            if (is_polar && n != 2 && !(n == 4 && !strcasecmp(w[3], "hue") && (!strcasecmp(w[2], "shorter") || !strcasecmp(w[2], "longer") || !strcasecmp(w[2], "increasing") || !strcasecmp(w[2], "decreasing")))) bad = true;
            if (!bad && strcasecmp(w[1], "oklab")) {
                bool hue = n == 4 && strcasecmp(w[2], "shorter");
                snprintf(space, sizeof space, "in %s%s%s%s", !strcasecmp(w[1], "xyz") ? "xyz-d65" : w[1], hue ? " " : "", hue ? w[2] : "", hue ? " hue" : "");
            }
        }
        if (np - first < 1) bad = true;
    }
    int ni = np - first;
    double pct[16];
    bool has[16];
    const char *name[16];
    char (*words)[2][160] = bad || ni > 16 ? NULL : calloc((size_t)ni, sizeof *words);
    if (!words) bad = true;
    for (int i = 0; !bad && i < ni; i++) {
        char w[4][160];
        int n = canon_words(parts[first + i], w, 4);
        if (n < 1 || n > 2) { bad = true; break; }
        int pi = -1;
        for (int k = 0; k < n; k++) if (isdigit((unsigned char)w[k][0]) || (w[k][0] == '-' && w[k][1] != '-') || w[k][0] == '.' || !strncasecmp(w[k], "calc(", 5)) pi = k;
        if (n == 2 && pi < 0) { bad = true; break; }
        const char *nm = w[pi == 0 ? 1 : 0];
        if (n == 1 && pi == 0) { bad = true; break; }
        (void)0;
        if (strncasecmp(nm, "palette-mix(", 12) && strncmp(nm, "--", 2) && strcasecmp(nm, "normal") && strcasecmp(nm, "light") && strcasecmp(nm, "dark")) { bad = true; break; }
        snprintf(words[i][0], 160, "%s", nm);
        name[i] = words[i][0];
        has[i] = pi >= 0;
        pct[i] = 0.0;
        if (has[i]) {
            const char *t = w[pi];
            if (!strncasecmp(t, "calc(", 5)) t += 5;
            pct[i] = strtod(t, NULL);
            snprintf(words[i][1], 160, "%s", w[pi]);
            if (pct[i] < 0.0 || pct[i] > 100.0) bad = true;
        }
    }
    double fillv = 0.0;
    bool fill = false;
    if (!bad) {
        if (space[0]) { buf_str(&b, space); buf_str(&b, ", "); }
        /* (the unsaid share what the said leave, unless a calc() says it) */
        int nsaid = 0, ncalc = 0;
        double sum = 0.0;
        for (int i = 0; i < ni; i++) if (has[i]) { nsaid++; sum += pct[i]; if (!strncasecmp(words[i][1], "calc(", 5)) ncalc++; }
        if (nsaid && nsaid < ni && !ncalc) { fill = true; fillv = (100.0 - sum) / (ni - nsaid); if (fillv < 0) fillv = 0; }
        /* (every one equal and the whole: unsaid) */
        bool even = !ncalc && ni <= 2;
        for (int i = 0; even && i < ni; i++) even = fabs((has[i] ? pct[i] : fill ? fillv : 100.0 / ni) - 100.0 / ni) < 1e-9;
        if (even && (nsaid || fill)) { fill = false; for (int i = 0; i < ni; i++) has[i] = false; }
        for (int i = 0; !bad && i < ni; i++) {
            if (i) buf_str(&b, ", ");
            if (!strncasecmp(name[i], "palette-mix(", 12)) {
                char *c = canon_palette_mix(name[i]);
                if (!c || !c[0]) bad = true;
                else buf_str(&b, c);
                free(c);
            } else buf_str(&b, name[i]);
            char num[48];
            if (has[i]) { buf_ch(&b, ' '); buf_str(&b, words[i][1]); }
            else if (fill) { snprintf(num, sizeof num, " %.6g%%", fillv); buf_str(&b, num); }
        }
    }
    free(words);
    for (int k = 0; k < np; k++) free(parts[k]);
    free(parts);
    if (bad) { free(b.s); return strdup(""); }
    buf_ch(&b, 0);
    out = (char *)malloc(strlen(b.s) + 15);
    if (out) sprintf(out, "palette-mix(%s)", b.s);
    free(b.s);
    return out;
}

/* A value's form where its grammar leaves several ways to write it (NULL: as it is). */
static char *css_prop_canon_in(const CssPropDef *p, const char *v);
static char *css_prop_canon(const CssPropDef *p, const char *v)
{
    const char *n = p->name;
    char *pre = NULL;
    /* (symbols(): symbolic, the default, unsaid) */
    if ((!strcmp(n, "content") || !strcmp(n, "list-style-type") || !strcmp(n, "list-style")) && strcasestr_simple(v, "symbols(symbolic ")) {
        Buf s = { 0 };
        for (const char *q = v; *q; ) {
            if (!strncasecmp(q, "symbols(symbolic ", 17)) { buf_str(&s, "symbols("); q += 17; continue; }
            buf_ch(&s, *q++);
        }
        pre = buf_take(&s);
    }
    /* (a calc() of a tree-counting function alone: the function) */
    if (strstr(pre ? pre : v, "calc(sibling-")) {
        const char *src = pre ? pre : v;
        Buf s = { 0 };
        for (const char *q = src; *q; ) {
            if (!strncmp(q, "calc(sibling-index())", 21)) { buf_str(&s, "sibling-index()"); q += 21; continue; }
            if (!strncmp(q, "calc(sibling-count())", 21)) { buf_str(&s, "sibling-count()"); q += 21; continue; }
            buf_ch(&s, *q++);
        }
        free(pre);
        pre = buf_take(&s);
    }
    char *r = css_prop_canon_in(p, pre ? pre : v);
    if (pre) { if (!r) return pre; free(pre); }
    return r;
}
static char *css_prop_canon_in(const CssPropDef *p, const char *v)
{
    const char *n = p->name;
    if (!strcmp(n, "display")) return canon_display(v);
    if (!strcmp(n, "aspect-ratio")) {
        /* (auto first; a lone number is that number over 1) */
        char w[4][160];
        int nw = canon_words(v, w, 4);
        bool au = false;
        Buf s = { 0 };
        int rest = 0;
        for (int i = 0; i < nw; i++) if (!strcasecmp(w[i], "auto")) au = true; else rest++;
        if (!rest || nw > 4) return NULL;
        bool slash = false;
        if (au) buf_str(&s, "auto");
        for (int i = 0; i < nw; i++) {
            if (!strcasecmp(w[i], "auto")) continue;
            if (!strcmp(w[i], "/")) slash = true;
            if (s.n) buf_ch(&s, ' ');
            buf_str(&s, w[i]);
        }
        if (!slash && rest == 1) buf_str(&s, " / 1");
        char *o = buf_take(&s);
        if (o && !strcmp(o, v)) { free(o); return NULL; }
        return o;
    }
    if (!strcmp(n, "vertical-align")) {
        /* first or last; how it's aligned, if not as the baseline; the shift, if any */
        char w[3][160], *fl = NULL, *al = NULL, *sh = NULL;
        int nw = canon_words(v, w, 3);
        /* (one word: as it is, a length going between lengths) */
        if (nw < 2) return NULL;
        for (int i = 0; i < nw; i++) {
            if (!strcasecmp(w[i], "first") || !strcasecmp(w[i], "last")) fl = w[i];
            else if (!strcasecmp(w[i], "baseline") || isalpha((unsigned char)w[i][0]) ? strcasecmp(w[i], "sub") && strcasecmp(w[i], "super") && strncasecmp(w[i], "calc", 4) : false) al = w[i];
            else sh = w[i];
        }
        if (al && !strcasecmp(al, "baseline")) al = NULL;
        if (sh && (!strcmp(sh, "0") || !strcmp(sh, "0px"))) sh = NULL;
        char o[400];
        snprintf(o, sizeof o, "%s%s%s%s%s", fl ? fl : "", fl && (al || sh) ? " " : "", al ? al : "", al && sh ? " " : "", sh ? sh : "");
        return strdup(o[0] ? o : "baseline");
    }
    /* (one word: the other axis is the center) */
    if (!g_canon_computed && (!strcmp(n, "object-position") || !strcmp(n, "perspective-origin")) && !strchr(v, ' ') && !strchr(v, '(') &&
        !strstr(v, "x-") && !strstr(v, "y-")) {
        char b[200];
        if (!strcasecmp(v, "top") || !strcasecmp(v, "bottom")) snprintf(b, sizeof b, "center %s", v);
        else if (!strcasecmp(v, "center")) snprintf(b, sizeof b, "center center");
        else snprintf(b, sizeof b, "%s center", v);
        return strdup(b);
    }
    /* (anchor-valid, anchor-visible, no-overflow: in that order) */
    if (!strcmp(n, "position-visibility") && strchr(v, ' ')) {
        static const char *const ord[] = { "anchors-valid", "anchor-valid", "anchors-visible", "anchor-visible", "no-overflow" };
        char b[200] = "";
        for (size_t k = 0; k < sizeof ord / sizeof ord[0]; k++) {
            const char *f = strstr(v, ord[k]);
            bool word = f && (f == v || f[-1] == ' ') && (f[strlen(ord[k])] == 0 || f[strlen(ord[k])] == ' ');
            if (word) { if (b[0]) strcat(b, " "); strcat(b, ord[k]); }
        }
        return b[0] ? strdup(b) : NULL;
    }
    if (!strcmp(n, "object-position") && (strstr(v, "x-start") || strstr(v, "x-end") || strstr(v, "y-start") || strstr(v, "y-end"))) {
        char w[4][160];
        int nw = canon_words(v, w, 4);
        char b[340];
        /* (one word: the other's the center; two pairs: the x axis first) */
        if (nw == 1) {
            snprintf(b, sizeof b, w[0][0] == 'x' ? "%s center" : "center %s", w[0]);
            return strdup(b);
        }
        if (nw == 4 && (w[0][0] == 'y' || !strcasecmp(w[0], "top") || !strcasecmp(w[0], "bottom"))) {
            snprintf(b, sizeof b, "%s %s %s %s", w[2], w[3], w[0], w[1]);
            return strdup(b);
        }
        return NULL;
    }
    if (!strcmp(n, "font-size-adjust") && !strncasecmp(v, "ex-height ", 10)) return strdup(v + 10);
    /* (each function by its name in lower case, its arguments ", " apart; a scale's
     * percentages as numbers, an angle of 0 in degrees) */
    if (!strcmp(n, "transform") && strcmp(v, "none")) {
        Buf s = { 0 };
        const char *q = v;
        bool first = true;
        while (*q) {
            while (isspace((unsigned char)*q)) q++;
            if (!*q) break;
            const char *open = strchr(q, '(');
            if (!open) { free(s.s); return NULL; }
            int d = 0;
            const char *close = open;
            for (; *close; close++) {
                if (*close == '(') d++;
                else if (*close == ')' && --d == 0) break;
            }
            if (!*close) { free(s.s); return NULL; }
            char name[32];
            size_t nl = (size_t)(open - q);
            if (nl >= sizeof name) { free(s.s); return NULL; }
            for (size_t i = 0; i < nl; i++) name[i] = (char)tolower((unsigned char)q[i]);
            name[nl] = 0;
            /* (translations keep their axis letter as the specification writes it) */
            if (!strncmp(name, "translate", 9) && (name[9] == 'x' || name[9] == 'y' || name[9] == 'z') && !name[10]) name[9] = (char)toupper((unsigned char)name[9]);
            bool scale = !strncmp(name, "scale", 5);
            bool angles = !strncmp(name, "rotate", 6) || !strncmp(name, "skew", 4);
            if (!first) buf_ch(&s, ' ');
            first = false;
            buf_str(&s, name);
            buf_ch(&s, '(');
            /* (its arguments, at the top level) */
            int argn = 0;
            const char *arg = open + 1;
            for (const char *c = open + 1; c <= close; c++) {
                if (*c == '(') d++;
                else if (*c == ')' && c != close) d--;
                if ((*c == ',' && d == 0) || c == close) {
                    const char *x = arg, *y = c;
                    while (x < y && isspace((unsigned char)*x)) x++;
                    while (y > x && isspace((unsigned char)y[-1])) y--;
                    if (argn++) buf_str(&s, ", ");
                    char *e = NULL;
                    double num = strtod(x, &e);
                    bool is_angle = angles && (strcmp(name, "rotate3d") || argn == 4);
                    if (scale && e != x && e + 1 == y && *e == '%') fmt_number(&s, num / 100.0);
                    else if (is_angle && e != x && e == y && num == 0.0) buf_str(&s, "0deg");
                    else buf_add(&s, x, (size_t)(y - x));
                    arg = c + 1;
                }
            }
            buf_ch(&s, ')');
            q = close + 1;
        }
        buf_ch(&s, 0);
        if (!strcmp(s.s, v)) { free(s.s); return NULL; }
        return s.s;
    }
    /* (a percentage is a number here; what's the same on each axis is said once) */
    if (!strcmp(n, "scale") && strcmp(v, "none")) {
        char w[3][160];
        int nw = canon_words(v, w, 3);
        Buf s = { 0 };
        double vals[3] = { 1, 1, 1 };
        bool plain = true;
        for (int i = 0; i < nw; i++) {
            char *e = NULL;
            vals[i] = strtod(w[i], &e);
            if (e == w[i] || (*e && *e != '%')) plain = false;
            if (*e == '%') vals[i] /= 100.0;
        }
        if (nw == 1) vals[1] = vals[0];
        if (!plain) { free(s.s); return NULL; }
        int show = vals[2] != 1 ? 3 : vals[0] != vals[1] ? 2 : 1;
        for (int i = 0; i < show; i++) { if (i) buf_ch(&s, ' '); fmt_number(&s, vals[i]); }
        buf_ch(&s, 0);
        if (!strcmp(s.s, v)) { free(s.s); return NULL; }
        return s.s;
    }
    /* (a translation's trailing zeros go unsaid) */
    if (!strcmp(n, "translate") && strcmp(v, "none")) {
        char w[3][160];
        int nw = canon_words(v, w, 3);
        if (nw < 2) return NULL;
        bool zero[3];
        for (int i = 0; i < nw; i++) {
            char *e = NULL;
            double x = strtod(w[i], &e);
            zero[i] = e != w[i] && x == 0.0 && (!*e || !strcasecmp(e, "px"));
        }
        int keep = nw;
        if (keep == 3 && zero[2]) keep = 2;
        if (keep == 2 && zero[1]) keep = 1;
        if (keep == nw) return NULL;
        Buf s = { 0 };
        for (int i = 0; i < keep; i++) { if (i) buf_ch(&s, ' '); buf_str(&s, w[i]); }
        return buf_take(&s);
    }
    /* (an axis that's an axis, by its name; a negative one is the angle turned round) */
    if (!strcmp(n, "rotate") && !strstr(v, "random(")) {
        char w[4][160];
        int rw = canon_words(v, w, 4);
        if (rw == 2) {
            /* (an axis keyword: first; the z axis goes without saying) */
            int ax = !strcasecmp(w[0], "x") || !strcasecmp(w[0], "y") || !strcasecmp(w[0], "z") ? 0 : !strcasecmp(w[1], "x") || !strcasecmp(w[1], "y") || !strcasecmp(w[1], "z") ? 1 : -1;
            if (ax >= 0) {
                char o[330];
                if (!strcasecmp(w[ax], "z")) snprintf(o, sizeof o, "%s", w[!ax]);
                else snprintf(o, sizeof o, "%c %s", tolower((unsigned char)w[ax][0]), w[!ax]);
                return strcmp(o, v) ? strdup(o) : NULL;
            }
        }
        if (rw == 4) {
            char *e0, *e1, *e2;
            double x = strtod(w[0], &e0), y = strtod(w[1], &e1), z = strtod(w[2], &e2);
            if (!*e0 && !*e1 && !*e2 && ((x != 0) + (y != 0) + (z != 0)) == 1) {
                double m = x + y + z;
                char *ae = NULL;
                double ang = strtod(w[3], &ae);
                if (ae != w[3] && !strchr(w[3], '(')) {
                    Buf s = { 0 };
                    if (x != 0) buf_str(&s, "x ");
                    else if (y != 0) buf_str(&s, "y ");
                    fmt_number(&s, m < 0 ? -ang : ang);
                    buf_str(&s, ae);
                    return buf_take(&s);
                }
            }
        }
        return NULL;
    }
    if (!strcmp(n, "margin-trim")) {
        char w[4][160];
        int nw = canon_words(v, w, 4), bits = 0;
        if (nw < 2) return NULL;
        static const char *const side[4] = { "block-start", "inline-start", "block-end", "inline-end" };
        for (int i = 0; i < nw; i++) {
            if (!strcasecmp(w[i], "block")) bits |= 5;
            else if (!strcasecmp(w[i], "inline")) bits |= 10;
            for (int k = 0; k < 4; k++) if (!strcasecmp(w[i], side[k])) bits |= 1 << k;
        }
        if (bits == 15) return strdup("block inline");
        if (bits == 5) return strdup("block");
        if (bits == 10) return strdup("inline");
        char o[64];
        o[0] = 0;
        for (int k = 0; k < 4; k++) if (bits & (1 << k)) { if (o[0]) strcat(o, " "); strcat(o, side[k]); }
        return o[0] ? strdup(o) : NULL;
    }
    if (!strcmp(n, "max-lines")) {
        char w[2][160];
        if (canon_words(v, w, 2) == 2 && !strcasecmp(w[0], "auto")) { char o[330]; snprintf(o, sizeof o, "%s auto", w[1]); return strdup(o); }
        return NULL;
    }
    if (!strcmp(n, "line-clamp")) {
        /* the count, auto where it isn't, the end; ellipsis is what it does anyway */
        char w[5][160], *num = NULL, *end = NULL;
        bool has_auto = false, legacy = false;
        int nw = canon_words(v, w, 5);
        if (nw == 1 && !strcasecmp(w[0], "none")) return NULL;
        for (int i = 0; i < nw; i++) {
            if (!strcasecmp(w[i], "auto")) has_auto = true;
            else if (!strcasecmp(w[i], "-webkit-legacy")) legacy = true;
            else if (!strcasecmp(w[i], "ellipsis")) continue;
            else if (isdigit((unsigned char)w[i][0]) || !strncasecmp(w[i], "calc", 4)) num = w[i];
            else end = w[i];
        }
        char o[600];
        snprintf(o, sizeof o, "%s%s%s%s%s%s", num ? num : "", num && (has_auto || !num) ? " " : "", has_auto || !num ? "auto" : "",
                 end ? " " : "", end ? end : "", legacy ? " -webkit-legacy" : "");
        return strdup(o[0] == ' ' ? o + 1 : o);
    }
    if (!strcmp(n, "color-scheme")) {
        char w[8][160];
        int nw = canon_words(v, w, 8);
        for (int i = 0; nw > 1 && i < nw; i++) if (!strcasecmp(w[i], "normal")) return strdup("");
        /* (only at either end) */
        for (int i = 1; i + 1 < nw; i++) if (!strcasecmp(w[i], "only")) return strdup("");
        if (nw && !strcasecmp(w[0], "only") && nw == 1) return strdup("");
        /* (only first: last) */
        if (nw > 1 && !strcasecmp(w[0], "only")) {
            Buf s = { 0 };
            for (int i = 1; i < nw; i++) { if (s.n) buf_ch(&s, ' '); buf_str(&s, w[i]); }
            buf_str(&s, " only");
            return buf_take(&s);
        }
        return NULL;
    }
    if (!strcmp(n, "font-language-override") || !strcmp(n, "font-variation-settings") || !strcmp(n, "font-feature-settings")) {
        /* (a tag: four printable characters) */
        bool lang = n[5] == 'l';
        if (strchr(v, '\\')) return strchr(v, '"') && !lang ? strdup("") : NULL;
        for (const char *q = v; *q; q++) {
            if (*q != '"' && *q != '\'') continue;
            const char *e = strchr(q + 1, *q);
            if (!e) break;
            size_t tl = (size_t)(e - q - 1);
            if (lang ? (tl < 1 || tl > 4) : tl != 4) return strdup("");
            for (const char *c = q + 1; c < e; c++) if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7e) return strdup("");
            q = e;
        }
        /* (a language's trailing spaces say nothing) */
        if (lang && v[0] == '"') {
            size_t l = strlen(v);
            if (l >= 3 && v[l - 1] == '"' && v[l - 2] == ' ') {
                char *o = strdup(v);
                size_t k = l - 1;
                while (k > 1 && o[k - 1] == ' ') k--;
                o[k] = '"';
                o[k + 1] = 0;
                return o;
            }
        }
        /* (each tag once, its last value where it first stood; 1 and on unsaid, off as 0) */
        if (n[5] == 'f' && strcmp(v, "normal")) {
            char **parts = NULL;
            int np = split_commas(v, &parts);
            if (np < 1) return NULL;
            char tag[32][16], val[32][32];
            int nt = 0;
            for (int i = 0; i < np && nt < 32; i++) {
                char w[2][160];
                int nw = canon_words(parts[i], w, 2);
                if (nw < 1) continue;
                const char *vv = nw > 1 ? w[1] : "1";
                if (!strcasecmp(vv, "on")) vv = "1";
                else if (!strcasecmp(vv, "off")) vv = "0";
                int at = -1;
                for (int j = 0; j < nt; j++) if (!strcmp(tag[j], w[0])) at = j;
                if (at < 0) { at = nt++; snprintf(tag[at], sizeof tag[at], "%s", w[0]); }
                snprintf(val[at], sizeof val[at], "%s", vv);
            }
            for (int i = 0; i < np; i++) free(parts[i]);
            free(parts);
            Buf s = { 0 };
            for (int j = 0; j < nt; j++) {
                if (j) buf_str(&s, ", ");
                buf_str(&s, tag[j]);
                if (strcmp(val[j], "1")) { buf_ch(&s, ' '); buf_str(&s, val[j]); }
            }
            buf_ch(&s, 0);
            if (!strcmp(s.s, v)) { free(s.s); return NULL; }
            return s.s;
        }
        return NULL;
    }
    if (!strcmp(n, "font-style")) {
        /* (an oblique of no angle is no oblique) */
        char w[2][160];
        if (canon_words(v, w, 2) == 2 && !strcasecmp(w[0], "oblique")) {
            char *e = NULL;
            double a = strtod(w[1], &e);
            if (e != w[1] && a == 0.0 && (!*e || !strcasecmp(e, "deg") || !strcasecmp(e, "rad") || !strcasecmp(e, "grad") || !strcasecmp(e, "turn"))) return strdup("normal");
        }
        return NULL;
    }
    if (!strcmp(n, "grid-template-areas") && *v == '"') return canon_grid_areas(v);
    /* (dense with row: row goes without saying) */
    if (!strcmp(n, "grid-auto-flow") && (!strcmp(v, "row dense") || !strcmp(v, "dense row"))) return strdup("dense");
    /* (an animation's name given as a string: the identifier it is; an empty one, none at all) */
    if (!strcmp(n, "animation-name") && strchr(v, '"')) {
        Buf o = { 0 };
        for (const char *p = v; *p; ) {
            while (*p == ' ' || *p == ',') { if (*p == ',') buf_str(&o, ", "); p++; }
            if (!*p) break;
            if (*p != '"') {
                const char *e = p;
                while (*e && *e != ',') e++;
                const char *t = e;
                while (t > p && t[-1] == ' ') t--;
                buf_add(&o, p, (size_t)(t - p));
                p = e;
                continue;
            }
            const char *s = p + 1, *e = s;
            Buf c = { 0 };
            while (*e && *e != '"') { if (*e == 92 && e[1]) e++; buf_ch(&c, *e); e++; }
            buf_ch(&c, 0);
            const char *txt = c.s ? c.s : "";
            if (!*txt) { free(c.s); free(o.s); return strdup(""); }
            bool kw = !strcasecmp(txt, "none") || !strcasecmp(txt, "initial") || !strcasecmp(txt, "inherit") || !strcasecmp(txt, "unset") ||
                      !strcasecmp(txt, "default") || !strcasecmp(txt, "revert") || !strcasecmp(txt, "revert-layer");
            if (kw) buf_add(&o, p, (size_t)(e + 1 - p));
            else {
                for (const char *q = txt; *q; q++) {
                    unsigned char ch = (unsigned char)*q;
                    bool first = q == txt, second_after_dash = q == txt + 1 && txt[0] == '-';
                    if (isdigit(ch) && (first || second_after_dash)) { char hx[8]; snprintf(hx, sizeof hx, "\\%x ", ch); buf_str(&o, hx); }
                    else if (isalnum(ch) || ch == '-' || ch == '_' || ch >= 0x80) buf_ch(&o, (char)ch);
                    else { buf_ch(&o, 92); buf_ch(&o, (char)ch); }
                }
            }
            free(c.s);
            p = *e ? e + 1 : e;
        }
        buf_ch(&o, 0);
        if (strcmp(o.s, v)) return o.s;
        free(o.s);
        return NULL;
    }
    /* (both insets the same: one; right, the default side of an emphasis mark: unsaid) */
    if (!strcmp(n, "text-decoration-inset") || !strcmp(n, "text-emphasis-position")) {
        char w[3][160];
        int nw = canon_words(v, w, 3);
        for (int i = 0; i < nw; i++) {
            size_t wl = strlen(w[i]);
            /* (calc(N%) or calc(Npx): the plain value) */
            if (g_canon_computed && !strncmp(w[i], "calc(", 5) && wl > 6 && w[i][wl - 1] == ')' && !strpbrk(w[i] + 5, " +*/(")) {
                memmove(w[i], w[i] + 5, wl - 5);
                w[i][wl - 6] = 0;
            }
        }
        if (n[5] == 'd' && nw == 2 && !strcmp(w[0], w[1])) nw = 1;
        if (n[5] == 'e' && nw == 2) {
            int side = !strcasecmp(w[0], "right") || !strcasecmp(w[0], "left") ? 0 : 1;
            if (!strcasecmp(w[side], "right")) { snprintf(w[0], 160, "%s", w[1 - side]); nw = 1; }
            else if (side == 0) { char t[160]; memcpy(t, w[0], 160); memcpy(w[0], w[1], 160); memcpy(w[1], t, 160); }
        }
        Buf o = { 0 };
        for (int i = 0; i < nw; i++) { if (i) buf_ch(&o, ' '); buf_str(&o, w[i]); }
        buf_ch(&o, 0);
        if (strcmp(o.s, v)) return o.s;
        free(o.s);
        return NULL;
    }
    /* (an animation range's offset that's the default for its end: unsaid; a calc() of a percentage, the percentage) */
    if (!strcmp(n, "animation-range-start") || !strcmp(n, "animation-range-end")) {
        bool start = n[16] == 's';
        Buf o = { 0 };
        bool changed = false;
        const char *p = v;
        while (*p) {
            const char *c = p;
            int d = 0;
            while (*c && !(*c == ',' && !d)) { if (*c == '(') d++; else if (*c == ')') d--; c++; }
            char *item = strndup(p, (size_t)(c - p));
            char w[3][160];
            int nw = canon_words(item, w, 3);
            free(item);
            if (o.n) buf_str(&o, ", ");
            for (int i = 0; i < nw; i++) {
                /* (calc(N%): N%) */
                size_t wl = strlen(w[i]);
                if (!strncmp(w[i], "calc(", 5) && wl > 7 && w[i][wl - 1] == ')' && w[i][wl - 2] == '%') {
                    char *e = NULL;
                    double x = strtod(w[i] + 5, &e);
                    if (e && !strcmp(e, "%)")) { snprintf(w[i], 160, "%.6g%%", x); changed = true; }
                }
            }
            if (nw == 2 && isalpha((unsigned char)w[0][0]) && !strcmp(w[1], start ? "0%" : "100%")) { nw = 1; changed = true; }
            for (int i = 0; i < nw; i++) { if (i) buf_ch(&o, ' '); buf_str(&o, w[i]); }
            p = *c ? c + 1 : c;
            while (*p == ' ') p++;
        }
        buf_ch(&o, 0);
        if (changed) return o.s;
        free(o.s);
    }
    /* (a threshold's percentage: the number it is) */
    if (!strcmp(n, "shape-image-threshold")) {
        char *e = NULL;
        double t = strtod(v, &e);
        if (e != v && !strcmp(e, "%")) { char b[48]; snprintf(b, sizeof b, "%.6g", t / 100.0); return strdup(b); }
    }
    /* (span 1 with a name: the 1 goes without saying) */
    if (!strncmp(n, "grid-", 5) && (strstr(n, "-start") || strstr(n, "-end")) && strstr(v, "span")) {
        char w[3][160];
        int nw = canon_words(v, w, 3);
        if (nw == 3) {
            int sp = -1, one = -1, nm = -1;
            for (int i = 0; i < 3; i++) {
                if (!strcasecmp(w[i], "span")) sp = i;
                else if (!strcmp(w[i], "1")) one = i;
                else nm = i;
            }
            if (sp >= 0 && one >= 0 && nm >= 0) { char b[340]; snprintf(b, sizeof b, "span %s", w[nm]); return strdup(b); }
        }
    }
    if (!strcmp(n, "container-name")) {
        char w[8][160];
        int nw = canon_words(v, w, 8);
        for (int i = 0; i < nw; i++)
            if (!strcasecmp(w[i], "and") || !strcasecmp(w[i], "or") || !strcasecmp(w[i], "not") || !strcasecmp(w[i], "default") ||
                (nw > 1 && !strcasecmp(w[i], "none"))) return strdup("");
        return NULL;
    }
    if (!strcmp(n, "white-space")) return canon_white_space(v);
    if (!strcmp(n, "will-change")) {
        /* (a CSS-wide keyword is no feature to watch) */
        char w[16][160];
        int nw = canon_words(v, w, 16);
        for (int i = 0; i < nw; i++) {
            char *c = w[i];
            size_t l = strlen(c);
            if (l && c[l - 1] == ',') c[l - 1] = 0;
            if (!strcasecmp(c, "revert-rule") || !strcasecmp(c, "revert-layer") || !strcasecmp(c, "revert") ||
                !strcasecmp(c, "initial") || !strcasecmp(c, "inherit") || !strcasecmp(c, "unset")) { if (nw > 1) return strdup(""); }
        }
        return NULL;
    }
    if (!strcmp(n, "contain") && g_canon_computed) {
        /* (computed: the four that make content, and size with them, strict) */
        char w[6][160];
        int nw = canon_words(v, w, 6);
        bool size = false, layout = false, style = false, paint = false, other = false;
        for (int i = 0; i < nw; i++) {
            if (!strcasecmp(w[i], "size")) size = true;
            else if (!strcasecmp(w[i], "layout")) layout = true;
            else if (!strcasecmp(w[i], "style")) style = true;
            else if (!strcasecmp(w[i], "paint")) paint = true;
            else other = true;
        }
        if (!other && layout && style && paint) return strdup(size ? "strict" : "content");
        return NULL;
    }
    if (!strcmp(n, "hyphenate-limit-chars") && false) {
        /* (trailing autos go without saying) */
        char w[3][160];
        int nw = canon_words(v, w, 3), keep = nw;
        while (keep > 1 && !strcasecmp(w[keep - 1], "auto")) keep--;
        if (keep == nw) return NULL;
        Buf s = { 0 };
        for (int i = 0; i < keep; i++) { if (i) buf_ch(&s, ' '); buf_str(&s, w[i]); }
        return buf_take(&s);
    }
    if (!strcmp(n, "text-fit") && g_canon_computed) {
        char w[3][160];
        int nw = canon_words(v, w, 3);
        if (nw == 2 && !strcasecmp(w[1], "consistent")) return strdup(w[0]);
        return NULL;
    }
    if (!strcmp(n, "content") && (strstr(v, "attr()") || strstr(v, "attr( )"))) return strdup("");
    if ((!strcmp(n, "content") || !strcmp(n, "list-style-type") || !strcmp(n, "list-style")) && strcasestr_simple(v, "symbols(")) {
        /* (symbols( [cyclic | numeric | alphabetic | symbolic | fixed]? <string>+ ): numeric and alphabetic
         * two at least; no other system, no identifiers for symbols) */
        for (const char *p = v; (p = strcasestr_simple(p, "symbols(")); p += 8) {
            const char *a = p + 8, *e = strchr(a, ')');
            if (!e) break;
            while (a < e && isspace((unsigned char)*a)) a++;
            static const char *const sys[] = { "cyclic", "numeric", "alphabetic", "symbolic", "fixed" };
            int si = -1;
            if (a < e && isalpha((unsigned char)*a)) {
                const char *w = a;
                while (a < e && (isalnum((unsigned char)*a) || *a == '-')) a++;
                for (int k = 0; k < 5; k++)
                    if ((size_t)(a - w) == strlen(sys[k]) && !strncasecmp(w, sys[k], (size_t)(a - w))) si = k;
                if (si < 0) return strdup("");
            }
            int strings = 0;
            for (const char *k = a; k < e; k++) {
                if (isspace((unsigned char)*k)) continue;
                if (*k == '"' || *k == '\'') { char q = *k; strings++; for (k++; k < e && *k != q; k++) if (*k == '\\' && k + 1 < e) k++; continue; }
                return strdup("");
            }
            if (!strings || ((si == 1 || si == 2) && strings < 2)) return strdup("");
        }
    }
    if (!strcmp(n, "content") && strcasestr_simple(v, "counter")) {
        /* (counter() and counters(): a predefined style's name lower-cased, decimal unsaid) */
        static const char *pre[] = { "decimal", "disc", "circle", "square", "none", "lower-roman", "upper-roman", "lower-alpha",
                                      "upper-alpha", "lower-latin", "upper-latin", "lower-greek", "decimal-leading-zero",
                                      "disclosure-open", "disclosure-closed", "armenian", "georgian", NULL };
        Buf s = { 0 };
        bool changed = false;
        for (const char *p = v; *p; ) {
            bool two = !strncasecmp(p, "counters(", 9), one = !two && !strncasecmp(p, "counter(", 8);
            if (!(one || two) || (p > v && (isalnum((unsigned char)p[-1]) || p[-1] == '-'))) { buf_ch(&s, *p++); continue; }
            const char *a = strchr(p, '('), *e = a + 1;
            int d = 1;
            bool q = false;
            char qc = 0;
            for (; *e; e++) {
                if (q) { if (*e == '\\' && e[1]) e++; else if (*e == qc) q = false; continue; }
                if (*e == '"' || *e == '\'') { q = true; qc = *e; }
                else if (*e == '(') d++;
                else if (*e == ')' && --d == 0) break;
            }
            if (!*e) { buf_ch(&s, *p++); continue; }
            {   /* (its name no keyword of every property, nor none) */
                static const char *const bad[] = { "inherit", "initial", "unset", "revert", "revert-layer", "default", "none", NULL };
                const char *n0 = a + 1, *n1;
                while (n0 < e && isspace((unsigned char)*n0)) n0++;
                n1 = n0;
                while (n1 < e && *n1 != ',' && !isspace((unsigned char)*n1)) n1++;
                for (int k = 0; bad[k]; k++)
                    if ((size_t)(n1 - n0) == strlen(bad[k]) && !strncasecmp(n0, bad[k], strlen(bad[k]))) { free(s.s); return strdup(""); }
            }
            /* (its last argument: after the last top-level comma) */
            const char *lc = NULL;
            d = 0; q = false;
            for (const char *k = a + 1; k < e; k++) {
                if (q) { if (*k == '\\' && k[1]) k++; else if (*k == qc) q = false; continue; }
                if (*k == '"' || *k == '\'') { q = true; qc = *k; }
                else if (*k == '(') d++;
                else if (*k == ')') d--;
                else if (*k == ',' && d == 0) lc = k;
            }
            int commas = 0;
            d = 0; q = false;
            for (const char *k = a + 1; k < e; k++) {
                if (q) { if (*k == '\\' && k[1]) k++; else if (*k == qc) q = false; continue; }
                if (*k == '"' || *k == '\'') { q = true; qc = *k; }
                else if (*k == '(') d++; else if (*k == ')') d--;
                else if (*k == ',' && d == 0) commas++;
            }
            bool styled = lc && commas == (two ? 2 : 1);
            if (!styled) { buf_add(&s, p, (size_t)(e + 1 - p)); p = e + 1; continue; }
            const char *st = lc + 1, *se = e;
            while (st < se && isspace((unsigned char)*st)) st++;
            while (se > st && isspace((unsigned char)se[-1])) se--;
            /* (none is no style a counter can be in) */
            if (se - st == 4 && !strncasecmp(st, "none", 4)) { free(s.s); return strdup(""); }
            int k = 0;
            for (; pre[k]; k++) if ((size_t)(se - st) == strlen(pre[k]) && !strncasecmp(st, pre[k], strlen(pre[k]))) break;
            if (!pre[k]) { buf_add(&s, p, (size_t)(e + 1 - p)); p = e + 1; continue; }
            const char *ae = lc;
            while (ae > a + 1 && isspace((unsigned char)ae[-1])) ae--;
            buf_add(&s, p, (size_t)(ae - p));
            if (strcmp(pre[k], "decimal")) { buf_str(&s, ", "); buf_str(&s, pre[k]); }
            buf_ch(&s, ')');
            changed = changed || strcmp(pre[k], "decimal") || true;
            p = e + 1;
        }
        buf_ch(&s, 0);
        if (!changed || !strcmp(s.s, v)) { free(s.s); return NULL; }
        return s.s;
    }
    if (!strcmp(n, "text-combine-upright") && strcasestr_simple(v, "calc(")) {
        /* (digits calc(...): an integer's math, coming to 2, 3 or 4) */
        const char *c = strcasestr_simple(v, "calc(");
        for (const char *k = c + 5; *k; k++) if (*k == '.' || *k == 'e' || *k == 'E' || *k == '/') return strdup("");
        CalcSum s;
        double d;
        const char *u;
        Toks ts = { 0 };
        if (!tokenize(c, strlen(c), &ts) || ts.n < 1) { toks_free(&ts); return strdup(""); }
        bool ok = calc_simplify_value(&ts, 0, &s) && calc_one(&s, &d, &u) && !*u && d >= 2 && d <= 4 && d == floor(d);
        toks_free(&ts);
        if (!ok) return strdup("");
        char b[32];
        snprintf(b, sizeof b, "digits %d", (int)d);
        return strdup(b);
    }
    if ((!strcmp(n, "counter-increment") || !strcmp(n, "counter-set")) && g_canon_computed) {
        /* (computed: each name with its number, 1 or 0 when unsaid) */
        char w[32][160];
        int nw = canon_words(v, w, 32);
        if (nw < 1 || (nw == 1 && !strcasecmp(w[0], "none"))) return NULL;
        const char *dflt = n[8] == 'i' ? " 1" : " 0";
        Buf s = { 0 };
        bool changed = false;
        for (int i = 0; i < nw; i++) {
            if (i) buf_ch(&s, ' ');
            buf_str(&s, w[i]);
            bool name = !isdigit((unsigned char)w[i][0]) && w[i][0] != '-' && w[i][0] != '+' && strncasecmp(w[i], "calc(", 5);
            bool next_num = i + 1 < nw && (isdigit((unsigned char)w[i + 1][0]) || w[i + 1][0] == '-' || w[i + 1][0] == '+' || !strncasecmp(w[i + 1], "calc(", 5));
            if (name && !next_num) { buf_str(&s, dflt); changed = true; }
        }
        if (changed) return buf_take(&s);
        free(s.s);
        return NULL;
    }
    if (g_canon_computed && (!strcmp(n, "font-weight") || !strcmp(n, "font-width") || !strcmp(n, "font-stretch") ||
                             !strncmp(n, "scroll-padding", 14))) {
        /* (computed: within what each can be) */
        char *e = NULL;
        double d = strtod(v, &e);
        if (e == v) return NULL;
        char b[64];
        if (!strcmp(n, "font-weight") && !*e && (d < 1 || d > 1000)) { snprintf(b, sizeof b, "%g", d < 1 ? 1.0 : 1000.0); return strdup(b); }
        if ((n[5] == 'w' || n[5] == 's') && n[0] == 'f' && !strcmp(e, "%") && d < 0) return strdup("0%");
        if (n[0] == 's' && d < 0 && !strchr(e, ' ')) return strdup("0px");
        return NULL;
    }
    if (!strcmp(n, "font-style") && g_canon_computed && !strncasecmp(v, "oblique ", 8)) {
        /* (computed: the angle within -90deg to 90deg, none of it normal) */
        double d = strtod(v + 8, NULL);
        if (d == 0) return strdup("normal");
        if (d > 90 || d < -90) return strdup(d > 0 ? "oblique 90deg" : "oblique -90deg");
        return NULL;
    }
    if (!strcmp(n, "color-scheme")) {
        /* (only: last) */
        char w[16][160];
        int nw = canon_words(v, w, 16);
        if (nw < 2 || strcasecmp(w[0], "only")) return NULL;
        Buf s = { 0 };
        for (int i = 1; i < nw; i++) { if (s.n) buf_ch(&s, ' '); buf_str(&s, w[i]); }
        buf_str(&s, " only");
        return buf_take(&s);
    }
    if ((!strcmp(n, "filter") || !strcmp(n, "backdrop-filter")) && strchr(v, '(')) {
        /* (amounts past the whole of what they do: the whole; a turn of nothing in degrees) */
        Buf s = { 0 };
        bool changed = false;
        for (const char *p = v; *p; ) {
            static const char *const capped[] = { "grayscale(", "invert(", "opacity(", "sepia(" };
            int which = -1;
            for (int k = 0; k < 4; k++) if (!strncmp(p, capped[k], strlen(capped[k])) && (p == v || p[-1] == ' ')) which = k;
            if (which >= 0) {
                const char *a = p + strlen(capped[which]);
                char *e = NULL;
                double d = strtod(a, &e);
                if (e != a && (*e == '%' ? e[1] == ')' && d > 100 : *e == ')' && d > 1)) {
                    buf_add(&s, p, (size_t)(a - p));
                    buf_str(&s, *e == '%' ? "100%)" : "1)");
                    p = (*e == '%' ? e + 2 : e + 1);
                    changed = true;
                    continue;
                }
            }
            if (!strncmp(p, "hue-rotate(0)", 13) && (p == v || p[-1] == ' ')) { buf_str(&s, "hue-rotate(0deg)"); p += 13; changed = true; continue; }
            buf_ch(&s, *p++);
        }
        buf_ch(&s, 0);
        if (!changed) { free(s.s); return NULL; }
        return s.s;
    }
    if (!strcmp(n, "mask-position") && !g_canon_computed) {
        /* (each one value: the other axis's center said too) */
        char **parts = NULL;
        int np = split_commas(v, &parts);
        Buf s = { 0 };
        bool changed = false;
        for (int k = 0; k < np; k++) {
            char w[2][160];
            if (k) buf_str(&s, ", ");
            if (canon_words(parts[k], w, 2) == 1) {
                if (!strcasecmp(w[0], "top") || !strcasecmp(w[0], "bottom")) { buf_str(&s, "center "); buf_str(&s, w[0]); }
                else { buf_str(&s, w[0]); buf_str(&s, " center"); }
                changed = true;
            } else {
                char *t = parts[k];
                while (isspace((unsigned char)*t)) t++;
                buf_str(&s, t);
            }
            free(parts[k]);
        }
        free(parts);
        buf_ch(&s, 0);
        if (!changed) { free(s.s); return NULL; }
        return s.s;
    }
    if ((!strcmp(n, "animation-range-start") || !strcmp(n, "animation-range-end")) && g_canon_computed) {
        /* (a range name at its own start or end: the name alone) */
        const char *dflt = !strcmp(n, "animation-range-start") ? " 0%" : " 100%";
        char **parts = NULL;
        int np = split_commas(v, &parts);
        Buf s = { 0 };
        bool changed = false;
        for (int k = 0; k < np; k++) {
            char *t = parts[k];
            while (isspace((unsigned char)*t)) t++;
            size_t tl = strlen(t), dl = strlen(dflt);
            if (k) buf_str(&s, ", ");
            if (tl > dl && !strcmp(t + tl - dl, dflt) && isalpha((unsigned char)t[0])) { buf_add(&s, t, tl - dl); changed = true; }
            else buf_str(&s, t);
            free(parts[k]);
        }
        free(parts);
        buf_ch(&s, 0);
        if (!changed) { free(s.s); return NULL; }
        return s.s;
    }
    if (!strcmp(n, "transform-origin") && !g_canon_computed) {
        /* (one value: the other axis's center said too) */
        char w[2][160];
        if (canon_words(v, w, 2) == 1) {
            Buf s = { 0 };
            if (!strcasecmp(w[0], "top") || !strcasecmp(w[0], "bottom")) { buf_str(&s, "center "); buf_str(&s, w[0]); }
            else { buf_str(&s, w[0]); buf_str(&s, " center"); }
            return buf_take(&s);
        }
        return NULL;
    }
    if (!strcmp(n, "text-spacing")) {
        /* (its trim, then its autospace, each unsaid when normal; space-all and no-autospace: none) */
        char w[8][160];
        int nw = canon_words(v, w, 8);
        if (nw < 1 || (nw == 1 && (!strcasecmp(w[0], "none") || !strcasecmp(w[0], "auto")))) return NULL;
        static const char *const trims[] = { "space-all", "space-first", "trim-start", "trim-both", "trim-all" };
        const char *trim = NULL;
        Buf as = { 0 };
        int normals = 0;
        for (int i = 0; i < nw; i++) {
            bool t = false;
            for (int k = 0; k < 5 && !t; k++) t = !strcasecmp(w[i], trims[k]);
            if (t) trim = w[i];
            else if (!strcasecmp(w[i], "normal")) normals++;
            else { if (as.n) buf_ch(&as, ' '); buf_str(&as, w[i]); }
        }
        Buf s = { 0 };
        bool no_auto = as.n && !strcmp(as.s, "no-autospace");
        if (trim && !strcasecmp(trim, "space-all") && no_auto) buf_str(&s, "none");
        else {
            if (trim) buf_str(&s, trim);
            if (as.n) { if (s.n) buf_ch(&s, ' '); buf_add(&s, as.s, as.n); }
            if (!s.n) buf_str(&s, "normal");
        }
        (void)normals;
        free(as.s);
        char *r = buf_take(&s);
        if (!strcmp(r, v)) { free(r); return NULL; }
        return r;
    }
    if (!strcmp(n, "size")) {
        /* (a square once, a portrait page size by its name, names in lower case) */
        char w[2][160];
        int nw = canon_words(v, w, 2);
        for (int i = 0; i < nw; i++) for (char *c = w[i]; *c; c++) *c = (char)tolower((unsigned char)*c);
        Buf s = { 0 };
        if (nw == 2 && !strcmp(w[0], w[1])) buf_str(&s, w[0]);
        else if (nw == 2 && !strcmp(w[1], "portrait")) buf_str(&s, w[0]);
        else if (nw == 2 && !strcmp(w[0], "portrait")) buf_str(&s, w[1]);
        else if (nw == 2 && (!strcmp(w[0], "landscape"))) { buf_str(&s, w[1]); buf_str(&s, " landscape"); }
        else { for (int i = 0; i < nw; i++) { if (i) buf_ch(&s, ' '); buf_str(&s, w[i]); } }
        char *r = buf_take(&s);
        if (!strcmp(r, v)) { free(r); return NULL; }
        return r;
    }
    if (!strcmp(n, "flex-wrap")) {
        /* (balance wraps: wrap goes without saying, wrap-reverse first) */
        char w[2][160];
        int nw = canon_words(v, w, 2);
        if (nw != 2) return NULL;
        int bi = !strcasecmp(w[0], "balance") ? 0 : 1, oi = 1 - bi;
        if (!strcasecmp(w[oi], "wrap")) return strdup("balance");
        if (bi == 0) { Buf s = { 0 }; buf_str(&s, w[oi]); buf_str(&s, " balance"); return buf_take(&s); }
        return NULL;
    }
    /* (proximity goes without saying) */
    if (!strcmp(n, "scroll-snap-type")) {
        size_t vl = strlen(v);
        if (vl > 10 && !strcasecmp(v + vl - 10, " proximity")) return strndup(v, vl - 10);
        return NULL;
    }
    if (!strcmp(n, "overflow-clip-margin")) {
        char w[2][160];
        int nw = canon_words(v, w, 2);
        /* (padding-box goes without saying; with nothing else it's 0px) */
        for (int i = 0; i < nw; i++)
            if (!strcasecmp(w[i], "padding-box")) return strdup(nw == 2 ? w[1 - i] : "0px");
        if (nw == 2 && strtod(w[1], NULL) == 0.0 && isdigit((unsigned char)w[1][0])) return strdup(w[0]);
        return NULL;
    }
    if (!strcmp(n, "counter-increment")) {
        /* (a name without a number: 1) */
        char w[16][160];
        int nw = canon_words(v, w, 16);
        if (nw < 1 || (nw == 1 && !strcasecmp(w[0], "none"))) return NULL;
        Buf s = { 0 };
        bool changed = false;
        for (int i = 0; i < nw; i++) {
            if (i) buf_ch(&s, ' ');
            buf_str(&s, w[i]);
            bool name = !isdigit((unsigned char)w[i][0]) && w[i][0] != '-' && w[i][0] != '+' && strncasecmp(w[i], "calc(", 5);
            bool next_num = i + 1 < nw && (isdigit((unsigned char)w[i + 1][0]) || w[i + 1][0] == '-' || w[i + 1][0] == '+' || !strncasecmp(w[i + 1], "calc(", 5));
            if (name && !next_num) { buf_str(&s, " 1"); changed = true; }
        }
        if (changed) return buf_take(&s);
        free(s.s);
        return NULL;
    }
    /* (the third as the second, the second auto: each unsaid) */
    if (!strcmp(n, "hyphenate-limit-chars")) {
        char w[3][160];
        int nw = canon_words(v, w, 3);
        /* (only auto and whole numbers from 1: anything else isn't this property's to shorten) */
        for (int i = 0; i < nw; i++) {
            if (!strcasecmp(w[i], "auto")) continue;
            for (const char *c = w[i]; *c; c++) if (!isdigit((unsigned char)*c)) return NULL;
            if (atoi(w[i]) < 1) return NULL;
        }
        if (nw == 3 && !strcasecmp(w[2], w[1])) nw = 2;
        if (nw == 2 && !strcasecmp(w[1], "auto")) nw = 1;
        if (nw == 3) return NULL;
        char b[400];
        if (nw == 2) snprintf(b, sizeof b, "%s %s", w[0], w[1]); else snprintf(b, sizeof b, "%s", w[0]);
        return strcmp(b, v) ? strdup(b) : NULL;
    }
    /* (padding-box goes without saying; with nothing else it's 0px) */
    if (!strcmp(n, "overflow-clip-margin") && strstr(v, "padding-box")) {
        char w[2][160];
        int nw = canon_words(v, w, 2);
        for (int i = 0; i < nw; i++)
            if (!strcasecmp(w[i], "padding-box")) {
                const char *o = nw == 2 ? w[1 - i] : "0px";
                return strdup(o);
            }
        return NULL;
    }
    if (!strcmp(n, "counter-reset") || !strcmp(n, "counter-set")) {
        /* (a name without a number: 0) */
        char w[16][160];
        int nw = canon_words(v, w, 16);
        if (nw < 1 || (nw == 1 && !strcasecmp(w[0], "none"))) return NULL;
        Buf s = { 0 };
        bool changed = false;
        for (int i = 0; i < nw; i++) {
            if (i) buf_ch(&s, ' ');
            buf_str(&s, w[i]);
            bool name = !isdigit((unsigned char)w[i][0]) && w[i][0] != '-' && w[i][0] != '+' && strncasecmp(w[i], "calc(", 5) && strncasecmp(w[i], "reversed(", 9);
            bool next_num = i + 1 < nw && (isdigit((unsigned char)w[i + 1][0]) || w[i + 1][0] == '-' || w[i + 1][0] == '+' || !strncasecmp(w[i + 1], "calc(", 5));
            if (name && !next_num) { buf_str(&s, " 0"); changed = true; }
        }
        if (changed) return buf_take(&s);
        free(s.s);
        return NULL;
    }
    if (!strcmp(n, "text-box-edge")) return canon_box_edge(v);
    if (!strcmp(n, "offset-rotate")) return canon_offset_rotate(v);
    if (!strcmp(n, "offset-path") && !strncmp(v, "ray(", 4)) return canon_ray(v);
    if (!strcmp(n, "font-palette") && !strncasecmp(v, "palette-mix(", 12)) return canon_palette_mix(v);
    if ((!strcmp(n, "animation-timing-function") || !strcmp(n, "transition-timing-function")) && strstr(v, "linear("))
        return canon_linear(v, g_canon_computed);
    if ((!strcmp(n, "animation-timing-function") || !strcmp(n, "transition-timing-function")) &&
        (strstr(v, "step") || (g_canon_computed && strstr(v, "cubic-bezier(")))) {
        /* (computed: the step keywords as the steps they are, end unsaid, a curve's x within 0 to 1) */
        Buf b = { 0 };
        bool changed = false;
        for (const char *p = v; *p; ) {
            bool at_word = p == v || !(isalnum((unsigned char)p[-1]) || p[-1] == '-');
            if (at_word && !strncmp(p, "step-start", 10) && !(isalnum((unsigned char)p[10]) || p[10] == '-')) { buf_str(&b, "steps(1, start)"); p += 10; changed = true; continue; }
            if (at_word && !strncmp(p, "step-end", 8) && !(isalnum((unsigned char)p[8]) || p[8] == '-')) { buf_str(&b, "steps(1)"); p += 8; changed = true; continue; }
            if (at_word && !strncmp(p, "steps(", 6)) {
                const char *e = strchr(p, ')');
                if (e) {
                    char in[64];
                    size_t l = (size_t)(e - p - 6) < sizeof in - 1 ? (size_t)(e - p - 6) : sizeof in - 1;
                    memcpy(in, p + 6, l); in[l] = 0;
                    char *c = strchr(in, ',');
                    const char *pos = "end";
                    if (c) { *c = 0; pos = c + 1; while (isspace((unsigned char)*pos)) pos++; }
                    char *t = in; while (isspace((unsigned char)*t)) t++;
                    char *ne = NULL;
                    double cnt = strtod(t, &ne);
                    if (ne != t) {
                        /* (a whole number of steps, at least one; two with no jump) */
                        long nst = (long)floor(cnt + 0.5);
                        long lo = !strcmp(pos, "jump-none") ? 2 : 1;
                        if (nst < lo) nst = lo;
                        char nb[96];
                        if (!strcmp(pos, "end") || !strcmp(pos, "jump-end")) snprintf(nb, sizeof nb, "steps(%ld)", nst);
                        else snprintf(nb, sizeof nb, "steps(%ld, %s)", nst, pos);
                        buf_str(&b, nb);
                        p = e + 1; changed = true; continue;
                    }
                }
            }
            if (at_word && g_canon_computed && !strncmp(p, "cubic-bezier(", 13)) {
                const char *e = strchr(p, ')');
                double x[4];
                if (e && sscanf(p + 13, "%lf , %lf , %lf , %lf", &x[0], &x[1], &x[2], &x[3]) == 4) {
                    bool clamp = x[0] < 0 || x[0] > 1 || x[2] < 0 || x[2] > 1;
                    if (clamp) {
                        x[0] = x[0] < 0 ? 0 : x[0] > 1 ? 1 : x[0];
                        x[2] = x[2] < 0 ? 0 : x[2] > 1 ? 1 : x[2];
                        buf_str(&b, "cubic-bezier(");
                        for (int k = 0; k < 4; k++) { if (k) buf_str(&b, ", "); fmt_number(&b, x[k]); }
                        buf_ch(&b, ')');
                        p = e + 1; changed = true; continue;
                    }
                }
            }
            buf_ch(&b, *p++);
        }
        buf_ch(&b, 0);
        if (!changed) { free(b.s); return NULL; }
        return b.s;
    }
    if (strstr(v, "image-set(")) return canon_image_set(v);
    if (!strcmp(n, "position-try-fallbacks")) return canon_try_fallbacks(v);
    if ((!strcmp(n, "grid-template-columns") || !strcmp(n, "grid-template-rows")) && strstr(v, "[") && strncasecmp(v, "subgrid", 7)) {
        /* (a set of no line names says nothing) */
        Buf nb = { 0 };
        bool gone = false;
        for (const char *q = v; *q; ) {
            if (*q == '[') {
                const char *c = q + 1;
                while (isspace((unsigned char)*c)) c++;
                if (*c == ']') {
                    q = c + 1;
                    while (isspace((unsigned char)*q)) q++;
                    gone = true;
                    continue;
                }
            }
            buf_ch(&nb, *q++);
        }
        buf_ch(&nb, 0);
        if (gone) {
            /* (no space left before a closing parenthesis) */
            for (char *sp; (sp = strstr(nb.s, " )")) != NULL; ) memmove(sp, sp + 1, strlen(sp + 1) + 1);
            size_t nl = strlen(nb.s);
            while (nl && isspace((unsigned char)nb.s[nl - 1])) nb.s[--nl] = 0;
            if (!grid_tracks_bad(nb.s) && nl) return nb.s;
            if (nl && grid_tracks_bad(nb.s)) { free(nb.s); return strdup(""); }
        }
        free(nb.s);
    }
    if ((!strcmp(n, "grid-template-columns") || !strcmp(n, "grid-template-rows")) && grid_tracks_bad(v)) return strdup("");
    if (strstr(v, "random(")) return canon_random(n, v);
    if (!strcmp(n, "clip-path") || !strcmp(n, "shape-outside") || (!strcmp(n, "offset-path") && !strstr(v, "ray("))) {
        char *bs = canon_basic_shape(n, v);
        if (bs) {
            char *rb = strstr(bs, " round ") ? canon_box_round(bs) : NULL;
            if (rb) { free(bs); return rb; }
            return bs;
        }
    }
    if ((!strcmp(n, "clip-path") || !strcmp(n, "shape-outside") || !strcmp(n, "offset-path")) && strstr(v, " round ")) return canon_box_round(v);
    return NULL;
}

/* will-change: what can't be named in its list. */
static bool will_change_bad(const char *v)
{
    char **parts = NULL;
    int np = split_commas(v, &parts);
    bool bad = false;
    for (int k = 0; k < np; k++) {
        char *t = parts[k];
        while (isspace((unsigned char)*t)) t++;
        size_t l = strlen(t);
        while (l && isspace((unsigned char)t[l - 1])) t[--l] = 0;
        if (!strcasecmp(t, "will-change") || !strcasecmp(t, "none") || !strcasecmp(t, "all") ||
            (np > 1 && (!strcasecmp(t, "auto") || !strcasecmp(t, "initial") || !strcasecmp(t, "inherit") || !strcasecmp(t, "unset") ||
                        !strcasecmp(t, "revert") || !strcasecmp(t, "revert-layer") || !strcasecmp(t, "default")))) bad = true;
    }
    for (int k = 0; k < np; k++) free(parts[k]);
    free(parts);
    return bad;
}

/* ---- calc-mix() ---- */
#define MIX_MAX 32
typedef struct { char *item; char *wtext; double w; bool known, has; } MixArg;

static const char *mix_close(const char *q)
{
    for (int d = 1; *q; q++) {
        if (*q == '(') d++;
        else if (*q == ')' && !--d) return q;
    }
    return NULL;
}

static char *mix_trim(const char *a, const char *b)
{
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    return strndup(a, (size_t)(b - a));
}

/* The arguments between a and b; -1 if they can't be. */
static int mix_args(const char *a, const char *b, MixArg *out)
{
    int n = 0;
    for (const char *s = a; ; ) {
        const char *e = s;
        for (int d = 0; e < b && (d || *e != ','); e++) {
            if (*e == '(') d++;
            else if (*e == ')') d--;
        }
        char *t = mix_trim(s, e);
        if (!t[0] || n == MIX_MAX) { free(t); return -1; }
        MixArg *m = &out[n++];
        memset(m, 0, sizeof *m);
        /* (the last word, if there's more than one, is the weight) */
        size_t l = strlen(t);
        const char *w = NULL;
        int d = 0;
        for (size_t i = l; i > 0; i--) {
            char c = t[i - 1];
            if (c == ')') d++;
            else if (c == '(') d--;
            else if (!d && isspace((unsigned char)c)) { w = t + i; break; }
        }
        if (w && (t[l - 1] == '%' || (t[l - 1] == ')' && strchr(w, '%')))) {
            m->has = true;
            m->item = mix_trim(t, w);
            if (t[l - 1] == '%') {
                char *q = NULL;
                m->w = strtod(w, &q);
                if (q == w || *q != '%' || q[1] || m->w < 0.0 || m->w > 100.0) { free(t); return -(n + 1); }
                m->known = true;
            } else m->wtext = strdup(w);
        } else m->item = strdup(t);
        free(t);
        if (e >= b) break;
        s = e + 1;
    }
    return n;
}

static void mix_free(MixArg *m, int n)
{
    for (int i = 0; i < n; i++) { free(m[i].item); free(m[i].wtext); }
}

/* A calc-mix() as it's specified: weights worked out, down to a calc() if it can be. */
static char *mix_norm(const char *a, const char *b)
{
    MixArg m[MIX_MAX];
    int n = mix_args(a, b, m);
    if (n <= 0) { mix_free(m, n < -1 ? -n - 1 : 0); return NULL; }
    bool all_known = true;
    double sum = 0.0;
    int missing = 0;
    for (int i = 0; i < n; i++) {
        if (m[i].wtext) all_known = false;
        if (!m[i].has) missing++;
        if (m[i].known) sum += m[i].w;
    }
    bool weights = true;
    for (int i = 0; i < n; i++) if (m[i].wtext) weights = false;
    if (weights) {
        if (sum > 100.0) {
            for (int i = 0; i < n; i++) m[i].w = m[i].has ? m[i].w * 100.0 / sum : 0.0;
        } else if (missing) {
            double each = (100.0 - sum) / missing;
            for (int i = 0; i < n; i++) if (!m[i].has) m[i].w = each;
        }
        for (int i = 0; i < n; i++) { m[i].known = m[i].has = true; }
    }
    Buf o = { 0 };
    char num[64];
    if (all_known) {
        /* (nothing weighs anything: nothing of the first's kind) */
        bool any = false;
        for (int i = 0; i < n; i++) any = any || m[i].w > 0.0;
        if (!any) {
            char *q = NULL;
            strtod(m[0].item, &q);
            size_t ul = q ? strcspn(q, " )*/+-") : 0;
            buf_str(&o, "calc(0");
            if (ul) buf_add(&o, q, ul);
            buf_ch(&o, ')');
            mix_free(m, n);
            return buf_take(&o);
        }
        /* (numbers of one unit: the number) */
        const char *unit = NULL;
        bool plain = true;
        double total = 0.0, wsum = 0.0;
        for (int i = 0; i < n; i++) {
            if (m[i].w <= 0.0) continue;
            char *q = NULL;
            double v = strtod(m[i].item, &q);
            if (q == m[i].item || (*q && strspn(q, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ%") != strlen(q))) { plain = false; break; }
            if (unit && strcasecmp(unit, q)) { plain = false; break; }
            unit = q;
            total += v * m[i].w / 100.0;
            wsum += m[i].w;
        }
        if (plain && unit) {
            snprintf(num, sizeof num, "%.6g", total);
            buf_str(&o, "calc(");
            buf_str(&o, num);
            buf_str(&o, unit);
            buf_ch(&o, ')');
            mix_free(m, n);
            return buf_take(&o);
        }
    }
    buf_str(&o, "calc-mix(");
    bool first = true;
    for (int i = 0; i < n; i++) {
        if (weights && m[i].w <= 0.0) continue;
        if (!first) buf_str(&o, ", ");
        first = false;
        buf_str(&o, m[i].item);
        if (m[i].wtext) { buf_ch(&o, ' '); buf_str(&o, m[i].wtext); }
        else if (m[i].has) { snprintf(num, sizeof num, " %.6g%%", m[i].w); buf_str(&o, num); }
    }
    buf_ch(&o, ')');
    mix_free(m, n);
    return buf_take(&o);
}

/* value, each calc-mix() as it's specified; NULL: one isn't valid. */
static char *calc_mix_canon(const char *value)
{
    Buf o = { 0 };
    const char *q = value;
    for (const char *g; (g = strstr(q, "calc-mix(")); ) {
        const char *c = mix_close(g + 9);
        if (!c) { free(o.s); return NULL; }
        /* (one inside another: that one first) */
        char *in = strndup(g + 9, (size_t)(c - g - 9));
        if (strstr(in, "calc-mix(")) {
            char *r = calc_mix_canon(in);
            free(in);
            if (!r) { free(o.s); return NULL; }
            in = r;
        }
        char *n = mix_norm(in, in + strlen(in));
        free(in);
        if (!n) { free(o.s); return NULL; }
        buf_add(&o, q, (size_t)(g - q));
        buf_str(&o, n);
        free(n);
        q = c + 1;
    }
    buf_str(&o, q);
    buf_ch(&o, 0);
    return o.s;
}

/* value, each calc-mix() as the sum it is. */
char *css_calc_mix_expand(const char *value)
{
    Buf o = { 0 };
    const char *q = value;
    for (const char *g; (g = strstr(q, "calc-mix(")); ) {
        const char *c = mix_close(g + 9);
        if (!c) { free(o.s); return NULL; }
        char *in = strndup(g + 9, (size_t)(c - g - 9));
        if (strstr(in, "calc-mix(")) {
            char *r = css_calc_mix_expand(in);
            free(in);
            if (!r) { free(o.s); return NULL; }
            in = r;
        }
        MixArg m[MIX_MAX];
        int n = mix_args(in, in + strlen(in), m);
        if (n <= 0) { mix_free(m, n < -1 ? -n - 1 : 0); free(in); free(o.s); return NULL; }
        buf_add(&o, q, (size_t)(g - q));
        buf_str(&o, "calc(");
        for (int i = 0; i < n; i++) {
            char num[48];
            if (i) buf_str(&o, " + ");
            buf_ch(&o, '(');
            buf_str(&o, m[i].item);
            buf_str(&o, ") * ");
            if (m[i].wtext) {
                /* (a weight's percentages, as fractions) */
                for (const char *w = m[i].wtext; *w; w++) {
                    if (*w == '%') buf_str(&o, " / 100");
                    else buf_ch(&o, *w);
                }
            } else {
                snprintf(num, sizeof num, "%.6g", m[i].has ? m[i].w / 100.0 : 1.0 / n);
                buf_str(&o, num);
            }
        }
        buf_ch(&o, ')');
        mix_free(m, n);
        free(in);
        q = c + 1;
    }
    buf_str(&o, q);
    buf_ch(&o, 0);
    return o.s;
}

/* A url()'s request modifiers in their order: cross-origin, integrity,
 * referrer-policy.  NULL when nothing moves. */
static char *url_mods_canon(const char *v)
{
    if (!strstr(v, "url(")) return NULL;
    Buf b = { 0 };
    bool moved = false;
    for (const char *p = v; *p; ) {
        if (strncasecmp(p, "url(", 4)) { buf_ch(&b, *p++); continue; }
        const char *s = p + 4;
        while (isspace((unsigned char)*s)) s++;
        if (*s != '"' && *s != '\'') { buf_ch(&b, *p++); continue; }
        char q = *s;
        const char *e = s + 1;
        while (*e && *e != q) { if (*e == '\\' && e[1]) e++; e++; }
        if (!*e) { buf_ch(&b, *p++); continue; }
        e++;
        /* (the modifiers: name( ... ) each) */
        const char *ms[3] = { 0 }, *me[3] = { 0 };
        int order[3], n = 0;
        const char *k = e;
        bool ok = true;
        while (ok) {
            while (isspace((unsigned char)*k)) k++;
            if (*k == ')' || !*k) break;
            int w = !strncasecmp(k, "cross-origin(", 13) ? 0 : !strncasecmp(k, "integrity(", 10) ? 1 : !strncasecmp(k, "referrer-policy(", 16) ? 2 : -1;
            if (w < 0 || ms[w]) { ok = false; break; }
            const char *x = strchr(k, '(') + 1;
            char qq = 0;
            for (; *x && (qq || *x != ')'); x++) { if (qq) { if (*x == qq) qq = 0; } else if (*x == '"' || *x == '\'') qq = *x; }
            if (!*x) { ok = false; break; }
            ms[w] = k; me[w] = x + 1;
            order[n++] = w;
            k = x + 1;
        }
        if (!ok || *k != ')' || n < 2) { buf_add(&b, p, (size_t)(e - p)); p = e; continue; }
        for (int i = 1; i < n; i++) if (order[i] < order[i - 1]) moved = true;
        buf_add(&b, p, (size_t)(e - p));
        for (int w = 0; w < 3; w++) if (ms[w]) { buf_ch(&b, ' '); buf_add(&b, ms[w], (size_t)(me[w] - ms[w])); }
        buf_ch(&b, ')');
        p = k + 1;
    }
    buf_ch(&b, 0);
    if (!moved) { free(b.s); return NULL; }
    return b.s;
}

char *css_prop_parse(const CssPropDef *p, const char *value)
{
    /* (worked out as far as it can be; what's left is checked as its sum, and stays) */
    if (value && strstr(value, "calc-mix(") && !has_substitution(value)) {
        char *canon = calc_mix_canon(value);
        if (!canon) return NULL;
        if (strstr(canon, "calc-mix(")) {
            char *sum = css_calc_mix_expand(canon);
            char *ok = sum ? css_prop_parse(p, sum) : NULL;
            free(sum);
            if (!ok) { free(canon); return NULL; }
            free(ok);
            return canon;
        }
        char *r = css_prop_parse(p, canon);
        free(canon);
        return r;
    }
    /* (first baseline is what baseline is) */
    char *firsted = NULL;
    if (value && p && (!strncmp(p->name, "align-", 6) || !strncmp(p->name, "justify-", 8) || !strncmp(p->name, "place-", 6))) {
        const char *fb = strstr(value, "first baseline");
        if (fb) {
            size_t pre = (size_t)(fb - value);
            firsted = (char *)malloc(strlen(value) + 1);
            if (firsted) {
                memcpy(firsted, value, pre);
                strcpy(firsted + pre, fb + 6);
                value = firsted;
            }
        }
    }
    /* (the old name of image-set()) */
    char *renamed = NULL;
    if (value && strstr(value, "-webkit-image-set(")) {
        Buf rb = { 0 };
        const char *q0 = value;
        for (const char *g; (g = strstr(q0, "-webkit-image-set(")); q0 = g + 8) buf_add(&rb, q0, (size_t)(g - q0));
        buf_str(&rb, q0);
        buf_ch(&rb, 0);
        renamed = rb.s;
        value = renamed;
    }
    char *v = prop_parse_raw(p, value);
    if (renamed) {
        /* (value isn't looked at again after this but by the notes below, which may keep it) */
        if (!v) { free(renamed); return NULL; }
    }
    if (firsted && !v) { free(firsted); return NULL; }
    if (v && strstr(v, "url(") && (strstr(v, "cross-origin(") || strstr(v, "integrity(") || strstr(v, "referrer-policy("))) {
        char *u = url_mods_canon(v);
        if (u) { free(v); v = u; }
    }
    if (v && p && !has_substitution(v) && !css_is_wide_keyword(v, strlen(v))) {
        if (!strcmp(p->name, "will-change") && will_change_bad(v)) { free(v); return NULL; }
        char *c = css_prop_canon(p, v);
        if (c) { free(v); v = c; }
        if (c && !c[0]) { free(v); return NULL; }
    }
    if (v && (strstr(value, "from ") || strstr(value, "color-mix(")) && strcmp(v, value)) color_raw_note(v, value);
    if (v && p && (!strcmp(p->name, "opacity") || strstr(p->name, "-opacity")) && strchr(v, '%') && !strchr(v, '(')) {
        char b[48];
        snprintf(b, sizeof b, "%.6g", atof(v) / 100.0);
        free(v);
        return strdup(b);
    }
    if (!v || !strstr(v, "gradient(") || has_substitution(v)) return v;
    Buf b = { 0 };
    const char *q = v;
    for (const char *g; (g = strstr(q, "gradient(")); ) {
        const char *arg = g + 9, *e = arg;
        int depth = 1;
        for (; *e; e++) {
            if (*e == '(') depth++;
            else if (*e == ')' && !--depth) break;
        }
        buf_add(&b, q, (size_t)(arg - q));
        gradient_canon_one(arg, e, &b);
        q = e;
        if (!*e) break;
    }
    buf_str(&b, q);
    buf_ch(&b, 0);
    free(v);
    return b.s;
}

/* ============================================================================
 * Registered custom properties (css-properties-values-api): the registry, flat
 * ============================================================================ */

enum { REG_MAX = 4096 };
static char *g_reg_name[REG_MAX], *g_reg_syntax[REG_MAX], *g_reg_initial[REG_MAX];
static bool g_reg_inherits[REG_MAX], g_reg_rule[REG_MAX];
static const void *g_reg_doc[REG_MAX];  /* (the document a rule's registration is of) */
static const void *g_reg_doc_cur;
void css_reg_set_doc(const void *doc) { g_reg_doc_cur = doc; }
static int g_reg_n;

int css_reg_count(void) { return g_reg_n; }

/* What the registry holds, as a number: a change means every element's custom
 * properties are to be worked out again. */
uint64_t css_reg_signature(void) {
    uint64_t h = 1469598103934665603ull;
    #define MIX(s) for (const char *q = (s); q && *q; q++) { h ^= (unsigned char)*q; h *= 1099511628211ull; }
    for (int r = 0; r < g_reg_n; r++) {
        MIX(g_reg_name[r]);
        MIX(g_reg_syntax[r]);
        MIX(g_reg_initial[r]);
        h ^= g_reg_inherits[r] ? 17 : 19;
        h *= 1099511628211ull;
    }
    #undef MIX
    return h;
}
const char *css_reg_name(int r) { return g_reg_name[r]; }
const char *css_reg_syntax(int r) { return g_reg_syntax[r]; }
const char *css_reg_initial(int r) { return g_reg_initial[r]; }
bool css_reg_inherits(int r) { return g_reg_inherits[r]; }

bool css_reg_scripted(const char *name) {
    int r = css_reg_find(name);
    return r >= 0 && !g_reg_rule[r];
}

/* A calc() of plain numbers, as its simplified serialization has it. */
bool css_calc_number(const char *v, double *out) {
    const CssPropDef *p = css_prop_find("opacity", 7);
    char *t = p ? prop_parse_raw(p, v) : NULL;
    if (!t) return false;
    const char *n = !strncasecmp(t, "calc(", 5) ? t + 5 : t;
    char *e = NULL;
    *out = strtod(n, &e);
    bool ok = e != n && (*e == 0 || *e == ')');
    free(t);
    return ok;
}

/* A time with math in it, in ms (false: not one that resolves). */
bool css_calc_time(const char *v, double *ms) {
    const CssPropDef *p = css_prop_find("animation-delay", 15);
    char *t = p ? prop_parse_raw(p, v) : NULL;
    if (!t) return false;
    const char *n = !strncasecmp(t, "calc(", 5) ? t + 5 : t;
    char *e = NULL;
    double d = strtod(n, &e);
    bool ok = e != n;
    if (ok && !strncasecmp(e, "ms", 2)) *ms = d;
    else if (ok && (*e == 's' || *e == 'S')) *ms = d * 1000.0;
    else ok = false;
    free(t);
    return ok;
}

int css_reg_find(const char *name) {
    for (int r = g_reg_n - 1; r >= 0; r--) if (!strcmp(g_reg_name[r], name)) return r;
    return -1;
}

static void reg_free(int r) {
    free(g_reg_name[r]);
    free(g_reg_syntax[r]);
    free(g_reg_initial[r]);
}

/* (rules: the last wins; a script's registration over any rule) */
bool css_reg_add(const char *name, const char *syntax, bool inherits, const char *initial, bool rule) {
    int r = css_reg_find(name);
    if (r >= 0 && !g_reg_rule[r] && rule) return false;
    if (r < 0) {
        if (g_reg_n == REG_MAX) return false;
        r = g_reg_n++;
    } else reg_free(r);
    g_reg_name[r] = strdup(name);
    g_reg_syntax[r] = strdup(syntax);
    g_reg_initial[r] = initial ? strdup(initial) : NULL;
    g_reg_inherits[r] = inherits;
    g_reg_rule[r] = rule;
    g_reg_doc[r] = g_reg_doc_cur;
    return true;
}

void css_reg_forget_rules(void) {
    int n = 0;
    for (int r = 0; r < g_reg_n; r++) {
        /* (another document's rules stay: an iframe's, while its parent restyles) */
        if (g_reg_rule[r] && g_reg_doc[r] == g_reg_doc_cur) { reg_free(r); continue; }
        g_reg_name[n] = g_reg_name[r];
        g_reg_syntax[n] = g_reg_syntax[r];
        g_reg_initial[n] = g_reg_initial[r];
        g_reg_inherits[n] = g_reg_inherits[r];
        g_reg_rule[n] = g_reg_rule[r];
        g_reg_doc[n] = g_reg_doc[r];
        n++;
    }
    g_reg_n = n;
}

void css_reg_reset(void) {
    for (int r = 0; r < g_reg_n; r++) reg_free(r);
    g_reg_n = 0;
}

/* The compiled grammar of a syntax string (each is compiled once). */
static GNode *syntax_grammar(const char *syntax) {
    static char *names[1024];
    static GNode *grammars[1024];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], syntax)) return grammars[i];
    GNode *g = g_compile(syntax);
    if (n < 1024) { names[n] = strdup(syntax); grammars[n++] = g; }
    return g;
}

static bool syntax_is_universal(const char *syntax) {
    while (*syntax && isspace((unsigned char)*syntax)) syntax++;
    if (*syntax != '*') return false;
    syntax++;
    while (*syntax && isspace((unsigned char)*syntax)) syntax++;
    return !*syntax;
}

/* A syntax string with its escapes written out (malloc'd). */
static char *syntax_unescape(const char *s)
{
    Buf b = { 0 };
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) { buf_ch(&b, *s); continue; }
        s++;
        if (!isxdigit((unsigned char)*s)) { buf_ch(&b, *s); continue; }
        unsigned c = 0;
        int n = 0;
        while (n < 6 && isxdigit((unsigned char)*s)) { c = c * 16 + (unsigned)(isdigit((unsigned char)*s) ? *s - '0' : (tolower((unsigned char)*s) - 'a' + 10)); s++; n++; }
        if (!isspace((unsigned char)*s)) s--;
        if (c < 0x80) buf_ch(&b, (char)c);
        else if (c < 0x800) { buf_ch(&b, (char)(0xC0 | c >> 6)); buf_ch(&b, (char)(0x80 | (c & 63))); }
        else if (c < 0x10000) { buf_ch(&b, (char)(0xE0 | c >> 12)); buf_ch(&b, (char)(0x80 | ((c >> 6) & 63))); buf_ch(&b, (char)(0x80 | (c & 63))); }
        else { buf_ch(&b, (char)(0xF0 | c >> 18)); buf_ch(&b, (char)(0x80 | ((c >> 12) & 63))); buf_ch(&b, (char)(0x80 | ((c >> 6) & 63))); buf_ch(&b, (char)(0x80 | (c & 63))); }
    }
    return buf_take(&b);
}

/* Whether the keywords of a value are written as the syntax has them, and its resolutions aren't below nothing. */
static bool syntax_exact(const char *syntax, const Toks *ts)
{
    for (int i = 0; i < ts->n; i++) {
        const Tok *t = &ts->t[i];
        if (t->type == T_DIM && t->num < 0 && strstr(syntax, "<resolution>") &&
            (!strcasecmp(t->str, "dpi") || !strcasecmp(t->str, "dpcm") || !strcasecmp(t->str, "dppx") || !strcasecmp(t->str, "x"))) return false;
        if (t->type != T_IDENT) continue;
        size_t l = strlen(t->str);
        bool some = false, same = false;
        for (const char *p = syntax; *p; p++) {
            if (strncasecmp(p, t->str, l)) continue;
            bool whole = (p == syntax || isspace((unsigned char)p[-1]) || p[-1] == '|') && (!p[l] || isspace((unsigned char)p[l]) || p[l] == '|' || p[l] == '#' || p[l] == '+');
            if (!whole) continue;
            some = true;
            if (!strncmp(p, t->str, l)) same = true;
        }
        if (some && !same) return false;
    }
    return true;
}

/* The value matched against a syntax string, serialized (malloc'd), or NULL. */
char *css_syntax_parse(const char *syntax, const char *value) {
    if (syntax_is_universal(syntax)) return trim_dup(value);
    char *v = trim_dup(value);
    char *plain = strchr(syntax, '\\') ? syntax_unescape(syntax) : NULL;
    if (plain) syntax = plain;
    GNode *g = v[0] ? syntax_grammar(syntax) : NULL;
    Toks ts;
    if (!g || !tokenize(v, strlen(v), &ts)) { free(v); free(plain); return NULL; }
    Matcher m;
    memset(&m, 0, sizeof m);
    m.ts = &ts;
    m.cur = -1;
    bool ok = ts.n > 0;
    for (int i = 0; ok && i < ts.n; i++) if (ts.t[i].type == T_BAD || ts.t[i].type == T_SEMI) ok = false;
    ok = ok && match_node(&m, g, 0, ts.n, at_end, NULL) && syntax_exact(syntax, &ts);
    free(m.hits);
    toks_free(&ts);
    free(plain);
    if (ok) return v;
    free(v);
    return NULL;
}

/* Which part of a syntax string the value is of: its <type> (without the
 * brackets, into type) and multiplier ('+', '#' or 0). */
bool css_syntax_component(const char *syntax, const char *value, char *type, size_t cap, char *mult) {
    const char *s = syntax;
    for (;;) {
        const char *e = strchr(s, '|');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        char part[128];
        if (n < sizeof part) {
            memcpy(part, s, n);
            part[n] = 0;
            char *p = part;
            while (*p && isspace((unsigned char)*p)) p++;
            size_t l = strlen(p);
            while (l && isspace((unsigned char)p[l - 1])) p[--l] = 0;
            char *got = css_syntax_parse(p, value);
            if (got) {
                free(got);
                *mult = 0;
                if (l && (p[l - 1] == '+' || p[l - 1] == '#')) { *mult = p[l - 1]; p[--l] = 0; }
                if (l >= 2 && p[0] == '<' && p[l - 1] == '>') { p[l - 1] = 0; p++; }
                snprintf(type, cap, "%s", p);
                return true;
            }
        }
        if (!e) return false;
        s = e + 1;
    }
}

/* The @property rules of a sheet's text: those with all they need are registered. */
void css_reg_scan_css(const char *css, size_t len) {
    const char *end = css + len, *p = css;
    while (p < end) {
        const char *at = NULL;
        for (const char *q = p; q + 9 <= end; q++)
            if (*q == '@' && !strncasecmp(q + 1, "property", 8)) { at = q; break; }
        if (!at) return;
        const char *q = at + 9;
        while (q < end && isspace((unsigned char)*q)) q++;
        const char *ns = q;
        while (q < end && !isspace((unsigned char)*q) && *q != '{') q++;
        char name[128] = "";
        if ((size_t)(q - ns) < sizeof name) { memcpy(name, ns, (size_t)(q - ns)); name[q - ns] = 0; }
        while (q < end && *q != '{') q++;
        if (q >= end) return;
        const char *body = ++q;
        int depth = 1;
        while (q < end && depth) { if (*q == '{') depth++; else if (*q == '}') depth--; q++; }
        const char *bend = depth ? q : q - 1;
        p = q;
        char *syntax = NULL, *initial = NULL;
        int inherits = -1;
        for (const char *d = body; d < bend;) {
            while (d < bend && (isspace((unsigned char)*d) || *d == ';')) d++;
            if (bend - d >= 2 && d[0] == '/' && d[1] == '*') {
                d += 2;
                while (d + 1 < bend && !(d[0] == '*' && d[1] == '/')) d++;
                d += 2;
                continue;
            }
            const char *dn = d;
            while (d < bend && *d != ':' && *d != ';') d++;
            if (d >= bend || *d != ':') break;
            size_t nl = (size_t)(d - dn);
            while (nl && isspace((unsigned char)dn[nl - 1])) nl--;
            const char *vs = ++d;
            char quote = 0;
            int nest = 0;
            for (; d < bend; d++) {
                if (quote) { if (*d == '\\') d++; else if (*d == quote) quote = 0; }
                else if (*d == '"' || *d == '\'') quote = *d;
                else if (*d == '(') nest++;
                else if (*d == ')') nest--;
                else if (*d == ';' && nest <= 0) break;
            }
            char *v = strndup(vs, (size_t)(d - vs));
            char *t = trim_dup(v);
            free(v);
            size_t tl = strlen(t);
            if (nl == 6 && !strncasecmp(dn, "syntax", 6) && tl >= 2 && (t[0] == '"' || t[0] == '\'')) {
                free(syntax);
                syntax = strndup(t + 1, tl - 2);
            } else if (nl == 8 && !strncasecmp(dn, "inherits", 8)) {
                inherits = !strcasecmp(t, "true") ? 1 : !strcasecmp(t, "false") ? 0 : -1;
            } else if (nl == 13 && !strncasecmp(dn, "initial-value", 13)) {
                free(initial);
                initial = strdup(t);
            }
            free(t);
        }
        bool ok = name[0] == '-' && name[1] == '-' && name[2] && syntax && inherits >= 0 && css_syntax_valid(syntax);
        if (ok && !syntax_is_universal(syntax)) {
            char *c = initial ? css_syntax_parse(syntax, initial) : NULL;
            ok = c && !css_value_depends_on_context(initial);
            if (ok) { free(initial); initial = c; } else free(c);
        }
        if (ok) css_reg_add(name, syntax, inherits == 1, initial, true);
        free(syntax);
        free(initial);
    }
}

/* A syntax string's own grammar (css-properties-values-api 5.3). */
bool css_syntax_valid(const char *syntax) {
    static const char *const types[] = { "angle", "color", "custom-ident", "image", "integer", "length",
        "length-percentage", "number", "percentage", "resolution", "string", "time", "url",
        "transform-function", "transform-list" };
    if (syntax_is_universal(syntax)) return true;
    if (strchr(syntax, 92)) {
        char *plain = syntax_unescape(syntax);
        bool ok = !strchr(plain, 92) && css_syntax_valid(plain);
        free(plain);
        return ok;
    }
    const char *s = syntax;
    if (!*s) return false;
    for (;;) {
        while (*s && isspace((unsigned char)*s)) s++;
        const char *b = s;
        const char *e = b;
        while (*e && *e != '|' && !isspace((unsigned char)*e)) e++;
        const char *after = e;
        while (*after && isspace((unsigned char)*after)) after++;
        if (*after && *after != '|') return false;
        size_t n = (size_t)(e - b);
        if (!n) return false;
        char mult = (b[n - 1] == '+' || b[n - 1] == '#') ? b[n - 1] : 0;
        if (mult) n--;
        if (!n) return false;
        if (b[0] == '<') {
            if (n < 3 || b[n - 1] != '>') return false;
            bool known = false;
            for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
                if (strlen(types[i]) == n - 2 && !strncmp(types[i], b + 1, n - 2)) known = true;
            if (!known) return false;
            if (mult && n - 2 == 14 && !strncmp(b + 1, "transform-list", 14)) return false;
        } else {
            /* (a custom ident, not a CSS-wide keyword) */
            if (!(isalpha((unsigned char)b[0]) || b[0] == '-' || b[0] == '_' || (unsigned char)b[0] >= 0x80)) return false;
            for (size_t i = 0; i < n; i++) {
                unsigned char c = (unsigned char)b[i];
                if (!(isalnum(c) || c == '-' || c == '_' || c >= 0x80 || c == '\\')) return false;
            }
            static const char *const wide[] = { "initial", "inherit", "unset", "revert", "revert-layer", "default" };
            for (size_t i = 0; i < sizeof(wide) / sizeof(wide[0]); i++)
                if (strlen(wide[i]) == n && !strncasecmp(wide[i], b, n)) return false;
        }
        if (!*after) return true;
        s = after + 1;
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) return false;
    }
}

/* Whether a value depends on what it is computed for: font or viewport units, var(). */
bool css_value_depends_on_context(const char *v) {
    /* (the viewport is the same for all: its units are independent) */
    static const char *const units[] = { "em", "ex", "ch", "ic", "cap", "lh", "rem", "rex", "rch", "ric", "rcap", "rlh",
        "cqw", "cqh", "cqi", "cqb", "cqmin", "cqmax" };
    if (strstr(v, "var(") || strstr(v, "env(")) return true;
    Toks ts;
    if (!tokenize(v, strlen(v), &ts)) return false;
    bool dep = false;
    for (int i = 0; i < ts.n && !dep; i++) {
        if (ts.t[i].type != T_DIM) continue;
        for (size_t u = 0; u < sizeof(units) / sizeof(units[0]); u++)
            if (ts.t[i].str && !strcasecmp(ts.t[i].str, units[u])) dep = true;
    }
    toks_free(&ts);
    return dep;
}
