#ifndef NET_POLICY_H
#define NET_POLICY_H

/* The network process's decisions on what a page may reach and read
 * (SANDBOX_DESIGN.md section 2.4): Private Network Access and Opaque
 * Response Blocking.  Pure C, no library calls, bounded loops, proved with
 * Frama-C/WP (tools/prove.sh): free of undefined behavior for any bytes
 * (P1), bounded (P7), and an address the client may not reach is never
 * allowed (P9). */

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

#endif
