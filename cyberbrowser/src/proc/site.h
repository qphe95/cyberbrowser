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

/*@ predicate host_char(char c) = ('a' <= c <= 'z') || ('0' <= c <= '9') || c == '-' || c == '.';
    predicate label_char(char c) = host_char(c) || c == '+';

    // A host as site_url_host makes it: [a-z0-9.-], not starting with '.' or
    // '-', not ending with '.', no empty label.
    predicate host_ok{L}(char *h, integer n) =
        1 <= n <= SITE_HOST_MAX &&
        (\forall integer i; 0 <= i < n ==> host_char(h[i])) &&
        h[0] != '.' && h[0] != '-' && h[n - 1] != '.' &&
        (\forall integer i; 0 <= i < n - 1 ==> !(h[i] == '.' && h[i + 1] == '.'));

    // A site label: [a-z0-9.+-], exactly one '+', neither first nor last,
    // not ending with '.', NUL-terminated within SITE_MAX bytes.  So it's a
    // folder name of its own: no '/', no '\', no ':', not "." or "..".
    predicate label_ok{L}(char *s) =
        \exists integer n; 1 <= n < SITE_MAX && s[n] == 0 &&
            (\forall integer i; 0 <= i < n ==> label_char(s[i])) &&
            s[n - 1] != '.' &&
            (\exists integer p; 0 < p < n - 1 && s[p] == '+' &&
                (\forall integer i; 0 <= i < n && i != p ==> s[i] != '+'));
 */

/* The host of an http(s) URL (len bytes), lowercased, into host[SITE_HOST_MAX + 1]:
 * after "scheme://", without userinfo or port.  False if the URL isn't
 * http(s), or the host is empty or has characters other than [a-z0-9.-]. */
/*@ requires \valid_read(url + (0 .. len - 1));
    requires \valid(scheme + (0 .. scheme_cap - 1)) && \valid(host + (0 .. host_cap - 1));
    requires \separated(scheme + (0 .. scheme_cap - 1), host + (0 .. host_cap - 1), url + (0 .. len - 1));
    assigns scheme[0 .. scheme_cap - 1], host[0 .. host_cap - 1];
    ensures \result ==> \exists integer n; n < host_cap && host[n] == 0 && host_ok(host, n);
 */
bool site_url_host(const uint8_t *url, uint32_t len, char *scheme, uint32_t scheme_cap,
                   char *host, uint32_t host_cap);

/* The site label of scheme + host into out[SITE_MAX].  False if it can't
 * be made (the host is malformed). */
/*@ requires \valid_read(scheme + (0 .. 7)) && \valid_read(host + (0 .. SITE_HOST_MAX));
    requires \valid(out + (0 .. SITE_MAX - 1));
    requires \separated(out + (0 .. SITE_MAX - 1), scheme + (0 .. 7), host + (0 .. SITE_HOST_MAX));
    assigns out[0 .. SITE_MAX - 1];
    ensures \result ==> label_ok(out);
 */
bool site_of(const char *scheme, const char *host, char *out);

/* Is `label` a well-formed site label. */
/*@ requires \valid_read(label + (0 .. SITE_MAX - 1));
    assigns \nothing;
    ensures \result ==> label_ok(label);
 */
bool site_label_ok(const char *label);

#endif
