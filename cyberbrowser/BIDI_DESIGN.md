# Bidirectional text: UAX #9 in inline layout

Status: step 1 done (src/bidi.c, src/bidi_tables.h from Unicode 18.0.0 by tools/gen_bidi_tables.js,
tests/test_bidi.c; unused by layout, css/CSS2 unchanged at 5321).  Unicode's conformance cases
pass: all 91,707 of BidiCharacterTest.txt 18.0.0 (BIDI_CHAR_TEST=<path> with the bidi tests; the
file is not committed, from https://www.unicode.org/Public/UCD/latest/ucd/).  Step 2 done (css/CSS2
5321 -> 5322, none lost): `unicode-bidi` in the box style (LayoutStyle.unicode_bidi, not inherited,
read by nothing yet); HTML's rules in the UA sheet (`bdi, output, [dir]` isolate, `bdo`
isolate-override, `[dir=ltr i]` / `[dir=rtl i]` direction, `textarea` / `pre` `[dir=auto i]`
plaintext); `dir=auto` and a `bdi` without `dir` take their direction from their text's first strong
character (style_auto_direction, applied as a presentational hint and part of the style cache key);
the shaper gives the bidi controls (ALM, LRM, RLM, U+202A-202E, U+2066-2069) no glyph and no
advance.  A `dir=rtl` block's lines now align right.  YouTube's replay: layouts, forced layouts and
the screenshot unchanged; its script's count of cached layout queries went 3856 -> 4120 already with
step 1 alone (unused code), so with the binary, not with what step 2 does.

Step 3 done (css/CSS2 unchanged at 5322; every screenshot identical).  At the end of a block's flow
(layout_bidi_paragraph), a paragraph that needs bidi (an rtl block, an inline with rtl or
`unicode-bidi`, or text with a right-to-left letter or a bidi control, by a byte scan) gives each of
its text boxes fragments (LayoutContext.text_frags: per line, the bytes, x and em-box top relative
to the box, width, level), from the shaper's own line breaking (text_shaper_wrap_lines: the wrap
walk records each line's first and last drawn byte at its three break points) or, unwrapped, one;
the block is then not reused at the next pass, as a split inline's is.  Paint draws a box with
fragments fragment by fragment (text_shaper_shape_span, which also draws right to left, mirrored,
for step 4).  Left on the old path: text in split inlines (block_in_inline), an unwrapped `pre` run
with newlines, clamped or ellipsised text, and text not laid out by the block flow.  Checked with
CYBER_BIDI_ALL=1 (fragments for every paragraph): of 8,345 css/CSS2 screenshots one differs,
white-space-processing-042, where the one-line path drew a tab as a .notdef box and fragments (as
the wrapped path always did) draw nothing; YouTube and Wikipedia screenshots identical, YouTube's
forced layouts 212 ms normally, 213 ms with every paragraph in fragments.

Step 4 done (css/CSS2 5322 -> 5374, none lost: bidi-text 40, css1 6, normal-flow 2, generated-content
2, box-display 1, positioning 1).  layout_bidi_reorder, after the fragments: the paragraph's code
points (text in logical order, U+FFFC per atomic inline, U+2029 per block, the controls each
inline's unicode-bidi stands for, and the block's own override or plaintext around it all), its
levels (bidi_resolve), its lines (from the fragments: a new one at a box's later fragment, or where
an item is both left of and below the one before; empty runs, collapsed spaces the flow parked
anywhere, neither start nor place one), L1 per line, runs cut at level changes and where the line's
trailing white space starts, and the controls kept as zero-width runs so an isolate still separates
what is around it; then L2, and x assigned from the line's start: each run with the edges (margin,
border, padding) of the inline boxes it opens before it and those it closes after it, the space the
flow left before it (negative where it overlapped boxes to collapse a space), its width; trailing
white space hangs (no room).  Atomic inlines move with their subtree; an inline box whose content
stays one piece on one line moves with it; a line holding wrapped text (which the flow does not
align) is aligned here.  Fragments at odd levels are drawn right to left, mirrored.  A paragraph
with nothing right to left in it (no R, AL or AN, no right-to-left control, a left-to-right block)
is left as the flow placed it, before the algorithm runs: Wikipedia's prose paragraphs need bidi only
for an isolate inside them.  Alongside, for every paragraph: a line's trailing collapsible white
space no longer counts when it is aligned right or centered (layout_line_hanging_space; CSS Text 3
section 4.1.3), as in browsers; Wikipedia's screenshot moves by a pixel or two on centered lines.
YouTube's forced layouts 205 ms, Wikipedia's 363 ms (369 before).  Left for step 5: an inline box
the reordering splits or reverses keeps its one box (backgrounds and borders where the flow put it,
edges on the left-to-right sides).  Steps 5-6 planned.

## Why

css/CSS2 has 105 failing pages that use right-to-left or bidi features: most of bidi-text (60),
box/rtl-* and ltr-*, and scattered pages in positioning, text, floats, generated-content.  What
they use: `direction: rtl` (68 pages), `unicode-bidi` (40), the embedding / override controls
U+202A-202E in text (27), the `dir` attribute (14), `<bdo>` (5), Hebrew letters (3).  So the suite
leans on overrides and embeddings more than on character classes; real pages (Hebrew, Arabic,
Persian, Urdu sites, and mixed numbers / punctuation in them) lean on the classes.

Today:

- No bidi algorithm.  `direction` is parsed and inherited, and drives only `text-align: start /
  end`, over-constrained block margins and the abspos static position.  `unicode-bidi` is in the
  property table (css_props_table.h) but layout ignores it.
- The UA sheet has no `[dir]` rules, so `<div dir="rtl">` changes nothing; `<bdo>` and `<bdi>` are
  plain inlines.
- Inline layout (layout_block_flow_inner) packs every line from the left in logical order.
  layout_align_line shifts a finished line's members for right / center alignment, but gives up on
  any line holding a wrapped text box, so a wrapped rtl paragraph stays left-aligned.
- A text node is one layout box however many lines it covers; its line breaks exist only inside
  the shaper (ts_wrap_walk), which layout runs to measure (text_shaper_wrap_measure) and paint runs
  again to draw (text_shaper_wrap_shape).  No per-line byte ranges are kept.
- An ordinary span over several lines paints one background / border rect, its bounding box.  Per
  line fragments exist only for an inline split around blocks (FlowPiece, ctx->pieces,
  paint_pieces), with the left edge on the first line and the right on the last, always.
- The shaper draws code points left to right only: no runs, no mirroring, and bidi control
  characters are not skipped (they draw as fallback glyphs).

## How browsers do it

CSS Writing Modes 3 section 2 and CSS 2.1 sections 8.6 / 9.10, with UAX #9: for each paragraph
(the inline content of a block container), the text is the logical sequence of its characters,
with inline boxes' `unicode-bidi` / `direction` contributing embeddings, overrides and isolates as
if bidi control characters were at their edges (atomic inlines count as U+FFFC, neutral).  UAX #9
resolves an embedding level per character.  Lines are broken in logical order as before; then each
line is reordered (rule L2: reverse every run at level >= k, for k from the highest level down to
the lowest odd one), whitespace at line end is reset to the paragraph level first (L1), and glyphs
at odd levels are mirrored (L4: `(` draws as `)`).  An inline box broken by the reordering becomes
several fragments; per CSS 2.1 8.6 its start-side margin / border / padding go on its first
fragment and end-side ones on its last, in the box's own direction.

## Design

### Unicode data and the algorithm (new src/bidi.c)

- Tables generated by a Node script (tools/gen_bidi_tables.js, like gen_charset_tables.js): the
  Bidi_Class of every code point as ranges (DerivedBidiClass.txt), mirroring pairs
  (BidiMirroring.txt), paired brackets (BidiBrackets.txt).  Node's RegExp cannot give Bidi_Class
  (no `\p{Bidi_Class=...}`), and nothing on this machine has the data (no ICU or fribidi headers),
  so the three files come from unicode.org (about 100 KB in all; a download, so with the user's
  consent).  Without them, a fallback derives the classes from Script and General_Category (Hebrew
  R, Arabic / Syriac / Thaana AL, Nd EN, Arabic-Indic digits AN, Mn NSM, Cf BN, ...), right for
  the suite and most text but not exact.
- UAX #9 in full on a code-point array: P2 / P3 (for `dir=auto` and `unicode-bidi: plaintext`),
  X1-X10 (embeddings, overrides, isolates, with the 125-level stack), W1-W7, N0 (bracket pairs),
  N1-N2, I1-I2, L1 per line, L2 as a visual-order index map per line.  Output: a level per code
  point.
- Unit tests from the UAX #9 examples, plus BidiCharacterTest.txt when present (another
  unicode.org file, a few MB, not committed: the test skips without it).

### When it runs

Only a paragraph that needs it: one with a character of class R, AL or AN, a bidi control
character, or an inline box with `direction: rtl` or `unicode-bidi` other than normal, or a block
with `direction: rtl`.  The check on text is a byte scan (UTF-8 lead bytes of U+0590-08FF, the
controls' E2 80 / E2 81 sequences, the presentation forms), so a left-to-right page pays a scan of
its text and nothing else; YouTube and Wikipedia (English) stay on today's path, measured with the
replays.

### Paragraph text and levels

In layout_block_flow_inner, before the line loop, for a paragraph that needs bidi: walk its inline
content (kids and nested inline elements), building the code-point array with the controls each
inline box implies (`embed` LRE / RLE ... PDF, `bidi-override` LRO / RLO, `isolate` LRI / RLI ...
PDI, `isolate-override`, `plaintext` FSI), U+FFFC for atomic inlines, and a map from each code
point to (box, byte offset).  Levels come back per code point and are kept for the paragraph.

### Line fragments

A bidi paragraph's lines are kept as fragments, the hook the investigation found missing:

- Text: for each text box, its per-line byte ranges, taken while it is measured (the shaper's
  existing per-line note, ts_note_line / t_line_starts, as text_shaper_truncate_lines uses it),
  split further where the level changes.  Stored per box like ctx->pieces (a fragment list anchored
  to the box, so layout_offset_subtree, flow_shift and incremental reuse move them with it):
  `{byte_start, byte_end, line, x, y, width, level}`.
- Inline elements: per-line fragments as LayoutPiece already are, generalised from block-in-inline
  to any inline element in a bidi paragraph, with `sides` decided by CSS 2.1 8.6 in the element's
  own direction instead of always left-first.  This also gives ordinary multi-line spans in such
  paragraphs per-line backgrounds; the left-to-right path keeps today's one rect until that is
  taken up on its own.
- Atomic inlines stay boxes, positioned by the reordering.

### Reordering a line

layout_align_line becomes the line's finish step: for a bidi paragraph, the line's items in
logical order (text fragments, atomic inlines, inline element edges at their embedding level), L1
on trailing white space, L2 to visual order, then x assigned left to right from the line's start
(its right end at a higher base level: rtl lines start at the right, with text-indent on that
side), then the text-align slack measured on the visual extent.  Lines with wrapped boxes no longer
need to bail out: in a bidi paragraph a wrapped box is its per-line fragments.

### Painting

The display list emits a box that has fragments fragment by fragment, through a new shaper entry
point (text_shaper_shape_span: bytes [start, end) at x, odd level drawn right to left, mirrored
glyphs at odd levels, bidi controls and other default-ignorables skipped), instead of re-walking
the wrap.  Decorations already follow the emitted glyphs.  The shaper skips bidi controls on the
left-to-right path too.

### Style and HTML

- `unicode-bidi` parsed to the box style; `direction` already is.
- UA sheet, per HTML's rendering section: `[dir] { unicode-bidi: isolate }`, `[dir=rtl i] {
  direction: rtl }`, `[dir=ltr i] { direction: ltr }`, `bdi { unicode-bidi: isolate }`, `bdo {
  unicode-bidi: isolate-override }`; `dir=auto` from the element's first strong character (P2 / P3
  over its text, isolates skipped).
- Not in scope: Arabic joining forms (the shaper has no GSUB; Arabic draws in isolated forms),
  vertical writing modes.

## Steps (each lands on its own: full css/CSS2 run, no page lost, replay gate)

1. **Data and algorithm**: tools/gen_bidi_tables.js, src/bidi.c, unit tests (UAX #9 examples;
   BidiCharacterTest.txt when present).  Unused by layout; no reftest change.
2. **Style and HTML**: `unicode-bidi` in the style, the `dir` / `bdi` / `bdo` UA rules, `dir=auto`,
   bidi controls skipped by the shaper.  Expected: pages whose only rtl is block-level (text-align,
   margins, abspos from `dir`) and those drawing stray control glyphs; a handful.
3. **Line fragments, left to right**: per-line text fragments and inline element pieces for
   paragraphs that need bidi, painted through text_shaper_shape_span, no reordering yet.  Checked by
   forcing the fragment path on for every paragraph (an env switch) and comparing the full css/CSS2
   run and the replay screenshots with the normal path: identical.  Expected: none (the foundation).
4. **Reordering**: paragraph levels, L1 / L2 per line, rtl glyph order, mirroring, rtl line start
   and text-indent, alignment on the visual extent.  Expected: most of bidi-text (bidi-0xx,
   breaking, glyph-mirroring, inline, unicode-bidi-*), box/rtl-*, line-breaking-bidi,
   text-align-white-space.
5. **Inline boxes split by bidi**: fragments' edges by CSS 2.1 8.6, backgrounds and borders per
   fragment.  Expected: bidi-box-model-*, bidi-00x with bordered spans, box/*-ib.
6. **The rest**: `plaintext`, generated content in bidi (bidi-generated-content), white space
   collapsing across bidi boundaries, the abspos static position of an inline in an rtl line, hit
   testing and getClientRects over fragments.

Expected in all: 50-70 of the 105 pages; some of the 105 fail for other reasons (floats placement,
positioning) and are counted only once looked at.

## Checks

- Unit tests for bidi.c (levels and visual order of the UAX #9 examples; the conformance file when
  present) and for paragraph text building (controls per `unicode-bidi`).
- Step 3's forced-fragment comparison: the new path draws left-to-right text exactly as the old.
- Full css/CSS2 runs after each step, no page lost; the replay gate (keep-verify, screenshots,
  layout time) unchanged on YouTube and Wikipedia, which never take the bidi path.

## Risks

- Layout and paint must agree: a box with fragments must never be re-wrapped at paint time.  The
  fragments are the only geometry painted for it.
- Incremental reuse (flow_shift, kept trees) moves boxes; fragments anchored like LayoutPiece move
  with them, and keep-verify catches what does not.
- Hit testing and getClientRects read box rectangles; until step 6 they see a bidi box's bounding
  box, as they do for wrapped text today.
