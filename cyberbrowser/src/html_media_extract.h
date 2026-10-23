#ifndef HTML_MEDIA_EXTRACT_H
#define HTML_MEDIA_EXTRACT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration of JsExecResult (defined in js_quickjs.h) */
struct JsExecResult;

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

/* visitorData extraction is disabled. Kept as a no-op stub for compatibility. */
bool html_extract_visitor_data(const char *html, char *out_vd, size_t out_len);

/* Execute all page scripts (inline + external) in document order.
 * Fetches external scripts, runs them through QuickJS, and pumps timers/microtasks
 * after each network response. Captured URLs are returned in out_result.
 */
bool html_execute_page_scripts(const char *html, struct JsExecResult *out_result);

/* Preload scan of a document while it downloads: queues the fetches of its
 * stylesheets and external scripts, and of the scripts and stylesheets it
 * preloads, in document order (as a browser's preload scanner does), so they
 * overlap the rest of the download.  Feed the decoded body in pieces. */
typedef struct HtmlPreloadScan HtmlPreloadScan;
HtmlPreloadScan *html_preload_scan_begin(void);
void html_preload_scan_feed(HtmlPreloadScan *s, const char *data, size_t len);
void html_preload_scan_end(HtmlPreloadScan *s);



/* ytInitialPlayerResponse media extraction is disabled. Kept as a no-op stub. */
bool html_extract_yt_player_response_media(const char *html, bool prefer_video,
                                           char *out_url, size_t out_url_len,
                                           char *out_mime, size_t out_mime_len,
                                           char *out_title, size_t out_title_len,
                                           char *out_thumbnail, size_t out_thumbnail_len);

#ifdef __cplusplus
}
#endif

#endif
