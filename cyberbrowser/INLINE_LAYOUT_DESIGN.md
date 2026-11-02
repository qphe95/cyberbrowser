# Inline layout on paragraph items

Status: steps 3, 4 and 5a done: inline layout on items by default (CYBER_IFC=0: the old path; steps 1 and 2 measured and dropped, below).

Step 3a (css_layout.c, "Inline formatting on items"): a block whose in-flow content is only text
and inline elements without left or right margin, border or padding, in white-space normal, with
nothing that needs bidi, no generated content and no pseudo-element box (ifc_eligible), is laid out
by ifc_layout instead of the per-child loop.  Tokens: each text box cut into words and collapsible
spaces, a space dropped after a space or at the start (white space collapsed across boxes), a mark
for a box with nothing kept.  Lines: greedy, a break only at a space (none between boxes without
one, so `Official&nbsp;<span>` text stays whole), spaces at a line's start removed and at its end
hanging, each line's room from layout_flow_line_geometry at its top, text-indent on the first,
text-align on its words.  Line height: as the flow's line box takes it (the item's line advance and
how far the item's own children reach), so the two paths agree.  Output: each text box's fragments
per line and its outline around them, each inline element's outline (one line: its content area
centred as the flow does), pieces per line for one on several lines with a background or border; the
block is kept from flow reuse until fragments are in the flow record (step 6).  Results under
CYBER_IFC=1: css/CSS2 5395 -> 5417, none lost (tables 6, css1 5, box-display 3, text 3, linebox 2,
ui 2, positioning 1); 57 screenshots differ from the old path (the fixes and failing pages that
changed).  YouTube's screenshot identical; Wikipedia's differs below its infobox because
"Official&nbsp;languages" (the space a non-breaking one, in its own span) no longer breaks, as in a
browser, and one line no longer starts with a stray space.  Forced layouts within noise of the old
path (Wikipedia 260 / 265 ms median, YouTube 208 / 210).  Without the switch every screenshot is
identical.

Step 3b: inline elements with left and right edges, `<br>` at any depth, empty inlines.  An
element's start edge (margin, border, padding) is a token joined to the word after it, its end
edge one joined to the word before it, neither a break opportunity; an end edge after a space stays
on that line; a space at a line's start is removed after a start edge too.  Spaces at a line's end
are removed with the end edges after them moved back to the last word (an inline box's border does
not take in a trailing space).  `<br>` is a token that ends its line; a line with only a break takes
the break's line height.  An element's outline runs from its start edge's border to its end edge's;
on several lines with a background or border it gets a piece per line, the start side on the line
it opens on and the end side on the line it closes on.  An inline split around a `<br>` by
layout_blockify (BIDI_DESIGN.md step 5) is this path's now: its split pieces are not set up.  Under
CYBER_IFC=1: css/CSS2 5395 -> 5432 (css1 15, text 7, tables 6, box-display 3, linebox 2,
margin-padding-clear 2, ui 2, normal-flow 1, positioning 1), two lost: normal-flow/inline-box-
border-line-break and -padding-line-break, laid out right (the final "xx" starts its line at 0, the
space before it removed there) against a reference that uses an inline-block, which the old path
lays out with that space kept; they come back when 3c takes inline-blocks.  YouTube and Wikipedia
screenshots as in 3a; forced layouts within noise.  Without the switch every screenshot identical.

Step 3c, first part: atomic inlines (inline-block, inline-flex, inline-grid, inline-table, controls)
and inline images.  Each is sized as the flow sizes it (layout_size_atomic_inline; an image by
layout_resolve_used_sizes) and laid out once at the content start, then moved with its subtree
(layout_offset_subtree) to its place: its margin box at its line's top, as the flow places it, the
line as tall as that margin box.  A break opportunity before and after it; the edges of the inline
elements around it stay with it.  An inline element holding only atomic inlines is outlined around
them at the line's top, as the flow sizes it.  An image with display: block is a block, not an atom.
Intrinsic widths of a block this path lays out come from the same words and edges (ifc_intrinsic:
max-content the widest line between forced breaks without its end spaces, min-content the widest
run without a break opportunity or atomic inline), so shrink-to-fit sizes and line breaking agree;
the old block branch made an abspos box too narrow for "a <inline-table> d" (tables/
table-anonymous-objects-178, -180).  Under CYBER_IFC=1: css/CSS2 5441, none lost (3b's two back).
YouTube's screenshot identical.  Wikipedia's infobox changes, both right: the flag's line is 68 px,
the image with its 1 px border (the old path sized its span and link from the image's natural
125x66, so the line was short and the centring 1 px off), and the anthem's audio player (30 px) is
inside its row (the old path gave its inline-block wrapper a 16.9 px text line, so the player drew
across the row's border).  Forced layouts: YouTube within noise, Wikipedia about 5 ms slower (min
262 against 254 ms), likely the word-by-word intrinsic widths; the cached widths of step 5 take that
back.  Left for the second part: floats inside the paragraph.

Step 3c, second part: floats that are the block's own children (floats deeper in, inside inline
elements, still take the old path).  The flow's placement is one function now, layout_place_float,
which both paths call (the old path's results unchanged).  A float is a token with no width on the
line: placed at its line's top when it fits beside what the line holds (CSS 2.1 section 9.5.1; the
line's tokens then start past a left float), else after the line, and once one waits for the line's
end the floats after it on that line wait too (none above an earlier one).  A line that floats
narrow too much for its first content goes down past them (section 9.5), and a line's room is
checked over its whole height (ifc_band_geometry: every float overlapping any of it), the line built
again once in that room when it is narrower.  The same band check now places a block that avoids
floats (overflow, flow-root, a table, flex, grid) with a definite height, in the old path too: it
went beside the floats at its top only (floats-wrap-top-below-bfc-001l, whose inline-block
reference the new path now lays out right).  Under CYBER_IFC=1: css/CSS2 5466, none lost (floats 12
of the fixes); without it 5398 (three floats pages fixed by the block placement, none lost, seven
screenshots changed).  YouTube and Wikipedia screenshots unchanged in both modes.  Forced layouts:
YouTube within noise; Wikipedia under the switch about 6 ms over the old path (min 256 against 250).

Step 3d: white-space modes and generated content.  Each text box's own white-space: pre and
pre-wrap keep its spaces (pre as part of the line, no break; pre-wrap as spaces that are break
opportunities and are not removed at a line's ends, hanging there), pre, pre-wrap and pre-line keep
its newlines as forced breaks (a newline line takes the text's line height), nowrap's collapsible
spaces are not break opportunities, except that a space of wrapping text collapsed into a nowrap
one keeps its break opportunity (text/white-space-collapsing-breaks-001).  The intrinsic widths
follow the same rules.  ::before and ::after boxes are inline elements like any other, and text with
generated content realized (text_gen) is laid out as it reads; ::first-letter stays on the flow's
path (taking it lost linebox/line-height-bleed-002, which needs its own sizing).  Under CYBER_IFC=1:
css/CSS2 5491, none lost (since 3c: tables 8, text 9, generated-content 6, lists 1, floats 1);
without it 5398, every screenshot as at 3c.  YouTube identical; Wikipedia's kept-tree check now finds
no layout that differs (it found one every run before), and its infobox label column is 5 px wider:
a label such as "Ethnic groups (2020)[8]" is one unbreakable run now (no space between the year and
the reference, and none between ")" and "[" is a break opportunity), where the old path broke
between the two boxes.  Forced layouts as at 3c.

Step 3 now covers what the flow's inline path does except: paragraphs that need bidi (step 4),
::first-letter, floats and positioned boxes inside inline elements, line-clamp and text-overflow
ellipsis.  Those blocks keep taking the old path under the switch.

Step 4 (ifc_bidi_levels, ifc_bidi_line): paragraphs that need bidi take the item path too, and the
bidi post-pass is not called for a block laid out on items.  Levels: UAX #9 over the paragraph's
text gathered as the post-pass gathers it (bidi_para_collect, the levels cache), set on the tokens
in place; a word is cut where its level changes (each part measured), and the controls
unicode-bidi stands for become zero-width tokens where they are in the text, so they keep runs
apart when reordered.  A paragraph is finished in visual order when it has a right-to-left level,
is right to left itself (its first line's indent at the right) or holds a right-to-left inline
element (whose start edge is its right one).  Lines break as before; each is then placed from its
start in visual order: its units (words, spaces kept, atomic inlines, breaks, marks, controls, and
the edge of an element with nothing else on the line) at their levels, white space at its end
back to the paragraph's (L1), reordered (L2); an inline element's pieces are its stretches of
visually adjacent units, its left and right edges on the leftmost and rightmost unit of the lines
CSS 2.1 section 8.6 puts them on; alignment on the visual extent.  Text fragments are per visual
run with their level; an element split by reordering or across lines with a background or border
gets a piece per stretch.  Under CYBER_IFC=1: css/CSS2 5500, none lost (bidi 6, floats 2, text 1);
without it 5398, every screenshot as at 3d.  YouTube identical.  Wikipedia: the paragraphs that
need bidi only for their isolated reference marks (`<sup>` with unicode-bidi: isolate) now come
from the item path, which drops the stray leading spaces and orphaned brackets the post-pass left
(" (2020)" in the infobox labels, "(4th" broken at the bracket); forced-RTL Wikipedia matches the
old path but for the same bracket fixes and the infobox column widths of 3d.

Timing, forced-RTL Wikipedia against plain (7 alternating rounds, min / median of the forced
layouts): items 278 / 291 ms against 249 / 279; old path 294 / 307 against 262 / 266.  The gap is
about the same (~30 ms); measured inside the item path over the replay's layouts (CPU, summed): the
levels pass 28 ms (gathering the text again 5, matching tokens to code points 9, levels 11 of which
half are cache misses since the cache is per thread, element setup 3), line finishing ~8 ms, and
the blocks still on the old path 6 ms.  No single part closes it; what does is the paragraph data
of the design: code points gathered and levels looked up while the tokens are made (one walk, no
matching), and the levels cache shared between layout threads.  That is step 4b.

Step 4b: a paragraph that needs bidi has its code points gathered while its tokens are made
(ifc_collect: each text box's text decoded once, U+FFFC per atomic inline, U+2029 per `<br>`, the
controls unicode-bidi stands for pushed with their zero-width tokens where they are), each token
noting its code-point range, so levels go straight onto tokens and a word is cut only where its own
range changes level; bidi_para_collect and the matching walk are gone from the item path.  The
levels cache is shared by the layout threads (8192 slots, a lock per stripe of 64, 16 MB at most),
where it was per thread and emptied at 8 MB.  Everything identical to step 4 (css/CSS2 5500 under
the switch and 5398 without, all screenshots; the three replays).  Timing (9 alternating rounds,
min / median): forced RTL 284 / 319 ms at step 4, 270 / 281 now; plain 245-252 / 262-265 in every
build.  Cache hits went from about half to two thirds of the code points (the misses are the first
layout's).  What is left of the gap, about 15-25 ms, is spread thin (CPU over the replay's
layouts): levels 9 (resolving 5, hashing and copying hits 4), levels onto tokens 5, line finishing
8, element setup 3, gathering 4, and 5 for the blocks still on the old path (mixed block and inline
content, ::first-letter); each is a few ms, and the larger cuts are the later steps (levels kept with
the paragraph data across layouts, step 6; those blocks on items, step 5).

Step 5a: the item path is the default (CYBER_IFC=0 turns it off); blocks it cannot take still go
the old way.  Two fixes the unit tests and the Wikipedia replay found, which css/CSS2 did not: an
atomic inline is a break opportunity only where the text around it wraps (not in nowrap or pre:
test_layout_nowrap_atomic_inlines, YouTube's info row), in line breaking and in min-content; and in
nowrap a collapsible space counts toward min-content only once content follows it (Wikipedia's
Article / Talk tabs, a link then a newline in a nowrap list item, were 3.9 px too wide).  css/CSS2:
5500 by default, none lost against step 4b under the switch (one failing page's screenshot changed,
visudet/content-height-004, an inline-block in pre); 5398 with CYBER_IFC=0, every screenshot as
before.  The three replays identical to 4b.  Timing, new default against CYBER_IFC=0 (alternating,
min / median): Wikipedia 255 / 283 against 262 / 287 ms; YouTube 197 / 231 against 197 / 234 (15
rounds).

Step 5b: absolutely positioned (and fixed) boxes at any depth of the paragraph no longer keep it
off the item path.  Each is a zero-width token (IFC_ABS) where it is in the content, out of the
line (the spaces around it collapse together), and after the lines are placed it gets its static
position (CSS 2.1 section 10.3.7), kept on its box relative to its parent (LayoutBox.static_dx /
static_dy, so later shifts carry it) for the positioning pass, which uses it instead of guessing
from the box before it, then clears it: one that was inline-level before blockification
(LayoutStyle.abs_inline, set there) where it is on its line, at the line's top; a block-level one
at the content's left edge, below its line when something comes before it there.  Tokens of a
paragraph with no line now sit at the content's start, not at x 0 (abspos-inline-008 and
toogle-abspos-on-relpos-inline-child, a relative span holding only an abspos box, which an earlier
try lost).  css/CSS2 5509, none lost (abspos 3, normal-flow 3, positioning 1, floats 1, text 1).
YouTube and Wikipedia identical; forced-RTL Wikipedia differs only where its list items' trailing
space (the visual left of a right-to-left line) now hangs as it should, where the old path left a
gap.  Timing against 5a (9 rounds, min / median): Wikipedia 256 / 262 against 257 / 267 ms,
YouTube 205 / 217 against 205 / 228.

Goal: lay out a block's inline content the way browsers do, from
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

1. **Paragraph data for the bidi pass.**  Not done: measured first, it would save 3-8 ms.  The
   levels are already cached by paragraph text, so only the gathering (8 ms over the replay's six
   layouts) was left to save, and half those layouts build a fresh tree, where data kept per box
   index cannot carry over; on the other half, flow reuse keeps 3,155 of 45,891 boxes, so its
   "unchanged" signal cannot be relied on either.  The paragraph data comes with step 3 instead.
2. **Recorded lines.**  Built and dropped: the flow recorded the lines of wrapped text (in inline
   elements too, 4,525 of 5,715 wrapped boxes on the forced-RTL replay) and fragments were built
   from them, and the shaper's line breaking measured each word once instead of three times; every
   screenshot identical, and no measurable change in 10 alternating rounds (forced-RTL Wikipedia
   289 -> 290 ms median, plain Wikipedia 256 -> 256, YouTube 201 -> 202).  The second line breaking
   cost about 5 ms over six layouts and recording about as much.  What is left of the gap is the
   post-pass as a whole; step 3 removes it.
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
   paragraphs of items).  Intrinsic widths from the paragraph data.  In sub-steps, by what still
   keeps blocks with inline content off the item path (counted on the replays: YouTube 1038 on items,
   166 line-clamp or ellipsis, 118 with an absolutely positioned child, 46 with block children among
   the inline ones; Wikipedia 15031 on items, 1185 abspos child, 165 block children, 20 an inline
   replaced element such as an input): 5a the item path by default (CYBER_IFC=0 the old one), the
   rest still on the old path; 5b absolutely positioned boxes in the paragraph (their static
   position from the line); 5c line-clamp and text-overflow ellipsis; 5d inline content between
   block children (each run of it a paragraph on items); 5e what remains (inline replaced elements,
   ::first-letter, floats inside inline elements, blocks inside inline elements); 5f the old inline
   path and the bidi post-pass removed.
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
