#!/bin/bash
# Release packages -> dist/:
#   BO3Recomp-<ver>-Linux-x64.tar.gz   bo3_mp + bo3_zm, launchers, instructions (no game files)
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
L="$OUT/BO3Recomp"
mkdir -p "$L/instalar"
for b in bo3_mp bo3_zm; do strip -o "$L/$b" build/$b; done
cat > "$L/jugar-multijugador.sh" <<'S'
#!/bin/sh
cd "$(dirname "$0")" && exec ./bo3_mp "$@"
S
cat > "$L/jugar-zombies.sh" <<'S'
#!/bin/sh
cd "$(dirname "$0")" && exec ./bo3_zm "$@"
S
chmod +x "$L"/*.sh "$L"/bo3_*
cp docs/LEEME-Linux.txt "$L/LEEME.txt"
cp docs/instalar-LEEME.txt "$L/instalar/LEEME.txt"
tar -C "$OUT" -czf "$OUT/BO3Recomp-$VER-Linux-x64.tar.gz" BO3Recomp
cp build-android/bo3recomp.apk "$OUT/BO3Recomp-$VER-Android.apk"
rm -rf "$L"
( cd "$OUT" && sha256sum BO3Recomp-* > SHA256SUMS.txt )
ls -la "$OUT"
