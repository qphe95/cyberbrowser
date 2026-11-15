#!/bin/bash
# wp.sh <probe.html>...: run testharness probe pages through the real browser; prints each page's result and messages.
# A probe: a testharness page whose failure message carries what it logs (assert_unreached(text)).
# The pages are copied into wpt/css/zzprobe for the run and removed after.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
mkdir -p "$WPT/css/zzprobe"
fs=
for f in "$@"; do cp "$f" "$WPT/css/zzprobe/"; fs="$fs css/zzprobe/$(basename "$f")"; done
R=wp$(date +%s)
cd "$REPO"
WPT_KEEP_RUNS=0 SHARDS=1 WPT_FAILS_MAX=3000 WPT_TIMEOUT_MULT=${MULT:-0.3} PORT=${PORT:-38950} bash tests/conformance/wpt_test.sh "$RUN" "$WPT" "$RES/$R" $fs > "$RES/$R.log" 2>&1
node -e "const r=JSON.parse(require('fs').readFileSync(process.argv[1]+'/results.json','utf8'));for(const k in r){console.log('##',k,r[k].status,r[k].pass,r[k].fail);for(const f of r[k].fails||[])console.log(f)}" "$(win "$RES/$R")"
rm -rf "$RES/$R" "$RES/$R.log" "$WPT/css/zzprobe"
