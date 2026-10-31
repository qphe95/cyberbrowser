#!/bin/bash
# web-platform-tests' testharness.js tests in the browser.  WPT isn't in the
# tree: a shallow, sparse clone of the parts to run will do, e.g.
#   git clone --depth 1 --filter=blob:none --sparse https://github.com/web-platform-tests/wpt.git <wpt>
#   git -C <wpt> sparse-checkout set css dom resources common fonts images
#
#   tests/conformance/wpt_test.sh <build dir> <wpt> <results dir> <dir or file under wpt>...
#
# PORT (default 38527) is the server's; WPT_TIMEOUT_MULT scales testharness's
# time limits (0.3: a test that hangs gives up after 3 s, not 10); with
# REFTEST=1, SHARDS (default 6) browsers run at once.  A run picks up where results.json in
# <results dir> left off; it prints and writes summary.txt at the end, and
# node tests/conformance/failures.js <results dir> lists what failed.
build=$(cd "$1" && pwd)
wpt=$(cd "$2" && pwd)
out=$3
shift 3
P=${PORT:-38527}
dir=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
rm -f "$out"/step-*.flag "$out"/pages.txt

node "$dir/wpt_server.js" "$wpt" "$P" "$out" "$@" > "$out/server.log" 2>&1 &
server=$!
for i in $(seq 100); do [ -s "$out/server.log" ] && break; sleep 0.2; done
head -1 "$out/server.log"

# Each page by its URL, then its answer (or its time up and 5 s more).  With
# REFTEST=1, reftest pages and their references in an 800x600 viewport, each
# saved (shotc) once it says it is ready or its time is up.
W=$(cygpath -m "$out")
script() {   # <pages file>: the UI script for its pages
    echo "open about:blank"
    [ -n "$REFTEST" ] && echo "viewport 800 600"
    while read -r k url ms shot; do
        echo "go $url"
        if [ -n "$shot" ]; then
            echo "waitfile $W/step-$k.flag $ms"
            echo "shotc $shot"
        else
            echo "waitfile $W/step-$k.flag $((ms + 5000))"
        fi
    done < "$1"
    [ -z "$REFTEST" ] && echo "go http://127.0.0.1:$P/end" && echo "wait 2000"
    echo "quit"
}
mkdir -p "$out/logs"
if [ -n "$REFTEST" ]; then
    # Reftest pages don't depend on one another: SHARDS browsers (default 6),
    # each from its own copy of the build (its own browser_data and logs),
    # take every SHARDS-th page.
    n=${SHARDS:-6}
    rm -f "$out"/pages-*.txt
    awk -v n="$n" -v o="$out" '{ print > (o "/pages-" (NR % n) ".txt") }' "$out/pages.txt"
    pids=
    for i in $(seq 0 $((n - 1))); do
        [ -s "$out/pages-$i.txt" ] || continue
        d="$out/shard-$i"
        mkdir -p "$d"
        cp "$build"/*.exe "$d/"
        cp "$build"/*.dll "$d/" 2> /dev/null
        [ -d "$build/shaders" ] && cp -r "$build/shaders" "$d/"
        script "$out/pages-$i.txt" > "$out/script-$i.txt"
        (cd "$d" && rm -rf browser_data logs/* && CYBER_UI_SCRIPT="$W/script-$i.txt" ./cb_coord.exe) 2> "$out/coord-$i.txt" &
        pids="$pids $!"
    done
    wait $pids
    for i in $(seq 0 $((n - 1))); do
        [ -d "$out/shard-$i/logs" ] || continue
        mkdir -p "$out/logs/shard-$i"
        cp "$out/shard-$i"/logs/*.log "$out/logs/shard-$i/" 2> /dev/null
    done
else
    script "$out/pages.txt" > "$out/script.txt"
    (cd "$build" && rm -rf browser_data logs/* && CYBER_UI_SCRIPT="$W/script.txt" ./cb_coord.exe) 2> "$out/coord.txt"
    cp "$build"/logs/*.log "$out/logs/" 2> /dev/null
fi
kill $server 2> /dev/null
[ -n "$REFTEST" ] && node "$dir/reftest_compare.js" "$out"

node -e '
const r = JSON.parse(require("fs").readFileSync(process.argv[1] + "/results.json", "utf8"));
let pages = 0, clean = 0, pass = 0, total = 0, by = {};
for (const k in r) {
    const x = r[k]; pages++;
    by[x.status] = (by[x.status] || 0) + 1;
    pass += x.pass; total += x.pass + x.fail;
    if (x.status === "OK" && x.fail === 0) clean++;
}
const s = `pages ${pages}: ${clean} with no failure; ` + Object.entries(by).map(e => e.join(" ")).join(", ") +
    `\nsubtests ${pass}/${total} pass (${total ? (100 * pass / total).toFixed(1) : 0}%)\n`;
process.stdout.write(s);
require("fs").writeFileSync(process.argv[1] + "/summary.txt", s);
' "$out"
