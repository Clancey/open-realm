#!/bin/sh
# platform/android/quest/scripts/test-stage-wc3-data.sh - exercises
# stage-wc3-data.sh against a fake adb/run-as/pm/df/sha256sum device, so this
# script's ROC/TFT validation, quoting, atomic-replace, and failure-detection
# behavior can be verified with no physical Quest hardware attached (see
# docs/quest-tabletop.md's "Layer 7" section). The fake device is just a
# plain host directory tree; fake `run-as`/`pm`/`df` shims (written below)
# stand in for the real Android toybox binaries the production script talks
# to via `adb shell`.
set -eu

tool_name=${0##*/}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STAGE_TOOL="$SCRIPT_DIR/stage-wc3-data.sh"

scratch=$(mktemp -d "${TMPDIR:-/tmp}/openrealm-quest-stage.XXXXXX")
scratch=$(CDPATH= cd -- "$scratch" && pwd)
trap 'rm -rf "$scratch"' 0 1 2 15

FAKE_BIN="$scratch/fakebin"
DEVICE_ROOT="$scratch/device"
STATE_FILE="$scratch/state.sh"
mkdir -p "$FAKE_BIN" "$DEVICE_ROOT"

PKG="org.openrealm.quest.test"
tests_run=0

fail() {
    printf '%s: FAIL: %s\n' "$tool_name" "$*" >&2
    exit 1
}

pass() {
    tests_run=$((tests_run + 1))
    printf '  %-55s PASS\n' "$1"
}

# Resolve a real host hash tool once (outside FAKE_BIN's PATH shadowing) so
# the fake sha256sum shim below can compute real hashes for the common case
# and only fabricate a wrong one when a test explicitly asks for corruption.
if command -v sha256sum >/dev/null 2>&1; then
    HOST_HASH_BIN=$(command -v sha256sum)
    HOST_HASH_ARGS=""
elif command -v shasum >/dev/null 2>&1; then
    HOST_HASH_BIN=$(command -v shasum)
    HOST_HASH_ARGS="-a 256"
else
    fail "no sha256sum/shasum on this host - cannot build the fake device's hash shim"
fi

write_state() {
    present=$1 installed_package=$2 debuggable=$3 free_kb=$4
    cat > "$STATE_FILE" <<EOF
PRESENT=$present
DEVICES=
INSTALLED_PACKAGE=$installed_package
DEBUGGABLE=$debuggable
FREE_KB=$free_kb
EOF
    # Every test starts from a pristine device: no leftover app files or
    # bounce-tmp files from a prior test's fixtures can leak in and produce a
    # false pass/fail (each test still stages/re-stages as many times as it
    # likes *within* its own body after this reset point).
    rm -rf "$DEVICE_ROOT" "$BZ_QUEST_STAGE_REMOTE_TMP"
    : > "$scratch/adb_invocations.log"
    mkdir -p "$DEVICE_ROOT" "$BZ_QUEST_STAGE_REMOTE_TMP"
}

# Like write_state, but attaches N>=1 fake devices at once instead of the
# single implicit "FAKESERIAL" - for multi-device rejection, --serial
# routing, and unknown/offline-serial tests. $1 is a space-separated list of
# "serial:state" pairs (e.g. "DEV1:device DEV2:device" or
# "DEV1:device DEV2:offline"); the fake adb shim below routes every push/
# shell call to a *per-serial* device root (see FAKE_DEVICE_ROOT below), so
# --serial's effect (and cross-device isolation) is genuinely observable
# rather than silently accepted-and-ignored.
write_devices_state() {
    devices=$1 installed_package=$2 debuggable=$3 free_kb=$4
    cat > "$STATE_FILE" <<EOF
PRESENT=1
DEVICES="$devices"
INSTALLED_PACKAGE=$installed_package
DEBUGGABLE=$debuggable
FREE_KB=$free_kb
EOF
    rm -rf "$DEVICE_ROOT" "$BZ_QUEST_STAGE_REMOTE_TMP"
    : > "$scratch/adb_invocations.log"
    mkdir -p "$DEVICE_ROOT" "$BZ_QUEST_STAGE_REMOTE_TMP"
}

# Rewrites only FREE_KB in the already-written state file, leaving every
# previously staged file on the (fake) device untouched. Used by tests that
# simulate a device filling up *between* two stage runs against the same
# package root - unlike write_state/write_devices_state, this must NOT wipe
# $DEVICE_ROOT, or it would trivially "prove" file preservation by never
# having written anything to preserve in the first place.
set_free_kb() {
    sed -i.bak "s/^FREE_KB=.*/FREE_KB=$1/" "$STATE_FILE"
    rm -f "$STATE_FILE.bak"
}


# pkg_root_dir() below assumes the single-device flat root; this variant
# resolves a specific serial's own per-device package root for multi-device
# tests.
pkg_root_dir_for_serial() {
    printf '%s/dev-%s/pkgs/%s' "$DEVICE_ROOT" "$1" "$PKG"
}

# --- fake device shims -------------------------------------------------------

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
if [ "${1:-}" = "path" ] && [ "${2:-}" = "${INSTALLED_PACKAGE:-}" ]; then
    echo "package:/data/app/fake-openrealm-quest/base.apk"
    exit 0
fi
exit 1
SHIM

cat > "$FAKE_BIN/df" <<'SHIM'
#!/bin/sh
# Only "-Pk ." is ever used by stage-wc3-data.sh; always reports FREE_KB.
echo "Filesystem 1024-blocks Used Available Capacity Mounted on"
echo "fake 99999999 1 ${FREE_KB:-10000000} 1% /fake"
SHIM

cat > "$FAKE_BIN/sha256sum" <<SHIM
#!/bin/sh
# Simulates on-device corruption: returns a deliberately wrong hash for any
# ".stage." temp file when BZ_TEST_CORRUPT=1 is exported by the test - never
# for a final (already-renamed) destination path, matching only the transfer-
# verification step this test targets.
if [ "\${BZ_TEST_CORRUPT:-0}" = "1" ]; then
    for a in "\$@"; do
        case "\$a" in
            *.stage.*) echo "0000000000000000000000000000000000000000000000000000000000000000  \$a"; exit 0 ;;
        esac
    done
fi
exec "$HOST_HASH_BIN" $HOST_HASH_ARGS "\$@"
SHIM

cat > "$FAKE_BIN/cp" <<'SHIM'
#!/bin/sh
# Simulates an interrupted device-side transfer: fails once when
# BZ_TEST_INTERRUPT=1 is exported by the test, leaving no destination file
# behind (real `cp` never partially wrote one either, since it fails before
# exec'ing here).
if [ "${BZ_TEST_INTERRUPT:-0}" = "1" ]; then
    echo "fake cp: simulated interrupted transfer" >&2
    exit 1
fi
exec /bin/cp "$@"
SHIM

chmod +x "$FAKE_BIN"/run-as "$FAKE_BIN"/pm "$FAKE_BIN"/df "$FAKE_BIN"/sha256sum "$FAKE_BIN"/cp

cat > "$FAKE_BIN/adb" <<SHIM
#!/bin/sh
set -eu
. "$STATE_FILE"
export INSTALLED_PACKAGE DEBUGGABLE FREE_KB
DEVICES=\${DEVICES:-}

serial_arg=""
if [ "\${1:-}" = "-s" ]; then serial_arg=\$2; shift 2; fi

cmd=\$1
shift

# Records every invocation's command + selected serial, so tests can assert
# that --serial (or its absence) is carried *consistently* across every adb
# call the script makes for a given run - not just the first one.
printf '%s serial=%s\n' "\$cmd" "\${serial_arg:-NONE}" >> "$scratch/adb_invocations.log"

# Multi-device fakes (DEVICES set via write_devices_state) route push/shell
# to a *per-serial* device root, so --serial's effect is genuinely
# observable rather than silently accepted-and-ignored; a real adb with no
# --serial and exactly one attached device implicitly targets that device,
# so the single-device case mirrors that default too.
if [ -n "\$DEVICES" ]; then
    effective_serial=\$serial_arg
    if [ -z "\$effective_serial" ]; then
        device_count=\$(set -- \$DEVICES; echo \$#)
        [ "\$device_count" = "1" ] && effective_serial=\${DEVICES%%:*}
    fi
    export FAKE_DEVICE_ROOT="$DEVICE_ROOT/dev-\${effective_serial:-none}"
else
    export FAKE_DEVICE_ROOT="$DEVICE_ROOT/dev-single"
fi

case "\$cmd" in
    devices)
        echo "List of devices attached"
        if [ -n "\$DEVICES" ]; then
            for pair in \$DEVICES; do
                printf '%s\t%s\n' "\${pair%%:*}" "\${pair##*:}"
            done
        elif [ "\$PRESENT" = "1" ]; then
            echo "FAKESERIAL	device"
        fi
        ;;
    push)
        if [ -z "\$DEVICES" ] && [ "\$PRESENT" != "1" ]; then echo "error: no devices/emulators found" >&2; exit 1; fi
        [ "\$1" = "--" ] && shift
        local=\$1
        remote=\$2
        mkdir -p "\$(dirname "\$remote")"
        cp -- "\$local" "\$remote"
        ;;
    shell)
        if [ -z "\$DEVICES" ] && [ "\$PRESENT" != "1" ]; then echo "error: no devices/emulators found" >&2; exit 1; fi
        PATH="$FAKE_BIN:\$PATH" sh -c "\$1"
        ;;
    logcat)
        :
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

mkdir -p "$BZ_QUEST_STAGE_REMOTE_TMP"

# --- fixture helpers ---------------------------------------------------------

fixture_dir=""
make_roc_fixture() {
    fixture_dir=$1
    mkdir -p "$fixture_dir"
    printf 'synthetic ROC archive bytes\n' > "$fixture_dir/War3.mpq"
}

make_tft_fixture() {
    fixture_dir=$1
    make_roc_fixture "$fixture_dir"
    printf 'synthetic TFT main archive bytes\n' > "$fixture_dir/War3x.mpq"
    printf 'synthetic TFT locale archive bytes\n' > "$fixture_dir/War3xLocal.mpq"
}

pkg_root_dir() { printf '%s/dev-single/pkgs/%s' "$DEVICE_ROOT" "$PKG"; }

# =============================================================================
# test_roc_only
# =============================================================================
test_roc_only() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/roc-src
    make_roc_fixture "$src"

    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>&1 ||
        fail "ROC-only stage failed: $(cat "$scratch/out")"

    root=$(pkg_root_dir)
    [ -f "$root/files/Warcraft III/War3.mpq" ] || fail "War3.mpq not staged"
    [ ! -e "$root/files/Warcraft III/War3x.mpq" ] || fail "unexpected TFT file staged for ROC-only"
    override=$(cat "$root/files/warcraft_data_path_override.txt")
    [ "$override" = "$root/files/Warcraft III" ] || fail "override file contents wrong: $override"
    cmp -s "$src/War3.mpq" "$root/files/Warcraft III/War3.mpq" || fail "staged bytes differ from source"
    pass "ROC-only layout stages and writes the override file"
}

# =============================================================================
# test_tft_over_roc
# =============================================================================
test_tft_over_roc() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/tft-src
    make_tft_fixture "$src"

    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>&1 ||
        fail "TFT stage failed: $(cat "$scratch/out")"

    root=$(pkg_root_dir)
    for f in War3.mpq War3x.mpq War3xLocal.mpq; do
        cmp -s "$src/$f" "$root/files/Warcraft III/$f" || fail "$f bytes differ from source or missing"
    done
    pass "TFT-over-ROC layout stages all three archives"
}

# =============================================================================
# test_incomplete_tft_rejected
# =============================================================================
test_incomplete_tft_rejected() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/incomplete-src
    make_roc_fixture "$src"
    printf 'orphan tft main\n' > "$src/War3x.mpq"
    # War3xLocal.mpq deliberately absent.

    if "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err"; then
        fail "incomplete TFT layout should have been rejected"
    fi
    grep -q "incomplete TFT layout" "$scratch/err" || fail "missing actionable incomplete-layout error: $(cat "$scratch/err")"
    pass "incomplete mixed ROC/TFT layout is rejected with an actionable error"
}

# =============================================================================
# test_missing_roc_rejected
# =============================================================================
test_missing_roc_rejected() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/no-roc-src
    mkdir -p "$src"
    printf 'tft main only\n' > "$src/War3x.mpq"
    printf 'tft locale only\n' > "$src/War3xLocal.mpq"

    if "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err"; then
        fail "TFT-without-ROC layout should have been rejected"
    fi
    grep -q "missing required ROC archive" "$scratch/err" || fail "missing actionable ROC-required error: $(cat "$scratch/err")"
    pass "TFT files without ROC's War3.mpq are rejected with an actionable error"
}

# =============================================================================
# test_spaces_and_case_preserved
# =============================================================================
test_spaces_and_case_preserved() {
    write_state 1 "$PKG" 1 10000000
    src="$scratch/source with spaces/My WC3 Data"
    mkdir -p "$src"
    printf 'roc bytes\n' > "$src/War3.mpq"
    printf 'tft main bytes\n' > "$src/war3x.mpq"       # deliberately lowercase
    printf 'tft locale bytes\n' > "$src/WAR3XLOCAL.MPQ" # deliberately upper-case

    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>&1 ||
        fail "stage with spaces/mixed case failed: $(cat "$scratch/out")"

    root=$(pkg_root_dir)
    [ -f "$root/files/Warcraft III/War3.mpq" ] || fail "War3.mpq missing"
    [ -f "$root/files/Warcraft III/war3x.mpq" ] || fail "war3x.mpq (exact source case) missing - case must be preserved"
    [ -f "$root/files/Warcraft III/WAR3XLOCAL.MPQ" ] || fail "WAR3XLOCAL.MPQ (exact source case) missing - case must be preserved"
    pass "source paths with spaces and mixed-case archive filenames are preserved exactly"
}

# =============================================================================
# test_unchanged_skip
# =============================================================================
test_unchanged_skip() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/unchanged-src
    make_roc_fixture "$src"
    "$STAGE_TOOL" stage "$src" --package "$PKG" >/dev/null 2>&1 || fail "initial stage failed"

    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>&1 ||
        fail "re-stage of unchanged source failed: $(cat "$scratch/out")"
    grep -q "unchanged" "$scratch/out" || fail "expected an 'unchanged...skipping' line: $(cat "$scratch/out")"
    pass "an unchanged, already-verified file is skipped on re-stage"
}

# =============================================================================
# test_changed_atomic_replace
# =============================================================================
test_changed_atomic_replace() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/changed-src
    make_roc_fixture "$src"
    "$STAGE_TOOL" stage "$src" --package "$PKG" >/dev/null 2>&1 || fail "initial stage failed"

    printf 'a brand new, different ROC payload\n' > "$src/War3.mpq"
    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>&1 ||
        fail "re-stage of changed source failed: $(cat "$scratch/out")"

    root=$(pkg_root_dir)
    cmp -s "$src/War3.mpq" "$root/files/Warcraft III/War3.mpq" || fail "changed file was not replaced"
    find "$root/files/Warcraft III" -name '*.stage.*' | grep -q . && fail "leftover .stage.* temp file after successful replace"
    pass "a changed file is atomically replaced (temp name + rename, no leftovers)"
}

# =============================================================================
# test_corruption_detected
# =============================================================================
test_corruption_detected() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/corrupt-src
    make_roc_fixture "$src"
    "$STAGE_TOOL" stage "$src" --package "$PKG" >/dev/null 2>&1 || fail "baseline stage failed"
    root=$(pkg_root_dir)
    baseline_hash=$("$HOST_HASH_BIN" $HOST_HASH_ARGS "$root/files/Warcraft III/War3.mpq" | awk '{print $1}')

    printf 'a second, different payload that will fail verification\n' > "$src/War3.mpq"
    BZ_TEST_CORRUPT=1
    export BZ_TEST_CORRUPT
    ok=1
    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err" || ok=0
    unset BZ_TEST_CORRUPT
    [ "$ok" -eq 0 ] || fail "corrupted transfer should have failed"
    grep -q "corruption detected" "$scratch/err" || fail "missing actionable corruption error: $(cat "$scratch/err")"
    find "$root/files/Warcraft III" -name '*.stage.*' | grep -q . && fail "corrupted temp file was not cleaned up"
    after_hash=$("$HOST_HASH_BIN" $HOST_HASH_ARGS "$root/files/Warcraft III/War3.mpq" | awk '{print $1}')
    [ "$after_hash" = "$baseline_hash" ] || fail "the previously-valid file was NOT left intact after a corrupted transfer"
    pass "device-side corruption is detected, the temp file is removed, and the prior valid file is left intact"
}

# =============================================================================
# test_interruption_leaves_prior_file
# =============================================================================
test_interruption_leaves_prior_file() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/interrupt-src
    make_roc_fixture "$src"
    "$STAGE_TOOL" stage "$src" --package "$PKG" >/dev/null 2>&1 || fail "baseline stage failed"
    root=$(pkg_root_dir)
    baseline_hash=$("$HOST_HASH_BIN" $HOST_HASH_ARGS "$root/files/Warcraft III/War3.mpq" | awk '{print $1}')

    printf 'this transfer will be interrupted mid-copy\n' > "$src/War3.mpq"
    BZ_TEST_INTERRUPT=1
    export BZ_TEST_INTERRUPT
    ok=1
    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err" || ok=0
    unset BZ_TEST_INTERRUPT
    [ "$ok" -eq 0 ] || fail "interrupted transfer should have failed"
    after_hash=$("$HOST_HASH_BIN" $HOST_HASH_ARGS "$root/files/Warcraft III/War3.mpq" | awk '{print $1}')
    [ "$after_hash" = "$baseline_hash" ] || fail "the previously-valid file was NOT left intact after an interrupted transfer"

    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out2" 2>"$scratch/err2" ||
        fail "retry after interruption should succeed: $(cat "$scratch/err2")"
    cmp -s "$src/War3.mpq" "$root/files/Warcraft III/War3.mpq" || fail "retry after interruption did not converge"
    pass "an interrupted transfer leaves the prior valid file intact, and a retry converges"
}

# =============================================================================
# test_no_device
# =============================================================================
test_no_device() {
    write_state 0 "$PKG" 1 10000000
    src=$scratch/no-device-src
    make_roc_fixture "$src"
    if "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err"; then
        fail "staging with no device attached should fail"
    fi
    grep -qi "no adb device" "$scratch/err" || fail "missing actionable no-device error: $(cat "$scratch/err")"
    pass "no attached device is detected explicitly"
}

# =============================================================================
# test_wrong_package
# =============================================================================
test_wrong_package() {
    write_state 1 "org.example.other" 1 10000000
    src=$scratch/wrong-pkg-src
    make_roc_fixture "$src"
    if "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err"; then
        fail "staging against an uninstalled package should fail"
    fi
    grep -q "is not installed" "$scratch/err" || fail "missing actionable wrong-package error: $(cat "$scratch/err")"
    pass "a package that is not installed is detected explicitly"
}

# =============================================================================
# test_non_debuggable
# =============================================================================
test_non_debuggable() {
    write_state 1 "$PKG" 0 10000000
    src=$scratch/non-debuggable-src
    make_roc_fixture "$src"
    if "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err"; then
        fail "staging against a non-debuggable app should fail"
    fi
    grep -q "debuggable" "$scratch/err" || fail "missing actionable non-debuggable error: $(cat "$scratch/err")"
    pass "a non-debuggable app (run-as unavailable) is detected explicitly"
}

# =============================================================================
# test_low_space
# =============================================================================
test_low_space() {
    write_state 1 "$PKG" 1 0
    src=$scratch/low-space-src
    make_roc_fixture "$src"
    if "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err"; then
        fail "staging with insufficient device space should fail"
    fi
    grep -q "insufficient space" "$scratch/err" || fail "missing actionable low-space error: $(cat "$scratch/err")"
    root=$(pkg_root_dir)
    [ -e "$root/files/Warcraft III/War3.mpq" ] && fail "no bytes should have been transferred when space check fails first"
    pass "insufficient device free space is detected before any transfer"
}

# =============================================================================
# test_near_full_device_blocks_peak_not_just_final_size
# =============================================================================
# The free-space preflight must account for the *transient* bounce
# (/data/local/tmp push) + app-private ".stage.$$" copy that briefly
# coexist with a changed file's eventual final bytes (see stage-wc3-data.sh's
# cmd_stage() peak-space comment) - not just the sum of final archive sizes.
# Picks a free-space figure that a final-size-only estimate would have
# accepted (1x the changed file's size) but the real transient peak (~2x)
# correctly rejects, and confirms the previously-staged valid file survives
# untouched.
test_near_full_device_blocks_peak_not_just_final_size() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/near-full-src
    make_roc_fixture "$src"
    "$STAGE_TOOL" stage "$src" --package "$PKG" >/dev/null 2>&1 || fail "baseline stage failed"
    root=$(pkg_root_dir)
    baseline_hash=$("$HOST_HASH_BIN" $HOST_HASH_ARGS "$root/files/Warcraft III/War3.mpq" | awk '{print $1}')

    # Exactly 2,000,000 bytes so the arithmetic below is exact.
    dd if=/dev/zero bs=1000 count=2000 2>/dev/null | tr '\0' 'x' > "$src/War3.mpq"
    file_bytes=$(wc -c < "$src/War3.mpq" | tr -d ' ')
    [ "$file_bytes" -eq 2000000 ] || fail "test fixture size assumption broke: got $file_bytes bytes"

    # avail_bytes ~= 2,999,296: > the changed file's own 1x size (a
    # final-size-only estimate would wrongly allow this) but < the correct
    # ~2x transient peak (1x steady-state + 1x bounce/stage duplicate).
    # set_free_kb (not write_state) so the baseline file staged above stays
    # in place - this test needs something to preserve.
    set_free_kb 2929
    ok=1
    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err" || ok=0
    [ "$ok" -eq 0 ] || fail "staging a changed file with only ~1.5x its size free should have been rejected"
    grep -q "insufficient space" "$scratch/err" || fail "missing actionable low-space error: $(cat "$scratch/err")"

    after_hash=$("$HOST_HASH_BIN" $HOST_HASH_ARGS "$root/files/Warcraft III/War3.mpq" | awk '{print $1}')
    [ "$after_hash" = "$baseline_hash" ] || fail "the previously-valid file was NOT preserved when the peak-space preflight rejected the transfer"
    find "$root/files/Warcraft III" -name '*.stage.*' | grep -q . && fail "no .stage.* temp file should exist - the preflight must reject before any transfer starts"
    pass "a near-full device is rejected using the transient bounce+stage peak (not just final size), preserving the prior file"
}

# =============================================================================
# test_idempotent_restage_not_blocked_by_near_full_space
# =============================================================================
# An already-fully-staged, byte-identical re-run needs zero new bytes (every
# file is skipped via the unchanged-hash fast path), so it must succeed even
# when the device reports almost no free space at all - the peak estimate
# must be computed from files that actually need transfer, not the full
# dataset size.
test_idempotent_restage_not_blocked_by_near_full_space() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/idempotent-src
    make_tft_fixture "$src"
    "$STAGE_TOOL" stage "$src" --package "$PKG" >/dev/null 2>&1 || fail "baseline stage failed"

    set_free_kb 1 # ~1 KB free - far less than any archive's own size; device root is preserved
    "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err" ||
        fail "an idempotent re-stage must not be blocked by a near-full device: $(cat "$scratch/err")"
    grep -q "unchanged" "$scratch/out" || fail "expected every file to take the unchanged/skip path: $(cat "$scratch/out")"
    pass "an idempotent (already fully staged) re-stage is never blocked by a peak estimate sized for a fresh transfer"
}

# =============================================================================
# test_multiple_devices_without_serial_rejected
# =============================================================================
test_multiple_devices_without_serial_rejected() {
    write_devices_state "DEV1:device DEV2:device" "$PKG" 1 10000000
    src=$scratch/multi-device-src
    make_roc_fixture "$src"
    if "$STAGE_TOOL" stage "$src" --package "$PKG" >"$scratch/out" 2>"$scratch/err"; then
        fail "staging with >1 attached device and no --serial should fail"
    fi
    grep -q "more than one adb device" "$scratch/err" || fail "missing actionable multi-device error: $(cat "$scratch/err")"
    grep -q -- "--serial" "$scratch/err" || fail "multi-device error should mention --serial: $(cat "$scratch/err")"
    pass "more than one attached device without --serial is rejected explicitly"
}

# =============================================================================
# test_serial_routes_to_correct_device
# =============================================================================
# With two devices attached, --serial DEV1 must stage only into DEV1's own
# package root (DEV2 must be untouched), and *every* adb invocation for this
# run must consistently carry "-s DEV1" - never a bare unsuffixed call and
# never DEV2's serial.
test_serial_routes_to_correct_device() {
    write_devices_state "DEV1:device DEV2:device" "$PKG" 1 10000000
    src=$scratch/serial-route-src
    make_roc_fixture "$src"

    "$STAGE_TOOL" stage "$src" --package "$PKG" --serial DEV1 >"$scratch/out" 2>"$scratch/err" ||
        fail "staging with an explicit valid --serial should succeed: $(cat "$scratch/err")"

    dev1_root=$(pkg_root_dir_for_serial DEV1)
    dev2_root=$(pkg_root_dir_for_serial DEV2)
    [ -f "$dev1_root/files/Warcraft III/War3.mpq" ] || fail "--serial DEV1 did not stage into DEV1's own package root"
    [ ! -e "$dev2_root/files/Warcraft III/War3.mpq" ] || fail "--serial DEV1 must not have touched DEV2's package root"

    # grep -v so a single non-matching line fails the check, not just "at
    # least one match" - every logged invocation must carry -s DEV1.
    if grep -v 'serial=DEV1$' "$scratch/adb_invocations.log" >"$scratch/bad_invocations"; then
        [ -s "$scratch/bad_invocations" ] && fail "not every adb invocation carried --serial DEV1 consistently: $(cat "$scratch/bad_invocations")"
    fi
    pass "--serial routes every adb invocation to the correct device consistently"
}

# =============================================================================
# test_unknown_serial_rejected
# =============================================================================
test_unknown_serial_rejected() {
    write_devices_state "DEV1:device" "$PKG" 1 10000000
    src=$scratch/unknown-serial-src
    make_roc_fixture "$src"
    if "$STAGE_TOOL" stage "$src" --package "$PKG" --serial NOPE >"$scratch/out" 2>"$scratch/err"; then
        fail "an unknown/offline --serial should be rejected"
    fi
    grep -q "no attached device matches --serial" "$scratch/err" || fail "missing actionable unknown-serial error: $(cat "$scratch/err")"
    pass "an unknown/unattached --serial is rejected explicitly"
}

# =============================================================================
# test_offline_serial_rejected
# =============================================================================
test_offline_serial_rejected() {
    write_devices_state "DEV1:device DEV2:offline" "$PKG" 1 10000000
    src=$scratch/offline-serial-src
    make_roc_fixture "$src"
    if "$STAGE_TOOL" stage "$src" --package "$PKG" --serial DEV2 >"$scratch/out" 2>"$scratch/err"; then
        fail "an offline --serial should be rejected"
    fi
    grep -q "offline" "$scratch/err" || fail "missing actionable offline-serial error: $(cat "$scratch/err")"
    pass "a matched but offline --serial device is rejected explicitly"
}

# =============================================================================
# test_safe_cleanup
# =============================================================================
test_safe_cleanup() {
    write_state 1 "$PKG" 1 10000000
    src=$scratch/cleanup-src
    make_tft_fixture "$src"
    "$STAGE_TOOL" stage "$src" --package "$PKG" >/dev/null 2>&1 || fail "baseline stage for cleanup test failed"
    root=$(pkg_root_dir)

    if "$STAGE_TOOL" clean --package "$PKG" >"$scratch/out" 2>"$scratch/err"; then
        fail "clean without --yes should refuse to run"
    fi
    grep -q -- "--yes" "$scratch/err" || fail "missing actionable confirmation-required error: $(cat "$scratch/err")"
    [ -e "$root/files/Warcraft III/War3.mpq" ] || fail "clean without --yes must not have deleted anything"

    "$STAGE_TOOL" clean --yes --package "$PKG" >"$scratch/out2" 2>&1 ||
        fail "confirmed clean failed: $(cat "$scratch/out2")"
    [ ! -e "$root/files/Warcraft III" ] || fail "clean --yes did not remove the staged data directory"
    [ ! -e "$root/files/warcraft_data_path_override.txt" ] || fail "clean --yes did not remove the override file"
    pass "clean refuses without --yes, and --yes safely removes only the resolved app-data subdirectory"
}

test_roc_only
test_tft_over_roc
test_incomplete_tft_rejected
test_missing_roc_rejected
test_spaces_and_case_preserved
test_unchanged_skip
test_changed_atomic_replace
test_corruption_detected
test_interruption_leaves_prior_file
test_no_device
test_wrong_package
test_non_debuggable
test_low_space
test_near_full_device_blocks_peak_not_just_final_size
test_idempotent_restage_not_blocked_by_near_full_space
test_multiple_devices_without_serial_rejected
test_serial_routes_to_correct_device
test_unknown_serial_rejected
test_offline_serial_rejected
test_safe_cleanup

printf '%s: %d/%d tests passed\n' "$tool_name" "$tests_run" "$tests_run"
