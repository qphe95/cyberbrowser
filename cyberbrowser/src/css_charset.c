/* A fetched stylesheet's bytes as UTF-8 (CSS Syntax 3, section 3.2: the
 * encoding from a BOM, else the HTTP charset, else @charset, else the
 * referrer's; decoded per the WHATWG Encoding standard). */
#include "css_charset.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sjis_index.h"
#include "charset_tables.h"

const char *encoding_name_for_label(const char *s, size_t n);


typedef struct { char *p; size_t n, cap; } Out;

static void put(Out *o, uint32_t cp)
{
    if (o->n + 4 >= o->cap) {
        size_t nc = o->cap * 2 + 64;
        char *np = (char *)realloc(o->p, nc);
        if (!np) return;
        o->p = np;
        o->cap = nc;
    }
    if (cp == 0 || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) cp = 0xFFFD;
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
        if (!strcmp(k_single_byte[i].name, enc)) return &k_single_byte[i];
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

bool css_sheet_type_ok(const char *h, size_t hn)
{
    for (size_t i = 0; h && i < hn;) {
        size_t e = i;
        while (e < hn && h[e] != '\n') e++;
        if (e - i > 13 && !strncasecmp(h + i, "content-type:", 13)) {
            size_t k = i + 13;
            while (k < e && (h[k] == ' ' || h[k] == '\t')) k++;
            if (k >= e || h[k] == '\r') return true;   /* (empty: as none) */
            return e - k >= 8 && !strncasecmp(h + k, "text/css", 8) &&
                   (k + 8 >= e || h[k + 8] == ';' || h[k + 8] == ' ' || h[k + 8] == '\r');
        }
        i = e + 1;
    }
    return true;
}
