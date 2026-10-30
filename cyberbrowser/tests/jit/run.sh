#!/bin/bash
# Differential test of the JIT (JIT_DESIGN.md section 10): run each
# tests/jit/*.js through CYBER_PROBE_JS with QJS_JIT=0 and with each mode
# given (default: stress), and compare what they print.  A mode is QJS_JIT's
# value, or t2 / t2stress for the optimizer (QJS_T2=1 with low thresholds,
# QJS_T2=stress), interpreting otherwise.
#   tests/jit/run.sh <cb_harness.exe> [mode...]
exe=$1; shift
modes=${*:-stress}
dir=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
fail=0
mode_env() {
    case "$1" in
        t2) echo "QJS_JIT=0 QJS_T2=1 QJS_T2_THRESHOLD=1000 QJS_T2_MIN_CALLS=0 QJS_FEEDBACK_THRESHOLD=100" ;;
        t2stress) echo "QJS_JIT=0 QJS_T2=stress" ;;
        *) echo "QJS_JIT=$1" ;;
    esac
}
for js in "$dir"/*.js; do
    name=$(basename "$js" .js)
    (cd "$tmp" && QJS_JIT=0 CYBER_PROBE_JS="$js" "$exe" 2>&1 | sed -n '/JITTEST-BEGIN/,/JITTEST-END/p' > "$name.0")
    if [ ! -s "$tmp/$name.0" ]; then echo "FAIL $name: no output with QJS_JIT=0"; fail=1; continue; fi
    for m in $modes; do
        (cd "$tmp" && env $(mode_env "$m") CYBER_PROBE_JS="$js" "$exe" 2>&1 | sed -n '/JITTEST-BEGIN/,/JITTEST-END/p' > "$name.$m")
        if cmp -s "$tmp/$name.0" "$tmp/$name.$m"; then
            echo "ok   $name ($m, $(($(wc -l < "$tmp/$name.0") - 2)) lines)"
        else
            echo "FAIL $name ($m):"; diff "$tmp/$name.0" "$tmp/$name.$m" | head -20; fail=1
        fi
    done
done
rm -rf "$tmp"
exit $fail
