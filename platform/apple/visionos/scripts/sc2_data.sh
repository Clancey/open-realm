#!/bin/sh
set -eu

tool_name=${0##*/}

visit_required_files() {
    visit_callback=$1
    "$visit_callback" Battle.net/Battle.net.MPQ
    "$visit_callback" Mods/Challenges.SC2Mod
    "$visit_callback" Mods/Core.SC2Mod/base.SC2Assets
    "$visit_callback" Mods/Core.SC2Mod/Base.SC2Data
    "$visit_callback" Mods/Core.SC2Mod/Index.SC2Locale
    "$visit_callback" Mods/Liberty.SC2Mod/base.SC2Assets
    "$visit_callback" Mods/Liberty.SC2Mod/Base.SC2Data
    "$visit_callback" Mods/LibertyMulti.SC2Mod/Base.SC2Data
    "$visit_callback" Campaigns/Liberty.SC2Campaign/base.SC2Assets
    "$visit_callback" Campaigns/Liberty.SC2Campaign/Base.SC2Data
    "$visit_callback" Campaigns/Liberty.SC2Campaign/Base.SC2Maps
    "$visit_callback" Campaigns/LibertyStory.SC2Campaign/Base.SC2Data
}

visit_required_dirs() {
    visit_callback=$1
    "$visit_callback" Battle.net
    "$visit_callback" Mods
    "$visit_callback" Mods/Core.SC2Mod
    "$visit_callback" Mods/Liberty.SC2Mod
    "$visit_callback" Mods/LibertyMulti.SC2Mod
    "$visit_callback" Campaigns
    "$visit_callback" Campaigns/Liberty.SC2Campaign
    "$visit_callback" Campaigns/LibertyStory.SC2Campaign
}

usage() {
    cat >&2 <<EOF
Usage:
  $tool_name verify-source
  $tool_name stage <app-or-staging-root>
  $tool_name verify-bundle <app-or-staging-root>
  $tool_name verify-repository [repository-root]

Source: \${BZ_SC2_DATA_DIR:-\$HOME/Downloads/Starcraft II/StarCraft2}
Bundle destination: <app-or-staging-root>/Resources/StarCraft II
EOF
}

verify_repository() {
    repository_root=$1
    if ! repository_files=$(git -C "$repository_root" ls-files); then
        printf '%s: failed to enumerate tracked files: %s\n' "$tool_name" "$repository_root" >&2
        return 1
    fi
    repository_paths=$(printf '%s\n' "$repository_files" |
        grep -Ei '\.(mpq|mpqe|sc2assets|sc2data|sc2locale|sc2maps|sc2mod|iso)$' || true)
    if [ -n "$repository_paths" ]; then
        printf '%s\n' "$repository_paths" | while IFS= read -r repository_path; do
            printf '%s: proprietary archive tracked by Git: %s\n' "$tool_name" "$repository_path" >&2
        done
        return 1
    fi
}

resolve_source_dir() {
    if [ -n "${BZ_SC2_DATA_DIR:-}" ]; then
        printf '%s\n' "$BZ_SC2_DATA_DIR"
    elif [ -n "${HOME:-}" ]; then
        printf '%s\n' "$HOME/Downloads/Starcraft II/StarCraft2"
    else
        printf '%s: HOME is unset and BZ_SC2_DATA_DIR was not provided\n' "$tool_name" >&2
        return 1
    fi
}

validate_required_file() {
    validate_path=$validate_dir/$1
    validate_parent=${validate_path%/*}
    while [ "$validate_parent" != "$validate_dir" ]; do
        if [ -L "$validate_parent" ]; then
            printf '%s: required directory must not be a symlink: %s\n' "$tool_name" "$validate_parent" >&2
            validate_result=1
            break
        fi
        validate_parent=${validate_parent%/*}
    done
    if [ -L "$validate_path" ]; then
        printf '%s: required file must not be a symlink: %s\n' "$tool_name" "$validate_path" >&2
        validate_result=1
    elif [ ! -f "$validate_path" ]; then
        printf '%s: missing required file: %s\n' "$tool_name" "$validate_path" >&2
        validate_result=1
    elif [ ! -s "$validate_path" ]; then
        printf '%s: required file is empty: %s\n' "$tool_name" "$validate_path" >&2
        validate_result=1
    fi
}

validate_required_files() {
    validate_dir=$1
    validate_result=0
    if [ -L "$validate_dir" ]; then
        printf '%s: data directory must not be a symlink: %s\n' "$tool_name" "$validate_dir" >&2
        return 1
    fi
    visit_required_files validate_required_file
    return "$validate_result"
}

expected_bundle_entry() {
    case $1 in
        Battle.net|Mods|Mods/Core.SC2Mod|Mods/Liberty.SC2Mod|Mods/LibertyMulti.SC2Mod|\
        Campaigns|Campaigns/Liberty.SC2Campaign|Campaigns/LibertyStory.SC2Campaign|\
        Battle.net/Battle.net.MPQ|Mods/Challenges.SC2Mod|\
        Mods/Core.SC2Mod/base.SC2Assets|Mods/Core.SC2Mod/Base.SC2Data|\
        Mods/Core.SC2Mod/Index.SC2Locale|Mods/Liberty.SC2Mod/base.SC2Assets|\
        Mods/Liberty.SC2Mod/Base.SC2Data|\
        Mods/LibertyMulti.SC2Mod/Base.SC2Data|\
        Campaigns/Liberty.SC2Campaign/base.SC2Assets|\
        Campaigns/Liberty.SC2Campaign/Base.SC2Data|\
        Campaigns/Liberty.SC2Campaign/Base.SC2Maps|\
        Campaigns/LibertyStory.SC2Campaign/Base.SC2Data) return 0 ;;
    esac
    return 1
}

verify_bundle() {
    verify_root=$1
    verify_resources=$verify_root/Resources
    verify_dir=$verify_resources/StarCraft\ II
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
    if ! validate_required_files "$verify_dir"; then verify_result=1; fi
    if [ -d "$verify_dir" ]; then
        while IFS= read -r verify_path; do
            verify_relative=${verify_path#"$verify_dir"/}
            if [ -L "$verify_path" ]; then
                printf '%s: bundle entry must not be a symlink: %s\n' "$tool_name" "$verify_path" >&2
                verify_result=1
            elif ! expected_bundle_entry "$verify_relative"; then
                printf '%s: unexpected bundle entry: %s\n' "$tool_name" "$verify_path" >&2
                verify_result=1
            fi
        done <<EOF
$(find "$verify_dir" -mindepth 1 -print)
EOF
    fi
    return "$verify_result"
}

copy_required_file() {
    copy_relative=$1
    mkdir -p "$stage_tmp/${copy_relative%/*}"
    cp "$stage_source/$copy_relative" "$stage_tmp/$copy_relative"
}

cleanup_stage() {
    cleanup_status=$?
    if [ "$stage_committed" -eq 1 ]; then cleanup_status=0; fi
    trap - 0
    trap '' 1 2 15
    if ! rm -rf "$stage_tmp"; then cleanup_status=1; fi
    if [ "$stage_committed" -eq 1 ]; then
        if ! rm -rf "$stage_backup"; then cleanup_status=1; fi
        exit "$cleanup_status"
    fi
    if [ "$stage_had_previous" -eq 1 ] && { [ -e "$stage_backup" ] || [ -L "$stage_backup" ]; }; then
        if { [ -e "$stage_target" ] || [ -L "$stage_target" ]; } && ! rm -rf "$stage_target"; then
            cleanup_status=1
        elif ! mv "$stage_backup" "$stage_target"; then
            cleanup_status=1
        fi
    elif [ "$stage_target_installed" -eq 1 ]; then
        if ! rm -rf "$stage_target"; then cleanup_status=1; fi
        if ! rm -rf "$stage_backup"; then cleanup_status=1; fi
    fi
    if [ "$cleanup_status" -eq 0 ]; then cleanup_status=1; fi
    exit "$cleanup_status"
}

stage_bundle() {
    stage_root=$1
    stage_source=$(resolve_source_dir)
    stage_resources=$stage_root/Resources
    stage_target=$stage_resources/StarCraft\ II
    stage_tmp=$stage_resources/.StarCraft\ II.stage.$$
    stage_backup=$stage_resources/.StarCraft\ II.previous.$$
    stage_had_previous=0
    stage_target_installed=0
    stage_committed=0
    if ! validate_required_files "$stage_source"; then return 1; fi
    if [ -L "$stage_resources" ]; then
        printf '%s: bundle Resources directory must not be a symlink: %s\n' \
            "$tool_name" "$stage_resources" >&2
        return 1
    fi
    mkdir -p "$stage_resources"
    rm -rf "$stage_tmp" "$stage_backup"
    trap cleanup_stage 0 1 2 15
    mkdir "$stage_tmp"
    visit_required_files copy_required_file
    if ! validate_required_files "$stage_tmp"; then return 1; fi
    if [ -e "$stage_target" ] || [ -L "$stage_target" ]; then
        stage_had_previous=1
        mv "$stage_target" "$stage_backup"
    fi
    stage_target_installed=1
    if ! mv "$stage_tmp" "$stage_target"; then return 1; fi
    if ! verify_bundle "$stage_root"; then return 1; fi
    stage_committed=1
    printf '%s: staged StarCraft II data at %s\n' "$tool_name" "$stage_target"
}

case ${1:-} in
    verify-source)
        if [ "$#" -ne 1 ]; then usage; exit 2; fi
        source_dir=$(resolve_source_dir)
        validate_required_files "$source_dir"
        printf '%s: verified StarCraft II data at %s\n' "$tool_name" "$source_dir"
        ;;
    stage)
        if [ "$#" -ne 2 ] || [ -z "$2" ]; then usage; exit 2; fi
        stage_bundle "$2"
        ;;
    verify-bundle)
        if [ "$#" -ne 2 ] || [ -z "$2" ]; then usage; exit 2; fi
        verify_bundle "$2"
        printf '%s: verified StarCraft II bundle data at %s/Resources/StarCraft II\n' "$tool_name" "$2"
        ;;
    verify-repository)
        if [ "$#" -gt 2 ]; then usage; exit 2; fi
        if [ "$#" -eq 2 ]; then
            repository_root=$2
        else
            script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
            repository_root=$(CDPATH= cd -- "$script_dir/../../../.." && pwd)
        fi
        verify_repository "$repository_root"
        printf '%s: verified repository contains no proprietary SC2 payloads\n' "$tool_name"
        ;;
    *) usage; exit 2 ;;
esac
