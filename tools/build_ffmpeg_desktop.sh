#!/bin/bash
# FFmpeg 7.1 (the headers in thirdparty/ffmpeg-src) for the Linux release build: static libraries
# with only the XMA2 decoder -> thirdparty/ffmpeg-desktop/{lib,include}. CMake links it when it
# exists, so the executables do not depend on the system's FFmpeg version.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER=7.1.1
WORK="$ROOT/thirdparty/ffmpeg-desktop-build"
OUT="$ROOT/thirdparty/ffmpeg-desktop"
mkdir -p "$WORK" "$OUT"
cd "$WORK"
[ -f ffmpeg-$VER.tar.xz ] || cp "$ROOT/thirdparty/ffmpeg-android-build/ffmpeg-$VER.tar.xz" . 2>/dev/null || curl -L -o ffmpeg-$VER.tar.xz https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz
[ -d ffmpeg-$VER ] || tar xf ffmpeg-$VER.tar.xz
cd ffmpeg-$VER
./configure --prefix="$OUT" --cc=clang --enable-static --disable-shared --disable-programs --disable-doc --disable-everything \
    --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-postproc --disable-avfilter \
    --disable-network --enable-decoder=xma2 --enable-pic --disable-x86asm --disable-autodetect
make -j"$(nproc)"
make install
echo "FFmpeg (static, xma2 only) in $OUT"
