/* net_decode.c - the network process's reading of a tab's messages
 * (net_decode.h).  No library calls: the prover sees all of it. */
#include "net_decode.h"

/* (o is one of NetRequest's offsets.) */
/*@ requires o <= 28 && \valid_read(m + (o .. o + 3));
    assigns \nothing;
    ensures \result == le32(m, o);
 */
static uint32_t rd32(const uint8_t *m, uint32_t o) {
    uint32_t b0 = m[o], b1 = m[o + 1], b2 = m[o + 2], b3 = m[o + 3];
    /*@ assert b0 <= 255 && b1 <= 255 && b2 <= 255 && b3 <= 255; */
    return b0 + 256u * b1 + 65536u * b2 + 16777216u * b3;
}

/* m[off .. off + size) is a string and its NUL. */
/*@ requires \valid_read(m + (off .. off + size - 1));
    requires off + size <= 0xffffffff;
    assigns \nothing;
    ensures \result <==> size >= 1 && cstr(m, off, size - 1);
 */
static bool str_ok(const uint8_t *m, uint32_t off, uint32_t size) {
    if (size == 0) return false;
    /*@ loop invariant off <= i <= off + size - 1;
        loop invariant no_nul(m, off, i - off);
        loop assigns i;
        loop variant off + size - 1 - i;
     */
    for (uint32_t i = off; i < off + size - 1; i++)
        if (m[i] == 0) return false;
    return m[off + size - 1] == 0;
}

bool net_decode_request(const uint8_t *m, uint32_t len, NetReq *q) {
    if (len < NET_REQUEST_FIXED) return false;
    uint32_t method_len = rd32(m, 4), url_len = rd32(m, 8), headers_len = rd32(m, 12), nheaders = rd32(m, 16);
    uint32_t body_len = rd32(m, 20), mode = rd32(m, 24), initiator_len = rd32(m, 28);
    uint64_t need = (uint64_t)NET_REQUEST_FIXED + method_len + url_len + headers_len + initiator_len + body_len;
    if (need != len || nheaders > NET_HEADERS_MAX || mode > NET_MODE_MAX || initiator_len > NET_INITIATOR_MAX ||
        method_len < 2 || url_len < 2)
        return false;
    uint32_t mo = NET_REQUEST_FIXED, uo = mo + method_len, ho = uo + url_len, io = ho + headers_len;
    uint32_t bo = io + initiator_len;
    if (!str_ok(m, mo, method_len) || !str_ok(m, uo, url_len)) return false;
    if (initiator_len && !str_ok(m, io, initiator_len)) return false;
    /* The headers: exactly nheaders strings, filling their part. */
    uint32_t k = 0, start = ho;
    /*@ loop invariant ho <= start <= i <= io;
        loop invariant 0 <= k <= nheaders;
        loop invariant no_nul(m, start, i - start);
        loop invariant \forall integer j; 0 <= j < k ==>
            q->header[j] + q->header_len[j] < start && cstr(m, q->header[j], q->header_len[j]);
        loop assigns i, k, start, q->header[0 .. NET_HEADERS_MAX - 1], q->header_len[0 .. NET_HEADERS_MAX - 1];
        loop variant io - i;
     */
    for (uint32_t i = ho; i < io; i++) {
        if (m[i]) continue;
        if (k >= nheaders) return false;
        q->header[k] = start;
        q->header_len[k] = i - start;
        k++;
        start = i + 1;
    }
    if (k != nheaders || start != io) return false;
    q->id = rd32(m, 0);
    q->mode = mode;
    q->nheaders = nheaders;
    q->method = mo;
    q->method_len = method_len - 1;
    q->url = uo;
    q->url_len = url_len - 1;
    if (initiator_len) {
        q->initiator = io;
        q->initiator_len = initiator_len - 1;
    } else {
        q->initiator = uo + url_len - 1;        /* the URL's NUL: "" */
        q->initiator_len = 0;
    }
    q->body = bo;
    q->body_len = body_len;
    return true;
}

bool net_decode_cookie_get(const uint8_t *m, uint32_t len, uint32_t *id, uint32_t *url_len) {
    if (len < 5) return false;
    uint32_t end = m[len - 1] == 0 ? len - 1 : len;
    if (end < 5) return false;
    /*@ loop invariant 4 <= i <= end;
        loop invariant no_nul(m, 4, i - 4);
        loop assigns i;
        loop variant end - i;
     */
    for (uint32_t i = 4; i < end; i++)
        if (m[i] == 0) return false;
    *id = rd32(m, 0);
    *url_len = end - 4;
    return true;
}

bool net_decode_cookie_set(const uint8_t *m, uint32_t len, uint32_t *url_len, uint32_t *line_len) {
    uint32_t u = 0;
    /*@ loop invariant 0 <= u <= len;
        loop invariant no_nul(m, 0, u);
        loop assigns u;
        loop variant len - u;
     */
    while (u < len && m[u] != 0) u++;
    if (u == 0 || u >= len) return false;
    uint32_t end = len;
    if (end > u + 1 && m[end - 1] == 0) end--;
    /*@ loop invariant u + 1 <= i <= end;
        loop invariant no_nul(m, u + 1, i - (u + 1));
        loop assigns i;
        loop variant end - i;
     */
    for (uint32_t i = u + 1; i < end; i++)
        if (m[i] == 0) return false;
    *url_len = u;
    *line_len = end - u - 1;
    return true;
}
