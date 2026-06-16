#!/usr/bin/env python3
"""Convert default_album_art.jpg to an embedded C array."""

import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ASSETS_DIR = os.path.join(SCRIPT_DIR, "..", "app", "src", "main", "assets")
CPP_DIR = os.path.join(SCRIPT_DIR, "..", "app", "src", "main", "cpp")

def main():
    jpg_path = os.path.join(ASSETS_DIR, "default_album_art.jpg")
    if not os.path.exists(jpg_path):
        print(f"ERROR: {jpg_path} not found. Run generate_icon.py first.")
        return 1

    with open(jpg_path, "rb") as f:
        data = f.read()

    # Write header
    h_path = os.path.join(CPP_DIR, "default_album_art.h")
    with open(h_path, "w") as f:
        f.write("#ifndef DEFAULT_ALBUM_ART_H\n")
        f.write("#define DEFAULT_ALBUM_ART_H\n")
        f.write("#include <stddef.h>\n")
        f.write("extern const unsigned char DEFAULT_ALBUM_ART_JPG[];\n")
        f.write("extern const size_t DEFAULT_ALBUM_ART_JPG_SIZE;\n")
        f.write("#endif\n")

    # Write C source
    c_path = os.path.join(CPP_DIR, "default_album_art.c")
    with open(c_path, "w") as f:
        f.write('#include "default_album_art.h"\n\n')
        f.write("const unsigned char DEFAULT_ALBUM_ART_JPG[] = {\n")
        for i in range(0, len(data), 16):
            line = data[i:i+16]
            hexes = ", ".join(f"0x{b:02X}" for b in line)
            f.write("    " + hexes + ",\n")
        f.write("};\n\n")
        f.write(f"const size_t DEFAULT_ALBUM_ART_JPG_SIZE = {len(data)};\n")

    print(f"Generated {h_path}")
    print(f"Generated {c_path}")
    print(f"Album art size: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    exit(main())
