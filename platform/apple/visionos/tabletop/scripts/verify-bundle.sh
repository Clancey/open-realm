#!/bin/sh
set -eu

PLATFORM=${1:-}
APP=${2:-}
case "$PLATFORM" in
    xrsimulator) EXPECTED_PLATFORM=XRSimulator; EXPECTED_MACHO_PLATFORM=VISIONOSSIMULATOR ;;
    xros) EXPECTED_PLATFORM=XROS; EXPECTED_MACHO_PLATFORM=VISIONOS ;;
    *) echo "usage: $0 xrsimulator|xros app-bundle" >&2; exit 2 ;;
esac
if [ ! -d "$APP" ]; then
    echo "missing app bundle: $APP" >&2
    exit 1
fi

PLIST="$APP/Info.plist"
EXECUTABLE=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$PLIST")
BUNDLE_ID=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$PLIST")
BIN="$APP/$EXECUTABLE"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

expect_plist() {
    KEY=$1
    EXPECTED=$2
    ACTUAL=$(/usr/libexec/PlistBuddy -c "Print :$KEY" "$PLIST")
    if [ "$ACTUAL" != "$EXPECTED" ]; then
        echo "Info.plist $KEY: expected '$EXPECTED', got '$ACTUAL'" >&2
        exit 1
    fi
}

expect_plist CFBundleExecutable OpenRealmTabletopFixture
expect_plist CFBundleIdentifier org.openrealm.tabletop.fixture
expect_plist CFBundlePackageType APPL
expect_plist MinimumOSVersion 2.0
expect_plist UIDeviceFamily:0 7
expect_plist UIRequiredDeviceCapabilities:0 arm64
expect_plist UIApplicationSupportsIndirectInputEvents true
expect_plist UIApplicationSceneManifest:UIApplicationSupportsMultipleScenes true
expect_plist CFBundleSupportedPlatforms:0 "$EXPECTED_PLATFORM"

for KEY in NSHandsTrackingUsageDescription NSWorldSensingUsageDescription; do
    VALUE=$(/usr/libexec/PlistBuddy -c "Print :$KEY" "$PLIST")
    if [ -z "$VALUE" ]; then echo "Info.plist $KEY must be non-empty" >&2; exit 1; fi
done

case "$BUNDLE_ID:$EXECUTABLE" in
    *openwarcraft3*|*opensc2*|*OpenWarcraft3*|*OpenSC2*)
        echo "tabletop bundle identity collides with a desktop product" >&2
        exit 1
        ;;
esac

file "$BIN" > "$TMP/file"
grep -q "Mach-O 64-bit executable arm64" "$TMP/file"
xcrun vtool -show-build "$BIN" > "$TMP/build-version"
grep -Eq "^[[:space:]]*platform $EXPECTED_MACHO_PLATFORM$" "$TMP/build-version"
grep -Eq "^[[:space:]]*minos 2\\.0$" "$TMP/build-version"
xcrun otool -s __TEXT __info_plist "$BIN" > "$TMP/embedded-plist"
grep -q "Contents of (__TEXT,__info_plist) section" "$TMP/embedded-plist"
xcrun otool -L "$BIN" > "$TMP/dylibs"
awk 'NR > 1 && $1 !~ "^/System/Library/Frameworks/" && $1 !~ "^/usr/lib/" { print $1 }' \
    "$TMP/dylibs" > "$TMP/prohibited-dylibs"
if [ -s "$TMP/prohibited-dylibs" ]; then
    cat "$TMP/prohibited-dylibs" >&2
    echo "prohibited dynamic framework or developer library linked into tabletop app" >&2
    exit 1
fi
if find "$APP" -type d -name '*.framework' -print -quit | grep -q .; then
    echo "embedded dynamic frameworks are prohibited" >&2
    exit 1
fi
if find "$APP" -type f -iname '*.mpq' -print -quit | grep -q .; then
    echo "private MPQs are prohibited in the fixture shell bundle" >&2
    exit 1
fi
if /usr/bin/strings "$BIN" | grep -Eq '/Users/|/Volumes/|/Applications/Xcode'; then
    echo "absolute developer path found in tabletop executable" >&2
    exit 1
fi
if /usr/libexec/PlistBuddy -c "Print :UIApplicationSceneManifest:UISceneConfigurations" "$PLIST" \
        > "$TMP/scene-configurations" 2>/dev/null &&
        grep -Eiq 'SDL.*SceneDelegate|SDLUIKitDelegate' "$TMP/scene-configurations"; then
    echo "SDL scene delegates are prohibited" >&2
    exit 1
fi

if [ "$PLATFORM" = xrsimulator ]; then
    codesign --verify --deep --strict "$APP"
    codesign -d --verbose=4 "$APP" > "$TMP/codesign" 2>&1
    grep -q "Signature=adhoc" "$TMP/codesign"
    SIGNING_ID=$(awk -F= '$1 == "Identifier" { print $2; exit }' "$TMP/codesign")
    if [ "$SIGNING_ID" != "$BUNDLE_ID" ]; then
        echo "signing identifier '$SIGNING_ID' does not match '$BUNDLE_ID'" >&2
        exit 1
    fi
elif codesign -d "$APP" > "$TMP/codesign" 2>&1; then
    echo "xros fixture bundle must remain unsigned" >&2
    exit 1
fi

echo "verified $PLATFORM bundle: $BUNDLE_ID / $EXECUTABLE"
