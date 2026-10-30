#ifndef NET_POLICY_H
#define NET_POLICY_H

/* The network process's decisions on what a page may reach and read
 * (SANDBOX_DESIGN.md section 2.4): Private Network Access, which documents
 * a tab is given, whom it may speak for, CORS and Opaque Response Blocking.
 * Pure C, no library calls, bounded loops, proved with Frama-C/WP
 * (tools/prove.sh): free of undefined behavior for any bytes (P1), bounded
 * (P7), an address the client may not reach is never allowed (P9), and a
 * tab is given only documents of its own site (P10).  The shim
 * (net_main.c) only extracts the strings these look at: sites (site_of),
 * origins, header values. */

#include <stdbool.h>
#include <stdint.h>

/* Address spaces, from the most public to the most private. */
enum { NET_SPACE_PUBLIC = 0, NET_SPACE_PRIVATE = 1, NET_SPACE_LOCAL = 2 };

/*@ predicate space_ok(integer s) = NET_SPACE_PUBLIC <= s <= NET_SPACE_LOCAL; */

/* An IPv4 address's space: loopback and 0.0.0.0/8 are local; 10/8,
 * 172.16/12, 192.168/16, 169.254/16 and 100.64/10 are private. */
/*@ requires \valid_read(a + (0 .. 3));
    assigns \nothing;
    ensures space_ok(\result);
 */
int net_space_v4(const uint8_t *a);

/* An IPv6 address's space: ::1 and :: are local; fc00::/7 and fe80::/10
 * are private; ::ffff:a.b.c.d is a.b.c.d's. */
/*@ requires \valid_read(a + (0 .. 15));
    assigns \nothing;
    ensures space_ok(\result);
 */
int net_space_v6(const uint8_t *a);

/* P9: a client in space `client` may reach `target` only if it is no more
 * private (public pages reach no private or local address, private pages
 * no local one). */
/*@ assigns \nothing;
    ensures \result == 1 ==> space_ok(client) && space_ok(target) && target <= client;
    ensures \result == 0 ==> !(space_ok(client) && space_ok(target) && target <= client);
 */
bool net_space_allows(int client, int target);

enum { ORB_BLOCK = 0, ORB_ALLOW = 1 };

/* Whether a cross-origin no-cors response may reach the page (ORB):
 * mime is the Content-Type header's value (as sent, may be empty), nosniff
 * whether X-Content-Type-Options is nosniff, status the HTTP status, body
 * the response (only its start is sniffed).
 *   - images, audio, video, fonts, scripts, stylesheets and media
 *     playlists by type: allowed;
 *   - types that are never anything else (PDF, ZIP, protobuf, CSV, ...):
 *     blocked;
 *   - HTML, JSON, XML or plain text with nosniff, or any non-2xx: blocked;
 *   - otherwise blocked if the body starts like HTML, XML or JSON (or a
 *     JSON-hijacking guard), else allowed (it may be a script). */
/*@ requires mime_len <= 4096 && \valid_read(mime + (0 .. mime_len - 1));
    requires \valid_read(body + (0 .. body_len - 1));
    assigns \nothing;
    ensures \result == ORB_BLOCK || \result == ORB_ALLOW;
 */
int orb_verdict(const uint8_t *mime, uint32_t mime_len, bool nosniff, int status, const uint8_t *body,
                uint32_t body_len);

/* ---- documents, initiators, CORS ------------------------------------------------------ */

/* What a request is for: HTTP_MODE_* of http_download.h (net_main.c checks
 * they agree), the tab's word. */
enum { NET_MODE_NO_CORS = 0, NET_MODE_CORS = 1, NET_MODE_NAVIGATE = 2, NET_MODE_FRAME = 3 };

/*@ predicate same{L}(uint8_t *a, integer an, uint8_t *b, integer bn) =
        an == bn && \forall integer i; 0 <= i < an ==> a[i] == b[i];
 */

/* Whether a[0 .. an) and b[0 .. bn) are the same bytes. */
/*@ requires \valid_read(a + (0 .. an - 1)) && \valid_read(b + (0 .. bn - 1));
    assigns \nothing;
    ensures \result <==> same(a, an, b, bn);
 */
bool net_same(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn);

enum { NET_DOC_OK = 0, NET_DOC_ELSEWHERE = 1 };

/* P10: may a document whose URL's site is doc_site (as site_of makes it, ""
 * if it has none) be given to a tab of tab_site ("" = opaque)?  The page's
 * own document (NET_MODE_NAVIGATE) only if it is of the tab's site, and an
 * opaque tab (about:blank, which fetches nothing) gets none: else it goes
 * elsewhere, to a tab of its own site.  A frame's document may be of any
 * site (frames share their embedder's process: section 9.1); the other
 * modes aren't documents. */
/*@ requires \valid_read(tab_site + (0 .. tab_len - 1)) && \valid_read(doc_site + (0 .. doc_len - 1));
    assigns \nothing;
    ensures \result == NET_DOC_OK || \result == NET_DOC_ELSEWHERE;
    ensures mode == NET_MODE_NAVIGATE ==>
        (\result == NET_DOC_OK <==> tab_len > 0 && same(tab_site, tab_len, doc_site, doc_len));
    ensures mode != NET_MODE_NAVIGATE ==> \result == NET_DOC_OK;
 */
int net_doc_verdict(int mode, const uint8_t *tab_site, uint32_t tab_len, const uint8_t *doc_site, uint32_t doc_len);

/* The origins a tab loaded frame documents from (an initiator it may name).
 * An origin longer than an initiator may be (NetRequest) is never named, so
 * it isn't kept. */
#define NET_ORIGIN_MAX 512
#define NET_FRAMES_MAX 64

typedef struct NetFrames {
    uint32_t n;
    uint32_t len[NET_FRAMES_MAX];
    uint8_t origin[NET_FRAMES_MAX][NET_ORIGIN_MAX];
} NetFrames;

/*@ predicate frames_ok{L}(NetFrames *f) =
        f->n <= NET_FRAMES_MAX && \forall integer k; 0 <= k < f->n ==> f->len[k] <= NET_ORIGIN_MAX;
    predicate frame_is{L}(NetFrames *f, integer k, uint8_t *o, integer n) =
        f->len[k] == n && \forall integer i; 0 <= i < n ==> f->origin[k][i] == o[i];
    predicate in_frames{L}(NetFrames *f, uint8_t *o, integer n) =
        \exists integer k; 0 <= k < f->n && frame_is(f, k, o, n);
 */

/*@ requires \valid_read(f) && frames_ok(f) && \valid_read(o + (0 .. n - 1));
    assigns \nothing;
    ensures \result <==> in_frames(f, o, n);
 */
bool net_frames_has(const NetFrames *f, const uint8_t *o, uint32_t n);

/* Add origin o; false (and nothing changed) if it is too long or the list
 * is full.  Entries are only ever added. */
/*@ requires \valid(f) && frames_ok(f) && \valid_read(o + (0 .. n - 1));
    requires \separated(f, o + (0 .. n - 1));
    assigns *f;
    ensures frames_ok(f);
    ensures \result <==> in_frames(f, o, n);
    ensures \old(f->n) <= f->n;
    ensures \forall integer k, i; 0 <= k < \old(f->n) && 0 <= i < NET_ORIGIN_MAX ==>
        f->len[k] == \old(f->len[k]) && f->origin[k][i] == \old(f->origin[k][i]);
 */
bool net_frames_add(NetFrames *f, const uint8_t *o, uint32_t n);

/*@ predicate initiator_ok{L}(NetFrames *f, uint8_t *tab_site, integer tab_len, uint8_t *init, integer init_len,
                              uint8_t *init_site, integer init_site_len) =
        (init_site_len > 0 && same(init_site, init_site_len, tab_site, tab_len)) || in_frames(f, init, init_len);
 */

/* Whether a tab of tab_site may speak for initiator init (an origin): one of
 * its own site (init_site: the origin's site, "" if none), or one it loaded
 * a frame document from. */
/*@ requires \valid_read(f) && frames_ok(f);
    requires \valid_read(tab_site + (0 .. tab_len - 1)) && \valid_read(init + (0 .. init_len - 1));
    requires \valid_read(init_site + (0 .. init_site_len - 1));
    assigns \nothing;
    ensures \result <==> initiator_ok(f, tab_site, tab_len, init, init_len, init_site, init_site_len);
 */
bool net_initiator_ok(const NetFrames *f, const uint8_t *tab_site, uint32_t tab_len, const uint8_t *init,
                      uint32_t init_len, const uint8_t *init_site, uint32_t init_site_len);

/* A response the tab asked for, as the shim extracts it: who asks, where it
 * ended (after redirects), and its headers. */
typedef struct NetRead {
    int mode;                                   /* NET_MODE_* */
    const uint8_t *tab_site; uint32_t tab_site_len;
    const uint8_t *init; uint32_t init_len;     /* the initiator the tab named (0: the page's) */
    const uint8_t *init_site; uint32_t init_site_len;
    const NetFrames *frames;
    const uint8_t *final_origin; uint32_t final_origin_len;
    const uint8_t *final_site; uint32_t final_site_len;
    bool credentialed;                          /* the request carried the site's cookie jar */
    bool have_acao;                             /* Access-Control-Allow-Origin, trimmed */
    const uint8_t *acao; uint32_t acao_len;
    const uint8_t *acao_site; uint32_t acao_site_len;   /* its site, "" if none */
    const uint8_t *acac; uint32_t acac_len;     /* Access-Control-Allow-Credentials, trimmed (0: none) */
    const uint8_t *mime; uint32_t mime_len;     /* Content-Type */
    bool nosniff;
    int status;
    const uint8_t *body; uint32_t body_len;
} NetRead;

enum {
    NET_READ_OK = 0,
    NET_READ_NO_ACAO,           /* CORS: no Access-Control-Allow-Origin */
    NET_READ_ACAO,              /* CORS: it names someone else */
    NET_READ_CREDENTIALS,       /* CORS: with the jar's cookies, not allowed with credentials */
    NET_READ_ORB,               /* no-cors: ORB blocked it */
    NET_READ_MODE,              /* no such mode */
};

/*@ predicate read_ok{L}(NetRead *r) =
        \valid_read(r) && \valid_read(r->frames) && frames_ok(r->frames) &&
        \valid_read(r->tab_site + (0 .. r->tab_site_len - 1)) && \valid_read(r->init + (0 .. r->init_len - 1)) &&
        \valid_read(r->init_site + (0 .. r->init_site_len - 1)) &&
        \valid_read(r->final_origin + (0 .. r->final_origin_len - 1)) &&
        \valid_read(r->final_site + (0 .. r->final_site_len - 1)) &&
        \valid_read(r->acao + (0 .. r->acao_len - 1)) && \valid_read(r->acao_site + (0 .. r->acao_site_len - 1)) &&
        \valid_read(r->acac + (0 .. r->acac_len - 1)) &&
        r->mime_len <= 4096 && \valid_read(r->mime + (0 .. r->mime_len - 1)) &&
        \valid_read(r->body + (0 .. r->body_len - 1));

    // The initiator counts: named, and one the tab may speak for.
    predicate counts{L}(NetRead *r) =
        r->init_len > 0 &&
        initiator_ok(r->frames, r->tab_site, r->tab_site_len, r->init, r->init_len, r->init_site, r->init_site_len);

    // From another origin than the initiator's, or (with none) another site
    // than the page's.
    predicate cross{L}(NetRead *r) =
        counts(r) ? !same(r->final_origin, r->final_origin_len, r->init, r->init_len)
                  : !same(r->final_site, r->final_site_len, r->tab_site, r->tab_site_len);

    predicate star{L}(NetRead *r) = r->acao_len == 1 && r->acao[0] == '*';
    predicate acac_true{L}(NetRead *r) =
        r->acac_len == 4 && (r->acac[0] == 't' || r->acac[0] == 'T') && (r->acac[1] == 'r' || r->acac[1] == 'R') &&
        (r->acac[2] == 'u' || r->acac[2] == 'U') && (r->acac[3] == 'e' || r->acac[3] == 'E');

    // CORS lets the initiator read it: Access-Control-Allow-Origin is * or
    // names the initiator (with none, an origin of the page's site); with
    // the jar's cookies, not *, and Access-Control-Allow-Credentials: true.
    predicate cors_ok{L}(NetRead *r) =
        r->have_acao &&
        (r->credentialed ==> !star(r) && acac_true(r)) &&
        (star(r) || (counts(r) && same(r->acao, r->acao_len, r->init, r->init_len)) ||
         (!counts(r) && r->acao_site_len > 0 && same(r->acao_site, r->acao_site_len, r->tab_site, r->tab_site_len)));
 */

/* Whether the tab may read this response.  Documents (the page's, a
 * frame's) pass here (net_doc_verdict decides the page's); so does what
 * isn't cross (above).  Else a cors response only as cors_ok says, and a
 * no-cors one only if orb_verdict allows it. */
/*@ requires read_ok(r);
    assigns \nothing;
    ensures NET_READ_OK <= \result <= NET_READ_MODE;
    ensures \result == NET_READ_OK ==>
        r->mode == NET_MODE_NAVIGATE || r->mode == NET_MODE_FRAME || !cross(r) ||
        (r->mode == NET_MODE_CORS && cors_ok(r)) || r->mode == NET_MODE_NO_CORS;
    ensures r->mode == NET_MODE_CORS && cross(r) ==> (\result == NET_READ_OK <==> cors_ok(r));
    ensures (r->mode == NET_MODE_NAVIGATE || r->mode == NET_MODE_FRAME || !cross(r)) &&
            NET_MODE_NO_CORS <= r->mode <= NET_MODE_FRAME ==> \result == NET_READ_OK;
    ensures !(NET_MODE_NO_CORS <= r->mode <= NET_MODE_FRAME) ==> \result == NET_READ_MODE;
 */
int net_may_read(const NetRead *r);

#endif
