#!/bin/bash
# The WebGL conformance suite and the WebGPU CTS in the browser (SANDBOX_DESIGN.md
# M8). The suites aren't in the tree: clone them (shallow) into one folder first,
#   git clone --depth 1 https://github.com/KhronosGroup/WebGL.git <suites>/webgl
#   git clone --depth 1 -b gh-pages https://github.com/gpuweb/cts.git <suites>/cts
#
#   tests/conformance/conformance_test.sh <build dir> <suites> <webgl1|webgl2|webgpu>
#       <results dir> [port=38526] [filter]
#
# A run picks up where results.json in <results dir> left off; it prints and
# writes summary.txt at the end. WebGL pages get 30 s each, WebGPU spec files 5
# minutes (and 30 s a case).
build=$(cd "$1" && pwd)
suites=$(cd "$2" && pwd)
suite=$3
out=$4
P=${5:-38526}
filter=$6
dir=$(cd "$(dirname "$0")" && pwd)
case $suite in
    webgl1|webgl2) src=$suites/webgl; page_s=30 ;;
    webgpu) src=$suites/cts; page_s=300 ;;
    *) echo "suite: webgl1, webgl2 or webgpu"; exit 2 ;;
esac
mkdir -p "$out"
out=$(cd "$out" && pwd)
rm -f "$out"/step-*.flag "$out"/pages.txt

node "$dir/conformance_server.js" "$suite" "$src" "$P" "$out" "$filter" > "$out/server.log" 2>&1 &
server=$!
for i in $(seq 50); do [ -s "$out/server.log" ] && break; sleep 0.2; done
head -1 "$out/server.log"

# Each page by its URL, then its answer (or its time up and 10 s more).
W=$(cygpath -m "$out")
{
    echo "open about:blank"
    while read -r k url; do
        echo "go $url"
        echo "waitfile $W/step-$k.flag $(((page_s + 10) * 1000))"
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
    if (x.status === "done" && x.fail === 0) clean++;
}
const s = `pages ${pages}: ${clean} with no failure; ` + Object.entries(by).map(e => e.join(" ")).join(", ") +
    `\nchecks ${pass}/${total} passed (${(100 * pass / Math.max(total, 1)).toFixed(1)}%)\n`;
process.stdout.write(s);
require("fs").writeFileSync(process.argv[1] + "/summary.txt", s);
' "$out"
