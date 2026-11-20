#ifndef CSS_CHARSET_H
#define CSS_CHARSET_H

#include <stdbool.h>
#include <stddef.h>

/* A fetched stylesheet's bytes decoded to UTF-8 (malloc'd, NUL-terminated,
 * *out_len bytes): the encoding is a BOM's, else the Content-Type charset in
 * the raw response headers, else an @charset rule's, else fallback_label's
 * (the referring document's; NULL: UTF-8).  UTF-8, UTF-16, the WHATWG
 * single-byte encodings (windows-125x, ISO-8859-x, KOI8, ...), Shift_JIS and
 * Big5 are decoded; other encodings pass through as UTF-8.  *enc_out: the
 * encoding's name (NULL: UTF-8 by default).  NULL when out of memory. */
char *css_sheet_decode(const char *bytes, size_t n, const char *headers, size_t headers_size,
                       const char *fallback_label, size_t *out_len, const char **enc_out);

/* Whether a fetched stylesheet's response may be one: no Content-Type in
 * the raw headers, or text/css (a sheet served as text/plain, say, is not
 * applied, CSS 2.1 section 4.4 and HTML's fetch of style sheets). */
bool css_sheet_type_ok(const char *headers, size_t headers_size);

/* An HTML document's encoding (HTML 13.2.3): a BOM's (*bom its length), the transport's charset label's, a meta's in the
 * first 1024 bytes, a bogo-XML declaration's, the inherited one, else UTF-8. */
const char *html_sniff_encoding(const char *bytes, size_t n, const char *header_label, const char *inherit, size_t *bom,
                                bool final, bool *more);
/* A URL string with its query's non-ASCII in the single-byte encoding enc (malloc'd); NULL when nothing changes. */
char *url_legacy_query(const char *in, const char *enc);
/* Whether charset_decode_chunk decodes enc (UTF-8 passes as it is). */
bool charset_decodable(const char *enc);
/* A chunk in enc as UTF-8 (malloc'd, *out_len bytes); *keep bytes at the end held back unless last. */
char *charset_decode_chunk(const char *enc, const char *bytes, size_t n, bool last, size_t *out_len, size_t *keep);

#endif
