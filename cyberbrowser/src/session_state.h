#ifndef SESSION_STATE_H
#define SESSION_STATE_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Global session tokens extracted from the first YouTube homepage/watch-page
 * response and injected into every youtubei request (C and JS paths). */
extern char g_visitor_data[256];

/* Set the visitor data token (visitorData / VISITOR_INFO1_LIVE value). */
void session_set_visitor_data(const char *value);

/* Extract a cookie value by name from a semicolon-separated cookie string.
 * Writes the value (without the name) into out and returns true if found. */
bool session_extract_cookie_value(const char *cookies, const char *name,
                                  char *out, size_t out_len);

/* Scan HTML/text for a JSON string field such as "visitorData":"...".
 * Writes the captured value into out and returns true if found. */
bool session_extract_json_field(const char *text, const char *field_name,
                                char *out, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_STATE_H */
