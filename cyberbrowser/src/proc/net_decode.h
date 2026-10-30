#ifndef NET_DECODE_H
#define NET_DECODE_H

/* The network process's reading of what a tab sends it (proto.h: TN_REQUEST,
 * TN_COOKIE_GET, TN_COOKIE_SET; SANDBOX_DESIGN.md section 8.7.3, step 3).
 * Pure C, no library calls, bounded loops, proved with Frama-C/WP
 * (tools/prove.sh net): free of undefined behavior for any bytes (P1),
 * bounded (P7), and a message is accepted only if it has the grammar below,
 * with every offset and length it passes on inside the message.  The shim
 * (net_main.c) uses the strings in place: each is NUL-terminated there. */

#include <stdbool.h>
#include <stdint.h>

#define NET_REQUEST_FIXED 32        /* sizeof(NetRequest): eight 32-bit fields */
#define NET_HEADERS_MAX 64
#define NET_INITIATOR_MAX 512       /* counting its NUL (NET_ORIGIN_MAX in net_policy.h) */
#define NET_MODE_MAX 3              /* NET_MODE_FRAME */

/* A TN_REQUEST, decoded: offsets into the message, string lengths without
 * their NUL. */
typedef struct NetReq {
    uint32_t id, mode, nheaders;
    uint32_t method, method_len;
    uint32_t url, url_len;
    uint32_t initiator, initiator_len;  /* none: the URL's NUL, length 0 (an empty string) */
    uint32_t body, body_len;
    uint32_t header[NET_HEADERS_MAX], header_len[NET_HEADERS_MAX];
} NetReq;

/*@ // A 32-bit little-endian field (x86's layout of NetRequest).
    logic integer le32{L}(uint8_t *m, integer o) =
        m[o] + 256 * m[o + 1] + 65536 * m[o + 2] + 16777216 * m[o + 3];

    // m[off .. off + n) has no NUL, and m[off + n] is one.
    predicate cstr{L}(uint8_t *m, integer off, integer n) =
        0 <= off && 0 <= n && (\forall integer i; off <= i < off + n ==> m[i] != 0) && m[off + n] == 0;

    // No NUL in m[off .. off + n).
    predicate no_nul{L}(uint8_t *m, integer off, integer n) =
        \forall integer i; off <= i < off + n ==> m[i] != 0;
 */

/* TN_REQUEST: NetRequest { id, method_len, url_len, headers_len, nheaders,
 * body_len, mode, initiator_len }, then the method, the URL, the headers,
 * the initiator and the body, which fill the message exactly:
 *   - method and URL: at least one byte other than NUL, then a NUL (the
 *     lengths count it);
 *   - headers: exactly nheaders (at most 64) strings, each NUL-terminated;
 *   - initiator: nothing, or a string and its NUL, at most 512 bytes;
 *   - mode at most NET_MODE_FRAME. */
/*@ requires \valid_read(m + (0 .. len - 1)) && \valid(q);
    requires \separated(q, m + (0 .. len - 1));
    assigns *q;
    ensures \result ==> len >= NET_REQUEST_FIXED &&
        NET_REQUEST_FIXED + le32(m, 4) + le32(m, 8) + le32(m, 12) + le32(m, 28) + le32(m, 20) == len;
    ensures \result ==> q->id == le32(m, 0) && q->nheaders == le32(m, 16) && q->mode == le32(m, 24);
    ensures \result ==> q->mode <= NET_MODE_MAX && q->nheaders <= NET_HEADERS_MAX;
    ensures \result ==> q->method_len >= 1 && q->method + q->method_len < len && cstr(m, q->method, q->method_len);
    ensures \result ==> q->url_len >= 1 && q->url + q->url_len < len && cstr(m, q->url, q->url_len);
    ensures \result ==> q->initiator_len < NET_INITIATOR_MAX && q->initiator + q->initiator_len < len &&
                        cstr(m, q->initiator, q->initiator_len);
    ensures \result ==> q->body + q->body_len == len && q->body_len == le32(m, 20);
    ensures \result ==> \forall integer k; 0 <= k < q->nheaders ==>
        q->header[k] + q->header_len[k] < len && cstr(m, q->header[k], q->header_len[k]);
 */
bool net_decode_request(const uint8_t *m, uint32_t len, NetReq *q);

/* TN_COOKIE_GET: NetCookieGet { id }, then the document's URL: at least one
 * byte, no NUL, and at most one NUL after it. */
/*@ requires \valid_read(m + (0 .. len - 1)) && \valid(id) && \valid(url_len);
    requires \separated(id, url_len, m + (0 .. len - 1));
    assigns *id, *url_len;
    ensures \result ==> len >= 5 && *id == le32(m, 0) && *url_len >= 1 && 4 + *url_len <= len &&
                        no_nul(m, 4, *url_len) && (4 + *url_len == len || (5 + *url_len == len && m[len - 1] == 0));
 */
bool net_decode_cookie_get(const uint8_t *m, uint32_t len, uint32_t *id, uint32_t *url_len);

/* TN_COOKIE_SET: the document's URL (at least one byte), a NUL, the cookie
 * line (no NUL), and at most one NUL after it.  The URL is m[0 .. url_len),
 * the line m[url_len + 1 .. url_len + 1 + line_len). */
/*@ requires \valid_read(m + (0 .. len - 1)) && \valid(url_len) && \valid(line_len);
    requires \separated(url_len, line_len, m + (0 .. len - 1));
    assigns *url_len, *line_len;
    ensures \result ==> *url_len >= 1 && cstr(m, 0, *url_len) && *url_len + 1 + *line_len <= len &&
                        no_nul(m, *url_len + 1, *line_len) &&
                        (*url_len + 1 + *line_len == len || (*url_len + 2 + *line_len == len && m[len - 1] == 0));
 */
bool net_decode_cookie_set(const uint8_t *m, uint32_t len, uint32_t *url_len, uint32_t *line_len);

#endif
