#!/usr/bin/env python3
"""Embed album art into an M4A file using mutagen."""
import sys
import os

try:
    from mutagen.mp4 import MP4, MP4Cover
except ImportError:
    print("mutagen not installed", file=sys.stderr)
    sys.exit(1)

def embed_art(m4a_path, image_path):
    if not os.path.exists(m4a_path):
        print(f"M4A file not found: {m4a_path}", file=sys.stderr)
        return False
    if not os.path.exists(image_path):
        print(f"Image file not found: {image_path}", file=sys.stderr)
        return False

    try:
        audio = MP4(m4a_path)
    except Exception as e:
        print(f"Failed to open M4A: {e}", file=sys.stderr)
        return False

    with open(image_path, 'rb') as f:
        image_data = f.read()

    # Determine image format
    if image_data[:2] == b'\xff\xd8':
        cover_format = MP4Cover.FORMAT_JPEG
    elif image_data[:8] == b'\x89PNG\r\n\x1a\n':
        cover_format = MP4Cover.FORMAT_PNG
    else:
        print(f"Unknown image format, assuming JPEG", file=sys.stderr)
        cover_format = MP4Cover.FORMAT_JPEG

    audio['covr'] = [MP4Cover(image_data, cover_format)]
    audio.save()
    print(f"Embedded album art into {m4a_path}")
    return True

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <m4a_file> <image_file>", file=sys.stderr)
        sys.exit(1)
    success = embed_art(sys.argv[1], sys.argv[2])
    sys.exit(0 if success else 1)
