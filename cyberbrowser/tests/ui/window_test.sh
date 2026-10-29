#!/bin/bash
# Windows, tab dragging and compositor scrolling (SANDBOX_DESIGN.md M6),
# through the UI's test script (CYBER_UI_SCRIPT):
#   - scrolling while the page's JS is busy: the GPU process moves the
#     content the tab drew last, before the tab can draw again;
#   - dragging a tab whose frames keep coming into a window of its own and
#     back: the frames keep coming, and its process stays the same;
#   - with a replay root, the same with the YouTube recording while its video
#     plays (replays/yt-dQw4w9WgXcQ).
#
#   tests/ui/window_test.sh <build dir> [replay root] [port=38517]
build=$(cd "$1" && pwd)
replays=$2
port=${3:-38517}
dir=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
W=$(cygpath -m "$tmp")
fails=0
check() {   # check <name> <value> <op> <limit>
    if awk "BEGIN { exit !($2 $3 $4) }"; then echo "ok   $1 ($2)"; else echo "FAIL $1 ($2, wants $3 $4)"; fails=$((fails + 1)); fi
}
run() {     # run <script> [env...]: the browser on it; the UI's log after
    (cd "$build" && rm -rf browser_data && env "${@:2}" CYBER_UI_SCRIPT="$(cygpath -m "$1")" timeout 300 ./cb_coord.exe) \
        2> "$tmp/coord.txt"
    cp "$build/logs/ui.log" "$tmp/ui.log"
}
starts() { grep -c "\] tab [0-9]* started" "$tmp/ui.log"; }

node "$dir/server.js" "$port" &
server=$!
sleep 1
cat > "$tmp/s.txt" <<EOF
open http://127.0.0.1:$port/busy
waitload 30000
shot $W/s0.bmp
wait 2500
log the page's JS is busy now
scroll 200
wait 300
shot $W/s1.bmp
wait 5000
shot $W/s2.bmp
open http://127.0.0.1:$port/anim
wait 2000
shot $W/t0.bmp
wait 600
shot $W/t1.bmp
dragout
wait 600
shot $W/u0.bmp
wait 600
shot $W/u1.bmp
dragto 0
wait 600
shot $W/v0.bmp
wait 600
shot $W/v1.bmp
quit
EOF
run "$tmp/s.txt"
kill $server 2> /dev/null
c() { node "$dir/check.js" "$@"; }
check "scrolled while busy: the old content, moved" "$(c moved "$tmp/s0.bmp" "$tmp/s1.bmp" 200)" ">" 0.99
check "then the tab's own frame (gray)" "$(c changed "$tmp/s1.bmp" "$tmp/s2.bmp" 0 68 1280 900)" ">" 0.9
check "frames keep coming" "$(c changed "$tmp/t0.bmp" "$tmp/t1.bmp" 0 68 640 368)" ">" 0.005
check "dragged out: frames keep coming" "$(c changed "$tmp/u0.bmp" "$tmp/u1.bmp" 0 68 640 368)" ">" 0.005
check "dragged back: frames keep coming" "$(c changed "$tmp/v0.bmp" "$tmp/v1.bmp" 0 68 640 368)" ">" 0.005
check "no tab process started again" "$(starts)" "==" 2
grep -q "tab [0-9]* moved to window 1" "$tmp/ui.log" && grep -q "tab [0-9]* moved to window 0" "$tmp/ui.log"
check "moved out and back" "$?" "==" 0

if [ -n "$replays" ]; then
    cat > "$tmp/y.txt" <<EOF
open about:blank
open https://www.youtube.com/watch?v=dQw4w9WgXcQ
wait 9000
shot $W/y0.bmp
wait 1500
shot $W/y1.bmp
dragout
wait 300
shot $W/y2.bmp
wait 1500
shot $W/y3.bmp
dragto 0
wait 1500
shot $W/y4.bmp
wait 1500
shot $W/y5.bmp
quit
EOF
    run "$tmp/y.txt" CYBER_REPLAY="$(cygpath -m "$replays/yt-dQw4w9WgXcQ")" CYBER_REPLAY_OFFLINE=1
    video="20 140 580 450"
    check "video plays" "$(c changed "$tmp/y0.bmp" "$tmp/y1.bmp" $video)" ">" 0.2
    check "dragged out: video plays" "$(c changed "$tmp/y2.bmp" "$tmp/y3.bmp" $video)" ">" 0.2
    check "dragged back: video plays" "$(c changed "$tmp/y4.bmp" "$tmp/y5.bmp" $video)" ">" 0.2
    check "no tab process started again" "$(starts)" "==" 2
fi
echo "$fails failed; screenshots in $tmp"
exit $((fails > 0))
