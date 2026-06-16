#!/bin/bash
# Build bgmdwnldr desktop app for Windows (from Git Bash / WSL)
set -e
cd "$(dirname "$0")"

echo "=== Building bgmdwnldr desktop app ==="
cd browser-emulator
./build_msvc.sh

echo ""
echo "=== Build complete ==="
