#!/usr/bin/env python3
"""Capture a reference screenshot of a URL using Google Chrome headless.

Usage:
    python scripts/capture_chrome_screenshot.py [URL] [OUT_PATH]

Defaults:
    URL: https://www.youtube.com/watch?v=dQw4w9WgXcQ
    OUT_PATH: cyberbrowser/build-mingw/chrome_ref.jpg
"""
import os
import platform
import subprocess
import sys
from pathlib import Path

DEFAULT_URL = "https://www.youtube.com/watch?v=dQw4w9WgXcQ"
DEFAULT_OUT = str(Path(__file__).parent.parent / "cyberbrowser" / "build-mingw" / "chrome_ref.jpg")

VIEWPORT_W = 1024
VIEWPORT_H = 2400


def find_chrome():
    system = platform.system()
    candidates = []
    if system == "Windows":
        pf = os.environ.get("PROGRAMFILES", r"C:\Program Files")
        pf_x86 = os.environ.get("PROGRAMFILES(X86)", r"C:\Program Files (x86)")
        candidates = [
            os.path.join(pf, "Google", "Chrome", "Application", "chrome.exe"),
            os.path.join(pf_x86, "Google", "Chrome", "Application", "chrome.exe"),
            os.path.join(os.environ.get("LOCALAPPDATA", ""), "Google", "Chrome", "Application", "chrome.exe"),
        ]
    elif system == "Darwin":
        candidates = [
            "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
            "/Applications/Chromium.app/Contents/MacOS/Chromium",
        ]
    else:
        candidates = [
            "/usr/bin/google-chrome",
            "/usr/bin/chromium",
            "/usr/bin/chromium-browser",
            "/usr/bin/chrome",
        ]

    for c in candidates:
        if os.path.isfile(c):
            return c
    # Fallback: PATH search
    for name in ("google-chrome", "chromium", "chromium-browser", "chrome"):
        for ext in ("", ".exe"):
            for path_dir in os.environ.get("PATH", "").split(os.pathsep):
                full = os.path.join(path_dir, name + ext)
                if os.path.isfile(full):
                    return full
    return None


def run(cmd):
    print("Running:", " ".join(cmd))
    subprocess.run(cmd, check=True)


def main():
    url = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL
    out_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_OUT
    out_path = os.path.abspath(out_path)

    chrome = find_chrome()
    if not chrome:
        print("ERROR: Could not find Google Chrome/Chromium.", file=sys.stderr)
        sys.exit(1)
    print(f"Using Chrome: {chrome}")

    out_dir = os.path.dirname(out_path)
    os.makedirs(out_dir, exist_ok=True)

    # Chrome's --screenshot writes a PNG by default.
    png_path = os.path.splitext(out_path)[0] + ".png"
    # Use a desktop user-agent, dark mode to match the emulator's rendered theme,
    # and a virtual-time budget so lazy/timer-driven Polymer content gets a chance
    # to stamp before the screenshot is taken.
    user_agent = (
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"
    )
    cmd = [
        chrome,
        "--headless",
        "--disable-gpu",
        "--hide-scrollbars",
        "--force-dark-mode",
        "--run-all-compositor-stages-before-draw",
        "--virtual-time-budget=20000",
        f"--user-agent={user_agent}",
        f"--window-size={VIEWPORT_W},{VIEWPORT_H}",
        f"--screenshot={png_path}",
        url,
    ]
    run(cmd)

    # Convert to the requested format/size. Prefer ImageMagick's "magick" CLI.
    magick = "magick"
    convert_cmd = [
        magick,
        "convert",
        png_path,
        "-resize",
        f"{VIEWPORT_W}x{VIEWPORT_H}!",
        out_path,
    ]
    try:
        run(convert_cmd)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("WARNING: ImageMagick 'magick' not found; leaving reference as PNG.")
        out_path = png_path

    print(f"Reference screenshot saved: {out_path}")


if __name__ == "__main__":
    main()
