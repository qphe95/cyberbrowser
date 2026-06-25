#!/usr/bin/env python3
"""Reproducible visual-diff pipeline for CyberBrowser vs. Chrome.

1. Capture a headless Chrome reference screenshot (forced dark mode).
2. Run CyberBrowser to produce `youtube_screenshot.jpg`.
3. Compare the two images with ImageMagick and write a per-band report.

Example:
    python3 scripts/run_visual_diff.py

The script is idempotent: re-running it will overwrite the reference and
screenshot files in the build directory.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path


def find_chrome() -> str:
    """Return a plausible Chrome executable path on Windows."""
    candidates = [
        os.environ.get("CHROME_PATH", ""),
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    ]
    for c in candidates:
        if c and os.path.exists(c):
            return c
    raise FileNotFoundError("Could not find chrome.exe. Set CHROME_PATH.")


def capture_chrome_reference(url: str, out_path: str, chrome_path: str,
                              width: int = 1024, height: int = 2400) -> None:
    """Capture a headless Chrome screenshot to *out_path*."""
    abs_out = os.path.abspath(out_path)
    os.makedirs(os.path.dirname(abs_out) or ".", exist_ok=True)
    cmd = [
        chrome_path,
        "--headless",
        "--disable-gpu",
        "--hide-scrollbars",
        "--enable-features=WebContentsForceDark",
        "--run-all-compositor-stages-before-draw",
        "--virtual-time-budget=20000",
        f"--window-size={width},{height}",
        f"--screenshot={abs_out}",
        url,
    ]
    print("Capturing Chrome reference ...")
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        # Chrome often prints benign errors to stderr; keep going if the file
        # was written.
        if not os.path.exists(abs_out):
            print(proc.stderr, file=sys.stderr)
            raise RuntimeError("Chrome screenshot capture failed")
    print(f"Reference saved: {abs_out}")


def run_cyberbrowser(build_dir: str, url: str) -> None:
    """Run the CyberBrowser executable from *build_dir*."""
    exe = os.path.join(build_dir, "cyberbrowser.exe")
    if not os.path.exists(exe):
        raise FileNotFoundError(f"CyberBrowser executable not found: {exe}")
    print("Running CyberBrowser ...")
    subprocess.run([exe, url], cwd=build_dir, check=True)
    screenshot = os.path.join(build_dir, "youtube_screenshot.jpg")
    if not os.path.exists(screenshot):
        raise RuntimeError(f"CyberBrowser did not produce {screenshot}")
    print(f"Screenshot saved: {screenshot}")


def run_compare(build_dir: str, band_height: int) -> None:
    """Invoke scripts/compare_screenshots.py on the two outputs."""
    ref = os.path.join(build_dir, "chrome_ref.jpg")
    cand = os.path.join(build_dir, "youtube_screenshot.jpg")
    diff = os.path.join(build_dir, "screenshot_diff.png")
    report = os.path.join(build_dir, "screenshot_diff_report.txt")

    script = os.path.join(os.path.dirname(__file__), "compare_screenshots.py")
    cmd = [
        sys.executable, script,
        ref, cand,
        "--diff", diff,
        "--report", report,
        "--band-height", str(band_height),
    ]
    print("Comparing screenshots ...")
    subprocess.run(cmd, check=True)
    print(f"Report written: {report}")
    print(f"Diff image written: {diff}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Capture Chrome reference, run CyberBrowser, and diff screenshots."
    )
    parser.add_argument("--url", default="https://www.youtube.com/watch?v=dQw4w9WgXcQ",
                        help="URL to load")
    parser.add_argument("--build-dir",
                        default="cyberbrowser/build-mingw",
                        help="Directory containing cyberbrowser.exe and outputs")
    parser.add_argument("--chrome-path", default=find_chrome(),
                        help="Path to chrome.exe")
    parser.add_argument("--skip-capture", action="store_true",
                        help="Reuse existing chrome_ref.jpg")
    parser.add_argument("--skip-cyberbrowser", action="store_true",
                        help="Reuse existing youtube_screenshot.jpg")
    parser.add_argument("--band-height", type=int, default=200,
                        help="Vertical band height for per-band report")
    args = parser.parse_args()

    build_dir = os.path.abspath(args.build_dir)

    if not args.skip_capture:
        capture_chrome_reference(args.url, os.path.join(build_dir, "chrome_ref.jpg"),
                                 args.chrome_path)
    if not args.skip_cyberbrowser:
        run_cyberbrowser(build_dir, args.url)

    run_compare(build_dir, args.band_height)
    return 0


if __name__ == "__main__":
    sys.exit(main())
