#!/usr/bin/env bash
# Build a game's Android APK on macOS/Linux (the same steps as build_apk.ps1): cmake (NDK) -> lib<game>.so -> aapt2 ->
# zipalign -> apksigner. Needs the Android SDK (ANDROID_HOME, default ~/Library/Android/sdk) with NDK 28.2 and
# build-tools 35, Vulkan headers (VULKAN_SDK with Include/, e.g. $(brew --prefix vulkan-headers)), cmake and ninja.
# Usage: android/build_apk.sh [--game <game>] [--nce] [--install]   (game: a folder under games/, default hades)
#
# A game with android/<game>/AndroidManifest.xml (wonder) gets its launcher: the Java in android/<game>/java (javac + d8),
# res/, assets/, the on-device importer android/<game>/importer -> libnximport.so, and the translated shaders
# (private/<game>/spirv -> assets/spirv.zip) in the APK. LAUNCHER_ONLY=1 builds without lib<game>.so (the UI and the
# import can be tried, Play cannot start the game). The APK carries shaders derived from the player's dump: for private
# use only. No game data, keys or dump content goes into it.
#
# --nce  native execution (CMake NX_NCE), the same build as build_apk.ps1 -Nce: the APK holds no game code, it runs
#        the exefs of the player's own dump (build in private/<game>/ba_nce, APK in private/<game>/apk_nce). This is
#        the P5R build: no private/<game>/spirv is needed (the shaders are translated on the device), and the APK also
#        carries lib<game>import.so (the on-device importer), libadrenotools' four hook libraries and, when it has been
#        built, libspirvharness.so (games/p5r/tools/build_harness_android.sh). Without the harness the APK still
#        builds and installs, but "Prepare the game" cannot translate the shaders; see the warning at the end.
#        The debug keystore is created if ~/.android/debug.keystore is missing.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
game=hades
install=0
nce=0
while [ $# -gt 0 ]; do
    case "$1" in
        --game) game="$2"; shift 2 ;;
        --nce) nce=1; shift ;;
        --install) install=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
# Hades keeps the layout from before there were other games (private/apk, private/ba).
if [ "$game" = hades ]; then priv="$root/private"; else priv="$root/private/$game"; fi
# -Nce of build_apk.ps1: its own build directory and its own output directory, so a normal and an NCE build coexist.
if [ "$nce" = 1 ]; then
    out="$priv/apk_nce"
    build="$priv/ba_nce"
else
    out="$priv/apk"
    build="$priv/ba"
fi
sdk="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ndk="$sdk/ndk/28.2.13676358"
bt="$sdk/build-tools/35.0.0"
jar="$sdk/platforms/android-35/android.jar"
host="$(uname -s | tr '[:upper:]' '[:lower:]')-x86_64"
export VULKAN_SDK="${VULKAN_SDK:-$(brew --prefix vulkan-headers 2>/dev/null || true)}"

# The libraries packed next to lib<game>.so. The P5R native build prepares the game on the phone (libp5rimport.so) with
# the shader translator (libspirvharness.so, built by games/p5r/tools/build_harness_android.sh) and can open a custom
# GPU driver (libadrenotools' hooks, loaded by name from the app's nativeLibraryDir).
extra_libs=()
targets=("$game")
if [ "$nce" = 1 ]; then
    if [ "$game" = p5r ]; then
        harness="$root/private/build/spirv_harness_android/libspirvharness.so"
        targets+=("p5rimport")
        extra_libs+=("$build/libp5rimport.so")
        if [ -f "$harness" ]; then
            extra_libs+=("$harness")
        fi
        for h in libhook_impl.so libmain_hook.so libfile_redirect_hook.so libgsl_alloc_hook.so; do
            extra_libs+=("$build/adrenotools/src/hook/$h")
        done
    fi
fi

launcher=0
[ -f "$root/android/$game/AndroidManifest.xml" ] && launcher=1
if [ "$launcher" = 1 ] && [ "${LAUNCHER_ONLY:-0}" = 1 ]; then
    echo "LAUNCHER_ONLY: no game library (Play will not start the game)"
    skip_native=1
else
    skip_native=0
fi
if [ "$skip_native" = 0 ] && [ ! -f "$build/build.ninja" ]; then
    cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DNX_GAME="$game" \
        -DNX_NCE="$([ "$nce" = 1 ] && echo ON || echo OFF)"
fi
if [ "$skip_native" = 0 ]; then cmake --build "$build" --target "${targets[@]}"; fi

rm -rf "$out"
mkdir -p "$out/lib/arm64-v8a"
if [ "$skip_native" = 0 ]; then
    "$ndk/toolchains/llvm/prebuilt/$host/bin/llvm-strip" --strip-unneeded -o "$out/lib/arm64-v8a/lib$game.so" "$build/lib$game.so"
    for lib in ${extra_libs+"${extra_libs[@]}"}; do
        "$ndk/toolchains/llvm/prebuilt/$host/bin/llvm-strip" --strip-unneeded \
            -o "$out/lib/arm64-v8a/$(basename "$lib")" "$lib"
    done
fi

# ---- native execution (--nce): the manifest, the launcher's Java, the res and the icon --------------------------------
# games/<game>/AndroidManifest.xml (not android/<game>/: that one is wonder's launcher, with its own importer and assets),
# the shared android/res, the project's icon (games/<game>/icon/res) and games/<game>/java. Same steps as
# build_apk.ps1 -Nce.
if [ "$nce" = 1 ]; then
    if [ "$skip_native" = 0 ] && [ ! -f "$out/lib/arm64-v8a/lib$game.so" ]; then
        echo "missing $out/lib/arm64-v8a/lib$game.so" >&2; exit 1
    fi
    # apksigner and d8 need a JDK; javac builds the launcher (games/<game>/java).
    if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac" ]; then
        javac="$JAVA_HOME/bin/javac"
    elif command -v javac >/dev/null 2>&1; then
        javac="$(command -v javac)"
    else
        echo "a JDK is needed for games/$game/java (JAVA_HOME, or javac on the PATH)" >&2; exit 1
    fi
    st="$out/stage"
    mkdir -p "$st/gen" "$st/classes" "$st/dex"
    manifest="$root/games/$game/AndroidManifest.xml"
    res_zips=()
    "$bt/aapt2" compile --dir "$root/android/res" -o "$out/res.zip"
    res_zips+=("$out/res.zip")
    # The launcher icon: the project's own (games/<game>/icon/res), else the game's, when the player made it from their
    # dump (private/<game>/icon/res). Compiled with the rest; the manifest copy names it.
    icon_res="$root/games/$game/icon/res"
    [ -d "$icon_res" ] || icon_res="$priv/icon/res"
    if [ -d "$icon_res" ]; then
        "$bt/aapt2" compile --dir "$icon_res" -o "$out/icon.zip"
        res_zips+=("$out/icon.zip")
        sed 's/<application/<application android:icon="@mipmap\/ic_launcher"/' "$manifest" > "$out/AndroidManifest.xml"
        manifest="$out/AndroidManifest.xml"
    fi
    "$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$manifest" "${res_zips[@]}"
    # The Java of the game's own (games/<game>/java: Launcher.java and the rest): javac against android.jar, d8 to dex.
    if [ -d "$root/games/$game/java" ]; then
        find "$root/games/$game/java" -name '*.java' > "$st/sources.txt"
        "$javac" -nowarn -encoding UTF-8 -source 1.8 -target 1.8 -Xlint:-options -bootclasspath "$jar" \
            -d "$st/classes" @"$st/sources.txt"
        (cd "$st/classes" && find . -name '*.class' > ../classes.txt)
        (cd "$st" && "$bt/d8" --release --min-api 30 --lib "$jar" --output dex @classes.txt)
        (cd "$st" && zip -q -j "$out/base.apk" dex/classes.dex)
    fi
    # The native libraries: compressed, so extractNativeLibs (the manifest) unpacks them and no page alignment is needed.
    (cd "$out" && zip -q -r -9 base.apk lib)
    "$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
    # A debug keystore, as build_apk.ps1 expects one: created here when the machine has none.
    ks="$HOME/.android/debug.keystore"
    if [ ! -f "$ks" ]; then
        mkdir -p "$HOME/.android"
        keytool -genkeypair -keystore "$ks" -storepass android -keypass android -alias androiddebugkey \
            -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000 >/dev/null
        echo "created $ks"
    fi
    "$bt/apksigner" sign --ks "$ks" --ks-pass pass:android --key-pass pass:android \
        --out "$out/$game.apk" "$out/aligned.apk"
    "$bt/apksigner" verify "$out/$game.apk"
    echo "APK: $out/$game.apk ($(du -h "$out/$game.apk" | cut -f1))"
    if [ "$game" = p5r ] && [ ! -f "$root/private/build/spirv_harness_android/libspirvharness.so" ]; then
        echo "WARNING: the APK has no libspirvharness.so, so \"Prepare the game\" cannot translate the shaders" >&2
        echo "WARNING: (the game itself still starts once spirv/ is in place). Build it once and re-run this script:" >&2
        echo "WARNING:   docker run --rm -v \"\$PWD:/src\" -w /src mcr.microsoft.com/dotnet/sdk:10.0 sh games/p5r/tools/build_harness_android.sh" >&2
        echo "WARNING: needs the Ryubing source in private/third_party/ryubing (docs/nvn/TRANSLATOR.md)." >&2
    fi
    if [ "$install" = 1 ]; then "$sdk/platform-tools/adb" install -r "$out/$game.apk"; fi
    exit 0
fi

if [ "$launcher" = 1 ]; then
    llvm="$ndk/toolchains/llvm/prebuilt/$host/bin"
    la="$root/android/$game"
    pkg=io.github.belmantegu.wonderrecomp
    st="$out/stage"
    mkdir -p "$st/gen" "$st/classes" "$st/assets"
    # the on-device importer (JNI class: $pkg.NativeImporter)
    "$llvm/clang++" --target=aarch64-linux-android30 -std=c++17 -O2 -fPIC -shared -static-libstdc++ \
        -fvisibility=hidden -Wl,-z,max-page-size=16384 -Wl,--gc-sections -ffunction-sections \
        -o "$out/lib/arm64-v8a/libnximport.so" "$la/importer/nximport_jni.cpp" "$la/importer/nximport.cpp" \
        "$la/importer/nxcrypto.cpp" -llog
    "$llvm/llvm-strip" --strip-unneeded "$out/lib/arm64-v8a/libnximport.so"
    # assets: launcher art, font, licences, and the translated shaders (zip) with their id
    cp -R "$la/assets/." "$st/assets/"
    spirv="$priv/spirv"
    if [ -f "$spirv/results.tsv" ]; then
        python3 -I - "$spirv" "$st/assets/spirv.zip" "$st/assets/spirv.id" <<'PY'
import hashlib, os, sys, zipfile
src, dst, idf = sys.argv[1:4]
names = sorted(n for n in os.listdir(src) if n.endswith('.spv') or n == 'results.tsv')
h = hashlib.sha256()
with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
    for n in names:
        data = open(os.path.join(src, n), 'rb').read()
        h.update(n.encode() + b'\0' + data)
        z.writestr(n, data)
open(idf, 'w').write(h.hexdigest()[:16] + '\n')
print(f'spirv.zip: {len(names)} files, {os.path.getsize(dst) / 1e6:.1f} MB, id {h.hexdigest()[:16]}')
PY
    else
        echo "WARNING: no $spirv/results.tsv: the APK has no shaders (the game will not start; docs/nvn/TRANSLATOR.md)"
    fi
    "$bt/aapt2" compile --dir "$la/res" -o "$st/res.zip"
    "$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$la/AndroidManifest.xml" --java "$st/gen" -A "$st/assets" \
        --min-sdk-version 30 --target-sdk-version 35 "$st/res.zip"
    find "$la/java" "$st/gen" -name '*.java' > "$st/sources.txt"
    javac -nowarn -encoding UTF-8 --release 11 -cp "$jar" -d "$st/classes" @"$st/sources.txt"
    (cd "$st/classes" && find . -name '*.class' > ../classes.txt && "$bt/d8" --release --min-api 30 --lib "$jar" --output .. @../classes.txt)
    (cd "$st" && zip -q -j "$out/base.apk" classes.dex)
    (cd "$out" && zip -q -r -9 base.apk lib)
    "$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
    "$bt/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android --key-pass pass:android \
        --out "$out/$game.apk" "$out/aligned.apk"
    "$bt/apksigner" verify "$out/$game.apk"
    echo "APK: $out/$game.apk ($(du -h "$out/$game.apk" | cut -f1))"
    if [ "$install" = 1 ]; then "$sdk/platform-tools/adb" install -r "$out/$game.apk"; fi
    exit 0
fi

"$bt/aapt2" compile --dir "$root/android/res" -o "$out/res.zip"
"$bt/aapt2" link -o "$out/base.apk" -I "$jar" --manifest "$root/games/$game/AndroidManifest.xml" "$out/res.zip"
(cd "$out" && zip -q -9 base.apk "lib/arm64-v8a/lib$game.so")
"$bt/zipalign" -f -p 4 "$out/base.apk" "$out/aligned.apk"
"$bt/apksigner" sign --ks "$HOME/.android/debug.keystore" --ks-pass pass:android --key-pass pass:android \
    --out "$out/$game.apk" "$out/aligned.apk"
echo "APK: $out/$game.apk ($(du -h "$out/$game.apk" | cut -f1))"

if [ "$install" = 1 ]; then "$sdk/platform-tools/adb" install -r "$out/$game.apk"; fi
