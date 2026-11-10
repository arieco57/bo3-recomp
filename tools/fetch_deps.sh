#!/bin/bash
# Third-party sources for building (not in the repository): pinned commits of the libraries the
# runtime builds with, FFmpeg 7.1 headers, and the static xma2-only FFmpeg for the Linux release.
# Android extras: tools/build_ffmpeg_android.sh, tools/build_turnip_android.sh.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p thirdparty
clone() {   # dir url commit
    if [ ! -d "$1/.git" ]; then git clone --filter=blob:none "$2" "$1"; fi
    git -C "$1" fetch -q origin "$3" 2>/dev/null || true
    git -C "$1" checkout -q "$3"
}
clone thirdparty/SDL3 https://github.com/libsdl-org/SDL.git a8589a8
clone thirdparty/glslang https://github.com/KhronosGroup/glslang.git 83f75db
clone thirdparty/Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git 88487f6
clone thirdparty/volk https://github.com/zeux/volk.git b8f91c0
clone thirdparty/libadrenotools https://github.com/bylaws/libadrenotools.git 8fae8ce
git -C thirdparty/libadrenotools submodule update --init --recursive -q
# FFmpeg 7.1.1: headers for the runtime (thirdparty/ffmpeg-src) + static xma2-only libraries
if [ ! -d thirdparty/ffmpeg-src/libavcodec ]; then
    mkdir -p thirdparty/ffmpeg-desktop-build
    [ -f thirdparty/ffmpeg-desktop-build/ffmpeg-7.1.1.tar.xz ] || curl -L -o thirdparty/ffmpeg-desktop-build/ffmpeg-7.1.1.tar.xz https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz
    tar xf thirdparty/ffmpeg-desktop-build/ffmpeg-7.1.1.tar.xz -C thirdparty/ffmpeg-desktop-build
    mkdir -p thirdparty/ffmpeg-src
    cp -r thirdparty/ffmpeg-desktop-build/ffmpeg-7.1.1/libavcodec thirdparty/ffmpeg-desktop-build/ffmpeg-7.1.1/libavutil thirdparty/ffmpeg-src/
fi
[ -f thirdparty/ffmpeg-desktop/lib/libavcodec.a ] || tools/build_ffmpeg_desktop.sh
echo "Dependencies ready. Next: tools/prepare.sh (needs your own copy of the game), then cmake."
