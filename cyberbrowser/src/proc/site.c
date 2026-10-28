/* site.c - sites of URLs (site.h).  No library calls, bounded loops: proved
 * with Frama-C/WP (tools/prove.sh) free of undefined behavior for any input,
 * and to make only well-formed labels (site_spec.h). */
#include "site.h"

/* The Public Suffix List (https://publicsuffix.org), generated from
 * third_party/psl/public_suffix_list.dat by tools/gen_psl.js. */
#include "psl_data.h"

/*@ requires \valid_read(s + (0 .. cap - 1));
    assigns \nothing;
    ensures 0 <= \result <= cap;
    ensures \forall integer i; 0 <= i < \result ==> s[i] != 0;
    ensures \result < cap ==> s[\result] == 0;
 */
static uint32_t len_of(const char *s, uint32_t cap) {
    uint32_t n = 0;
    /*@ loop invariant 0 <= n <= cap;
        loop invariant \forall integer i; 0 <= i < n ==> s[i] != 0;
        loop assigns n;
        loop variant cap - n;
     */
    while (n < cap && s[n]) n++;
    return n;
}

/* Is a (alen bytes) the string b, which has room for alen + 1 bytes. */
/*@ requires \valid_read(a + (0 .. alen - 1)) && \valid_read(b + (0 .. alen));
    assigns \nothing;
    ensures \result ==> \forall integer i; 0 <= i < alen ==> a[i] == b[i];
 */
static bool same(const char *a, uint32_t alen, const char *b) {
    uint32_t i = 0;
    /*@ loop invariant 0 <= i <= alen;
        loop invariant \forall integer k; 0 <= k < i ==> a[k] == b[k];
        loop assigns i;
        loop variant alen - i;
     */
    for (; i < alen; i++)
        if (!b[i] || b[i] != a[i]) return false;
    return b[i] == 0;
}

/* Compare a (alen bytes) with the string at b, of which bmax bytes may be
 * read (a missing NUL ends it there), by byte value: <0, 0 or >0. */
/*@ requires \valid_read(a + (0 .. alen - 1)) && \valid_read(b + (0 .. bmax - 1));
    assigns \nothing;
 */
static int cmp(const char *a, uint32_t alen, const char *b, uint32_t bmax) {
    /*@ loop invariant 0 <= i <= alen;
        loop assigns i;
        loop variant alen - i;
     */
    for (uint32_t i = 0; i < alen; i++) {
        if (i >= bmax || !b[i]) return 1;
        if (a[i] != b[i]) return (uint8_t)a[i] < (uint8_t)b[i] ? -1 : 1;
    }
    return alen < bmax && b[alen] ? -1 : 0;
}

/* Is name (len bytes) in a sorted table of n strings, packed in str (size
 * bytes) at the offsets off: at most 14 steps for the largest. */
/*@ requires \valid_read(str + (0 .. size - 1)) && \valid_read(off + (0 .. n - 1));
    requires \valid_read(name + (0 .. len - 1));
    assigns \nothing;
 */
static bool in_table(const char *str, uint32_t size, const uint32_t *off, uint32_t n, const char *name,
                     uint32_t len) {
    uint32_t lo = 0, hi = n;
    /*@ loop invariant 0 <= lo <= hi <= n;
        loop assigns lo, hi;
        loop variant hi - lo;
     */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (off[mid] >= size) return false;         /* not so in a generated table */
        int c = cmp(name, len, str + off[mid], size - off[mid]);
        if (c == 0) return true;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return false;
}

/*@ requires \valid_read(h + (0 .. n - 1));
    assigns \nothing;
    ensures \result ==> host_ok(h, n);
 */
static bool host_ok(const char *h, uint32_t n) {
    if (n == 0 || n > SITE_HOST_MAX || h[0] == '.' || h[n - 1] == '.' || h[0] == '-') return false;
    uint32_t label = 0;
    /*@ loop invariant 0 <= i <= n;
        loop invariant 0 <= label <= 63;
        loop invariant \forall integer k; 0 <= k < i ==> host_char(h[k]);
        loop invariant \forall integer k; 0 <= k < i - 1 ==> !(h[k] == '.' && h[k + 1] == '.');
        loop invariant i > 0 ==> (label == 0 <==> h[i - 1] == '.');
        loop assigns i, label;
        loop variant n - i;
     */
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

/*@ requires \valid_read(h + (0 .. n - 1));
    assigns \nothing;
 */
static bool is_ipv4(const char *h, uint32_t n) {
    uint32_t dots = 0;
    /*@ loop invariant 0 <= i <= n;
        loop invariant 0 <= dots <= i;
        loop assigns i, dots;
        loop variant n - i;
     */
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
    /*@ loop invariant start <= end <= len;
        loop assigns end;
        loop variant len - end;
     */
    while (end < len && url[end] != '/' && url[end] != '?' && url[end] != '#' && url[end] != '\\') end++;
    /* Without userinfo (up to the last '@'). */
    uint32_t h0 = start;
    /*@ loop invariant start <= i <= end;
        loop invariant start <= h0 <= end;
        loop assigns i, h0;
        loop variant end - i;
     */
    for (uint32_t i = start; i < end; i++)
        if (url[i] == '@') h0 = i + 1;
    if (h0 < end && url[h0] == '[') return false;          /* IPv6: no site yet */
    /* Without the port. */
    uint32_t h1 = h0;
    /*@ loop invariant h0 <= h1 <= end;
        loop assigns h1;
        loop variant end - h1;
     */
    while (h1 < end && url[h1] != ':') h1++;
    /* The port: digits only. */
    /*@ loop invariant h1 <= i <= end;
        loop assigns i;
        loop variant end - i;
     */
    for (uint32_t i = h1; i < end; i++)
        if (i > h1 && (url[i] < '0' || url[i] > '9')) return false;
    uint32_t n = h1 - h0;
    if (n > 0 && url[h1 - 1] == '.') n--;                   /* "example.com." */
    if (n == 0 || n + 1 > host_cap) return false;
    /*@ loop invariant 0 <= i <= n;
        loop assigns i, host[0 .. n - 1];
        loop variant n - i;
     */
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
    /*@ assert scheme_ok: \forall integer i; 0 <= i < slen ==> 'a' <= scheme[i] <= 'z'; */
    /* Label starts, from the left. */
    uint32_t starts[128];
    uint32_t nlabels = 0;
    starts[nlabels++] = 0;
    /*@ loop invariant 0 <= i <= n;
        loop invariant 1 <= nlabels <= 128;
        loop invariant starts[0] == 0;
        loop invariant \forall integer k; 1 <= k < nlabels ==> 1 <= starts[k] < n && host[starts[k] - 1] == '.';
        loop invariant \forall integer k; 1 <= k < nlabels ==> starts[k] <= i;
        loop assigns i, nlabels, starts[1 .. 127];
        loop variant n - i;
     */
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
        /*@ loop invariant 1 <= k <= nlabels + 1;
            loop invariant 0 <= suffix <= nlabels;
            loop assigns k, suffix;
            loop variant nlabels + 1 - k;
         */
        for (uint32_t k = 1; k <= nlabels; k++) {
            const char *tail = host + starts[nlabels - k];
            uint32_t tlen = n - starts[nlabels - k];
            if (in_table(PSL_EXC_STR, sizeof(PSL_EXC_STR), PSL_EXC_OFF, PSL_EXC_N, tail, tlen)) {
                suffix = k - 1;
                break;
            }
            if (in_table(PSL_PLAIN_STR, sizeof(PSL_PLAIN_STR), PSL_PLAIN_OFF, PSL_PLAIN_N, tail, tlen)) suffix = k;
            if (k >= 2) {
                uint32_t p = starts[nlabels - k + 1];  /* the parent of tail */
                if (in_table(PSL_WILD_STR, sizeof(PSL_WILD_STR), PSL_WILD_OFF, PSL_WILD_N, host + p, n - p))
                    suffix = k;
            }
        }
        keep = suffix + 1 <= nlabels ? suffix + 1 : nlabels;
    }
    uint32_t from = starts[nlabels - keep], dlen = n - from;
    if (slen + 1 + dlen + 1 > SITE_MAX) return false;
    uint32_t o = 0;
    /*@ loop invariant 0 <= i <= slen && o == i;
        loop invariant \forall integer k; 0 <= k < i ==> out[k] == scheme[k];
        loop assigns i, o, out[0 .. slen - 1];
        loop variant slen - i;
     */
    for (uint32_t i = 0; i < slen; i++) out[o++] = scheme[i];
    out[o++] = '+';
    /*@ loop invariant 0 <= i <= dlen && o == slen + 1 + i;
        loop invariant \forall integer k; 0 <= k < slen ==> out[k] == scheme[k];
        loop invariant out[slen] == '+';
        loop invariant \forall integer k; 0 <= k < i ==> out[slen + 1 + k] == host[from + k];
        loop assigns i, o, out[slen + 1 .. slen + dlen];
        loop variant dlen - i;
     */
    for (uint32_t i = 0; i < dlen; i++) out[o++] = host[from + i];
    out[o] = 0;
    return true;
}

bool site_label_ok(const char *label) {
    uint32_t n = len_of(label, SITE_MAX);
    if (n == 0 || n >= SITE_MAX) return false;
    uint32_t plus = 0;
    /*@ ghost uint32_t at = 0; */
    /*@ loop invariant 0 <= i <= n;
        loop invariant \forall integer k; 0 <= k < i ==> label_char(label[k]);
        loop invariant plus == 0 ==> \forall integer k; 0 <= k < i ==> label[k] != '+';
        loop invariant plus >= 1 ==> 0 <= at < i && label[at] == '+';
        loop invariant plus == 1 ==> \forall integer k; 0 <= k < i && k != at ==> label[k] != '+';
        loop invariant 0 <= plus <= i;
        loop assigns i, plus, at;
        loop variant n - i;
     */
    for (uint32_t i = 0; i < n; i++) {
        char c = label[i];
        if (c == '+') {
            plus++;
            /*@ ghost at = i; */
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
            return false;
        }
    }
    return plus == 1 && label[0] != '+' && label[n - 1] != '+' && label[n - 1] != '.';
}
