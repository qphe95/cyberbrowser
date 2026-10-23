/*
 * Structure index of a script (PARALLEL_PARSE_DESIGN.md, S1): every function
 * body the lazy parser could skip, with the facts the skip needs, found in
 * one scan of the whole script.  GC-free: built on any thread, from plain
 * bytes, and read by the parser on the JS thread.
 *
 * The scan makes, for each body, the decisions js_lazy_scan_body makes when
 * it scans that body alone from its '{': where the body ends, which names it
 * references (its whole subtree's, as the serial scan collects them), which
 * of those it certainly declares at its own function level, whether it uses
 * this / super / new, and whether the serial scan would refuse it (then the
 * body is parsed eagerly, as today).  Offsets are relative to the start of
 * the script.
 */
#ifndef JS_STRUCT_INDEX_H
#define JS_STRUCT_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JS_STRUCT_USES_THIS  (1u << 0)
#define JS_STRUCT_USES_SUPER (1u << 1)
#define JS_STRUCT_USES_NEW   (1u << 2)
#define JS_STRUCT_EAGER      (1u << 8)   /* the serial scan would refuse this body */

typedef struct JSStructName {
    uint32_t off, len;          /* identifier bytes in the script */
    uint64_t hash;
} JSStructName;

typedef struct JSStructRecord {
    uint32_t open;              /* offset of the body's '{' */
    uint32_t close;             /* offset just past the matching '}'; 0: not closed */
    uint32_t names, n_names;    /* distinct names referenced in the body (JSStructIndex.names) */
    uint32_t decls, n_decls;    /* names certainly declared at the body's level (.decls) */
    uint32_t flags;             /* JS_STRUCT_* */
} JSStructRecord;

typedef struct JSStructIndex {
    size_t src_len;
    JSStructRecord *recs;       /* sorted by open */
    uint32_t n_recs;
    JSStructName *names;
    uint32_t n_names;
    JSStructName *decls;
    uint32_t n_decls;
    uint32_t stop;              /* offset the scan stopped at (src_len: the whole script) */
    double scan_ms;
} JSStructIndex;

/* Scan `src` (any thread).  NULL when out of memory. */
JSStructIndex *js_struct_index_build(const uint8_t *src, size_t len);
void js_struct_index_free(JSStructIndex *ix);

/* The record of the body whose '{' is at `open`, or NULL (not a body the
 * scan found, not closed, or past where it stopped). */
const JSStructRecord *js_struct_index_find(const JSStructIndex *ix, uint32_t open);

/* Whether `r` certainly declares the name `n` (one of the index's names). */
bool js_struct_index_declares(const JSStructIndex *ix, const JSStructRecord *r,
                              const uint8_t *src, const JSStructName *n);

/* ---- Handing an index from the thread that scans to the JS thread ----
 * The scanning thread announces the buffer before it hands the buffer on
 * (so a claim can never miss a scan in flight), scans a copy, and publishes.
 * The JS thread claims the index for the exact buffer it evaluates (same
 * address and length), waiting for a scan in flight; a buffer changed
 * before evaluation (a different address or length) finds none. */
void *js_struct_index_announce(const char *data, size_t len);
void js_struct_index_publish(void *ticket, JSStructIndex *ix);
JSStructIndex *js_struct_index_claim(const char *data, size_t len);

/* Scan a fetched body on the calling thread when it looks like a script
 * worth indexing: announce, copy, `release` (the caller hands the buffer on
 * in there), scan, publish.  Otherwise just `release`. */
void js_struct_index_scan_fetched(const char *url, const char *data, size_t len,
                                  void (*release)(void *arg), void *arg);

typedef struct JSStructIndexStats {
    uint64_t built, claimed, waited, stopped_early;
    double scan_ms, wait_ms;
} JSStructIndexStats;
void js_struct_index_stats(JSStructIndexStats *out);

#ifdef __cplusplus
}
#endif

#endif /* JS_STRUCT_INDEX_H */
