/* net_policy.c - the network process's decisions (net_policy.h): Private
 * Network Access, Opaque Response Blocking, the documents a tab is given
 * (P10), initiators and CORS.  No library calls: the prover sees all of it. */
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

/* ---- documents, initiators, CORS ------------------------------------------------------ */

bool net_same(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn) {
    if (an != bn) return false;
    /*@ loop invariant 0 <= i <= an;
        loop invariant \forall integer k; 0 <= k < i ==> a[k] == b[k];
        loop assigns i;
        loop variant an - i;
     */
    for (uint32_t i = 0; i < an; i++)
        if (a[i] != b[i]) return false;
    return true;
}

int net_doc_verdict(int mode, const uint8_t *tab_site, uint32_t tab_len, const uint8_t *doc_site, uint32_t doc_len) {
    if (mode != (int)NET_MODE_NAVIGATE) return NET_DOC_OK;
    return tab_len > 0 && net_same(tab_site, tab_len, doc_site, doc_len) ? NET_DOC_OK : NET_DOC_ELSEWHERE;
}

/* Entry k of f is o[0 .. n). */
/*@ requires \valid_read(f) && frames_ok(f) && 0 <= k < f->n && \valid_read(o + (0 .. n - 1));
    assigns \nothing;
    ensures \result <==> frame_is(f, k, o, n);
 */
static bool frame_eq(const NetFrames *f, uint32_t k, const uint8_t *o, uint32_t n) {
    if (f->len[k] != n) return false;
    /*@ loop invariant 0 <= i <= n;
        loop invariant \forall integer j; 0 <= j < i ==> f->origin[k][j] == o[j];
        loop assigns i;
        loop variant n - i;
     */
    for (uint32_t i = 0; i < n; i++)
        if (f->origin[k][i] != o[i]) return false;
    return true;
}

bool net_frames_has(const NetFrames *f, const uint8_t *o, uint32_t n) {
    /*@ loop invariant 0 <= k <= f->n;
        loop invariant \forall integer j; 0 <= j < k ==> !frame_is(f, j, o, n);
        loop assigns k;
        loop variant f->n - k;
     */
    for (uint32_t k = 0; k < f->n; k++)
        if (frame_eq(f, k, o, n)) return true;
    return false;
}

bool net_frames_add(NetFrames *f, const uint8_t *o, uint32_t n) {
    if (net_frames_has(f, o, n)) return true;
    if (n > NET_ORIGIN_MAX || f->n >= NET_FRAMES_MAX) return false;
    uint32_t k = f->n;
    /*@ loop invariant 0 <= i <= n;
        loop invariant \forall integer j; 0 <= j < i ==> f->origin[k][j] == o[j];
        loop assigns i, f->origin[k][0 .. NET_ORIGIN_MAX - 1];
        loop variant n - i;
     */
    for (uint32_t i = 0; i < n; i++) f->origin[k][i] = o[i];
    f->len[k] = n;
    f->n = k + 1;
    /*@ assert frame_is(f, k, o, n); */
    /*@ assert in_frames(f, o, n); */
    return true;
}

bool net_initiator_ok(const NetFrames *f, const uint8_t *tab_site, uint32_t tab_len, const uint8_t *init,
                      uint32_t init_len, const uint8_t *init_site, uint32_t init_site_len) {
    if (init_site_len > 0 && net_same(init_site, init_site_len, tab_site, tab_len)) return true;
    return net_frames_has(f, init, init_len);
}

/* "true", ignoring ASCII case. */
/*@ requires \valid_read(a + (0 .. n - 1));
    assigns \nothing;
    ensures \result <==> n == 4 && (a[0] == 't' || a[0] == 'T') && (a[1] == 'r' || a[1] == 'R') &&
                         (a[2] == 'u' || a[2] == 'U') && (a[3] == 'e' || a[3] == 'E');
 */
static bool is_true(const uint8_t *a, uint32_t n) {
    return n == 4 && (a[0] == 't' || a[0] == 'T') && (a[1] == 'r' || a[1] == 'R') && (a[2] == 'u' || a[2] == 'U') &&
           (a[3] == 'e' || a[3] == 'E');
}

int net_may_read(const NetRead *r) {
    if (r->mode < (int)NET_MODE_NO_CORS || r->mode > (int)NET_MODE_FRAME) return NET_READ_MODE;
    if (r->mode == (int)NET_MODE_NAVIGATE || r->mode == (int)NET_MODE_FRAME) return NET_READ_OK;
    /* An initiator the tab may not speak for is taken as the page. */
    bool counts = r->init_len > 0 && net_initiator_ok(r->frames, r->tab_site, r->tab_site_len, r->init, r->init_len,
                                                      r->init_site, r->init_site_len);
    bool cross = counts ? !net_same(r->final_origin, r->final_origin_len, r->init, r->init_len)
                        : !net_same(r->final_site, r->final_site_len, r->tab_site, r->tab_site_len);
    if (!cross) return NET_READ_OK;
    if (r->mode == (int)NET_MODE_CORS) {
        if (!r->have_acao) return NET_READ_NO_ACAO;
        bool star = r->acao_len == 1 && r->acao[0] == '*';
        if (r->credentialed && (star || !is_true(r->acac, r->acac_len))) return NET_READ_CREDENTIALS;
        if (star) return NET_READ_OK;
        if (counts) return net_same(r->acao, r->acao_len, r->init, r->init_len) ? NET_READ_OK : NET_READ_ACAO;
        return r->acao_site_len > 0 && net_same(r->acao_site, r->acao_site_len, r->tab_site, r->tab_site_len)
                   ? NET_READ_OK : NET_READ_ACAO;
    }
    return orb_verdict(r->mime, r->mime_len, r->nosniff, r->status, r->body, r->body_len) == ORB_ALLOW
               ? NET_READ_OK : NET_READ_ORB;
}
