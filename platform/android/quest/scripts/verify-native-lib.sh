#!/bin/sh
# platform/android/quest/scripts/verify-native-lib.sh
#
# Automated dependency/symbol/ABI/manifest check for the Quest native shell,
# mirroring platform/apple/visionos/tabletop/scripts/verify-bundle.sh's role
# for visionOS: fails loudly (not silently) if a forbidden dependency slipped
# into libbz_quest_native.so, if the packaged Khronos OpenXR loader .so or
# its manifest merge (OPENXR permissions/runtime_broker queries) goes
# missing, if the manifest's headtracking/hand-tracking/supportedDevices/
# debuggable/min-max-SDK contract regresses, or if the ABI/entry-point
# contract this layer promises (arm64-v8a only, NativeActivity entry points
# present, no desktop main()) regresses. See docs/quest-tabletop.md.
#
# usage: verify-native-lib.sh path/to/app-debug.apk
set -eu

APK=${1:-}
if [ -z "$APK" ] || [ ! -f "$APK" ]; then
    echo "usage: $0 path/to/app-debug.apk" >&2
    exit 2
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    if [ -n "${ANDROID_HOME:-}" ] && [ -d "$ANDROID_HOME/ndk" ]; then
        ANDROID_NDK_HOME=$(find "$ANDROID_HOME/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n1)
    elif [ -d "$HOME/Library/Android/SDK/ndk" ]; then
        ANDROID_NDK_HOME=$(find "$HOME/Library/Android/SDK/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n1)
    fi
fi
if [ -z "${ANDROID_NDK_HOME:-}" ] || [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo "verify-native-lib.sh: could not resolve an installed NDK (set ANDROID_NDK_HOME)" >&2
    exit 1
fi

HOST_TAG=darwin-x86_64
case "$(uname -s)" in
    Linux) HOST_TAG=linux-x86_64 ;;
esac
LLVM_BIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST_TAG/bin"
READELF="$LLVM_BIN/llvm-readelf"
NM="$LLVM_BIN/llvm-nm"
for TOOL in "$READELF" "$NM"; do
    if [ ! -x "$TOOL" ]; then
        echo "verify-native-lib.sh: missing NDK tool $TOOL" >&2
        exit 1
    fi
done

# aapt2 (SDK build-tools) for the manifest/min-max-SDK checks below - same
# resolution fallback shape as ANDROID_NDK_HOME above (a real SDK is already
# required to have built this APK in the first place via Gradle/AGP, which
# itself depends on aapt2, so requiring it here adds no new toolchain
# dependency for anyone who can already run `make quest-assemble-debug`).
if [ -z "${AAPT2:-}" ]; then
    if [ -n "${ANDROID_HOME:-}" ] && [ -d "$ANDROID_HOME/build-tools" ]; then
        AAPT2=$(find "$ANDROID_HOME/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n1)/aapt2
    elif [ -d "$HOME/Library/Android/SDK/build-tools" ]; then
        AAPT2=$(find "$HOME/Library/Android/SDK/build-tools" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n1)/aapt2
    fi
fi
if [ -z "${AAPT2:-}" ] || [ ! -x "$AAPT2" ]; then
    echo "verify-native-lib.sh: could not resolve aapt2 (set ANDROID_HOME or AAPT2) - required for the manifest/min-max-SDK checks below" >&2
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- ABI: exactly one lib/ ABI directory, and it must be arm64-v8a --------
unzip -l "$APK" | awk '{print $4}' | grep '^lib/' | awk -F/ '{print $2}' | sort -u > "$TMP/abis"
if [ "$(cat "$TMP/abis")" != "arm64-v8a" ]; then
    echo "expected exactly one packaged ABI (arm64-v8a), got:" >&2
    cat "$TMP/abis" >&2
    exit 1
fi

SO=lib/arm64-v8a/libbz_quest_native.so
unzip -p "$APK" "$SO" > "$TMP/lib.so" 2>/dev/null ||
    { echo "missing $SO inside $APK" >&2; exit 1; }

# --- packaged loader: the Khronos OpenXR Android loader's own .so must
# actually be bundled inside the APK's lib/ dir (not merely DT_NEEDED-
# referenced by name below) - a Prefab/AAR packaging regression could drop
# the file while leaving the NEEDED entry, which xrInitializeLoaderKHR
# would only discover as a runtime dlopen() failure on-device. ------------
LOADER_SO=lib/arm64-v8a/libopenxr_loader.so
unzip -l "$APK" | grep -q " $LOADER_SO\$" ||
    { echo "$LOADER_SO is not packaged in $APK - the Khronos OpenXR loader AAR dependency must ship its own .so alongside $SO" >&2; exit 1; }

# --- NEEDED (DT_NEEDED) allowlist ------------------------------------------
# Only the Khronos OpenXR loader (Prefab), the NDK's Vulkan loader stub, the
# NDK's AAudio stub (layer 7's native audio sink - see bz_quest_audio.h),
# and the standard Bionic/NDK system libraries may appear here. Anything
# else (SDL2, an Apple/ObjC runtime, a desktop GL library, Meta's
# proprietary VrApi, Meta's Audio SDK, ...) fails the check.
"$READELF" -d "$TMP/lib.so" | awk '/NEEDED/ { gsub(/[\[\]]/, "", $NF); print $NF }' | sort -u > "$TMP/needed"
ALLOWED='^(libopenxr_loader\.so|libvulkan\.so|libaaudio\.so|libandroid\.so|liblog\.so|libz\.so|libm\.so|libdl\.so|libc\.so|libc\+\+_shared\.so)$'
if grep -Ev "$ALLOWED" "$TMP/needed" > "$TMP/disallowed-needed" 2>/dev/null && [ -s "$TMP/disallowed-needed" ]; then
    echo "disallowed shared library dependency in $SO:" >&2
    cat "$TMP/disallowed-needed" >&2
    exit 1
fi

# --- forbidden symbol scan (SDL2 / desktop GL / Apple-ObjC runtime) --------
"$NM" -D --undefined-only "$TMP/lib.so" > "$TMP/undefined-symbols" 2>/dev/null || true
if grep -Ei 'SDL_|glXGetProc|glBegin|glOrtho|wglCreateContext|objc_msgSend|OBJC_CLASS' "$TMP/undefined-symbols" > "$TMP/forbidden-symbols" 2>/dev/null \
        && [ -s "$TMP/forbidden-symbols" ]; then
    echo "forbidden SDL2/desktop-GL/Apple-ObjC symbol referenced by $SO:" >&2
    cat "$TMP/forbidden-symbols" >&2
    exit 1
fi

# --- no desktop main(), NativeActivity entry points present ----------------
# Release/stripped .so files only retain the dynamic symbol table (.dynsym),
# so query that (-D) rather than the full table, which stripDebugDebugSymbols
# already removed from the packaged library.
"$NM" -D "$TMP/lib.so" > "$TMP/dynamic-symbols" 2>/dev/null
if awk '{print $NF}' "$TMP/dynamic-symbols" | grep -Fxq 'main'; then
    echo "$SO links a desktop main() - common/main.c must never be part of the Quest source list" >&2
    exit 1
fi
for SYMBOL in ANativeActivity_onCreate android_main; do
    if ! awk '{print $NF}' "$TMP/dynamic-symbols" | grep -Fxq "$SYMBOL"; then
        echo "$SO is missing required entry point symbol: $SYMBOL" >&2
        exit 1
    fi
done

# --- manifest: OpenXR/passthrough/hand-tracking/debuggable/SDK contract ----
# aapt2 dump badging gives a curated summary (permissions/features/SDK
# versions); dump xmltree gives the full raw tree, needed for meta-data
# elements badging omits (com.oculus.supportedDevices) - see
# docs/quest-tabletop.md's acceptance-automation section for the full
# citation of what the Khronos OpenXR loader AAR merges into this manifest
# automatically (verified once against a real assembleDebug output).
"$AAPT2" dump badging "$APK" > "$TMP/badging" 2>/dev/null ||
    { echo "aapt2 dump badging failed for $APK" >&2; exit 1; }
"$AAPT2" dump xmltree "$APK" --file AndroidManifest.xml > "$TMP/xmltree" 2>/dev/null ||
    { echo "aapt2 dump xmltree failed for $APK" >&2; exit 1; }

if ! grep -q 'application-debuggable' "$TMP/badging"; then
    echo "$APK is not debuggable (android:debuggable=true is required - stage-wc3-data.sh's/acceptance-runner.sh's run-as-based staging assumes a debug build)" >&2
    exit 1
fi

MIN_SDK=$(sed -n "s/^minSdkVersion:'\([0-9]*\)'.*/\1/p" "$TMP/badging")
TARGET_SDK=$(sed -n "s/^targetSdkVersion:'\([0-9]*\)'.*/\1/p" "$TMP/badging")
[ "$MIN_SDK" = "29" ] || { echo "expected minSdkVersion 29 (app/build.gradle), got '$MIN_SDK'" >&2; exit 1; }
[ "$TARGET_SDK" = "32" ] || { echo "expected targetSdkVersion 32 (app/build.gradle), got '$TARGET_SDK'" >&2; exit 1; }

if ! grep -q "uses-feature: name='android.hardware.vr.headtracking'" "$TMP/badging"; then
    echo "missing required uses-feature android.hardware.vr.headtracking in $APK" >&2
    exit 1
fi
if ! grep -q "uses-feature-not-required: name='oculus.software.handtracking'" "$TMP/badging"; then
    echo "missing optional (required=false) uses-feature oculus.software.handtracking in $APK - hand tracking must stay optional, never required for startup" >&2
    exit 1
fi
if ! grep -q "uses-permission: name='com.oculus.permission.HAND_TRACKING'" "$TMP/badging"; then
    echo "missing com.oculus.permission.HAND_TRACKING permission in $APK" >&2
    exit 1
fi

# The Khronos OpenXR loader AAR merges its own OPENXR/OPENXR_SYSTEM
# permissions and a <queries> runtime-broker provider/intent block into the
# final manifest automatically - their presence proves the loader's own
# manifest requirements were actually pulled in by the Gradle dependency,
# not just compiled against. Passthrough (XR_FB_passthrough) requires NO
# manifest declaration at all (unlike hand tracking) per Meta's own
# native-manifest guidance - there is deliberately no check for one here.
if ! grep -q "org.khronos.openxr.permission.OPENXR" "$TMP/xmltree"; then
    echo "missing org.khronos.openxr.permission.OPENXR/OPENXR_SYSTEM permissions in $APK - the Khronos OpenXR loader AAR's manifest merge did not apply" >&2
    exit 1
fi
if ! grep -q "org.khronos.openxr.runtime_broker" "$TMP/xmltree"; then
    echo "missing org.khronos.openxr.runtime_broker <queries> provider in $APK - the Khronos OpenXR loader AAR's manifest merge did not apply" >&2
    exit 1
fi
if ! grep -q '"com.oculus.supportedDevices"' "$TMP/xmltree" || ! grep -q 'quest3' "$TMP/xmltree"; then
    echo "missing com.oculus.supportedDevices meta-data (quest3|quest3s) in $APK" >&2
    exit 1
fi

echo "verified quest arm64-v8a native library + manifest: $SO"
