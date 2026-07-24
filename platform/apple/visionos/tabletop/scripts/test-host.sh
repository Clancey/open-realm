#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
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
    '_ = BZ_TTA_RegisterTeamTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, BZ_TTA_TEAM_TEXTURE_GLOW, 0)' > "$ABI_CHECK"
xcrun swiftc -typecheck -I "$TABLETOP/bridge" -Xcc -I"$ROOT" "$ABI_CHECK"
xcrun swiftc -parse-as-library \
    "$TABLETOP/app/TabletopSnapshot.swift" \
    "$TABLETOP/app/TabletopCommand.swift" \
    "$TABLETOP/app/TabletopAdapter.swift" \
    "$TABLETOP/app/TabletopLiveValues.swift" \
    "$TABLETOP/app/WarcraftRenderDescriptors.swift" \
    "$TABLETOP/app/WarcraftRenderCache.swift" \
    "$TABLETOP/app/WarcraftRenderMath.swift" \
    "$TABLETOP/app/WarcraftRenderReconciliation.swift" \
    "$TABLETOP/app/FixtureWarcraftRenderProvider.swift" \
    "$TABLETOP/app/FixtureSnapshotTransport.swift" \
    "$TABLETOP/app/TabletopReducer.swift" \
    "$TABLETOP/app/TabletopPlacement.swift" \
    "$TABLETOP/app/TabletopReconciliation.swift" \
    "$TABLETOP/app/TabletopGesture.swift" \
    "$TABLETOP/tests/TabletopPureTests.swift" \
    -o "$OUT"
"$OUT"

test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$PLIST")" = OpenRealmTabletop
test "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$PLIST")" = org.openrealm.visionos.tabletop
