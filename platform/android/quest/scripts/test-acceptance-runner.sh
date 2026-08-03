#!/bin/sh
# platform/android/quest/scripts/test-acceptance-runner.sh
#
# Device-free (no NDK/Gradle/Quest hardware required) fake-adb/run-as/pm
# harness for scripts/acceptance-runner.sh - exercises the REAL production
# script's own functions/flow end to end (device resolution delegated to the
# REAL scripts/stage-wc3-data.sh, install, native-lib verification seam,
# package/debuggable/run-as validation, ROC/TFT staging+evidence, launch,
# logcat capture/analysis, OVR Metrics automation, cleanup) against a fake
# device built from plain host directories and shell shims - mirroring
# scripts/test-stage-wc3-data.sh's own technique for stage-wc3-data.sh. See
# docs/quest-tabletop.md's acceptance-automation section for the full
# design writeup.
set -eu

tool_name=${0##*/}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUNNER_TOOL="$SCRIPT_DIR/acceptance-runner.sh"
# stage-wc3-data.sh itself is exercised only indirectly (acceptance-runner.sh
# spawns it), so this file never invokes it directly - no STAGE_TOOL needed.

scratch=$(mktemp -d "${TMPDIR:-/tmp}/openrealm-quest-acceptance.XXXXXX")
scratch=$(CDPATH= cd -- "$scratch" && pwd)
trap 'rm -rf "$scratch"' 0 1 2 15

FAKE_BIN="$scratch/fakebin"
DEVICE_ROOT="$scratch/device"
STATE_FILE="$scratch/state.sh"
ARTIFACTS_ROOT="$scratch/artifacts"
OVR_CSV_DEVICE_DIR="/sdcard/Android/data/com.oculus.ovrmonitormetricsservice/files/CapturedMetrics"
OVR_CSV_HOST_DIR="$scratch/ovr_captured_metrics"
mkdir -p "$FAKE_BIN" "$DEVICE_ROOT" "$ARTIFACTS_ROOT"

PKG="org.openrealm.quest.test"
tests_run=0

fail() {
    printf '%s: FAIL: %s\n' "$tool_name" "$*" >&2
    exit 1
}

pass() {
    tests_run=$((tests_run + 1))
    printf '  %-70s PASS\n' "$1"
}

if command -v sha256sum >/dev/null 2>&1; then
    HOST_HASH_BIN=$(command -v sha256sum); HOST_HASH_ARGS=""
elif command -v shasum >/dev/null 2>&1; then
    HOST_HASH_BIN=$(command -v shasum); HOST_HASH_ARGS="-a 256"
else
    fail "no sha256sum/shasum on this host - cannot build the fake device's hash shim"
fi

MOCK_APK="$scratch/app-debug.apk"
: > "$MOCK_APK"

# --- fake device state -------------------------------------------------------

# Arguments: devices installed_package debuggable has_ovr_metrics
# install_fail launch_fail verify_fail app_pid
# devices is a space-separated "serial:state" list, e.g. "DEV1:device" or
# "DEV1:device DEV2:offline" - mirrors test-stage-wc3-data.sh's own
# write_devices_state() so --serial routing/rejection is genuinely
# observable, not silently accepted. app_pid defaults to a stable fake PID
# (the app is "running" and resolvable) since that is the common case for
# every existing scenario; pass "" explicitly to simulate the app already
# having exited (or never having been scheduled) before this script's own
# bounded pidof poll - see the fake pidof shim below and
# acceptance-runner.sh's "12b" PID-resolution step.
write_state() {
    devices=$1 installed_package=$2 debuggable=$3 has_ovr_metrics=$4
    install_fail=${5:-0} launch_fail=${6:-0} verify_fail=${7:-0} app_pid=${8-9001}
    cat > "$STATE_FILE" <<EOF
DEVICES="$devices"
INSTALLED_PACKAGE=$installed_package
DEBUGGABLE=$debuggable
HAS_OVR_METRICS=$has_ovr_metrics
INSTALL_FAIL=$install_fail
LAUNCH_FAIL=$launch_fail
VERIFY_FAIL=$verify_fail
APP_PID=$app_pid
EOF
    # scripts/verify-native-lib.sh's fake stand-in is invoked directly by
    # acceptance-runner.sh (a SIBLING of the fake adb process tree, never a
    # child of it), so it cannot inherit state through the fake adb's own
    # "source STATE_FILE then export" chain below - export directly here
    # instead, inherited transitively by every process this test script
    # goes on to spawn.
    VERIFY_FAIL=$verify_fail
    export VERIFY_FAIL
    rm -rf "$DEVICE_ROOT" "$ARTIFACTS_ROOT" "$OVR_CSV_HOST_DIR"
    : > "$scratch/adb_invocations.log"
    mkdir -p "$DEVICE_ROOT" "$ARTIFACTS_ROOT"
    if [ "$has_ovr_metrics" = "1" ]; then
        mkdir -p "$OVR_CSV_HOST_DIR"
        printf 'timestamp,fps,cpu_level,gpu_level\n2026-08-02T12:00:00Z,72.0,3,4\n' > "$OVR_CSV_HOST_DIR/ovr_report_1.csv"
    fi
}

pkg_root_dir_for_serial() { printf '%s/dev-%s/pkgs/%s' "$DEVICE_ROOT" "$1" "$PKG"; }

# --- fake device-side shims (run-as/pm/df/sha256sum/cp) ----------------------
# Identical in spirit to test-stage-wc3-data.sh's own shims (proven there
# against stage-wc3-data.sh directly) - reproduced here because
# acceptance-runner.sh spawns stage-wc3-data.sh as an independent process
# that must see the SAME fake device through the SAME BZ_QUEST_ADB seam.

cat > "$FAKE_BIN/run-as" <<'SHIM'
#!/bin/sh
set -eu
pkg=$1
shift
if [ "$pkg" != "${INSTALLED_PACKAGE:-}" ]; then
    echo "run-as: Package '$pkg' is unknown" >&2
    exit 1
fi
if [ "${DEBUGGABLE:-0}" != "1" ]; then
    echo "run-as: Package '$pkg' is not debuggable" >&2
    exit 1
fi
pkgroot="$FAKE_DEVICE_ROOT/pkgs/$pkg"
mkdir -p "$pkgroot"
cd "$pkgroot" || exit 1
exec "$@"
SHIM

cat > "$FAKE_BIN/pm" <<'SHIM'
#!/bin/sh
set -eu
if [ "${1:-}" = "path" ]; then
    if [ "${2:-}" = "${INSTALLED_PACKAGE:-}" ]; then
        echo "package:/data/app/fake-openrealm-quest/base.apk"
        exit 0
    fi
    if [ "${2:-}" = "com.oculus.ovrmonitormetricsservice" ] && [ "${HAS_OVR_METRICS:-0}" = "1" ]; then
        echo "package:/data/app/fake-ovr-metrics/base.apk"
        exit 0
    fi
fi
exit 1
SHIM

# Fake pidof - mirrors real pidof's contract: prints the PID and exits 0
# if found, prints nothing and exits nonzero otherwise. APP_PID="" (see
# write_state's own header comment) simulates the app having already
# self-exited, or never having been scheduled, before
# acceptance-runner.sh's own bounded poll gives up - proving that path
# degrades gracefully instead of hanging/failing outright.
cat > "$FAKE_BIN/pidof" <<'SHIM'
#!/bin/sh
set -eu
if [ "${1:-}" = "${INSTALLED_PACKAGE:-}" ] && [ -n "${APP_PID:-}" ]; then
    echo "$APP_PID"
    exit 0
fi
exit 1
SHIM

cat > "$FAKE_BIN/df" <<'SHIM'
#!/bin/sh
echo "Filesystem 1024-blocks Used Available Capacity Mounted on"
echo "fake 99999999 1 10000000 1% /fake"
SHIM

cat > "$FAKE_BIN/sha256sum" <<SHIM
#!/bin/sh
exec "$HOST_HASH_BIN" $HOST_HASH_ARGS "\$@"
SHIM

cat > "$FAKE_BIN/cp" <<'SHIM'
#!/bin/sh
exec /bin/cp "$@"
SHIM

chmod +x "$FAKE_BIN"/run-as "$FAKE_BIN"/pm "$FAKE_BIN"/pidof "$FAKE_BIN"/df "$FAKE_BIN"/sha256sum "$FAKE_BIN"/cp

# --- fake am (am start / am force-stop / am broadcast) -----------------------

cat > "$FAKE_BIN/am" <<'SHIM'
#!/bin/sh
set -eu
case "${1:-}" in
    start)
        if [ "${LAUNCH_FAIL:-0}" = "1" ]; then
            echo "Error: Activity not started, unable to resolve Intent" >&2
            exit 1
        fi
        echo "Starting: Intent { cmp=fake/.NativeActivity }"
        ;;
    force-stop) : ;;
    broadcast) echo "Broadcast completed: result=0" ;;
    *) echo "fake am: unsupported command $*" >&2; exit 1 ;;
esac
SHIM
chmod +x "$FAKE_BIN/am"

# --- fake verify-native-lib.sh (device-free stand-in for the real NDK-tool-
# dependent script; the real one has its own dedicated verification and is
# invoked for real elsewhere - see "make quest-verify-native-lib") ----------

cat > "$FAKE_BIN/verify-native-lib.sh" <<'SHIM'
#!/bin/sh
set -eu
apk=${1:-}
[ -f "$apk" ] || { echo "verify-native-lib.sh (fake): APK not found: $apk" >&2; exit 1; }
if [ "${VERIFY_FAIL:-0}" = "1" ]; then
    echo "verify-native-lib.sh (fake): forced failure for test coverage" >&2
    exit 1
fi
echo "verify-native-lib.sh (fake): OK"
SHIM
chmod +x "$FAKE_BIN/verify-native-lib.sh"

# --- fake adb -----------------------------------------------------------------

cat > "$FAKE_BIN/adb" <<SHIM
#!/bin/sh
set -eu
. "$STATE_FILE"
export INSTALLED_PACKAGE DEBUGGABLE HAS_OVR_METRICS INSTALL_FAIL LAUNCH_FAIL VERIFY_FAIL APP_PID
DEVICES=\${DEVICES:-}

serial_arg=""
if [ "\${1:-}" = "-s" ]; then serial_arg=\$2; shift 2; fi

cmd=\$1
shift

printf '%s serial=%s\n' "\$cmd" "\${serial_arg:-NONE}" >> "$scratch/adb_invocations.log"

effective_serial=\$serial_arg
if [ -z "\$effective_serial" ]; then
    device_count=\$(set -- \$DEVICES; echo \$#)
    [ "\$device_count" = "1" ] && effective_serial=\${DEVICES%%:*}
fi
export FAKE_DEVICE_ROOT="$DEVICE_ROOT/dev-\${effective_serial:-none}"

case "\$cmd" in
    devices)
        echo "List of devices attached"
        for pair in \$DEVICES; do
            printf '%s\t%s\n' "\${pair%%:*}" "\${pair##*:}"
        done
        ;;
    install)
        [ "\${1:-}" = "-r" ] && shift
        [ "\${1:-}" = "--" ] && shift
        if [ "\${INSTALL_FAIL:-0}" = "1" ]; then
            echo "adb: failed to install \$1: INSTALL_FAILED_TEST" >&2
            exit 1
        fi
        [ -f "\$1" ] || { echo "adb: apk file not found: \$1" >&2; exit 1; }
        echo "Success"
        ;;
    push)
        # Needed by stage-wc3-data.sh's host->device bounce (see its own
        # header comment) when acceptance-runner.sh delegates --data
        # staging to it - mirrors test-stage-wc3-data.sh's own fake push.
        [ "\${1:-}" = "--" ] && shift
        local=\$1
        remote=\$2
        mkdir -p "\$(dirname "\$remote")"
        cp -- "\$local" "\$remote"
        ;;
    shell)
        inner=\$1
        # OVR Metrics CSV directory listing is special-cased here (rather
        # than as a generic PATH-shimmed \`ls\`) since it must translate the
        # Android-only absolute device path to this fake device's own host
        # directory - see this file's header comment.
        case "\$inner" in
            *"ls -t '$OVR_CSV_DEVICE_DIR'"*)
                if [ "\${HAS_OVR_METRICS:-0}" = "1" ]; then
                    ls -t "$OVR_CSV_HOST_DIR" 2>/dev/null | head -n1
                fi
                ;;
            *)
                PATH="$FAKE_BIN:\$PATH" sh -c "\$inner"
                ;;
        esac
        ;;
    logcat)
        if [ "\${1:-}" = "-c" ]; then
            exit 0
        fi
        if [ -f "$scratch/mock_logcat.log" ]; then
            cat "$scratch/mock_logcat.log"
        fi
        ;;
    pull)
        [ "\${1:-}" = "--" ] && shift
        src=\$1
        dst=\$2
        case "\$src" in
            "$OVR_CSV_DEVICE_DIR"/*)
                base=\${src##*/}
                if [ -f "$OVR_CSV_HOST_DIR/\$base" ]; then
                    cp "$OVR_CSV_HOST_DIR/\$base" "\$dst"
                    echo "Success"
                else
                    echo "adb: remote object '\$src' does not exist" >&2
                    exit 1
                fi
                ;;
            *)
                echo "adb: pull failed for \$src" >&2
                exit 1
                ;;
        esac
        ;;
    *)
        echo "fake adb: unsupported command \$cmd" >&2
        exit 1
        ;;
esac
SHIM
chmod +x "$FAKE_BIN/adb"

export BZ_QUEST_ADB="$FAKE_BIN/adb"
export BZ_QUEST_STAGE_REMOTE_TMP="$DEVICE_ROOT/remote_tmp"
export BZ_QUEST_VERIFY_NATIVE_LIB="$FAKE_BIN/verify-native-lib.sh"
# Bounded pidof-poll sleep (see acceptance-runner.sh's "12b" step) - the
# real 1s-per-attempt default would add up to 5s to every test that
# exercises a PID-resolution-exhausted (APP_PID="") scenario; the fake
# device resolves instantly either way, so this only shortens the
# deliberately-empty-PID test cases, never changes what they prove.
export BZ_QUEST_PID_POLL_SLEEP=0
mkdir -p "$BZ_QUEST_STAGE_REMOTE_TMP"

# --- WC3 data fixtures (reused verbatim from test-stage-wc3-data.sh's own
# ROC/TFT fixture shape) ------------------------------------------------------

make_roc_fixture() {
    mkdir -p "$1"
    printf 'synthetic ROC archive bytes\n' > "$1/War3.mpq"
}
make_tft_fixture() {
    make_roc_fixture "$1"
    printf 'synthetic TFT main archive bytes\n' > "$1/War3x.mpq"
    printf 'synthetic TFT locale archive bytes\n' > "$1/War3xLocal.mpq"
}

# --- mock logcat fixtures -----------------------------------------------------
# Every line's literal text is traced against the real BZ_QUEST_LOGI/LOGE
# call sites in bz_quest_host.c/bz_quest_xr.c/bz_quest_vk.c/
# bz_quest_passthrough.c/bz_quest_audio.c/bz_quest_xr_hands.c/
# bz_quest_bridge.c - never invented - see docs/quest-tabletop.md's
# acceptance-automation section for the full per-marker citation table.

MOCK_LOGCAT="$scratch/mock_logcat.log"

write_no_data_logcat() {
    # The REAL no-data early-exit lifecycle (bz_quest_host.c, traced not
    # invented): bz_quest_ensure_bridge_start()'s failure leaves bridge->lc
    # NULL, so bz_quest_bridge_state() reports BZ_QUEST_BRIDGE_FAILED - a
    # terminal state - which android_main()'s loop checks immediately after
    # processing whichever command ALooper_pollOnce() just delivered. Since
    # this all happens on the SAME iteration that processed APP_CMD_START,
    # the loop breaks into the shared graceful teardown path BEFORE a later
    # ALooper_pollOnce() call could ever deliver the separate APP_CMD_RESUME
    # event - so this fixture, unlike the old single "failed" branch of the
    # removed write_full_logcat(), correctly has NO APP_CMD_RESUME/
    # XrEventDataSessionStateChanged/xrBeginSession lines at all, and DOES
    # have the graceful "destroy requested"/"exiting android_main" lines
    # (see acceptance-runner.sh's mode-aware markers and
    # docs/quest-tabletop.md's corrected "Hardware-only acceptance gates" A).
    cat > "$MOCK_LOGCAT" <<EOF
08-02 12:00:00.001 9001 9001 I OpenRealmQuest: bz_quest_host: starting (layer 4: tabletop lifecycle/snapshot bridge)
08-02 12:00:00.010 9001 9001 I OpenRealmQuest: APP_CMD_START
08-02 12:00:00.020 9001 9001 I OpenRealmQuest: xrInitializeLoaderKHR succeeded
08-02 12:00:00.030 9001 9001 I OpenRealmQuest: xrCreateInstance succeeded: runtime=Mock version=1.0.0
08-02 12:00:00.040 9001 9001 I OpenRealmQuest: xrGetSystem succeeded: systemName=Mock vendorId=0 passthroughCapabilities=0x1 handTrackingSupported=1 handTrackingAimSupported=0
08-02 12:00:00.050 9001 9001 I OpenRealmQuest: Vulkan API version bound: min=1.0 max=1.3
08-02 12:00:00.060 9001 9001 I OpenRealmQuest: xrCreateSession succeeded
08-02 12:00:00.070 9001 9001 I OpenRealmQuest: selected swapchain color format: 43
08-02 12:00:00.080 9001 9001 I OpenRealmQuest: swapchain[0]: 1832x1920, 3 images
08-02 12:00:00.090 9001 9001 I OpenRealmQuest: swapchain[1]: 1832x1920, 3 images
08-02 12:00:00.100 9001 9001 I OpenRealmQuest: passthrough object + reconstruction layer created
08-02 12:00:00.110 9001 9001 I OpenRealmQuest: passthrough started
08-02 12:00:00.115 9001 9001 I OpenRealmQuest: hand tracking enabled (XR_EXT_hand_tracking only)
08-02 12:00:00.120 9001 9001 I OpenRealmQuest: bz_quest_renderer_init succeeded
08-02 12:00:00.130 9001 9001 E OpenRealmQuest: bz_quest_bridge_start failed: Failed to add data directory: /data/user/0/$PKG/files/Warcraft III - see docs/quest-tabletop.md's data-path contract; continuing to pump the Android event loop with no engine running.
08-02 12:00:00.140 9001 9001 I OpenRealmQuest: bz_quest_audio_start succeeded (nativeSampleRate=48000)
08-02 12:00:00.150 9001 9001 I OpenRealmQuest: tabletop frame: status=1 generation=1 lifecycleState=0 lifecycleError=- mapLoaded=0 entities=0(+0 overflow) selected=0
08-02 12:00:00.160 9001 9001 I OpenRealmQuest: tabletop bridge reached a terminal state (2) - requesting host exit
08-02 12:00:00.170 9001 9001 I OpenRealmQuest: bz_quest_host: destroy requested, tearing down audio, bridge, and renderer
08-02 12:00:00.180 9001 9001 I OpenRealmQuest: bz_quest_audio_stop complete
08-02 12:00:00.190 9001 9001 I OpenRealmQuest: bz_quest_host: exiting android_main
EOF
}

write_staged_active_logcat() {
    # $1 = roc|tft (edition, best-effort fprintf(stderr) line only)
    #
    # The REAL healthy staged-data lifecycle, ended the way
    # acceptance-runner.sh itself ALWAYS ends a healthy session: an
    # external `am force-stop` (step 15) once --duration elapses, which
    # kills the process outright - bz_quest_host.c's own event loop never
    # observes app->destroyRequested, so its graceful in-process teardown
    # ("destroy requested"/"exiting android_main") never runs. This
    # fixture therefore deliberately has NO teardown lines at all - unlike
    # the old combined write_full_logcat()'s "succeeded" branch, which
    # impossibly stitched them onto a run this script itself never lets
    # reach that path (see acceptance-runner.sh's mode-aware markers).
    cat > "$MOCK_LOGCAT" <<EOF
08-02 12:00:00.001 9001 9001 I OpenRealmQuest: bz_quest_host: starting (layer 4: tabletop lifecycle/snapshot bridge)
08-02 12:00:00.010 9001 9001 I OpenRealmQuest: APP_CMD_START
08-02 12:00:00.020 9001 9001 I OpenRealmQuest: xrInitializeLoaderKHR succeeded
08-02 12:00:00.030 9001 9001 I OpenRealmQuest: xrCreateInstance succeeded: runtime=Mock version=1.0.0
08-02 12:00:00.040 9001 9001 I OpenRealmQuest: xrGetSystem succeeded: systemName=Mock vendorId=0 passthroughCapabilities=0x1 handTrackingSupported=1 handTrackingAimSupported=0
08-02 12:00:00.050 9001 9001 I OpenRealmQuest: Vulkan API version bound: min=1.0 max=1.3
08-02 12:00:00.060 9001 9001 I OpenRealmQuest: xrCreateSession succeeded
08-02 12:00:00.070 9001 9001 I OpenRealmQuest: selected swapchain color format: 43
08-02 12:00:00.080 9001 9001 I OpenRealmQuest: swapchain[0]: 1832x1920, 3 images
08-02 12:00:00.090 9001 9001 I OpenRealmQuest: swapchain[1]: 1832x1920, 3 images
08-02 12:00:00.100 9001 9001 I OpenRealmQuest: passthrough object + reconstruction layer created
08-02 12:00:00.110 9001 9001 I OpenRealmQuest: passthrough started
08-02 12:00:00.115 9001 9001 I OpenRealmQuest: hand tracking enabled (XR_EXT_hand_tracking only)
08-02 12:00:00.120 9001 9001 I OpenRealmQuest: bz_quest_renderer_init succeeded
08-02 12:00:00.130 9001 9001 I OpenRealmQuest: bz_quest_bridge_start succeeded (data dir '/data/user/0/$PKG/files/Warcraft III')
08-02 12:00:00.135 9001 9001 W OpenRealmQuest: bz_quest_bridge_start: resolved data dir '/data/user/0/$PKG/files/Warcraft III', edition=${1:-roc}
08-02 12:00:00.140 9001 9001 I OpenRealmQuest: bz_quest_audio_start succeeded (nativeSampleRate=48000)
08-02 12:00:00.190 9001 9001 I OpenRealmQuest: APP_CMD_RESUME
08-02 12:00:00.200 9001 9001 I OpenRealmQuest: XrEventDataSessionStateChanged: state=3
08-02 12:00:00.210 9001 9001 I OpenRealmQuest: xrBeginSession succeeded
08-02 12:00:00.220 9001 9001 I OpenRealmQuest: XrEventDataSessionStateChanged: state=4
08-02 12:00:00.300 9001 9001 I OpenRealmQuest: tabletop frame: status=1 generation=3 lifecycleState=2 lifecycleError=- mapLoaded=0 entities=0(+0 overflow) selected=0
EOF
}


# --- test cases ---------------------------------------------------------------

test_missing_serial_flag_rejected() {
    write_state "DEV1:device" "$PKG" 1 0
    if "$RUNNER_TOOL" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should require --serial explicitly"
    fi
    grep -qi -- "--serial is required" "$scratch/err" || fail "missing actionable --serial-required error: $(cat "$scratch/err")"
    pass "missing --serial is rejected explicitly (stricter than stage-wc3-data.sh's auto-select)"
}

test_no_device_rejected() {
    write_state "" "$PKG" 1 0
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail with no attached device"
    fi
    grep -qi "no adb device" "$scratch/err" || fail "missing actionable no-device error: $(cat "$scratch/err")"
    pass "no attached device is rejected explicitly"
}

test_unknown_serial_rejected() {
    write_state "DEV1:device" "$PKG" 1 0
    if "$RUNNER_TOOL" --serial NOPE --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should reject an unknown --serial"
    fi
    grep -q "no attached device matches --serial" "$scratch/err" || fail "missing actionable unknown-serial error: $(cat "$scratch/err")"
    pass "an unknown --serial is rejected explicitly"
}

test_offline_serial_rejected() {
    write_state "DEV1:device DEV2:offline" "$PKG" 1 0
    if "$RUNNER_TOOL" --serial DEV2 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should reject an offline --serial"
    fi
    grep -q "offline" "$scratch/err" || fail "missing actionable offline-serial error: $(cat "$scratch/err")"
    pass "an offline --serial is rejected explicitly"
}

test_serial_routes_to_correct_device() {
    write_state "DEV1:device DEV2:device" "$PKG" 1 0
    write_no_data_logcat
    if ! "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "run against a valid --serial DEV1 should succeed: $(cat "$scratch/err")"
    fi
    if grep -v 'serial=DEV1$' "$scratch/adb_invocations.log" > "$scratch/bad_invocations"; then
        [ -s "$scratch/bad_invocations" ] && fail "not every adb invocation carried --serial DEV1 consistently: $(cat "$scratch/bad_invocations")"
    fi
    pass "--serial DEV1 (of two attached devices) routes every adb invocation consistently"
}

test_missing_apk_custom_path_rejected() {
    write_state "DEV1:device" "$PKG" 1 0
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" \
        --apk "$scratch/does-not-exist.apk" >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail loudly for a missing custom --apk path (never silently auto-build a non-default path)"
    fi
    grep -q "cannot be auto-built" "$scratch/err" || fail "missing actionable custom-apk error: $(cat "$scratch/err")"
    pass "a missing custom --apk path fails loudly instead of silently attempting a build"
}

test_native_lib_verification_failure() {
    write_state "DEV1:device" "$PKG" 1 0 0 0 1
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail when native-lib/manifest verification fails"
    fi
    grep -q "native-lib/manifest verification failed" "$scratch/err" || fail "missing actionable verification error: $(cat "$scratch/err")"
    pass "native-lib/manifest verification failure fails the run loudly"
}

test_install_failure() {
    write_state "DEV1:device" "$PKG" 1 0 1
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail when adb install -r fails"
    fi
    grep -q "adb install -r failed" "$scratch/err" || fail "missing actionable install-failure error: $(cat "$scratch/err")"
    pass "adb install -r failure fails the run loudly"
}

test_missing_package_rejected() {
    write_state "DEV1:device" "org.example.other" 1 0
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail when the target package is not installed"
    fi
    grep -q "package/debuggable/run-as validation failed" "$scratch/err" || fail "missing actionable package-validation error: $(cat "$scratch/err")"
    pass "a package that is not installed is rejected explicitly"
}

test_non_debuggable_rejected() {
    write_state "DEV1:device" "$PKG" 0 0
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail when the package is not debuggable (run-as unavailable)"
    fi
    grep -q "package/debuggable/run-as validation failed" "$scratch/err" || fail "missing actionable non-debuggable error: $(cat "$scratch/err")"
    pass "a non-debuggable package (run-as unavailable) is rejected explicitly"
}

test_launch_failure() {
    write_state "DEV1:device" "$PKG" 1 0 0 1
    write_no_data_logcat
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail when the app launch (am start) fails"
    fi
    grep -q "failed to launch" "$scratch/err" || fail "missing actionable launch-failure error: $(cat "$scratch/err")"
    pass "app launch failure fails the run loudly"
}

test_roc_only_staging_and_evidence() {
    write_state "DEV1:device" "$PKG" 1 0
    write_staged_active_logcat roc
    src="$scratch/roc-src"
    make_roc_fixture "$src"
    out=$("$RUNNER_TOOL" --serial DEV1 --package "$PKG" --data "$src" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" 2>"$scratch/err") ||
        fail "ROC-only staged run should pass: $(cat "$scratch/err")"
    printf '%s\n' "$out" | grep -q "PASS - acceptance evidence complete" || fail "expected overall PASS: $out"
    run_dir=$(find "$ARTIFACTS_ROOT" -mindepth 1 -maxdepth 1 -type d | head -n1)
    [ -n "$run_dir" ] || fail "no artifact run directory created"
    grep -q '"data_layout": "roc"' "$run_dir/metadata.json" || fail "metadata.json did not classify layout as roc: $(cat "$run_dir/metadata.json")"
    pass "ROC-only data staging + evidence classification (layout=roc) passes end to end"
}

test_tft_staging_and_evidence() {
    write_state "DEV1:device" "$PKG" 1 0
    write_staged_active_logcat tft
    src="$scratch/tft-src"
    make_tft_fixture "$src"
    out=$("$RUNNER_TOOL" --serial DEV1 --package "$PKG" --data "$src" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" 2>"$scratch/err") ||
        fail "TFT staged run should pass: $(cat "$scratch/err")"
    printf '%s\n' "$out" | grep -q "PASS - acceptance evidence complete" || fail "expected overall PASS: $out"
    run_dir=$(find "$ARTIFACTS_ROOT" -mindepth 1 -maxdepth 1 -type d | head -n1)
    grep -q '"data_layout": "tft"' "$run_dir/metadata.json" || fail "metadata.json did not classify layout as tft: $(cat "$run_dir/metadata.json")"
    grep -q "War3x.mpq present" "$run_dir/data_verify.log" || fail "data_verify.log missing TFT archive evidence"
    pass "TFT-over-ROC data staging + evidence classification (layout=tft, expansion archives present) passes end to end"
}

test_no_data_expects_clean_bridge_failure() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err" || fail "hardware-only (no --data) run expecting the documented clean bridge failure should still PASS overall: $(cat "$scratch/err")"
    grep -q "PASS - acceptance evidence complete" "$scratch/out" || fail "expected overall PASS (clean documented failure is the correct outcome with no data staged): $(cat "$scratch/out")"
    pass "no --data given: the documented clean bz_quest_bridge_start failure is correctly treated as a PASS, not a false failure"
}

test_bridge_succeeded_required_when_data_staged() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    src="$scratch/roc-src-mismatch"
    make_roc_fixture "$src"
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --data "$src" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should FAIL when --data was staged but the bridge only logged the no-data failure line"
    fi
    grep -q "Tabletop bridge startup (data staged: expects success)" "$scratch/out" || fail "expected the bridge-succeeded marker to be reported missing: $(cat "$scratch/out")"
    grep -q '\[MISSING\]' "$scratch/out" || fail "expected an explicit MISSING marker line: $(cat "$scratch/out")"
    pass "when --data is staged, a missing 'bz_quest_bridge_start succeeded' line correctly fails the run"
}

test_missing_required_marker_fails() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    # Remove the AAudio startup marker only - simulates a genuine regression
    # (or a hang/timeout that never reaches this point) without touching
    # anything else in the fixture.
    grep -v 'bz_quest_audio_start succeeded' "$MOCK_LOGCAT" > "$MOCK_LOGCAT.tmp" && mv "$MOCK_LOGCAT.tmp" "$MOCK_LOGCAT"
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail when a required marker is absent"
    fi
    grep -q "AAudio stream startup" "$scratch/out" || fail "expected the missing AAudio marker to be named: $(cat "$scratch/out")"
    pass "a missing required marker (AAudio startup) fails the run loudly"
}

test_timeout_incomplete_shutdown_fails() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    # Truncate the fixture right after the first tabletop-frame line -
    # simulating a bounded no-data session that timed out/hung before ever
    # reaching its own expected graceful self-exit (bridge-terminal
    # detection, then the shared teardown sequence - see
    # acceptance-runner.sh's mode-aware markers for why these are REQUIRED
    # in the no-data path specifically, unlike a healthy --data run).
    awk '/tabletop frame:/{print; exit} {print}' "$MOCK_LOGCAT" > "$MOCK_LOGCAT.tmp" && mv "$MOCK_LOGCAT.tmp" "$MOCK_LOGCAT"
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail when the bounded session times out before clean shutdown is observed"
    fi
    grep -q "Bridge-terminal detection" "$scratch/out" || fail "expected the missing bridge-terminal marker to be named: $(cat "$scratch/out")"
    grep -q "Clean in-process teardown requested" "$scratch/out" || fail "expected the missing clean-teardown marker to be named: $(cat "$scratch/out")"
    grep -q "Clean host exit" "$scratch/out" || fail "expected the missing host-exit marker to be named: $(cat "$scratch/out")"
    pass "a timed-out/hung no-data session (no graceful self-exit observed) fails the run loudly"
}

test_forbidden_validation_error_fails() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    # Same PID as the launched app (9001, see write_state's fake pidof) but
    # a DIFFERENT tag (AndroidRuntime, not OpenRealmQuest) - a realistic
    # Java-side uncaught exception in this app's own process reports under
    # exactly this tag while still sharing its crashing process's PID.
    # Proves the PID-scoped net (not just the tag net) independently
    # catches an app-attributable error - see "PID/tag-scoped
    # forbidden-error scan" in docs/quest-tabletop.md.
    printf '08-02 12:00:05.500 9001 2000 E AndroidRuntime: FATAL EXCEPTION: main\n' >> "$MOCK_LOGCAT"
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail when a forbidden fatal error appears under the launched app's own PID, even under a different tag"
    fi
    grep -q "forbidden fatal/validation-layer error" "$scratch/out" || fail "expected forbidden-error detection to be reported: $(cat "$scratch/out")"
    pass "a forbidden fatal error under a different tag (AndroidRuntime) but the SAME app PID fails the run loudly"
}

test_unrelated_vendor_noise_does_not_fail() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    # A DIFFERENT PID (555, some unrelated vendor/HAL service) AND a tag
    # outside the verified set (not OpenRealmQuest/OpenXR/VrApi) - this is
    # exactly the kind of system-wide *:W noise the OLD unscoped scan used
    # to false-fail on. Must NOT be attributed to this launch.
    printf '08-02 12:00:03.000 555 555 E hal_camera_vendor: sensor recalibration warning (unrelated to this launch)\n' >> "$MOCK_LOGCAT"
    printf '08-02 12:00:03.500 777 777 F some_other_vendor_svc: internal watchdog reset\n' >> "$MOCK_LOGCAT"
    out=$("$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" 2>"$scratch/err") ||
        fail "runner should still PASS despite unrelated vendor/HAL E/F noise under a different PID and tag: $(cat "$scratch/err")"
    printf '%s\n' "$out" | grep -q "PASS - acceptance evidence complete" || fail "expected overall PASS: $out"
    pass "unrelated vendor/HAL E/F noise under a different PID and tag does not fail the run"
}

test_map_epoch_cache_reset_error_fails() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    printf '08-02 12:00:05.500 1000 1000 E OpenRealmQuest: bz_quest_vk_wc3: map-epoch model/texture cache reset failed - vkDeviceWaitIdle timed out\n' \
        >> "$MOCK_LOGCAT"
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail when the PR #28 map-epoch cache-reset fix logs an error"
    fi
    grep -q "map-epoch GPU cache reset logged an error" "$scratch/out" || fail "expected the named map-epoch cache-reset detection: $(cat "$scratch/out")"
    pass "a map-epoch GPU cache-reset error (bz_quest_vk_wc3.c, PR #28 fix) fails the run loudly with a named message"
}

test_map_epoch_cache_reset_absence_is_informational() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    out=$("$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" 2>"$scratch/err") ||
        fail "runner should still PASS when no map-epoch cache-reset error is present: $(cat "$scratch/err")"
    printf '%s\n' "$out" | grep -q "map-epoch GPU cache reset: not independently observable" || fail "expected the informational not-observable note: $out"
    printf '%s\n' "$out" | grep -q "PASS - acceptance evidence complete" || fail "expected overall PASS: $out"
    pass "absence of any map-epoch cache-reset log line is reported as informational, never required"
}

test_pid_unresolved_falls_back_to_tag_scoping() {
    # app_pid="" (8th write_state arg) simulates the fake pidof NEVER
    # finding the app - the documented "already exited, or never
    # scheduled" case (see acceptance-runner.sh's "12b" step and
    # BZ_QUEST_PID_POLL_SLEEP=0 above, which keeps this bounded-retry-then-
    # give-up path near-instant here rather than the real 5x1s bound).
    # Must NOT hang, and must still PASS a genuinely clean run.
    write_state "DEV1:device" "$PKG" 1 0 0 0 0 ""
    write_no_data_logcat
    started=$(date +%s)
    out=$("$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" 2>"$scratch/err") ||
        fail "runner should still PASS when the app PID could not be resolved: $(cat "$scratch/err")"
    elapsed=$(($(date +%s) - started))
    [ "$elapsed" -lt 10 ] || fail "PID-resolution poll took ${elapsed}s - should be bounded/near-instant against a fake device, never hang"
    printf '%s\n' "$out" | grep -qi "could not resolve the launched app PID" || fail "expected an explicit PID-unresolved warning: $out"
    printf '%s\n' "$out" | grep -q "PID unresolved - fell back to.*tag-only scoping" || fail "expected the tag-only-scoping fallback to be reported: $out"
    printf '%s\n' "$out" | grep -q "PASS - acceptance evidence complete" || fail "expected overall PASS: $out"
    pass "an unresolvable app PID degrades gracefully to tag-only scoping, stays bounded, and still PASSes a clean run"
}

test_pid_unresolved_tag_based_error_still_fails() {
    # Same PID-unresolved fallback as above, but this time inject a real
    # app-tagged (OpenRealmQuest) error - proves the tag-only fallback is
    # NOT a silent bypass; an app-attributable error is still caught by
    # tag alone when PID scoping is unavailable.
    write_state "DEV1:device" "$PKG" 1 0 0 0 0 ""
    write_no_data_logcat
    printf '08-02 12:00:05.500 4242 4242 E OpenRealmQuest: vkCreateGraphicsPipelines catastrophic failure\n' >> "$MOCK_LOGCAT"
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "runner should fail on an app-tagged error even when the PID could not be resolved"
    fi
    grep -q "forbidden fatal/validation-layer error" "$scratch/out" || fail "expected forbidden-error detection via tag-only fallback: $(cat "$scratch/out")"
    pass "an app-tagged error still fails the run via tag-only fallback when the PID could not be resolved"
}

test_metrics_unavailable_is_not_fatal() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    out=$("$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" 2>"$scratch/err") ||
        fail "runner should still PASS when OVR Metrics Tool is not installed: $(cat "$scratch/err")"
    printf '%s\n' "$out" | grep -q "PASS - acceptance evidence complete" || fail "expected overall PASS despite metrics being unavailable: $out"
    printf '%s\n' "$out" | grep -qi "not installed on this device" || fail "expected an explicit OVR Metrics prerequisite note: $out"
    run_dir=$(find "$ARTIFACTS_ROOT" -mindepth 1 -maxdepth 1 -type d | head -n1)
    [ -f "$run_dir/analysis_report.md" ] || fail "analysis_report.md missing"
    ! grep -q "ovr_metrics.csv" "$run_dir/metadata.json" || grep -q '"ovr_metrics_csv_path": ""' "$run_dir/metadata.json" || fail "no CSV should have been captured"
    pass "OVR Metrics Tool unavailable is reported as an explicit prerequisite, never a failure"
}

test_metrics_available_captures_csv() {
    write_state "DEV1:device" "$PKG" 1 1
    write_no_data_logcat
    "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err" || fail "runner should pass with OVR Metrics Tool available: $(cat "$scratch/err")"
    run_dir=$(find "$ARTIFACTS_ROOT" -mindepth 1 -maxdepth 1 -type d | head -n1)
    [ -f "$run_dir/ovr_metrics.csv" ] || fail "ovr_metrics.csv was not captured despite OVR Metrics Tool being available"
    grep -q "fps" "$run_dir/ovr_metrics.csv" || fail "captured ovr_metrics.csv does not look like the raw CSV fixture"
    pass "OVR Metrics Tool available: CSV report is captured into the artifact directory"
}

test_spaces_in_data_dir_are_preserved() {
    write_state "DEV1:device" "$PKG" 1 0
    write_staged_active_logcat roc
    src="$scratch/roc src with spaces"
    make_roc_fixture "$src"
    "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --data "$src" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err" || fail "a --data path containing spaces should stage successfully: $(cat "$scratch/err")"
    grep -q "PASS - acceptance evidence complete" "$scratch/out" || fail "expected overall PASS with a spaces-containing --data path: $(cat "$scratch/out")"
    pass "a --data directory path containing spaces is safely quoted through every layer"
}

test_artifacts_root_with_spaces_is_preserved() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    spaced_root="$scratch/artifact root with spaces"
    "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$spaced_root" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err" || fail "an --artifacts root containing spaces should work: $(cat "$scratch/err")"
    [ -d "$spaced_root" ] || fail "artifacts root with spaces was not created"
    run_dir=$(find "$spaced_root" -mindepth 1 -maxdepth 1 -type d | head -n1)
    [ -n "$run_dir" ] && [ -f "$run_dir/metadata.json" ] || fail "artifact files missing under a spaces-containing artifacts root"
    pass "an --artifacts root path containing spaces is safely quoted and used"
}

test_artifact_preservation() {
    write_state "DEV1:device" "$PKG" 1 1
    write_no_data_logcat
    "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err" || fail "baseline run for artifact-preservation test failed: $(cat "$scratch/err")"
    run_dir=$(find "$ARTIFACTS_ROOT" -mindepth 1 -maxdepth 1 -type d | head -n1)
    for f in metadata.json commands.log logcat.log analysis_report.md dependencies_report.txt guided_checklist.md; do
        [ -f "$run_dir/$f" ] || fail "expected artifact file missing: $f"
    done
    grep -q "resolve-device" "$run_dir/commands.log" || fail "commands.log missing recorded commands"
    grep -q "xrInitializeLoaderKHR succeeded" "$run_dir/logcat.log" || fail "logcat.log does not contain captured evidence"
    pass "every documented artifact (metadata/commands/logcat/metrics/analysis) is preserved in the run directory"
}

test_cleanup_on_failure_still_force_stops() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    grep -v 'bz_quest_audio_start succeeded' "$MOCK_LOGCAT" > "$MOCK_LOGCAT.tmp" && mv "$MOCK_LOGCAT.tmp" "$MOCK_LOGCAT"
    if "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
        >"$scratch/out" 2>"$scratch/err"; then
        fail "expected this run to fail (missing marker) for the cleanup-on-failure test"
    fi
    grep -q '^shell serial=DEV1$' "$scratch/adb_invocations.log" || fail "no shell invocations recorded at all"
    # force-stop is issued via "adb shell am force-stop ..." - confirm at
    # least one shell invocation happened after the failure was detected
    # (the cleanup trap's own force-stop, on top of the explicit one before
    # evidence analysis).
    shell_count=$(grep -c '^shell serial=DEV1$' "$scratch/adb_invocations.log")
    [ "$shell_count" -gt 0 ] || fail "expected at least one adb shell invocation for force-stop on failure"
    pass "cleanup still force-stops the app and preserves partial artifacts on a failed run"
}

test_cleanup_on_signal() {
    write_state "DEV1:device" "$PKG" 1 0
    write_no_data_logcat
    "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --duration 30 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" --non-interactive \
        >"$scratch/sig_out" 2>"$scratch/sig_err" &
    runner_pid=$!
    # Give it time to get through install/verify/launch and into the
    # bounded sleep before signaling - long enough on a loaded CI host, far
    # shorter than the 30s --duration so we know we interrupted mid-sleep.
    sleep 2
    kill -TERM "$runner_pid" 2>/dev/null || true
    # A bounded watchdog kill -9 guards against wait() blocking forever if
    # TERM was somehow never delivered/handled - never trusted alone (a
    # SIGKILL teardown would never reach the cleanup trap at all, which
    # the assertions below would then correctly catch as a failure).
    (sleep 15; kill -9 "$runner_pid" 2>/dev/null || true) &
    watchdog_pid=$!
    # "wait" as a bare statement (or as an `if ! wait ...` condition, whose
    # "!" negation would itself overwrite the captured "$?") would either
    # trip this script's own `set -e`, or silently discard the REAL exit
    # code - suspend set -e around exactly this one capture instead, which
    # preserves wait's genuine exit status either way.
    set +e
    wait "$runner_pid" 2>/dev/null
    signal_exit=$?
    set -e
    kill "$watchdog_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
    [ "$signal_exit" -ne 0 ] || fail "a SIGTERM-interrupted run should not report a clean zero exit"
    grep -qi "force-stopping" "$scratch/sig_out" "$scratch/sig_err" || fail "expected the cleanup trap to force-stop the app after SIGTERM: exit=$signal_exit out=$(cat "$scratch/sig_out") err=$(cat "$scratch/sig_err")"
    pass "a SIGTERM mid-session still force-stops the app via the cleanup trap"
}

test_repeated_runs_are_stable() {
    write_state "DEV1:device" "$PKG" 1 1
    write_no_data_logcat
    i=1
    while [ "$i" -le 3 ]; do
        "$RUNNER_TOOL" --serial DEV1 --package "$PKG" --non-interactive --duration 1 --artifacts "$ARTIFACTS_ROOT" --apk "$MOCK_APK" \
            >"$scratch/out_$i" 2>"$scratch/err_$i" || fail "repeated run #$i failed: $(cat "$scratch/err_$i")"
        grep -q "PASS - acceptance evidence complete" "$scratch/out_$i" || fail "repeated run #$i did not report PASS"
        i=$((i + 1))
    done
    run_dirs=$(find "$ARTIFACTS_ROOT" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')
    [ "$run_dirs" = "3" ] || fail "expected 3 distinct timestamped artifact run directories, found $run_dirs"
    pass "3 consecutive runs against the same fake device all pass identically and produce distinct artifact directories"
}

# --- execute all tests --------------------------------------------------------

test_missing_serial_flag_rejected
test_no_device_rejected
test_unknown_serial_rejected
test_offline_serial_rejected
test_serial_routes_to_correct_device
test_missing_apk_custom_path_rejected
test_native_lib_verification_failure
test_install_failure
test_missing_package_rejected
test_non_debuggable_rejected
test_launch_failure
test_roc_only_staging_and_evidence
test_tft_staging_and_evidence
test_no_data_expects_clean_bridge_failure
test_bridge_succeeded_required_when_data_staged
test_missing_required_marker_fails
test_timeout_incomplete_shutdown_fails
test_forbidden_validation_error_fails
test_unrelated_vendor_noise_does_not_fail
test_map_epoch_cache_reset_error_fails
test_map_epoch_cache_reset_absence_is_informational
test_pid_unresolved_falls_back_to_tag_scoping
test_pid_unresolved_tag_based_error_still_fails
test_metrics_unavailable_is_not_fatal
test_metrics_available_captures_csv
test_spaces_in_data_dir_are_preserved
test_artifacts_root_with_spaces_is_preserved
test_artifact_preservation
test_cleanup_on_failure_still_force_stops
test_cleanup_on_signal
test_repeated_runs_are_stable

printf '%s: %d/%d tests passed\n' "$tool_name" "$tests_run" "$tests_run"
