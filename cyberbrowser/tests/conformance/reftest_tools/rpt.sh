#!/bin/bash
# rpt.sh <wpt paths>...: a reftest run of those paths (directories or files) into results/sub/refp<time>;
# prints the summary, the pages that never set their ready flag (they waited their whole time), and R=<run>.
# Compare it with the base:  node rcmp.js "$RES/$(cat $STATE/last_ref.txt)" "$RES/sub/<run>"  (WON=1 lists wins too)
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
cd "$REPO"
R=$RES/sub; RN=refp$(date +%s)
REFTEST=1 WPT_FAILS_MAX=3000 PORT=${PORT:-38950} timeout ${TO:-1800} bash tests/conformance/wpt_test.sh "$RUN" "$WPT" "$R/$RN" "$@" > "$R/$RN.log" 2>&1
tail -3 "$R/$RN.log"
cd "$R/$RN" && node -e "const fs=require('fs');const l=fs.readFileSync('pages.txt','utf8').trim().split('\n');let m=0;for(const x of l){const k=x.split(' ')[0];if(!fs.existsSync('step-'+k+'.flag')){m++;if(m<=40)console.log('  noflag',x.split(' ')[1].replace(/^http:..[^/]+./,''))}}console.log('no flag',m,'of',l.length)"
echo R=$RN
