#ifndef CSS_CHARSET_H
#define CSS_CHARSET_H

#include <stdbool.h>
#include <stddef.h>

/* A fetched stylesheet's bytes decoded to UTF-8 (malloc'd, NUL-terminated,
 * *out_len bytes): the encoding is a BOM's, else the Content-Type charset in
 * the raw response headers, else an @charset rule's, else fallback_label's
 * (the referring document's; NULL: UTF-8).  UTF-8, UTF-16, windows-1252
 * (with its aliases ISO-8859-1 and US-ASCII) and Shift_JIS are decoded;
 * other encodings pass through as UTF-8.  *enc_out: the encoding's name
 * (NULL: UTF-8 by default).  NULL when out of memory. */
char *css_sheet_decode(const char *bytes, size_t n, const char *headers, size_t headers_size,
                       const char *fallback_label, size_t *out_len, const char **enc_out);

#endif
