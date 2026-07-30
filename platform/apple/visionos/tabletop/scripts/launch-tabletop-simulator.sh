#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/tabletop-acceptance-patterns.sh"
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../../.." && pwd)
APP="$ROOT/build/visionos/tabletop/xrsimulator/OpenRealmTabletop.app"
BUNDLE_ID=org.openrealm.visionos.tabletop
WC3_DATA_TOOL="$ROOT/platform/apple/visionos/scripts/wc3_data.sh"
LOG_DIR="${OPENREALM_TABLETOP_LOG_DIR:-$ROOT/build/visionos/tabletop/acceptance}"
INSTALL_APP="$LOG_DIR/OpenRealmTabletopAcceptance.app"
STDOUT="$LOG_DIR/stdout.log"
STDERR="$LOG_DIR/stderr.log"
BOOT_OUT="$LOG_DIR/boot.out"
BOOT_ERR="$LOG_DIR/boot.err"
UDID=
LAUNCH_PID=
CREATE_INTERRUPTED=0
PROTECTED_UDID=0FDBB103-A3CE-461F-AF41-30CDBEC010FE
MAP="${OPENREALM_TABLETOP_MAP:-Human02}"
STRICT_HUMAN02=0
test "$MAP" != Human02 || STRICT_HUMAN02=1
if [ "$STRICT_HUMAN02" = 1 ]; then
    EXPECTED_ITEM_CLASSES="${OPENREALM_TABLETOP_EXPECTED_ITEM_CLASSES:-$BZ_TABLETOP_EXPECTED_ITEM_CLASSES}"
elif [ "${OPENREALM_TABLETOP_EXPECTED_ITEM_CLASSES+x}" != x ]; then
    echo "launch-tabletop-simulator.sh: non-Human02 maps require exact expected item classes" >&2
    exit 2
else
    EXPECTED_ITEM_CLASSES="$OPENREALM_TABLETOP_EXPECTED_ITEM_CLASSES"
fi
REQUIRED_CATEGORIES="${OPENREALM_TABLETOP_REQUIRED_CATEGORIES:-unit}"
test "$STRICT_HUMAN02" = 0 || REQUIRED_CATEGORIES="unit building resource doodad destructable"

wait_bounded() {
    wait_pid=$1
    wait_seconds=$2
    wait_elapsed=0
    while kill -0 "$wait_pid" 2>/dev/null; do
        wait_state=$(ps -p "$wait_pid" -o stat= 2>/dev/null || true)
        case "$wait_state" in ""|*Z*) break ;; esac
        if [ "$wait_elapsed" -ge "$wait_seconds" ]; then
            kill "$wait_pid" 2>/dev/null || true
            wait "$wait_pid" 2>/dev/null || true
            return 124
        fi
        sleep 1
        wait_elapsed=$((wait_elapsed + 1))
    done
    wait "$wait_pid"
}

hold_resident() {
    hold_seconds=$1
    hold_elapsed=0
    while [ "$hold_elapsed" -lt "$hold_seconds" ]; do
        if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
            wait "$LAUNCH_PID" || true
            echo "launch-tabletop-simulator.sh: app exited during bounded residency hold" >&2
            cat "$STDERR" >&2
            exit 1
        fi
        sleep 1
        hold_elapsed=$((hold_elapsed + 1))
    done
}

cleanup() {
    cleanup_status=$?
    cleanup_failed=0
    trap - 0 1 2 15
    if [ -n "$UDID" ]; then
        xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
        # Cleanup must stay bounded even if CoreSimulator refuses to terminate the app.
        if [ -n "$LAUNCH_PID" ]; then wait_bounded "$LAUNCH_PID" 5 2>/dev/null || true; fi
        xcrun simctl shutdown "$UDID" >/dev/null 2>&1 || true
        if xcrun simctl delete "$UDID" >>"$LOG_DIR/cleanup.out" 2>>"$LOG_DIR/cleanup.err"; then
            printf 'deleted disposable simulator %s\n' "$UDID"
        else
            printf 'launch-tabletop-simulator.sh: failed to delete disposable simulator %s\n' "$UDID" >&2
            cleanup_failed=1
        fi
    fi
    rm -rf "$INSTALL_APP"
    if [ "$cleanup_status" -eq 0 ] && [ "$cleanup_failed" -ne 0 ]; then cleanup_status=1; fi
    exit "$cleanup_status"
}

trap cleanup 0 1 2 15
if [ ! -d "$APP" ]; then
    printf 'launch-tabletop-simulator.sh: missing app bundle: %s\n' "$APP" >&2
    exit 1
fi
mkdir -p "$LOG_DIR"
: > "$STDOUT"
: > "$STDERR"
: > "$LOG_DIR/cleanup.out"
: > "$LOG_DIR/cleanup.err"

# HACK: CoreSimulator cannot import the 717 MB sealed production bundle within
# a useful bounded gate. Install identical signed code, then use the production
# helper to stage the same verified MPQs into this clone's app container only.
rm -rf "$INSTALL_APP"
mkdir -p "$INSTALL_APP"
cp "$APP/Info.plist" "$APP/OpenRealmTabletop" "$INSTALL_APP/"
codesign --force --sign - --identifier "$BUNDLE_ID" --timestamp=none "$INSTALL_APP"

RUNTIME=$(xcrun simctl list runtimes available |
    sed -nE 's/.* - (com\.apple\.CoreSimulator\.SimRuntime\.xrOS[^ ]*)$/\1/p' | tail -1)
[ -n "$RUNTIME" ] || {
    echo "launch-tabletop-simulator.sh: no available xrOS runtime can create an isolated simulator" >&2
    exit 1
}
# Defer cancellation until the new ID is proven disposable, so cleanup can never target the protected demo.
trap 'CREATE_INTERRUPTED=1' 1 2 15
CANDIDATE_UDID=$(xcrun simctl create "Open Realm Tabletop Acceptance $$" \
    com.apple.CoreSimulator.SimDeviceType.Apple-Vision-Pro-4K \
    "$RUNTIME")
if [ "$CANDIDATE_UDID" = "$PROTECTED_UDID" ]; then
    CANDIDATE_UDID=
    trap cleanup 1 2 15
    echo "launch-tabletop-simulator.sh: CoreSimulator returned the protected demo device" >&2
    exit 1
fi
UDID=$CANDIDATE_UDID
CANDIDATE_UDID=
trap cleanup 1 2 15
if [ "$CREATE_INTERRUPTED" -ne 0 ]; then exit 130; fi
xcrun simctl boot "$UDID"
xcrun simctl bootstatus "$UDID" -b >"$BOOT_OUT" 2>"$BOOT_ERR" &
BOOT_PID=$!
if ! wait_bounded "$BOOT_PID" 120; then
    echo "launch-tabletop-simulator.sh: isolated simulator did not boot within 120 seconds" >&2
    exit 1
fi
xcrun simctl install "$UDID" "$INSTALL_APP" >"$LOG_DIR/install.out" 2>"$LOG_DIR/install.err" &
INSTALL_PID=$!
if ! wait_bounded "$INSTALL_PID" 60; then
    echo "launch-tabletop-simulator.sh: app install did not complete within 60 seconds" >&2
    exit 1
fi
APP_CONTAINER=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" app)
"$WC3_DATA_TOOL" stage "$APP_CONTAINER"

SIMCTL_CHILD_BZ_TABLETOP_MODE=live \
SIMCTL_CHILD_BZ_TABLETOP_MAP="$MAP" \
SIMCTL_CHILD_BZ_TABLETOP_TFT="${OPENREALM_TABLETOP_TFT:-0}" \
SIMCTL_CHILD_BZ_TABLETOP_ACCEPTANCE=1 \
SIMCTL_CHILD_BZ_TABLETOP_AUTOSTART=1 \
xcrun simctl launch --console --terminate-running-process "$UDID" "$BUNDLE_ID" >"$STDOUT" 2>"$STDERR" &
LAUNCH_PID=$!
hold_resident 5

ASSET_WAIT=0
ASSET_WAIT_LIMIT="${OPENREALM_TABLETOP_ASSET_WAIT_LIMIT:-180}"
while [ "$ASSET_WAIT" -lt "$ASSET_WAIT_LIMIT" ]; do
    cat "$STDOUT" "$STDERR" > "$LOG_DIR/combined.log"
    if grep -Fq "OpenRealmTabletopAssets: abi=2 cache_phase=stable" "$LOG_DIR/combined.log"; then break; fi
    if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
        wait "$LAUNCH_PID" || true
        echo "launch-tabletop-simulator.sh: app exited before stable asset publication" >&2
        cat "$STDERR" >&2
        exit 1
    fi
    sleep 1
    ASSET_WAIT=$((ASSET_WAIT + 1))
done
if [ "$ASSET_WAIT" -ge "$ASSET_WAIT_LIMIT" ]; then
    echo "launch-tabletop-simulator.sh: stable asset publication timed out" >&2
    cat "$STDERR" >&2
    exit 1
fi
hold_resident "${OPENREALM_TABLETOP_POST_STABLE_WAIT:-240}"
cat "$STDOUT" "$STDERR" > "$LOG_DIR/combined.log"
if ! grep -Fq "BZTabletopTransport: initialized" "$LOG_DIR/combined.log"; then
    echo "launch-tabletop-simulator.sh: transport initialization evidence is missing" >&2
    cat "$STDERR" >&2
    exit 1
fi
if ! grep -Fq "BZTabletopAssets: initialized, abi_version=2" "$LOG_DIR/combined.log"; then
    echo "launch-tabletop-simulator.sh: asset ABI initialization evidence is missing" >&2
    cat "$STDERR" >&2
    exit 1
fi
if ! grep -Fq "OpenRealmTabletop: first snapshot generation" "$LOG_DIR/combined.log"; then
    echo "launch-tabletop-simulator.sh: first-snapshot evidence is missing" >&2
    cat "$STDERR" >&2
    exit 1
fi
grep -F 'CL_SendBegin: sending begin world="' "$LOG_DIR/combined.log" > "$LOG_DIR/begin.log" || true
if ! grep -Fiq "$MAP" "$LOG_DIR/begin.log"; then
    echo "launch-tabletop-simulator.sh: $MAP startup evidence is missing" >&2
    cat "$STDERR" >&2
    exit 1
fi
if grep -Eq 'water shader preparation failed|authoritative water material is unavailable|'\
'parameterNameNotFound|incorrectTypeForParameterName|_BindNodeGraph|Terrain chunk .* failed' \
    "$LOG_DIR/combined.log"; then
    echo "launch-tabletop-simulator.sh: authoritative water shader failed" >&2
    cat "$STDERR" >&2
    exit 1
fi
if grep -Eq "$BZ_TABLETOP_METADATA_FAILURE_RE" "$LOG_DIR/combined.log"; then
    echo "launch-tabletop-simulator.sh: late production metadata placeholder detected" >&2
    grep -E 'metadata unavailable|metadata status|Missing production descriptor' "$LOG_DIR/combined.log" >&2
    exit 1
fi
INITIAL_SUMMARY=$(grep -F "OpenRealmTabletopAssets: abi=2 cache_phase=initial" \
    "$LOG_DIR/combined.log" | tail -1 || true)
STABLE_SUMMARY=$(grep -F "OpenRealmTabletopAssets: abi=2 cache_phase=stable" \
    "$LOG_DIR/combined.log" | tail -1 || true)
ITEM_SUMMARY=$(grep -F "OpenRealmTabletopAssets: abi=2 cache_phase=item" \
    "$LOG_DIR/combined.log" | tail -1 || true)
if [ -z "$INITIAL_SUMMARY" ] || [ -z "$STABLE_SUMMARY" ] ||
        ! bz_tabletop_item_summary_valid "$EXPECTED_ITEM_CLASSES" "$ITEM_SUMMARY"; then
    # Empty-item maps do not publish an item phase; requiring one made valid simple-map acceptance impossible.
    echo "launch-tabletop-simulator.sh: copied asset/terrain summary is missing" >&2
    cat "$STDERR" >&2
    exit 1
fi
printf '%s\n' "$STABLE_SUMMARY" | awk -v strict="$STRICT_HUMAN02" '
{
    for (i = 1; i <= NF; i++) {
        split($i, pair, "=")
        value[pair[1]] = pair[2]
    }
    split(value["terrain"], terrain, "x")
    if (value["placeholders"] != 0 || value["placeholder_logs"] != 0 ||
        value["metadata_logs"] != 0 || value["models"] < 1 ||
        value["geosets"] < 1 || value["textured_materials"] < 1 ||
        value["hits"] < 1 || value["misses"] < 1 || value["fog"] != 1) exit 1
    if (terrain[1] == 128 && terrain[2] == 128 && value["chunks"] > 16) exit 1
    if (strict && (terrain[1] != 128 || terrain[2] != 128 || value["chunks"] != 16 ||
        value["terrain_textures"] != 9 || value["no_cliff"] != 2349 ||
        value["fog"] != 1 || value["entities"] != 1024 || value["active_visible"] != 2397 ||
        value["overflow"] != 1373)) exit 1
}' || {
    echo "launch-tabletop-simulator.sh: asset/chunk/fog/cache thresholds failed: $STABLE_SUMMARY" >&2
    exit 1
}
CATEGORIES=$(printf '%s\n' "$STABLE_SUMMARY" | sed -nE 's/.* categories=([^ ]+).*/\1/p')
for CATEGORY in $REQUIRED_CATEGORIES; do
    case ",$CATEGORIES," in
        *,"$CATEGORY",*) ;;
        *) echo "launch-tabletop-simulator.sh: category $CATEGORY is missing" >&2; exit 1 ;;
    esac
done
ITEM_CLASSES=
if [ -n "$ITEM_SUMMARY" ]; then
    ITEM_CLASSES=$(printf '%s\n' "$ITEM_SUMMARY" | sed -nE 's/.* item_classes=([^ ]+).*/\1/p')
fi
if [ "$ITEM_CLASSES" != "$EXPECTED_ITEM_CLASSES" ]; then
    echo "launch-tabletop-simulator.sh: late item class set is incorrect: $ITEM_SUMMARY" >&2
    exit 1
fi
if [ -n "$ITEM_SUMMARY" ]; then
    printf '%s\n' "$ITEM_SUMMARY" | awk '
{
    for (i = 1; i <= NF; i++) {
        split($i, pair, "=")
        value[pair[1]] = pair[2]
    }
    if (value["placeholders"] != 0 || value["placeholder_logs"] != 0 ||
        value["metadata_logs"] != 0) exit 1
}' || {
        echo "launch-tabletop-simulator.sh: late item publication counters failed: $ITEM_SUMMARY" >&2
        exit 1
    }
fi
INITIAL_MISSES=$(printf '%s\n' "$INITIAL_SUMMARY" | sed -nE 's/.* misses=([0-9]+).*/\1/p')
STABLE_MISSES=$(printf '%s\n' "$STABLE_SUMMARY" | sed -nE 's/.* misses=([0-9]+).*/\1/p')
INITIAL_HITS=$(printf '%s\n' "$INITIAL_SUMMARY" | sed -nE 's/.* hits=([0-9]+).*/\1/p')
STABLE_HITS=$(printf '%s\n' "$STABLE_SUMMARY" | sed -nE 's/.* hits=([0-9]+).*/\1/p')
if [ "$INITIAL_MISSES" != "$STABLE_MISSES" ] || [ "$STABLE_HITS" -le "$INITIAL_HITS" ]; then
    echo "launch-tabletop-simulator.sh: cache did not transition from decode misses to stable hits" >&2
    exit 1
fi

xcrun simctl terminate "$UDID" "$BUNDLE_ID"
wait "$LAUNCH_PID" 2>/dev/null || true
LAUNCH_PID=
printf 'simulator acceptance passed: five-second residency on disposable device %s\n' "$UDID"
