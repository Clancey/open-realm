#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/tabletop-acceptance-patterns.sh"
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../../.." && pwd)
TABLETOP="$ROOT/platform/apple/visionos/tabletop"
OUT="$ROOT/build/tests/visionos-tabletop-pure"
PLIST="$TABLETOP/Info.plist"
ABI_CHECK="$ROOT/build/tests/visionos-tabletop-asset-import.swift"

mkdir -p "$(dirname -- "$OUT")"
trap 'rm -f "$ABI_CHECK"' EXIT
printf '%s\n' \
    'import OpenRealmTabletopBridge' \
    'let _: UInt32 = BZ_TTA_AbiVersion()' \
    'let _: bzTTTerrainTextureKind_t = BZ_TTA_TERRAIN_TEXTURE_WATER' \
    'let _: bzTTTeamTextureKind_t = BZ_TTA_TEAM_TEXTURE_GLOW' \
    'let _: UInt32 = BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION, BZ_TTA_TEAM_TEXTURE_COLOR)' \
    '_ = BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, BZ_TTA_TEAM_TEXTURE_GLOW, 0)' \
    'let _: bzTTAssetCategory_t = BZ_TTA_CATEGORY_ITEM' > "$ABI_CHECK"
xcrun swiftc -typecheck -I "$TABLETOP/bridge" -Xcc -I"$ROOT" "$ABI_CHECK"
xcrun swiftc -parse-as-library \
    "$TABLETOP/app/TabletopSnapshot.swift" \
    "$TABLETOP/app/TabletopCommand.swift" \
    "$TABLETOP/app/TabletopAdapter.swift" \
    "$TABLETOP/app/TabletopLiveValues.swift" \
    "$TABLETOP/app/WarcraftRenderDescriptors.swift" \
    "$TABLETOP/app/WarcraftAssetAdapter.swift" \
    "$TABLETOP/app/WarcraftRenderCache.swift" \
    "$TABLETOP/app/WarcraftRenderMath.swift" \
    "$TABLETOP/app/WarcraftRenderReconciliation.swift" \
    "$TABLETOP/app/FixtureWarcraftRenderProvider.swift" \
    "$TABLETOP/app/FixtureSnapshotTransport.swift" \
    "$TABLETOP/app/TabletopReducer.swift" \
    "$TABLETOP/app/TabletopPlacement.swift" \
    "$TABLETOP/app/TabletopReconciliation.swift" \
    "$TABLETOP/app/TabletopGesture.swift" \
    "$TABLETOP/app/TabletopControls.swift" \
    "$TABLETOP/tests/TabletopPureTests.swift" \
    -o "$OUT"
"$OUT"
printf '%s\n' \
    'BZTabletopAssets: class_id 0x34656472 metadata unavailable (6); cached error' \
    'Asset class 0x34656472 metadata status 6; using explicit placeholder' \
    'Missing production descriptor for entity 1; using placeholder' |
    grep -Eq "$BZ_TABLETOP_METADATA_FAILURE_RE"
if printf '%s\n' 'metadata unavailable (0)' | grep -Eq "$BZ_TABLETOP_METADATA_FAILURE_RE"; then
    echo "test-host.sh: metadata failure regex matched success status" >&2
    exit 1
fi
bz_tabletop_exact_item_classes '34656472,66696c72,66746172,676e6b63,74767270,7a697772'
if bz_tabletop_exact_item_classes '34656472,66696c72,66746172,676e6b63,74767270,7a697772,12345678'; then
    echo "test-host.sh: late item class check accepted an unexpected class" >&2
    exit 1
fi

test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$PLIST")" = OpenRealmTabletop
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PLIST")" = org.openrealm.visionos.tabletop
