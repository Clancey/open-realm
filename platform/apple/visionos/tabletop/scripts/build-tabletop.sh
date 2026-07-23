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
APP="$BUILD/OpenRealmTabletopFixture.app"
EXECUTABLE=OpenRealmTabletopFixture
SDK_PATH=$(xcrun --sdk "$SDK" --show-sdk-path)

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
    -module-name OpenRealmTabletopFixture \
    -framework SwiftUI -framework RealityKit \
    -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker "$APP/Info.plist" \
    $LINK_FLAGS \
    "$TABLETOP"/app/*.swift \
    -o "$APP/$EXECUTABLE"

# The data-layer lane may supply a hook later; this fixture lane deliberately copies no game archives itself.
if [ -n "${BZ_TABLETOP_RESOURCE_HOOK:-}" ]; then
    if [ ! -x "$BZ_TABLETOP_RESOURCE_HOOK" ]; then
        echo "tabletop resource hook is not executable: $BZ_TABLETOP_RESOURCE_HOOK" >&2
        exit 1
    fi
    "$BZ_TABLETOP_RESOURCE_HOOK" "$APP/Resources"
fi

if [ "$PLATFORM" = xrsimulator ]; then
    codesign --force --sign - --identifier org.openrealm.tabletop.fixture --timestamp=none "$APP"
fi
"$SCRIPT_DIR/verify-bundle.sh" "$PLATFORM" "$APP"
printf '%s\n' "$APP"
