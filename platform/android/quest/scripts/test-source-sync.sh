#!/bin/sh
# platform/android/quest/scripts/test-source-sync.sh
#
# Guards the Make -> CMake source-list synchronization contract documented
# in docs/quest-tabletop.md: CMakeLists.txt fetches BZ_XR_ENGINE_SRCS,
# BZ_XR_GAME_SRCS, BZ_XR_ASSET_SRCS, BZ_XR_JASS_SRCS, BZ_XR_SHEET_SRCS,
# BZ_XR_SHARED_SRCS, BZ_XR_BASE_CFLAGS, BZ_XR_CFLAGS, and BZ_XR_FDF_CFLAGS
# live from platform/apple/visionos/build.mk via the root Makefile's
# `print-%` debug target, instead of duplicating those file lists as a
# second, driftable literal. This test fails loudly if:
#   - any of those Make variables goes missing or resolves empty (would
#     silently make CMakeLists.txt's bz_make_srcs() fail at configure time -
#     this test catches the same drift earlier and with a clearer message)
#   - a known sentinel source file drops out of one of the six source lists
#   - the print-% target itself regresses (inverse case: an undefined
#     variable name must still resolve to an empty (not erroring) `NAME=`
#     line, which is the exact contract CMakeLists.txt's bz_make_srcs()
#     depends on to detect drift and fail loudly rather than silently
#     linking zero sources)
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

FAIL=0

# name:sentinel-substring pairs. Sentinels are real, currently-listed files
# picked from each group (verified via `make -s print-<VAR>` while writing
# this test) so a future accidental removal from build.mk is caught here
# instead of only failing deep inside a slow Gradle/CMake configure.
check_srcs() {
    VAR=$1
    SENTINEL=$2
    VALUE=$(make -s "print-$VAR")
    LIST=${VALUE#"$VAR="}
    if [ "$LIST" = "$VALUE" ]; then
        echo "test-source-sync: 'make print-$VAR' did not produce a '$VAR=' prefixed line: $VALUE" >&2
        FAIL=1
        return
    fi
    if [ -z "$LIST" ]; then
        echo "test-source-sync: $VAR resolved to an empty source list" >&2
        FAIL=1
        return
    fi
    case " $LIST " in
        *" $SENTINEL "*) ;;
        *)
            echo "test-source-sync: $VAR is missing expected sentinel file '$SENTINEL'" >&2
            FAIL=1
            ;;
    esac
}

check_cflags() {
    VAR=$1
    VALUE=$(make -s "print-$VAR")
    LIST=${VALUE#"$VAR="}
    if [ "$LIST" = "$VALUE" ] || [ -z "$LIST" ]; then
        echo "test-source-sync: $VAR resolved empty/unexpected: $VALUE" >&2
        FAIL=1
        return
    fi
    case " $LIST " in
        *" -I. "*) ;;
        *)
            echo "test-source-sync: $VAR is missing the repo-root include flag '-I.' that CMakeLists.txt's bz_make_cflags() rewrites to an absolute path" >&2
            FAIL=1
            ;;
    esac
}

check_srcs BZ_XR_ENGINE_SRCS common/bz_runtime.c
check_srcs BZ_XR_GAME_SRCS   games/warcraft-3/game/g_ai.c
check_srcs BZ_XR_ASSET_SRCS  games/warcraft-3/visionos/wc3_blp_decode.c
check_srcs BZ_XR_JASS_SRCS   games/warcraft-3/jass/jcode.c
check_srcs BZ_XR_SHEET_SRCS  games/warcraft-3/sheet/sheet.c
check_srcs BZ_XR_SHARED_SRCS shared/source/box2.c

check_cflags BZ_XR_BASE_CFLAGS
check_cflags BZ_XR_CFLAGS
check_cflags BZ_XR_FDF_CFLAGS

# Inverse case: an undefined variable name must resolve to an empty value
# (`NAME=`, no error) - this is the exact "silent empty" failure mode
# CMakeLists.txt's bz_make_srcs()/bz_make_var() must catch and turn into a
# FATAL_ERROR instead of silently linking zero sources.
UNDEFINED_NAME=BZ_XR_THIS_VARIABLE_DOES_NOT_EXIST_SENTINEL
UNDEFINED_VALUE=$(make -s "print-$UNDEFINED_NAME")
if [ "$UNDEFINED_VALUE" != "$UNDEFINED_NAME=" ]; then
    echo "test-source-sync: print-% target's undefined-variable contract regressed: got '$UNDEFINED_VALUE'" >&2
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "test-source-sync: OK (Quest CMakeLists.txt's Make source-list synchronization contract holds)"
