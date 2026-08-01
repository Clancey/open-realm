#!/bin/sh
# platform/android/quest/scripts/verify-native-lib.sh
#
# Automated dependency/symbol/ABI check for the Quest native shell, mirroring
# platform/apple/visionos/tabletop/scripts/verify-bundle.sh's role for
# visionOS: fails loudly (not silently) if a forbidden dependency slipped
# into libbz_quest_native.so, or if the ABI/entry-point contract this layer
# promises (arm64-v8a only, NativeActivity entry points present, no desktop
# main()) regresses. See docs/quest-tabletop.md.
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

echo "verified quest arm64-v8a native library: $SO"
