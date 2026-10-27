#ifndef SITE_H
#define SITE_H

/* Sites (SANDBOX_DESIGN.md section 3.1): the scheme and registrable domain
 * of a URL, as the label of the processes that load it and the name of its
 * folder in browser_data/sites/.  Pure C, no library calls, bounded loops:
 * part of what M4 proves.
 *
 * A label is "<scheme>+<registrable domain>", e.g. "https+youtube.com":
 * only [a-z0-9.+-], one '+', at most SITE_MAX - 1 bytes, so it is a safe
 * folder name and two sites never share one. */

#include <stdbool.h>
#include <stdint.h>

#define SITE_MAX 128
#define SITE_HOST_MAX 253

/* The host of an http(s) URL (len bytes), lowercased, into host[SITE_HOST_MAX + 1]:
 * after "scheme://", without userinfo or port.  False if the URL isn't
 * http(s), or the host is empty or has characters other than [a-z0-9.-]. */
bool site_url_host(const uint8_t *url, uint32_t len, char *scheme, uint32_t scheme_cap,
                   char *host, uint32_t host_cap);

/* The site label of scheme + host into out[SITE_MAX].  False if it can't
 * be made (the host is malformed). */
bool site_of(const char *scheme, const char *host, char *out);

/* Is `label` a well-formed site label. */
bool site_label_ok(const char *label);

#endif
