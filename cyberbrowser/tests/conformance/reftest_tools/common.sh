#!/bin/bash
# common.sh: sourced by the reftest tools: where the repo, the conformance tree, its results and the tools' state are.
#   CB_CONFORMANCE  the tree with run-s/ (the browser binaries), wpt/ (a WPT checkout) and results/; default ~/cb_conformance
#   CB_RUN, CB_WPT  override run-s/ and wpt/ on their own
#   REFTEST_STATE   where the base run and the failing list are kept; default $CB_CONFORMANCE/state
T=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$T/../../.." && pwd)
CB=${CB_CONFORMANCE:-$HOME/cb_conformance}
RUN=${CB_RUN:-$CB/run-s}
WPT=${CB_WPT:-$CB/wpt}
RES=$CB/results
STATE=${REFTEST_STATE:-$CB/state}
mkdir -p "$STATE" "$RES/sub"
# (node on Windows wants C:/..., not /c/...)
win() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }
