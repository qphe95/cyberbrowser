# Fonts and glyphs: typefaces, strikes and one glyph atlas

Status: plan.  Replaces the per-size `TextShaper` copies and per-slot glyph atlases with the
layering browsers use.

## Why

Today one `TextShaper` (src/text_shaper.c) is a font file at a size *and* its metrics, kerning,
glyph rasterizer and its own glyph atlas.  Other sizes are further shapers (`ts_sized`, sizes
quantized to 0.25 px), each with its own atlas and a font-registry slot (src/font_registry.c,
`add_sized_slot`).  Glyph commands carry that slot (`DisplayGlyph.font_slot`), and the GPU process
keeps one atlas texture per slot (`TG_ATLAS`, `GPU_FONT_SLOTS` = 255).

Fixed limits with no way back:

- at most 16 sizes per font (`TS_SIZED_MAX`), at most 255 slots in all, never freed;
- past either, text is drawn by scaling another size's glyph bitmaps: slightly different, blurrier
  text, depending on what the process drew before.

A tab process now lives across same-site navigations (CYBER_TAB_REUSE), so this is reached in
ordinary browsing, and it already shows up as order-dependent reftest failures (e.g.
css/CSS2/visudet/inline-block-baseline-011 passes alone, fails after the ::first-letter tests,
which use many sizes).  Each size also holds a whole atlas whether it draws 3 glyphs or 300.

## How browsers do it

Chrome (Blink + Skia; Firefox is similar):

1. **Typeface**: a font file / face, matched from CSS family, weight, style, stretch; fallback per
   character.
2. **Font**: a value, typeface + size + synthetic bold/italic + rendering settings.  Its metrics
   (ascent, descent, line gap rounded to whole pixels) are cached per font.  A new size costs
   nothing.
3. **Shaping** (HarfBuzz): text run -> glyph ids and positions, cached per font and text.  Never
   touches glyph images.
4. **Strike cache** (Skia `SkStrikeCache`): one strike per (typeface, size, transform, settings)
   holding glyph metrics and images, LRU-evicted under a memory budget.
5. **One GPU glyph atlas**: glyph images of every font and size packed into a few large texture
   pages, regions allocated and evicted by last use, glyphs rasterized again when needed.
6. Across processes, paint commands name glyphs (typeface, size, glyph id, position); nothing
   refers to a fixed slot.

## Design

Our constraint (SANDBOX_DESIGN.md section 5): the GPU process never parses fonts.  The tab
shapes and rasterizes; it uploads glyph masks to the GPU process's atlas for that tab.  So the
atlas is managed in the tab, and the GPU process mirrors its pages.

### Layers (tab side)

- **Typeface** (font_registry): the face data (file view or web font bytes) and face index, its
  family/weight/style, unscaled metrics and kerning.  Immutable once loaded; the registry holds
  typefaces only (no sizes).  Metric and kerning tables stay lock-free as now
  (PARALLEL_STYLE_LAYOUT.md section 8): layout threads measure.
- **Font** (value type `CbFont`): typeface id, size in 1/4 px, synthetic bold.  Measurement and
  shaping take a `CbFont` (today's `text_shaper_measure` / `wrap_*` / `shape_*`, minus the
  `scale` argument: the size is the font's).  Metrics at the exact size, from outlines.
- **Strike**: per (typeface, size, bold), created on first draw: the stb_truetype scale and, per
  glyph drawn, its atlas entry (page, rect, bearing) or "evicted".  A hash of strikes; no cap.
- **Glyph atlas** (new src/glyph_atlas.c): A8 pages of 1024 x 1024 (as many as needed up to a
  budget: 4 pages = 4 MB per tab to start), shelf allocation.  Each entry is stamped with the
  frame that last used it.  Dirty rectangles per page for upload.

### Drawing a glyph

`display_list_add_glyph` gets (page, uv rect) from the strike: present -> stamp it with the frame
being built; absent -> rasterize at the exact size, allocate, stamp, mark dirty.  Allocation that
fails evicts: whole shelves whose entries all have a stamp older than the last frame sent (so the
GPU never shows a frame whose glyphs were overwritten under it), least recently used first; then
a new page if under budget.  A frame that alone needs more than the budget draws the remaining
glyphs from a smaller strike scaled (today's fallback, now only in that extreme).

### Wire and GPU process

Step 1 keeps the protocol: a page is sent as a "slot" (`TG_ATLAS` rows of page k, `WIRE_GLYPH`
ref = k), with k < 16 well inside `GPU_FONT_SLOTS`.  So `gpu_input` / `gpu_decode` and their
proofs (`tools/prove.sh gpu gpu_input`) are unchanged at first.  Step 4 renames slot -> page and
narrows the bound, with the proofs redone.

CPU rasterization (src/raster_cpu.c, raster_page.c) reads the same pages.

## Steps (each lands on its own, tested)

1. **Atlas and strikes, pages as slots.**  glyph_atlas.c (pages, shelves, dirty rects, stamps);
   strikes in text_shaper.c keyed by (face, size, bold) drawing into it; `ts_sized`, per-shaper
   atlases and the slot allocator go; the registry's slots become typefaces plus atlas pages.  No
   eviction yet (pages grow to a high cap).  Unit tests: allocator, strike lookup.
2. **Eviction**: frame stamps, shelf eviction past the last sent frame, the budget, the extreme
   fallback.  Unit tests: eviction order, never evicting the last sent frame's glyphs; a stress
   page drawing 200 sizes stays crisp (compared with drawing them alone).
3. **`CbFont` through layout**: measurement and shaping take a font value instead of (shaper,
   scale); the 16 px base shaper and `scale` arithmetic leave css_layout.c and display_list.c.
4. **Protocol cleanup**: `font_slot` -> `atlas_page`, `GPU_FONT_SLOTS` -> `GPU_ATLAS_PAGES`;
   contracts in gpu_input.h / gpu_decode.h and their proofs updated.
5. Later, separately: vertical hinting or at least whole-pixel alignment of vertical stems (the
   Ahem edge rows), per-character fallback review, HarfBuzz-level shaping.

## Checks

- Unit tests (build-release/tests/cyberbrowser-tests.exe) for the atlas and strikes.
- Reftests: full css/CSS2 runs (`REFTEST=1 ... wpt_test.sh ... css/CSS2`); the order-dependent
  text failures should go, and a run in reverse page order should give the same results.
- Replay gate A/B against the previous commit (CPU per page, node counts) and tab memory.
- Screenshots of the YouTube and Wikipedia replays, before and after.

## Risks

- Every text path (measure, wrap, shape, display list, CPU raster, GPU upload) changes in step 1;
  keeping metrics and positions bit-identical for the same sizes (only rasterization moves) is the
  check: reftest results must not move except where scaled glyphs were drawn before.
- Eviction racing presentation: guarded by never evicting glyphs of the last frame sent.
- Atlas uploads: many small dirty rects per frame; batch per page per frame.
