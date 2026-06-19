#!/bin/bash
# Build browser-emulator on Windows with MSVC using CMake.
# Run this from a Developer Command Prompt for VS 2022 (or ensure cl.exe is in PATH).
set -e
cd "$(dirname "$0")"

mkdir -p build-msvc
cd build-msvc

cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release --target browser-emulator-tests -j4

echo ""
echo "Build complete. Test binary: $(pwd)/tests/Release/browser-emulator-tests.exe"
