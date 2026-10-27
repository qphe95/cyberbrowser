/* site.c - sites of URLs (site.h).  No library calls, bounded loops. */
#include "site.h"

/* The Public Suffix List (https://publicsuffix.org), generated from
 * third_party/psl/public_suffix_list.dat by tools/gen_psl.js. */
#include "psl_data.h"

static uint32_t len_of(const char *s, uint32_t cap) {
    uint32_t n = 0;
    while (n < cap && s[n]) n++;
    return n;
}

static bool same(const char *a, uint32_t alen, const char *b) {
    uint32_t i = 0;
    for (; i < alen; i++)
        if (!b[i] || b[i] != a[i]) return false;
    return b[i] == 0;
}

/* Compare a (alen bytes) with the string b by byte value: <0, 0 or >0. */
static int cmp(const char *a, uint32_t alen, const char *b) {
    for (uint32_t i = 0; i < alen; i++) {
        if (!b[i]) return 1;
        if (a[i] != b[i]) return (uint8_t)a[i] < (uint8_t)b[i] ? -1 : 1;
    }
    return b[alen] ? -1 : 0;
}

/* Is name (len bytes) in a sorted table of n strings: at most 14 steps for
 * the largest. */
static bool in_table(const char *str, const uint32_t *off, uint32_t n, const char *name, uint32_t len) {
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int c = cmp(name, len, str + off[mid]);
        if (c == 0) return true;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return false;
}

static bool host_ok(const char *h, uint32_t n) {
    if (n == 0 || n > SITE_HOST_MAX || h[0] == '.' || h[n - 1] == '.' || h[0] == '-') return false;
    uint32_t label = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = h[i];
        if (c == '.') {
            if (label == 0) return false;
            label = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        if (++label > 63) return false;
    }
    return true;
}

static bool is_ipv4(const char *h, uint32_t n) {
    uint32_t dots = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (h[i] == '.') dots++;
        else if (h[i] < '0' || h[i] > '9') return false;
    }
    return dots == 3;
}

bool site_url_host(const uint8_t *url, uint32_t len, char *scheme, uint32_t scheme_cap,
                   char *host, uint32_t host_cap) {
    uint32_t start;
    if (len > 7 && url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' &&
        url[4] == ':' && url[5] == '/' && url[6] == '/') {
        start = 7;
        if (scheme_cap < 5) return false;
        scheme[0] = 'h'; scheme[1] = 't'; scheme[2] = 't'; scheme[3] = 'p'; scheme[4] = 0;
    } else if (len > 8 && url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' &&
               url[4] == 's' && url[5] == ':' && url[6] == '/' && url[7] == '/') {
        start = 8;
        if (scheme_cap < 6) return false;
        scheme[0] = 'h'; scheme[1] = 't'; scheme[2] = 't'; scheme[3] = 'p'; scheme[4] = 's'; scheme[5] = 0;
    } else {
        return false;
    }
    /* The authority: up to '/', '?', '#' or '\'. */
    uint32_t end = start;
    while (end < len && url[end] != '/' && url[end] != '?' && url[end] != '#' && url[end] != '\\') end++;
    /* Without userinfo (up to the last '@'). */
    uint32_t h0 = start;
    for (uint32_t i = start; i < end; i++)
        if (url[i] == '@') h0 = i + 1;
    if (h0 < end && url[h0] == '[') return false;          /* IPv6: no site yet */
    /* Without the port. */
    uint32_t h1 = h0;
    while (h1 < end && url[h1] != ':') h1++;
    for (uint32_t i = h1 + 1; i < end; i++)
        if (url[i] < '0' || url[i] > '9') return false;
    uint32_t n = h1 - h0;
    if (n > 0 && url[h1 - 1] == '.') n--;                   /* "example.com." */
    if (n == 0 || n + 1 > host_cap) return false;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = url[h0 + i];
        host[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    host[n] = 0;
    return host_ok(host, n);
}

bool site_of(const char *scheme, const char *host, char *out) {
    uint32_t slen = len_of(scheme, 8), n = len_of(host, SITE_HOST_MAX + 1);
    if (!host_ok(host, n) || !((slen == 4 && same(scheme, 4, "http")) || (slen == 5 && same(scheme, 5, "https"))))
        return false;
    /* Label starts, from the left. */
    uint32_t starts[128];
    uint32_t nlabels = 0;
    starts[nlabels++] = 0;
    for (uint32_t i = 0; i < n && nlabels < 128; i++)
        if (host[i] == '.') starts[nlabels++] = i + 1;
    uint32_t keep;                                   /* labels in the result */
    if (is_ipv4(host, n)) {
        keep = nlabels;
    } else {
        /* Labels in the public suffix, by the list's algorithm: the longest
         * matching rule wins, an exception rule beats every other, and with
         * no match the suffix is the last label (the implicit rule "*"). */
        uint32_t suffix = 1;
        for (uint32_t k = 1; k <= nlabels; k++) {
            const char *tail = host + starts[nlabels - k];
            uint32_t tlen = n - starts[nlabels - k];
            if (in_table(PSL_EXC_STR, PSL_EXC_OFF, PSL_EXC_N, tail, tlen)) {
                suffix = k - 1;
                break;
            }
            if (in_table(PSL_PLAIN_STR, PSL_PLAIN_OFF, PSL_PLAIN_N, tail, tlen)) suffix = k;
            if (k >= 2) {
                uint32_t p = starts[nlabels - k + 1];  /* the parent of tail */
                if (in_table(PSL_WILD_STR, PSL_WILD_OFF, PSL_WILD_N, host + p, n - p)) suffix = k;
            }
        }
        keep = suffix + 1 <= nlabels ? suffix + 1 : nlabels;
    }
    uint32_t from = starts[nlabels - keep], dlen = n - from;
    if (slen + 1 + dlen + 1 > SITE_MAX) return false;
    uint32_t o = 0;
    for (uint32_t i = 0; i < slen; i++) out[o++] = scheme[i];
    out[o++] = '+';
    for (uint32_t i = 0; i < dlen; i++) out[o++] = host[from + i];
    out[o] = 0;
    return true;
}

bool site_label_ok(const char *label) {
    uint32_t n = len_of(label, SITE_MAX);
    if (n == 0 || n >= SITE_MAX) return false;
    uint32_t plus = 0;
    for (uint32_t i = 0; i < n; i++) {
        char c = label[i];
        if (c == '+') plus++;
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    return plus == 1 && label[0] != '+' && label[n - 1] != '+' && label[n - 1] != '.';
}
