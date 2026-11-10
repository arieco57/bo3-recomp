#!/bin/bash
# Builds bo3recomp.apk from android/ (manifest, SDL Java glue, two launcher activities) and the
# native libraries of build-android (libbo3_zm.so / libbo3_mp.so) + FFmpeg, with the SDK tools
# directly (aapt2, javac, d8, zipalign, apksigner) - no Gradle.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${ANDROID_HOME:-$HOME/Android/Sdk}"
BT="$SDK/build-tools/$(ls "$SDK/build-tools" | sort -V | tail -1)"
JAR="$SDK/platforms/$(ls "$SDK/platforms" | sort -V | tail -1)/android.jar"
NDK="${ANDROID_NDK_HOME:-$SDK/ndk/$(ls "$SDK/ndk" | sort -V | tail -1)}"
OUT="$ROOT/build-android/apk"
rm -rf "$OUT" && mkdir -p "$OUT/res" "$OUT/gen" "$OUT/classes" "$OUT/dex" "$OUT/lib/arm64-v8a"

"$BT/aapt2" compile --dir "$ROOT/android/res" -o "$OUT/res/res.zip"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$JAR" --manifest "$ROOT/android/AndroidManifest.xml" \
    --min-sdk-version 29 --target-sdk-version 35 --java "$OUT/gen" "$OUT/res/res.zip"
mapfile -d '' SRCS < <(find "$ROOT/android/src" "$OUT/gen" -name '*.java' -print0)
javac --release 17 -nowarn -classpath "$JAR" -d "$OUT/classes" "${SRCS[@]}" 2>&1 | grep -v "^Note:" || true
mapfile -d '' CLASSES < <(find "$OUT/classes" -name '*.class' -print0)
"$BT/d8" --release --min-api 29 --lib "$JAR" --output "$OUT/dex" "${CLASSES[@]}"

for so in libbo3_zm.so libbo3_mp.so; do
    [ -f "$ROOT/build-android/$so" ] && cp "$ROOT/build-android/$so" "$OUT/lib/arm64-v8a/"
done
cp "$ROOT/thirdparty/ffmpeg-android/lib/libavcodec.so" "$ROOT/thirdparty/ffmpeg-android/lib/libavutil.so" "$OUT/lib/arm64-v8a/"
# Turnip + libadrenotools' hook libraries (loaded from the native library folder)
cp "$ROOT/thirdparty/turnip-android/libvulkan_freedreno.so" "$OUT/lib/arm64-v8a/"
find "$ROOT/build-android/thirdparty/libadrenotools" -name 'lib*hook*.so' -exec cp {} "$OUT/lib/arm64-v8a/" \;
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
for so in "$OUT"/lib/arm64-v8a/*.so; do "$STRIP" --strip-unneeded "$so"; done

cp "$OUT/base.apk" "$OUT/unsigned.apk"
(cd "$OUT/dex" && zip -q -j "$OUT/unsigned.apk" classes.dex)
(cd "$OUT" && zip -q -r "$OUT/unsigned.apk" lib)
"$BT/zipalign" -p -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"

KS="$HOME/.android/debug.keystore"
if [ ! -f "$KS" ]; then
    mkdir -p "$HOME/.android"
    keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
"$BT/apksigner" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android --out "$ROOT/build-android/bo3recomp.apk" "$OUT/aligned.apk"
ls -la "$ROOT/build-android/bo3recomp.apk"
