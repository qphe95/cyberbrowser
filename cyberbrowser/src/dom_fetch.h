/*
 * Script fetch pool (PARALLEL_DOM.md §8).
 *
 * External scripts found while the page is parsed are handed to a small pool
 * of fetch threads (separate from the job system: fetches block on sockets),
 * so their downloads overlap.  Scripts still execute in document order: the
 * executor asks for script i with dom_fetch_script(), which waits for that
 * fetch (or fetches synchronously when the URL was never prefetched).
 * Completion is tracked with counters.
 */
#ifndef DOM_FETCH_H
#define DOM_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "http_download.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Queue a fetch of `url` (no-op when it is already queued or fetched).
 * CYBER_FETCH_THREADS sets the pool size (default 6; 0 disables prefetch). */
void dom_fetch_prefetch(const char *url);
/* The same, ahead of everything queued (stylesheets and fonts, which the
 * first layout waits for). */
void dom_fetch_prefetch_first(const char *url);

/* The body of `url`: the prefetched result (waiting for it when it is still
 * in flight) or a synchronous fetch.  The caller owns `out` (free with
 * http_free_buffer).  A prefetched result is handed out once. */
bool dom_fetch_script(const char *url, HttpBuffer *out, char *err, size_t err_len);

/* Has a queued fetch of `url` finished (dom_fetch_script would not wait)? */
bool dom_fetch_ready(const char *url);
/* Wait until every prefetch has finished downloading (at most timeout_ms);
 * false on timeout.  For CYBER_DET_TIME (det_clock.h). */
bool dom_fetch_wait_all(int timeout_ms);

typedef struct DomFetchStats {
    uint64_t queued, completed, hits, waits, sync_fetches;
    double wait_ms;            /* time the executor spent waiting */
} DomFetchStats;
void dom_fetch_stats(DomFetchStats *out);

/* Fetch function used by the pool (tests replace it). */
typedef bool (*DomFetchFn)(const char *url, HttpBuffer *out, char *err, size_t err_len);
void dom_fetch_set_fetcher(DomFetchFn fn);

/* Wait for every queued fetch and drop unclaimed results. */
void dom_fetch_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DOM_FETCH_H */
