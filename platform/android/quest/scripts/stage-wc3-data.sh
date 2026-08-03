#!/bin/sh
# platform/android/quest/scripts/stage-wc3-data.sh - layer 7's developer ADB
# workflow for staging a user-owned Warcraft III ROC/TFT install onto a Quest
# device's app-private storage, targeting the exact data-path/override-file
# contract bz_quest_data.h/.c (layer 4) already implements and documents. See
# docs/quest-tabletop.md's "Layer 7: data staging + native audio" section for
# the full write-up this script's behavior is documented against.
#
# Why run-as + /data/local/tmp instead of `adb push` to /sdcard/Android/data:
# scoped storage (Android 10+ / API 29+) blocks other processes - including
# the adbd shell user pushing directly - from writing into another app's
# external-storage sandbox without that app's own runtime API cooperating;
# see https://developer.android.com/training/data-storage/app-specific's
# "Access from internal storage" section for why app-private storage (what
# ANativeActivity::internalDataPath resolves to - Context.getFilesDir()) is
# only reachable by the app's own UID. `run-as` lets an adb shell assume a
# *debuggable* app's UID for exactly this kind of development-time file
# access - see https://developer.android.com/studio/debug's "run-as
# your-package-name pwd" capability check - but `run-as` cannot itself
# receive arbitrary local file bytes as stdin/argv from the host; the
# well-established two-hop workaround (used by IDE "Device File Explorer"
# tooling on API <30 and documented across the Android developer community)
# is: `adb push` the bytes to /data/local/tmp (owned by the shell user,
# world-readable, NOT part of the scoped-storage model - it predates and is
# unaffected by it), then have `run-as` `cp` from there into the app's own
# sandbox, then remove the /data/local/tmp copy. Every device-side command
# below is built as ONE pre-quoted string and passed to `adb shell` as a
# single argument - `adb shell` with >1 argv word joins them with spaces
# before handing off to the remote shell, which would silently re-tokenize
# any host path/package value containing spaces or shell metacharacters.
set -eu

tool_name=${0##*/}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)

# Must exactly match bz_quest_data.h's BZ_QUEST_DATA_SUBDIR / OVERRIDE_FILENAME
# (platform/android/quest/app/src/main/cpp/bz_quest_data.h) - this script and
# that header are two halves of the same contract and must never drift.
DATA_SUBDIR="Warcraft III"
OVERRIDE_FILENAME="warcraft_data_path_override.txt"

# The three archive filenames games/warcraft-3's own common/common.c looks
# for (case-insensitively - see FS_AddDataDirectory()'s ".mpq" extension
# scan and its "War3x" 5-char expansion-archive prefix check). TFT is an
# overlay on ROC (see docs/quest-tabletop.md and platform/apple/visionos/
# scripts/wc3_data.sh, which requires all three unconditionally); this
# script additionally accepts ROC standing alone, since Quest layer 7 must
# "support ROC-only and TFT-over-ROC explicitly" per its task scope.
ROC_ARCHIVE="War3.mpq"
TFT_MAIN_ARCHIVE="War3x.mpq"
TFT_LOCALE_ARCHIVE="War3xLocal.mpq"

DEFAULT_PACKAGE="org.openrealm.quest" # platform/android/quest/app/build.gradle's applicationId
ACTIVITY="android.app.NativeActivity" # AndroidManifest.xml's single plain NativeActivity entry point
LOG_TAG="OpenRealmQuest"              # bz_quest_log.h's BZ_QUEST_LOG_TAG

# Overridable for the fake-device test harness (test-stage-wc3-data.sh) -
# never overridden in normal developer use, where the real `adb` on PATH is
# used exactly as any other adb-based tool would.
ADB=${BZ_QUEST_ADB:-adb}

# The world-writable, non-scoped-storage bounce location described in this
# file's header comment. Overridable only so the fake-device test harness
# can point it at a real host directory instead of an actual device's
# /data/local/tmp - never overridden for a real device.
REMOTE_TMP_DIR=${BZ_QUEST_STAGE_REMOTE_TMP:-/data/local/tmp}

package=$DEFAULT_PACKAGE
serial=""

usage() {
    cat >&2 <<EOF
Usage:
  $tool_name stage <local-wc3-data-dir> [--package PKG] [--serial SERIAL]
  $tool_name verify [--package PKG] [--serial SERIAL]
  $tool_name clean --yes [--package PKG] [--serial SERIAL]
  $tool_name run [--package PKG] [--serial SERIAL]
  $tool_name log [--package PKG] [--serial SERIAL]
  $tool_name resolve-device [--serial SERIAL]
  $tool_name check-runtime [--package PKG] [--serial SERIAL]

Stages a local Warcraft III ROC (War3.mpq) or TFT-over-ROC (War3.mpq +
War3x.mpq + War3xLocal.mpq) install into the debuggable Quest app's private
"$DATA_SUBDIR" directory over adb (via run-as; no root, no /sdcard/Android/
data access required - see this script's header comment), then writes the
$OVERRIDE_FILENAME override file bz_quest_data.c's data-path contract reads.
Never bundles, copies, or commits Warcraft III data into this repository or
into the built APK - see docs/quest-tabletop.md.

  --package PKG   Application ID to target (default: $DEFAULT_PACKAGE).
  --serial SERIAL adb device/emulator serial (default: the sole attached
                  device; required if more than one is attached).

resolve-device prints the resolved target device serial (applying the same
no-device/multiple-without-serial/unknown/offline rejection rules as every
other subcommand above); check-runtime additionally validates the package is
installed and debuggable (run-as capable) and prints its resolved data root.
Both exist so scripts/acceptance-runner.sh can reuse this script's own
device/package validation instead of duplicating it - see
docs/quest-tabletop.md's acceptance-automation section.
EOF
}

die() {
    printf '%s: %s\n' "$tool_name" "$*" >&2
    exit 1
}

# POSIX single-quote-safe escaping for one word, so every remote command this
# script builds is assembled as ONE pre-quoted string before it ever reaches
# `adb shell` - see this file's header comment for why that matters.
sh_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

adb_() {
    if [ -n "$serial" ]; then
        "$ADB" -s "$serial" "$@"
    else
        "$ADB" "$@"
    fi
}

# Runs one pre-built, already-quoted command string on the device's default
# shell. Callers must build $1 with sh_quote for every path/package-derived
# word - see this file's header comment.
adb_shell() {
    adb_ shell "$1"
}

run_as() {
    # $1 = inner command string (already fully quoted); wraps it in
    # `run-as <package> sh -c <inner>` and sends the whole thing as one
    # adb-shell argument.
    adb_shell "run-as $(sh_quote "$package") sh -c $(sh_quote "$1")"
}

# --- local host helpers -----------------------------------------------------

local_sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum -- "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 -- "$1" | awk '{print $1}'
    else
        die "no sha256sum/shasum found on this host - cannot verify transferred archive bytes"
    fi
}

local_size() {
    wc -c < "$1" | tr -d ' '
}

# --- device readiness (explicit, no silent fallback) ------------------------

require_device() {
    device_list=$(adb_ devices 2>/dev/null | awk 'NR>1 && NF>0')
    if [ -z "$device_list" ]; then
        die "no adb device/emulator attached - connect the Quest over USB (or Wi-Fi debugging) and accept its debugging prompt, then retry"
    fi
    if [ -n "$serial" ]; then
        state=$(printf '%s\n' "$device_list" | awk -v s="$serial" '$1==s{print $2}')
        [ -n "$state" ] || die "no attached device matches --serial '$serial' (attached: $(printf '%s' "$device_list" | awk '{print $1}' | tr '\n' ' '))"
    else
        count=$(printf '%s\n' "$device_list" | wc -l | tr -d ' ')
        if [ "$count" -gt 1 ]; then
            die "more than one adb device attached - pass --serial (attached: $(printf '%s' "$device_list" | awk '{print $1}' | tr '\n' ' '))"
        fi
        state=$(printf '%s\n' "$device_list" | awk '{print $2}')
        # Records the auto-selected sole device back into $serial (was only
        # used locally for the state check above until now) so callers -
        # notably cmd_resolve_device() below - can rely on $serial always
        # being populated with the exact device every subsequent adb_() call
        # in this run targets, whether auto-selected or --serial-provided.
        serial=$(printf '%s\n' "$device_list" | awk '{print $1}')
    fi
    case "$state" in
        device) ;;
        unauthorized) die "device found but unauthorized - accept the USB/Wi-Fi debugging prompt on the headset, then retry" ;;
        offline) die "device found but offline - reconnect/replug the headset, then retry" ;;
        *) die "device found in unexpected adb state '$state'" ;;
    esac
}

require_package_installed() {
    path_out=$(adb_shell "pm path $(sh_quote "$package")" 2>/dev/null || true)
    case "$path_out" in
        package:*) ;;
        *) die "package '$package' is not installed on the device - run 'make -f platform/android/quest/build.mk quest-install-debug' first" ;;
    esac
}

require_debuggable_run_as() {
    if ! run_as_out=$(run_as "echo ok" 2>&1); then
        die "run-as failed for package '$package' ($run_as_out) - this requires a debuggable build (Gradle's assembleDebug already sets android:debuggable=true) on a userdebug/eng-capable device; a release build or a locked-down production device cannot be staged this way"
    fi
    [ "$run_as_out" = "ok" ] || die "run-as for package '$package' produced unexpected output: $run_as_out"
}

# Sets pkg_root (absolute, no trailing slash) to the package's private data
# root - i.e. what `run-as <pkg> pwd` reports, confirmed by
# https://developer.android.com/studio/debug's "run-as your-package-name
# pwd" capability check to be run-as's own default working directory.
resolve_pkg_root() {
    pkg_root=$(run_as "pwd")
    case "$pkg_root" in
        /*) ;;
        *) die "run-as reported a non-absolute package root '$pkg_root' - cannot build the override file's required absolute path" ;;
    esac
}

require_free_space() {
    needed=$1
    avail_kb=$(run_as "df -Pk . | awk 'NR==2{print \$4}'") || die "could not query free space on the device for package '$package'"
    case "$avail_kb" in
        ''|*[!0-9]*) die "could not parse free space from the device (got '$avail_kb')" ;;
    esac
    avail_bytes=$((avail_kb * 1024))
    if [ "$avail_bytes" -lt "$needed" ]; then
        die "insufficient space on device: need $needed bytes, only $avail_bytes available in package '$package''s storage - free up space and retry"
    fi
}

# --- source-side ROC/TFT validation (before any device I/O) -----------------

# Case-insensitively finds a file in $1 matching basename $2, printing its
# EXACT on-disk name (preserving whatever case/spacing the user's install
# actually uses - never renamed/canonicalized) or nothing if absent.
find_ci() {
    find "$1" -maxdepth 1 -type f -iname "$2" -print | head -n1
}

# Populates roc_file/tft_main_file/tft_locale_file (exact on-disk names, or
# empty) and layout ("roc" or "tft"), or dies with an actionable message for
# any incomplete/mixed layout - "Reject incomplete mixed layouts with
# actionable errors" per this layer's task scope.
resolve_source_layout() {
    src=$1
    [ -d "$src" ] || die "source directory does not exist: $src"
    roc_file=$(find_ci "$src" "$ROC_ARCHIVE")
    tft_main_file=$(find_ci "$src" "$TFT_MAIN_ARCHIVE")
    tft_locale_file=$(find_ci "$src" "$TFT_LOCALE_ARCHIVE")

    if [ -z "$roc_file" ]; then
        die "missing required ROC archive '$ROC_ARCHIVE' in $src - TFT is an overlay on ROC (see common/common.c's archive scan) and cannot be staged standalone"
    fi
    if [ ! -s "$roc_file" ]; then
        die "required file is empty: $roc_file"
    fi

    if [ -n "$tft_main_file" ] || [ -n "$tft_locale_file" ]; then
        if [ -z "$tft_main_file" ]; then
            die "incomplete TFT layout in $src: found '$tft_locale_file' but missing '$TFT_MAIN_ARCHIVE'"
        fi
        if [ -z "$tft_locale_file" ]; then
            die "incomplete TFT layout in $src: found '$tft_main_file' but missing '$TFT_LOCALE_ARCHIVE'"
        fi
        [ -s "$tft_main_file" ] || die "required file is empty: $tft_main_file"
        [ -s "$tft_locale_file" ] || die "required file is empty: $tft_locale_file"
        layout="tft"
    else
        layout="roc"
    fi
}

# --- per-file staging (skip-if-unchanged / atomic replace) ------------------

# Returns (echoes) the sha256 of $1 (a device-relative dest path under the
# package's own working directory) if it already exists there, or nothing.
# Extracted so cmd_stage()'s free-space preflight and stage_one_file() share
# exactly one remote round-trip per file instead of querying it twice.
remote_file_hash() {
    run_as "test -f $(sh_quote "$1") && sha256sum -- $(sh_quote "$1") | awk '{print \$1}' || true"
}

# Streams $1 (local path) to $pkg_root/files/$DATA_SUBDIR/$2 (device-side
# basename, preserved exactly) via the /data/local/tmp bounce described in
# this file's header comment, then verifies via a fresh remote sha256sum
# before atomically renaming into place - "idempotent incremental behavior
# without trusting stale files" and "an interrupted transfer leaves the
# prior valid file intact" per this layer's task scope: nothing this
# function does ever removes or overwrites the final destination name
# except via one `mv` immediately preceded by a successful hash comparison.
# $3 is the dest file's existing remote sha256 (or empty if absent),
# already fetched once by cmd_stage()'s free-space preflight - passed in
# rather than re-queried here to avoid a second identical remote round-trip.
stage_one_file() {
    local_path=$1
    dest_name=$2
    existing_hash=$3
    dest_dir="files/$DATA_SUBDIR"
    dest_path="$dest_dir/$dest_name"
    local_hash=$(local_sha256 "$local_path")
    local_bytes=$(local_size "$local_path")

    if [ "$existing_hash" = "$local_hash" ]; then
        printf '%s: %s unchanged (sha256 %s), skipping transfer\n' "$tool_name" "$dest_name" "$local_hash"
        return 0
    fi

    remote_tmp_name="$REMOTE_TMP_DIR/bz_quest_stage.$$.$dest_name"
    dest_tmp_name="$dest_name.stage.$$"
    dest_tmp_path="$dest_dir/$dest_tmp_name"

    cleanup_remote_tmp() {
        adb_shell "rm -f -- $(sh_quote "$remote_tmp_name")" >/dev/null 2>&1 || true
    }
    trap cleanup_remote_tmp EXIT INT TERM

    if ! adb_ push -- "$local_path" "$remote_tmp_name" >/dev/null; then
        cleanup_remote_tmp
        trap - EXIT INT TERM
        die "adb push failed for '$local_path' - check the connection and device storage, then retry (the previously staged '$dest_name', if any, was left untouched)"
    fi

    run_as "mkdir -p $(sh_quote "$dest_dir")"
    # Removes only this file's own prior temp-name family (never a bare
    # wildcard delete of the directory) so a stale leftover from a killed
    # previous run never gets mistaken for today's transfer.
    run_as "rm -f -- $(sh_quote "$dest_dir")/$(sh_quote "$dest_name").stage.*"
    if ! run_as "cp -- $(sh_quote "$remote_tmp_name") $(sh_quote "$dest_tmp_path")"; then
        cleanup_remote_tmp
        trap - EXIT INT TERM
        die "device-side copy into '$dest_tmp_path' failed - the previously staged '$dest_name', if any, was left untouched"
    fi
    cleanup_remote_tmp
    trap - EXIT INT TERM

    staged_hash=$(run_as "sha256sum -- $(sh_quote "$dest_tmp_path") | awk '{print \$1}'")
    if [ "$staged_hash" != "$local_hash" ]; then
        run_as "rm -f -- $(sh_quote "$dest_tmp_path")"
        die "corruption detected staging '$dest_name': local sha256 $local_hash, device copy $staged_hash (temp file removed; the previously staged file, if any, was left untouched) - retry, and if this repeats, check the cable/connection"
    fi

    if ! run_as "mv -- $(sh_quote "$dest_tmp_path") $(sh_quote "$dest_path")"; then
        die "failed to atomically install '$dest_name' after verification (temp file left at '$dest_tmp_path' for inspection) - the previously staged file, if any, was left untouched"
    fi
    printf '%s: staged %s (%s bytes, sha256 %s)\n' "$tool_name" "$dest_name" "$local_bytes" "$local_hash"
}

# --- commands ----------------------------------------------------------------

cmd_stage() {
    src=$1
    [ -n "$src" ] || { usage; exit 2; }
    resolve_source_layout "$src"

    require_device
    require_package_installed
    require_debuggable_run_as
    resolve_pkg_root

    # Determine which files actually need transfer (remote hash missing or
    # mismatched) BEFORE estimating how much *extra* space this run will
    # transiently need. An already-fully-staged, byte-identical re-run must
    # not be blocked by a peak estimate sized for a fresh transfer - see
    # this function's peak-space comment below and docs/quest-tabletop.md's
    # free-space-preflight write-up.
    transfer_bytes=0
    max_transfer_bytes=0
    roc_dest_name=$(basename "$roc_file")
    roc_existing_hash=$(remote_file_hash "files/$DATA_SUBDIR/$roc_dest_name")
    if [ "$roc_existing_hash" != "$(local_sha256 "$roc_file")" ]; then
        roc_bytes=$(local_size "$roc_file")
        transfer_bytes=$((transfer_bytes + roc_bytes))
        [ "$roc_bytes" -gt "$max_transfer_bytes" ] && max_transfer_bytes=$roc_bytes
    fi

    tft_main_existing_hash=""
    tft_locale_existing_hash=""
    if [ "$layout" = "tft" ]; then
        tft_main_dest_name=$(basename "$tft_main_file")
        tft_main_existing_hash=$(remote_file_hash "files/$DATA_SUBDIR/$tft_main_dest_name")
        if [ "$tft_main_existing_hash" != "$(local_sha256 "$tft_main_file")" ]; then
            tft_main_bytes=$(local_size "$tft_main_file")
            transfer_bytes=$((transfer_bytes + tft_main_bytes))
            [ "$tft_main_bytes" -gt "$max_transfer_bytes" ] && max_transfer_bytes=$tft_main_bytes
        fi

        tft_locale_dest_name=$(basename "$tft_locale_file")
        tft_locale_existing_hash=$(remote_file_hash "files/$DATA_SUBDIR/$tft_locale_dest_name")
        if [ "$tft_locale_existing_hash" != "$(local_sha256 "$tft_locale_file")" ]; then
            tft_locale_bytes=$(local_size "$tft_locale_file")
            transfer_bytes=$((transfer_bytes + tft_locale_bytes))
            [ "$tft_locale_bytes" -gt "$max_transfer_bytes" ] && max_transfer_bytes=$tft_locale_bytes
        fi
    fi

    # Peak transient requirement: every file that will actually be
    # transferred needs its own final steady-state bytes on disk, PLUS -
    # for whichever single file is mid-transfer at any instant (files are
    # staged strictly one at a time; see stage_one_file()) - ONE extra
    # duplicate copy. That is because a host->device bounce copy under
    # $REMOTE_TMP_DIR and this script's own app-private ".stage.$$" copy
    # transiently coexist with that file's own eventual final bytes during
    # its own cp+verify window (stage_one_file()'s header comment); the
    # largest file being transferred produces the worst-case transient
    # overshoot, so it alone (not the sum of every file's overhead) is
    # added on top of the total steady-state size. Files whose remote hash
    # already matches contribute zero bytes here, so a fully idempotent
    # re-stage on an otherwise near-full device is never falsely blocked.
    require_free_space $((transfer_bytes + max_transfer_bytes))

    stage_one_file "$roc_file" "$roc_dest_name" "$roc_existing_hash"
    if [ "$layout" = "tft" ]; then
        stage_one_file "$tft_main_file" "$tft_main_dest_name" "$tft_main_existing_hash"
        stage_one_file "$tft_locale_file" "$tft_locale_dest_name" "$tft_locale_existing_hash"
    fi

    override_abs="$pkg_root/files/$DATA_SUBDIR"
    override_tmp="files/$OVERRIDE_FILENAME.stage.$$"
    override_final="files/$OVERRIDE_FILENAME"
    # bz_quest_data_validate_override() requires an absolute path with none
    # of '"', CR, LF, ';' - $override_abs is package-root-derived (never
    # user-controlled) plus the fixed "$DATA_SUBDIR" constant, so it can
    # never contain any of those; still written via the same quoted-string
    # + temp-name + rename discipline as the archives above.
    run_as "printf '%s\\n' $(sh_quote "$override_abs") > $(sh_quote "$override_tmp")"
    run_as "mv -- $(sh_quote "$override_tmp") $(sh_quote "$override_final")"
    printf '%s: wrote override file -> %s (layout: %s)\n' "$tool_name" "$override_abs" "$layout"
    printf '%s: done. Launch/restart the app for bz_quest_bridge_start() to pick up the new data directory.\n' "$tool_name"
}

cmd_verify() {
    require_device
    require_package_installed
    require_debuggable_run_as
    resolve_pkg_root

    override_final="files/$OVERRIDE_FILENAME"
    override_contents=$(run_as "cat -- $(sh_quote "$override_final") 2>/dev/null || true")
    [ -n "$override_contents" ] || die "no override file staged yet at $pkg_root/$override_final - run '$tool_name stage <dir>' first"
    printf '%s: override file points at %s\n' "$tool_name" "$override_contents"

    for name in "$ROC_ARCHIVE" "$TFT_MAIN_ARCHIVE" "$TFT_LOCALE_ARCHIVE"; do
        remote_path="files/$DATA_SUBDIR/$name"
        hash=$(run_as "test -f $(sh_quote "$remote_path") && sha256sum -- $(sh_quote "$remote_path") | awk '{print \$1}' || true")
        if [ -n "$hash" ]; then
            printf '%s: %s present (sha256 %s)\n' "$tool_name" "$name" "$hash"
        fi
    done
}

cmd_clean() {
    confirm=$1
    [ "$confirm" = "--yes" ] || die "clean requires an explicit --yes (targets $DEFAULT_PACKAGE's private '$DATA_SUBDIR' data only - never a broader/recursive delete): $tool_name clean --yes [--package PKG] [--serial SERIAL]"
    require_device
    require_package_installed
    require_debuggable_run_as
    resolve_pkg_root

    target_dir="files/$DATA_SUBDIR"
    override_final="files/$OVERRIDE_FILENAME"
    printf '%s: removing %s/%s and %s/%s from package %s on %s\n' \
        "$tool_name" "$pkg_root" "$target_dir" "$pkg_root" "$override_final" "$package" "${serial:-the attached device}"
    # Scoped to exactly the two paths this script itself ever writes - never
    # a wildcard/recursive delete of the whole app data directory.
    run_as "rm -rf -- $(sh_quote "$target_dir")"
    run_as "rm -f -- $(sh_quote "$override_final")"
    printf '%s: cleaned\n' "$tool_name"
}

cmd_run() {
    require_device
    require_package_installed
    adb_shell "am start -n $(sh_quote "$package/$ACTIVITY")" >/dev/null
    printf '%s: launched %s/%s\n' "$tool_name" "$package" "$ACTIVITY"
}

cmd_log() {
    require_device
    adb_ logcat -s "$LOG_TAG:V"
}

# Resolves and prints the single target device serial (auto-selected if
# exactly one device is attached, otherwise requiring --serial), applying
# the EXACT same no-device/multiple-without-serial/unknown/offline/
# unauthorized rejection rules require_device() already enforces for
# stage/verify/clean/run/log above. Exists so a caller like
# scripts/acceptance-runner.sh can resolve+validate the target device
# through this ONE already-tested implementation instead of duplicating
# require_device()'s safety-critical logic a second time - see
# docs/quest-tabletop.md's acceptance-automation section.
cmd_resolve_device() {
    require_device
    printf '%s\n' "$serial"
}

# Validates that the target device is attached, the package is installed
# and debuggable (run-as capable), and prints the resolved app-private data
# root - the exact same three checks cmd_stage()/cmd_verify()/cmd_clean()
# already run before touching any file, exposed standalone so a caller like
# scripts/acceptance-runner.sh can validate "package/debuggable/run-as"
# through this ONE already-tested implementation before installing/staging/
# launching, instead of re-implementing pm path/run-as parsing a second
# time - see docs/quest-tabletop.md's acceptance-automation section.
cmd_check_runtime() {
    require_device
    require_package_installed
    require_debuggable_run_as
    resolve_pkg_root
    printf 'serial=%s\n' "$serial"
    printf 'pkg_root=%s\n' "$pkg_root"
}

# --- argument parsing --------------------------------------------------------

[ $# -ge 1 ] || { usage; exit 2; }
command=$1
shift

src_dir=""
clean_confirm=""
case "$command" in
    stage)
        [ $# -ge 1 ] || { usage; exit 2; }
        src_dir=$1
        shift
        ;;
    clean)
        [ $# -ge 1 ] || { usage; exit 2; }
        clean_confirm=$1
        shift
        ;;
esac

while [ $# -gt 0 ]; do
    case "$1" in
        --package) [ $# -ge 2 ] || die "--package requires a value"; package=$2; shift 2 ;;
        --serial) [ $# -ge 2 ] || die "--serial requires a value"; serial=$2; shift 2 ;;
        *) usage; exit 2 ;;
    esac
done

case "$command" in
    stage) cmd_stage "$src_dir" ;;
    verify) cmd_verify ;;
    clean) cmd_clean "$clean_confirm" ;;
    run) cmd_run ;;
    log) cmd_log ;;
    resolve-device) cmd_resolve_device ;;
    check-runtime) cmd_check_runtime ;;
    *) usage; exit 2 ;;
esac
