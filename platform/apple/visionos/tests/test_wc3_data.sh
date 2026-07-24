#!/bin/sh
set -eu

test_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
tool=$test_dir/../scripts/wc3_data.sh
tool_name=${tool##*/}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/openrealm-wc3-data.XXXXXX")
tests_run=0
trap 'rm -rf "$scratch"' 0 1 2 15

fail() {
    printf 'test_wc3_data: FAIL: %s\n' "$*" >&2
    exit 1
}

visit_test_mpqs() {
    test_callback=$1
    "$test_callback" War3.mpq
    "$test_callback" War3x.mpq
    "$test_callback" War3xLocal.mpq
}

write_fixture_mpq() {
    printf 'synthetic fixture: %s\n' "$1" > "$fixture_dir/$1"
}

make_fixture() {
    fixture_dir=$1
    mkdir -p "$fixture_dir"
    visit_test_mpqs write_fixture_mpq
}

assert_same() {
    cmp -s "$1" "$2" || fail "$1 and $2 differ"
}

assert_absent() {
    if [ -e "$1" ] || [ -L "$1" ]; then fail "unexpected path: $1"; fi
}

assert_line() {
    grep -F -x "$2" "$1" >/dev/null || fail "missing diagnostic: $2"
}

assert_override_mpq() {
    assert_same "$override_source/$1" "$override_dest/Resources/Warcraft III/$1"
    assert_absent "$override_dest/$1"
}

test_source_override_and_layout() {
    override_source=$scratch/override/source\ with\ spaces
    override_dest=$scratch/override/App\ Stage
    make_fixture "$override_source"
    mkdir -p "$scratch/override/unused-home"
    HOME="$scratch/override/unused-home" BZ_WC3_DATA_DIR="$override_source" \
        "$tool" stage "$override_dest" >/dev/null
    visit_test_mpqs assert_override_mpq
}

assert_default_mpq() {
    assert_same "$default_source/$1" "$default_dest/Resources/Warcraft III/$1"
}

test_default_source() {
    default_home=$scratch/default/home\ with\ spaces
    default_source=$default_home/Downloads/Warcraft\ III
    default_dest=$scratch/default/Bundle\ Stage
    make_fixture "$default_source"
    (unset BZ_WC3_DATA_DIR; HOME="$default_home" "$tool" stage "$default_dest") >/dev/null
    visit_test_mpqs assert_default_mpq
}

test_missing_empty_and_failed_stage_cleanup() {
    invalid_source=$scratch/invalid/source
    invalid_dest=$scratch/invalid/destination
    invalid_bundle=$invalid_dest/Resources/Warcraft\ III
    invalid_err=$scratch/invalid/stderr
    mkdir -p "$invalid_source" "$invalid_bundle"
    printf 'base\n' > "$invalid_source/War3.mpq"
    : > "$invalid_source/War3x.mpq"
    printf 'keep\n' > "$invalid_bundle/sentinel"
    if BZ_WC3_DATA_DIR="$invalid_source" "$tool" stage "$invalid_dest" \
        >"$scratch/invalid/stdout" 2>"$invalid_err"; then
        fail "stage accepted missing or empty source files"
    fi
    assert_line "$invalid_err" "$tool_name: required file is empty: $invalid_source/War3x.mpq"
    assert_line "$invalid_err" "$tool_name: missing required file: $invalid_source/War3xLocal.mpq"
    [ -f "$invalid_bundle/sentinel" ] || fail "failed stage replaced the existing bundle"
    invalid_temps=$(find "$invalid_dest/Resources" -mindepth 1 -maxdepth 1 \
        \( -name '.Warcraft III.stage.*' -o -name '.Warcraft III.previous.*' \) -print)
    [ -z "$invalid_temps" ] || fail "failed stage left temporary paths: $invalid_temps"
}

assert_repeat_mpq() {
    assert_same "$repeat_source/$1" "$repeat_bundle/$1"
}

test_cleanup_and_idempotence() {
    repeat_source=$scratch/repeat/source
    repeat_dest=$scratch/repeat/destination
    repeat_bundle=$repeat_dest/Resources/Warcraft\ III
    make_fixture "$repeat_source"
    BZ_WC3_DATA_DIR="$repeat_source" "$tool" stage "$repeat_dest" >/dev/null
    mkdir -p "$repeat_bundle/stale-directory"
    printf 'stale\n' > "$repeat_bundle/stale.iso"
    printf 'updated\n' > "$repeat_source/War3x.mpq"
    BZ_WC3_DATA_DIR="$repeat_source" "$tool" stage "$repeat_dest" >/dev/null
    BZ_WC3_DATA_DIR="$repeat_source" "$tool" stage "$repeat_dest" >/dev/null
    assert_absent "$repeat_bundle/stale-directory"
    assert_absent "$repeat_bundle/stale.iso"
    visit_test_mpqs assert_repeat_mpq
    repeat_count=$(find "$repeat_bundle" -mindepth 1 -maxdepth 1 -print | wc -l | tr -d '[:space:]')
    [ "$repeat_count" = 3 ] || fail "idempotent stage produced $repeat_count entries"
    repeat_temps=$(find "$repeat_dest/Resources" -mindepth 1 -maxdepth 1 \
        \( -name '.Warcraft III.stage.*' -o -name '.Warcraft III.previous.*' \) -print)
    [ -z "$repeat_temps" ] || fail "staging temporary paths remain: $repeat_temps"
}

test_prohibited_files_and_bundle_verifier() {
    exclude_source=$scratch/exclude/source
    exclude_dest=$scratch/exclude/destination
    exclude_bundle=$exclude_dest/Resources/Warcraft\ III
    exclude_err=$scratch/exclude/stderr
    make_fixture "$exclude_source"
    mkdir -p "$exclude_source/unrelated-directory"
    printf 'iso\n' > "$exclude_source/Warcraft III - Reign of Chaos.iso"
    printf 'setup\n' > "$exclude_source/SETUP.MPQ"
    printf 'notes\n' > "$exclude_source/README.txt"
    BZ_WC3_DATA_DIR="$exclude_source" "$tool" stage "$exclude_dest" >/dev/null
    "$tool" verify-bundle "$exclude_dest" >/dev/null
    assert_absent "$exclude_bundle/Warcraft III - Reign of Chaos.iso"
    assert_absent "$exclude_bundle/SETUP.MPQ"
    assert_absent "$exclude_bundle/README.txt"
    assert_absent "$exclude_bundle/unrelated-directory"

    printf 'forbidden\n' > "$exclude_bundle/Warcraft III - Reign of Chaos.iso"
    if "$tool" verify-bundle "$exclude_dest" >"$scratch/exclude/stdout" 2>"$exclude_err"; then
        fail "bundle verifier accepted an unexpected ISO"
    fi
    assert_line "$exclude_err" \
        "$tool_name: unexpected bundle entry: $exclude_bundle/Warcraft III - Reign of Chaos.iso"
}

test_bundle_symlink_rejection() {
    symlink_source=$scratch/symlink/source
    symlink_dest=$scratch/symlink/destination
    symlink_bundle=$symlink_dest/Resources/Warcraft\ III
    symlink_err=$scratch/symlink/stderr
    make_fixture "$symlink_source"
    BZ_WC3_DATA_DIR="$symlink_source" "$tool" stage "$symlink_dest" >/dev/null

    rm "$symlink_bundle/War3x.mpq"
    ln -s "$symlink_source/War3x.mpq" "$symlink_bundle/War3x.mpq"
    if "$tool" verify-bundle "$symlink_dest" >"$scratch/symlink/stdout" 2>"$symlink_err"; then
        fail "bundle verifier accepted a symlinked MPQ"
    fi
    assert_line "$symlink_err" \
        "$tool_name: bundle file must not be a symlink: $symlink_bundle/War3x.mpq"

    rm -rf "$symlink_bundle"
    ln -s "$symlink_source" "$symlink_bundle"
    if "$tool" verify-bundle "$symlink_dest" >"$scratch/symlink/stdout" 2>"$symlink_err"; then
        fail "bundle verifier accepted a symlinked data directory"
    fi
    assert_line "$symlink_err" \
        "$tool_name: bundle data directory must not be a symlink: $symlink_bundle"

    rm "$symlink_bundle"
    rm -rf "$symlink_dest/Resources"
    ln -s "$scratch/symlink" "$symlink_dest/Resources"
    if "$tool" verify-bundle "$symlink_dest" >"$scratch/symlink/stdout" 2>"$symlink_err"; then
        fail "bundle verifier accepted a symlinked Resources directory"
    fi
    assert_line "$symlink_err" \
        "$tool_name: bundle Resources directory must not be a symlink: $symlink_dest/Resources"
    if BZ_WC3_DATA_DIR="$symlink_source" "$tool" stage "$symlink_dest" \
        >"$scratch/symlink/stdout" 2>"$symlink_err"; then
        fail "stage accepted a symlinked Resources directory"
    fi
    assert_line "$symlink_err" \
        "$tool_name: bundle Resources directory must not be a symlink: $symlink_dest/Resources"
}

assert_interrupt_mpq() {
    assert_same "$interrupt_expected/$1" "$interrupt_bundle/$1"
}

assert_replacement_mpq() {
    assert_same "$interrupt_source/$1" "$interrupt_bundle/$1"
}

run_interrupted_stage() {
    interrupt_point=$1
    if PATH="$interrupt_bin:$PATH" BZ_TEST_INTERRUPT_MOVE="$interrupt_point" \
        BZ_WC3_DATA_DIR="$interrupt_source" "$tool" stage "$interrupt_dest" \
        >"$scratch/interrupt/stdout" 2>"$scratch/interrupt/stderr"; then
        fail "stage survived the injected $interrupt_point interrupt"
    fi
    visit_test_mpqs assert_interrupt_mpq
    "$tool" verify-bundle "$interrupt_dest" >/dev/null
    interrupt_temps=$(find "$interrupt_dest/Resources" -mindepth 1 -maxdepth 1 \
        \( -name '.Warcraft III.stage.*' -o -name '.Warcraft III.previous.*' \) -print)
    [ -z "$interrupt_temps" ] || fail "$interrupt_point interrupt left temporary paths: $interrupt_temps"
}

test_interrupt_restores_previous_bundle() {
    interrupt_source=$scratch/interrupt/source
    interrupt_expected=$scratch/interrupt/expected
    interrupt_dest=$scratch/interrupt/destination
    interrupt_bundle=$interrupt_dest/Resources/Warcraft\ III
    interrupt_bin=$scratch/interrupt/bin
    make_fixture "$interrupt_source"
    BZ_WC3_DATA_DIR="$interrupt_source" "$tool" stage "$interrupt_dest" >/dev/null
    mkdir -p "$interrupt_expected" "$interrupt_bin"
    cp "$interrupt_bundle/"*.mpq "$interrupt_expected/"
    cat > "$interrupt_bin/mv" <<'EOF'
#!/bin/sh
/bin/mv "$@"
case ${BZ_TEST_INTERRUPT_MOVE:-} in
    backup) case ${2:-} in *'.previous.'*) kill -TERM "$PPID" ;; esac ;;
    target) case ${1:-} in *'.stage.'*) kill -TERM "$PPID" ;; esac ;;
esac
EOF
    cat > "$interrupt_bin/rm" <<'EOF'
#!/bin/sh
/bin/rm "$@"
if [ "${BZ_TEST_INTERRUPT_MOVE:-}" = finalize ] && [ "$#" -eq 2 ]; then
    case ${2:-} in *'.previous.'*) kill -TERM "$PPID" ;; esac
fi
EOF
    chmod +x "$interrupt_bin/mv"
    chmod +x "$interrupt_bin/rm"
    printf 'replacement\n' > "$interrupt_source/War3.mpq"
    printf 'replacement\n' > "$interrupt_source/War3x.mpq"
    printf 'replacement\n' > "$interrupt_source/War3xLocal.mpq"
    run_interrupted_stage backup
    run_interrupted_stage target
    PATH="$interrupt_bin:$PATH" BZ_TEST_INTERRUPT_MOVE=finalize \
        BZ_WC3_DATA_DIR="$interrupt_source" "$tool" stage "$interrupt_dest" >/dev/null
    visit_test_mpqs assert_replacement_mpq
    "$tool" verify-bundle "$interrupt_dest" >/dev/null
    interrupt_temps=$(find "$interrupt_dest/Resources" -mindepth 1 -maxdepth 1 \
        \( -name '.Warcraft III.stage.*' -o -name '.Warcraft III.previous.*' \) -print)
    [ -z "$interrupt_temps" ] || fail "finalization interrupt left temporary paths: $interrupt_temps"
}

run_test() {
    "$1"
    tests_run=$((tests_run + 1))
}

run_test test_source_override_and_layout
run_test test_default_source
run_test test_missing_empty_and_failed_stage_cleanup
run_test test_cleanup_and_idempotence
run_test test_prohibited_files_and_bundle_verifier
run_test test_bundle_symlink_rejection
run_test test_interrupt_restores_previous_bundle
printf 'test_wc3_data: %d/7 passed\n' "$tests_run"
