#!/usr/bin/env python3
"""
Embed binary shader files as C arrays.

Usage:
    python embed_shaders.py output_dir shader1.spv shader2.spv ...

Generates:
    output_dir/embedded_shaders.h
    output_dir/embedded_shaders.c
"""

import sys
import os


def sanitize_name(filename):
    """Convert a filename to a valid C identifier."""
    base = os.path.basename(filename)
    # Remove extension
    base = os.path.splitext(base)[0]
    # Replace non-alphanumeric with underscore
    result = ""
    for ch in base:
        if ch.isalnum():
            result += ch
        else:
            result += "_"
    return result


def main():
    if len(sys.argv) < 3:
        print("Usage: embed_shaders.py <output_dir> <shader1.spv> [shader2.spv ...]")
        sys.exit(1)

    output_dir = sys.argv[1]
    shader_files = sys.argv[2:]

    os.makedirs(output_dir, exist_ok=True)

    h_path = os.path.join(output_dir, "embedded_shaders.h")
    c_path = os.path.join(output_dir, "embedded_shaders.c")

    # Build header
    h_lines = [
        "#ifndef EMBEDDED_SHADERS_H",
        "#define EMBEDDED_SHADERS_H",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "#ifdef __cplusplus",
        "extern \"C\" {",
        "#endif",
        "",
    ]

    c_lines = [
        '#include "embedded_shaders.h"',
        "",
    ]

    for spv_path in shader_files:
        if not os.path.exists(spv_path):
            print(f"Error: file not found: {spv_path}")
            sys.exit(1)

        name = sanitize_name(spv_path)

        h_lines.append(f"extern const uint8_t {name}_spv[];")
        h_lines.append(f"extern const size_t {name}_spv_len;")
        h_lines.append("")

        with open(spv_path, "rb") as f:
            data = f.read()

        c_lines.append(f"const uint8_t {name}_spv[] = {{")

        # Write up to 12 bytes per line
        for i in range(0, len(data), 12):
            chunk = data[i : i + 12]
            hex_vals = ", ".join(f"0x{b:02x}" for b in chunk)
            c_lines.append(f"    {hex_vals},")

        c_lines.append("};")
        c_lines.append(f"const size_t {name}_spv_len = sizeof({name}_spv);")
        c_lines.append("")

    h_lines.append("#ifdef __cplusplus")
    h_lines.append("}")
    h_lines.append("#endif")
    h_lines.append("")
    h_lines.append("#endif /* EMBEDDED_SHADERS_H */")
    h_lines.append("")

    with open(h_path, "w", newline="\n") as f:
        f.write("\n".join(h_lines))

    with open(c_path, "w", newline="\n") as f:
        f.write("\n".join(c_lines))

    print(f"Generated {h_path} and {c_path}")


if __name__ == "__main__":
    main()
