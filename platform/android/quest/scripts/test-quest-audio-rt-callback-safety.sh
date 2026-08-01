#!/bin/sh
# platform/android/quest/scripts/test-quest-audio-rt-callback-safety.sh
#
# Structurally guards the real-time-safety contract documented in
# bz_quest_audio.c's bz_quest_audio_data_callback() header comment and
# bz_quest_audio_mixer.h's bz_quest_audio_mixer_render() contract: AAudio's
# data callback runs on a real-time-priority audio thread, and per the
# official AAudio guidance
# (https://developer.android.com/ndk/guides/audio/aaudio/aaudio#audio-callback),
# that callback must never allocate, free, lock, log, block on I/O, or call
# into any file-decode/bridge/engine API - only touch pre-prepared PCM.
#
# This is a no-NDK/no-device host build vs. one that would need real AAudio
# hardware/host stream integration to observe indirectly at runtime - so we
# grep the real source instead, extracting exactly the RT-called function
# bodies (not the whole file, which legitimately does malloc/free/log
# elsewhere on the control thread) and failing loudly if a future edit
# reintroduces a forbidden call into either scope.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

AUDIO_C=platform/android/quest/app/src/main/cpp/bz_quest_audio.c
MIXER_C=platform/android/quest/app/src/main/cpp/bz_quest_audio_mixer.c
FAIL=0

if [ ! -f "$AUDIO_C" ] || [ ! -f "$MIXER_C" ]; then
    echo "test-quest-audio-rt-callback-safety: expected source files missing: $AUDIO_C / $MIXER_C" >&2
    exit 1
fi

# Extracts one function's body (from its "name(" line up to the line whose
# sole content is the closing brace) via awk - good enough for this
# project's one-brace-per-line C style, and exactly what the two functions
# below look like.
extract_fn() {
    file=$1
    name=$2
    awk -v fn="$name" '
        $0 ~ fn"\\(" { found=1 }
        found { print }
        found && /^}/ { exit }
    ' "$file"
}

check_forbidden() {
    label=$1
    body=$2
    # Forbidden: heap (de)allocation, locking, logging, and the two obvious
    # ways this codebase calls into bridge/file APIs (BZ_TT_ and BZ_ prefixed
    # bridge functions, fopen/fread for file decode). Deliberately NOT
    # matching bare "free" as a substring (e.g. inside a comment word like
    # "freely") - only real call-sites "malloc(", "free(", etc.
    for pat in 'malloc(' 'calloc(' 'realloc(' 'free(' 'pthread_mutex_' \
               'BZ_QUEST_LOGE' 'BZ_QUEST_LOGW' 'BZ_QUEST_LOGI' \
               'fprintf(' 'printf(' 'fopen(' 'fread(' 'BZ_TT_' ; do
        if printf '%s' "$body" | grep -qF "$pat"; then
            echo "test-quest-audio-rt-callback-safety: $label calls forbidden '$pat' - the AAudio data callback must not allocate, lock, log, or touch files/bridge APIs (see $AUDIO_C's bz_quest_audio_data_callback header comment)" >&2
            FAIL=1
        fi
    done
}

data_cb_body=$(extract_fn "$AUDIO_C" "bz_quest_audio_data_callback")
if [ -z "$data_cb_body" ]; then
    echo "test-quest-audio-rt-callback-safety: could not find bz_quest_audio_data_callback in $AUDIO_C (renamed/removed?)" >&2
    FAIL=1
else
    check_forbidden "bz_quest_audio_data_callback" "$data_cb_body"
fi

render_body=$(extract_fn "$MIXER_C" "bz_quest_audio_mixer_render")
if [ -z "$render_body" ]; then
    echo "test-quest-audio-rt-callback-safety: could not find bz_quest_audio_mixer_render in $MIXER_C (renamed/removed?)" >&2
    FAIL=1
else
    check_forbidden "bz_quest_audio_mixer_render" "$render_body"
fi

# The data callback must be the ONLY function this file registers with
# AAudio as the data callback (i.e. AAudioStreamBuilder_setDataCallback is
# called exactly once, with this function) - guards against a future
# second stream/callback being added without the same scrutiny.
callback_registrations=$(grep -c 'AAudioStreamBuilder_setDataCallback' "$AUDIO_C" || true)
if [ "$callback_registrations" != "1" ]; then
    echo "test-quest-audio-rt-callback-safety: expected exactly one AAudioStreamBuilder_setDataCallback call in $AUDIO_C, found $callback_registrations" >&2
    FAIL=1
fi
if ! grep -q 'AAudioStreamBuilder_setDataCallback(builder, bz_quest_audio_data_callback' "$AUDIO_C"; then
    echo "test-quest-audio-rt-callback-safety: $AUDIO_C no longer registers bz_quest_audio_data_callback as the AAudio data callback" >&2
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "test-quest-audio-rt-callback-safety: bz_quest_audio_data_callback and bz_quest_audio_mixer_render contain no allocation/lock/log/file/bridge calls; callback registration intact"
