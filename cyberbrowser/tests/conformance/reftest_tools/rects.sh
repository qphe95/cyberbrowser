#!/bin/bash
# rects.sh <css path>...: each test page's element boxes (tag.class#id x y w h, from getBoundingClientRect) through the real browser.
# (Unreliable for .xht pages and for images still loading.)
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
D=$(mktemp -d)
n=0; fs=
for t in "$@"; do
  n=$((n+1)); dir=$(dirname $t)
  { echo "<!doctype html><base href=\"/css/$dir/\"><script src=\"/resources/testharness.js\"></script><script src=\"/resources/testharnessreport.js\"></script>"
    grep -vi "<!DOCTYPE" "$WPT/css/$t" | grep -vi "rel=.*match"
    cat <<'EOS'
<script>
test(()=>{document.body.offsetWidth;const o=[];const walk=(e,d)=>{if(e.tagName=='SCRIPT'||e.tagName=='P')return;const r=e.getBoundingClientRect();o.push(' '.repeat(d)+e.tagName.toLowerCase()+(e.className?'.'+String(e.className).replace(/ /g,'.'):'')+(e.id?'#'+e.id:'')+' '+[r.x,r.y,r.width,r.height].map(v=>Math.round(v*100)/100).join(' '));for(const c of e.children)walk(c,d+1)};for(const c of document.body.children)walk(c,0);assert_unreached('\n'+o.join('\n'));});
</script>
EOS
  } > "$D/rp$n.html"
  fs="$fs $D/rp$n.html"
done
bash "$T/wp.sh" $fs 2>&1 | cut -c1-200
rm -rf "$D"
