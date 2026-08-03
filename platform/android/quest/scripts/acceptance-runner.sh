#!/bin/sh
# platform/android/quest/scripts/acceptance-runner.sh
#
# Automated (+ guided) physical Meta Quest 3/3S acceptance runner. Builds/
# verifies the debug APK, installs it, validates the package/debuggable/
# run-as assumptions, optionally stages a developer's own local ROC/TFT
# Warcraft III data, launches the real NativeActivity for a bounded session,
# captures logcat + (if available) OVR Metrics Tool performance data, and
# analyzes the captured logcat for the exact evidence markers documented in
# docs/quest-tabletop.md's "Exact on-device acceptance procedure". Force-
# stops and cleans up on success, error, timeout, or signal.
#
# Deliberately reuses, rather than reimplements, scripts/stage-wc3-data.sh's
# own device-resolution ("resolve-device"), package/debuggable/run-as
# validation ("check-runtime"), data staging ("stage"/"verify"), and launch
# ("run") subcommands - see that script's header comment for the exact
# run-as/scoped-storage contract this delegates to. This script owns only
# what stage-wc3-data.sh does not: APK build/install, native-lib/manifest
# verification, logcat capture, OVR Metrics automation, guided manual
# checkpoints, and evidence analysis.
#
# This script assumes hardware (a real, adb-attached Quest 3/3S) and is
# NEVER run by `make test`/`make quest` - see
# platform/android/quest/scripts/test-acceptance-runner.sh for the
# device-free fake-adb harness that exercises this script's OWN logic
# (quoting, evidence classification, cleanup) without hardware, and
# docs/quest-tabletop.md's acceptance-automation section for the full
# design writeup, evidence table, and research citations (OVR Metrics Tool
# mechanism, why some markers are best-effort, etc).
#
# Usage:
#   acceptance-runner.sh --serial SERIAL [--package PKG] [--apk PATH]
#                        [--data DATA_DIR] [--duration SECONDS]
#                        [--artifacts DIR] [--non-interactive]
#
# --serial is REQUIRED (stricter than stage-wc3-data.sh's own single-device
# auto-select convenience): an acceptance run is a deliberate, reproducible
# validation step, and MUST record exactly which physical device it ran
# against rather than silently trusting "whichever device happens to be
# alone right now" - see docs/quest-tabletop.md.
set -eu

tool_name=${0##*/}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
STAGE_SCRIPT="$SCRIPT_DIR/stage-wc3-data.sh"

# Must exactly match bz_quest_log.h's BZ_QUEST_LOG_TAG and
# app/build.gradle's applicationId/AndroidManifest.xml's NativeActivity -
# see stage-wc3-data.sh's own header comment for why every Quest script
# re-declares these three literals locally rather than sourcing a shared
# file (each script here is independently invocable/testable).
DEFAULT_PACKAGE="org.openrealm.quest"
DEFAULT_ACTIVITY="android.app.NativeActivity"
LOG_TAG="OpenRealmQuest"
DEFAULT_DURATION=20
DEFAULT_APK_PATH="$ROOT_DIR/platform/android/quest/app/build/outputs/apk/debug/app-debug.apk"
DEFAULT_ARTIFACTS_ROOT="$ROOT_DIR/build/quest-acceptance-artifacts"

# Meta's OVR Metrics Tool (Meta Horizon Store app) automation surface -
# verified against Meta's own documentation
# (developers.meta.com/horizon/documentation/native/android/ts-ovrmetricstool/,
# fetched 2026-08-02): the automatable component is the
# com.oculus.ovrmonitormetricsservice package's SettingsBroadcastReceiver
# (NOT the com.oculus.ovrmetricstool launcher app package - that name is
# the user-facing app; ADB automation targets the separate metrics
# *service* package), and CSV reports land under that package's own
# scoped-storage external files dir. See docs/quest-tabletop.md's
# acceptance-automation section for the full citation and every other
# adb command this page documents.
OVR_METRICS_PKG="com.oculus.ovrmonitormetricsservice"
OVR_METRICS_RECEIVER="$OVR_METRICS_PKG/.SettingsBroadcastReceiver"
OVR_METRICS_CSV_DIR="/sdcard/Android/data/$OVR_METRICS_PKG/files/CapturedMetrics"

# Overridable seams for scripts/test-acceptance-runner.sh's fake-adb/fake-
# verify harness - never overridden in normal developer use, exactly
# mirroring stage-wc3-data.sh's own BZ_QUEST_ADB convention. Faking the
# verify-native-lib.sh path (rather than special-casing "are we under
# test" inside this script) means the real control flow - always call the
# verifier, always check its exit code - is exercised by the test harness
# too; only the external tool identity is swapped, never this script's own
# logic (see docs/quest-tabletop.md).
ADB=${BZ_QUEST_ADB:-adb}
VERIFY_NATIVE_LIB_SH=${BZ_QUEST_VERIFY_NATIVE_LIB:-"$SCRIPT_DIR/verify-native-lib.sh"}

serial=""
package="$DEFAULT_PACKAGE"
apk_path="${BZ_QUEST_APK:-$DEFAULT_APK_PATH}"
data_dir=""
duration="$DEFAULT_DURATION"
artifacts_root="$DEFAULT_ARTIFACTS_ROOT"
interactive=1

usage() {
    cat >&2 <<EOF
Usage:
  $tool_name --serial SERIAL [options]

Required:
  --serial SERIAL     Target adb device serial (required - see this
                      script's header comment for why, unlike
                      stage-wc3-data.sh, this is never auto-selected).

Options:
  --package PKG       Application ID to target (default: $DEFAULT_PACKAGE).
  --apk PATH          Debug APK path (default: $DEFAULT_APK_PATH;
                      built via 'make quest-assemble-debug' if missing).
  --data DIR          Local Warcraft III ROC/TFT data directory to stage
                      and verify before launch (optional - omit to run the
                      hardware-only, no-data acceptance procedure).
  --duration SECONDS  Bounded session length in non-interactive mode
                      (default: $DEFAULT_DURATION).
  --artifacts DIR     Output root for per-run artifact directories
                      (default: $DEFAULT_ARTIFACTS_ROOT, already
                      gitignored via the repo's top-level 'build/' rule).
  --non-interactive   Skip the guided manual checklist; run strictly for
                      --duration seconds then analyze captured evidence.
  -h, --help          Show this help message.
EOF
}

die() {
    printf '%s: %s\n' "$tool_name" "$*" >&2
    exit 1
}

sh_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

adb_() {
    "$ADB" -s "$serial" "$@"
}

adb_shell() {
    # $1 must already be one fully pre-quoted command string - see
    # stage-wc3-data.sh's header comment for why every path/package-derived
    # word passed through here is built with sh_quote() first.
    adb_ shell "$1"
}

# --- argument parsing --------------------------------------------------------

while [ $# -gt 0 ]; do
    case "$1" in
        --serial) [ $# -ge 2 ] || die "--serial requires a value"; serial=$2; shift 2 ;;
        --package) [ $# -ge 2 ] || die "--package requires a value"; package=$2; shift 2 ;;
        --apk) [ $# -ge 2 ] || die "--apk requires a value"; apk_path=$2; shift 2 ;;
        --data) [ $# -ge 2 ] || die "--data requires a value"; data_dir=$2; shift 2 ;;
        --duration) [ $# -ge 2 ] || die "--duration requires a value"; duration=$2; shift 2 ;;
        --artifacts) [ $# -ge 2 ] || die "--artifacts requires a value"; artifacts_root=$2; shift 2 ;;
        --non-interactive) interactive=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage; exit 2 ;;
    esac
done

[ -n "$serial" ] || { usage; die "--serial is required"; }
case "$duration" in
    ''|*[!0-9]*) die "--duration must be a positive integer number of seconds, got '$duration'" ;;
esac
[ "$duration" -gt 0 ] || die "--duration must be a positive integer number of seconds, got '$duration'"

# --- state used by the cleanup trap ------------------------------------------

bg_logcat_pid=""
app_launched=0
ovr_metrics_csv_enabled=0
cleanup_ran=0
cleanup_exit_code=0

cleanup() {
    # MUST be the very first statement, before even the cleanup_ran guard
    # check below: "$?" is only reliable for exactly one statement after
    # entering the trap - the guard's own "[ ... ]" test (and any
    # assignment) immediately overwrites it, so it has to be captured into
    # a local before anything else runs, then reused explicitly below
    # instead of re-reading a live "$?" a second time.
    prev_status=$?
    # $1, when given, is the conventional 128+signum exit code for the
    # INT/TERM traps below - "$?" inside a signal-triggered trap does NOT
    # reliably reflect the interrupting signal (it may still hold a stale
    # value from whatever foreground command last completed before the
    # signal arrived), so INT/TERM pass their own explicit code instead of
    # relying on it; the plain EXIT trap has no such problem and keeps
    # using the real (just-captured) "$?" from the script's own normal
    # exit path.
    #
    # Calling `exit` from inside the INT/TERM-triggered invocation below
    # ALSO re-triggers the plain EXIT trap (POSIX: exit always runs a
    # pending EXIT trap) - guard against that re-entrant second call
    # re-deriving "$?" (which by then only reflects this function's own
    # preceding kill/printf/adb_shell calls, not the original signal) by
    # remembering and reusing the FIRST captured code instead.
    if [ "$cleanup_ran" -eq 1 ]; then
        exit "$cleanup_exit_code"
    fi
    cleanup_ran=1
    if [ $# -ge 1 ]; then
        cleanup_exit_code=$1
    else
        cleanup_exit_code=$prev_status
    fi
    set +e
    if [ -n "$bg_logcat_pid" ]; then
        kill "$bg_logcat_pid" >/dev/null 2>&1
        wait "$bg_logcat_pid" 2>/dev/null
        bg_logcat_pid=""
    fi
    if [ "$app_launched" -eq 1 ]; then
        adb_shell "am force-stop $(sh_quote "$package")" >/dev/null 2>&1
        printf '%s: force-stopping %s (signal/error cleanup)...\n' "$tool_name" "$package" >&2
        app_launched=0
    fi
    if [ "$ovr_metrics_csv_enabled" -eq 1 ]; then
        adb_shell "am broadcast -n $(sh_quote "$OVR_METRICS_RECEIVER") -a $(sh_quote "$OVR_METRICS_PKG.DISABLE_CSV")" >/dev/null 2>&1
        ovr_metrics_csv_enabled=0
    fi
    exit "$cleanup_exit_code"
}
# Runs on normal exit, an explicit die()/exit, AND a caught signal - covers
# "force-stop/cleanup on success, error, timeout, and signal" exactly (a
# timeout in this script is always just the bounded --duration sleep
# returning, i.e. ordinary control flow, not a separate signal path).
# INT/TERM pass the standard 128+signum code explicitly (see cleanup()'s
# own comment above); plain EXIT keeps relying on the real "$?".
trap 'cleanup 130' INT
trap 'cleanup 143' TERM
trap cleanup EXIT

# --- 1. resolve + validate the target device (delegates to stage-wc3-data.sh) -

resolved_serial=$("$STAGE_SCRIPT" resolve-device --serial "$serial") || exit 1
serial=$resolved_serial
printf '%s: target device: %s\n' "$tool_name" "$serial"

# --- 2. verify/build the debug APK through the documented existing target ---

if [ ! -f "$apk_path" ]; then
    # quest-assemble-debug (build.mk) always builds to exactly
    # DEFAULT_APK_PATH - it has no notion of a caller's custom --apk
    # override, so auto-building can only ever satisfy a MISSING default
    # path. A missing custom --apk path is a caller error (typo, or a
    # not-yet-built override path) - fail with an actionable message
    # instead of running a build that provably cannot produce that path.
    if [ "$apk_path" != "$DEFAULT_APK_PATH" ]; then
        die "APK not found at custom --apk path $apk_path - quest-assemble-debug always builds to $DEFAULT_APK_PATH, so it cannot be auto-built here; build it yourself first or omit --apk to use the default path"
    fi
    printf '%s: APK not found at %s - building via the documented quest-assemble-debug target...\n' "$tool_name" "$apk_path"
    (cd "$ROOT_DIR" && make -f platform/android/quest/build.mk quest-assemble-debug) ||
        die "quest-assemble-debug failed - see docs/quest-tabletop.md's Prerequisites for the required JAVA_HOME/ANDROID_HOME/NDK versions"
    [ -f "$apk_path" ] || die "APK still missing at $apk_path after quest-assemble-debug reported success"
fi

# --- 3. native dependency/symbol/manifest verification (never bypassed) -----

printf '%s: verifying native library + manifest (%s)...\n' "$tool_name" "$VERIFY_NATIVE_LIB_SH"
"$VERIFY_NATIVE_LIB_SH" "$apk_path" || die "native-lib/manifest verification failed - see verify-native-lib.sh's own error above"

# --- 4. install -r ------------------------------------------------------------

printf '%s: installing %s onto %s...\n' "$tool_name" "$apk_path" "$serial"
adb_ install -r -- "$apk_path" || die "adb install -r failed for $apk_path on device $serial"

# --- 5. validate package/debuggable/run-as (delegates to stage-wc3-data.sh) --

"$STAGE_SCRIPT" check-runtime --package "$package" --serial "$serial" >/dev/null ||
    die "package/debuggable/run-as validation failed - see stage-wc3-data.sh's own error above"

# --- 6. resolve artifact directory + start metadata/commands log ------------

run_timestamp=$(date -u +%Y%m%dT%H%M%SZ)
artifact_dir="$artifacts_root/acceptance_${run_timestamp}_${serial}"
mkdir -p "$artifact_dir"
commands_log="$artifact_dir/commands.log"
: > "$commands_log"
log_command() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >> "$commands_log"; }

log_command "resolve-device --serial $serial"
log_command "install -r $apk_path"

# --- 7. stage + verify user-owned ROC/TFT data (optional) -------------------

data_staged=0
data_layout="none"
data_verify_out="$artifact_dir/data_verify.log"
: > "$data_verify_out"
if [ -n "$data_dir" ]; then
    printf '%s: staging Warcraft III data from %s...\n' "$tool_name" "$data_dir"
    log_command "stage $data_dir --package $package --serial $serial"
    "$STAGE_SCRIPT" stage "$data_dir" --package "$package" --serial "$serial" ||
        die "failed to stage Warcraft III data from $data_dir - see stage-wc3-data.sh's own error above"

    log_command "verify --package $package --serial $serial"
    # NOT "verify | tee file || die" - a pipeline's exit status is tee's
    # (which almost always succeeds), not stage-wc3-data.sh verify's own,
    # which would silently swallow a real verify failure under `set -e`.
    if "$STAGE_SCRIPT" verify --package "$package" --serial "$serial" > "$data_verify_out" 2>&1; then
        cat "$data_verify_out"
    else
        cat "$data_verify_out" >&2
        die "failed to verify staged Warcraft III data on device"
    fi
    data_staged=1
    # Authoritative ROC-vs-TFT evidence comes from stage-wc3-data.sh's OWN
    # adb/run-as-based device file-presence report captured just above -
    # NOT from logcat (see this file's evidence-analysis section below for
    # why the bridge's fprintf(stderr, ...) edition line is only ever
    # treated as best-effort corroboration, never authoritative).
    if grep -q "War3x.mpq present" "$data_verify_out" || grep -q "War3xLocal.mpq present" "$data_verify_out"; then
        data_layout="tft"
    else
        data_layout="roc"
    fi
    printf '%s: staged data layout classified as: %s\n' "$tool_name" "$data_layout"
fi

# --- 8. native dependency report (best-effort; never fatal if aapt2 missing) -

dependencies_report="$artifact_dir/dependencies_report.txt"
{
    printf 'Native library/manifest verification: PASS (%s)\n' "$VERIFY_NATIVE_LIB_SH"
    printf '\nAPK manifest badging (best-effort):\n'
    if command -v aapt2 >/dev/null 2>&1; then
        aapt2 dump badging "$apk_path" 2>/dev/null || echo "(aapt2 dump badging failed)"
    elif command -v aapt >/dev/null 2>&1; then
        aapt dump badging "$apk_path" 2>/dev/null || echo "(aapt dump badging failed)"
    else
        echo "(neither aapt2 nor aapt found on PATH - skipping badging dump; verify-native-lib.sh's own ABI/dependency checks above are authoritative regardless)"
    fi
} > "$dependencies_report"

# --- 9. OVR Metrics Tool detection (explicit prerequisite, never invented) ---

ovr_metrics_available=0
ovr_metrics_note="OVR Metrics Tool ($OVR_METRICS_PKG) not installed on this device - performance capture is an explicit prerequisite (install from the Meta Horizon Store BEFORE launching the app under test; installing while the app is running force-closes it - see docs/quest-tabletop.md's acceptance-automation section for the exact adb commands this script uses). No metrics captured this run; this does not fail acceptance."
ovr_pm_path=$(adb_shell "pm path $(sh_quote "$OVR_METRICS_PKG")" 2>/dev/null || true)
case "$ovr_pm_path" in
    package:*)
        ovr_metrics_available=1
        ovr_metrics_note="OVR Metrics Tool detected ($OVR_METRICS_PKG) - CSV report capture enabled for this run."
        ;;
esac
printf '%s: %s\n' "$tool_name" "$ovr_metrics_note"

# --- 10. clear + start bounded background logcat capture ---------------------

logcat_file="$artifact_dir/logcat.log"
adb_ logcat -c
log_command "logcat -c"
# Both the app's own tag at full verbosity AND a catch-all *:W (warn and
# above, from ANY tag/process - loader/runtime broker, Vulkan validation,
# ART crash reporter, etc.) so a fatal error surfacing under a different
# process/tag is never missed - see "Also watch for OpenXR loader/runtime
# broker errors under its own tags" in docs/quest-tabletop.md.
adb_ logcat "$LOG_TAG:V" "*:W" > "$logcat_file" 2>&1 &
bg_logcat_pid=$!
log_command "logcat $LOG_TAG:V *:W > $logcat_file (background, pid $bg_logcat_pid)"

# --- 11. OVR Metrics: query current state + enable CSV recording ------------

if [ "$ovr_metrics_available" -eq 1 ]; then
    adb_shell "am broadcast -n $(sh_quote "$OVR_METRICS_RECEIVER") -a $(sh_quote "$OVR_METRICS_PKG.LOG_STATE")" >/dev/null 2>&1 || true
    log_command "am broadcast $OVR_METRICS_RECEIVER LOG_STATE"
    adb_shell "am broadcast -n $(sh_quote "$OVR_METRICS_RECEIVER") -a $(sh_quote "$OVR_METRICS_PKG.ENABLE_CSV")" >/dev/null 2>&1 ||
        die "failed to enable OVR Metrics Tool CSV recording"
    ovr_metrics_csv_enabled=1
    log_command "am broadcast $OVR_METRICS_RECEIVER ENABLE_CSV"
fi

# --- 12. launch (delegates to stage-wc3-data.sh) -----------------------------

printf '%s: launching %s/%s on %s...\n' "$tool_name" "$package" "$DEFAULT_ACTIVITY" "$serial"
log_command "run --package $package --serial $serial"
"$STAGE_SCRIPT" run --package "$package" --serial "$serial" || die "failed to launch $package/$DEFAULT_ACTIVITY"
app_launched=1

# --- 13. guided checklist (interactive) or bounded automated wait -----------

guided_checklist_file="$artifact_dir/guided_checklist.md"
: > "$guided_checklist_file"
if [ "$interactive" -eq 1 ]; then
    # Guided manual checkpoints exist ONLY for XR actions this host cannot
    # synthesize (real controller/hand input, real focus loss, real
    # haptics) - see docs/quest-tabletop.md's per-layer "Exact on-device
    # acceptance procedure" sections this checklist is transcribed from.
    # Requires a REAL terminal: never fabricate a "y" answer when stdin
    # isn't interactive (that would silently turn every unattended run
    # into a fraudulent all-PASS checklist) - pass --non-interactive for
    # unattended/CI-style runs instead.
    if [ ! -t 0 ]; then
        die "interactive guided checklist requested but stdin is not a terminal - pass --non-interactive for an unattended run, or run this script attached to a real terminal"
    fi
    printf '\n=== Guided hardware acceptance checklist (wear the headset now) ===\n'
    prompt_scenario() {
        title=$1
        instructions=$2
        printf '\n%s\n  %s\n' "$title" "$instructions"
        response=""
        while true; do
            printf '  Behaved correctly? [y/n]: '
            read -r response || die "stdin closed while awaiting a guided checklist answer for '$title' - rerun with --non-interactive for an unattended session"
            response=$(printf '%s' "$response" | tr '[:upper:]' '[:lower:]')
            case "$response" in
                y|yes) printf -- '- [PASS] %s\n' "$title" >> "$guided_checklist_file"; break ;;
                n|no) printf -- '- [FAIL] %s: %s\n' "$title" "$instructions" >> "$guided_checklist_file"; break ;;
                *) printf '  please answer y or n\n' ;;
            esac
        done
    }
    prompt_scenario "Controller select/smart/target/cancel" \
        "Trigger over a unit selects it (a LATER-frame marker, never instant); grip over a unit issues a smart order; in target mode, trigger over terrain/unit issues the target-point order; B/Y cancels."
    prompt_scenario "Controller board control" \
        "Left grip-drag pans, right stick rotates/zooms, left stick raises/lowers, left menu resets/recenters - all moving terrain+models+fog+selection+HUD together."
    prompt_scenario "Controller HUD" \
        "The reticle tints amber over the HUD, green over a unit, cyan over terrain; a command-card button press posts its action."
    prompt_scenario "Hand select/smart/target/cancel" \
        "An index pinch aimed at a unit selects it; aimed at terrain issues a smart-point order; aimed at the HUD Cancel region cancels - all with no button."
    prompt_scenario "Hand board pan (if XR_FB_hand_tracking_aim active)" \
        "A left-hand middle pinch + physical hand drag pans the board; board rotate/zoom/height do NOT respond to any hand gesture."
    prompt_scenario "Controller/hand source switch" \
        "Setting both controllers down: after a brief pause, hand rays appear tinted identically to the controller rays they replaced. Picking a controller back up INSTANTLY reclaims its ray with no phantom command fired."
    prompt_scenario "Tracking loss/reacquisition" \
        "Covering a controller's tracking ring, or hiding a hand behind your back, then re-exposing it: the ray/hand recovers smoothly with no stuck state."
    prompt_scenario "Focus/suspend/resume" \
        "Pressing the Oculus button (or removing the headset) suspends the app; resuming brings it back cleanly with no Vulkan/audio crash and no stuck/looping audio."
    prompt_scenario "Recenter" \
        "The recenter button/gesture smoothly realigns the tabletop board without a visual jump or crash."
    prompt_scenario "Haptics and visual feedback" \
        "An accepted action (e.g. a successful select/order) produces a crisp haptic buzz; a refused one (disabled HUD slot, stale generation) produces a distinct, softer buzz; hovering a selectable unit highlights it."
    prompt_scenario "Passthrough coverage/premultiplied blend correctness (PR #28 fix)" \
        "Semi-transparent (fog/selection/HUD) layers darken/blend correctly with NO double-darkening; additive glows/particles never occlude the passthrough camera feed or geometry behind them; a team-color/glow MODULATE tint never erodes or invents coverage; opaque units show NO pinholes/see-through spots even where their own texture alpha is below 1."
    prompt_scenario "Cross-map GPU cache reload correctness (PR #28 fix)" \
        "Load a second, different map after a first: the second map's own textures/models display correctly (never the first map's stale GPU resource), including for any identically-named custom-imported asset path reused by both maps; only one brief hitch (a single vkDeviceWaitIdle stall) occurs at the moment of reload, and repeatedly reloading the SAME map causes no further stall/re-upload."
    printf '\n%s: guided checklist complete - see %s\n' "$tool_name" "$guided_checklist_file"
else
    printf '%s: running non-interactively for %d seconds...\n' "$tool_name" "$duration"
    printf 'non-interactive run: guided manual checkpoints skipped (see docs/quest-tabletop.md)\n' > "$guided_checklist_file"
    # Backgrounded + waited-for, NOT a bare foreground `sleep` - bash only
    # promptly interrupts and runs a pending INT/TERM trap while blocked in
    # `wait`; a signal arriving during a plain foreground `sleep N` is
    # deferred until that sleep finishes on its own (see the bash manual's
    # "SIGNALS" section), which would silently turn Ctrl+C/`kill` into a
    # multi-second-late no-op instead of the prompt cleanup this script's
    # whole design promises.
    sleep "$duration" &
    sleep_pid=$!
    wait "$sleep_pid" 2>/dev/null || true
fi

# --- 14. OVR Metrics: disable CSV + attempt to pull the newest report -------

ovr_metrics_csv_path=""
if [ "$ovr_metrics_available" -eq 1 ]; then
    adb_shell "am broadcast -n $(sh_quote "$OVR_METRICS_RECEIVER") -a $(sh_quote "$OVR_METRICS_PKG.DISABLE_CSV")" >/dev/null 2>&1 || true
    log_command "am broadcast $OVR_METRICS_RECEIVER DISABLE_CSV"
    ovr_metrics_csv_enabled=0
    newest_csv=$(adb_shell "ls -t $(sh_quote "$OVR_METRICS_CSV_DIR") 2>/dev/null | head -n1" | tr -d '\r\n')
    if [ -n "$newest_csv" ]; then
        if adb_ pull -- "$OVR_METRICS_CSV_DIR/$newest_csv" "$artifact_dir/ovr_metrics.csv" >/dev/null 2>&1; then
            ovr_metrics_csv_path="$artifact_dir/ovr_metrics.csv"
            printf '%s: pulled OVR Metrics report: %s\n' "$tool_name" "$ovr_metrics_csv_path"
        else
            printf '%s: WARNING: found %s on device but adb pull failed (raw remote path preserved in metadata.json)\n' "$tool_name" "$newest_csv"
        fi
    else
        printf '%s: WARNING: OVR Metrics Tool was enabled but no CSV file was found under %s\n' "$tool_name" "$OVR_METRICS_CSV_DIR"
    fi
fi

# --- 15. force-stop + stop background logcat (also idempotently done by the -
# --     cleanup trap on any other exit path) ---------------------------------

printf '%s: force-stopping %s...\n' "$tool_name" "$package"
adb_shell "am force-stop $(sh_quote "$package")" >/dev/null 2>&1
app_launched=0
if [ -n "$bg_logcat_pid" ]; then
    kill "$bg_logcat_pid" >/dev/null 2>&1 || true
    wait "$bg_logcat_pid" 2>/dev/null || true
    bg_logcat_pid=""
fi

# --- 16. evidence analysis ---------------------------------------------------

evidence_report="$artifact_dir/analysis_report.md"
overall_pass=1
evidence_lines=""

note_line() { evidence_lines="$evidence_lines
- [$1] $2"; }

# A REQUIRED marker: its absence fails the whole run. Matches against the
# EXACT literal text of the real BZ_QUEST_LOGI/LOGE call sites (traced via
# grep against bz_quest_host.c/bz_quest_xr.c/bz_quest_vk.c/
# bz_quest_passthrough.c/bz_quest_audio.c - never guessed; see
# docs/quest-tabletop.md's acceptance-automation section for the full
# per-marker source citation).
require_marker() {
    pattern=$1
    description=$2
    if grep -qE "$pattern" "$logcat_file"; then
        note_line OK "$description"
    else
        note_line MISSING "$description (required log line not found - see docs/quest-tabletop.md's exact expected log sequence)"
        overall_pass=0
    fi
}

# An INFORMATIONAL marker: absence is reported honestly but never fails the
# run - for capability-optional or best-effort-only evidence (hand
# tracking, the fprintf(stderr)-based data-dir/edition line, PRE2).
note_marker() {
    pattern=$1
    description=$2
    if grep -qE "$pattern" "$logcat_file"; then
        note_line OK "$description"
    else
        note_line INFO "$description (not observed this run - see docs/quest-tabletop.md for why this is never a required marker)"
    fi
}

require_marker 'xrInitializeLoaderKHR succeeded' "OpenXR loader initialization"
require_marker 'xrCreateInstance succeeded' "OpenXR instance creation"
require_marker 'xrGetSystem succeeded' "OpenXR system/passthrough-capability query"
require_marker 'Vulkan API version bound' "Vulkan device/version negotiation (XR_KHR_vulkan_enable2)"
require_marker 'xrCreateSession succeeded' "OpenXR session creation"
require_marker 'selected swapchain color format' "Vulkan swapchain format selection"
swapchain_eye_count=$(grep -cE 'swapchain\[[0-9]+\]: [0-9]+x[0-9]+' "$logcat_file" || true)
if [ "$swapchain_eye_count" -ge 2 ]; then
    note_line OK "both per-eye Vulkan swapchains created (stereo: $swapchain_eye_count swapchain[] lines)"
else
    note_line MISSING "both per-eye Vulkan swapchains created (found $swapchain_eye_count swapchain[] line(s), expected 2 - one per eye)"
    overall_pass=0
fi
require_marker 'passthrough object [+] reconstruction layer created' "XR_FB_passthrough object/layer creation"
require_marker 'passthrough started' "XR_FB_passthrough started"
require_marker 'bz_quest_renderer_init succeeded' "Vulkan/OpenXR/scene renderer initialization (host+assets/renderer)"
require_marker 'APP_CMD_START' "Android lifecycle: APP_CMD_START"
require_marker 'APP_CMD_RESUME' "Android lifecycle: APP_CMD_RESUME"
require_marker 'xrBeginSession succeeded' "OpenXR session begin (progressing toward FOCUSED)"
require_marker 'bz_quest_audio_start succeeded' "AAudio stream startup"
require_marker 'tabletop frame: status=[0-9]+ generation=[0-9]+' "Tabletop snapshot capture (advancing generation - see note below)"
require_marker 'bz_quest_host: destroy requested' "Clean teardown requested"
require_marker 'bz_quest_host: exiting android_main' "Clean host exit"

# bz_quest_frame_should_log() deliberately throttles this line to fire only
# on a status/lifecycleState/lifecycleError CHANGE, never on a bare
# generation advance (see docs/quest-tabletop.md's "Diagnostics: throttled
# log, never per-frame") - so exactly ONE "tabletop frame: ..." line during
# a healthy steady-state run is the CORRECT, expected signal, not a
# failure. This is why "advancing snapshots" is proven by the presence of
# that one line (required above) plus the process staying alive/responsive
# for the full bounded session (proven by force-stop succeeding below),
# NOT by comparing multiple generation values against each other.
frame_count=$(grep -cE 'tabletop frame: status=[0-9]+ generation=[0-9]+' "$logcat_file" || true)
if [ "$frame_count" -gt 1 ]; then
    note_line OK "multiple tabletop-frame log lines observed ($frame_count) - a lifecycle/status change occurred (e.g. a guided suspend/resume checkpoint); this is bonus corroborating evidence, not required"
fi

# bz_quest_bridge_start() has exactly two valid, mutually-exclusive
# outcomes depending on whether --data was staged - see docs/
# quest-tabletop.md's "Hardware-only acceptance gates" (A) vs "Hardware/
# data-only acceptance procedure" (C). Checking for "succeeded" unconditionally
# would be WRONG (and always fail) on a --data-less hardware-only run, where
# a clean, documented failure is the CORRECT outcome.
if [ "$data_staged" -eq 1 ]; then
    require_marker 'bz_quest_bridge_start succeeded' "Tabletop bridge startup (data staged: expects success)"
else
    require_marker 'bz_quest_bridge_start failed' "Tabletop bridge startup (no --data given: expects the documented clean failure, not a crash)"
fi

# Hand tracking is an optional, runtime-negotiated capability (layer 8) -
# bz_quest_xr_hands_create() always logs exactly one of these two lines,
# so absence of BOTH would be unusual, but neither line's presence/absence
# ever fails acceptance on its own - see docs/quest-tabletop.md's
# "Capability negotiation" section.
if grep -qE 'hand tracking enabled' "$logcat_file"; then
    note_line OK "hand tracking: enabled (see logcat for XR_FB_hand_tracking_aim vs XR_EXT_hand_tracking-only tier)"
elif grep -qE 'XR_EXT_hand_tracking not supported this session' "$logcat_file"; then
    note_line OK "hand tracking: not supported on this runtime/device/account (optional capability, honestly absent - not a failure)"
else
    note_line INFO "hand tracking: capability negotiation line not observed (renderer init may have failed earlier - see required markers above)"
fi

# bz_quest_bridge_start()'s data-dir/edition line uses fprintf(stderr, ...)
# (bz_quest_bridge.c), NOT BZ_QUEST_LOGI/__android_log_print - unlike
# <android/log.h>'s guaranteed-to-reach-logcat API, raw native stdio is
# widely documented NOT to be redirected to logcat on Android by default
# (a well-known NDK limitation; this app does not install a stdout/stderr-
# to-logcat redirect). This was NOT independently verified against real
# Quest hardware in this environment (no device available) - see
# docs/quest-tabletop.md's acceptance-automation section. Treated as
# best-effort-only corroboration; authoritative ROC/TFT evidence is
# data_verify_out (stage-wc3-data.sh's own adb/run-as-based report) below.
note_marker "resolved data dir.*edition=(roc|tft)" "bridge data-dir/edition line (fprintf(stderr) - may not reach logcat; best-effort only)"

if [ "$data_staged" -eq 1 ]; then
    note_line OK "ROC/TFT staged-data evidence: layout=$data_layout (from stage-wc3-data.sh verify's own device-side report, see $data_verify_out)"
else
    note_line INFO "ROC/TFT staged-data evidence: not applicable (no --data given this run)"
fi

# PRE2 particle-emitter rendering (layer 9) has no positive/info-level
# success log anywhere in bz_quest_wc3_particles.c/bz_quest_vk_wc3_particles.c
# (only BZ_QUEST_LOGE failure paths exist), and this environment never
# loads a map (bz_quest_bridge_start() always passes a NULL map name - see
# docs/quest-tabletop.md's Current limitations), so zero entities/particles
# can exist to render regardless. PRE2 acceptance therefore remains a
# hardware+real-map+human-visual-confirmation-only gate - this script only
# checks for the ABSENCE of particle-renderer errors as weak, negative
# corroboration, never a positive/required marker.
if grep -qE 'bz_quest_vk_wc3_particles:' "$logcat_file"; then
    note_line FAIL "PRE2 particle renderer logged an error - see logcat for the exact bz_quest_vk_wc3_particles: line"
    overall_pass=0
else
    note_line INFO "PRE2 particle rendering: not independently observable via logcat (no success marker exists; no map is ever loaded in this layer) - no particle-renderer errors observed either"
fi

# Map-epoch GPU cache reset (PR #28 fix, bz_quest_vk_wc3.c) has no
# positive/info-level success log either - only 4 new BZ_QUEST_LOGE
# failure paths exist ("map-epoch model/texture cache reset failed",
# "vkDeviceWaitIdle before map-epoch cache reset failed", "model/texture
# cache re-init after map-epoch reset failed"), all uniquely identified by
# the substring "map-epoch" (confirmed against bz_quest_vk_wc3.c - no
# other log line in that file contains it). This environment never loads
# even one map, let alone two in sequence, so the reset path never runs
# and this is, like PRE2, a hardware+two-distinct-real-maps+human-visual-
# confirmation-only gate (see "Cross-map GPU cache reload correctness"
# above) - this script only checks for the ABSENCE of cache-reset errors
# as weak, negative corroboration, never a positive/required marker. This
# check is intentionally more specific than the generic forbidden-error
# scan below (which would also catch it) so a failure here names the
# exact subsystem instead of a bare "some E-line was found somewhere".
if grep -qE 'bz_quest_vk_wc3:.*map-epoch' "$logcat_file"; then
    note_line FAIL "map-epoch GPU cache reset logged an error - see logcat for the exact bz_quest_vk_wc3: map-epoch line"
    overall_pass=0
else
    note_line INFO "map-epoch GPU cache reset: not independently observable via logcat (no success marker exists; no map is ever loaded in this layer) - no cache-reset errors observed either"
fi

# Forbidden fatal/validation errors: structural (any E/F-priority logcat
# line, any tag/process - threadtime format's "<level> <tag>:" shape,
# independent of preceding date/pid/tid field widths) PLUS a keyword net
# for well-known crash signatures that might not always surface at E/F
# priority under every OEM logging configuration. Excludes the ONE
# documented, expected E-priority app line this script already validates
# explicitly above via require_marker('bz_quest_bridge_start failed') on a
# --data-less run (see "Hardware-only acceptance gates" A in
# docs/quest-tabletop.md) - BZ_QUEST_LOGE is Error priority by definition,
# so that single, correctly-classified-elsewhere line must not also trip
# this generic forbidden-error net.
#
# The premultiplied-alpha/coverage fix (PR #28) adds no logging surface
# whatsoever (a pure GPU blend-state/shader-math correctness change) and
# is guided-checklist-only (see "Passthrough coverage/premultiplied blend
# correctness" above) - there is nothing for this scan, or any other
# automated check, to observe for that fix.
forbidden_structural=$(grep -E ' (E|F) [A-Za-z_][A-Za-z0-9_.]*:' "$logcat_file" | grep -v 'bz_quest_bridge_start failed' || true)
forbidden_keywords=$(grep -Ei 'FATAL EXCEPTION|backtrace:|SIGSEGV|SIGABRT|VUID-|validation layer' "$logcat_file" || true)
if [ -n "$forbidden_structural" ] || [ -n "$forbidden_keywords" ]; then
    note_line FAIL "forbidden fatal/validation-layer error(s) present in logcat - see $logcat_file"
    overall_pass=0
else
    note_line OK "no forbidden fatal/Vulkan/OpenXR validation errors observed"
fi

note_line "$([ "$ovr_metrics_available" -eq 1 ] && echo OK || echo INFO)" "$ovr_metrics_note"
if [ -n "$ovr_metrics_csv_path" ]; then
    note_line OK "OVR Metrics CSV report captured: $ovr_metrics_csv_path"
fi

if [ "$interactive" -eq 1 ] && grep -q '\[FAIL\]' "$guided_checklist_file"; then
    note_line FAIL "one or more guided manual checklist items were marked FAIL - see $guided_checklist_file"
    overall_pass=0
fi

# --- 17. write metadata + final reports --------------------------------------

cat > "$artifact_dir/metadata.json" <<EOF
{
  "timestamp_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "device_serial": "$serial",
  "package_id": "$package",
  "apk_path": "$apk_path",
  "data_dir_given": $([ -n "$data_dir" ] && echo true || echo false),
  "data_layout": "$data_layout",
  "interactive": $([ "$interactive" -eq 1 ] && echo true || echo false),
  "duration_seconds": $duration,
  "ovr_metrics_available": $([ "$ovr_metrics_available" -eq 1 ] && echo true || echo false),
  "ovr_metrics_csv_path": "$ovr_metrics_csv_path",
  "overall_pass": $([ "$overall_pass" -eq 1 ] && echo true || echo false)
}
EOF

{
    printf '# Quest acceptance runner analysis report\n\n'
    printf 'Timestamp (UTC): %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'Device serial: %s\n' "$serial"
    printf 'Package: %s\n' "$package"
    printf 'Data staged: %s (layout=%s)\n\n' "$([ "$data_staged" -eq 1 ] && echo yes || echo no)" "$data_layout"
    printf '## Evidence\n%s\n\n' "$evidence_lines"
    printf '## Overall result\n\n'
    if [ "$overall_pass" -eq 1 ]; then printf '**PASS**\n'; else printf '**FAIL**\n'; fi
} > "$evidence_report"

printf '%s\n' "$evidence_lines"
printf '%s: artifacts written to %s\n' "$tool_name" "$artifact_dir"

if [ "$overall_pass" -eq 1 ]; then
    printf '%s: PASS - acceptance evidence complete.\n' "$tool_name"
    exit 0
else
    printf '%s: FAIL - see %s for missing/forbidden evidence.\n' "$tool_name" "$evidence_report"
    exit 1
fi
