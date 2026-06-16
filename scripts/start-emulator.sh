#!/bin/bash
# Start the bgmdwldr emulator with UI
# Usage: ./scripts/start-emulator.sh [avd_name]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
AVD_NAME="${1:-bgmdwldr_avd}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${GREEN}[EMU]${NC} $1"; }
info() { echo -e "${BLUE}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }

log "Starting emulator: $AVD_NAME"

# Find Android SDK
if [ -z "$ANDROID_SDK_ROOT" ] && [ -z "$ANDROID_HOME" ]; then
    # Try to find SDK in common locations
    # Prefer newer emulator (v36+) at ~/Android/sdk over old (v33) at ~/Library/Android/sdk
    if [ -d "$HOME/Android/sdk" ]; then
        export ANDROID_SDK_ROOT="$HOME/Android/sdk"
    elif [ -d "$HOME/Library/Android/sdk" ]; then
        export ANDROID_SDK_ROOT="$HOME/Library/Android/sdk"
    else
        echo "Error: ANDROID_SDK_ROOT or ANDROID_HOME not set"
        exit 1
    fi
fi

SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
EMULATOR="$SDK_ROOT/emulator/emulator"

if [ ! -f "$EMULATOR" ]; then
    echo "Error: Emulator not found at $EMULATOR"
    exit 1
fi

# Kill any existing emulator processes
info "Cleaning up existing emulators..."
adb devices | grep emulator | awk '{print $1}' | while read device; do
    adb -s $device emu kill 2>/dev/null || true
done

# Kill emulator processes
pkill -9 -f "qemu-system" 2>/dev/null || true
pkill -9 -f "emulator" 2>/dev/null || true
sleep 2

# Check if AVD exists
if ! "$EMULATOR" -list-avds | grep -q "^${AVD_NAME}$"; then
    echo "Error: AVD '$AVD_NAME' not found"
    echo "Available AVDs:"
    "$EMULATOR" -list-avds
    exit 1
fi

# Start emulator with UI
log "Starting emulator with UI..."
"$EMULATOR" -avd "$AVD_NAME" -no-snapshot-load -gpu host &
EMULATOR_PID=$!

# Save PID for cleanup
echo $EMULATOR_PID > /tmp/bgmdwldr_emulator.pid

info "Emulator PID: $EMULATOR_PID"
info "Waiting for device to boot..."

# Wait for ADB
timeout=120
while [ $timeout -gt 0 ]; do
    if adb devices | grep -q "emulator-[0-9]*[[:space:]]*device"; then
        break
    fi
    sleep 2
    timeout=$((timeout - 2))
done

if [ $timeout -le 0 ]; then
    echo "Error: Emulator failed to boot"
    kill $EMULATOR_PID 2>/dev/null || true
    exit 1
fi

# Wait for device to be fully ready
adb wait-for-device

# Wait for boot completion
while [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" != "1" ]; do
    sleep 1
done

log "Emulator ready!"
adb devices
