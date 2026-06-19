#ifndef HTML_MEDIA_EXTRACT_H
#define HTML_MEDIA_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HtmlMediaCandidate {
    char url[2048];
    char mime[64];
    char title[256];
    char thumbnailUrl[2048];
} HtmlMediaCandidate;

bool html_extract_media_url(const char *html, HtmlMediaCandidate *outCandidate,
                            char *err, size_t errLen);

/* Extract inline JavaScript scripts from HTML (scripts without src attribute).
 * Returns number of scripts extracted (up to max_scripts).
 * Caller must free each out_scripts[i] with free().
 */
int html_extract_inline_scripts(const char *html, char **out_scripts, int max_scripts);

/* Extract visitorData from HTML by executing inline scripts in QuickJS.
 * This performs true browser emulation: scripts run and populate ytcfg,
 * then we read ytcfg.get('VISITOR_DATA') from the live JS objects.
 * Returns true if visitorData was successfully extracted.
 */
bool html_extract_visitor_data(const char *html, char *out_vd, size_t out_len);

#ifdef __cplusplus
}
#endif

#endif
