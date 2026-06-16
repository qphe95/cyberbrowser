#!/bin/bash
# Stop the bgmdwldr emulator
# Usage: ./scripts/stop-emulator.sh

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log() { echo -e "${GREEN}[EMU]${NC} $1"; }
info() { echo -e "${BLUE}[INFO]${NC} $1"; }

log "Stopping emulator..."

# Try graceful shutdown first
adb devices | grep emulator | awk '{print $1}' | while read device; do
    info "Killing $device..."
    adb -s $device emu kill 2>/dev/null || true
done

# Kill from PID file if exists
if [ -f /tmp/bgmdwldr_emulator.pid ]; then
    PID=$(cat /tmp/bgmdwldr_emulator.pid)
    if kill -0 $PID 2>/dev/null; then
        info "Killing emulator process $PID"
        kill -9 $PID 2>/dev/null || true
    fi
    rm -f /tmp/bgmdwldr_emulator.pid
fi

# Force kill any remaining emulator processes
pkill -9 -f "qemu-system.*bgmdwldr" 2>/dev/null || true
pkill -9 -f "emulator.*bgmdwldr" 2>/dev/null || true

# Kill ADB server
adb kill-server 2>/dev/null || true

log "Emulator stopped"
