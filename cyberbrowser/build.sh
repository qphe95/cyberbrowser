#!/bin/bash
# Build script for browser-emulator on macOS

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}Building browser-emulator for macOS...${NC}"

# Check dependencies
echo -e "${YELLOW}Checking dependencies...${NC}"

if ! command -v cmake &> /dev/null; then
    echo -e "${RED}Error: cmake is not installed${NC}"
    echo "Install with: brew install cmake"
    exit 1
fi

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Configure with CMake
echo -e "${YELLOW}Configuring with CMake...${NC}"
cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBE_BUILD_TESTS=ON

# Build
echo -e "${YELLOW}Building...${NC}"
make -j$(sysctl -n hw.ncpu)

echo -e "${GREEN}Build complete!${NC}"
echo ""
echo "To run tests:"
echo "  ${BUILD_DIR}/tests/browser-emulator-tests"
echo ""
echo "To install (optional):"
echo "  cd ${BUILD_DIR} && sudo make install"
