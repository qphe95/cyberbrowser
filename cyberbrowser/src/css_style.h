/*
 * Compiled style matching (PARALLEL_STYLE_LAYOUT.md §4.1, §4.4, §7.2).
 *
 *   CssAtom      interned, lower-cased names (tags, ids, classes, attributes).
 *   CssStyleSet  the document's stylesheets compiled for one viewport:
 *                selectors as atom compounds, filed in buckets by the key of
 *                their rightmost compound, with ancestor Bloom hashes; media
 *                conditions evaluated once.  Immutable once built.
 *   CssStyleDoc  per-document element features (tag/id/class/attribute atoms,
 *                parent, sibling positions) in flat arrays indexed by DOM
 *                index.  Immutable once built.
 *   CssStyleScratch  per-thread matching scratch (stamps, Bloom filter,
 *                output arrays).  Matching reads only the two immutable
 *                tables, so any number of threads may match concurrently,
 *                each with its own scratch.
 *
 * The matcher reproduces the legacy string matcher (css_rule_matches /
 * css_selector_matches) exactly, including its quirks: greedy descendant
 * matching, sibling combinators that never match, and selector-text
 * truncation.  Only the work is different.
 */
#ifndef CSS_STYLE_H
#define CSS_STYLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "css_parser.h"
#include "html_dom.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t CssAtom;
#define CSS_ATOM_NONE  0u
#define CSS_ATOM_EMPTY 1u   /* the empty string */

/* Intern `len` bytes of `s`, ASCII-lower-cased.  Thread-safe (lock-free
 * insert).  Capacity is reserved up front with css_atom_reserve(). */
CssAtom css_atom_intern(const char *s, size_t len);
/* Look up without inserting; CSS_ATOM_NONE when absent. */
CssAtom css_atom_find(const char *s, size_t len);
/* Make room for `extra` more atoms.  Must be called while no other thread
 * is interning (the pipeline calls it before dispatching jobs). */
void css_atom_reserve(size_t extra);
const char *css_atom_str(CssAtom a);
size_t css_atom_count(void);

typedef struct CssStyleSet CssStyleSet;
typedef struct CssStyleDoc CssStyleDoc;
typedef struct CssStyleScratch CssStyleScratch;

/* Compile `count` sheets (in cascade order) for the given viewport. */
CssStyleSet *css_styleset_build(CssStylesheet *const *sheets, int count,
                                double viewport_width, double viewport_height);
void css_styleset_free(CssStyleSet *set);
int  css_styleset_rule_count(const CssStyleSet *set);
const CssRule *css_styleset_rule(const CssStyleSet *set, int g);
/* Legacy cascade order of rule g's first declaration (sheet*1e6 + rule*1e3). */
int css_styleset_rule_order(const CssStyleSet *set, int g);
/* Statistics for logging: compiled chains, chains in the universal bucket. */
void css_styleset_stats(const CssStyleSet *set, int *chains, int *universal);

/* Build element features for every node of `doc`. */
CssStyleDoc *css_styledoc_build(HtmlDocument *doc);
void css_styledoc_free(CssStyleDoc *sd);

CssStyleScratch *css_style_scratch_new(const CssStyleSet *set);
void css_style_scratch_free(CssStyleScratch *t);

/* Prepare the ancestor Bloom filter for a node whose parent (DOM index) is
 * `parent_dom`; then match.  Nodes must be visited parent-first; the filter
 * is maintained incrementally and rebuilt automatically when a scratch
 * starts on an unrelated subtree. */
void css_style_bloom_enter(CssStyleScratch *t, const CssStyleDoc *sd, int parent_dom);
/* Push `dom` after it was matched (so its children see it as an ancestor). */
void css_style_bloom_push(CssStyleScratch *t, const CssStyleDoc *sd, int dom);

/* Match every rule against element `dom`.  Returns the declarations of the
 * matching rules sorted by cascade order (css_applied_decl_compare); the
 * array lives in the scratch until the next call.  Each entry's `rule` is
 * the global rule index. */
int css_style_match(const CssStyleSet *set, const CssStyleDoc *sd, int dom,
                    CssStyleScratch *t, const CssAppliedDecl **out);

/* ::after and ::before (is_before) rules whose branch matches `dom`: global rule index and the
 * specificity of the first matching branch (legacy semantics), in rule
 * order.  Valid until the next call. */
typedef struct CssAfterMatch { int rule; int specificity; int is_before; } CssAfterMatch;
int css_style_match_after(const CssStyleSet *set, const CssStyleDoc *sd, int dom,
                          CssStyleScratch *t, const CssAfterMatch **out);

/* Pseudo-element "aspect" base (text before the first :before/:after of a
 * rule's selector): does the rule have one, and does it match `dom`? */
bool css_style_rule_has_aspect_base(const CssStyleSet *set, int g);
bool css_style_aspect_base_matches(const CssStyleSet *set, const CssStyleDoc *sd,
                                   int g, int dom);

/* Style sharing (Stylo's sharing cache, PARALLEL_STYLE_LAYOUT.md §7.5).
 * Two elements whose parents have equivalent styles may share a computed
 * style when their features are equal and every revalidation chain (those
 * testing sibling position or :empty) gives the same result for both. */
int  css_style_reval_count(const CssStyleSet *set);
void css_style_reval_bits(const CssStyleSet *set, const CssStyleDoc *sd, int dom, uint64_t *bits);
bool css_style_shareable(const CssStyleDoc *sd, int dom);          /* element without an id */
bool css_style_features_equal(const CssStyleDoc *sd, int a, int b); /* tag, classes, attribute names */
CssAtom css_style_tag(const CssStyleDoc *sd, int dom);

/* Counters for logging (per scratch). */
typedef struct CssMatchStats {
    uint64_t nodes, candidates, bloom_rejects, full_matches, matched_rules;
} CssMatchStats;
const CssMatchStats *css_style_scratch_stats(const CssStyleScratch *t);

#ifdef __cplusplus
}
#endif

#endif /* CSS_STYLE_H */
