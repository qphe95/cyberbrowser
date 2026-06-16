#!/usr/bin/env python3
"""
Patch QuickJS headers for MSVC compatibility.

Issues fixed:
1. GC_MKVAL / GC_MKHANDLE compound literals (C4576)
2. extern "C" blocks that contain functions returning C++ classes (C2526)
3. Individual extern "C" declarations

All changes are wrapped in #ifdef _MSC_VER so they don't affect GCC/Clang builds.
"""

import os
import sys

QUICKJS_DIR = os.path.join(os.path.dirname(__file__), "..", "browser-emulator", "third_party", "quickjs")

def patch_quickjs_h():
    path = os.path.join(QUICKJS_DIR, "quickjs.h")
    with open(path, "r") as f:
        content = f.read()

    # Fix GC_MKVAL / GC_MKHANDLE compound literals
    old_mkval = '#define GC_MKVAL(tag, val) (GCValue){ (GCValueUnion){ .int32 = val }, tag }'
    new_mkval = '''#ifdef _MSC_VER
static inline GCValue GC_MKVAL_FUNC(int64_t tag, int32_t val) {
    GCValue v;
    v.u.int32 = val;
    v.tag = tag;
    return v;
}
#define GC_MKVAL(tag, val) GC_MKVAL_FUNC(tag, val)
#else
#define GC_MKVAL(tag, val) (GCValue){ (GCValueUnion){ .int32 = val }, tag }
#endif'''
    content = content.replace(old_mkval, new_mkval)

    old_mkhandle = '#define GC_MKHANDLE(tag, handle_val) (GCValue){ (GCValueUnion){ .handle = handle_val }, tag }'
    new_mkhandle = '''#ifdef _MSC_VER
static inline GCValue GC_MKHANDLE_FUNC(int64_t tag, GCHandle handle_val) {
    GCValue v;
    v.u.handle = handle_val;
    v.tag = tag;
    return v;
}
#define GC_MKHANDLE(tag, handle_val) GC_MKHANDLE_FUNC(tag, handle_val)
#else
#define GC_MKHANDLE(tag, handle_val) (GCValue){ (GCValueUnion){ .handle = handle_val }, tag }
#endif'''
    content = content.replace(old_mkhandle, new_mkhandle)

    # Also fix the second GC_MKHANDLE definition
    old_mkhandle2 = '#define GC_MKHANDLE(tag, h) (GCValue){ (GCValueUnion){ .handle = (h) }, (tag) }'
    new_mkhandle2 = '''#ifdef _MSC_VER
#define GC_MKHANDLE(tag, h) GC_MKHANDLE_FUNC(tag, h)
#else
#define GC_MKHANDLE(tag, h) (GCValue){ (GCValueUnion){ .handle = (h) }, (tag) }
#endif'''
    content = content.replace(old_mkhandle2, new_mkhandle2)

    with open(path, "w") as f:
        f.write(content)
    print(f"Patched {path}")


def patch_extern_c_blocks(filepath):
    with open(filepath, "r") as f:
        lines = f.readlines()

    # Find extern "C" { and matching }
    result = []
    in_extern_c = False
    extern_c_start_line = -1
    modified = False

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()

        # Check for opening extern "C" {
        if stripped == 'extern "C" {':
            # Look back to see if it's guarded by #ifdef __cplusplus
            if i > 0 and '#ifdef __cplusplus' in lines[i-1]:
                # Insert #ifndef _MSC_VER after #ifdef __cplusplus
                result.append(lines[i-1])
                result.append('#ifndef _MSC_VER\n')
                result.append(line)
                in_extern_c = True
                extern_c_start_line = i
                i += 1
                modified = True
                continue
            else:
                # Unconditional extern "C" - wrap it
                result.append('#ifdef __cplusplus\n')
                result.append('#ifndef _MSC_VER\n')
                result.append(line)
                result.append('#endif\n')
                result.append('#endif\n')
                in_extern_c = True
                modified = True
                i += 1
                continue

        # Check for closing } with extern "C" comment
        if in_extern_c and ('} /* extern "C" */' in stripped or '} /* extern "C" { */' in stripped):
            if '#ifdef __cplusplus' in lines[i-1] if i > 0 else False:
                result.append('#endif\n')
                result.append(lines[i-1])
                result.append(line)
                in_extern_c = False
                i += 1
                modified = True
                continue
            else:
                result.append('#ifdef __cplusplus\n')
                result.append('#ifndef _MSC_VER\n')
                result.append(line)
                result.append('#endif\n')
                result.append('#endif\n')
                in_extern_c = False
                i += 1
                modified = True
                continue

        # Check for plain closing } that matches an extern "C" {
        # We only do this for files where we know the exact line
        if in_extern_c and stripped == '}':
            if '#ifdef __cplusplus' in (lines[i+1] if i+1 < len(lines) else ''):
                result.append('#endif\n')
                result.append(line)
                in_extern_c = False
                i += 1
                modified = True
                continue
            else:
                result.append('#ifdef __cplusplus\n')
                result.append('#ifndef _MSC_VER\n')
                result.append(line)
                result.append('#endif\n')
                result.append('#endif\n')
                in_extern_c = False
                i += 1
                modified = True
                continue

        # Check for individual extern "C" function declarations
        if stripped.startswith('extern "C" ') and '{' not in stripped:
            # e.g., extern "C" void *gc_deref(uint32_t handle);
            result.append('#ifdef _MSC_VER\n')
            result.append(stripped.replace('extern "C" ', '', 1) + '\n')
            result.append('#else\n')
            result.append(line)
            result.append('#endif\n')
            modified = True
            i += 1
            continue

        result.append(line)
        i += 1

    if modified:
        with open(filepath, "w") as f:
            f.writelines(result)
        print(f"Patched {filepath}")
    else:
        print(f"No changes needed for {filepath}")


def patch_file_lines(filepath, replacements):
    """Replace specific lines in a file."""
    with open(filepath, "r") as f:
        lines = f.readlines()

    modified = False
    for old_line, new_lines in replacements:
        for i, line in enumerate(lines):
            if line.rstrip('\n') == old_line:
                lines[i:i+1] = [l + '\n' for l in new_lines]
                modified = True
                break

    if modified:
        with open(filepath, "w") as f:
            f.writelines(lines)
        print(f"Patched {filepath}")
    else:
        print(f"No changes needed for {filepath}")


def main():
    patch_quickjs_h()

    # Patch files with known extern "C" block locations
    files_to_patch = [
        os.path.join(QUICKJS_DIR, "quickjs.h"),
        os.path.join(QUICKJS_DIR, "quickjs_gc_unified.h"),
        os.path.join(QUICKJS_DIR, "quickjs_types.h"),
        os.path.join(QUICKJS_DIR, "quickjs-internal.h"),
        os.path.join(QUICKJS_DIR, "js_fast_dispatch.h"),
        os.path.join(QUICKJS_DIR, "js_atom_cache.h"),
        os.path.join(QUICKJS_DIR, "quickjs-libc.h"),
        os.path.join(QUICKJS_DIR, "quickjs_handle_classes.h"),
    ]

    for filepath in files_to_patch:
        if os.path.exists(filepath):
            patch_extern_c_blocks(filepath)
        else:
            print(f"File not found: {filepath}")


if __name__ == "__main__":
    main()
