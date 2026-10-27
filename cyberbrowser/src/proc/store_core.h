#ifndef STORE_CORE_H
#define STORE_CORE_H

/* The storage service's rules (SANDBOX_DESIGN.md section 3.3), pure C: the
 * grammar of paths a client may name, and quotas.  The shim (store_main.c)
 * opens files only through store_join, after these checks.  Part of what M4
 * proves (P4). */

#include <stdbool.h>
#include <stdint.h>
#include "site.h"

#define STORE_PATH_MAX 600              /* a relative path */
#define STORE_QUOTA_BYTES (64ull << 20) /* per site */

/* 1-8 components of [a-z0-9_.-]{1,64} separated by '/', none of them "."
 * or "..", none starting or ending with '.' (Windows drops trailing dots,
 * so "cookies." would name "cookies"), none a DOS device name ("nul",
 * "com1.txt"). */
bool store_rel_path_ok(const char *rel, uint32_t len);

/* "<site label>/<rel>" into out (cap bytes), '/' separated; false unless both
 * are well formed and it fits. */
bool store_join(const char *site, const char *rel, uint32_t rel_len, char *out, uint32_t cap);

/* May a client with badge `client_site` (NULL for the network process) act
 * on `target_site`'s `rel`?  A tab only on its own site, and never on its
 * "cookies" file; the network process only on a site's "cookies" file. */
bool store_allowed(const char *client_site, const char *target_site, const char *rel, uint32_t rel_len);

/* The bytes a site would use after replacing a file of old_size bytes with
 * one of new_size: false if that passes the quota. */
bool store_quota_ok(uint64_t used, uint64_t old_size, uint64_t new_size);

#endif
