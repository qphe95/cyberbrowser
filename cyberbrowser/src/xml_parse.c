/*
 * xml_parse.c - XML documents into the node store (XML 1.0 and Namespaces
 * in XML, non-validating): elements and attributes in their namespaces,
 * character data with character and the predefined entity references,
 * CDATA sections, comments, processing instructions and the doctype (of its
 * internal subset, the general entities declared; with an XHTML doctype,
 * HTML's named references too, as browsers have them).  White space outside the root element is no
 * node; the XML declaration is none either.
 *
 * Not well-formed input stops the parse: the caller gets the message and
 * the position, and DOMParser makes a parsererror document of them.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dom_parse.h"
#include "dom_store.h"
#include "xml_parse.h"

#define XMLNS_URI "http://www.w3.org/2000/xmlns/"
#define XML_URI "http://www.w3.org/XML/1998/namespace"

typedef struct Binding { char *prefix; char *uri; int depth; } Binding;   /* prefix NULL: the default */
typedef struct Entity { char *name; char *text; size_t nl, tl; } Entity;  /* from the internal subset */

typedef struct P {
    const char *s, *e, *p;
    NodeId doc;
    char *err;
    size_t errlen;
    bool failed;
    Binding *b;
    int nb, capb;
    int depth;
    char *buf;              /* scratch for decoded text */
    size_t bufcap;
    Entity *ents;
    int nents;
    int ent_depth;          /* entity expansions open */
    bool html_ents;         /* an XHTML doctype: HTML's named references too */
} P;

static bool content(P *p, NodeId parent);
static void append(NodeId parent, NodeId child);

static bool fail(P *p, const char *msg) {
    if (!p->failed) {
        int line = 1, col = 1;
        for (const char *q = p->s; q < p->p && q < p->e; q++) {
            if (*q == '\n') { line++; col = 1; } else col++;
        }
        snprintf(p->err, p->errlen, "%s (line %d, column %d)", msg, line, col);
    }
    p->failed = true;
    return false;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static void skip_ws(P *p) { while (p->p < p->e && is_ws(*p->p)) p->p++; }
static bool at(P *p, const char *lit) {
    size_t n = strlen(lit);
    return (size_t)(p->e - p->p) >= n && !memcmp(p->p, lit, n);
}

/* ---- names ---- */

static uint32_t cp_at(const char *s, const char *e, size_t *len) {
    const unsigned char *u = (const unsigned char *)s;
    size_t n = (size_t)(e - s);
    if (u[0] < 0x80) { *len = 1; return u[0]; }
    if (u[0] < 0xE0 && n >= 2) { *len = 2; return ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu); }
    if (u[0] < 0xF0 && n >= 3) { *len = 3; return ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu); }
    if (n >= 4) {
        *len = 4;
        return ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
    }
    *len = 1;
    return 0xFFFD;
}

static bool name_start(uint32_t c) {
    return c == ':' || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0xD6) ||
           (c >= 0xD8 && c <= 0xF6) || (c >= 0xF8 && c <= 0x2FF) || (c >= 0x370 && c <= 0x37D) ||
           (c >= 0x37F && c <= 0x1FFF) || (c >= 0x200C && c <= 0x200D) || (c >= 0x2070 && c <= 0x218F) ||
           (c >= 0x2C00 && c <= 0x2FEF) || (c >= 0x3001 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFDCF) ||
           (c >= 0xFDF0 && c <= 0xFFFD) || (c >= 0x10000 && c <= 0xEFFFF);
}

static bool name_char(uint32_t c) {
    return name_start(c) || c == '-' || c == '.' || (c >= '0' && c <= '9') || c == 0xB7 ||
           (c >= 0x300 && c <= 0x36F) || (c >= 0x203F && c <= 0x2040);
}

/* A Name at p (its length; 0 if none). */
static size_t name_len(P *p) {
    const char *q = p->p;
    size_t l;
    if (q >= p->e || !name_start(cp_at(q, p->e, &l))) return 0;
    q += l;
    while (q < p->e && name_char(cp_at(q, p->e, &l))) q += l;
    return (size_t)(q - p->p);
}

/* ---- namespaces ---- */

static void bind(P *p, const char *prefix, size_t pn, const char *uri, size_t un) {
    if (p->nb == p->capb) {
        p->capb = p->capb ? p->capb * 2 : 16;
        p->b = realloc(p->b, (size_t)p->capb * sizeof(Binding));
    }
    Binding *b = &p->b[p->nb++];
    b->prefix = prefix ? strndup(prefix, pn) : NULL;
    b->uri = strndup(uri, un);
    b->depth = p->depth;
}

static void unbind_depth(P *p) {
    while (p->nb && p->b[p->nb - 1].depth >= p->depth) {
        p->nb--;
        free(p->b[p->nb].prefix);
        free(p->b[p->nb].uri);
    }
}

/* The URI bound to prefix (NULL: the default namespace); NULL if none, ""
 * for an undeclared default. */
static const char *lookup(P *p, const char *prefix, size_t pn) {
    if (prefix && pn == 3 && !memcmp(prefix, "xml", 3)) return XML_URI;
    if (prefix && pn == 5 && !memcmp(prefix, "xmlns", 5)) return XMLNS_URI;
    for (int i = p->nb - 1; i >= 0; i--) {
        Binding *b = &p->b[i];
        if (!prefix ? !b->prefix : (b->prefix && strlen(b->prefix) == pn && !memcmp(b->prefix, prefix, pn)))
            return b->uri;
    }
    return prefix ? NULL : "";
}

/* ---- character data ---- */

static void buf_put(P *p, size_t *n, const char *s, size_t len) {
    if (*n + len + 1 > p->bufcap) {
        while (*n + len + 1 > p->bufcap) p->bufcap = p->bufcap ? p->bufcap * 2 : 256;
        p->buf = realloc(p->buf, p->bufcap);
    }
    memcpy(p->buf + *n, s, len);
    *n += len;
    p->buf[*n] = 0;
}

static void buf_cp(P *p, size_t *n, uint32_t c) {
    char o[4];
    size_t l;
    if (c < 0x80) { o[0] = (char)c; l = 1; }
    else if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); l = 2; }
    else if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); l = 3; }
    else { o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63)); o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63)); l = 4; }
    buf_put(p, n, o, l);
}

/* A reference at p ('&'), decoded onto the buffer; in content (parent set)
 * an entity whose text has markup is parsed there, as content of parent. */
static bool reference(P *p, size_t *n, NodeId parent) {
    p->p++;
    if (p->p < p->e && *p->p == '#') {
        p->p++;
        bool hex = p->p < p->e && *p->p == 'x';
        if (hex) p->p++;
        uint32_t c = 0;
        const char *d = p->p;
        while (p->p < p->e && *p->p != ';') {
            char ch = *p->p;
            int v = ch >= '0' && ch <= '9' ? ch - '0' : hex && ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                  : hex && ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (v < 0 || c > 0x10FFFF) return fail(p, "invalid character reference");
            c = c * (hex ? 16 : 10) + (uint32_t)v;
            p->p++;
        }
        if (p->p >= p->e || p->p == d) return fail(p, "invalid character reference");
        p->p++;
        if (!(c == 0x9 || c == 0xA || c == 0xD || (c >= 0x20 && c <= 0xD7FF) || (c >= 0xE000 && c <= 0xFFFD) ||
              (c >= 0x10000 && c <= 0x10FFFF)))
            return fail(p, "character reference to an invalid character");
        buf_cp(p, n, c);
        return true;
    }
    static const struct { const char *name, *text; } pre[] = {
        { "lt", "<" }, { "gt", ">" }, { "amp", "&" }, { "quot", "\"" }, { "apos", "'" } };
    size_t l = name_len(p);
    if (!l || p->p + l >= p->e || p->p[l] != ';') return fail(p, "invalid entity reference");
    for (size_t i = 0; i < sizeof pre / sizeof pre[0]; i++)
        if (strlen(pre[i].name) == l && !memcmp(p->p, pre[i].name, l)) {
            buf_put(p, n, pre[i].text, 1);
            p->p += l + 1;
            return true;
        }
    for (int i = 0; i < p->nents; i++) {
        Entity *en = &p->ents[i];
        if (en->nl != l || memcmp(p->p, en->name, l)) continue;
        p->p += l + 1;
        if (!memchr(en->text, '<', en->tl) && !memchr(en->text, '&', en->tl)) {
            buf_put(p, n, en->text, en->tl);
            return true;
        }
        if (!parent) return fail(p, "markup in an entity in an attribute value");
        if (p->ent_depth >= 8) return fail(p, "entities nested too deeply");
        /* (its text then "</", where content stops, parsed in place of the input) */
        if (*n) { append(parent, dom_create_chardata(DOM_K_TEXT, p->buf, *n, p->doc)); *n = 0; }
        char *t = malloc(en->tl + 2);
        if (!t) return fail(p, "out of memory");
        memcpy(t, en->text, en->tl);
        memcpy(t + en->tl, "</", 2);
        const char *ss = p->s, *se = p->e, *sp = p->p;
        p->s = p->p = t;
        p->e = t + en->tl + 2;
        p->ent_depth++;
        bool ok = content(p, parent) && p->p == p->e - 2;
        if (!ok) fail(p, "invalid entity text");
        p->ent_depth--;
        p->s = ss;
        p->e = se;
        p->p = sp;
        free(t);
        if (p->buf) p->buf[0] = 0;
        return ok;
    }
    if (p->html_ents) {
        uint32_t c = dom_parse_named_entity(p->p, l);
        if (c) {
            buf_cp(p, n, c);
            p->p += l + 1;
            return true;
        }
    }
    return fail(p, "undefined entity");
}

/* ---- markup ---- */

static void append(NodeId parent, NodeId child) { dom_link_append(parent, child); }

static bool comment(P *p, NodeId parent) {
    p->p += 4;
    const char *b = p->p;
    while (p->p < p->e && !at(p, "--")) p->p++;
    if (!at(p, "-->")) return fail(p, at(p, "--") ? "\"--\" in a comment" : "unclosed comment");
    if (parent) append(parent, dom_create_chardata(DOM_K_COMMENT, b, (size_t)(p->p - b), p->doc));
    p->p += 3;
    return true;
}

static bool pi(P *p, NodeId parent) {
    p->p += 2;
    size_t l = name_len(p);
    if (!l) return fail(p, "processing instruction without a target");
    const char *t = p->p;
    p->p += l;
    if (l == 3 && (t[0] | 32) == 'x' && (t[1] | 32) == 'm' && (t[2] | 32) == 'l')
        return fail(p, "XML declaration not at the start");
    const char *d = p->p;
    if (!at(p, "?>")) {
        if (p->p >= p->e || !is_ws(*p->p)) return fail(p, "invalid processing instruction");
        skip_ws(p);
        d = p->p;
    }
    while (p->p < p->e && !at(p, "?>")) p->p++;
    if (!at(p, "?>")) return fail(p, "unclosed processing instruction");
    if (parent) append(parent, dom_create_pi(t, l, d, (size_t)(p->p - d), p->doc));
    p->p += 2;
    return true;
}

/* A quoted literal at p: its bytes (unquoted). */
static bool literal(P *p, const char **s, size_t *n) {
    if (p->p >= p->e || (*p->p != '"' && *p->p != '\'')) return fail(p, "expected a quoted literal");
    char q = *p->p++;
    *s = p->p;
    while (p->p < p->e && *p->p != q) p->p++;
    if (p->p >= p->e) return fail(p, "unclosed literal");
    *n = (size_t)(p->p - *s);
    p->p++;
    return true;
}

static bool doctype(P *p) {
    p->p += 9;
    skip_ws(p);
    size_t l = name_len(p);
    if (!l) return fail(p, "doctype without a name");
    const char *name = p->p, *pub = "", *sys = "";
    size_t pn = 0, sn = 0;
    p->p += l;
    skip_ws(p);
    if (at(p, "PUBLIC")) {
        p->p += 6;
        skip_ws(p);
        if (!literal(p, &pub, &pn)) return false;
        skip_ws(p);
        if (!literal(p, &sys, &sn)) return false;
    } else if (at(p, "SYSTEM")) {
        p->p += 6;
        skip_ws(p);
        if (!literal(p, &sys, &sn)) return false;
    }
    skip_ws(p);
    /* (the XHTML and MathML DTDs: HTML's named references) */
    if ((pn >= 17 && !memcmp(pub, "-//W3C//DTD XHTML", 17)) || (pn >= 18 && !memcmp(pub, "-//W3C//DTD MathML", 18)) ||
        (pn >= 22 && !memcmp(pub, "-//WAPFORUM//DTD XHTML", 22)))
        p->html_ents = true;
    if (p->p < p->e && *p->p == '[') {
        /* The internal subset: its general entities with literal text noted,
         * other declarations skipped (past quoted strings and comments). */
        for (p->p++, skip_ws(p); p->p < p->e && *p->p != ']';) {
            if (at(p, "<!--")) {
                while (p->p < p->e && !at(p, "-->")) p->p++;
                if (p->p < p->e) p->p += 3;
                continue;
            }
            if (at(p, "<!ENTITY") && p->p + 8 < p->e && is_ws(p->p[8])) {
                p->p += 8;
                skip_ws(p);
                size_t nl = *p->p == '%' ? 0 : name_len(p);
                const char *nm = p->p;
                if (nl) {
                    p->p += nl;
                    skip_ws(p);
                    const char *tx;
                    size_t tl;
                    if (p->p < p->e && (*p->p == '"' || *p->p == '\'')) {
                        if (!literal(p, &tx, &tl)) return false;
                        bool dup = false;   /* (the first declaration binds) */
                        for (int i = 0; i < p->nents; i++) dup |= p->ents[i].nl == nl && !memcmp(p->ents[i].name, nm, nl);
                        Entity *ne = dup ? NULL : realloc(p->ents, (size_t)(p->nents + 1) * sizeof(Entity));
                        if (ne) {
                            p->ents = ne;
                            Entity *en = &ne[p->nents++];
                            en->name = malloc(nl);
                            en->text = malloc(tl ? tl : 1);
                            en->nl = nl;
                            en->tl = tl;
                            if (en->name) memcpy(en->name, nm, nl);
                            if (en->text) memcpy(en->text, tx, tl);
                            if (!en->name || !en->text) p->nents--;
                        }
                    }
                }
            }
            /* (to the declaration's end) */
            char q = 0;
            for (; p->p < p->e; p->p++) {
                if (q) { if (*p->p == q) q = 0; continue; }
                if (*p->p == '"' || *p->p == '\'') q = *p->p;
                else if (*p->p == '>') { p->p++; break; }
                else if (*p->p == ']') break;
            }
            skip_ws(p);
        }
        if (p->p >= p->e) return fail(p, "unclosed internal subset");
        p->p++;
        skip_ws(p);
    }
    if (p->p >= p->e || *p->p != '>') return fail(p, "invalid doctype");
    p->p++;
    append(p->doc, dom_create_doctype(name, l, pub, pn, sys, sn, p->doc));
    return true;
}

typedef struct RawAttr { const char *name; size_t nl; char *value; size_t vl; } RawAttr;

static bool content(P *p, NodeId parent);

/* An element at p ('<' then a name), under parent. */
static bool element(P *p, NodeId parent) {
    p->p++;
    size_t l = name_len(p);
    if (!l) return fail(p, "invalid element name");
    const char *qn = p->p;
    p->p += l;
    RawAttr *attrs = NULL;
    int na = 0, cap = 0;
    bool empty = false, ok = true;
    p->depth++;
    for (;;) {
        bool ws = p->p < p->e && is_ws(*p->p);
        skip_ws(p);
        if (p->p >= p->e) { ok = fail(p, "unclosed start tag"); break; }
        if (*p->p == '>') { p->p++; break; }
        if (at(p, "/>")) { p->p += 2; empty = true; break; }
        size_t al = name_len(p);
        if (!al || !ws) { ok = fail(p, "invalid attribute"); break; }
        const char *an = p->p;
        p->p += al;
        skip_ws(p);
        if (p->p >= p->e || *p->p != '=') { ok = fail(p, "attribute without a value"); break; }
        p->p++;
        skip_ws(p);
        if (p->p >= p->e || (*p->p != '"' && *p->p != '\'')) { ok = fail(p, "unquoted attribute value"); break; }
        char q = *p->p++;
        size_t n = 0;
        if (p->buf) p->buf[0] = 0;
        while (p->p < p->e && *p->p != q) {
            char c = *p->p;
            if (c == '<') { ok = fail(p, "'<' in an attribute value"); break; }
            if (c == '&') { if (!reference(p, &n, 0)) { ok = false; break; } continue; }
            /* (attribute value normalization: white space characters are spaces) */
            if (c == '\t' || c == '\n' || c == '\r') { buf_put(p, &n, " ", 1); p->p++; continue; }
            buf_put(p, &n, p->p++, 1);
        }
        if (!ok) break;
        if (p->p >= p->e) { ok = fail(p, "unclosed attribute value"); break; }
        p->p++;
        for (int i = 0; i < na; i++)
            if (attrs[i].nl == al && !memcmp(attrs[i].name, an, al)) { ok = fail(p, "duplicate attribute"); break; }
        if (!ok) break;
        if (na == cap) { cap = cap ? cap * 2 : 8; attrs = realloc(attrs, (size_t)cap * sizeof(RawAttr)); }
        attrs[na++] = (RawAttr){ an, al, strndup(p->buf ? p->buf : "", n), n };
        /* namespace declarations */
        if (al == 5 && !memcmp(an, "xmlns", 5)) bind(p, NULL, 0, p->buf ? p->buf : "", n);
        else if (al > 6 && !memcmp(an, "xmlns:", 6)) {
            if (!n) { ok = fail(p, "a prefix bound to no namespace"); break; }
            bind(p, an + 6, al - 6, p->buf, n);
        }
    }
    NodeId el = DOM_NULL;
    if (ok) {
        const char *colon = memchr(qn, ':', l);
        const char *prefix = colon ? qn : NULL, *local = colon ? colon + 1 : qn;
        size_t pn = colon ? (size_t)(colon - qn) : 0, ln = colon ? l - pn - 1 : l;
        const char *uri = lookup(p, prefix, pn);
        if (!uri) ok = fail(p, "undeclared namespace prefix");
        else {
            el = dom_create_element_str(local, ln, p->doc);
            dom_set_element_name(el, dom_ns_of(uri, strlen(uri)), prefix, pn, local, ln);
            for (int i = 0; ok && i < na; i++) {
                RawAttr *a = &attrs[i];
                const char *c = memchr(a->name, ':', a->nl);
                uint32_t ans = 0;
                if (a->nl == 5 && !memcmp(a->name, "xmlns", 5)) ans = dom_name_intern(XMLNS_URI, strlen(XMLNS_URI));
                else if (c) {
                    const char *u = lookup(p, a->name, (size_t)(c - a->name));
                    if (!u) { ok = fail(p, "undeclared attribute prefix"); break; }
                    ans = dom_name_intern(u, strlen(u));
                }
                dom_set_attr_at(el, -1, ans, a->name, a->nl, a->value, a->vl);
            }
            if (ok) append(parent, el);
        }
    }
    for (int i = 0; i < na; i++) free(attrs[i].value);
    free(attrs);
    if (ok && !empty) {
        ok = content(p, el);
        if (ok) {
            /* the end tag */
            if (!at(p, "</")) ok = fail(p, "missing end tag");
            else {
                p->p += 2;
                if ((size_t)(p->e - p->p) < l || memcmp(p->p, qn, l) || (p->p + l < p->e && name_char((unsigned char)p->p[l])))
                    ok = fail(p, "mismatched end tag");
                else {
                    p->p += l;
                    skip_ws(p);
                    if (p->p >= p->e || *p->p != '>') ok = fail(p, "invalid end tag");
                    else p->p++;
                }
            }
        }
    }
    unbind_depth(p);
    p->depth--;
    return ok;
}

/* An element's content, to its end tag ("</"). */
static bool content(P *p, NodeId parent) {
    size_t n = 0;
    if (p->buf) p->buf[0] = 0;
    for (;;) {
        if (p->p >= p->e) return fail(p, "unclosed element");
        char c = *p->p;
        if (c == '<') {
            if (n) { append(parent, dom_create_chardata(DOM_K_TEXT, p->buf, n, p->doc)); n = 0; }
            if (at(p, "</")) return true;
            if (at(p, "<!--")) { if (!comment(p, parent)) return false; continue; }
            if (at(p, "<![CDATA[")) {
                p->p += 9;
                const char *b = p->p;
                while (p->p < p->e && !at(p, "]]>")) p->p++;
                if (p->p >= p->e) return fail(p, "unclosed CDATA section");
                append(parent, dom_create_chardata(DOM_K_CDATA, b, (size_t)(p->p - b), p->doc));
                p->p += 3;
                continue;
            }
            if (at(p, "<?")) { if (!pi(p, parent)) return false; continue; }
            if (at(p, "<!")) return fail(p, "invalid markup");
            if (!element(p, parent)) return false;
            if (p->buf) p->buf[0] = 0;
            continue;
        }
        if (c == '&') { if (!reference(p, &n, parent)) return false; continue; }
        if (at(p, "]]>")) return fail(p, "\"]]>\" in content");
        /* (line ends are normalized to \n) */
        if (c == '\r') {
            buf_put(p, &n, "\n", 1);
            p->p += p->p + 1 < p->e && p->p[1] == '\n' ? 2 : 1;
            continue;
        }
        const char *b = p->p;
        while (p->p < p->e && *p->p != '<' && *p->p != '&' && *p->p != '\r' && *p->p != ']') p->p++;
        if (p->p == b) p->p++;
        buf_put(p, &n, b, (size_t)(p->p - b));
    }
}

/* Comments, PIs and white space outside the root (misc). */
static bool misc(P *p, bool allow_doctype, bool *saw_doctype) {
    for (;;) {
        skip_ws(p);
        if (at(p, "<!--")) { if (!comment(p, p->doc)) return false; continue; }
        if (at(p, "<?")) { if (!pi(p, p->doc)) return false; continue; }
        if (allow_doctype && at(p, "<!DOCTYPE")) {
            if (*saw_doctype) return fail(p, "a second doctype");
            *saw_doctype = true;
            if (!doctype(p)) return false;
            continue;
        }
        return true;
    }
}

bool dom_parse_xml(const char *s, size_t n, NodeId doc, char *err, size_t errlen) {
    P p = { .s = s, .e = s + n, .p = s, .doc = doc, .err = err, .errlen = errlen };
    err[0] = 0;
    if (n >= 3 && !memcmp(s, "\xEF\xBB\xBF", 3)) p.p += 3;      /* (a BOM) */
    if (at(&p, "<?xml") && p.p + 5 < p.e && is_ws(p.p[5])) {
        while (p.p < p.e && !at(&p, "?>")) p.p++;
        if (p.p >= p.e) { fail(&p, "unclosed XML declaration"); goto done; }
        p.p += 2;
    }
    bool saw_doctype = false;
    if (!misc(&p, true, &saw_doctype)) goto done;
    if (p.p >= p.e || *p.p != '<') { fail(&p, "no root element"); goto done; }
    if (!element(&p, doc)) goto done;
    if (!misc(&p, false, &saw_doctype)) goto done;
    if (p.p < p.e) fail(&p, "content after the root element");
done:
    for (int i = 0; i < p.nb; i++) { free(p.b[i].prefix); free(p.b[i].uri); }
    free(p.b);
    for (int i = 0; i < p.nents; i++) { free(p.ents[i].name); free(p.ents[i].text); }
    free(p.ents);
    free(p.buf);
    return !p.failed;
}
