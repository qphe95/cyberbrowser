#!/bin/bash
# Time a recorded page load (CYBER_RECORD) replayed with each JIT setting:
# the JS thread's CPU time until the page settled, median of the runs, and
# the DOM node count (which varies a little from run to run).  The settings
# take turns within each round, so drift in the machine's load affects them
# alike.
#   tests/jit/replay.sh <cyberbrowser.exe> <replay dir> <url> [runs=5] [setting...]
# A setting is QJS_JIT's value, optionally with a threshold and more
# environment variables: 0, 1, 1@20000, 1+QJS_JIT_LOG=1+FOO=2.
exe=$1; dir=$2; url=$3; runs=${4:-5}
shift 4 2>/dev/null
settings=${*:-"0 1"}
tmp=$(mktemp -d)
for i in $(seq "$runs"); do
    for s in $settings; do
        base=${s%%+*}; extra=
        [ "$s" != "$base" ] && extra=${s#*+}
        mode=${base%@*}; thr=
        [ "$base" != "$mode" ] && thr=${base#*@}
        out=$(cd "$tmp" && env CYBER_REPLAY="$dir" CYBER_TIMELINE=1 QJS_JIT=$mode ${thr:+QJS_JIT_THRESHOLD=$thr} \
              ${extra//+/ } "$exe" "$url" 2>&1)
        cpu=$(echo "$out" | grep 'before page_final' | sed 's/.*thread cpu *//; s/ ms.*//')
        nodes=$(echo "$out" | grep -m1 'DOM nodes' | sed 's/.*: //')
        echo "$s $cpu $nodes" >> "$tmp/results"
    done
done
for s in $settings; do
    awk -v s="$s" '$1 == s { c[++n] = $2; d = d " " $3 }
        END { asort(c); printf "%s: median %s ms  [", s, c[int((n + 1) / 2)];
              for (i = 1; i <= n; i++) printf "%s%s", c[i], i < n ? " " : "";
              printf "]  nodes:%s\n", d }' "$tmp/results"
done
rm -rf "$tmp"
