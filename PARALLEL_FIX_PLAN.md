# Parallel Wikipedia Rendering-Fix Plan (worktrees + subagents)

## Goal
Bring CyberBrowser's rendering of the English Wikipedia Main Page (viewport 1024x2400)
to pixel-parity with headless Chrome, by farming independent bug clusters to subagents,
each working in its own git worktree, then merging through a verified gate.

## Hard constraints (read first)

1. **Subagent model is not selectable from here.** My Agent tool only exposes agent
   *types* (`coder`, `explore`, `plan`). I cannot request "Deepseek v4 flash"; each
   subagent runs on whatever model this runtime is configured with. The plan works
   regardless of model because every task ships with exact commands and pass/fail gates.
2. **Subagents run foreground-only in this runtime (60-min cap each).** They will be
   launched one at a time, not concurrently. Wall-clock parallelism is not available
   from this session; worktree isolation still gives clean separation, independent
   builds, and independent verification. (If you later run multiple client instances,
   each can take one worktree from this plan unchanged.)
3. **Wikipedia content rotates during the day** (DYK several times/day, TFA/TFP/OTD at
   00:00 UTC) and our browser re-fetches live on every run. Without pinning, agents
   chase a moving target and "fixes" can't be compared. **All work uses a frozen
   fixture**, below.
4. **All rendering bugs funnel through `css_layout.cpp` / `text_shaper.cpp`.** Merge
   conflicts are inevitable if two agents touch the same function. Mitigation:
   bug-partitioned briefs, "add helpers, don't rewrite shared code" rule, and a
   sequential coordinator merge with a build+MAE gate after every patch.

## Step 0 — Coordinator prep (me, ~15 min)

1. Commit current good state (hyphen breaking + hlist separators + table work,
   MAE 0.0767) as the baseline branch point: `git add -A && git commit -m "baseline:
   wikipedia MAE 0.0767"`.
2. Build the **frozen fixture** in `fixtures/wiki-2026-07-31/`:
   - `page.html` — today's Main Page HTML (copy of `build-mingw/page_final.html`,
     with `href="/`/`src="/` rewritten to absolute `https://en.wikipedia.org/`).
   - `chrome_ref.png` — headless Chrome screenshot of **that file** via
     `file:///` (banner-free, deterministic), resized to exactly 1024x2400.
   - `chrome_dom.txt` — Chrome DOM measurements of every key section
     (mp-tfa/dyk/itn/otd/tfl/tfp/footer: x/y/w/h + font metrics) via the existing
     probe pattern (`probes/run_measure19.py` pointed at the fixture).
   - `truth.md` — table of Chrome truth values for agents to check against.
3. Verify fixture determinism: `CYBER_LAYOUT_HTML=fixtures/.../page.html` produces
   identical layout twice; document this in the fixture README.
4. Create worktrees (119 MB build dir is *not* copied; each worktree configures its
   own build with `cmake -G "MinGW Makefiles"` + `mingw32-make -j8 cyberbrowser`,
   first build a few minutes):
   ```
   git worktree add ../cb-wt-thumbs   -b fix/thumb-captions
   git worktree add ../cb-wt-inline   -b fix/inline-overflow
   git worktree add ../cb-wt-header   -b fix/header-band
   git worktree add ../cb-wt-gencontent -b fix/gencontent-polish
   git worktree add ../cb-wt-bottom   -b fix/bottom-sections
   ```

## Agent assignments (one coder subagent each, sequential launch)

Every brief includes: repo path (its worktree), fixture paths, build/run commands,
the exact bug(s) with measured Chrome-vs-ours numbers, a "do not touch" list, and
the acceptance gate. No agent may edit the fixture or another worktree.

### Agent A — Thumbnail boxes & captions  (`fix/thumb-captions`)
Bugs (measured 2026-07-31):
- OTD Lafayette caption text **overflows its box and overlaps the following list
  text** ("Marquis de Lafayette" drawn over "instructions from Adolf Hitler,").
  Caption box height is not grown to fit wrapped caption lines.
- DYK "Statue in John Paul II Park" wraps as "John Paul / II Park"; Chrome wraps
  "John / Paul II Park" → caption line width is ~15px too narrow.
Files: `css_layout.cpp` (thumb/float/caption sizing). Gate: caption boxes size to
content, no overlap in dump; section MAE 800-1200 improves; no regression elsewhere.

### Agent B — Inline overflow at line start  (`fix/inline-overflow`)
Bug: OTD date line — run `" (Sovereignty Restoration Day)"` starts mid-line at
x=968.23 with w=233.83 (column right edge ≈991): it **overflows ~210px and is
visually truncated** ("eignty Restoration Day)" disappears). Chrome wraps the whole
run to the next line. Root cause: in the block-flow inline branch, when a run's
first word doesn't fit the sliver the run must move to the next line; here it stays
(hyphen-fix regression candidate — check the new first-word hyphen branch first).
Files: `css_layout.cpp` inline branch, `text_shaper.cpp`. Gate: dump shows the run
wrapped whole to y=878.x; OTD band MAE improves; hyphen probe still splits
"comic-book" correctly (regression test included in brief).

### Agent C — Header band 0–200  (`fix/header-band`)
Bugs: "Main menu"/"Tools" render as visible bordered boxes overlapping the Read/
View tabs; search box overlapped by "Donate Create account" links; Wikipedia logo
wordmark missing. Suspect: header flex items with auto basis measuring only text
(images not counted), plus `display:none`/visibility rules for menu labels not
applied. Files: `css_layout.cpp` flex pass, `css_parser.cpp` if selector gaps found.
Gate: band 0-200 MAE improves from 0.041; welcome banner (139-270) unchanged ±0.5px.

### Agent D — Generated-content polish  (`fix/gencontent-polish`)
Bugs: two hlist footers still lack separators — "More featured articles About"
(tfa-footer) and "More current events Nominate an article" (itn-footer). The
whitespace-sibling injection path misses these (likely minified `</li><li>` or the
fallback path failing); separators should also render **bold**. Files:
`css_layout.cpp` generated-content injection only. Gate: all six footers match
Chrome text; fixture screenshot crops pixel-compared.

### Agent E — Bottom sections  (`fix/bottom-sections`)
Scope: TFL block (mp-middle/mp-tfl, present today), TFP caption row
("Map credit: ..." alignment), and everything below y=2400 (sister projects,
other areas, languages — currently wrong enough to matter if the viewport grows).
Files: `css_layout.cpp` serial/table layout. Gate: TFL/TFP bands 1600-2400 MAE
improves or holds; dump positions within 1px of `chrome_dom.txt` values.

## Verification gates

- **Per agent (before returning):** builds clean; its section dump values within 1px
  of fixture `truth.md`; its section crop MAE (ImageMagick compare of fixture crops)
  improved; `git diff` limited to allowed files; produces `patch.diff` + report.
- **Coordinator (me, after each merge):** apply one patch at a time to master,
  rebuild, run full pipeline against the **frozen** Chrome ref. Merge only if
  whole-page MAE does not regress (>0.002 tolerance) and the agent's claimed band
  improved. On failure: bounce the patch back with failing evidence.

## Execution order & rollback

1. Launch A (worst visual bug: overlapping text). 2. Merge-gate. 3. Launch B.
4. Merge-gate. 5. C, D, E likewise. Each merge is a separate commit; any gate
failure → `git reset --hard` to the pre-merge commit, agent retries with the
evidence. Final: full-day-stability check by re-capturing a live Chrome ref and
re-running MAE once.

## Expected outcome

Bands 800-1200 (currently 0.109-0.115) and 0-200 (0.041) carry the most recoverable
error; realistic landing zone is whole-page MAE ≈0.055-0.065 from 0.0767, plus
elimination of the two glaring visual bugs (overlapping caption, truncated date line).
