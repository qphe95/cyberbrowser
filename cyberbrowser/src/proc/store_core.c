/* store_core.c - the storage service's rules (store_core.h).  No library
 * calls, bounded loops. */
#include "store_core.h"

bool store_rel_path_ok(const char *rel, uint32_t len) {
    if (len == 0 || len > STORE_PATH_MAX) return false;
    uint32_t components = 0, clen = 0, dots = 0;
    for (uint32_t i = 0; i <= len; i++) {
        char c = i < len ? rel[i] : '/';
        if (c == '/') {
            if (clen == 0 || clen > 64 || dots == clen) return false;   /* empty, too long, "." or ".." */
            if (++components > 8) return false;
            clen = dots = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
        if (clen == 0 && c == '.') return false;                      /* hidden, "." and ".." */
        if (c == '.') dots++;
        clen++;
    }
    return true;
}

bool store_join(const char *site, const char *rel, uint32_t rel_len, char *out, uint32_t cap) {
    if (!site_label_ok(site) || !store_rel_path_ok(rel, rel_len)) return false;
    uint32_t o = 0;
    for (uint32_t i = 0; site[i] && i < SITE_MAX; i++) {
        if (o + 1 >= cap) return false;
        out[o++] = site[i];
    }
    if (o + 1 >= cap) return false;
    out[o++] = '/';
    for (uint32_t i = 0; i < rel_len; i++) {
        if (o + 1 >= cap) return false;
        out[o++] = rel[i];
    }
    out[o] = 0;
    return true;
}

static bool equal(const char *a, const char *b) {
    uint32_t i = 0;
    for (; i < SITE_MAX && a[i] && b[i]; i++)
        if (a[i] != b[i]) return false;
    return i < SITE_MAX && a[i] == b[i];
}

bool store_allowed(const char *client_site, const char *target_site, const char *rel, uint32_t rel_len) {
    if (!site_label_ok(target_site) || !store_rel_path_ok(rel, rel_len)) return false;
    if (client_site) return equal(client_site, target_site);
    /* The network process: a site's cookie jar, nothing else. */
    return rel_len == 7 && rel[0] == 'c' && rel[1] == 'o' && rel[2] == 'o' && rel[3] == 'k' &&
           rel[4] == 'i' && rel[5] == 'e' && rel[6] == 's';
}

bool store_quota_ok(uint64_t used, uint64_t old_size, uint64_t new_size) {
    if (old_size > used || new_size > STORE_QUOTA_BYTES) return false;
    return used - old_size <= STORE_QUOTA_BYTES - new_size;
}
