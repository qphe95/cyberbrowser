#!/bin/bash
# web-platform-tests' testharness.js tests in the browser.  WPT isn't in the
# tree: a shallow, sparse clone of the parts to run will do, e.g.
#   git clone --depth 1 --filter=blob:none --sparse https://github.com/web-platform-tests/wpt.git <wpt>
#   git -C <wpt> sparse-checkout set css dom resources common fonts images
#
#   tests/conformance/wpt_test.sh <build dir> <wpt> <results dir> <dir or file under wpt>...
#
# PORT (default 38527) is the server's.  A run picks up where results.json in
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

# Each page by its URL, then its answer (or its time up and 5 s more).
W=$(cygpath -m "$out")
{
    echo "open about:blank"
    while read -r k url ms; do
        echo "go $url"
        echo "waitfile $W/step-$k.flag $((ms + 5000))"
    done < "$out/pages.txt"
    echo "go http://127.0.0.1:$P/end"
    echo "wait 2000"
    echo "quit"
} > "$out/script.txt"
(cd "$build" && rm -rf browser_data logs/* && CYBER_UI_SCRIPT="$W/script.txt" ./cb_coord.exe) 2> "$out/coord.txt"
mkdir -p "$out/logs"
cp "$build"/logs/*.log "$out/logs/" 2> /dev/null
kill $server 2> /dev/null

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
