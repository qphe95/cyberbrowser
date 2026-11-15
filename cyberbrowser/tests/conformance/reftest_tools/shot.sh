#!/bin/bash
# shot.sh [out dir] < bodies: probe bodies on stdin (separated by lines of ====), each rendered as a reftest
# against the green square; test | reference | difference views written to <out dir>/s<N>.png (default .).
# CROP=x,y,w,h picks the region (default 0,0,300,260).
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
O=$(cd "${1:-.}" && pwd)
D=$WPT/css/zzprobe; rm -rf "$D"; mkdir -p "$D"
N=$(node -e "
const fs=require('fs');const parts=fs.readFileSync(0,'utf8').split(/^====\s*$/m).filter(x=>x.trim());
parts.forEach((b,i)=>fs.writeFileSync(process.argv[1]+'/s'+(i+1)+'.html','<!DOCTYPE html>\n<link rel=\"match\" href=\"../reference/ref-filled-green-100px-square.xht\">\n<p>Test passes if there is a filled green square and <strong>no red</strong>.</p>\n'+b));
console.log(parts.length);" "$(win "$D")")
R=$(bash "$T/rpt.sh" css/zzprobe 2>&1 | grep -o "R=.*" | tail -1); R=${R#R=}
for i in $(seq 1 $N); do CROP=${CROP:-0,0,300,260} node "$T/refview.js" "$(win "$RES/sub/$R")" css/zzprobe/s$i.html "$(win "$O/s$i.png")" 2>&1 | sed 's/ -> .*//' | tail -1; done
rm -rf "$D"
