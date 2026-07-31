#!/bin/sh
set -eu

test_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
tool=$test_dir/../scripts/sc2_data.sh
tool_name=${tool##*/}
scratch=$(mktemp -d "${TMPDIR:-/tmp}/openrealm-sc2-data.XXXXXX")
tests_run=0
trap 'rm -rf "$scratch"' 0 1 2 15

fail() { printf 'test_sc2_data: FAIL: %s\n' "$*" >&2; exit 1; }

visit_files() {
    callback=$1
    "$callback" Battle.net/Battle.net.MPQ
    "$callback" Mods/Challenges.SC2Mod
    "$callback" Mods/Core.SC2Mod/base.SC2Assets
    "$callback" Mods/Core.SC2Mod/Base.SC2Data
    "$callback" Mods/Core.SC2Mod/Index.SC2Locale
    "$callback" Mods/Liberty.SC2Mod/base.SC2Assets
    "$callback" Mods/Liberty.SC2Mod/Base.SC2Data
    "$callback" Mods/LibertyMulti.SC2Mod/Base.SC2Data
    "$callback" Campaigns/Liberty.SC2Campaign/base.SC2Assets
    "$callback" Campaigns/Liberty.SC2Campaign/Base.SC2Data
    "$callback" Campaigns/Liberty.SC2Campaign/Base.SC2Maps
    "$callback" Campaigns/LibertyStory.SC2Campaign/Base.SC2Data
}

write_fixture() {
    mkdir -p "$fixture_dir/${1%/*}"
    printf 'synthetic SC2 fixture: %s\n' "$1" > "$fixture_dir/$1"
}

make_fixture() { fixture_dir=$1; mkdir -p "$fixture_dir"; visit_files write_fixture; }
assert_same() { cmp -s "$1" "$2" || fail "$1 and $2 differ"; }
assert_absent() { if [ -e "$1" ] || [ -L "$1" ]; then fail "unexpected path: $1"; fi; }
assert_line() { grep -F -x "$2" "$1" >/dev/null || fail "missing diagnostic: $2"; }
assert_staged() { assert_same "$source_dir/$1" "$bundle_dir/$1"; }

test_override_default_and_exact_layout() {
    source_dir=$scratch/override/source\ with\ spaces
    stage_dir=$scratch/override/App\ Stage
    bundle_dir=$stage_dir/Resources/StarCraft\ II
    make_fixture "$source_dir"
    BZ_SC2_DATA_DIR="$source_dir" "$tool" verify-source >/dev/null
    BZ_SC2_DATA_DIR="$source_dir" "$tool" stage "$stage_dir" >/dev/null
    "$tool" verify-bundle "$stage_dir" >/dev/null
    visit_files assert_staged
    printf 'ignored\n' > "$source_dir/Installer Tome 1.MPQE"
    assert_absent "$bundle_dir/Installer Tome 1.MPQE"

    default_home=$scratch/default/home\ with\ spaces
    source_dir=$default_home/Downloads/Starcraft\ II/StarCraft2
    stage_dir=$scratch/default/Bundle
    bundle_dir=$stage_dir/Resources/StarCraft\ II
    make_fixture "$source_dir"
    (unset BZ_SC2_DATA_DIR; HOME="$default_home" "$tool" stage "$stage_dir") >/dev/null
    visit_files assert_staged
}

test_missing_empty_and_source_symlinks() {
    source_dir=$scratch/invalid/source
    stage_dir=$scratch/invalid/stage
    err=$scratch/invalid/stderr
    make_fixture "$source_dir"
    : > "$source_dir/Mods/Core.SC2Mod/Base.SC2Data"
    rm "$source_dir/Mods/Liberty.SC2Mod/base.SC2Assets"
    if BZ_SC2_DATA_DIR="$source_dir" "$tool" stage "$stage_dir" 2>"$err"; then
        fail "accepted missing or empty source files"
    fi
    assert_line "$err" "$tool_name: required file is empty: $source_dir/Mods/Core.SC2Mod/Base.SC2Data"
    assert_line "$err" "$tool_name: missing required file: $source_dir/Mods/Liberty.SC2Mod/base.SC2Assets"
    assert_absent "$stage_dir/Resources/StarCraft II"

    make_fixture "$source_dir"
    mv "$source_dir/Mods/LibertyMulti.SC2Mod/Base.SC2Data" "$scratch/invalid/real-data"
    ln -s "$scratch/invalid/real-data" "$source_dir/Mods/LibertyMulti.SC2Mod/Base.SC2Data"
    if BZ_SC2_DATA_DIR="$source_dir" "$tool" verify-source 2>"$err"; then
        fail "accepted a symlinked required source file"
    fi
    assert_line "$err" \
        "$tool_name: required file must not be a symlink: $source_dir/Mods/LibertyMulti.SC2Mod/Base.SC2Data"
}

test_cleanup_idempotence_and_bundle_rejection() {
    source_dir=$scratch/repeat/source
    stage_dir=$scratch/repeat/stage
    bundle_dir=$stage_dir/Resources/StarCraft\ II
    err=$scratch/repeat/stderr
    make_fixture "$source_dir"
    BZ_SC2_DATA_DIR="$source_dir" "$tool" stage "$stage_dir" >/dev/null
    mkdir -p "$bundle_dir/IX86"
    printf 'installer\n' > "$bundle_dir/Installer Tome 1.MPQE"
    printf 'updated\n' > "$source_dir/Mods/Core.SC2Mod/base.SC2Assets"
    BZ_SC2_DATA_DIR="$source_dir" "$tool" stage "$stage_dir" >/dev/null
    BZ_SC2_DATA_DIR="$source_dir" "$tool" stage "$stage_dir" >/dev/null
    assert_absent "$bundle_dir/IX86"
    assert_absent "$bundle_dir/Installer Tome 1.MPQE"
    visit_files assert_staged
    printf 'forbidden\n' > "$bundle_dir/Campaigns/Liberty.SC2Campaign/installer.iso"
    if "$tool" verify-bundle "$stage_dir" 2>"$err"; then fail "accepted unexpected bundle data"; fi
    assert_line "$err" \
        "$tool_name: unexpected bundle entry: $bundle_dir/Campaigns/Liberty.SC2Campaign/installer.iso"
}

test_bundle_symlink_rejection() {
    source_dir=$scratch/symlink/source
    stage_dir=$scratch/symlink/stage
    bundle_dir=$stage_dir/Resources/StarCraft\ II
    err=$scratch/symlink/stderr
    make_fixture "$source_dir"
    BZ_SC2_DATA_DIR="$source_dir" "$tool" stage "$stage_dir" >/dev/null
    rm "$bundle_dir/Mods/Core.SC2Mod/Base.SC2Data"
    ln -s "$source_dir/Mods/Core.SC2Mod/Base.SC2Data" "$bundle_dir/Mods/Core.SC2Mod/Base.SC2Data"
    if "$tool" verify-bundle "$stage_dir" 2>"$err"; then fail "accepted bundle file symlink"; fi
    assert_line "$err" \
        "$tool_name: required file must not be a symlink: $bundle_dir/Mods/Core.SC2Mod/Base.SC2Data"
    rm -rf "$stage_dir/Resources"
    ln -s "$scratch/symlink" "$stage_dir/Resources"
    if BZ_SC2_DATA_DIR="$source_dir" "$tool" stage "$stage_dir" 2>"$err"; then
        fail "accepted symlinked Resources"
    fi
    assert_line "$err" "$tool_name: bundle Resources directory must not be a symlink: $stage_dir/Resources"
}

assert_previous() { assert_same "$expected_dir/$1" "$bundle_dir/$1"; }

test_interrupt_restores_previous_bundle() {
    source_dir=$scratch/interrupt/source
    stage_dir=$scratch/interrupt/stage
    bundle_dir=$stage_dir/Resources/StarCraft\ II
    expected_dir=$scratch/interrupt/expected
    bin_dir=$scratch/interrupt/bin
    make_fixture "$source_dir"
    BZ_SC2_DATA_DIR="$source_dir" "$tool" stage "$stage_dir" >/dev/null
    mkdir -p "$expected_dir" "$bin_dir"
    cp -R "$bundle_dir/." "$expected_dir/"
    printf 'replacement\n' > "$source_dir/Mods/Core.SC2Mod/Base.SC2Data"
    cat > "$bin_dir/mv" <<'EOF'
#!/bin/sh
/bin/mv "$@"
case ${2:-} in *'.previous.'*) kill -TERM "$PPID" ;; esac
EOF
    chmod +x "$bin_dir/mv"
    if PATH="$bin_dir:$PATH" BZ_SC2_DATA_DIR="$source_dir" "$tool" stage "$stage_dir" \
        >"$scratch/interrupt/stdout" 2>"$scratch/interrupt/stderr"; then
        fail "stage survived injected interruption"
    fi
    visit_files assert_previous
    "$tool" verify-bundle "$stage_dir" >/dev/null
    temps=$(find "$stage_dir/Resources" -mindepth 1 -maxdepth 1 \
        \( -name '.StarCraft II.stage.*' -o -name '.StarCraft II.previous.*' \) -print)
    [ -z "$temps" ] || fail "interrupted stage left temporary paths: $temps"
}

test_repository_proprietary_guard() {
    repository=$scratch/repository
    err=$scratch/repository-stderr
    not_repository=$scratch/not-repository
    mkdir -p "$not_repository"
    if "$tool" verify-repository "$not_repository" >"$scratch/repository-stdout" 2>"$err"; then
        fail "repository guard ignored Git enumeration failure"
    fi
    assert_line "$err" "$tool_name: failed to enumerate tracked files: $not_repository"
    mkdir -p "$repository"
    git -C "$repository" init -q
    printf 'safe\n' > "$repository/safe.txt"
    git -C "$repository" add safe.txt
    "$tool" verify-repository "$repository" >/dev/null
    for payload in payload.mpq payload.mpqe payload.SC2Assets payload.SC2Data \
        payload.SC2Locale payload.SC2Maps payload.SC2Mod payload.iso; do
        printf 'proprietary\n' > "$repository/$payload"
        git -C "$repository" add "$payload"
        if "$tool" verify-repository "$repository" >"$scratch/repository-stdout" 2>"$err"; then
            fail "repository guard accepted $payload"
        fi
        assert_line "$err" "$tool_name: proprietary archive tracked by Git: $payload"
        git -C "$repository" rm --cached -q "$payload"
        rm "$repository/$payload"
    done
}

run_test() { "$1"; tests_run=$((tests_run + 1)); }
run_test test_override_default_and_exact_layout
run_test test_missing_empty_and_source_symlinks
run_test test_cleanup_idempotence_and_bundle_rejection
run_test test_bundle_symlink_rejection
run_test test_interrupt_restores_previous_bundle
run_test test_repository_proprietary_guard
printf 'test_sc2_data: %d/6 passed\n' "$tests_run"
