#!/bin/bash
set -e

echo "=== Building bgmdwldr with QuickJS browser stubs ==="

# Build debug APK
echo "Building..."
./gradlew assembleDebug

# Install to device
echo "Installing..."
adb install -r app/build/outputs/apk/debug/app-debug.apk

echo "=== Done! App installed ==="
echo ""
echo "To test, run:"
echo "  adb logcat -c && adb logcat -s js_quickjs:* bgmdwldr:* *:S"
