#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../../.." && pwd)
APP="$ROOT/build/visionos/tabletop/xrsimulator/OpenRealmTabletop.app"
BUNDLE_ID=org.openrealm.visionos.tabletop
WC3_DATA_TOOL="$ROOT/platform/apple/visionos/scripts/wc3_data.sh"
LOG_DIR="$ROOT/build/visionos/tabletop/acceptance"
INSTALL_APP="$LOG_DIR/OpenRealmTabletopAcceptance.app"
STDOUT="$LOG_DIR/stdout.log"
STDERR="$LOG_DIR/stderr.log"
BOOT_OUT="$LOG_DIR/boot.out"
BOOT_ERR="$LOG_DIR/boot.err"
UDID=
LAUNCH_PID=

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

cleanup() {
    cleanup_status=$?
    trap - 0 1 2 15
    if [ -n "$UDID" ]; then
        xcrun simctl terminate "$UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
        if [ -n "$LAUNCH_PID" ]; then wait "$LAUNCH_PID" 2>/dev/null || true; fi
        xcrun simctl shutdown "$UDID" >/dev/null 2>&1 || true
        xcrun simctl delete "$UDID" >/dev/null 2>&1 || true
    fi
    rm -rf "$INSTALL_APP"
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

# HACK: CoreSimulator cannot import the 717 MB sealed production bundle within
# a useful bounded gate. Install identical signed code, then use the production
# helper to stage the same verified MPQs into this clone's app container only.
rm -rf "$INSTALL_APP"
mkdir -p "$INSTALL_APP"
cp "$APP/Info.plist" "$APP/OpenRealmTabletop" "$INSTALL_APP/"
codesign --force --sign - --identifier "$BUNDLE_ID" --timestamp=none "$INSTALL_APP"

SOURCE=$(xcrun simctl list devices available |
    sed -nE 's/.*Apple Vision Pro \(([0-9A-F-]+)\) \(Shutdown\).*/\1/p' | head -1)
if [ -z "$SOURCE" ]; then
    echo "launch-tabletop-simulator.sh: no shutdown Apple Vision Pro simulator is available to clone" >&2
    exit 1
fi
UDID=$(xcrun simctl clone "$SOURCE" "Open Realm Tabletop Acceptance $$")
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
SIMCTL_CHILD_BZ_TABLETOP_MAP=Human02 \
xcrun simctl launch --console --terminate-running-process "$UDID" "$BUNDLE_ID" >"$STDOUT" 2>"$STDERR" &
LAUNCH_PID=$!
RESIDENT=0
while [ "$RESIDENT" -lt 5 ]; do
    sleep 1
    if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
        wait "$LAUNCH_PID" || true
        echo "launch-tabletop-simulator.sh: app exited before five-second residency" >&2
        cat "$STDERR" >&2
        exit 1
    fi
    RESIDENT=$((RESIDENT + 1))
done

cat "$STDOUT" "$STDERR" > "$LOG_DIR/combined.log"
if ! grep -Fq "BZTabletopTransport: initialized" "$LOG_DIR/combined.log"; then
    echo "launch-tabletop-simulator.sh: transport initialization evidence is missing" >&2
    cat "$STDERR" >&2
    exit 1
fi
if ! grep -Fq "OpenRealmTabletop: first snapshot generation" "$LOG_DIR/combined.log"; then
    echo "launch-tabletop-simulator.sh: first-snapshot evidence is missing" >&2
    cat "$STDERR" >&2
    exit 1
fi
if ! grep -Fq 'CL_SendBegin: sending begin world="Maps\Campaign\Human02.w3m"' "$LOG_DIR/combined.log"; then
    echo "launch-tabletop-simulator.sh: Human02 startup evidence is missing" >&2
    cat "$STDERR" >&2
    exit 1
fi

xcrun simctl terminate "$UDID" "$BUNDLE_ID"
wait "$LAUNCH_PID" 2>/dev/null || true
LAUNCH_PID=
printf 'simulator acceptance passed: five-second residency on disposable device %s\n' "$UDID"
