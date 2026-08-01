#!/bin/sh
# platform/android/quest/scripts/test-wc3-pointer-layout.sh
#
# Structurally guards layer 6's Touch-controller ray/reticle GPU contract
# where host-unit tests cannot reach (bz_quest_input_state.h/.c's pure
# state-machine/board/hit-priority logic and bz_quest_xr_bindings.h/.c's pure
# binding tables ARE covered by test-quest-host-tests - this script only
# covers the impure Vulkan pointer module, the impure OpenXR action module,
# and their renderer wiring):
#   1. the Quest-native build must compile the new bz_quest_vk_wc3_pointer.c,
#      bz_quest_xr_actions.c, bz_quest_xr_bindings.c and bz_quest_input_state.c
#      modules at all;
#   2. the pointer module must REUSE the existing warcraft_marker shader pair
#      (position-only vertex + mvp/tint push constant) - layer 6 introduces NO
#      new shader, so it must not reference any *_pointer_*.vert/.frag;
#   3. the pointer pipeline must stay depth-tested but depth-write-disabled
#      (a beam is occluded by nearer board geometry yet never itself occludes
#      later draws - same rationale as layer 5D/5E), VK_CULL_MODE_NONE (a beam
#      quad is viewed from any side) and VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
#   4. the pointer pipeline must use straight-alpha blending
#      (SRC_ALPHA/ONE_MINUS_SRC_ALPHA) so rays read correctly over AR
#      passthrough (matches the fog markers / HUD panels);
#   5. the shared eye-pass must record the pointer LAST (after the HUD), in
#      the PLAIN per-eye view*projection (physical controllers live in
#      tracking space, not the board-folded composed space);
#   6. the renderer must fold the board transform into the world/HUD draws
#      (mvpBoard = mvp * board) but draw the pointer with the plain mvp;
#   7. the OpenXR action module must sync actions exactly once per frame and
#      only while the session is focused (no wasteful/undefined sync while
#      unfocused - OpenXR spec).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

CMAKE=platform/android/quest/app/src/main/cpp/CMakeLists.txt
RENDERER=platform/android/quest/app/src/main/cpp/bz_quest_renderer.c
RENDERER_H=platform/android/quest/app/src/main/cpp/bz_quest_renderer.h
VK_POINTER_C=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3_pointer.c
XR_ACTIONS_C=platform/android/quest/app/src/main/cpp/bz_quest_xr_actions.c
INPUT_STATE_C=platform/android/quest/app/src/main/cpp/bz_quest_input_state.c
XR_BINDINGS_C=platform/android/quest/app/src/main/cpp/bz_quest_xr_bindings.c
FAIL=0

for f in "$CMAKE" "$RENDERER" "$RENDERER_H" "$VK_POINTER_C" "$XR_ACTIONS_C" "$INPUT_STATE_C" \
    "$XR_BINDINGS_C"; do
    if [ ! -f "$f" ]; then
        echo "test-wc3-pointer-layout: expected file missing: $f" >&2
        exit 1
    fi
done

for dep in \
    'bz_quest_vk_wc3_pointer.c' \
    'bz_quest_xr_actions.c' \
    'bz_quest_xr_bindings.c' \
    'bz_quest_input_state.c'
do
    if ! grep -q "$dep" "$CMAKE"; then
        echo "test-wc3-pointer-layout: $CMAKE missing '$dep'" >&2
        FAIL=1
    fi
done

# (2) Reuse the marker shaders; introduce no pointer shader.
if ! grep -q 'g_bz_quest_warcraft_marker_vert_spv' "$VK_POINTER_C" \
    || ! grep -q 'g_bz_quest_warcraft_marker_frag_spv' "$VK_POINTER_C"; then
    echo "test-wc3-pointer-layout: $VK_POINTER_C no longer reuses the warcraft_marker shader pair" >&2
    FAIL=1
fi
if ls platform/android/quest/app/src/main/cpp/shaders/*pointer* >/dev/null 2>&1; then
    echo "test-wc3-pointer-layout: layer 6 must introduce no new pointer shader (found one under shaders/)" >&2
    FAIL=1
fi

# (3) Pipeline state.
if ! grep -q 'depthStencil.depthTestEnable = VK_TRUE;' "$VK_POINTER_C" \
    || ! grep -q 'depthStencil.depthWriteEnable = VK_FALSE;' "$VK_POINTER_C"; then
    echo "test-wc3-pointer-layout: $VK_POINTER_C no longer keeps the pointer pipeline depth-tested and depth-write-disabled" >&2
    FAIL=1
fi
if ! grep -q 'rasterization.cullMode = VK_CULL_MODE_NONE;' "$VK_POINTER_C"; then
    echo "test-wc3-pointer-layout: $VK_POINTER_C no longer keeps the pointer pipeline at VK_CULL_MODE_NONE" >&2
    FAIL=1
fi
if ! grep -q 'inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;' "$VK_POINTER_C"; then
    echo "test-wc3-pointer-layout: $VK_POINTER_C no longer keeps the pointer pipeline at VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST" >&2
    FAIL=1
fi

# (4) Straight-alpha blend over passthrough.
if ! grep -q 'blendAttachment.srcColorBlendFactor = blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;' "$VK_POINTER_C"; then
    echo "test-wc3-pointer-layout: $VK_POINTER_C no longer uses SRC_ALPHA source blend for the pointer" >&2
    FAIL=1
fi

# (5)/(6) Renderer wiring: pointer recorded after the HUD, with the plain mvp.
if ! grep -q 'bz_quest_vk_wc3_pointer_create' "$RENDERER" \
    || ! grep -q 'bz_quest_vk_wc3_pointer_update' "$RENDERER" \
    || ! grep -q 'bz_quest_vk_wc3_pointer_record' "$RENDERER" \
    || ! grep -q 'bz_quest_vk_wc3_pointer_destroy' "$RENDERER"; then
    echo "test-wc3-pointer-layout: $RENDERER no longer wires create/update/record/destroy for the pointer module" >&2
    FAIL=1
fi
if ! grep -q 'bzQuestVkWc3Pointer_t wc3Pointer;' "$RENDERER_H" \
    || ! grep -q 'bzQuestXrActions_t xrActions;' "$RENDERER_H" \
    || ! grep -q 'bzQuestInputState_t inputState;' "$RENDERER_H"; then
    echo "test-wc3-pointer-layout: $RENDERER_H no longer owns the layer 6 pointer/xrActions/inputState members" >&2
    FAIL=1
fi
if ! grep -q 'bz_quest_mat4_multiply(mvp, boardMatrix, mvpBoard);' "$RENDERER"; then
    echo "test-wc3-pointer-layout: $RENDERER no longer folds the board transform into the world view*projection (mvpBoard = mvp * board)" >&2
    FAIL=1
fi

line_of() {
    grep -n "$1" "$RENDERER" | head -n1 | cut -d: -f1
}
HUD_RECORD=$(line_of 'bz_quest_vk_wc3_hud_record(&renderer->wc3Hud')
POINTER_RECORD=$(line_of 'bz_quest_vk_wc3_pointer_record(&renderer->wc3Pointer')
if [ -z "$HUD_RECORD" ] || [ -z "$POINTER_RECORD" ]; then
    echo "test-wc3-pointer-layout: could not locate the HUD/pointer record call sites in $RENDERER" >&2
    FAIL=1
elif ! [ "$HUD_RECORD" -lt "$POINTER_RECORD" ]; then
    echo "test-wc3-pointer-layout: $RENDERER must record the pointer AFTER the HUD (pointer last)" >&2
    FAIL=1
fi

# (7) Action sync gated on focus, exactly one xrSyncActions call site.
SYNC_COUNT=$(grep -c 'xrSyncActions(' "$XR_ACTIONS_C" || true)
if [ "$SYNC_COUNT" -ne 1 ]; then
    echo "test-wc3-pointer-layout: $XR_ACTIONS_C must call xrSyncActions exactly once (found $SYNC_COUNT)" >&2
    FAIL=1
fi
if ! grep -q 'XR_SESSION_STATE_FOCUSED' "$XR_ACTIONS_C"; then
    echo "test-wc3-pointer-layout: $XR_ACTIONS_C no longer gates action sync on XR_SESSION_STATE_FOCUSED" >&2
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

echo "test-wc3-pointer-layout: OK"
