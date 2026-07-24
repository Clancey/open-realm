#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../../.." && pwd)
TABLETOP="$ROOT/platform/apple/visionos/tabletop"
OUT="$ROOT/build/tests/visionos-tabletop-pure"
PLIST="$TABLETOP/Info.plist"

mkdir -p "$(dirname -- "$OUT")"
xcrun swiftc -parse-as-library \
    "$TABLETOP/app/TabletopSnapshot.swift" \
    "$TABLETOP/app/TabletopCommand.swift" \
    "$TABLETOP/app/TabletopAdapter.swift" \
    "$TABLETOP/app/TabletopLiveValues.swift" \
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
