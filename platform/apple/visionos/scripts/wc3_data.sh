#!/bin/sh
set -eu

tool_name=${0##*/}

visit_required_mpqs() {
    visit_callback=$1
    "$visit_callback" War3.mpq
    "$visit_callback" War3x.mpq
    "$visit_callback" War3xLocal.mpq
}

usage() {
    cat >&2 <<EOF
Usage:
  $tool_name verify-source
  $tool_name stage <app-or-staging-root>
  $tool_name verify-bundle <app-or-staging-root>

Source: \${BZ_WC3_DATA_DIR:-\$HOME/Downloads/Warcraft III}
Bundle destination: <app-or-staging-root>/Resources/Warcraft III
EOF
}

resolve_source_dir() {
    if [ -n "${BZ_WC3_DATA_DIR:-}" ]; then
        printf '%s\n' "$BZ_WC3_DATA_DIR"
        return
    fi
    if [ -z "${HOME:-}" ]; then
        printf '%s: HOME is unset and BZ_WC3_DATA_DIR was not provided\n' "$tool_name" >&2
        return 1
    fi
    printf '%s\n' "$HOME/Downloads/Warcraft III"
}

validate_required_mpq() {
    validate_path=$validate_dir/$1
    if [ "$validate_reject_symlinks" -eq 1 ] && [ -L "$validate_path" ]; then
        printf '%s: bundle file must not be a symlink: %s\n' "$tool_name" "$validate_path" >&2
        validate_result=1
    elif [ ! -f "$validate_path" ]; then
        printf '%s: missing required file: %s\n' "$tool_name" "$validate_path" >&2
        validate_result=1
    elif [ ! -s "$validate_path" ]; then
        printf '%s: required file is empty: %s\n' "$tool_name" "$validate_path" >&2
        validate_result=1
    fi
}

validate_required_mpqs() {
    validate_dir=$1
    validate_reject_symlinks=${2:-0}
    validate_result=0
    visit_required_mpqs validate_required_mpq
    return "$validate_result"
}

remove_expected_bundle_entry() {
    verify_entries=$(printf '%s\n' "$verify_entries" |
        awk -v expected="$verify_dir/$1" '$0 != expected')
}

verify_bundle() {
    verify_root=$1
    verify_resources="$verify_root/Resources"
    verify_dir="$verify_root/Resources/Warcraft III"
    verify_result=0

    if [ -L "$verify_resources" ]; then
        printf '%s: bundle Resources directory must not be a symlink: %s\n' \
            "$tool_name" "$verify_resources" >&2
        return 1
    fi
    if [ -L "$verify_dir" ]; then
        printf '%s: bundle data directory must not be a symlink: %s\n' "$tool_name" "$verify_dir" >&2
        return 1
    fi
    if ! validate_required_mpqs "$verify_dir" 1; then verify_result=1; fi
    if [ -d "$verify_dir" ]; then
        verify_entries=$(find "$verify_dir" -mindepth 1 -maxdepth 1 -print)
        if [ -n "$verify_entries" ]; then
            visit_required_mpqs remove_expected_bundle_entry
        fi
        if [ -n "$verify_entries" ]; then
            printf '%s\n' "$verify_entries" | while IFS= read -r verify_path; do
                printf '%s: unexpected bundle entry: %s\n' "$tool_name" "$verify_path" >&2
            done
            verify_result=1
        fi
    fi
    return "$verify_result"
}

copy_required_mpq() {
    cp "$stage_source/$1" "$stage_tmp/$1"
}

cleanup_stage() {
    cleanup_status=$?
    if [ "$stage_committed" -eq 1 ]; then cleanup_status=0; fi
    trap - 0
    trap '' 1 2 15
    if ! rm -rf "$stage_tmp"; then
        printf '%s: failed to remove staging directory: %s\n' "$tool_name" "$stage_tmp" >&2
        cleanup_status=1
    fi
    if [ "$stage_committed" -eq 1 ]; then
        if ! rm -rf "$stage_backup"; then
            printf '%s: failed to remove previous bundle backup: %s\n' "$tool_name" "$stage_backup" >&2
            cleanup_status=1
        fi
        exit "$cleanup_status"
    fi
    if [ "$stage_had_previous" -eq 1 ]; then
        if [ -e "$stage_backup" ] || [ -L "$stage_backup" ]; then
            cleanup_can_restore=1
            if { [ -e "$stage_target" ] || [ -L "$stage_target" ]; } && ! rm -rf "$stage_target"; then
                printf '%s: failed to remove replacement bundle data: %s\n' "$tool_name" "$stage_target" >&2
                cleanup_status=1
                cleanup_can_restore=0
            fi
            if [ "$cleanup_can_restore" -eq 1 ] && ! mv "$stage_backup" "$stage_target"; then
                printf '%s: failed to restore previous bundle data: %s\n' "$tool_name" "$stage_target" >&2
                cleanup_status=1
            fi
        elif [ -e "$stage_target" ] || [ -L "$stage_target" ]; then
            :
        else
            printf '%s: previous bundle backup disappeared: %s\n' "$tool_name" "$stage_backup" >&2
            cleanup_status=1
        fi
    else
        if [ "$stage_target_installed" -eq 1 ] && ! rm -rf "$stage_target"; then
            printf '%s: failed to remove incomplete bundle data: %s\n' "$tool_name" "$stage_target" >&2
            cleanup_status=1
        fi
        if ! rm -rf "$stage_backup"; then
            printf '%s: failed to remove unused bundle backup: %s\n' "$tool_name" "$stage_backup" >&2
            cleanup_status=1
        fi
    fi
    if [ "$cleanup_status" -eq 0 ]; then cleanup_status=1; fi
    exit "$cleanup_status"
}

stage_bundle() {
    stage_root=$1
    stage_source=$(resolve_source_dir)
    stage_resources="$stage_root/Resources"
    stage_target="$stage_resources/Warcraft III"
    stage_tmp="$stage_resources/.Warcraft III.stage.$$"
    stage_backup="$stage_resources/.Warcraft III.previous.$$"
    stage_had_previous=0
    stage_target_installed=0
    stage_committed=0

    if ! validate_required_mpqs "$stage_source"; then return 1; fi
    if [ -L "$stage_resources" ]; then
        printf '%s: bundle Resources directory must not be a symlink: %s\n' \
            "$tool_name" "$stage_resources" >&2
        return 1
    fi
    mkdir -p "$stage_resources"
    rm -rf "$stage_tmp" "$stage_backup"
    trap cleanup_stage 0 1 2 15
    mkdir "$stage_tmp"
    visit_required_mpqs copy_required_mpq
    if ! validate_required_mpqs "$stage_tmp" 1; then return 1; fi

    if [ -e "$stage_target" ] || [ -L "$stage_target" ]; then
        stage_had_previous=1
        mv "$stage_target" "$stage_backup"
    fi
    stage_target_installed=1
    if ! mv "$stage_tmp" "$stage_target"; then
        return 1
    fi
    if ! verify_bundle "$stage_root"; then return 1; fi
    stage_committed=1
    printf '%s: staged Warcraft III data at %s\n' "$tool_name" "$stage_target"
}

case ${1:-} in
    verify-source)
        if [ "$#" -ne 1 ]; then usage; exit 2; fi
        source_dir=$(resolve_source_dir)
        validate_required_mpqs "$source_dir"
        printf '%s: verified Warcraft III data at %s\n' "$tool_name" "$source_dir"
        ;;
    stage)
        if [ "$#" -ne 2 ] || [ -z "$2" ]; then usage; exit 2; fi
        stage_bundle "$2"
        ;;
    verify-bundle)
        if [ "$#" -ne 2 ] || [ -z "$2" ]; then usage; exit 2; fi
        verify_bundle "$2"
        printf '%s: verified Warcraft III bundle data at %s/Resources/Warcraft III\n' "$tool_name" "$2"
        ;;
    *)
        usage
        exit 2
        ;;
esac
