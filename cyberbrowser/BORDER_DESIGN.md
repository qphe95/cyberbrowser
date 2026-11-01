# Borders: per-side colors and styles, corner joins, the collapsing model

Status: steps 1 and 2 done (css/CSS2 5028 -> 5096 pages passing, none lost; the gpu_decode
contracts are updated for mask kind 6 but `tools/prove.sh` was not rerun: Frama-C is not installed
on the machine it was built on); step 3 done (dashed / dotted: no css/CSS2 reftest uses them
where they fail, so the count is unchanged); step 4 done (css/CSS2 5096 -> 5180, the same in repeated runs, none lost).

As built, step 4: a pass after the style pass (layout_collapse_borders) places each collapsing
table's grid (table_place, split out of table_build), resolves its edges and writes the used
borders from the cascade's (border_spec_*, kept by the style finish, border_coll marking a style the
pass wrote).  An edge is split in whole pixels (floor(w / 2) to the box before it); the table keeps
its half of the outer edges as room but paints none, the cells along the outside painting their
edges whole (border_out).  Boxes whose collapsed borders changed lose their last layout
(style_left), else the flow pass would reuse a layout from before the pass.  The pass checks the
style's skipped, not the box's LAYOUT_STYLE_SKIPPED flag: that is still the last layout's (another
page's box at that index, in a reused tab) until layout starts the box, which made the results
depend on the pages run before.

## Why

A box's border today is four widths, one color for all sides and a "styled" bit per side
(LayoutStyle `border_top..left`, `border_color_*`, `border_styled`).  It is painted as four
rectangles (display_list.c, and `paint_pieces` for split inlines), or as a ring mask when the box
is rounded.  So:

- `border-top-color` and friends, and `border-color` with 2-4 values, set the one color (the last
  one wins);
- every style is drawn solid: dashed, dotted, double, groove, ridge, inset and outset look alike;
- where two sides meet, one rectangle overlaps the other instead of the diagonal join browsers
  draw, which shows as soon as the two sides differ in color or style;
- `border-collapse: collapse` only removes the spacing: there is no conflict resolution (CSS 2.1
  section 17.6.2), cells keep their whole borders, and rows, row groups and columns paint none.

About 110 failing pages in css/CSS2 involve these: the `border-*-applies-to-001..006` tests
(borders on row groups, rows, columns: collapsing model), `border-conflict-style-*`,
`border-conflict-element-*`, `collapsing-border-model-*`, `border-collapse-*`,
`groove-default` / `ridge-default`, the per-side color tests, plus a share of css1/.

Some failures that look like border tests are not (to fix in passing, not part of this plan):
`border-width-005..014` and `border-*-018` are `inherit` on the border shorthands;
`border-color-001`, `border-width-shorthand-00x` fail in their references (an `img` floated right
after an inline `img` lands in the wrong place).

## How browsers draw borders

Blink (`BoxBorderPainter`), Gecko similar:

- **Per side**: width, style, color.  `currentColor` when no color is given.
- **Joins**: each side owns the trapezoid between the outer and inner edges, cut at the corners
  by the line from the outer corner to the inner corner.  When all four sides have the same
  color and style, the border is drawn as one ring (no visible join).
- **double**: outer third and inner third of the width (rounded), gap between.
- **groove / ridge**: the width split in two halves; groove is an inset outer half and outset inner
  half, ridge the reverse.
- **inset / outset**: top and left sides darker (inset) or bottom and right darker (outset).
  "Darker" is Blink's `Color::Dark()`: black becomes rgb(84, 84, 84); otherwise each channel is
  multiplied by max(0, (v - 0.33) / v), v = max channel / 255.
- **dashed**: dashes about 3 widths long with gaps adjusted so the side starts and ends with a
  dash (the exact lengths to be matched against Blink's `StrokeData` dash pattern when step 3
  starts); corners stay solid.
- **dotted**: round dots of the width's diameter, spaced to fit whole dots.

## Design

### Style (css_layout.h / .c)

- Per side: `border_color[4]` as packed RGBA (uint32, 0 = currentColor not set: a
  `border_color_set` bit per side), `border_style[4]` (enum: none, hidden, solid, dashed, dotted,
  double, groove, ridge, inset, outset).  Widths stay as they are.  Packed to keep LayoutStyle (and
  every style memcmp and cache entry) small: 20 bytes instead of 4 doubles per side.
- `border_styled` becomes derived (style not none / hidden); everything that reads it keeps
  working.
- Parsing: `border-*-color`, `border-color` (1-4 values), `border-*-style`, `border-style`
  (1-4), the `border` / `border-<side>` shorthands set all three; `inherit` / `initial` for every
  border property, the shorthands included (the `border-width: inherit` tests).
- currentColor resolved at paint time per side, as now for the single color.

### Painting (display_list.c)

Uniform borders stay plain rectangles (no mask, no extra GPU pass).  Where sides differ, each side
is drawn through a new mask kind, so its diagonal joins are antialiased as in Blink:

- **Mask kind 6, border side** (DisplayMask: the border box, border[4] widths, the side, the band
  of the width it covers as two fractions, rx/ry for rounded boxes): coverage is the side's
  trapezoid (outer edge, inner edge, cut by the lines from each outer corner to the matching inner
  corner), intersected with the ring of kind 4 when the box is rounded.  Implemented in the CPU
  raster (raster_cpu.c mask_alpha_at), the GPU mask shader (mask.frag) and its upload
  (gpu_vk.c), with the wire mask (ipc/gpu_wire.h), its validation (gpu_input / gpu_decode) and
  their proofs (`tools/prove.sh gpu gpu_input`) extended for the new kind.
- **Uniform border** (all sides one color and style, the common case): drawn as today.
- **Sides that differ**: one masked rectangle per side (the side's bounding box in its color).
- **double / groove / ridge / inset / outset**: bands of a side, each a kind 6 mask with that band's
  fractions, in the side's color or its darker variant.
- **dashed / dotted**: dashes as rectangles along the side (masked at the corners); dots as kind 3
  rounded rectangles (circles), or squares under 3px.
- `paint_pieces` (split inline boxes) goes through the same per-side code.
- A mask costs a save and a blend on the GPU: only boxes whose sides differ, or with a non-solid
  style, pay for it.

### The collapsing model (css_layout.c, table code)

After `table_build` places the cells, for a table with `border-collapse: collapse`:

1. **Conflict resolution** (CSS 2.1 section 17.6.2.1) for every grid edge: the candidates are the
   borders of the cells on either side, the rows / row groups / columns / column groups whose edge
   it is, and the table at the outer edges.  `hidden` wins outright; then the wider; then the style
   by rank (double, solid, dashed, dotted, ridge, outset, groove, inset); then cell over row over
   row group over column over column group over table; then the left / top one.
2. **Widths for layout**: each cell's border on a side is half the resolved edge (CSS 2.1 section
   17.6.2: the row width equation); the table's own border is half its outer edges'.
3. **Painting**: cells paint the resolved edges (their half and the neighbour's half is the full
   line centered on the grid line); rows, row groups and columns paint none of their own.

The cascade's border values stay as computed: the specified borders are kept in the style
(`border_spec_*`, written by the style pass), and table layout writes the used ones into
`border_*` each time it lays out the table.  A style slot reused on a kept tree therefore never
halves an already halved width, and the kept-tree check (fresh layout vs kept) compares like with
like.

## Steps (each lands on its own, with a full css/CSS2 run: no page lost)

1. **Per-side colors and styles in the style**, painted solid with today's rectangle joins;
   `inherit` / `initial` on all border properties.  Expected: the per-side color tests,
   `border-*-inherit`, part of css1.
2. **Corner joins** (mask kind 6, wire and proofs included) and the **double / groove / ridge / inset / outset** bands.
   Expected: `groove-default`, `ridge-default`, the per-side color tests with diagonal corners.
3. **dashed / dotted**.
4. **The collapsing border model**: conflict resolution, half widths, painting by cells.
   Expected: `border-*-applies-to-001..006`, `border-conflict-*`, `collapsing-border-model-*`,
   `border-collapse-*`.

## Checks

- Full css/CSS2 runs after each step, compared with the last (`cmp2.js`): no page lost.
- Unit tests: border parsing (1-4 values, shorthands, inherit), the darker-color function against
  Blink's values, conflict resolution order.
- Replays: YouTube and Wikipedia screenshots before and after (uniform borders must not change;
  Wikipedia's infobox uses `border-collapse: collapse`, so step 4 will move it: compared against
  how Chrome draws it); the kept-tree check (CYBER_LAYOUT_KEEP_VERIFY) on both.
- Replay gate A/B: the style struct grows by about 20 bytes; style pass time must not move.

## Risks

- LayoutStyle size and the style cache: packed fields keep the growth small.
- Collapsing model on real pages (Wikipedia tables): a visible change, to be compared with Chrome.
- Masks on many boxes (pages with per-side colored borders everywhere): one GPU save and blend
  per masked side; to be measured on the replays, batching masks of one box if needed.
- The wire format changes (a mask kind): the proofs are redone in the same step.
