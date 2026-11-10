#!/bin/bash
# FFmpeg 7.1 (libavcodec 61.19 / libavutil 59.39, the headers in thirdparty/ffmpeg-src) for Android
# arm64 with only the XMA2 decoder -> thirdparty/ffmpeg-android/{lib,include}.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/29.0.14206865}"
API=29
VER=7.1.1
WORK="$ROOT/thirdparty/ffmpeg-android-build"
OUT="$ROOT/thirdparty/ffmpeg-android"
mkdir -p "$WORK" "$OUT"
cd "$WORK"
[ -f ffmpeg-$VER.tar.xz ] || curl -L -o ffmpeg-$VER.tar.xz https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz
[ -d ffmpeg-$VER ] || tar xf ffmpeg-$VER.tar.xz
cd ffmpeg-$VER
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
./configure --prefix="$OUT" --target-os=android --arch=aarch64 --enable-cross-compile \
    --cc="$TC/aarch64-linux-android$API-clang" --cxx="$TC/aarch64-linux-android$API-clang++" \
    --ar="$TC/llvm-ar" --nm="$TC/llvm-nm" --ranlib="$TC/llvm-ranlib" --strip="$TC/llvm-strip" \
    --enable-shared --disable-static --disable-programs --disable-doc --disable-everything \
    --disable-avdevice --disable-avformat --disable-swscale --disable-swresample --disable-postproc --disable-avfilter \
    --disable-network --enable-decoder=xma2 --enable-pic
make -j"$(nproc)"
make install
echo "FFmpeg for Android in $OUT"
