#!/bin/bash
# bld.sh [build dir]: build the browser and copy its binaries to the run directory the reftests use.
# The build dir defaults to build-release, or build-mq while a cb_tab.exe (a browser in use) holds build-release's binaries.
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[ -d /c/msys64/mingw64/bin ] && export PATH=/c/msys64/mingw64/bin:$PATH
B=${1:-build-release}
[ -z "$1" ] && command -v tasklist >/dev/null 2>&1 && tasklist | grep -qi cb_tab && B=build-mq
cd "$REPO/$B" || exit 1
E=$(timeout 600 cmake --build . -j ${JOBS:-12} 2>&1 | grep -E " error|Error " | head)
[ -n "$E" ] && { echo "$E"; echo FAIL; exit 1; }
mkdir -p "$RUN" && cp cb_*.exe "$RUN"/ && echo "built $B -> $RUN"
