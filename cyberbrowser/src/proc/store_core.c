/* store_core.c - the storage service's rules (store_core.h).  No library
 * calls, bounded loops: proved with Frama-C/WP (tools/prove.sh). */
#include "store_core.h"

/* Is the component at c (n bytes) a DOS device name, which Windows opens
 * as the device whatever the folder and extension: con, prn, aux, nul,
 * com0-9, lpt0-9, alone or before a dot. */
/*@ requires \valid_read(c + (0 .. n - 1));
    assigns \nothing;
    ensures device(c, n) ==> \result;
 */
static bool device_name(const char *c, uint32_t n) {
    if (n >= 3 && (n == 3 || c[3] == '.') &&
        ((c[0] == 'c' && c[1] == 'o' && c[2] == 'n') || (c[0] == 'p' && c[1] == 'r' && c[2] == 'n') ||
         (c[0] == 'a' && c[1] == 'u' && c[2] == 'x') || (c[0] == 'n' && c[1] == 'u' && c[2] == 'l')))
        return true;
    if (n >= 4 && (n == 4 || c[4] == '.') && c[3] >= '0' && c[3] <= '9' &&
        ((c[0] == 'c' && c[1] == 'o' && c[2] == 'm') || (c[0] == 'l' && c[1] == 'p' && c[2] == 't')))
        return true;
    return false;
}

bool store_rel_path_ok(const char *rel, uint32_t len) {
    if (len == 0 || len > STORE_PATH_MAX) return false;
    uint32_t components = 0, start = 0;
    /*@ loop invariant 0 <= start <= i <= len + 1;
        loop invariant start == 0 || start == len + 1 || (start <= len && rel[start - 1] == '/');
        loop invariant i <= len ==> \forall integer k; start <= k < i ==> rel[k] != '/' && rel_char(rel[k]);
        loop invariant \forall integer k; 0 <= k < i && k < len ==> rel_char(rel[k]) || rel[k] == '/';
        loop invariant \forall integer a, e; component(rel, len, a, e) && e < start ==>
            a < e && rel[a] != '.' && rel[e - 1] != '.' && !device(rel + a, e - a);
        loop invariant i == len + 1 ==> start == len + 1;
        loop invariant 0 <= components <= 8;
        loop assigns i, start, components;
        loop variant len + 1 - i;
     */
    for (uint32_t i = 0; i <= len; i++) {
        if (i < len && rel[i] != '/') {
            char c = rel[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-'))
                return false;
            continue;
        }
        /* rel[start .. i - 1] is a component. */
        uint32_t clen = i - start;
        if (clen == 0 || clen > 64) return false;                   /* empty or too long */
        if (rel[start] == '.' || rel[i - 1] == '.') return false;   /* ".", "..", hidden; Windows drops trailing dots */
        if (device_name(rel + start, clen)) return false;
        if (++components > 8) return false;
        start = i + 1;
    }
    return true;
}

bool store_join(const char *site, const char *rel, uint32_t rel_len, char *out, uint32_t cap) {
    if (!site_label_ok(site) || !store_rel_path_ok(rel, rel_len)) return false;
    /*@ assert \forall integer k; 0 <= k < rel_len ==> rel[k] != 0; */
    uint32_t n = 0;                 /* the site's length */
    /*@ loop invariant 0 <= n <= SITE_MAX;
        loop invariant n > 0 ==> n < cap;
        loop invariant \forall integer k; 0 <= k < n ==> out[k] == site[k] && site[k] != 0;
        loop assigns n, out[0 .. cap - 1];
        loop variant SITE_MAX - n;
     */
    while (n < SITE_MAX && site[n]) {
        if (n + 1 >= cap) return false;
        out[n] = site[n];
        n++;
    }
    /*@ assert n < SITE_MAX && site[n] == 0; */
    /*@ assert label_char(site[0]) && site[0] != 0; */
    /*@ assert n >= 1; */
    if (n + 1 >= cap) return false;
    out[n] = '/';
    /*@ loop invariant 0 <= i <= rel_len && n + 1 + i < cap + 1;
        loop invariant out[n] == '/';
        loop invariant \forall integer k; 0 <= k < n ==> out[k] == site[k] && out[k] != 0;
        loop invariant \forall integer k; 0 <= k < i ==> out[n + 1 + k] == rel[k] && out[n + 1 + k] != 0;
        loop assigns i, out[0 .. cap - 1];
        loop variant rel_len - i;
     */
    for (uint32_t i = 0; i < rel_len; i++) {
        if (n + 1 + i + 1 >= cap) return false;
        out[n + 1 + i] = rel[i];
    }
    if (n + 1 + rel_len >= cap) return false;
    out[n + 1 + rel_len] = 0;
    /*@ assert 1 <= n < SITE_MAX && site[n] == 0 && n + 1 + rel_len < cap &&
            (\forall integer i; 0 <= i < n ==> out[i] == site[i]) && out[n] == '/' &&
            (\forall integer i; 0 <= i < rel_len ==> out[n + 1 + i] == rel[i]) && out[n + 1 + rel_len] == 0; */
    /*@ assert \forall integer i; 0 <= i < rel_len ==> out[n + 1 + i] != 0; */
    /*@ assert \forall integer i; 0 <= i < n + 1 + rel_len ==> out[i] != 0; */
    return true;
}

/*@ requires \valid_read(a + (0 .. SITE_MAX - 1)) && \valid_read(b + (0 .. SITE_MAX - 1));
    assigns \nothing;
    ensures \result ==> same_label(a, b);
 */
static bool equal(const char *a, const char *b) {
    uint32_t i = 0;
    /*@ loop invariant 0 <= i <= SITE_MAX;
        loop invariant \forall integer k; 0 <= k < i ==> a[k] == b[k] && a[k] != 0;
        loop assigns i;
        loop variant SITE_MAX - i;
     */
    for (; i < SITE_MAX && a[i] && b[i]; i++)
        if (a[i] != b[i]) return false;
    return i < SITE_MAX && a[i] == b[i];
}

bool store_allowed(const char *client_site, const char *target_site, const char *rel, uint32_t rel_len) {
    if (!site_label_ok(target_site) || !store_rel_path_ok(rel, rel_len)) return false;
    bool cookies = rel_len == 7 && rel[0] == 'c' && rel[1] == 'o' && rel[2] == 'o' && rel[3] == 'k' &&
                   rel[4] == 'i' && rel[5] == 'e' && rel[6] == 's';
    /* A tab: its own site's folder, but not the cookie jar, which holds
     * the HttpOnly cookies it mustn't see. */
    if (client_site) return equal(client_site, target_site) && !cookies;
    /* The network process: a site's cookie jar, nothing else. */
    return cookies;
}

bool store_quota_ok(uint64_t used, uint64_t old_size, uint64_t new_size) {
    if (old_size > used || new_size > STORE_QUOTA_BYTES) return false;
    return used - old_size <= STORE_QUOTA_BYTES - new_size;
}

/* ---- a client's requests ----------------------------------------------------------- */

/*@ requires \valid_read(p + (0 .. 3));
    assigns \nothing;
    ensures \result == le32(p);
 */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] + (uint32_t)p[1] * 0x100u + (uint32_t)p[2] * 0x10000u + (uint32_t)p[3] * 0x1000000u;
}

int store_decode(const uint8_t *m, uint32_t len, const char *client_site, const uint16_t *root, uint32_t root_len,
                 StoreReq *q, uint16_t *path, uint32_t cap) {
    if (len < STORE_REQ_HEAD) return STORE_REQ_SHORT;
    q->id = rd32(m);
    uint32_t site_len = rd32(m + 4), path_len = rd32(m + 8), data_len = rd32(m + 12);
    if ((uint64_t)STORE_REQ_HEAD + site_len + path_len + data_len != len || site_len >= SITE_MAX ||
        path_len > STORE_PATH_MAX)
        return STORE_REQ_BAD;

    /* The site: the one named (the network process's word), else the client's. */
    bool nul = false;
    /*@ loop invariant 0 <= i <= site_len;
        loop invariant \forall integer k; 0 <= k < i ==> q->target[k] == (char)m[STORE_REQ_HEAD + k];
        loop assigns i, nul, q->target[0 .. SITE_MAX - 1];
        loop variant site_len - i;
     */
    for (uint32_t i = 0; i < site_len; i++) {
        q->target[i] = (char)m[STORE_REQ_HEAD + i];
        if (!m[STORE_REQ_HEAD + i]) nul = true;
    }
    uint32_t tn = site_len;
    if (site_len == 0 && client_site) {
        /*@ loop invariant 0 <= i <= SITE_MAX - 1;
            loop assigns i, q->target[0 .. SITE_MAX - 1];
            loop variant SITE_MAX - 1 - i;
         */
        for (uint32_t i = 0; i < SITE_MAX - 1; i++) q->target[i] = client_site[i];
        tn = SITE_MAX - 1;
    }
    q->target[tn] = 0;
    q->target[SITE_MAX - 1] = 0;

    /* The path, and where the data starts. */
    /*@ loop invariant 0 <= i <= path_len;
        loop invariant \forall integer k; 0 <= k < i ==> q->rel[k] == (char)m[STORE_REQ_HEAD + site_len + k];
        loop assigns i, q->rel[0 .. STORE_PATH_MAX];
        loop variant path_len - i;
     */
    for (uint32_t i = 0; i < path_len; i++) q->rel[i] = (char)m[STORE_REQ_HEAD + site_len + i];
    q->rel[path_len] = 0;
    q->rel_len = path_len;
    q->data_off = STORE_REQ_HEAD + site_len + path_len;
    q->data_len = data_len;

    if (nul || (client_site && !client_site[0])) return STORE_REQ_REFUSED;
    if (!store_allowed(client_site, q->target, q->rel, path_len) ||
        !store_join(q->target, q->rel, path_len, q->joined, (uint32_t)sizeof(q->joined)))
        return STORE_REQ_REFUSED;

    /* joined is "<target>/<rel>": the site's length, as store_join found it. */
    uint32_t n = 0;
    /*@ loop invariant 0 <= n <= SITE_MAX - 1;
        loop invariant \forall integer k; 0 <= k < n ==> q->target[k] != 0;
        loop assigns n;
        loop variant SITE_MAX - 1 - n;
     */
    while (n < SITE_MAX - 1 && q->target[n]) n++;
    /*@ assert q->target[n] == 0; */
    /*@ assert \forall integer k; 0 <= k < n ==> label_char(q->target[k]); */
    /*@ assert 1 <= n && n + 1 + path_len < SITE_MAX + STORE_PATH_MAX + 2; */
    /*@ assert \forall integer k; 0 <= k < n ==> q->joined[k] == q->target[k]; */
    /*@ assert q->joined[n] == '/'; */
    /*@ assert \forall integer k; 0 <= k < path_len ==> q->joined[n + 1 + k] == q->rel[k]; */
    /*@ assert \forall integer k; 0 <= k < n ==> 0 < q->target[k] && q->target[k] != '/'; */
    /*@ assert \forall integer k; 0 <= k < path_len ==> 0 < q->rel[k]; */

    /* root '\' joined, '/' as '\': refused rather than cut short. */
    uint32_t jn = n + 1 + path_len;
    if ((uint64_t)root_len + 1 + jn >= cap) return STORE_REQ_REFUSED;
    /*@ loop invariant 0 <= i <= root_len;
        loop invariant \forall integer k; 0 <= k < i ==> path[k] == root[k];
        loop assigns i, path[0 .. cap - 1];
        loop variant root_len - i;
     */
    for (uint32_t i = 0; i < root_len; i++) path[i] = root[i];
    path[root_len] = '\\';
    /*@ loop invariant 0 <= j <= jn;
        loop invariant \forall integer k; 0 <= k < root_len ==> path[k] == root[k];
        loop invariant path[root_len] == '\\';
        loop invariant \forall integer k; 0 <= k < j ==>
            path[root_len + 1 + k] == (q->joined[k] == '/' ? (uint16_t)'\\' : (uint16_t)q->joined[k]);
        loop assigns j, path[0 .. cap - 1];
        loop variant jn - j;
     */
    for (uint32_t j = 0; j < jn; j++) {
        char ch = q->joined[j];
        path[root_len + 1 + j] = ch == '/' ? (uint16_t)'\\' : (uint16_t)ch;
    }
    path[root_len + 1 + jn] = 0;
    /*@ assert \forall integer k; 0 <= k < n ==> path[root_len + 1 + k] == q->target[k]; */
    /*@ assert path[root_len + 1 + n] == '\\'; */
    /*@ assert \forall integer k; 0 <= k < path_len ==>
            path[root_len + 2 + n + k] == (q->rel[k] == '/' ? '\\' : q->rel[k]); */
    /*@ assert path[root_len + 2 + n + path_len] == 0 && root_len + 2 + n + path_len < cap; */
    /*@ assert label_ok(&q->target[0]) && rel_ok(&q->rel[0], q->rel_len); */
    /*@ assert wpath(path, cap, root, root_len, &q->target[0], &q->rel[0], q->rel_len); */
    return STORE_REQ_OK;
}
