#!/bin/bash
# Run JetStream's plain-JS tests (Octane, the Web Tooling Benchmark and the
# rest; tools/jsbench.js makes the scripts) under several settings and
# compare them (JS_PERF_DESIGN.md section 4.3).  Each run is a fresh process;
# it reports every iteration's time, which become three numbers: the first
# iteration (startup), the mean of the last half (steady state) and the
# total.  Settings and tests take turns within each round, the settings in
# a rotated order, as in replay.sh.  tools/jsbench_stats.js then reports, per
# test and over all of them (geometric mean), each setting's change from the
# first with a 95% confidence interval.
#
#   tests/jit/jsbench.sh <cyberbrowser.exe> <scripts dir> [runs=3] [setting...]
#
# A setting is QJS_JIT's value with optional extra environment variables
# (0, 1, 1+QJS_JIT_THRESHOLD=20000); "<label>%" before it runs $EXE_<label>
# instead.  TESTS="richards acorn-wtb" limits the tests (default: every
# script in the directory); KEEP=<file> keeps the raw results; TIMEOUT
# (seconds, default 1200) bounds a run.  A failed run's output is kept as
# <results>.fail.<test>.<setting>.<round>.log.
exe=$1; dir=$2; runs=${3:-3}
shift 3 2>/dev/null
settings=${*:-"0 1"}
tools=$(cd "$(dirname "$0")/../../tools" && pwd)
tmp=$(mktemp -d)
results=${KEEP:-$tmp/results}
: > "$results"
tests=${TESTS:-$(ls "$dir"/*.js | xargs -n1 basename | sed 's/\.js$//')}

run() {
    local t=$1 s=$2
    local bin=$exe
    if [[ "$s" == *%* ]]; then
        local label=${s%%\%*} var
        var="EXE_$label"
        bin=${!var}
        s=${s#*%}
    fi
    local base=${s%%+*} extra=
    [ "$s" != "$base" ] && extra=${s#*+}
    (cd "$tmp" && timeout "${TIMEOUT:-1200}" env QJS_JIT=$base ${extra//+/ } CYBER_PROBE_JS="$dir/$t.js" "$bin" 2>&1)
}

read -r -a all_settings <<< "$settings"
n=${#all_settings[@]}
for i in $(seq "$runs"); do
    for t in $tests; do
        for k in $(seq 0 $((n - 1))); do
            s=${all_settings[$(( (k + i) % n ))]}
            out=$(run "$t" "$s")
            times=
            echo "$out" | grep -q '^\[E/console\] JSBENCH-DONE' &&
                times="[$(echo "$out" | grep '^\[E/console\] JSBENCH-ITER ' | sed 's/.*JSBENCH-ITER //' | paste -sd, -)]"
            if [ -z "$times" ]; then
                echo "bench $t $s $i NA NA NA" >> "$results"
                # keep the failed run's output (a crash's backtrace) next to the results
                echo "$out" > "$results.fail.$t.${s//[^A-Za-z0-9_=.-]/_}.$i.log"
                continue
            fi
            echo "$times" | node -e '
                let s = ""; process.stdin.on("data", d => s += d).on("end", () => {
                    const a = JSON.parse(s), h = a.slice(Math.floor(a.length / 2));
                    const sum = x => x.reduce((p, q) => p + q, 0);
                    console.log([a[0], sum(h) / h.length, sum(a)].map(x => x.toFixed(3)).join(" "));
                });' | sed "s/^/bench $t $s $i /" >> "$results"
        done
    done
done
node "$tools/jsbench_stats.js" "$results" $settings
rm -rf "$tmp"
