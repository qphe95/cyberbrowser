# Inline layout on paragraph items

Status: planned, nothing built.  Goal: lay out a block's inline content the way browsers do, from
a per-paragraph list of items analysed once (text, white space, break opportunities, bidi levels,
widths) and kept across layouts, so that right-to-left and mixed-direction text costs about what
left-to-right does, line breaking never measures the same text twice, and the inline boxes a line
splits get exact per-line pieces.  It replaces the bidi post-pass of BIDI_DESIGN.md steps 3-5 and
the inline half of layout_block_flow_inner.

## Why

Measured after the bidi speedups (BIDI_DESIGN.md, "Speed of the right-to-left path"): the
Wikipedia replay with every paragraph forced right to left takes about 292 ms of forced layouts
against 259 ms left to right (+13%).  What is left of the bidi pass, 46 ms over the replay's
layouts, has no hot spot: gathering text 8 ms, levels 10, fragments 11 (most of it a second line
breaking of each wrapped text box), cutting and placing runs 15.  Each exists because bidi runs
after the flow, on boxes the flow placed without it, and runs again in full on every layout.

The same structure costs left-to-right pages too:
- An inline element is measured as its children's text concatenated into an 8 KB buffer
  (layout_concat_box_text), laid out as one run, and its children placed afterwards
  (layout_inline_wrapped_children, or the single-text-child shortcut).  Content past 8 KB, a `<br>`
  two inlines deep, and an inline's edges inside wrapped text are approximations.
- A wrapped text box is one box with wrap geometry (first line width, continuation x and width, a
  second continuation geometry under a float that ends); its lines exist only inside the shaper's
  walk, run once to measure and again to paint.  Floats that end after the second line, and
  anything per line (pieces, getClientRects), have nothing to work from.
- White space collapses by overlapping neighbouring boxes and a leading-space skip
  (text_lead_skip), not in the text.
- Intrinsic widths (layout_intrinsic_width) measure text again with their own walk.

## How browsers do it

(From their source, as recalled; not re-checked for this plan.)  Chrome's LayoutNG keeps per
inline formatting context an InlineNodeData: the text content of the whole paragraph after white
space collapsing, a list of InlineItems (text runs, open and close tags of inline boxes, atomic
inlines, controls, floats, `<br>`), each text item split where its bidi level or font changes, and
a ShapeResult per item from HarfBuzz.  The bidi algorithm (ICU's, through NGBidiParagraph) runs once
when the data is built, and only when the paragraph has a right-to-left character, a control, or
right-to-left direction.  The data lives until the text or a style that affects it changes.  The
line breaker walks items and reads widths from the shape results (reshaping only at a break inside
a run where shaping could differ); a line's items are put in visual order by index
(IndicesInVisualOrder), cheap per line; the result is a line box fragment tree that painting, hit
testing and getClientRects read.  Firefox resolves bidi when frames are built or text changes
(nsBidiPresUtils::Resolve splits text frames into continuations per level, flagged dirty on
change) and only reorders per line in reflow.  WebKit's newer inline layout (IFC) builds InlineItems
with levels once per content change the same way.  In all three a relayout of unchanged text does
no bidi analysis and no measuring beyond the line breaker's sums, which is why right to left costs
them about what left to right does.

## Design

### Paragraph data (LayoutInline, per block that has inline content)

Built from the block's inline content (into inline elements, not into blocks, atomic inlines or
out-of-flow boxes, the walk bidi_para_collect does today):
- `text`: UTF-8, the paragraph's content after CSS Text 3 section 4.1 white-space processing
  (collapsing, segment breaks, pre / pre-wrap / pre-line / nowrap / break-spaces), U+FFFC per atomic
  inline, the controls `unicode-bidi` stands for, U+2028 per `<br>`.  Generated content (::before,
  ::after) is in it as its own text items.
- `items`: kind (text, open, close, atomic, br, float, control), the box, its byte range in
  `text`, its bidi level, its style's font slot and spacing.  A text item is split where the level
  changes.
- Break opportunities per text item: the ones the shaper's walk uses today (after spaces, after a
  hyphen between letters), as a bit per byte or a list of offsets.
- Widths: per text item, the width of each piece between break opportunities (with its trailing
  spaces apart), measured once with the item's font and spacing (ts_measure_span sums today).  Line
  breaking adds these; text cut at a level change inside a piece is measured then.
- Bidi: levels from bidi_resolve, only when the paragraph has right-to-left content (the checks of
  layout_bidi_reorder today); the paragraph level.

Kept in the LayoutContext per block index (as text_frags and pieces are).  Valid while nothing in
the paragraph changed: its boxes and their order, their text, the styles that affect text, and
fonts (a font load clears all, as css_layout_retained_invalidate does for the flow record).  On a
kept tree, carried to the new index by flow_old as the flow record is; on a fresh tree rebuilt (the
bidi levels cache by content still saves the algorithm).  Memory: about 10-14 bytes per character
plus items; the Wikipedia page's ~300K characters are ~4 MB.  One paragraph is laid out by one
thread (parallel layout splits between independent subtrees), so the data needs no locks.

### Lines

The line breaker walks the items from the block's content start: text by its pieces' widths,
open and close items adding the inline box's start or end edges (margin, border, padding), atomic
inlines sized as now (layout_size_atomic_inline), `<br>` ending the line, floats placed when met (as
now) and every line's left and width from layout_flow_line_geometry at its top (no more first /
continuation / cont2 geometry), text-indent on the first line, `white-space` nowrap and pre, a word
too wide for an empty line broken at a hyphen or overflowing, trailing collapsible spaces hanging.
A line is an item range with a start and end byte in its first and last text item.

Finishing a line: L1 and L2 over its items when the paragraph has bidi (whole items, split only at
the line ends: no re-measuring); x assigned from the line's start in visual order, each inline box's
edges by CSS 2.1 section 8.6 (BIDI_DESIGN.md step 5's rule); text-align on the visual extent,
trailing hanging space left out; the line box height from the strut and its items (content areas
of inline boxes, atomic margin boxes), as today's line_box.  vertical-align stays baseline-only, as
today.

### Output: the structures painting already reads

- Text boxes: their fragments (LayoutTextFrag per line, byte range, x, y, width, level), which paint
  draws since bidi step 3; the box itself the union of its fragments (what scripts measure and hit
  testing uses today).
- Inline elements: their pieces per line (LayoutPieceList, anchored to themselves), painted since
  step 5; the box the union.
- Atomic inlines: placed.
- The flow cursor after the paragraph, and the floats it placed, as now.

So the display list, hit testing and the DOM geometry APIs keep working on the same fields; the
wrap-geometry fields (wrap_first_w, wrap_cont_*) stop being written and their paint path goes.

Intrinsic widths: max-content is the sum of item widths per forced line, min-content the widest
unbreakable piece plus edges, both from the paragraph data (layout_intrinsic_width's text path goes).

### Incremental layout

Fragments and pieces go into the flow record (moved to their new index on reuse, relative to their
box so they shift with it), so a paragraph with bidi or a split inline is reused like any other
block; layout_flow_noreuse_up stops being needed for either.

## Steps (each lands on its own: full css/CSS2 run with no page lost, screenshots compared,
YouTube and Wikipedia replays, LTR and forced-RTL timing)

1. **Paragraph data for the bidi pass.**  Build and cache LayoutInline (text, items, levels) and
   have layout_bidi_reorder take its text and levels from it instead of gathering and resolving per
   layout.  The flow is unchanged.  Expected: identical screenshots; the bidi pass's gather and
   levels (18 ms of 46 on the forced-RTL replay) gone on layouts after the first.
2. **Widths and recorded lines.**  Piece widths in the paragraph data; the flow's text wrapping
   sums them instead of walking the shaper, and records each wrapped box's lines where it places it,
   so fragments come from the flow and not from a second wrap walk, and cuts reuse the widths.
   Expected: identical screenshots (checked: summed widths equal walked ones to the last bit, or
   the difference found), the fragments step (11 ms) mostly gone, the flow's own text measuring
   cheaper for every page.
3. **The line breaker, beside the old flow (CYBER_IFC=1).**  Inline content laid out from items
   as above, writing fragments, pieces and boxes; the old path stays the default.  Compared run by
   run against the old path on the full suite and the replays, in sub-steps that each reach no page
   lost under the switch: 3a text and inline elements without edges; 3b inline box edges, `<br>` at
   any depth, empty inlines; 3c atomic inlines and floats (every line's own geometry); 3d white-space
   modes and the collapsing done in the text, generated content.  Expected under the switch: the
   pages that fail for nested `<br>`, inline edges in wrapped text and white space between boxes.
4. **Line finishing.**  Bidi reordering of a line's items, edges by section 8.6 and alignment in
   the new path; the bidi post-pass is not called under the switch.  Expected: forced-RTL layout time
   within noise of LTR.
5. **Switch.**  CYBER_IFC on by default, then the old inline path removed:
   layout_inline_wrapped_children, the concatenated-text measuring, the wrap-geometry paint path,
   the bidi post-pass, block_in_inline 2 pieces (a block inside an inline becomes a break between
   paragraphs of items).  Intrinsic widths from the paragraph data.
6. **Reuse.**  Fragments and pieces in the flow record; no paragraph kept from reuse.
7. **Bidi step 6** (BIDI_DESIGN.md) on the new path: `plaintext` per paragraph, white space at bidi
   boundaries (now in the text), generated content in bidi, the static position of an inline
   abspos box in a right-to-left line, getClientRects per fragment.

Sizes: steps 1, 2, 4 and 6 are a few hundred lines each; step 3 is a rewrite of the inline flow
(the per-child loop of layout_block_flow_inner is about 900 lines with many page-specific rules,
each to keep or retire with a test showing why) and is most of the work; step 5 is mostly deletion.

## Checks

- Every step: the full css/CSS2 run (no page lost against the step before), byte comparison of
  all screenshots (cmpshots), the YouTube and Wikipedia replays (keep-verify differ counts and
  screenshots), unit tests.
- Timing per step, alternating builds: Wikipedia and YouTube forced layouts (must not get slower),
  and the forced-RTL Wikipedia replay against the plain one (the gap is the target).  A real
  right-to-left page (Hebrew or Arabic Wikipedia) recorded as a replay would be the better
  benchmark: recording fetches it from the web, so it waits on the user's go-ahead.
- Under CYBER_IFC=1 before step 5: the same suite and replay comparisons against the old path, so
  each difference is a fix or a bug known before the switch.

## Risks

- The old flow's page-specific rules (a run pushed whole to the next line unless its first word
  fits; `<span> </span>` between YouTube hashtags as one collapsible space; the 80x24 fallback for
  empty form elements; hyphen prefixes): each needs its reason found and kept or dropped on
  evidence, or the replays drift.
- Scripts that measure text (getBoundingClientRect, offsetWidth) see slightly different numbers
  where the new breaker places text more exactly; YouTube's layout-dependent scripts are the check.
- Memory for the paragraph data on very long pages; dropped for blocks scrolled far away if it
  matters (not planned).
- Summed piece widths differ from a full walk by kerning across a break (none today: the walk
  restarts kerning at each word too) and by float rounding; step 2 measures this before relying on
  it.
