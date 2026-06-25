#!/usr/bin/env python3
"""Compare two screenshots and produce an MAE diff report.

Uses ImageMagick's `magick compare -metric MAE`.  The reference image is
assumed to be the ground truth (e.g. a Chrome capture); the second image is
the candidate (e.g. CyberBrowser's `youtube_screenshot.jpg`).

Example:
    python3 scripts/compare_screenshots.py \
        cyberbrowser/build-mingw/chrome_ref.jpg \
        cyberbrowser/build-mingw/youtube_screenshot.jpg \
        --diff cyberbrowser/build-mingw/screenshot_diff.png \
        --report cyberbrowser/build-mingw/screenshot_diff_report.txt
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile


def run_magick_compare(ref: str, cand: str, diff: str | None = None) -> tuple[float, float]:
    """Return (absolute_error, normalized_error) from `magick compare -metric MAE`."""
    cmd = ["magick", "compare", "-metric", "MAE", ref, cand]
    if diff:
        cmd.append(diff)
    # ImageMagick writes the metric to stderr and returns 1 even on success.
    proc = subprocess.run(cmd, capture_output=True, text=True)
    err = proc.stderr.strip() or proc.stdout.strip()
    m = re.search(r"([0-9.]+)\s*\(([0-9.]+)\)", err)
    if not m:
        raise RuntimeError(f"Could not parse ImageMagick output: {err!r}")
    return float(m.group(1)), float(m.group(2))


def get_image_size(path: str) -> tuple[int, int]:
    """Return (width, height) using ImageMagick identify."""
    proc = subprocess.run(
        ["magick", "identify", "-format", "%w %h", path],
        capture_output=True, text=True, check=True
    )
    parts = proc.stdout.strip().split()
    return int(parts[0]), int(parts[1])


def crop_compare(ref: str, cand: str, x: int, y: int, w: int, h: int) -> tuple[float, float]:
    """Compare a cropped region and return (abs, norm) MAE."""
    geo = f"{w}x{h}+{x}+{y}"
    cmd = ["magick", "compare", "-metric", "MAE", "-crop", geo, ref, cand, "null:"]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    err = proc.stderr.strip() or proc.stdout.strip()
    m = re.search(r"([0-9.]+)\s*\(([0-9.]+)\)", err)
    if not m:
        raise RuntimeError(f"Could not parse ImageMagick output: {err!r}")
    return float(m.group(1)), float(m.group(2))


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare two screenshots with MAE.")
    parser.add_argument("reference", help="Reference screenshot")
    parser.add_argument("candidate", help="Candidate screenshot")
    parser.add_argument("--diff", help="Path to write diff visualization")
    parser.add_argument("--report", help="Path to write text report")
    parser.add_argument("--band-height", type=int, default=200,
                        help="Vertical band height for per-band report")
    args = parser.parse_args()

    if not os.path.exists(args.reference):
        print(f"Reference not found: {args.reference}", file=sys.stderr)
        return 1
    if not os.path.exists(args.candidate):
        print(f"Candidate not found: {args.candidate}", file=sys.stderr)
        return 1

    width, height = get_image_size(args.reference)
    cand_w, cand_h = get_image_size(args.candidate)
    if (width, height) != (cand_w, cand_h):
        print(f"Size mismatch: reference {width}x{height} vs candidate {cand_w}x{cand_h}",
              file=sys.stderr)
        return 1

    whole_abs, whole_norm = run_magick_compare(args.reference, args.candidate, args.diff)

    lines = [
        "Screenshot diff report",
        "======================",
        f"Reference : {args.reference}",
        f"Candidate : {args.candidate}",
        f"Size      : {width}x{height}",
        "",
        f"Whole image MAE : {whole_abs:.2f} ({whole_norm:.4f})",
        "",
        f"Per-band MAE (bands of {args.band_height}px):",
    ]

    band = args.band_height
    y = 0
    while y < height:
        h = min(band, height - y)
        try:
            b_abs, b_norm = crop_compare(args.reference, args.candidate, 0, y, width, h)
        except Exception as e:
            b_abs, b_norm = 0.0, 0.0
            lines.append(f"  {y:4d}-{y+h:4d}: error ({e})")
        else:
            lines.append(f"  {y:4d}-{y+h:4d}: {b_abs:8.2f} ({b_norm:.4f})")
        y += h

    report = "\n".join(lines) + "\n"
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            f.write(report)
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
