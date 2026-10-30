#ifndef STORE_CORE_H
#define STORE_CORE_H

/* The storage service's rules (SANDBOX_DESIGN.md section 3.3), pure C: the
 * grammar of paths a client may name, and quotas.  The shim (store_main.c)
 * opens files only through store_join, after these checks.  Proved with
 * Frama-C/WP (tools/prove.sh): no undefined behavior for any input (P1), and
 * file containment and the quota (P4), as the contracts below say. */

#include <stdbool.h>
#include <stdint.h>
#include "site.h"

#define STORE_PATH_MAX 600              /* a relative path */
#define STORE_QUOTA_BYTES (64ull << 20) /* per site */

/*@ predicate rel_char(char c) = ('a' <= c <= 'z') || ('0' <= c <= '9') || c == '_' || c == '.' || c == '-';

    // DOS device names, which Windows opens as the device whatever the folder
    // and extension: con, prn, aux, nul, com0-9, lpt0-9, alone or before a dot.
    predicate dev3{L}(char *c) =
        (c[0] == 'c' && c[1] == 'o' && c[2] == 'n') || (c[0] == 'p' && c[1] == 'r' && c[2] == 'n') ||
        (c[0] == 'a' && c[1] == 'u' && c[2] == 'x') || (c[0] == 'n' && c[1] == 'u' && c[2] == 'l');
    predicate dev4{L}(char *c) =
        ((c[0] == 'c' && c[1] == 'o' && c[2] == 'm') || (c[0] == 'l' && c[1] == 'p' && c[2] == 't')) &&
        '0' <= c[3] <= '9';
    predicate device{L}(char *c, integer n) =
        (n >= 3 && (n == 3 || c[3] == '.') && dev3(c)) || (n >= 4 && (n == 4 || c[4] == '.') && dev4(c));

    // rel[a .. e - 1] is a component of rel (len bytes): between slashes or the ends.
    predicate component{L}(char *rel, integer len, integer a, integer e) =
        0 <= a <= e <= len && (a == 0 || rel[a - 1] == '/') && (e == len || rel[e] == '/') &&
        (\forall integer k; a <= k < e ==> rel[k] != '/');

    // A path a client may name (section 3.3): components of [a-z0-9_.-], none
    // empty, none starting with '.' (so no "." or ".."), none ending with '.'
    // (Windows drops trailing dots), none a device name.  It can't leave the
    // folder it's joined to: no '\', ':', drive, root, "..", stream or device.
    predicate rel_ok{L}(char *rel, integer len) =
        1 <= len <= STORE_PATH_MAX &&
        (\forall integer k; 0 <= k < len ==> rel_char(rel[k]) || rel[k] == '/') &&
        (\forall integer a, e; component(rel, len, a, e) ==>
            a < e && rel[a] != '.' && rel[e - 1] != '.' && !device(rel + a, e - a));

    predicate is_cookies{L}(char *rel, integer len) =
        len == 7 && rel[0] == 'c' && rel[1] == 'o' && rel[2] == 'o' && rel[3] == 'k' &&
        rel[4] == 'i' && rel[5] == 'e' && rel[6] == 's';

    // Under cache/http/: the network process's HTTP cache (net_cache.h).
    predicate is_http_cache{L}(char *rel, integer len) =
        len > 11 && rel[0] == 'c' && rel[1] == 'a' && rel[2] == 'c' && rel[3] == 'h' && rel[4] == 'e' &&
        rel[5] == '/' && rel[6] == 'h' && rel[7] == 't' && rel[8] == 't' && rel[9] == 'p' && rel[10] == '/';

    // a and b are the same NUL-terminated label.
    predicate same_label{L}(char *a, char *b) =
        \exists integer n; 0 <= n < SITE_MAX && a[n] == 0 && (\forall integer i; 0 <= i <= n ==> a[i] == b[i]);
 */

/* 1-8 components of [a-z0-9_.-]{1,64} separated by '/', none of them "."
 * or "..", none starting or ending with '.' (Windows drops trailing dots,
 * so "cookies." would name "cookies"), none a DOS device name ("nul",
 * "com1.txt"). */
/*@ requires \valid_read(rel + (0 .. len - 1));
    assigns \nothing;
    ensures \result ==> rel_ok(rel, len);
 */
bool store_rel_path_ok(const char *rel, uint32_t len);

/* "<site label>/<rel>" into out (cap bytes), '/' separated; false unless both
 * are well formed and it fits. */
/*@ requires \valid_read(site + (0 .. SITE_MAX - 1)) && \valid_read(rel + (0 .. rel_len - 1));
    requires \valid(out + (0 .. cap - 1));
    requires \separated(out + (0 .. cap - 1), site + (0 .. SITE_MAX - 1), rel + (0 .. rel_len - 1));
    assigns out[0 .. cap - 1];
    ensures \result ==> \old(label_ok(site) && rel_ok(rel, rel_len));
    ensures \result ==> \exists integer n; 1 <= n < SITE_MAX && site[n] == 0 &&
        n + 1 + rel_len < cap &&
        (\forall integer i; 0 <= i < n ==> out[i] == site[i]) && out[n] == '/' &&
        (\forall integer i; 0 <= i < rel_len ==> out[n + 1 + i] == rel[i]) && out[n + 1 + rel_len] == 0 &&
        (\forall integer i; 0 <= i < n + 1 + rel_len ==> out[i] != 0);
 */
bool store_join(const char *site, const char *rel, uint32_t rel_len, char *out, uint32_t cap);

/* May a client with badge `client_site` (NULL for the network process) act
 * on `target_site`'s `rel`?  A tab only on its own site, and never on its
 * "cookies" file or its HTTP cache (cache/http/...); the network process
 * only on those. */
/*@ requires client_site == \null || \valid_read(client_site + (0 .. SITE_MAX - 1));
    requires \valid_read(target_site + (0 .. SITE_MAX - 1)) && \valid_read(rel + (0 .. rel_len - 1));
    assigns \nothing;
    ensures \result ==> label_ok(target_site) && rel_ok(rel, rel_len);
    ensures \result && client_site != \null ==>
        same_label(client_site, target_site) && !is_cookies(rel, rel_len) && !is_http_cache(rel, rel_len);
    ensures \result && client_site == \null ==> is_cookies(rel, rel_len) || is_http_cache(rel, rel_len);
 */
bool store_allowed(const char *client_site, const char *target_site, const char *rel, uint32_t rel_len);

/* The bytes a site would use after replacing a file of old_size bytes with
 * one of new_size: false if that passes the quota. */
/*@ assigns \nothing;
    ensures \result ==> old_size <= used && used - old_size + new_size <= STORE_QUOTA_BYTES;
 */
bool store_quota_ok(uint64_t used, uint64_t old_size, uint64_t new_size);

/* ---- a client's requests (M9, SANDBOX_DESIGN.md section 8.7.3) ---------------- */

/* TS_READ, TS_WRITE and TS_DELETE (proto.h), as a client sends them:
 *   id, site_len, path_len, data_len     four little-endian uint32 (StoreRequest)
 *   site                                 site_len bytes, no NUL, under SITE_MAX:
 *                                        only the network process names one
 *   path                                 path_len bytes, at most STORE_PATH_MAX
 *   data                                 data_len bytes (TS_WRITE's content)
 * and nothing after. */
#define STORE_REQ_HEAD 16

enum {
    STORE_REQ_SHORT = 0,            /* not even a header: no answer */
    STORE_REQ_BAD,                  /* the lengths don't add up: refused (q->id) */
    STORE_REQ_REFUSED,              /* well formed, not allowed (or too long a path): refused */
    STORE_REQ_OK,                   /* allowed: path names the file */
};

typedef struct StoreReq {
    uint32_t id;
    char target[SITE_MAX];          /* the site acted on, NUL-terminated */
    char rel[STORE_PATH_MAX + 1];   /* the path in it, NUL-terminated */
    uint32_t rel_len;
    uint32_t data_off, data_len;    /* the data: msg[data_off .. data_off + data_len - 1] */
    char joined[SITE_MAX + STORE_PATH_MAX + 2];   /* store_join's "<target>/<rel>" */
} StoreReq;

/*@ logic integer le32{L}(uint8_t *p) = p[0] + p[1] * 0x100 + p[2] * 0x10000 + p[3] * 0x1000000;

    // The header's lengths add up to the message's, each in range.
    predicate req_ok{L}(uint8_t *m, integer len) =
        len >= STORE_REQ_HEAD && le32(m + 4) < SITE_MAX && le32(m + 8) <= STORE_PATH_MAX &&
        STORE_REQ_HEAD + le32(m + 4) + le32(m + 8) + le32(m + 12) == len;

    // path (UTF-16) is root, '\', the site label, '\', then rel with '/' as '\', NUL-terminated.
    predicate wpath{L}(uint16_t *path, integer cap, uint16_t *root, integer rn, char *site, char *rel, integer rl) =
        \exists integer n; 1 <= n < SITE_MAX && site[n] == 0 && rn + 2 + n + rl < cap &&
            (\forall integer i; 0 <= i < rn ==> path[i] == root[i]) && path[rn] == '\\' &&
            (\forall integer i; 0 <= i < n ==> path[rn + 1 + i] == site[i]) && path[rn + 1 + n] == '\\' &&
            (\forall integer i; 0 <= i < rl ==> path[rn + 2 + n + i] == (rel[i] == '/' ? '\\' : rel[i])) &&
            path[rn + 2 + n + rl] == 0;
 */

/* Decode a request (len bytes at m) from a client badged client_site (NULL
 * for the network process; "" for an opaque origin, which may do nothing),
 * decide it, and make the file's path: root (root_len UTF-16 units: the
 * service's browser_data\sites), '\', then store_join's "<site>/<path>"
 * with '\' for '/', into path (cap units).  The path is refused rather than
 * cut short if it doesn't fit.  Pure: the shim only opens what path names
 * (P4). */
/*@ requires \valid_read(m + (0 .. len - 1));
    requires client_site == \null || \valid_read(client_site + (0 .. SITE_MAX - 1));
    requires \valid_read(root + (0 .. root_len - 1));
    requires \valid(q) && \valid(path + (0 .. cap - 1));
    requires \separated(q, path + (0 .. cap - 1), m + (0 .. len - 1), root + (0 .. root_len - 1));
    requires client_site != \null ==> \separated(q, path + (0 .. cap - 1), client_site + (0 .. SITE_MAX - 1));
    requires \separated(path + (0 .. cap - 1), m + (0 .. len - 1), root + (0 .. root_len - 1));
    assigns *q, path[0 .. cap - 1];
    ensures STORE_REQ_SHORT <= \result <= STORE_REQ_OK;
    ensures \result == STORE_REQ_SHORT <==> len < STORE_REQ_HEAD;
    ensures \result != STORE_REQ_SHORT ==> q->id == le32(m);
    ensures \result >= STORE_REQ_REFUSED <==> req_ok(m, len);
    ensures \result >= STORE_REQ_REFUSED ==>
        q->rel_len == le32(m + 8) && q->data_len == le32(m + 12) &&
        q->data_off == STORE_REQ_HEAD + le32(m + 4) + le32(m + 8) && q->data_off + q->data_len == len &&
        q->target[SITE_MAX - 1] == 0 && q->rel[q->rel_len] == 0;
    // P4: what the shim opens is the file store_allowed and store_join approved.
    ensures \result == STORE_REQ_OK ==>
        label_ok(&q->target[0]) && rel_ok(&q->rel[0], q->rel_len) &&
        (\forall integer i; 0 <= i < q->rel_len ==> q->rel[i] == (char)m[STORE_REQ_HEAD + le32(m + 4) + i]) &&
        (client_site != \null ==> same_label(client_site, &q->target[0]) && !is_cookies(&q->rel[0], q->rel_len) &&
            !is_http_cache(&q->rel[0], q->rel_len)) &&
        (client_site == \null ==> (is_cookies(&q->rel[0], q->rel_len) || is_http_cache(&q->rel[0], q->rel_len)) &&
            \forall integer i; 0 <= i < le32(m + 4) ==> q->target[i] == (char)m[STORE_REQ_HEAD + i]) &&
        wpath(path, cap, root, root_len, &q->target[0], &q->rel[0], q->rel_len);
 */
int store_decode(const uint8_t *m, uint32_t len, const char *client_site, const uint16_t *root, uint32_t root_len,
                 StoreReq *q, uint16_t *path, uint32_t cap);

#endif
