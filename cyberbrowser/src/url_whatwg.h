/*
 * url_whatwg.h - URL parsing and serialization per the WHATWG URL Standard
 * (https://url.spec.whatwg.org/), used by the URL / URLSearchParams APIs
 * and by <a>/<area> elements.
 *
 * Strings are UTF-8.  All memory is malloc'd; free a record with
 * wurl_free().
 */
#ifndef URL_WHATWG_H
#define URL_WHATWG_H

#include <stddef.h>
#include <stdbool.h>

typedef struct WUrl {
    char *scheme;       /* lower case, without ':' */
    char *username;     /* percent-encoded, "" if none */
    char *password;     /* percent-encoded, "" if none */
    char *host;         /* serialized host; NULL = null host */
    int port;           /* -1 = null */
    char **path;        /* path segments (when !opaque_path) */
    int path_len;
    int path_cap;
    char *opaque_path;  /* non-NULL for URLs with an opaque path */
    char *query;        /* NULL = null */
    char *fragment;     /* NULL = null */
} WUrl;

/* URL setters (state overrides of the basic URL parser) */
typedef enum {
    WURL_SET_HREF,
    WURL_SET_PROTOCOL,
    WURL_SET_USERNAME,
    WURL_SET_PASSWORD,
    WURL_SET_HOST,
    WURL_SET_HOSTNAME,
    WURL_SET_PORT,
    WURL_SET_PATHNAME,
    WURL_SET_SEARCH,
    WURL_SET_HASH,
} WUrlSetter;

/* Parse 'input' (UTF-8) against 'base' (may be NULL).  Returns 0 and fills
 * *out, or -1 on failure (*out is then empty). */
int wurl_parse(const char *input, size_t len, const WUrl *base, WUrl *out);

/* Apply a setter to 'url' (the URL API setter steps).  Returns 0, or -1 if
 * the value was rejected (only WURL_SET_HREF can fail; other setters
 * silently ignore invalid input, as the standard says). */
int wurl_set(WUrl *url, WUrlSetter which, const char *value, size_t len);

/* Serializations (malloc'd, NUL-terminated). */
char *wurl_href(const WUrl *url);

/* Resolve `href` against the absolute URL `base` (NULL/"" = no base) and
 * return the serialized result (malloc'd), or NULL when it does not parse. */
char *wurl_resolve(const char *base, const char *href);
char *wurl_origin(const WUrl *url);
char *wurl_protocol(const WUrl *url);   /* "https:" */
char *wurl_host(const WUrl *url);       /* host[:port], "" for a null host */
char *wurl_hostname(const WUrl *url);
char *wurl_port(const WUrl *url);
char *wurl_pathname(const WUrl *url);
char *wurl_search(const WUrl *url);     /* "?q" or "" */
char *wurl_hash(const WUrl *url);       /* "#f" or "" */

void wurl_free(WUrl *url);
int wurl_copy(WUrl *dst, const WUrl *src);

bool wurl_is_special(const WUrl *url);

/* application/x-www-form-urlencoded */
/* Percent-encode 'len' bytes with the form-urlencoded set (space -> '+'). */
char *wurl_form_encode(const char *s, size_t len);
/* Percent-decode a form component ('+' -> space); *out_len gets the length
 * (the result may contain NULs). */
char *wurl_form_decode(const char *s, size_t len, size_t *out_len);

#endif /* URL_WHATWG_H */
