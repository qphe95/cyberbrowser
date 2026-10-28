#!/bin/bash
# The sandbox's escape tests (SANDBOX_DESIGN.md section 11.1).
#
#   tests/sandbox/escape_test.sh <build dir> [port=38517]
#
# Starts the test server (server.js) and the browser (cb_coord.exe) with a
# UI script that opens a page of each of two sites, then a hostile tab
# (cb_escape.exe, run in a tab's sandbox in place of cb_tab.exe for the URL
# ending in #escape), then a page of each site again.  It passes when:
#   - every attempt of the hostile tab was blocked and its checks worked;
#   - no file was planted in the other site's folder;
#   - no tab was started for the file: and other non-http URLs it asked for;
#   - the network process, the storage service, the relay and the GPU process
#     survived (the coordinator reports it if one ends) and still work: the GPU
#     process drew the next pages (the UI's last screenshot), and the last
#     localhost page's localStorage counter and cookies carry on from the first;
#   - the cookie it set for the other site reached none of that site's
#     requests;
#   - the UI lived on (its last screenshot was saved);
#   - no tab's AppContainer profile outlived the browser.
# Before it starts, it plants what the hostile tab looks for: a canary file
# in the user's temp folder and in the other site's folder, and in the
# hostile tab's own folder a junction and a hard link to the other site's.
build=$(cd "$1" && pwd)
port=${2:-38517}
dir=$(cd "$(dirname "$0")" && pwd)
win() { cygpath -w "$1"; }
wincmd() { MSYS2_ARG_CONV_EXCL='*' cmd /c "$@"; }
tmp=$(mktemp -d)
sites="$build/browser_data/sites"
own="http+localhost"
other="http+127.0.0.1"
fail=0
check() {   # check <what> <0 = passed>
    if [ "$2" = 0 ]; then echo "  pass  $1"; else echo "  FAIL  $1"; fail=1; fi
}

mkdir -p "$sites/$own" "$sites/$other"
echo "the other site's secret" > "$sites/$other/escape_canary"
rm -f "$sites/$other/escape_planted"
echo "the user's secret" > "$tmp/canary.txt"
[ -e "$sites/$own/jlink" ] && wincmd rmdir "$(win "$sites/$own/jlink")"
rm -f "$sites/$own/hlink"
wincmd mklink /J "$(win "$sites/$own/jlink")" "$(win "$sites/$other")" > /dev/null
wincmd mklink /H "$(win "$sites/$own/hlink")" "$(win "$sites/$other/escape_canary")" > /dev/null
[ -e "$sites/$own/jlink/escape_canary" ] && [ -e "$sites/$own/hlink" ] || { echo "could not make the links"; exit 1; }

node "$dir/server.js" "$port" "$tmp/requests.log" &
server=$!
sleep 1
cat > "$tmp/script.txt" <<EOF
open http://127.0.0.1:$port/
waitload 30000
wait 1500
open http://localhost:$port/
waitload 30000
wait 1500
log escape-start
open http://localhost:$port/#escape
wait 90000
open http://localhost:$port/
waitload 30000
wait 2000
open http://127.0.0.1:$port/
waitload 30000
wait 2000
shot $(cygpath -m "$tmp/after.bmp")
quit
EOF
logs="$build/logs"
touch "$tmp/start"      # tab logs older than this are from earlier runs
export CYBER_TAB_EXE=cb_escape.exe
export CYBER_ESCAPE_PORT=$port CYBER_ESCAPE_OTHER=$other
export CYBER_ESCAPE_READ="$(win "$tmp/canary.txt");$(win "$sites/$other/escape_canary");$(win "$sites/$other/cookies");C:\\Windows\\System32\\config\\SAM;$(win "$build/cb_tab.exe")"
export CYBER_ESCAPE_WRITE="$(win "$build");$(win "$tmp");$(win "$sites");$(win "$sites/$own");$(win "$sites/$other")"
(cd "$build" && CYBER_UI_SCRIPT="$(cygpath -m "$tmp/script.txt")" timeout 300 ./cb_coord.exe) 2> "$tmp/coord.txt"
coord=$?
kill $server 2> /dev/null
tab_logs=$(find "$logs" -name "tab-*.log" -newer "$tmp/start")

escape_log=$(grep -l "^\[escape\] tab" $tab_logs | head -1)
echo "hostile tab ($(basename "$escape_log")):"
grep -E "^\[escape\] (ESCAPED|FAILED|note)" "$escape_log" | sed 's/^/    /'
done_line=$(grep "^\[escape\] done" "$escape_log")
echo "    ${done_line:-no summary: the hostile tab ended early}"
echo "results:"
check "the hostile tab finished" $([ -n "$done_line" ]; echo $?)
check "every attempt blocked, every check worked" $(echo "$done_line" | grep -q " 0 escaped, 0 failed"; echo $?)
check "nothing planted in the other site's folder" $([ ! -e "$sites/$other/escape_planted" ]; echo $?)
check "no tab for a non-http URL" \
    $(! grep -h -E "^\[tab [0-9]+\] [^ ]+ \([0-9]+x[0-9]+\)$" $tab_logs | grep -v -q "\] http"; echo $?)
check "the coordinator ended normally" $([ "$coord" = 0 ]; echo $?)
check "the network, storage, relay and GPU processes lived" $(! grep -q "ended" "$tmp/coord.txt"; echo $?)
pings=$(grep "^localhost:$port GET /ping" "$tmp/requests.log" | sed -n 's/.*fromjs=\([0-9]*\).*/\1/p')
first=$(echo "$pings" | head -1)
last=$(echo "$pings" | tail -1)
check "storage and cookies work after the attack (counter $first -> $last)" \
    $([ "$(echo "$pings" | wc -l)" -ge 2 ] && [ "$last" = $((first + 1)) ]; echo $?)
check "the other site's requests never carried the planted cookie" \
    $(grep "^127.0.0.1:$port" "$tmp/requests.log" | grep -v "/echo" | grep -q planted; [ $? != 0 ]; echo $?)
check "the other site's last page loaded" $([ "$(grep -c "^127.0.0.1:$port GET /ping" "$tmp/requests.log")" -ge 2 ]; echo $?)
check "the UI lived through the hostile tab (its last screenshot)" $([ -s "$tmp/after.bmp" ]; echo $?)
check "no tab profile outlived the browser" $(! ls "$LOCALAPPDATA/Packages" | grep -q "^cyberbrowser\.tab\."; echo $?)
sed 's/^/  coordinator: /' "$tmp/coord.txt"
wincmd rmdir "$(win "$sites/$own/jlink")"
rm -f "$sites/$own/hlink"
rm -rf "$tmp"
exit $fail
