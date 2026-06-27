#!/usr/bin/env python3
"""Instrument GC-reference-writing setters in quickjs_handle_classes.h
   with write barriers using this->handle_ as the source."""
import re
from pathlib import Path

FILE = Path("cyberbrowser/third_party/quickjs/quickjs_handle_classes.h")
text = FILE.read_text(encoding="utf-8")

# We transform setters of the form:
#   void set_xxx(const GCValue& val) {
#       SomeType* p = get_ptr();
#       if (p) p->field = val;
#   }
# or with GCValue v, or GCHandle h/handle/val.
#
# We only touch bodies that contain a single assignment guarded by if (p).
# More complex methods are left for manual review.

def repl_gvalue(m):
    body = m.group(0)
    # Find the parameter name: const GCValue& NAME  or  GCValue NAME
    sig = m.group(1)
    # The full match includes the braces; we need the assignment line.
    # Extract the field expression after "p->" and before " = <param>;"
    ma = re.search(r'if \(p\) p->([a-zA-Z0-9_.\[\]]+)\s*=\s*([a-zA-Z0-9_]+)\s*;', body)
    if not ma:
        return body
    field = ma.group(1)
    param = ma.group(2)
    # Validate parameter name appears in signature
    if param not in sig:
        return body
    new_body = body[:ma.start()] + \
        f"if (p) {{\n            p->{field} = {param};\n            gc_write_barrier(handle_, GC_VALUE_GET_HANDLE({param}));\n        }}" + \
        body[ma.end():]
    return new_body

def repl_ghandle(m):
    body = m.group(0)
    sig = m.group(1)
    ma = re.search(r'if \(p\) p->([a-zA-Z0-9_.\[\]]+)\s*=\s*([a-zA-Z0-9_]+)\s*;', body)
    if not ma:
        return body
    field = ma.group(1)
    param = ma.group(2)
    if param not in sig:
        return body
    new_body = body[:ma.start()] + \
        f"if (p) {{\n            p->{field} = {param};\n            gc_write_barrier(handle_, {param});\n        }}" + \
        body[ma.end():]
    return new_body

# Match methods with a single-line if assignment body.
# We use a regex that spans from "void set_...(" to the closing brace,
# but stops at first "}". This works for simple bodies only.
pat_gvalue = re.compile(
    r'void set_([a-zA-Z0-9_]+)\(((?:const\s+)?GCValue(?:\s*&\s*|\s+)([a-zA-Z0-9_]+))\)\s*\{[^}]*if \(p\) p->[a-zA-Z0-9_.\[\]]+\s*=\s*\3\s*;[^}]*\}',
    re.DOTALL
)
pat_ghandle = re.compile(
    r'void set_([a-zA-Z0-9_]+)\((GCHandle\s+([a-zA-Z0-9_]+))\)\s*\{[^}]*if \(p\) p->[a-zA-Z0-9_.\[\]]+\s*=\s*\3\s*;[^}]*\}',
    re.DOTALL
)

# Apply replacements until stable (avoid overlapping issues)
old = None
while old != text:
    old = text
    text = pat_gvalue.sub(repl_gvalue, text)
    text = pat_ghandle.sub(repl_ghandle, text)

FILE.write_text(text, encoding="utf-8")
print("Done")
