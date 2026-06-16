#!/bin/bash
# Wrapper to build_msvc.bat for Git Bash / WSL users
set -e
cd "$(dirname "$0")"

echo "Building with MSVC via build_msvc.bat..."
MSYS_NO_PATHCONV=1 cmd.exe /s /c 'build_msvc.bat'
