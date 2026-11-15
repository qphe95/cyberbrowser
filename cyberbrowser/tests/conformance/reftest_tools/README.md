# Reftest tools

Wrappers around `../wpt_test.sh` (the runner, with `../wpt_server.js` serving the WPT tree and
`../reftest_compare.js` comparing shots) for the CSS reftest loop: build, run a subset, compare
with a base run, look at the differences, bank a full run.

Paths come from `common.sh`: `CB_CONFORMANCE` (default `~/cb_conformance`) holds `run-s/` (the
browser binaries), `wpt/` (a WPT checkout) and `results/`; `REFTEST_STATE` (default
`$CB_CONFORMANCE/state`) keeps the base run's name and its failing list.

| Script | What it does |
| --- | --- |
| `bld.sh [build dir]` | Builds the browser and copies the binaries into `run-s/`. |
| `rpt.sh <paths>...` | Runs a reftest subset (directories or files) and prints `R=<run>`. |
| `full.sh [port]` | Runs every css reftest, compares with the base run and prints the pass rate and `RUN=<run>`. |
| `rebase.sh <run>` | Makes a full run the base and writes `allfail.txt`. |
| `node rcmp.js <base> <run>` | Lists what a run won and lost against a base (`WON=1` lists wins too). |
| `node reffails.js <run> [depth]` | Shows the pass rate and failures by directory, with how badly they fail. |
| `node fl.js <run>` | Lists failing tests as `pixels/max difference`. |
| `node refview.js <run> <test> out.png` | Writes test, reference and difference side by side (`CROP=x,y,w,h`, `SCALE=n`). |
| `wp.sh <probe.html>...` | Runs testharness probe pages and prints their messages. |
| `rects.sh <css test>...` | Prints a test page's element boxes from the real browser. |
| `shot.sh [out] < bodies` | Renders probe bodies (separated by `====`) against the green square. |

A typical loop:

```bash
bash bld.sh && bash rpt.sh css/css-forms
node rcmp.js "$CB/results/$(cat $STATE/last_ref.txt)" "$CB/results/sub/<run>"
bash full.sh && bash rebase.sh <run>
```

Run one browser job at a time: building or probing during a full run disturbs it, and the WPT tree must
not change during a run.
