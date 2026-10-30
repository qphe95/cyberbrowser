#!/bin/bash
# Run Speedometer (JS_PERF_DESIGN.md section 4.3) in cyberbrowser under
# several settings: the whole benchmark, in one browser process per run, as a
# browser runs it.  Runs are interleaved and their order rotated each round,
# as in replay.sh.  tools/speedometer_stats.js then reports, per setting, the
# score and each suite's time (medians over the rounds) and the change from
# the first setting.
#   tests/jit/speedometer.sh <cb_harness.exe> <Speedometer dir> [iterations=5] [rounds=3] [setting...]
# A setting is interp (QJS_JIT=0), t1 (QJS_JIT=1), t2 (QJS_T2=1), or any
# space-free env assignments joined by commas (QJS_JIT=0,QJS_T2=stress).
# The default settings are interp t1 t2.  SPEEDOMETER_SUITES=A,B runs those
# suites only; SPEEDOMETER_PORT picks the server's port (18765);
# SPEEDOMETER_RESULTS=<file> keeps the raw results there.
exe=$1; dir=$2; iterations=${3:-5}; rounds=${4:-3}; shift 4 2>/dev/null
settings=${*:-interp t1 t2}
if [ ! -x "$exe" ] || [ ! -f "$dir/index.html" ]; then
    echo "usage: $0 <cb_harness.exe> <Speedometer dir> [iterations] [rounds] [setting...]" >&2
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
tools=$here/../../tools
port=${SPEEDOMETER_PORT:-18765}
tmp=$(mktemp -d)
results=$tmp/results
: > "$results"

# The checkout, served on localhost (the page loads its suites in frames).
node "$tools/speedometer_server.js" "$dir" "$port" > "$tmp/server.log" 2>&1 &
server=$!
trap 'kill $server 2>/dev/null; rm -rf "$tmp"' EXIT
for i in $(seq 50); do grep -q listening "$tmp/server.log" && break; sleep 0.1; done
if ! grep -q listening "$tmp/server.log"; then echo "server did not start:" >&2; cat "$tmp/server.log" >&2; exit 1; fi

setting_env() {
    case "$1" in
        interp) echo "QJS_JIT=0" ;;
        t1) echo "QJS_JIT=1" ;;
        t2) echo "QJS_JIT=0 QJS_T2=1" ;;
        *) echo "$1" | tr ',' ' ' ;;
    esac
}
query="startAutomatically=true&iterationCount=$iterations"
[ -n "$SPEEDOMETER_SUITES" ] && query="$query&suites=$SPEEDOMETER_SUITES"
url="http://127.0.0.1:$port/index.html?$query"
prelude=$(cd "$tools" && pwd -W 2>/dev/null || pwd)/speedometer_prelude.js

set -- $settings
n=$#
for round in $(seq "$rounds"); do
    for k in $(seq 0 $((n - 1))); do
        idx=$(( (k + round - 1) % n + 1 ))
        s=${!idx}
        start=$(date +%s)
        out=$(cd "$tmp" && env $(setting_env "$s") CYBER_PRELUDE_JS="$prelude" CYBER_RUN_UNTIL=__cyberDone \
              CYBER_SETTLE_MS=${SPEEDOMETER_TIMEOUT_MS:-3600000} "$exe" "$url" 2>&1)
        line=$(echo "$out" | grep -m1 -E '^\[E/console\] SPEEDOMETER-(DONE|ERROR)' | sed 's/^\[E\/console\] //')
        secs=$(( $(date +%s) - start ))
        if [ "${line%% *}" = SPEEDOMETER-DONE ]; then
            echo "$s $round ${line#SPEEDOMETER-DONE }" >> "$results"
            score=$(echo "${line#SPEEDOMETER-DONE }" | node -e "let s='';process.stdin.on('data',d=>s+=d).on('end',()=>console.log(JSON.parse(s).Score.mean.toFixed(2)))")
            echo "round $round $s: score $score (${secs}s)"
        else
            echo "round $round $s: FAILED (${secs}s) ${line:-no result}" | cut -c1-300
        fi
    done
done
[ -n "$SPEEDOMETER_RESULTS" ] && cp "$results" "$SPEEDOMETER_RESULTS"
node "$tools/speedometer_stats.js" "$results" $settings
