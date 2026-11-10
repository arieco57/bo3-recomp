#!/bin/bash
# Turnip (Mesa's Vulkan driver for Adreno 6xx/7xx, KGSL kernel interface) for Android arm64 ->
# thirdparty/turnip-android/libvulkan_freedreno.so (loaded with libadrenotools: the stock
# Adreno 618 driver is Vulkan 1.1, the renderer needs 1.3).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-$HOME/Android/Sdk/ndk/29.0.14206865}"
MESA="${MESA_SRC:-$HOME/Android/mesa}"
VENV="$HOME/Android/mesa-venv"
API=29
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
OUT="$ROOT/thirdparty/turnip-android"
BUILD="$MESA/build-android"
mkdir -p "$OUT"
CROSS="$MESA/android-aarch64.ini"
cat > "$CROSS" <<EOT
[binaries]
ar = '$TC/llvm-ar'
c = ['$TC/aarch64-linux-android$API-clang']
cpp = ['$TC/aarch64-linux-android$API-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$TC/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=/nonexistent', '/usr/bin/pkg-config']

[built-in options]
cpp_link_args = ['-static-libstdc++']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'
EOT
export PATH="$VENV/bin:$PATH"
cd "$MESA"
# our KGSL patch: VK_EXT_external_memory_host via user-memory import (cached guest memory)
if ! git apply --check -R "$ROOT/tools/patches/turnip-kgsl-host-import.patch" 2>/dev/null; then
    git apply "$ROOT/tools/patches/turnip-kgsl-host-import.patch"
fi
if [ ! -d "$BUILD" ]; then
    meson setup "$BUILD" --cross-file "$CROSS" -Dbuildtype=release -Dplatforms=android -Dplatform-sdk-version=$API \
        -Dandroid-stub=true -Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=kgsl -Dvulkan-beta=true \
        -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false -Dglx=disabled \
        -Dllvm=disabled -Dzstd=disabled -Dandroid-libbacktrace=disabled -Db_lto=false -Dstrip=true
fi
ninja -C "$BUILD" src/freedreno/vulkan/libvulkan_freedreno.so
cp "$BUILD/src/freedreno/vulkan/libvulkan_freedreno.so" "$OUT/"
ls -la "$OUT/libvulkan_freedreno.so"
