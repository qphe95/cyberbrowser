#!/bin/bash
# Measure recorded page loads (tests/jit/replays.txt) under several settings
# (JS_PERF_DESIGN.md section 4).  Each run reports the JS thread's CPU cycles
# until the page settled (the "page settled" mark), its CPU time and the DOM
# node count.  The page runs on the deterministic clock with fetches done
# inline (CYBER_DET_TIME=1 CYBER_SYNC_FETCH=1; REAL=1: real time), so every
# run does the same work.  Settings and recordings take turns within each
# round, the settings in a rotated order, so drift in the machine's load and
# the order runs come in affect them alike.  tools/replay_stats.js
# then reports, per recording, each setting's median and its change from
# the first setting with a 95% confidence interval.
#
#   tests/jit/replay.sh <cyberbrowser.exe> <replay root> [runs=5] [setting...]
#
# A setting is QJS_JIT's value, optionally with a threshold and more
# environment variables: 0, 1, 1@20000, 1+QJS_JIT_IC=0+CYBER_SPIN_PCT=1.
# REPLAYS="yt-home wiki-us" limits the recordings; COUNTERS=1 adds one run
# per recording and setting with CYBER_COUNTERS=1 and prints its counters
# side by side; KEEP=<file> keeps the raw results.
exe=$1; root=$2; runs=${3:-5}
shift 3 2>/dev/null
settings=${*:-"0 1"}
dir=$(cd "$(dirname "$0")" && pwd)
tools=$(cd "$dir/../../tools" && pwd)
tmp=$(mktemp -d)
results=${KEEP:-$tmp/results}
: > "$results"

recordings=()
while read -r name url; do
    case "$name" in ''|'#'*) continue ;; esac
    if [ -n "$REPLAYS" ] && [[ " $REPLAYS " != *" $name "* ]]; then continue; fi
    [ -d "$root/$name" ] || { echo "missing recording $root/$name" >&2; continue; }
    recordings+=("$name $url")
done < "$dir/replays.txt"

# run <recording> <url> <setting> [extra env...]: the run's output.
run() {
    local name=$1 url=$2 s=$3; shift 3
    local base=${s%%+*} extra=
    [ "$s" != "$base" ] && extra=${s#*+}
    local mode=${base%@*} thr=
    [ "$base" != "$mode" ] && thr=${base#*@}
    (cd "$tmp" && env CYBER_REPLAY="$root/$name" CYBER_TIMELINE=1 $det QJS_JIT=$mode ${thr:+QJS_JIT_THRESHOLD=$thr} \
        ${extra//+/ } "$@" "$exe" "$url" 2>&1)
}

det="CYBER_DET_TIME=1 CYBER_SYNC_FETCH=1 CYBER_REPLAY_OFFLINE=1"
[ -n "$REAL" ] && det=
read -r -a all_settings <<< "$settings"
n_settings=${#all_settings[@]}
for i in $(seq "$runs"); do
    for r in "${recordings[@]}"; do
        set -- $r
        for k in $(seq 0 $((n_settings - 1))); do
            s=${all_settings[$(( (k + i) % n_settings ))]}
            out=$(run "$1" "$2" "$s")
            mark=$(echo "$out" | grep 'page settled')
            cycles=$(echo "$mark" | sed -n 's/.*, *\([0-9.]*\) Mcycles.*/\1/p')
            cpu=$(echo "$mark" | sed -n 's/.*thread cpu *\([0-9.]*\) ms.*/\1/p')
            nodes=$(echo "$out" | grep -m1 'DOM nodes' | sed 's/.*: //')
            calls=$(echo "$mark" | sed -n 's/.*Mcycles, *\([0-9]*\) calls.*/\1/p')
            echo "run $1 $s $i ${cycles:-NA} ${cpu:-NA} ${nodes:-NA} ${calls:-NA}" >> "$results"
        done
    done
done
if [ -n "$COUNTERS" ]; then
    for r in "${recordings[@]}"; do
        set -- $r
        for s in $settings; do
            run "$1" "$2" "$s" CYBER_COUNTERS=1 | grep '^\[counters\]' | sed "s/^\[counters\]/counter $1 $s/" >> "$results"
        done
    done
fi
node "$tools/replay_stats.js" "$results" $settings
rm -rf "$tmp"
