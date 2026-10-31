/* net_cache.c - the network process's HTTP cache (net_cache.h). */
#include "net_cache.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

int mbedtls_sha256(const unsigned char *input, size_t ilen, unsigned char *output, int is224);

static const char k_magic[8] = "CBHTTP2";
static const NetCacheIo *g_io;

void net_cache_init(const NetCacheIo *io) {
    g_io = io;
}

void net_cached_free(NetCached *c) {
    http_free_buffer(&c->b);
    memset(c, 0, sizeof(*c));
}

/* Now, ms since 1970. */
static int64_t now_ms(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (int64_t)((t - 116444736000000000ULL) / 10000ULL);
}

/* A header's value from a raw header block ("" if none). */
static bool header(const char *h, size_t len, const char *name, char *out, size_t cap) {
    size_t nl = strlen(name);
    out[0] = 0;
    for (size_t i = 0; i + nl + 1 < len;) {
        size_t e = i;
        while (e < len && h[e] != '\n') e++;
        if (e - i > nl && !_strnicmp(h + i, name, nl) && h[i + nl] == ':') {
            size_t v = i + nl + 1;
            while (v < e && (h[v] == ' ' || h[v] == '\t')) v++;
            size_t ve = e;
            while (ve > v && (h[ve - 1] == '\r' || h[ve - 1] == ' ' || h[ve - 1] == '\t')) ve--;
            snprintf(out, cap, "%.*s", (int)(ve - v), h + v);
            return true;
        }
        i = e + 1;
    }
    return false;
}

/* A Cache-Control directive's presence, and its number (or -1). */
static bool directive(const char *cc, const char *name, long long *value) {
    size_t nl = strlen(name);
    for (const char *p = cc; *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        if ((size_t)(e - p) >= nl && !_strnicmp(p, name, nl) && (p + nl == e || p[nl] == '=' || p[nl] == ' ')) {
            if (value) {
                *value = -1;
                if (p[nl] == '=') {
                    const char *v = p + nl + 1;
                    if (*v == '"') v++;
                    if (isdigit((unsigned char)*v)) *value = strtoll(v, NULL, 10);
                }
            }
            return true;
        }
        p = e;
    }
    return false;
}

/* An IMF-fixdate ("Sun, 06 Nov 1994 08:49:37 GMT") as ms since 1970; -1 if
 * it isn't one. */
static int64_t http_date(const char *s) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = "";
    int d, y, h, mi, se;
    const char *comma = strchr(s, ',');
    if (!comma || sscanf(comma + 1, " %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &se) != 6) return -1;
    const char *m = strstr(months, mon);
    if (!m || strlen(mon) != 3 || (m - months) % 3) return -1;
    int month = (int)(m - months) / 3;
    /* Days from 1970-01-01 (proleptic Gregorian). */
    int yy = y - (month < 2), era = (yy >= 0 ? yy : yy - 399) / 400;
    int yoe = yy - era * 400, mp = (month + 9) % 12, doy = (153 * mp + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + doe - 719468;
    return ((days * 24 + h) * 60 + mi) * 60000LL + se * 1000LL;
}

bool net_cache_request_ok(const char *method, const char *const *headers, uint32_t nheaders) {
    if (!g_io || strcmp(method, "GET")) return false;
    for (uint32_t i = 0; i < nheaders; i++) {
        const char *h = headers[i];
        if (!_strnicmp(h, "Range:", 6) || !_strnicmp(h, "Authorization:", 14)) return false;
        if (!_strnicmp(h, "Cache-Control:", 14) &&
            (directive(h + 14, "no-store", NULL) || directive(h + 14, "no-cache", NULL)))
            return false;
        if (!_strnicmp(h, "Pragma:", 7) && strstr(h, "no-cache")) return false;
    }
    return true;
}

static void path_of(const char *url, char path[64]) {
    uint8_t sha[32];
    mbedtls_sha256((const unsigned char *)url, strlen(url), sha, 0);
    static const char hex[] = "0123456789abcdef";
    memcpy(path, "cache/http/", 11);
    for (int i = 0; i < 16; i++) {
        path[11 + 2 * i] = hex[sha[i] >> 4];
        path[12 + 2 * i] = hex[sha[i] & 15];
    }
    path[43] = 0;
}

/* The request's side of a response's Vary (RFC 9111 4.1): each header it
 * names, with the value the request had ("name:value\n" each, "" when it
 * had none).  False if the response may not be kept: Vary: *, or a header
 * this client sets anew per request (Cookie).  The ones it sends the same on
 * every request match always and are left out. */
static bool vary_key(const char *h, size_t hl, const char *const *req, uint32_t nreq, char *out, size_t cap) {
    static const char *const constant[] = { "accept-encoding", "user-agent", "accept", "accept-language" };
    char v[512];
    size_t n = 0;
    out[0] = 0;
    if (!header(h, hl, "Vary", v, sizeof(v))) return true;
    for (char *p = v; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        char *e = p;
        while (*e && *e != ',') e++;
        char *ne = e;
        while (ne > p && (ne[-1] == ' ' || ne[-1] == '\t')) ne--;
        size_t nl = (size_t)(ne - p);
        if (nl) {
            if ((nl == 1 && *p == '*') || (nl == 6 && !_strnicmp(p, "cookie", 6))) return false;
            bool same = false;
            for (size_t k = 0; k < sizeof(constant) / sizeof(constant[0]) && !same; k++)
                same = strlen(constant[k]) == nl && !_strnicmp(p, constant[k], nl);
            if (!same) {
                const char *val = "";
                for (uint32_t i = 0; i < nreq; i++)
                    if (!_strnicmp(req[i], p, nl) && req[i][nl] == ':') {
                        val = req[i] + nl + 1;
                        while (*val == ' ' || *val == '\t') val++;
                        break;
                    }
                int w = snprintf(out + n, cap - n, "%.*s:%s\n", (int)nl, p, val);
                if (w < 0 || (size_t)w >= cap - n) return false;    /* (too long to keep) */
                for (size_t k = n; k < n + nl; k++) out[k] = (char)tolower((unsigned char)out[k]);
                n += (size_t)w;
            }
        }
        p = *e ? e + 1 : e;
    }
    return true;
}

/* Until when a response with these headers is fresh (ms since 1970; 0: it
 * must be revalidated); false if it may not be kept at all. */
static bool freshness(const char *h, size_t hl, int64_t now, int64_t *until) {
    char cc[512], v[256];
    header(h, hl, "Cache-Control", cc, sizeof(cc));
    if (directive(cc, "no-store", NULL) || header(h, hl, "Set-Cookie", v, sizeof(v))) return false;
    char etag[8], lm[64];
    bool validators = header(h, hl, "ETag", etag, sizeof(etag)) | header(h, hl, "Last-Modified", lm, sizeof(lm));
    long long n;
    *until = 0;
    if (directive(cc, "no-cache", NULL) || directive(cc, "must-revalidate", NULL)) {
        /* (must-revalidate: kept, but asked again once stale; here at once) */
    } else if (directive(cc, "max-age", &n) && n >= 0) {
        long long age = header(h, hl, "Age", v, sizeof(v)) ? strtoll(v, NULL, 10) : 0;
        if (n > age) *until = now + (n - age) * 1000LL;
    } else if (header(h, hl, "Expires", v, sizeof(v))) {
        int64_t exp = http_date(v), date = header(h, hl, "Date", v, sizeof(v)) ? http_date(v) : -1;
        if (exp > 0) *until = now + (exp - (date > 0 ? date : now));
        if (*until < now) *until = 0;
    } else if (header(h, hl, "Last-Modified", v, sizeof(v))) {
        int64_t last = http_date(v), date = header(h, hl, "Date", v, sizeof(v)) ? http_date(v) : now;
        if (last > 0 && date > last) {
            int64_t heuristic = (date - last) / 10;
            if (heuristic > 86400000LL) heuristic = 86400000LL;
            *until = now + heuristic;
        }
    }
    return *until > now || validators;
}

/* An entry: magic, fresh until (i64), status (u32), url, headers, the
 * request's side of Vary (vary_key), body (each a u32 length and its bytes). */
static void put_entry(const char *site, const char *url, int64_t until, int status, const char *h, size_t hl,
                      const char *vary, const char *body, size_t bl) {
    size_t ul = strlen(url), vl = strlen(vary), n = 8 + 8 + 4 + 4 + ul + 4 + hl + 4 + vl + 4 + bl;
    if (n > NET_CACHE_ENTRY_MAX) return;
    uint8_t *e = (uint8_t *)malloc(n), *p = e;
    if (!e) return;
    uint32_t u32;
    memcpy(p, k_magic, 8); p += 8;
    memcpy(p, &until, 8); p += 8;
    u32 = (uint32_t)status; memcpy(p, &u32, 4); p += 4;
    u32 = (uint32_t)ul; memcpy(p, &u32, 4); p += 4; memcpy(p, url, ul); p += ul;
    u32 = (uint32_t)hl; memcpy(p, &u32, 4); p += 4; memcpy(p, h, hl); p += hl;
    u32 = (uint32_t)vl; memcpy(p, &u32, 4); p += 4; memcpy(p, vary, vl); p += vl;
    u32 = (uint32_t)bl; memcpy(p, &u32, 4); p += 4; if (bl) memcpy(p, body, bl);
    char path[64];
    path_of(url, path);
    g_io->write(site, path, e, (uint32_t)n);
    free(e);
}

bool net_cache_lookup(const char *site, const char *url, const char *const *req, uint32_t nreq, NetCached *out) {
    memset(out, 0, sizeof(*out));
    if (!g_io || !site[0]) return false;
    char path[64];
    path_of(url, path);
    uint8_t *d = NULL;
    uint32_t n = 0;
    if (!g_io->read(site, path, &d, &n)) return false;
    /* Each length is checked against what is left before it is used. */
    const uint8_t *p = d, *end = d + n;
    int64_t until;
    uint32_t status, ul, hl, vl = 0, bl;
    bool ok = n >= 24 && !memcmp(p, k_magic, 8);
    if (ok) { p += 8; memcpy(&until, p, 8); p += 8; memcpy(&status, p, 4); p += 4; memcpy(&ul, p, 4); p += 4; }
    ok = ok && (size_t)(end - p) >= (size_t)ul + 4 && ul == strlen(url) && !memcmp(p, url, ul);
    if (ok) { p += ul; memcpy(&hl, p, 4); p += 4; }
    ok = ok && (size_t)(end - p) >= (size_t)hl + 4;
    const uint8_t *h = p;
    if (ok) { p += hl; memcpy(&vl, p, 4); p += 4; }
    ok = ok && vl < sizeof(out->vary) && (size_t)(end - p) >= (size_t)vl + 4;
    const uint8_t *vary = p;
    if (ok) { p += vl; memcpy(&bl, p, 4); p += 4; }
    ok = ok && (size_t)(end - p) == bl;
    /* (stored for a request whose Vary headers had other values: not this one's) */
    if (ok) {
        char want[sizeof(out->vary)];
        ok = vary_key((const char *)h, hl, req, nreq, want, sizeof(want)) && strlen(want) == vl &&
             !memcmp(want, vary, vl);
        if (ok) memcpy(out->vary, want, (size_t)vl + 1);
    }
    if (ok) {
        out->b.headers = (char *)malloc((size_t)hl + 1);
        out->b.data = (char *)malloc((size_t)bl + 1);
        ok = out->b.headers && out->b.data;
    }
    if (ok) {
        memcpy(out->b.headers, h, hl);
        out->b.headers[hl] = 0;
        out->b.headers_size = hl;
        memcpy(out->b.data, p, bl);
        out->b.data[bl] = 0;
        out->b.size = bl;
        out->status = (int)status;
        out->found = true;
        out->fresh = until > now_ms();
        header(out->b.headers, hl, "ETag", out->etag, sizeof(out->etag));
        header(out->b.headers, hl, "Last-Modified", out->last_modified, sizeof(out->last_modified));
    } else {
        net_cached_free(out);
    }
    free(d);
    return out->found;
}

void net_cache_store(const char *site, const char *url, const char *const *req, uint32_t nreq, int status,
                     const HttpBuffer *b) {
    if (!g_io || !site[0] || status != 200 || !b->headers) return;
    int64_t until;
    char vary[sizeof(((NetCached *)0)->vary)];
    if (!freshness(b->headers, b->headers_size, now_ms(), &until) ||
        !vary_key(b->headers, b->headers_size, req, nreq, vary, sizeof(vary)))
        return;
    put_entry(site, url, until, status, b->headers, b->headers_size, vary, b->data ? b->data : "",
              b->data ? b->size : 0);
}

bool net_cache_revalidated(const char *site, const char *url, const HttpBuffer *b304, NetCached *old) {
    if (!g_io || !old->found || !b304->headers) return false;
    /* The 304's caching headers, over the stored ones' (RFC 9111 4.3.4):
     * here, its freshness decides, the stored headers are kept. */
    int64_t until;
    char cc[512];
    header(b304->headers, b304->headers_size, "Cache-Control", cc, sizeof(cc));
    const HttpBuffer *by = cc[0] || strstr(b304->headers, "xpires") ? b304 : &old->b;
    if (!freshness(by->headers, by->headers_size, now_ms(), &until)) return false;
    put_entry(site, url, until, old->status, old->b.headers, old->b.headers_size, old->vary, old->b.data, old->b.size);
    old->fresh = true;
    return true;
}
