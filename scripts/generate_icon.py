#!/usr/bin/env python3
"""Generate a bold cyberpunk app icon with the Chinese character 音 (sound)."""

import os
import sys
import subprocess
from PIL import Image, ImageDraw, ImageFont, ImageFilter

# Try to find a good bold CJK font
FONT_CANDIDATES = [
    "/System/Library/Fonts/Hiragino Sans GB.ttc",   # Hiragino Sans GB W6 (bold)
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/Library/Fonts/Arial Unicode.ttf",
]

def find_font():
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            # Try to load W6 (bold) for Hiragino, otherwise index 0
            if "Hiragino" in path:
                try:
                    return ImageFont.truetype(path, size=100, index=2)  # W6/Bold
                except Exception:
                    try:
                        return ImageFont.truetype(path, size=100, index=0)
                    except Exception:
                        continue
            else:
                try:
                    return ImageFont.truetype(path, size=100, index=0)
                except Exception:
                    continue
    raise RuntimeError("No suitable CJK font found")


def make_icon(size):
    """Render the icon at the given square size."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Background: dark cyberpunk gradient-ish (solid dark for simplicity)
    bg_color = (8, 8, 30, 255)
    draw.rounded_rectangle(
        [(0, 0), (size - 1, size - 1)],
        radius=size // 5,
        fill=bg_color,
        outline=None,
    )

    # Load font and find the largest size that fits
    base_font = find_font()
    char = "音"
    # Binary search for max font size
    low, high = 1, size
    best_font = None
    best_bbox = None
    while low <= high:
        mid = (low + high) // 2
        try:
            font = ImageFont.truetype(base_font.path, size=mid, index=getattr(base_font, 'index', 0))
        except Exception:
            font = base_font
        bbox = draw.textbbox((0, 0), char, font=font)
        w = bbox[2] - bbox[0]
        h = bbox[3] - bbox[1]
        margin = size // 8
        if w <= size - margin * 2 and h <= size - margin * 2:
            best_font = font
            best_bbox = bbox
            low = mid + 1
        else:
            high = mid - 1

    if best_font is None:
        best_font = base_font
        best_bbox = draw.textbbox((0, 0), char, font=best_font)

    # Center the character
    w = best_bbox[2] - best_bbox[0]
    h = best_bbox[3] - best_bbox[1]
    x = (size - w) // 2 - best_bbox[0]
    y = (size - h) // 2 - best_bbox[1]

    # Glow layer
    if size >= 32:
        glow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        glow_draw = ImageDraw.Draw(glow)
        glow_draw.text((x, y), char, font=best_font, fill=(0, 255, 255, 120))
        glow = glow.filter(ImageFilter.GaussianBlur(radius=max(2, size // 40)))
        img = Image.alpha_composite(img, glow)
        draw = ImageDraw.Draw(img)

    # Main character: bright cyan
    draw.text((x, y), char, font=best_font, fill=(0, 238, 238, 255))

    # Small highlight dot for punch
    dot_r = max(2, size // 35)
    dot_x = size - size // 5
    dot_y = size // 5
    draw.ellipse(
        [(dot_x - dot_r, dot_y - dot_r), (dot_x + dot_r, dot_y + dot_r)],
        fill=(255, 255, 255, 220),
    )

    return img


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    assets_dir = os.path.join(script_dir, "..", "app", "src", "main", "assets")
    os.makedirs(assets_dir, exist_ok=True)

    iconset_dir = os.path.join(assets_dir, "AppIcon.iconset")
    os.makedirs(iconset_dir, exist_ok=True)

    sizes = [16, 32, 64, 128, 256, 512, 1024]
    for sz in sizes:
        img = make_icon(sz)
        path = os.path.join(iconset_dir, f"icon_{sz}x{sz}.png")
        img.save(path)
        print(f"Generated {sz}x{sz}")

    # Generate @2x versions
    double_map = {
        16: 32, 32: 64, 128: 256, 256: 512, 512: 1024,
    }
    for base, double_sz in double_map.items():
        src = os.path.join(iconset_dir, f"icon_{double_sz}x{double_sz}.png")
        dst = os.path.join(iconset_dir, f"icon_{base}x{base}@2x.png")
        if os.path.exists(src):
            import shutil
            shutil.copy(src, dst)

    # Build icns
    icns_path = os.path.join(assets_dir, "AppIcon.icns")
    result = subprocess.run(
        ["iconutil", "-c", "icns", iconset_dir, "-o", icns_path],
        capture_output=True, text=True,
    )
    if result.returncode != 0:
        print("iconutil failed:", result.stderr)
        sys.exit(1)

    # Cleanup
    import shutil
    shutil.rmtree(iconset_dir)

    # Also save the SVG as a simple reference (borderless)
    svg_path = os.path.join(assets_dir, "icon.svg")
    with open(svg_path, "w") as f:
        f.write('''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024">
  <rect width="1024" height="1024" rx="205" ry="205" fill="#08081e"/>
  <text x="512" y="720" font-family="Hiragino Sans GB, PingFang SC, sans-serif"
        font-size="720" font-weight="bold" fill="#0ee" text-anchor="middle">音</text>
  <circle cx="820" cy="200" r="30" fill="#fff" opacity="0.9"/>
</svg>''')

    print(f"AppIcon.icns generated: {icns_path}")

    # Generate default album art (512x512 JPEG) for embedding into M4A files
    album_art = make_icon(512)
    album_path = os.path.join(assets_dir, "default_album_art.jpg")
    # Convert to RGB for JPEG
    album_rgb = Image.new("RGB", album_art.size, (8, 8, 30))
    album_rgb.paste(album_art, mask=album_art.split()[3])
    album_rgb.save(album_path, "JPEG", quality=90)
    print(f"Default album art generated: {album_path}")


if __name__ == "__main__":
    main()
