#ifndef NET_CACHE_H
#define NET_CACHE_H

/* net_cache.h - the network process's HTTP cache (RFC 9111, a private
 * cache's subset).  Entries live in the folder of the site whose tab asked
 * (cache/http/<key>), through the storage service, so one site's tabs never
 * see what another's fetched (a cache partitioned by the top-level site).
 *
 * Kept: GET responses with status 200, not redirected, without Set-Cookie,
 * no-store, Vary: * or Vary: Cookie, of at most NET_CACHE_ENTRY_MAX,
 * either fresh for a while (max-age, Expires, or a tenth of the time since
 * Last-Modified, up to a day) or able to be revalidated (ETag,
 * Last-Modified).  A stale entry is asked for again with If-None-Match /
 * If-Modified-Since; a 304 makes it fresh again.  An entry keeps the
 * request's values of the headers its Vary names, and serves only requests
 * with the same (the headers this client sends alike on every request
 * match always). */

#include <stdbool.h>
#include <stdint.h>
#include "http_download.h"

#define NET_CACHE_ENTRY_MAX (16u << 20)

typedef struct NetCacheIo {
    /* site's file at path into *data (malloc'd); false if none. */
    bool (*read)(const char *site, const char *path, uint8_t **data, uint32_t *len);
    void (*write)(const char *site, const char *path, const void *data, uint32_t len);
} NetCacheIo;
void net_cache_init(const NetCacheIo *io);

typedef struct NetCached {
    bool found, fresh;
    int status;
    HttpBuffer b;                       /* its headers and body (malloc'd) */
    char etag[256], last_modified[64];  /* its validators ("" if none) */
    char vary[1024];                    /* the request's side of its Vary */
} NetCached;
void net_cached_free(NetCached *c);

/* Whether a request may use the cache at all (a GET, no Range or
 * Authorization, no Cache-Control: no-store / no-cache). */
bool net_cache_request_ok(const char *method, const char *const *headers, uint32_t nheaders);

/* url's entry in site's cache for a request with these headers
 * (out->found), fresh or to revalidate. */
bool net_cache_lookup(const char *site, const char *url, const char *const *req, uint32_t nreq, NetCached *out);

/* Keep a response to url (status, headers and body), to a request with
 * these headers, if it may be kept. */
void net_cache_store(const char *site, const char *url, const char *const *req, uint32_t nreq, int status,
                     const HttpBuffer *b);

/* A 304 for url, whose entry is old: the entry, fresh again by the 304's
 * headers (rewritten); false if it can't be made so. */
bool net_cache_revalidated(const char *site, const char *url, const HttpBuffer *b304, NetCached *old);

#endif
