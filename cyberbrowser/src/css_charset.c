/* A fetched stylesheet's bytes as UTF-8 (CSS Syntax 3, section 3.2: the
 * encoding from a BOM, else the HTTP charset, else @charset, else the
 * referrer's; decoded per the WHATWG Encoding standard). */
#include "css_charset.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "sjis_index.h"

const char *encoding_name_for_label(const char *s, size_t n);

/* windows-1252's 0x80..0x9F (the rest are Latin-1). */
static const uint16_t k_1252[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

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

static void dec_1252(const uint8_t *s, size_t n, Out *o)
{
    for (size_t i = 0; i < n; i++) put(o, s[i] >= 0x80 && s[i] < 0xA0 ? k_1252[s[i] - 0x80] : s[i]);
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
    if (!enc || !strcmp(enc, "UTF-8") || !strcmp(enc, "replacement") ||
        (strcmp(enc, "UTF-16BE") && strcmp(enc, "UTF-16LE") && strcmp(enc, "windows-1252") &&
         strcmp(enc, "Shift_JIS"))) {
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
    else if (!strcmp(enc, "windows-1252")) dec_1252(s, n, &o);
    else dec_sjis(s, n, &o);
    put(&o, 'x');   /* (room for the terminator) */
    if (!o.p) return NULL;
    o.n--;
    o.p[o.n] = 0;
    *out_len = o.n;
    return o.p;
}
