#!/bin/bash
# Build the minimal static ffmpeg the media pipeline links against.
#
# Only what MSE playback of YouTube needs is enabled: the mov (fragmented
# MP4) and matroska (WebM) demuxers, the h264 / vp9 / aac / opus decoders and
# their parsers, swscale for YUV -> RGBA and swresample (opus needs it).
# No programs, devices, filters, network or external libraries; LGPL (no
# --enable-gpl).  The hevc parser is there only because 7.1's h264 SEI code
# needs aom_film_grain.o, which the build compiles for HEVC SEI alone.
#
# Output: third_party/ffmpeg/install/{include,lib} (git-ignored).  CMake uses
# it when present (CYBER_FFMPEG_DIR), and builds without video decoding
# otherwise.
#
# Usage (from Git Bash or an MSYS2 shell):  tools/build_ffmpeg.sh
set -euo pipefail

FFMPEG_VERSION=7.1.1
FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# configure needs one consistent POSIX toolset: run under MSYS2's own bash
# (Git Bash's runtime mangles the temp paths MSYS2's sed and chmod see).
if [ -z "${CYBER_FFMPEG_IN_MSYS2:-}" ] && [ -x /c/msys64/usr/bin/bash ]; then
    exec env CYBER_FFMPEG_IN_MSYS2=1 MSYSTEM=MINGW64 CHERE_INVOKING=1 \
        /c/msys64/usr/bin/bash -lc "cd '$ROOT' && tools/build_ffmpeg.sh"
fi

TP="$ROOT/third_party/ffmpeg"
SRC="$TP/ffmpeg-$FFMPEG_VERSION"
BUILD="$TP/build"
PREFIX="$TP/install"
TARBALL="$TP/ffmpeg-$FFMPEG_VERSION.tar.xz"

export PATH="/mingw64/bin:/usr/bin:$PATH"
mkdir -p "$TP/tmp"
export TMPDIR="$TP/tmp"

if [ ! -f "$TARBALL" ]; then
    curl -fL -o "$TARBALL.part" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$FFMPEG_SHA256  $TARBALL" | sha256sum -c -
[ -d "$SRC" ] || tar -xJf "$TARBALL" -C "$TP"

mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" \
    --prefix="$PREFIX" \
    --target-os=mingw32 --arch=x86_64 \
    --cc=gcc --ar=ar --ranlib=ranlib \
    --enable-static --disable-shared \
    --disable-programs --disable-doc --disable-debug \
    --disable-autodetect --disable-network \
    --disable-avdevice --disable-avfilter \
    --disable-x86asm \
    --disable-everything \
    --enable-avformat --enable-avcodec --enable-swscale --enable-swresample \
    --enable-demuxer=mov,matroska \
    --enable-decoder=h264,vp9,aac,opus \
    --enable-parser=h264,vp9,aac,opus,hevc \
    --enable-bsf=vp9_superframe_split \
    --enable-w32threads
make -j"$(nproc)"
make install
echo "ffmpeg $FFMPEG_VERSION installed to $PREFIX"
