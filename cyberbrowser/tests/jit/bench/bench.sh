#!/bin/bash
# Time each benchmark in bench.js with the interpreter and the JIT.
#   tests/jit/bench/bench.sh <cyberbrowser.exe> [runs=3] [modes="0 1"] [bench...]
# Prints, per benchmark and mode, the best time of the runs in ms as the
# script measured it, the best wall time of the process (startup included)
# in parentheses, and the checksum (which must not depend on the mode).
exe=$1
runs=${2:-3}
modes=${3:-"0 1"}
shift 3 2>/dev/null
dir=$(cd "$(dirname "$0")" && pwd)
benches=${*:-"calls props closures arrays branches strings"}
tmp=$(mktemp -d)
for b in $benches; do
    { echo "globalThis.__bench = '$b';"; cat "$dir/bench.js"; } > "$tmp/$b.js"
    line="$b:"
    for m in $modes; do
        best=
        wall=
        for i in $(seq "$runs"); do
            s=$(date +%s%N)
            all=$(cd "$tmp" && QJS_JIT=$m CYBER_PROBE_JS="$tmp/$b.js" "$exe" 2>&1)
            e=$(date +%s%N)
            out=$(echo "$all" | grep '^\[E/console\] BENCH ' | sed 's/.*BENCH //')
            t=$(echo "$all" | grep '^\[E/console\] BENCHTIME ' | sed 's/.*=//')
            w=$(( (e - s) / 1000000 ))
            if [ -z "$best" ] || [ "$t" -lt "$best" ]; then best=$t; fi
            if [ -z "$wall" ] || [ "$w" -lt "$wall" ]; then wall=$w; fi
        done
        line="$line  jit=$m ${best}ms (${wall}) [$out]"
    done
    echo "$line"
done
rm -rf "$tmp"
