#!/bin/bash
# Build cyberbrowser on Windows with MSVC using CMake.
# Run this from a Developer Command Prompt for VS 2022 (or ensure cl.exe is in PATH).
set -e
cd "$(dirname "$0")"

echo "=== Building cyberbrowser with MSVC ==="
cd cyberbrowser
./build_msvc.sh

echo ""
echo "=== Build complete ==="
