#!/bin/sh
# platform/android/quest/scripts/test-quest-hand-tracking-layout.sh
#
# Structurally guards layer 8's Meta Quest hand-tracking contract where
# host-unit tests cannot reach (bz_quest_hand_input.h/.c's pure gesture
# builder and bz_quest_input_state.h/.c's source arbitration ARE covered by
# test-quest-host-tests - this script only covers the impure OpenXR
# capability-negotiation/tracker-lifecycle modules and their renderer
# wiring):
#   1. the Quest-native build must compile the new bz_quest_hand_input.c/
#      bz_quest_xr_hands.c modules at all;
#   2. capability negotiation must be explicit and optional: both
#      XR_EXT_hand_tracking and XR_FB_hand_tracking_aim are probed (never
#      assumed) and their absence must never fail bz_quest_xr_create_instance()/
#      _get_system() the way a genuinely-required extension does;
#   3. XR_FB_hand_tracking_aim must only ever be requested/enabled alongside
#      XR_EXT_hand_tracking (its declared OpenXR registry dependency), never
#      standalone;
#   4. one XrHandTrackerEXT is created AND destroyed per hand - create/destroy
#      calls present and paired, teardown XR_NULL_HANDLE-guarded;
#   5. xrLocateHandJointsEXT is only ever called under the same focus/
#      session-running gate bz_quest_xr_actions.c's action sync already uses
#      - never an independent, potentially-unfocused call path;
#   6. the frame-critical bz_quest_hand_sample_build() allocates nothing,
#      locks nothing, and calls no log/file/bridge API (mirrors
#      test-quest-audio-rt-callback-safety.sh's RT-safety contract for the
#      AAudio callback);
#   7. the renderer wires create/sync/destroy for the hand-tracking module,
#      and destroy happens before the session/instance are torn down;
#   8. no hand-mesh rendering was added (XR_MSFT_hand_tracking_mesh/
#      XR_FB_hand_tracking_mesh are out of scope per this layer's task
#      contract - "do not add hand mesh rendering unless required");
#   9. the AndroidManifest.xml declares the permission/feature Quest's OS
#      requires to enumerate hand-tracking devices at all, with the feature
#      marked optional (required="false") - hand support must not be
#      required for startup.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

CMAKE=platform/android/quest/app/src/main/cpp/CMakeLists.txt
MANIFEST=platform/android/quest/app/src/main/AndroidManifest.xml
XR_C=platform/android/quest/app/src/main/cpp/bz_quest_xr.c
XR_H=platform/android/quest/app/src/main/cpp/bz_quest_xr.h
XR_HANDS_C=platform/android/quest/app/src/main/cpp/bz_quest_xr_hands.c
XR_HANDS_H=platform/android/quest/app/src/main/cpp/bz_quest_xr_hands.h
HAND_INPUT_C=platform/android/quest/app/src/main/cpp/bz_quest_hand_input.c
HAND_INPUT_H=platform/android/quest/app/src/main/cpp/bz_quest_hand_input.h
RENDERER_C=platform/android/quest/app/src/main/cpp/bz_quest_renderer.c
RENDERER_H=platform/android/quest/app/src/main/cpp/bz_quest_renderer.h
FAIL=0

for f in "$CMAKE" "$MANIFEST" "$XR_C" "$XR_H" "$XR_HANDS_C" "$XR_HANDS_H" "$HAND_INPUT_C" \
    "$HAND_INPUT_H" "$RENDERER_C" "$RENDERER_H"; do
    if [ ! -f "$f" ]; then
        echo "test-quest-hand-tracking-layout: expected file missing: $f" >&2
        exit 1
    fi
done

# (1) Build wiring.
for dep in 'bz_quest_hand_input.c' 'bz_quest_xr_hands.c'; do
    if ! grep -q "$dep" "$CMAKE"; then
        echo "test-quest-hand-tracking-layout: $CMAKE missing '$dep'" >&2
        FAIL=1
    fi
done

# (2) Optional capability negotiation: both extension names are probed, and
# the SAME function that hard-fails on a genuinely required extension
# missing must not do so for these two - i.e. no "return false" hard-coded
# immediately beside checking handCapability.extEnabled/aimExtEnabled. We
# check the positive contract instead: both extension-name macros appear in
# bz_quest_xr.c (probed), and bz_quest_xr_hands_create()'s unsupported path
# returns true (never false) when capability is absent.
if ! grep -q 'XR_EXT_HAND_TRACKING_EXTENSION_NAME' "$XR_C"; then
    echo "test-quest-hand-tracking-layout: $XR_C no longer probes XR_EXT_HAND_TRACKING_EXTENSION_NAME" >&2
    FAIL=1
fi
if ! grep -q 'XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME' "$XR_C"; then
    echo "test-quest-hand-tracking-layout: $XR_C no longer probes XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME" >&2
    FAIL=1
fi
if ! grep -q 'XrSystemHandTrackingPropertiesEXT' "$XR_C"; then
    echo "test-quest-hand-tracking-layout: $XR_C no longer queries XrSystemHandTrackingPropertiesEXT" >&2
    FAIL=1
fi
if ! grep -q 'bool bz_quest_xr_hands_create' "$XR_HANDS_C"; then
    echo "test-quest-hand-tracking-layout: could not find bz_quest_xr_hands_create in $XR_HANDS_C" >&2
    FAIL=1
else
    CREATE_BODY=$(awk '/^bool bz_quest_xr_hands_create/{found=1} found{print} found && /^}/{exit}' "$XR_HANDS_C")
    if ! printf '%s' "$CREATE_BODY" | grep -q 'handCapability.supported'; then
        echo "test-quest-hand-tracking-layout: bz_quest_xr_hands_create no longer gates on xr->handCapability.supported" >&2
        FAIL=1
    fi
    if ! printf '%s' "$CREATE_BODY" | grep -q 'return true; /\* optional capability'; then
        echo "test-quest-hand-tracking-layout: bz_quest_xr_hands_create's unsupported-capability path no longer returns true (hand tracking must never be a hard startup requirement)" >&2
        FAIL=1
    fi
fi

# (3) FB aim only ever requested alongside the base EXT extension.
if ! grep -q 'handCapability.aimExtEnabled =' "$XR_C" || \
   ! grep -A2 'handCapability.aimExtEnabled =' "$XR_C" | grep -q 'handCapability.extEnabled'; then
    echo "test-quest-hand-tracking-layout: $XR_C no longer gates aimExtEnabled on extEnabled (XR_FB_hand_tracking_aim's declared XR_EXT_hand_tracking dependency)" >&2
    FAIL=1
fi

# (4) One tracker created + destroyed per hand, paired, guarded.
if ! grep -q 'createHandTrackerEXT(xr->session' "$XR_HANDS_C"; then
    echo "test-quest-hand-tracking-layout: $XR_HANDS_C no longer calls xrCreateHandTrackerEXT" >&2
    FAIL=1
fi
if ! grep -q 'destroyHandTrackerEXT(hands->tracker\[h\])' "$XR_HANDS_C"; then
    echo "test-quest-hand-tracking-layout: $XR_HANDS_C no longer calls xrDestroyHandTrackerEXT" >&2
    FAIL=1
fi
if ! grep -q 'tracker\[h\] != XR_NULL_HANDLE && hands->destroyHandTrackerEXT' "$XR_HANDS_C"; then
    echo "test-quest-hand-tracking-layout: $XR_HANDS_C's tracker teardown is no longer XR_NULL_HANDLE-guarded" >&2
    FAIL=1
fi

# (5) Locate gated on focus/session-running, matching the action module.
if ! grep -q 'XR_SESSION_STATE_FOCUSED' "$XR_HANDS_C" || ! grep -q 'sessionRunning' "$XR_HANDS_C"; then
    echo "test-quest-hand-tracking-layout: $XR_HANDS_C no longer gates xrLocateHandJointsEXT on focus+sessionRunning" >&2
    FAIL=1
fi
LOCATE_CALLS=$(grep -c 'locateHandJointsEXT(hands->tracker\[h\]' "$XR_HANDS_C" || true)
if [ "$LOCATE_CALLS" -ne 1 ]; then
    echo "test-quest-hand-tracking-layout: expected exactly one xrLocateHandJointsEXT call site in $XR_HANDS_C, found $LOCATE_CALLS" >&2
    FAIL=1
fi

# (6) Frame-critical RT-safety for bz_quest_hand_sample_build() - similar
# technique to test-quest-audio-rt-callback-safety.sh, but anchored to a
# line that actually STARTS the function definition (column 0, a letter/
# underscore - this codebase's convention for every top-level declaration)
# rather than the bare substring match that script uses, which would
# otherwise latch onto this very file's own header-comment prose mention of
# the function name (verified while writing this check: an unanchored match
# grabbed the header comment down to the first unrelated static helper's
# closing brace, silently extracting zero lines of the real function and
# missing an injected malloc() entirely - see docs/quest-tabletop.md).
extract_fn() {
    file=$1
    name=$2
    awk -v fn="$name" '
        $0 ~ "^[A-Za-z_].*" fn "\\(" { found=1 }
        found { print }
        found && /^}/ { exit }
    ' "$file"
}
check_forbidden() {
    label=$1
    body=$2
    for pat in 'malloc(' 'calloc(' 'realloc(' 'free(' 'pthread_mutex_' \
               'BZ_QUEST_LOGE' 'BZ_QUEST_LOGW' 'BZ_QUEST_LOGI' \
               'fprintf(' 'printf(' 'fopen(' 'fread(' 'BZ_TT_' ; do
        if printf '%s' "$body" | grep -qF "$pat"; then
            echo "test-quest-hand-tracking-layout: $label calls forbidden '$pat' - bz_quest_hand_sample_build() must not allocate, lock, log, or touch files/bridge APIs (frame-critical, runs every frame on the XR render thread - see $HAND_INPUT_H's header comment)" >&2
            FAIL=1
        fi
    done
}
build_body=$(extract_fn "$HAND_INPUT_C" "bz_quest_hand_sample_build")
if [ -z "$build_body" ]; then
    echo "test-quest-hand-tracking-layout: could not find bz_quest_hand_sample_build in $HAND_INPUT_C (renamed/removed?)" >&2
    FAIL=1
else
    check_forbidden "bz_quest_hand_sample_build" "$build_body"
fi

# (7) Renderer wiring: create/sync/destroy all referenced, destroy ordered
# before the session/instance teardown (bz_quest_xr_destroy).
if ! grep -q 'bz_quest_xr_hands_create' "$RENDERER_C" \
    || ! grep -q 'bz_quest_xr_hands_sync' "$RENDERER_C" \
    || ! grep -q 'bz_quest_xr_hands_destroy' "$RENDERER_C"; then
    echo "test-quest-hand-tracking-layout: $RENDERER_C no longer wires create/sync/destroy for the hand-tracking module" >&2
    FAIL=1
fi
if ! grep -q 'bzQuestXrHands_t xrHands;' "$RENDERER_H"; then
    echo "test-quest-hand-tracking-layout: $RENDERER_H no longer owns the layer 8 xrHands member" >&2
    FAIL=1
fi
line_of() {
    grep -n "$1" "$RENDERER_C" | head -n1 | cut -d: -f1
}
HANDS_DESTROY=$(line_of 'bz_quest_xr_hands_destroy(&renderer->xrHands)')
XR_DESTROY=$(line_of 'bz_quest_xr_destroy(&renderer->xr)')
if [ -z "$HANDS_DESTROY" ] || [ -z "$XR_DESTROY" ]; then
    echo "test-quest-hand-tracking-layout: could not locate the hands/xr teardown call sites in $RENDERER_C" >&2
    FAIL=1
elif ! [ "$HANDS_DESTROY" -lt "$XR_DESTROY" ]; then
    echo "test-quest-hand-tracking-layout: $RENDERER_C must destroy hand trackers BEFORE the OpenXR session/instance" >&2
    FAIL=1
fi

# (8) No hand-mesh rendering - out of scope for this layer.
if grep -qi 'hand_tracking_mesh\|HandMeshMSFT\|HandTrackingMeshFB\|XrHandMesh' \
    "$XR_HANDS_C" "$XR_HANDS_H" "$HAND_INPUT_C" "$HAND_INPUT_H" 2>/dev/null; then
    echo "test-quest-hand-tracking-layout: hand-mesh extension/type referenced - hand mesh rendering is out of scope for this layer (task contract: 'Do not add hand mesh rendering unless required')" >&2
    FAIL=1
fi

# (9) Manifest: optional hand-tracking permission/feature.
if ! grep -q 'com.oculus.permission.HAND_TRACKING' "$MANIFEST"; then
    echo "test-quest-hand-tracking-layout: $MANIFEST missing com.oculus.permission.HAND_TRACKING" >&2
    FAIL=1
fi
if ! grep -q 'oculus.software.handtracking' "$MANIFEST"; then
    echo "test-quest-hand-tracking-layout: $MANIFEST missing the oculus.software.handtracking uses-feature" >&2
    FAIL=1
fi
if ! grep -A2 'oculus.software.handtracking' "$MANIFEST" | grep -q 'android:required="false"'; then
    echo "test-quest-hand-tracking-layout: oculus.software.handtracking uses-feature must be android:required=\"false\" (hand support must not be required for startup)" >&2
    FAIL=1
fi

# Pure-module boundary: no OpenXR/Vulkan type may appear in real code in the
# pure header/impl (mirrors bz_quest_input_state.h's own discipline this
# layer reuses) - comments are stripped first since both files' header
# comments legitimately DISCUSS OpenXR type names in prose (e.g. explaining
# what bz_quest_xr_hands.c unpacks) without ever using them as a real type.
strip_c_comments() {
    awk '
    BEGIN { in_comment = 0 }
    {
        line = $0
        out = ""
        i = 1
        n = length(line)
        while (i <= n) {
            if (!in_comment && substr(line, i, 2) == "/*") { in_comment = 1; i += 2; continue }
            if (in_comment && substr(line, i, 2) == "*/") { in_comment = 0; i += 2; continue }
            if (!in_comment) out = out substr(line, i, 1)
            i++
        }
        print out
    }' "$1"
}
hand_input_code=$(strip_c_comments "$HAND_INPUT_H"; strip_c_comments "$HAND_INPUT_C")
if printf '%s\n' "$hand_input_code" | grep -qE '\bXr[A-Z]|\bVk[A-Z]'; then
    echo "test-quest-hand-tracking-layout: an OpenXR/Vulkan type leaked into real code in the pure $HAND_INPUT_H/$HAND_INPUT_C - these must stay plain C POD only" >&2
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

echo "test-quest-hand-tracking-layout: OK"
