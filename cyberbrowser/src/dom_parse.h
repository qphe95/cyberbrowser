/*
 * HTML tokenizer and tree builder over the node store (PARALLEL_DOM.md §5).
 *
 *   Tokens   compact records {kind, tag atom, attribute span, text span,
 *            source offsets} in a chunk's arena.  Tag and attribute names are
 *            interned while tokenizing; entities are decoded and whitespace is
 *            collapsed into the arena.  No malloc per token, no fixed buffers.
 *   Chunks   large inputs are cut at '<' boundaries outside raw-text elements
 *            and comments; chunks are tokenized in parallel jobs, each assuming
 *            the data state at its start, and a continuation validates the
 *            chain (re-tokenizing a chunk from its true start when the previous
 *            chunk did not end exactly there).  The token stream is therefore
 *            always the serial tokenizer's.
 *   Builder  one serial pass over the tokens builds the tree into the store,
 *            assigning slots in document order.  It reproduces the legacy
 *            recursive parser exactly (no implied end tags, prefix-matched end
 *            tags, stray end tags dropped, comments dropped, nesting capped at
 *            256), except that names and values have no length limits.
 *   Post     per-node work over the new nodes as range jobs (§5.4).
 */
#ifndef DOM_PARSE_H
#define DOM_PARSE_H

#include "dom_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode character references in place on a NUL-terminated buffer of `len`
 * bytes (numeric and common named ones, to UTF-8).  Returns the new length. */
uint32_t dom_parse_decode_entities(char *str, size_t len);

/* Parse `len` bytes of HTML into a new detached fragment (owner `owner_doc`)
 * whose children are the top-level nodes.  Returns the fragment. */
NodeId dom_parse_html(const char *html, size_t len, NodeId owner_doc);

/* The same for a fragment (innerHTML, outerHTML, insertAdjacentHTML): text
 * outside any element is kept ("a<b>x</b>b" has three top-level nodes),
 * where document parsing drops it like the legacy parser. */
NodeId dom_parse_html_fragment(const char *html, size_t len, NodeId owner_doc);

/* Parse into an existing node: the top-level nodes become `parent`'s last
 * children.  For innerHTML-style fragments the whole input is treated like a
 * document (the legacy behaviour): see dom_parse_fragment_children(). */
void dom_parse_html_into(const char *html, size_t len, NodeId parent, NodeId owner_doc);

/* The node an innerHTML-style insertion should take children from: the
 * parsed <body>'s children when the input has a body, else the top level. */
NodeId dom_parse_fragment_source(NodeId fragment);

/* Streaming: build a document into `parent` (a live document) as its source
 * arrives, stopping after each <script> element so the caller can run it
 * before the rest is parsed (HTML's parser-blocking scripts).  The tree is the
 * one dom_parse_html builds from the whole input, except that a template's
 * children go straight into its content.
 *   append  add arrived bytes (the source is kept whole)
 *   pump    build what has arrived: returns the <script> element just
 *           completed (call again after running it), or DOM_NULL when it
 *           needs more input; `final` when the input is complete
 *   new_roots  the nodes (slots) this pump attached to nodes from before it, in
 *           document order (what a connected-callback walk starts from) */
typedef struct DomStream DomStream;
DomStream *dom_stream_begin(NodeId parent, NodeId owner_doc);
void dom_stream_append(DomStream *st, const char *data, size_t len);
NodeId dom_stream_pump(DomStream *st, bool final);
const uint32_t *dom_stream_new_roots(DomStream *st, uint32_t *n);
const char *dom_stream_source(DomStream *st, size_t *len);
void dom_stream_end(DomStream *st);
/* document.write: `insert` puts input at the insertion point (right after the
 * script pump last returned, then after what was inserted there) and moves
 * the point past it.  A pump with a limit builds no further than it (the
 * written input, before the rest of the page). */
void dom_stream_insert(DomStream *st, const char *data, size_t len);
size_t dom_stream_insertion_point(DomStream *st);
void dom_stream_set_insertion_point(DomStream *st, size_t pos);
void dom_stream_set_limit(DomStream *st, size_t limit);

/* Head / body / root of a parsed fragment, as html_parse() reports them. */
typedef struct DomParseRoots { NodeId root, head, body; } DomParseRoots;
DomParseRoots dom_parse_roots(NodeId fragment);

/* Statistics and switches.
 *   DOM_THREADS / CSS_THREADS  thread count (shared job system)
 *   DOM_CHUNK=<bytes>          chunk size (default 1 MB); inputs smaller
 *                              than two chunks are tokenized serially
 *   DOM_CHUNK_FORCE=1          chunk at exactly DOM_CHUNK bytes, ignoring
 *                              the prescan (tests: forces bad boundaries)
 *   DOM_LOG=1                  per-parse timing on stderr */
typedef struct DomParseStats {
    uint32_t chunks;
    uint32_t chunks_redone;       /* failed validation, re-tokenized */
    uint32_t tokens;
    uint32_t nodes;
    uint32_t detours;             /* builder re-tokenized at the top level */
    double prescan_ms, tokenize_ms, build_ms, post_ms;
} DomParseStats;
void dom_parse_last_stats(DomParseStats *out);
/* Override DOM_CHUNK / DOM_CHUNK_FORCE (tests): chunk 0 and force -1 restore
 * the environment's settings. */
void dom_parse_set_chunking(size_t chunk, int force);

#ifdef __cplusplus
}
#endif

#endif /* DOM_PARSE_H */
