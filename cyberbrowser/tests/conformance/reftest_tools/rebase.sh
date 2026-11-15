#!/bin/bash
# rebase.sh <run>: make a full run the base later runs are compared against, and write its failing list
# ($STATE/allfail.txt: "<test> <pixels differing>/<max channel difference>", or "other").
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[ -d "$RES/$1" ] || { echo "no run $RES/$1"; exit 1; }
node "$T/fl.js" "$(win "$RES/$1")" > "$STATE/allfail.txt"
echo "$1" > "$STATE/last_ref.txt"
echo "base $1, $(wc -l < "$STATE/allfail.txt") failing"
