#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
APP_NAME="BGMDWLDR"
APP_BUNDLE="${APP_NAME}.app"
BUILD_DIR="${SCRIPT_DIR}/app/build_macos"

echo "=== Building ${APP_NAME} for macOS ==="

# Build with CMake
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
cmake "${SCRIPT_DIR}/app" -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)

# Create app bundle
BUNDLE_DIR="${BUILD_DIR}/${APP_BUNDLE}"
rm -rf "${BUNDLE_DIR}"
mkdir -p "${BUNDLE_DIR}/Contents/MacOS"
mkdir -p "${BUNDLE_DIR}/Contents/Resources"

# Copy binary
cp "${BUILD_DIR}/bgmdwnldr-mac" "${BUNDLE_DIR}/Contents/MacOS/${APP_NAME}"

# Copy shaders
cp "${SCRIPT_DIR}/app/src/main/assets/triangle.vert.spv" \
   "${BUNDLE_DIR}/Contents/Resources/"
cp "${SCRIPT_DIR}/app/src/main/assets/triangle.frag.spv" \
   "${BUNDLE_DIR}/Contents/Resources/"

# Copy icon
cp "${SCRIPT_DIR}/app/src/main/assets/AppIcon.icns" \
   "${BUNDLE_DIR}/Contents/Resources/"

# Create Info.plist
cat > "${BUNDLE_DIR}/Contents/Info.plist" << 'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>BGMDWLDR</string>
    <key>CFBundleIdentifier</key>
    <string>com.bgmdwnldr.app</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>BGMDWLDR</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>CFBundleVersion</key>
    <string>1</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
PLIST

echo ""
echo "=== Build complete ==="
echo "App bundle: ${BUNDLE_DIR}"
echo ""
echo "To run:"
echo "  open \"${BUNDLE_DIR}\""
echo ""
echo "Or double-click ${APP_BUNDLE} in Finder:"
echo "  open -R \"${BUNDLE_DIR}\""
