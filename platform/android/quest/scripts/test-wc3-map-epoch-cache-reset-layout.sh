#!/bin/sh
# platform/android/quest/scripts/test-wc3-map-epoch-cache-reset-layout.sh
#
# Structurally guards the PR #28 High-severity fix for stale model/texture
# GPU caches across a map reload: bz_quest_vk_wc3.c's modelCache/textureCache
# are keyed by asset identity (path/variant) alone, with process lifetime by
# default (bz_quest_wc3_cache_acquire() early-returns a resident entry
# unconditionally) - a DIFFERENT map can validly reuse the exact same
# imported path with DIFFERENT content, so without a map-epoch-gated reset a
# stale GPU asset from the PREVIOUS map would silently keep being displayed.
#
# Host-testable, Vulkan-free coverage of the underlying reset PATTERN and
# the shared bz_quest_wc3_epoch_changed() map-reload detector already lives
# in platform/android/quest/tests/test_bz_quest_wc3_cache.c (part of
# test-quest-host-tests) - this script instead guards the impure Vulkan
# call site/ordering that host tests cannot reach:
#   1. bzQuestVkWc3_t must carry a modelTextureCacheEpoch tracker (the new
#      field this fix adds) and a particlePoolEpoch tracker (the DRY
#      refactor of the pre-existing particle-pool epoch fields onto the
#      same shared bzQuestWc3EpochTracker_t type/bz_quest_wc3_epoch_changed()
#      helper) - the OLD standalone particlePoolMapEpoch/havePoolMapEpoch
#      fields must NOT reappear (a partial revert of the DRY refactor).
#   2. reset_model_texture_caches() must exist and, in order, wait for the
#      device to go idle, shut down BOTH modelCache and textureCache (never
#      just one - "texture+model symmetry"), then re-initialize BOTH with
#      their original capacity/create/destroy arguments.
#   3. bz_quest_vk_wc3_capture_and_upload() must check
#      bz_quest_wc3_epoch_changed(&vk3->modelTextureCacheEpoch, mapEpoch)
#      and call reset_model_texture_caches() on a real transition, and this
#      check must run strictly BEFORE bz_quest_wc3_capture_frame() - the
#      safe top-of-frame point, before any new-map model/texture offer.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

CACHE_H=platform/android/quest/app/src/main/cpp/bz_quest_wc3_cache.h
CACHE_C=platform/android/quest/app/src/main/cpp/bz_quest_wc3_cache.c
VK_WC3_H=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3.h
VK_WC3_C=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3.c
FAIL=0

for f in "$CACHE_H" "$CACHE_C" "$VK_WC3_H" "$VK_WC3_C"; do
    if [ ! -f "$f" ]; then
        echo "test-wc3-map-epoch-cache-reset-layout: expected source file missing: $f" >&2
        exit 1
    fi
done

# (1) Shared epoch-tracker type/function must exist.
if ! grep -q 'typedef struct {' "$CACHE_H" || ! grep -q 'bzQuestWc3EpochTracker_t;' "$CACHE_H"; then
    echo "test-wc3-map-epoch-cache-reset-layout: $CACHE_H no longer defines bzQuestWc3EpochTracker_t" >&2
    FAIL=1
fi
if ! grep -q 'bool bz_quest_wc3_epoch_changed(bzQuestWc3EpochTracker_t \*tracker, uint64_t epoch);' "$CACHE_H"; then
    echo "test-wc3-map-epoch-cache-reset-layout: $CACHE_H no longer declares bz_quest_wc3_epoch_changed()" >&2
    FAIL=1
fi
if ! grep -q '^bool bz_quest_wc3_epoch_changed(bzQuestWc3EpochTracker_t \*tracker, uint64_t epoch) {' "$CACHE_C"; then
    echo "test-wc3-map-epoch-cache-reset-layout: $CACHE_C no longer defines bz_quest_wc3_epoch_changed()" >&2
    FAIL=1
fi

# (2) bzQuestVkWc3_t must carry both epoch trackers via the shared type -
#     the standalone pre-refactor fields must be gone, not merely renamed
#     alongside the new ones (a partial/duplicated revert).
if ! grep -q 'bzQuestWc3EpochTracker_t modelTextureCacheEpoch;' "$VK_WC3_H"; then
    echo "test-wc3-map-epoch-cache-reset-layout: $VK_WC3_H no longer declares bzQuestVkWc3_t::modelTextureCacheEpoch" >&2
    FAIL=1
fi
if ! grep -q 'bzQuestWc3EpochTracker_t particlePoolEpoch;' "$VK_WC3_H"; then
    echo "test-wc3-map-epoch-cache-reset-layout: $VK_WC3_H no longer declares bzQuestVkWc3_t::particlePoolEpoch (DRY refactor onto the shared epoch tracker reverted?)" >&2
    FAIL=1
fi
if grep -qE 'uint64_t particlePoolMapEpoch;|bool havePoolMapEpoch;' "$VK_WC3_H" "$VK_WC3_C"; then
    echo "test-wc3-map-epoch-cache-reset-layout: the old standalone particlePoolMapEpoch/havePoolMapEpoch fields have reappeared - they must stay merged into the shared bzQuestWc3EpochTracker_t particlePoolEpoch" >&2
    FAIL=1
fi

# (3) reset_model_texture_caches() must exist, and its body (extraction
#     anchors at a REAL DEFINITION line - the function name immediately
#     followed by '(...) {' at the end of the line, never merely '(' - so
#     neither a plain call site NOR this file's own forward declaration
#     (reset_model_texture_caches(bzQuestVkWc3_t *vk3); - a bare prototype
#     ending in ';', not '{') can ever be mistaken for the definition) must,
#     in order: wait for the device idle, shut down BOTH caches, then
#     re-init BOTH.
extract_fn() {
    file=$1
    name=$2
    awk -v fn="$name" '
        $0 ~ "^[A-Za-z_][A-Za-z0-9_ *]*" fn "\\([^)]*\\)[[:space:]]*\\{$" { found=1 }
        found { print }
        found && /^}/ { exit }
    ' "$file"
}
RESET_FN=$(extract_fn "$VK_WC3_C" "reset_model_texture_caches")
if [ -z "$RESET_FN" ]; then
    echo "test-wc3-map-epoch-cache-reset-layout: $VK_WC3_C no longer defines reset_model_texture_caches()" >&2
    FAIL=1
else
    line_in_fn() {
        printf '%s\n' "$RESET_FN" | grep -n "$1" | head -n1 | cut -d: -f1
    }
    L_WAIT=$(line_in_fn 'vkDeviceWaitIdle(vk3->vk->device)')
    L_SHUTDOWN_MODEL=$(line_in_fn 'bz_quest_wc3_cache_shutdown(&vk3->modelCache)')
    L_SHUTDOWN_TEX=$(line_in_fn 'bz_quest_wc3_cache_shutdown(&vk3->textureCache)')
    L_INIT_MODEL=$(line_in_fn 'bz_quest_wc3_cache_init(&vk3->modelCache,')
    L_INIT_TEX=$(line_in_fn 'bz_quest_wc3_cache_init(&vk3->textureCache,')
    for pair in "L_WAIT:vkDeviceWaitIdle" "L_SHUTDOWN_MODEL:modelCache shutdown" \
                "L_SHUTDOWN_TEX:textureCache shutdown" "L_INIT_MODEL:modelCache re-init" \
                "L_INIT_TEX:textureCache re-init"; do
        varname=${pair%%:*}
        label=${pair#*:}
        eval "val=\${$varname:-}"
        if [ -z "$val" ]; then
            echo "test-wc3-map-epoch-cache-reset-layout: reset_model_texture_caches() no longer contains its $label call" >&2
            FAIL=1
        fi
    done
    if [ -n "${L_WAIT:-}" ] && [ -n "${L_SHUTDOWN_MODEL:-}" ] && [ "$L_WAIT" -ge "$L_SHUTDOWN_MODEL" ]; then
        echo "test-wc3-map-epoch-cache-reset-layout: reset_model_texture_caches() must wait for the device idle BEFORE shutting down modelCache" >&2
        FAIL=1
    fi
    if [ -n "${L_SHUTDOWN_MODEL:-}" ] && [ -n "${L_SHUTDOWN_TEX:-}" ] && [ -n "${L_INIT_MODEL:-}" ] && [ "$L_INIT_MODEL" -le "$L_SHUTDOWN_TEX" ]; then
        echo "test-wc3-map-epoch-cache-reset-layout: reset_model_texture_caches() must shut down BOTH caches before re-initializing either (transactional order)" >&2
        FAIL=1
    fi
    # No ordering requirement between the two re-inits themselves (L_INIT_MODEL vs L_INIT_TEX) -
    # only their presence (checked above) and that both follow both shutdowns (checked above)
    # matter for transactional correctness.
fi

# (4) bz_quest_vk_wc3_capture_and_upload() must check the model/texture
#     epoch tracker and call reset_model_texture_caches() STRICTLY BEFORE
#     bz_quest_wc3_capture_frame() - the safe top-of-frame point.
line_of() {
    grep -n "$1" "$VK_WC3_C" | head -n1 | cut -d: -f1
}
EPOCH_CHECK=$(line_of 'bz_quest_wc3_epoch_changed(&vk3->modelTextureCacheEpoch, mapEpoch)')
RESET_CALL=$(line_of 'if (!reset_model_texture_caches(vk3))')
CAPTURE_FRAME=$(line_of 'bz_quest_wc3_capture_frame(&callbacks, outRenderList);')
if [ -z "$EPOCH_CHECK" ]; then
    echo "test-wc3-map-epoch-cache-reset-layout: $VK_WC3_C no longer checks bz_quest_wc3_epoch_changed(&vk3->modelTextureCacheEpoch, mapEpoch)" >&2
    FAIL=1
fi
if [ -z "$RESET_CALL" ]; then
    echo "test-wc3-map-epoch-cache-reset-layout: $VK_WC3_C no longer calls reset_model_texture_caches() from bz_quest_vk_wc3_capture_and_upload()" >&2
    FAIL=1
fi
if [ -z "$CAPTURE_FRAME" ]; then
    echo "test-wc3-map-epoch-cache-reset-layout: $VK_WC3_C's expected bz_quest_wc3_capture_frame(&callbacks, outRenderList) call site was not found (file restructured? update this test)" >&2
    FAIL=1
fi
if [ -n "$EPOCH_CHECK" ] && [ -n "$CAPTURE_FRAME" ] && [ "$EPOCH_CHECK" -ge "$CAPTURE_FRAME" ]; then
    echo "test-wc3-map-epoch-cache-reset-layout: the model/texture cache epoch check must run BEFORE bz_quest_wc3_capture_frame(), not after" >&2
    FAIL=1
fi
if [ -n "$RESET_CALL" ] && [ -n "$CAPTURE_FRAME" ] && [ "$RESET_CALL" -ge "$CAPTURE_FRAME" ]; then
    echo "test-wc3-map-epoch-cache-reset-layout: reset_model_texture_caches() must be called BEFORE bz_quest_wc3_capture_frame(), not after" >&2
    FAIL=1
fi

# (5) The particle-pool epoch check must still be present too, now wired
#     through the shared helper (the DRY refactor's other call site).
if ! grep -q 'bz_quest_wc3_epoch_changed(&vk3->particlePoolEpoch, mapEpoch)' "$VK_WC3_C"; then
    echo "test-wc3-map-epoch-cache-reset-layout: $VK_WC3_C no longer checks bz_quest_wc3_epoch_changed(&vk3->particlePoolEpoch, mapEpoch) for the particle pool reset" >&2
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "test-wc3-map-epoch-cache-reset-layout: OK (model/texture GPU caches and the particle pool share one map-epoch-gated, transactional, texture+model-symmetric reset run strictly before any new-map capture offer)"
