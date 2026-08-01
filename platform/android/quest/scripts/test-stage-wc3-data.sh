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
INSTALLED_PACKAGE=$installed_package
DEBUGGABLE=$debuggable
FREE_KB=$free_kb
EOF
    # Every test starts from a pristine device: no leftover app files or
    # bounce-tmp files from a prior test's fixtures can leak in and produce a
    # false pass/fail (each test still stages/re-stages as many times as it
    # likes *within* its own body after this reset point).
    rm -rf "$DEVICE_ROOT/pkgs/$PKG" "$BZ_QUEST_STAGE_REMOTE_TMP"
    mkdir -p "$BZ_QUEST_STAGE_REMOTE_TMP"
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
export FAKE_DEVICE_ROOT="$DEVICE_ROOT"

serial_arg=""
if [ "\${1:-}" = "-s" ]; then serial_arg=\$2; shift 2; fi

cmd=\$1
shift

case "\$cmd" in
    devices)
        echo "List of devices attached"
        [ "\$PRESENT" = "1" ] && echo "FAKESERIAL	device"
        ;;
    push)
        if [ "\$PRESENT" != "1" ]; then echo "error: no devices/emulators found" >&2; exit 1; fi
        [ "\$1" = "--" ] && shift
        local=\$1
        remote=\$2
        mkdir -p "\$(dirname "\$remote")"
        cp -- "\$local" "\$remote"
        ;;
    shell)
        if [ "\$PRESENT" != "1" ]; then echo "error: no devices/emulators found" >&2; exit 1; fi
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

pkg_root_dir() { printf '%s/pkgs/%s' "$DEVICE_ROOT" "$PKG"; }

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
test_safe_cleanup

printf '%s: %d/%d tests passed\n' "$tool_name" "$tests_run" "$tests_run"
