#!/bin/bash
# Frames from other sites, ORB, CORS and Private Network Access
# (SANDBOX_DESIGN.md M7), through the browser on the pages of server.js.
# 127.0.0.1 and localhost are two sites; port B is made public
# (CYBER_PNA_PUBLIC_PORTS) for the PNA run.
#
#   tests/sites/sites_test.sh <build dir> [port A=38517] [port B=38518]
build=$(cd "$1" && pwd)
A=${2:-38517}
B=${3:-38518}
dir=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
fails=0
expect() {      # expect <name> <file> <pattern>: a line matching pattern
    if grep -qE -- "$3" "$2"; then echo "ok   $1"; else echo "FAIL $1 (no /$3/ in $(basename "$2"))"; fails=$((fails + 1)); fi
}
never() {       # never <name> <file> <pattern>
    if grep -qE -- "$3" "$2"; then echo "FAIL $1 ($(grep -m1 -E -- "$3" "$2"))"; fails=$((fails + 1)); else echo "ok   $1"; fi
}
run() {         # run <script> [env...]: the pages' lines and the network process's log
    (cd "$build" && rm -rf browser_data logs && env "${@:2}" CYBER_UI_SCRIPT="$(cygpath -m "$1")" timeout 300 ./cb_coord.exe) \
        2> "$tmp/coord.txt"
    cat "$build"/logs/tab-*.log | grep "\[sites\]" > "$tmp/pages.txt"
    cp "$build/logs/net.log" "$tmp/net.txt"
}

node "$dir/server.js" "$A" "$B" &
server=$!
sleep 1

cat > "$tmp/s.txt" <<EOF
open http://127.0.0.1:$A/top
wait 5000
open http://localhost:$A/alone
wait 3000
open http://127.0.0.1:$A/orb
wait 4000
quit
EOF
run "$tmp/s.txt"
p="$tmp/pages.txt"
expect "the frame loads, with its own storage" "$p" "frame sees .* ls=frame"
expect "the frame can't reach its parent's document or element" "$p" "frame sees parent.document=(undefined|threw .*) frameElement=null"
expect "the page can't reach the frame's document" "$p" "top sees contentDocument=null document=undefined frame-ls=undefined"
expect "the frame's message reaches the page, from its origin" "$p" "top got \{\"hello\":\"from frame\",\"ls\":\"frame\"\} from http://localhost:$A source-is-frame=true"
expect "the page's reply reaches the frame" "$p" "frame got \{\"reply\":\"hi frame\"\} from http://127.0.0.1:$A"
never "a message for another origin is dropped" "$p" "frame got \{\"wrong\""
expect "an about:blank frame is the page's own origin" "$p" "blank frame document=reachable"
expect "the frame's site alone has none of its storage" "$p" "alone ls=null cookie=$"
expect "its storage is under the page's site" <(cd "$build/browser_data/sites" && find . -path "*partitions*" -type f) \
       "^./http\+127.0.0.1/partitions/http_localhost/local_storage/http_localhost_$A$"
expect "ORB blocks JSON across sites" "$p" "json-no-cors failed"
expect "ORB blocks HTML across sites" "$p" "html-no-cors failed"
expect "ORB lets scripts through" "$p" "script-no-cors read 19"
expect "CORS without Access-Control-Allow-Origin fails" "$p" "cors-without-acao failed"
expect "CORS with it works" "$p" "cors-with-acao read 6"
expect "the network process blocked with ORB" "$tmp/net.txt" "ORB: http://localhost:$A/data.json"

cat > "$tmp/p.txt" <<EOF
open http://127.0.0.1:$B/pna
wait 5000
quit
EOF
run "$tmp/p.txt" CYBER_PNA_PUBLIC_PORTS=$B
p="$tmp/pages.txt"
expect "a public page can't fetch from a local address" "$p" "public-to-local failed"
expect "it can fetch from a public one" "$p" "public-to-public read 6"
never "nor frame a local page" "$p" "frame sees"
expect "the network process blocked with PNA" "$tmp/net.txt" "Private Network Access: 127.0.0.1:$A"

kill $server 2> /dev/null
echo "$fails failed; logs in $tmp"
exit $((fails > 0))
