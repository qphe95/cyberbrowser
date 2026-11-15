#!/bin/bash
# full.sh [port] [lines]: the full css reftest run into results/ref<time>; prints the summary, the changes against the
# base run ($STATE/last_ref.txt, if any) and the pass rate, then RUN=<run>.  Make a run the new base with rebase.sh.
# Keeps the base, this run and the one before (a run is ~37k shots, ~840 MB); keeps the last three subset runs.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$REPO"
RN=ref$(date +%s)
B=$(cat "$STATE/last_ref.txt" 2>/dev/null)
REFTEST=1 WPT_FAILS_MAX=30000 PORT=${1:-38420} timeout ${TO:-1500} bash tests/conformance/wpt_test.sh "$RUN" "$WPT" "$RES/$RN" css > "$RES/$RN.log" 2>&1
tail -3 "$RES/$RN.log" | cut -c1-150
[ -n "$B" ] && WON=1 node "$T/rcmp.js" "$(win "$RES/$B")" "$(win "$RES/$RN")" | sed 's/ match .*//' | cut -c1-120 | head -${2:-60}
node "$T/reffails.js" "$(win "$RES/$RN")" 1 "" 1 | head -1
echo $RN > "$STATE/last_full.txt"
echo RUN=$RN
cd "$RES" && ls -d ref[0-9]*/ 2>/dev/null | sed 's|/||' | sort | head -n -2 | while read d; do [ "$d" != "$B" ] && rm -rf "$d" "$d.log"; done
cd "$RES/sub" && ls -d refp*/ 2>/dev/null | sed 's|/||' | sort | head -n -3 | while read d; do rm -rf "$d" "$d.log"; done
