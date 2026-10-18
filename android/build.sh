#!/bin/sh
# Builds the phone files the router agent pushes:
#   build/android/remodroid-server.jar        Java part (dex), run with app_process
#   build/android/<abi>/libremodroid.so        native WebRTC stack (all libraries linked in)
# Needs: ANDROID_JAR (platform android.jar), D8 (build-tools d8), NDK (Android NDK root).
#   ./android/build.sh            jar + arm64-v8a + armeabi-v7a
set -eu
cd "$(dirname "$0")/.."
: "${ANDROID_JAR:?set ANDROID_JAR to platforms/android-34/android.jar}"
: "${D8:?set D8 to build-tools/<ver>/d8}"
: "${NDK:?set NDK to the Android NDK root}"
OUT=build/android
mkdir -p "$OUT/classes"

echo "==> java"
find android/java/src -name '*.java' > "$OUT/sources.txt"
javac -nowarn --release 8 -cp "$ANDROID_JAR" -d "$OUT/classes" @"$OUT/sources.txt" 2>&1 | grep -v "^warning: \[options\]\|^Note:\|^4 warnings\|^[0-9] warning" || true
[ -f "$OUT/classes/io/remodroid/Server.class" ] || { echo "javac failed"; exit 1; }
rm -f "$OUT"/dex/*.dex && mkdir -p "$OUT/dex"
"$D8" --release --min-api 26 --lib "$ANDROID_JAR" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
rm -f "$OUT/remodroid-server.jar" && jar cfM "$OUT/remodroid-server.jar" -C "$OUT/dex" classes.dex
ls -l "$OUT/remodroid-server.jar"

TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
for abi in ${ABIS:-arm64-v8a armeabi-v7a}; do
    case "$abi" in
        arm64-v8a)   cc="$TC/aarch64-linux-android26-clang" ;;
        armeabi-v7a) cc="$TC/armv7a-linux-androideabi26-clang" ;;
        x86_64)      cc="$TC/x86_64-linux-android26-clang" ;;
        *) echo "unknown ABI $abi"; exit 2 ;;
    esac
    echo "==> native $abi"
    make -j"$(nproc)" TARGET="android-$abi" CC="$cc" AR="$TC/llvm-ar" OPT=-Os build/android-$abi/rtc/librtc.a >/dev/null
    mkdir -p "$OUT/$abi"
    "$cc" -shared -o "$OUT/$abi/libremodroid.so" -Os -s -Wl,--gc-sections -Wl,--exclude-libs,ALL \
        -Iandroid/native -Icommon -Ithird_party/libev -Ithird_party/cjson -DEV_STANDALONE=1 \
        android/native/jni.c android/native/image.c build/android-$abi/rtc/librtc.a -llog -ldl -lm
    ls -l "$OUT/$abi/libremodroid.so"
done
