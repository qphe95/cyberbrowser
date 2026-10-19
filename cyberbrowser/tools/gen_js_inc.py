#!/usr/bin/env python3
"""gen_js_inc.py in.js out.inc : turn a JS source file into a C string
literal for #include (used for src/browser/url_api.js)."""
import sys
src = open(sys.argv[1], encoding='utf-8').read()
out = ['/* Generated from %s by tools/gen_js_inc.py - do not edit. */' % sys.argv[1].split('/')[-1]]
for line in src.split('\n'):
    esc = line.replace('\\', '\\\\').replace('"', '\\"')
    esc = ''.join(c if ord(c) < 128 else '\\u%04x' % ord(c) for c in esc)
    out.append('"' + esc + '\\n"')
open(sys.argv[2], 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
