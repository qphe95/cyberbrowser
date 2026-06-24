/*
 * URL utility helpers used across the browser-emulator module.
 */

#ifndef URL_UTILS_H
#define URL_UTILS_H

#include <stdbool.h>
#include <ctype.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Return true if |url| begins with a valid URL scheme followed by ':'.
 * This detects both hierarchical schemes (http:, https:, ftp:, file:) and
 * non-hierarchical schemes (data:, blob:, javascript:, about:, mailto:).
 * A single-letter scheme followed by a drive-letter path (e.g. "C:\foo")
 * is intentionally rejected so Windows paths are not mistaken for URLs.
 */
static inline bool url_has_scheme(const char *url)
{
    if (!url || !url[0]) return false;

    /* Scheme must start with a letter. */
    if (!isalpha((unsigned char)url[0])) return false;

    const char *p = url + 1;
    while (*p && (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')) {
        p++;
    }

    if (*p != ':') return false;

    /* Reject single-letter schemes followed by '\' or '/' (Windows drive letters). */
    if (p == url + 1 && (p[1] == '\\' || p[1] == '/')) return false;

    return true;
}

/*
 * Return true if |url| is a data: URL.
 */
static inline bool url_is_data_url(const char *url)
{
    return url && strncasecmp(url, "data:", 5) == 0;
}

/*
 * Return true if |url| uses a network scheme (http or https).
 */
static inline bool url_is_network_url(const char *url)
{
    return url && (strncasecmp(url, "http://", 7) == 0 ||
                   strncasecmp(url, "https://", 8) == 0);
}

#ifdef __cplusplus
}
#endif

#endif /* URL_UTILS_H */
