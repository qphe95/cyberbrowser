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
# REFTEST=1, SHARDS (default: one per CPU) browsers run at once.  A run picks up where results.json in
# <results dir> left off; it prints and writes summary.txt at the end, and
# node tests/conformance/failures.js <results dir> lists what failed.
build=$(cd "$1" && pwd)
wpt=$(cd "$2" && pwd)
out=$3
shift 3
P=${PORT:-38527}
dir=$(cd "$(dirname "$0")" && pwd)
# A page of the site a tab is on loads in its process (CYBER_TAB_REUSE=0:
# a process each, as the browser's default).
export CYBER_TAB_REUSE=${CYBER_TAB_REUSE-1}
[ "$CYBER_TAB_REUSE" = 0 ] && unset CYBER_TAB_REUSE
mkdir -p "$out"
out=$(cd "$out" && pwd)
rm -f "$out"/step-*.flag "$out"/pages.txt

# (each phase's time, printed at the end)
t0=$EPOCHREALTIME; times=
mark() { local now=$EPOCHREALTIME; times="$times $1 $(( (${now/./} - ${t0/./}) / 1000 )) ms,"; t0=$now; }
node "$dir/wpt_server.js" "$wpt" "$P" "$out" "$@" > "$out/server.log" 2>&1 &
server=$!
for i in $(seq 400); do [ -s "$out/server.log" ] && break; sleep 0.05; done
mark server
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
    # Reftest pages don't depend on one another: SHARDS browsers (default:
    # one per two CPUs, which measured best) take every SHARDS-th page
    # (reftest_prepare.js makes them ready).
    n=${SHARDS:-$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))}
    rm -f "$out"/pages-*.txt "$out"/script-*.txt
    node "$dir/reftest_prepare.js" "$build" "$out" "$W" "$n"
    mark prepare
    pids=
    for i in $(seq 0 $((n - 1))); do
        [ -s "$out/pages-$i.txt" ] || continue
        # (the first browser gives the UI sandbox the results folder before
        # the others start, rather than all of them at once)
        if [ "$i" = 1 ]; then
            for t in $(seq 200); do [ -s "$build/shard-0/logs/ui.log" ] && break; sleep 0.025; done
        fi
        (cd "$build/shard-$i" && CYBER_FRAME_IMAGE_WAIT=1 CYBER_RAF_MS=${CYBER_RAF_MS:-2} CYBER_UI_SCRIPT="$W/script-$i.txt" ./cb_coord.exe) \
            2> "$out/coord-$i.txt" &
        pids="$pids $!"
    done
    mark launch
    wait $pids
    mark browsers
    # Their logs, each folder renamed into the results (in one process: a
    # program started here costs tens of ms).
    node -e '
        const fs = require("fs"), [build, out, n] = process.argv.slice(1);
        for (let i = 0; i < +n; i++) {
            const from = `${build}/shard-${i}/logs`, to = `${out}/logs/shard-${i}`;
            if (!fs.existsSync(from)) continue;
            fs.rmSync(to, { recursive: true, force: true });
            fs.renameSync(from, to);
            fs.mkdirSync(from);
        }' "$build" "$out" "$n"
else
    script "$out/pages.txt" > "$out/script.txt"
    (cd "$build" && rm -rf browser_data logs/* && CYBER_UI_SCRIPT="$W/script.txt" ./cb_coord.exe) 2> "$out/coord.txt"
    cp "$build"/logs/*.log "$out/logs/" 2> /dev/null
fi
kill $server 2> /dev/null
mark logs
[ -n "$REFTEST" ] && node "$dir/reftest_compare.js" "$out" && mark compare

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
mark summary
echo "times:${times%,}"
