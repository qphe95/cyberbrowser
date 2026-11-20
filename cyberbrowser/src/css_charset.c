/* A fetched stylesheet's bytes as UTF-8 (CSS Syntax 3, section 3.2: the
 * encoding from a BOM, else the HTTP charset, else @charset, else the
 * referrer's; decoded per the WHATWG Encoding standard). */
#include "css_charset.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sjis_index.h"
#include "charset_tables.h"

const char *encoding_name_for_label(const char *s, size_t n);


typedef struct { char *p; size_t n, cap; } Out;

static bool g_keep_nul;   /* (an HTML document's: U+0000 as it is, for the tokenizer) */
static void put(Out *o, uint32_t cp)
{
    if (o->n + 4 >= o->cap) {
        size_t nc = o->cap * 2 + 64;
        char *np = (char *)realloc(o->p, nc);
        if (!np) return;
        o->p = np;
        o->cap = nc;
    }
    if ((cp == 0 && !g_keep_nul) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
    if (cp < 0x80) o->p[o->n++] = (char)cp;
    else if (cp < 0x800) {
        o->p[o->n++] = (char)(0xC0 | (cp >> 6));
        o->p[o->n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o->p[o->n++] = (char)(0xE0 | (cp >> 12));
        o->p[o->n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o->p[o->n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        o->p[o->n++] = (char)(0xF0 | (cp >> 18));
        o->p[o->n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o->p[o->n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o->p[o->n++] = (char)(0x80 | (cp & 0x3F));
    }
}

static void dec_utf16(const uint8_t *s, size_t n, bool be, Out *o)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint32_t u = be ? (uint32_t)(s[i] << 8 | s[i + 1]) : (uint32_t)(s[i + 1] << 8 | s[i]);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
            uint32_t v = be ? (uint32_t)(s[i + 2] << 8 | s[i + 3]) : (uint32_t)(s[i + 3] << 8 | s[i + 2]);
            if (v >= 0xDC00 && v <= 0xDFFF) {
                put(o, 0x10000 + ((u - 0xD800) << 10) + (v - 0xDC00));
                i += 2;
                continue;
            }
        }
        put(o, u);
    }
    if (n & 1) put(o, 0xFFFD);
}


/* A WHATWG single-byte encoding's table, NULL for none of them. */
static const SingleByteTable *single_byte(const char *enc)
{
    for (size_t i = 0; i < sizeof(k_single_byte) / sizeof(k_single_byte[0]); i++)
        if (!strcasecmp(k_single_byte[i].name, enc)) return &k_single_byte[i];
    return NULL;
}

static void dec_single(const SingleByteTable *t, const uint8_t *s, size_t n, Out *o)
{
    for (size_t i = 0; i < n; i++)
        put(o, s[i] < 0x80 ? s[i] : t->hi[s[i] - 0x80] ? t->hi[s[i] - 0x80] : 0xFFFD);
}

/* Big5 (WHATWG): two bytes, lead 0x81..0xFE, trail 0x40..0x7E or 0xA1..0xFE. */
static void dec_big5(const uint8_t *s, size_t n, Out *o)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = s[i];
        if (b < 0x80) { put(o, b); continue; }
        if (b < 0x81 || b > 0xFE || i + 1 >= n) { put(o, 0xFFFD); continue; }
        uint8_t t = s[i + 1];
        if (!((t >= 0x40 && t <= 0x7E) || (t >= 0xA1 && t <= 0xFE))) {
            put(o, 0xFFFD);
            continue;   /* (the trail byte is read again on its own) */
        }
        i++;
        unsigned ptr = (unsigned)(b - 0x81) * 157u + (unsigned)(t - (t < 0x7F ? 0x40 : 0x62));
        bool two = false;
        for (size_t k = 0; k < sizeof(k_big5_pairs) / sizeof(k_big5_pairs[0]); k++)
            if (k_big5_pairs[k][0] == ptr) { put(o, k_big5_pairs[k][1]); put(o, k_big5_pairs[k][2]); two = true; }
        if (two) continue;
        uint32_t cp = ptr < sizeof(k_big5_index) / sizeof(k_big5_index[0]) ? k_big5_index[ptr] : 0;
        if (cp) put(o, cp);
        else {
            put(o, 0xFFFD);
            if (t < 0x80) put(o, t);
        }
    }
}

static void dec_sjis(const uint8_t *s, size_t n, Out *o)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t b = s[i];
        if (b <= 0x80) { put(o, b); continue; }
        if (b >= 0xA1 && b <= 0xDF) { put(o, 0xFF61u + (b - 0xA1u)); continue; }
        if ((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) {
            if (i + 1 >= n) { put(o, 0xFFFD); break; }
            uint8_t t = s[i + 1];
            if ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC)) {
                unsigned ptr = (unsigned)(b - (b < 0xA0 ? 0x81 : 0xC1)) * 188u + (unsigned)(t - (t < 0x7F ? 0x40 : 0x41));
                uint32_t cp = ptr >= 8836 && ptr <= 10715 ? 0xE000u - 8836u + ptr
                              : ptr < sizeof(k_sjis_index) / sizeof(k_sjis_index[0]) ? k_sjis_index[ptr] : 0;
                i++;
                if (cp) { put(o, cp); continue; }
                put(o, 0xFFFD);
                if (t < 0x80) put(o, t);   /* (an ASCII trail byte is kept) */
                continue;
            }
            put(o, 0xFFFD);
            continue;
        }
        put(o, 0xFFFD);
    }
}

/* The charset parameter of a Content-Type header in a raw header block. */
static const char *header_charset(const char *h, size_t hn, size_t *len)
{
    for (size_t i = 0; h && i < hn;) {
        size_t e = i;
        while (e < hn && h[e] != '\n') e++;
        if (e - i > 13 && !strncasecmp(h + i, "content-type:", 13)) {
            for (size_t k = i + 13; k + 8 <= e; k++) {
                if (strncasecmp(h + k, "charset=", 8)) continue;
                const char *v = h + k + 8;
                size_t l = 0;
                if (*v == '"') {
                    v++;
                    while (v + l < h + e && v[l] != '"') l++;
                } else {
                    while (v + l < h + e && v[l] != ';' && v[l] != '\r' && v[l] != ' ') l++;
                }
                *len = l;
                return v;
            }
        }
        i = e + 1;
    }
    return NULL;
}

char *css_sheet_decode(const char *bytes, size_t n, const char *headers, size_t headers_size,
                       const char *fallback_label, size_t *out_len, const char **enc_out)
{
    const uint8_t *s = (const uint8_t *)bytes;
    const char *enc = NULL;
    /* A BOM. */
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { enc = "UTF-8"; s += 3; n -= 3; }
    else if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) { enc = "UTF-16BE"; s += 2; n -= 2; }
    else if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) { enc = "UTF-16LE"; s += 2; n -= 2; }
    /* The transport's. */
    if (!enc) {
        size_t l = 0;
        const char *v = header_charset(headers, headers_size, &l);
        if (v) enc = encoding_name_for_label(v, l);
    }
    /* @charset "x"; exactly, at the very start. */
    if (!enc && n > 10 && !memcmp(s, "@charset \"", 10)) {
        size_t e = 10;
        while (e < n && e < 64 && s[e] != '"') e++;
        if (e + 1 < n && s[e] == '"' && s[e + 1] == ';') {
            enc = encoding_name_for_label((const char *)s + 10, e - 10);
            if (enc && (!strcmp(enc, "UTF-16BE") || !strcmp(enc, "UTF-16LE"))) enc = "UTF-8";
        }
    }
    /* The referrer's. */
    if (!enc && fallback_label) enc = encoding_name_for_label(fallback_label, strlen(fallback_label));
    if (enc_out) *enc_out = enc;
    const SingleByteTable *sb = enc ? single_byte(enc) : NULL;
    if (!enc || !strcmp(enc, "UTF-8") || !strcmp(enc, "replacement") ||
        (!sb && strcmp(enc, "UTF-16BE") && strcmp(enc, "UTF-16LE") && strcmp(enc, "Shift_JIS") &&
         strcmp(enc, "Big5"))) {
        /* (UTF-8, and those without a decoder here, as they are) */
        char *p = (char *)malloc(n + 1);
        if (!p) return NULL;
        memcpy(p, s, n);
        p[n] = 0;
        *out_len = n;
        return p;
    }
    Out o = { NULL, 0, 0 };
    if (!strcmp(enc, "UTF-16BE")) dec_utf16(s, n, true, &o);
    else if (!strcmp(enc, "UTF-16LE")) dec_utf16(s, n, false, &o);
    else if (sb) dec_single(sb, s, n, &o);
    else if (!strcmp(enc, "Big5")) dec_big5(s, n, &o);
    else dec_sjis(s, n, &o);
    put(&o, 'x');   /* (room for the terminator) */
    if (!o.p) return NULL;
    o.n--;
    o.p[o.n] = 0;
    *out_len = o.n;
    return o.p;
}

/* ---- a URL's query in a document's encoding (URL Standard 4.4, encoding override) ---- */

/* (the next code point of UTF-8 at *p, past it) */
static uint32_t utf8_next(const unsigned char **p, const unsigned char *e)
{
    const unsigned char *s = *p;
    uint32_t c = *s++;
    if (c >= 0xF0 && s + 3 <= e) { c = (c & 7) << 18 | (uint32_t)(s[0] & 63) << 12 | (uint32_t)(s[1] & 63) << 6 | (s[2] & 63); s += 3; }
    else if (c >= 0xE0 && s + 2 <= e) { c = (c & 15) << 12 | (uint32_t)(s[0] & 63) << 6 | (s[1] & 63); s += 2; }
    else if (c >= 0xC0 && s + 1 <= e) { c = (c & 31) << 6 | (s[0] & 63); s += 1; }
    *p = s;
    return c;
}

/* A URL string whose query (after its first "?", before "#") has non-ASCII code points: those percent-encoded in the
 * single-byte encoding enc, a character it lacks as "&#N;" (percent-encoded), as the URL parser does with an encoding
 * override.  NULL: nothing to do (UTF-8 or another encoding, no such query, or a scheme that is always UTF-8). */
char *url_legacy_query(const char *in, const char *enc)
{
    if (!in || !enc || !strcmp(enc, "UTF-8")) return NULL;
    const SingleByteTable *t = single_byte(enc);
    if (!t) return NULL;
    size_t n = strlen(in);
    const char *hash = memchr(in, '#', n);
    size_t pre = hash ? (size_t)(hash - in) : n;
    const char *q = memchr(in, '?', pre);
    if (!q) return NULL;
    bool wide = false;
    for (const char *c = q; c < in + pre; c++) if ((unsigned char)*c >= 0x80) { wide = true; break; }
    if (!wide) return NULL;
    /* (a scheme of its own: only special ones but ws/wss take the override) */
    {
        size_t k = 0;
        while (k < pre && (isalnum((unsigned char)in[k]) || in[k] == '+' || in[k] == '-' || in[k] == '.')) k++;
        if (k < pre && in[k] == ':' && k > 0 && isalpha((unsigned char)in[0])) {
            bool special = (k == 4 && !strncasecmp(in, "http", 4)) || (k == 5 && !strncasecmp(in, "https", 5)) ||
                           (k == 3 && !strncasecmp(in, "ftp", 3)) || (k == 4 && !strncasecmp(in, "file", 4));
            if (!special) return NULL;
        }
    }
    size_t cap = n * 12 + 16, o = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    memcpy(out, in, (size_t)(q - in) + 1);
    o = (size_t)(q - in) + 1;
    const unsigned char *p = (const unsigned char *)q + 1, *e = (const unsigned char *)in + pre;
    while (p < e) {
        if (*p < 0x80) { out[o++] = (char)*p++; continue; }
        uint32_t cp = utf8_next(&p, e);
        int b = -1;
        for (int k = 0; k < 128; k++) if (t->hi[k] == cp) { b = 0x80 + k; break; }
        if (b >= 0) o += (size_t)snprintf(out + o, cap - o, "%%%02X", b);
        else o += (size_t)snprintf(out + o, cap - o, "%%26%%23%u%%3B", (unsigned)cp);
    }
    if (hash) { memcpy(out + o, hash, n - pre); o += n - pre; }
    out[o] = 0;
    return out;
}

/* ---- an HTML document's bytes (HTML 13.2.3) ---- */

/* Whether enc is one this decodes (not UTF-8, which passes as it is). */
bool charset_decodable(const char *enc)
{
    if (!enc || !strcmp(enc, "UTF-8")) return false;
    return single_byte(enc) || !strcmp(enc, "UTF-16BE") || !strcmp(enc, "UTF-16LE") || !strcmp(enc, "Shift_JIS") || !strcmp(enc, "Big5");
}

/* A chunk of a document in enc, as UTF-8 (malloc'd); *keep: how many bytes at its end were held back (half a character),
 * to come again before the next chunk. */
char *charset_decode_chunk(const char *enc, const char *bytes, size_t n, bool last, size_t *out_len, size_t *keep)
{
    const uint8_t *s = (const uint8_t *)bytes;
    *keep = 0;
    const SingleByteTable *sb = single_byte(enc);
    bool u16 = !strcmp(enc, "UTF-16BE") || !strcmp(enc, "UTF-16LE");
    if (!last && u16) {
        size_t k = n & 1;
        /* (a high surrogate at the end: with its low one, next time) */
        if (n - k >= 2) {
            size_t i = n - k - 2;
            uint32_t u = !strcmp(enc, "UTF-16BE") ? (uint32_t)(s[i] << 8 | s[i + 1]) : (uint32_t)(s[i + 1] << 8 | s[i]);
            if (u >= 0xD800 && u <= 0xDBFF) k += 2;
        }
        *keep = k;
        n -= k;
    } else if (!last && !sb && n && s[n - 1] >= 0x80) {
        /* (Big5, Shift_JIS: a lead byte at the end waits for its trail) */
        size_t k = 0;
        while (k < n && k < 1 && s[n - 1 - k] >= 0x80) k++;
        *keep = k;
        n -= k;
    }
    Out o = { NULL, 0, 0 };
    g_keep_nul = true;
    if (!strcmp(enc, "UTF-16BE")) dec_utf16(s, n, true, &o);
    else if (!strcmp(enc, "UTF-16LE")) dec_utf16(s, n, false, &o);
    else if (sb) dec_single(sb, s, n, &o);
    else if (!strcmp(enc, "Big5")) dec_big5(s, n, &o);
    else dec_sjis(s, n, &o);
    g_keep_nul = false;
    if (!o.p) { o.p = (char *)malloc(1); if (!o.p) return NULL; }
    *out_len = o.n;
    return o.p;
}

/* (the prescan's "get an attribute": name and value at p, past it; false at a > or the end) */
static bool pre_attr(const uint8_t *s, size_t n, size_t *pp, char *name, size_t ncap, char *val, size_t vcap)
{
    size_t p = *pp, nl = 0, vl = 0;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 12 || s[p] == 13 || s[p] == 32 || s[p] == '/')) p++;
    if (p >= n || s[p] == '>') { *pp = p; return false; }
    name[0] = val[0] = 0;
    for (;;) {
        if (p >= n) { *pp = p; return false; }
        uint8_t c = s[p];
        if (c == '=' && nl) { p++; break; }
        if (c == 9 || c == 10 || c == 12 || c == 13 || c == 32) {
            while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 12 || s[p] == 13 || s[p] == 32)) p++;
            if (p < n && s[p] == '=') { p++; break; }
            name[nl] = 0; *pp = p; return true;
        }
        if (c == '/' || c == '>') { name[nl] = 0; *pp = p; return true; }
        if (nl + 1 < ncap) name[nl++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
        p++;
    }
    name[nl] = 0;
    while (p < n && (s[p] == 9 || s[p] == 10 || s[p] == 12 || s[p] == 13 || s[p] == 32)) p++;
    if (p >= n) { *pp = p; return false; }
    if (s[p] == '"' || s[p] == '\'') {
        uint8_t q = s[p++];
        while (p < n && s[p] != q) { if (vl + 1 < vcap) val[vl++] = (char)(s[p] >= 'A' && s[p] <= 'Z' ? s[p] + 32 : s[p]); p++; }
        if (p < n) p++;
    } else {
        while (p < n && !(s[p] == 9 || s[p] == 10 || s[p] == 12 || s[p] == 13 || s[p] == 32 || s[p] == '>')) {
            if (vl + 1 < vcap) val[vl++] = (char)(s[p] >= 'A' && s[p] <= 'Z' ? s[p] + 32 : s[p]);
            p++;
        }
    }
    val[vl] = 0;
    *pp = p;
    return true;
}

/* (a Content-Type's charset, HTML 2.6.? "algorithm for extracting a character encoding from a meta element") */
static const char *meta_content_charset(const char *v)
{
    for (const char *p = v; (p = strstr(p, "charset")) != NULL; p += 7) {
        const char *q = p + 7;
        while (*q == 9 || *q == 10 || *q == 12 || *q == 13 || *q == 32) q++;
        if (*q != '=') continue;
        q++;
        while (*q == 9 || *q == 10 || *q == 12 || *q == 13 || *q == 32) q++;
        size_t l = 0;
        if (*q == '"' || *q == '\'') {
            char c = *q++;
            while (q[l] && q[l] != c) l++;
            if (!q[l]) return NULL;
        } else while (q[l] && q[l] != ';' && q[l] != 9 && q[l] != 10 && q[l] != 12 && q[l] != 13 && q[l] != 32) l++;
        return l ? encoding_name_for_label(q, l) : NULL;
    }
    return NULL;
}

/* HTML 13.2.3.2: the prescan of a document's first bytes for a meta's encoding; NULL none.  As engines have it: a
 * tokenizer's, the text of script, style, title and the like skipped; any meta in the first 1024 bytes, and past them
 * only while still in the head (no </head>, no element a head does not hold).  *more: it needs bytes it does not have. */
static bool pre_ws(uint8_t c) { return c == 9 || c == 10 || c == 12 || c == 13 || c == 32; }
static const char *html_prescan(const uint8_t *s, size_t n, bool final, bool *more)
{
    size_t p = 0;
    bool in_head = true;
    *more = false;
#define NEED() do { if (!final) *more = true; return NULL; } while (0)
    while (p < n) {
        if (!in_head && p >= 1024) return NULL;
        if (s[p] != '<') { p++; continue; }
        if (p + 4 <= n && !memcmp(s + p, "<!--", 4)) {
            size_t k = p + 4;
            while (k + 3 <= n && !(s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>')) k++;
            if (k + 3 > n) NEED();
            p = k + 3;
            continue;
        }
        if (p + 1 >= n) NEED();
        bool end = s[p + 1] == '/';
        size_t q = p + (end ? 2 : 1);
        if (q < n && (((s[q] | 32) >= 'a' && (s[q] | 32) <= 'z'))) {
            char name[16];
            size_t nl = 0;
            while (q < n && !pre_ws(s[q]) && s[q] != '>' && s[q] != '/') { if (nl + 1 < sizeof name) name[nl++] = (char)(s[q] >= 'A' && s[q] <= 'Z' ? s[q] + 32 : s[q]); q++; }
            name[nl] = 0;
            if (q >= n) NEED();
            if (end) {
                while (q < n && s[q] != '>') q++;
                if (q >= n) NEED();
                if (!strcmp(name, "head")) in_head = false;
                p = q + 1;
                continue;
            }
            char an[64], av[256];
            bool got_pragma = false, seen_he = false, seen_ct = false, seen_cs = false;
            int need = 0;   /* 1 pragma, 2 charset */
            const char *cs = NULL;
            bool meta = !strcmp(name, "meta");
            while (pre_attr(s, n, &q, an, sizeof an, av, sizeof av)) {
                if (!meta) continue;
                if (!strcmp(an, "http-equiv") && !seen_he) { seen_he = true; if (!strcmp(av, "content-type")) got_pragma = true; }
                else if (!strcmp(an, "content") && !seen_ct && !cs) { seen_ct = true; const char *c = meta_content_charset(av); if (c) { cs = c; need = 1; } }
                else if (!strcmp(an, "charset") && !seen_cs) {
                    seen_cs = true; cs = encoding_name_for_label(av, strlen(av)); need = 2;
                    /* (character references in it: the tree builder's meta, its value decoded, changes the encoding the same way) */
                    if (!cs && strchr(av, '&')) {
                        char dv[256];
                        size_t o = 0;
                        for (size_t i = 0; av[i] && o + 1 < sizeof dv;) {
                            if (av[i] == '&' && av[i + 1] == '#') {
                                size_t j = i + 2;
                                bool hex = av[j] == 'x' || av[j] == 'X';
                                if (hex) j++;
                                unsigned long cp = 0;
                                size_t d0 = j;
                                while (av[j] && (hex ? isxdigit((unsigned char)av[j]) : isdigit((unsigned char)av[j]))) {
                                    cp = cp * (hex ? 16 : 10) + (unsigned long)(isdigit((unsigned char)av[j]) ? av[j] - '0' : (av[j] | 32) - 'a' + 10);
                                    if (cp > 0x10FFFF) cp = 0x110000;
                                    j++;
                                }
                                if (j > d0 && cp > 0 && cp < 0x80) { dv[o++] = (char)cp; i = av[j] == ';' ? j + 1 : j; continue; }
                            }
                            dv[o++] = av[i++];
                        }
                        dv[o] = 0;
                        cs = encoding_name_for_label(dv, o);
                    }
                }
            }
            if (q >= n) NEED();
            p = q + 1;
            if (meta && need && (need != 1 || got_pragma) && cs) {
                if (!strcmp(cs, "UTF-16BE") || !strcmp(cs, "UTF-16LE")) cs = "UTF-8";
                if (!strcmp(cs, "x-user-defined")) cs = "windows-1252";
                return cs;
            }
            /* (raw text: to its end tag) */
            static const char *const raw[] = { "script", "style", "title", "textarea", "xmp", "iframe", "noembed", "noframes", "noscript" };
            for (size_t r = 0; r < sizeof raw / sizeof raw[0]; r++) {
                if (strcmp(name, raw[r])) continue;
                size_t l = strlen(name), k = p;
                for (;; k++) {
                    if (k + 2 + l > n) NEED();
                    if (s[k] == '<' && s[k + 1] == '/' && !strncasecmp((const char *)s + k + 2, name, l)) break;
                }
                p = k;
                break;
            }
            static const char *const headish[] = { "html", "head", "script", "noscript", "style", "link", "meta", "object", "title", "base" };
            bool hd = false;
            for (size_t r = 0; r < sizeof headish / sizeof headish[0]; r++) if (!strcmp(name, headish[r])) hd = true;
            if (!hd) in_head = false;
            continue;
        }
        if (s[p + 1] == '!' || s[p + 1] == '/' || s[p + 1] == '?') {
            while (p < n && s[p] != '>') p++;
            if (p >= n) NEED();
            p++;
            continue;
        }
        p++;
    }
#undef NEED
    if (!final) *more = true;
    return NULL;
}

/* HTML 13.2.3.2 "get an XML encoding": a bogo-XML declaration's encoding; NULL none */
static const char *html_xml_decl(const uint8_t *s, size_t n)
{
    if (n < 5 || memcmp(s, "<?xml", 5)) return NULL;
    size_t gt = 5;
    while (gt < n && s[gt] != '>') gt++;
    if (gt >= n) return NULL;
    size_t e = 5;
    for (; e + 8 <= gt; e++) if (!memcmp(s + e, "encoding", 8)) break;
    if (e + 8 > gt) return NULL;
    e += 8;
    while (e < gt && s[e] <= 0x20) e++;
    if (e >= gt || s[e] != '=') return NULL;
    e++;
    while (e < gt && s[e] <= 0x20) e++;
    if (e >= gt || (s[e] != '"' && s[e] != '\'')) return NULL;
    uint8_t q = s[e++];
    size_t v = e;
    while (e < gt && s[e] != q) e++;
    if (e >= gt) return NULL;
    for (size_t k = v; k < e; k++) if (s[k] <= 0x20) return NULL;
    const char *enc = encoding_name_for_label((const char *)s + v, e - v);
    if (enc && (!strcmp(enc, "UTF-16BE") || !strcmp(enc, "UTF-16LE"))) enc = "UTF-8";
    return enc;
}

/* A document's encoding from its first bytes (at most 1024), the transport's charset label and the encoding its
 * parent's document has when that one may be inherited: the WHATWG name; "UTF-8" by default. *bom: the BOM's length. */
const char *html_sniff_encoding(const char *bytes, size_t n, const char *header_label, const char *inherit, size_t *bom,
                                bool final, bool *more)
{
    const uint8_t *s = (const uint8_t *)bytes;
    *bom = 0;
    *more = false;
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) { *bom = 3; return "UTF-8"; }
    if (n >= 2 && s[0] == 0xFE && s[1] == 0xFF) { *bom = 2; return "UTF-16BE"; }
    if (n >= 2 && s[0] == 0xFF && s[1] == 0xFE) { *bom = 2; return "UTF-16LE"; }
    if (header_label && header_label[0]) {
        const char *e = encoding_name_for_label(header_label, strlen(header_label));
        if (e) return e;
    }
    const char *e = html_prescan(s, n, final, more);
    if (*more) return NULL;
    if (!e) e = html_xml_decl(s, n < 1024 ? n : 1024);
    if (!e && inherit && inherit[0]) e = encoding_name_for_label(inherit, strlen(inherit));
    return e ? e : "UTF-8";
}

bool css_sheet_type_ok(const char *h, size_t hn)
{
    for (size_t i = 0; h && i < hn;) {
        size_t e = i;
        while (e < hn && h[e] != '\n') e++;
        if (e - i > 13 && !strncasecmp(h + i, "content-type:", 13)) {
            size_t k = i + 13;
            while (k < e && (h[k] == ' ' || h[k] == '\t')) k++;
            if (k >= e || h[k] == '\r') return true;   /* (empty: as none) */
            /* (not a MIME type, no "/" in it: as none) */
            {
                size_t q = k;
                while (q < e && h[q] != '/' && h[q] != ';' && h[q] != '\r') q++;
                if (q >= e || h[q] != '/') return true;
            }
            return e - k >= 8 && !strncasecmp(h + k, "text/css", 8) &&
                   (k + 8 >= e || h[k + 8] == ';' || h[k + 8] == ' ' || h[k + 8] == '\r');
        }
        i = e + 1;
    }
    return true;
}
