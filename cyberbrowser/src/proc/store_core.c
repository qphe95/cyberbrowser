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
        loop invariant \forall integer k; 0 <= k < n ==> out[k] == site[k];
        loop invariant \forall integer k; 0 <= k < i ==> out[n + 1 + k] == rel[k];
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
