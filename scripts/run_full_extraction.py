#!/usr/bin/env python3
"""
Run the full YouTube browser API extraction pipeline.

1. (Optional) Capture fresh JSON API responses from a real Chrome session.
2. Extract browser API names from all files under youtube_data/.

Usage:
    python scripts/run_full_extraction.py          # extract only
    python scripts/run_full_extraction.py --capture # capture + extract
"""

import subprocess
import sys
from pathlib import Path

SCRIPTS_DIR = Path(__file__).parent.resolve()


def run(cmd: list[str]) -> int:
    print("\n" + "=" * 80, flush=True)
    print("Running:", " ".join(cmd), flush=True)
    print("=" * 80, flush=True)
    return subprocess.call(cmd, cwd=SCRIPTS_DIR.parent)


def main() -> int:
    capture = "--capture" in sys.argv

    if capture:
        rc = run([sys.executable, str(SCRIPTS_DIR / "capture_youtube_network.py")])
        if rc != 0:
            print("Capture failed, aborting.", file=sys.stderr)
            return rc

    rc = run([sys.executable, str(SCRIPTS_DIR / "extract_browser_apis.py")])
    return rc


if __name__ == "__main__":
    sys.exit(main())
