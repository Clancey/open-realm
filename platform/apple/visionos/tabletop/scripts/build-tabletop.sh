#!/bin/sh
set -eu

PLATFORM=${1:-}
case "$PLATFORM" in
    xrsimulator)
        SDK=xrsimulator
        TRIPLE=arm64-apple-xros2.0-simulator
        SUPPORTED_PLATFORM=XRSimulator
        ;;
    xros)
        SDK=xros
        TRIPLE=arm64-apple-xros2.0
        SUPPORTED_PLATFORM=XROS
        ;;
    *)
        echo "usage: $0 xrsimulator|xros" >&2
        exit 2
        ;;
esac

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../../.." && pwd)
TABLETOP="$ROOT/platform/apple/visionos/tabletop"
BUILD="$ROOT/build/visionos/tabletop/$PLATFORM"
APP="$BUILD/OpenRealmTabletop.app"
EXECUTABLE=OpenRealmTabletop
SDK_PATH=$(xcrun --sdk "$SDK" --show-sdk-path)
ENGINE="$ROOT/build/lib/visionos/$PLATFORM/libopenwarcraft3-engine.a"
BRIDGE="$ROOT/build/lib/visionos/$PLATFORM/libopenwarcraft3-bridge.a"
WC3_DATA_TOOL="$ROOT/platform/apple/visionos/scripts/wc3_data.sh"

if [ ! -f "$ENGINE" ] || [ ! -f "$BRIDGE" ]; then
    echo "missing layer-2 engine/bridge archives for $PLATFORM" >&2
    exit 1
fi

rm -rf "$APP"
mkdir -p "$APP/Resources"
cp "$TABLETOP/Info.plist" "$APP/Info.plist"
plutil -replace CFBundleSupportedPlatforms -json "[\"$SUPPORTED_PLATFORM\"]" "$APP/Info.plist"
plutil -replace DTPlatformName -string "$SDK" "$APP/Info.plist"

LINK_FLAGS=
if [ "$PLATFORM" = xros ]; then
    LINK_FLAGS="-Xlinker -no_adhoc_codesign"
fi

# shellcheck disable=SC2086
xcrun --sdk "$SDK" swiftc -parse-as-library -O -target "$TRIPLE" -sdk "$SDK_PATH" \
    -module-name OpenRealmTabletop \
    -I "$TABLETOP/bridge" \
    -framework SwiftUI -framework RealityKit \
    -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker "$APP/Info.plist" \
    $LINK_FLAGS \
    "$TABLETOP"/app/*.swift \
    "$BRIDGE" "$ENGINE" -framework Foundation -lc++ -lpthread -lz \
    -o "$APP/$EXECUTABLE"

"$WC3_DATA_TOOL" stage "$APP"
if [ -n "${BZ_TABLETOP_RESOURCE_HOOK:-}" ]; then
    if [ ! -x "$BZ_TABLETOP_RESOURCE_HOOK" ]; then
        echo "tabletop resource hook is not executable: $BZ_TABLETOP_RESOURCE_HOOK" >&2
        exit 1
    fi
    "$BZ_TABLETOP_RESOURCE_HOOK" "$APP/Resources"
fi

if [ "$PLATFORM" = xrsimulator ]; then
    codesign --force --sign - --identifier org.openrealm.visionos.tabletop --timestamp=none "$APP"
fi
"$SCRIPT_DIR/verify-bundle.sh" "$PLATFORM" "$APP"
printf '%s\n' "$APP"
