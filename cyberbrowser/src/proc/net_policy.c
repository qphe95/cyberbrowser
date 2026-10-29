/* net_policy.c - Private Network Access and Opaque Response Blocking
 * decisions (net_policy.h).  No library calls: the prover sees all of it. */
#include "net_policy.h"

/* ---- address spaces ----------------------------------------------------------------- */

int net_space_v4(const uint8_t *a) {
    if (a[0] == 127 || a[0] == 0) return NET_SPACE_LOCAL;
    if (a[0] == 10) return NET_SPACE_PRIVATE;
    if (a[0] == 172 && a[1] >= 16 && a[1] <= 31) return NET_SPACE_PRIVATE;
    if (a[0] == 192 && a[1] == 168) return NET_SPACE_PRIVATE;
    if (a[0] == 169 && a[1] == 254) return NET_SPACE_PRIVATE;
    if (a[0] == 100 && a[1] >= 64 && a[1] <= 127) return NET_SPACE_PRIVATE;
    return NET_SPACE_PUBLIC;
}

int net_space_v6(const uint8_t *a) {
    bool zero10 = true;                 /* the first ten bytes are 0 */
    /*@ loop invariant 0 <= i <= 10;
        loop invariant zero10 <==> (\forall integer k; 0 <= k < i ==> a[k] == 0);
        loop assigns i, zero10;
        loop variant 10 - i;
     */
    for (int i = 0; i < 10; i++)
        if (a[i] != 0) zero10 = false;
    if (zero10 && a[10] == 0xff && a[11] == 0xff) return net_space_v4(a + 12);
    if (zero10 && a[10] == 0 && a[11] == 0 && a[12] == 0 && a[13] == 0 && a[14] == 0 && a[15] <= 1)
        return NET_SPACE_LOCAL;         /* :: and ::1 */
    if ((a[0] & 0xfe) == 0xfc) return NET_SPACE_PRIVATE;
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return NET_SPACE_PRIVATE;
    return NET_SPACE_PUBLIC;
}

bool net_space_allows(int client, int target) {
    /* (int: an enum's constants may be taken as unsigned, and these are
     * a peer's numbers.) */
    if (client < (int)NET_SPACE_PUBLIC || client > (int)NET_SPACE_LOCAL) return false;
    if (target < (int)NET_SPACE_PUBLIC || target > client) return false;
    return true;
}

/* ---- ORB ---------------------------------------------------------------------------- */

/*@ assigns \nothing; */
static uint8_t lc(uint8_t c) {
    return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c;
}

/* p[0 .. n) starts with lit[0 .. ln), ignoring ASCII case in p. */
/*@ requires \valid_read(p + (0 .. n - 1)) && \valid_read(lit + (0 .. ln - 1));
    requires ln <= 64;
    assigns \nothing;
 */
static bool starts(const uint8_t *p, uint32_t n, const char *lit, uint32_t ln) {
    if (n < ln) return false;
    /*@ loop invariant 0 <= i <= ln;
        loop assigns i;
        loop variant ln - i;
     */
    for (uint32_t i = 0; i < ln; i++)
        if (lc(p[i]) != (uint8_t)lit[i]) return false;
    return true;
}

/*@ requires \valid_read(p + (0 .. n - 1)) && \valid_read(lit + (0 .. ln - 1));
    requires ln <= 64;
    assigns \nothing;
 */
static bool is(const uint8_t *p, uint32_t n, const char *lit, uint32_t ln) {
    return n == ln && starts(p, n, lit, ln);
}

/*@ requires \valid_read(p + (0 .. n - 1)) && \valid_read(lit + (0 .. ln - 1));
    requires ln <= 64;
    assigns \nothing;
 */
static bool ends(const uint8_t *p, uint32_t n, const char *lit, uint32_t ln) {
    return n >= ln && starts(p + (n - ln), ln, lit, ln);
}

#define IS(p, n, s) is(p, n, s, sizeof(s) - 1)
#define STARTS(p, n, s) starts(p, n, s, sizeof(s) - 1)
#define ENDS(p, n, s) ends(p, n, s, sizeof(s) - 1)

/* By type alone: what a page may embed. */
/*@ requires \valid_read(m + (0 .. n - 1));
    assigns \nothing;
 */
static bool safelisted(const uint8_t *m, uint32_t n) {
    return STARTS(m, n, "image/") || STARTS(m, n, "audio/") || STARTS(m, n, "video/") ||
           STARTS(m, n, "font/") || STARTS(m, n, "application/font-") || STARTS(m, n, "application/x-font-") ||
           IS(m, n, "text/css") || IS(m, n, "text/javascript") || IS(m, n, "application/javascript") ||
           IS(m, n, "application/x-javascript") || IS(m, n, "application/ecmascript") ||
           IS(m, n, "text/ecmascript") || IS(m, n, "text/jscript") || IS(m, n, "text/x-javascript") ||
           IS(m, n, "application/x-ecmascript") || IS(m, n, "application/vnd.ms-fontobject") ||
           IS(m, n, "text/vtt") || IS(m, n, "application/dash+xml") ||
           IS(m, n, "application/vnd.apple.mpegurl") || IS(m, n, "application/x-mpegurl") ||
           IS(m, n, "application/ogg");
}

/* Types that are never a script, a style or media. */
/*@ requires \valid_read(m + (0 .. n - 1));
    assigns \nothing;
 */
static bool never_sniffed(const uint8_t *m, uint32_t n) {
    return IS(m, n, "application/pdf") || IS(m, n, "application/zip") || IS(m, n, "application/gzip") ||
           IS(m, n, "application/x-gzip") || IS(m, n, "application/x-protobuf") ||
           IS(m, n, "application/x-protobuffer") || IS(m, n, "application/msword") ||
           IS(m, n, "application/vnd.ms-excel") || IS(m, n, "application/vnd.ms-powerpoint") ||
           IS(m, n, "application/x-www-form-urlencoded") || IS(m, n, "text/event-stream") ||
           IS(m, n, "text/csv") || STARTS(m, n, "multipart/") ||
           STARTS(m, n, "application/vnd.openxmlformats-");
}

/* HTML, JSON or XML by type. */
/*@ requires \valid_read(m + (0 .. n - 1));
    assigns \nothing;
 */
static bool doc_type(const uint8_t *m, uint32_t n) {
    return IS(m, n, "text/html") || IS(m, n, "application/json") || IS(m, n, "text/json") ||
           ENDS(m, n, "+json") || IS(m, n, "text/xml") || IS(m, n, "application/xml") || ENDS(m, n, "+xml");
}

/*@ assigns \nothing; */
static bool space(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* An HTML tag's name at p, followed by a space or '>'. */
/*@ requires \valid_read(p + (0 .. n - 1)) && \valid_read(lit + (0 .. ln - 1));
    requires 1 <= ln <= 64;
    assigns \nothing;
 */
static bool tag(const uint8_t *p, uint32_t n, const char *lit, uint32_t ln) {
    return n > ln && starts(p, n, lit, ln) && (space(p[ln]) || p[ln] == '>');
}

#define TAG(p, n, s) tag(p, n, s, sizeof(s) - 1)
#define SNIFF_MAX 1024u             /* bytes looked at */

/* The body starts like HTML, XML or JSON (or a guard against reading
 * JSON as a script). */
/*@ requires \valid_read(body + (0 .. len - 1));
    assigns \nothing;
 */
static bool sniffs_as_doc(const uint8_t *body, uint32_t len) {
    uint32_t n = len < SNIFF_MAX ? len : SNIFF_MAX;
    uint32_t i = 0;
    if (n >= 3 && body[0] == 0xef && body[1] == 0xbb && body[2] == 0xbf) i = 3;
    /*@ loop invariant 0 <= i <= n;
        loop assigns i;
        loop variant n - i;
     */
    while (i < n && space(body[i])) i++;
    const uint8_t *p = body + i;
    uint32_t r = n - i;
    if (STARTS(p, r, ")]}'") || STARTS(p, r, "{}&&") || STARTS(p, r, "for(;;);") || STARTS(p, r, "while(1);"))
        return true;
    if (STARTS(p, r, "<?xml") || STARTS(p, r, "<!--") || TAG(p, r, "<!doctype html") || TAG(p, r, "<html") ||
        TAG(p, r, "<head") || TAG(p, r, "<body") || TAG(p, r, "<script") || TAG(p, r, "<iframe") ||
        TAG(p, r, "<style") || TAG(p, r, "<title") || TAG(p, r, "<table") || TAG(p, r, "<div") ||
        TAG(p, r, "<p") || TAG(p, r, "<a") || TAG(p, r, "<b") || TAG(p, r, "<br") || TAG(p, r, "<h1") ||
        TAG(p, r, "<font"))
        return true;
    /* A JSON object: '{', then a quoted key, then ':'. */
    if (r == 0 || p[0] != '{') return false;
    uint32_t k = 1;
    /*@ loop invariant 1 <= k <= r;
        loop assigns k;
        loop variant r - k;
     */
    while (k < r && space(p[k])) k++;
    if (k >= r || p[k] != '"') return false;
    k++;
    /*@ loop invariant 1 <= k <= r;
        loop assigns k;
        loop variant r - k;
     */
    while (k < r && p[k] != '"') {
        if (p[k] == '\\' && k + 1 < r) k++;
        k++;
    }
    if (k >= r) return false;
    k++;
    /*@ loop invariant 1 <= k <= r;
        loop assigns k;
        loop variant r - k;
     */
    while (k < r && space(p[k])) k++;
    return k < r && p[k] == ':';
}

int orb_verdict(const uint8_t *mime, uint32_t mime_len, bool nosniff, int status, const uint8_t *body,
                uint32_t body_len) {
    /* The essence: the type and subtype, before any parameters. */
    uint32_t s = 0;
    /*@ loop invariant 0 <= s <= mime_len;
        loop assigns s;
        loop variant mime_len - s;
     */
    while (s < mime_len && (mime[s] == ' ' || mime[s] == '\t')) s++;
    uint32_t e = s;
    /*@ loop invariant s <= e <= mime_len;
        loop assigns e;
        loop variant mime_len - e;
     */
    while (e < mime_len && mime[e] != ';' && mime[e] != ' ' && mime[e] != '\t') e++;
    const uint8_t *m = mime + s;
    uint32_t n = e - s;
    if (safelisted(m, n)) return ORB_ALLOW;
    if (never_sniffed(m, n)) return ORB_BLOCK;
    if (nosniff && (doc_type(m, n) || IS(m, n, "text/plain"))) return ORB_BLOCK;
    if (status < 200 || status > 299) return ORB_BLOCK;
    if (sniffs_as_doc(body, body_len)) return ORB_BLOCK;
    return ORB_ALLOW;
}
