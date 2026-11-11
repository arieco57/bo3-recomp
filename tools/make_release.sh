#!/bin/bash
# Release packages -> dist/:
#   BO3Recomp-{mp,zm}-<ver>-x86_64.AppImage   Linux, one AppImage per mode (no game files)
#   BO3Recomp-<ver>-Android.apk        Multiplayer + Zombies (one app, two icons, one copy of the data)
# The online backend (Supabase URL + publishable key) comes from release.conf (not in git).
#   tools/make_release.sh v0.1.0
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
VER="${1:?usage: tools/make_release.sh <version>}"
[ -f release.conf ] && . ./release.conf
DEFS=(-DBO3_DEFAULT_SUPABASE_URL="${SUPABASE_URL:-}" -DBO3_DEFAULT_SUPABASE_KEY="${SUPABASE_KEY:-}")

echo "==> Linux"
cmake -S . -B build "${DEFS[@]}" > /dev/null
cmake --build build -j"$(nproc)"
echo "==> Android"
cmake -S . -B build-android "${DEFS[@]}" > /dev/null
cmake --build build-android -j"$(nproc)"
tools/package_android.sh > /dev/null

OUT=dist/$VER
rm -rf "$OUT"
mkdir -p "$OUT"

# Linux: one AppImage per mode (single file, double-click; the binaries need only glibc/libm).
# appimagetool is downloaded once into tools/cache/ (not in git).
AIT=tools/cache/appimagetool-x86_64.AppImage
if [ ! -x "$AIT" ]; then
    mkdir -p tools/cache
    curl -sL -o "$AIT" "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
    chmod +x "$AIT"
fi
for g in mp zm; do
    if [ $g = mp ]; then NAME="BO3 Recomp - Multijugador"; else NAME="BO3 Recomp - Zombies"; fi
    D=$(mktemp -d)/BO3Recomp-$g.AppDir
    mkdir -p "$D/usr/bin"
    strip -o "$D/usr/bin/bo3_$g" build/bo3_$g
    cp android/res/mipmap-xxxhdpi/ic_$g.png "$D/bo3recomp-$g.png"
    cp docs/appimage-AppRun.sh "$D/AppRun"
    sed -i "s/usr\/bin\/BIN/usr\/bin\/bo3_$g/" "$D/AppRun"
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=bo3_%s\nIcon=bo3recomp-%s\nCategories=Game;\nTerminal=false\n' "$NAME" $g $g > "$D/bo3recomp-$g.desktop"
    chmod +x "$D/AppRun" "$D/usr/bin/bo3_$g"
    ARCH=x86_64 "$AIT" --appimage-extract-and-run "$D" "$OUT/BO3Recomp-$g-$VER-x86_64.AppImage" > /dev/null
done
cp build-android/bo3recomp.apk "$OUT/BO3Recomp-$VER-Android.apk"
rm -rf "$L"
( cd "$OUT" && sha256sum BO3Recomp-* > SHA256SUMS.txt )
ls -la "$OUT"
