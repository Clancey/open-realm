#!/bin/sh
# platform/android/quest/scripts/test-wc3-bone-palette-layout.sh
#
# Structurally guards the layer-5C GPU-skinning bone-palette contract
# documented in bz_quest_vk_wc3.h's paletteBuffer/paletteSlotStride/
# BZ_QUEST_VK_WC3_PALETTE_SLOT_COUNT comments and bz_quest_wc3_render.h's
# bzQuestWc3Vertex_t comment. No NDK/Gradle/Vulkan device is available in
# this host environment to actually create the buffer/descriptor set and
# query real device limits, so this script instead greps the real source
# for the specific structural properties those runtime calls depend on -
# the same technique as this directory's own
# test-wc3-descriptor-pool-headroom.sh, applied to a different real bug
# class:
#
#   1. paletteSlotStride must be computed from the DEVICE's own
#      minUniformBufferOffsetAlignment (never a hardcoded stride) and
#      checked against the device's own maxUniformBufferRange before use -
#      a device with a larger-than-expected alignment or a smaller-than-
#      expected uniform-buffer-range limit must fail loudly at create()
#      time, not silently corrupt every dynamic-offset bind thereafter.
#   2. BZ_QUEST_VK_WC3_PALETTE_SLOT_COUNT must stay defined as
#      BZ_QUEST_WC3_MAX_SKINNED_DRAWS_PER_FRAME + 1 (the "+1" is the
#      permanently-reserved identity slot 0 - see that constant's own doc
#      comment) - a future edit collapsing this back to the bare per-frame
#      budget would leave no identity fallback slot for static models/
#      budget overflow, corrupting every such draw's bind offset.
#   3. The bone-index/bone-weight vertex attributes must keep the exact
#      unnormalized-uint/normalized-unorm split this slice's GPU skinning
#      formula requires (see warcraft_vert.vert's header comment and
#      bz_quest_vk_wc3.c's create_pipeline_variant() attrs[] comment,
#      transcribed from renderer/r_buffer.c:87-88) - accidentally
#      normalizing the bone INDEX (or leaving the bone WEIGHT
#      unnormalized) would silently corrupt every skinned vertex's bone
#      selection/weighting on-device with no host-visible symptom.
#   4. The palette descriptor must stay VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_
#      DYNAMIC (not a plain UNIFORM_BUFFER) - this is what lets one
#      descriptor set serve every draw via a per-draw dynamic offset
#      instead of needing one set per skinned draw.
#   5. The anim-arena ownership-transfer contract (model_ready_cb() frees
#      model->meta.anim on every non-success path; model_cache_destroy()
#      frees it on eviction/shutdown) must not regress back to the leak
#      this slice fixed - see bz_quest_vk_wc3.c's model_ready_cb() doc
#      comment.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

VK_WC3_H=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3.h
VK_WC3_C=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3.c
RENDER_H=platform/android/quest/app/src/main/cpp/bz_quest_wc3_render.h
VERT=platform/android/quest/app/src/main/cpp/shaders/warcraft_vert.vert
FAIL=0

for f in "$VK_WC3_H" "$VK_WC3_C" "$RENDER_H" "$VERT"; do
    if [ ! -f "$f" ]; then
        echo "test-wc3-bone-palette-layout: expected source file missing: $f" >&2
        exit 1
    fi
done

# 1. paletteSlotStride must be derived from device limits, never hardcoded.
if ! grep -q 'vkGetPhysicalDeviceProperties(out->vk->physicalDevice, &props);' "$VK_WC3_C"; then
    echo "test-wc3-bone-palette-layout: $VK_WC3_C's create_palette_resources() no longer queries vkGetPhysicalDeviceProperties (paletteSlotStride must be device-limit-derived, not hardcoded)" >&2
    FAIL=1
fi
if ! grep -q 'props.limits.minUniformBufferOffsetAlignment' "$VK_WC3_C"; then
    echo "test-wc3-bone-palette-layout: $VK_WC3_C no longer aligns paletteSlotStride to minUniformBufferOffsetAlignment" >&2
    FAIL=1
fi
if ! grep -q 'slotStride > props.limits.maxUniformBufferRange' "$VK_WC3_C"; then
    echo "test-wc3-bone-palette-layout: $VK_WC3_C no longer checks the aligned palette slot stride against maxUniformBufferRange" >&2
    FAIL=1
fi

# 2. BZ_QUEST_VK_WC3_PALETTE_SLOT_COUNT must keep its "+1 identity slot"
#    definition.
if ! grep -q 'BZ_QUEST_VK_WC3_PALETTE_SLOT_COUNT = BZ_QUEST_WC3_MAX_SKINNED_DRAWS_PER_FRAME + 1' "$VK_WC3_H"; then
    echo "test-wc3-bone-palette-layout: $VK_WC3_H no longer defines BZ_QUEST_VK_WC3_PALETTE_SLOT_COUNT as BZ_QUEST_WC3_MAX_SKINNED_DRAWS_PER_FRAME + 1" >&2
    FAIL=1
fi

# 3. Bone index/weight vertex attribute formats must keep the exact
#    unnormalized-uint / normalized-unorm split the skinning formula needs.
if ! grep -q 'VK_FORMAT_R8G8B8A8_UINT, offsetof(bzQuestWc3Vertex_t, boneIndex)' "$VK_WC3_C"; then
    echo "test-wc3-bone-palette-layout: $VK_WC3_C's boneIndex vertex attribute is no longer VK_FORMAT_R8G8B8A8_UINT (must stay unnormalized - see renderer/r_buffer.c's i_skin1 GL_FALSE)" >&2
    FAIL=1
fi
if ! grep -q 'VK_FORMAT_R8G8B8A8_UNORM, offsetof(bzQuestWc3Vertex_t, boneWeight)' "$VK_WC3_C"; then
    echo "test-wc3-bone-palette-layout: $VK_WC3_C's boneWeight vertex attribute is no longer VK_FORMAT_R8G8B8A8_UNORM (must stay normalized - see renderer/r_buffer.c's i_boneWeight1 GL_TRUE)" >&2
    FAIL=1
fi
if ! grep -q 'uint8_t boneIndex\[4\];' "$RENDER_H" || ! grep -q 'uint8_t boneWeight\[4\];' "$RENDER_H"; then
    echo "test-wc3-bone-palette-layout: $RENDER_H's bzQuestWc3Vertex_t no longer declares boneIndex/boneWeight as uint8_t[4]" >&2
    FAIL=1
fi

# 4. The palette descriptor type must stay DYNAMIC (one set reused every
#    draw via a per-draw offset, not one set per skinned draw).
if ! grep -q 'binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;' "$VK_WC3_C"; then
    echo "test-wc3-bone-palette-layout: $VK_WC3_C's palette descriptor set layout binding is no longer VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC" >&2
    FAIL=1
fi

# 5. Anim-arena ownership-transfer contract: model_ready_cb() must free
#    model->meta.anim on every non-success path, and model_cache_destroy()
#    must free it on eviction/shutdown - both via
#    bz_quest_wc3_model_anim_free(), never a bare free()/leaked pointer.
if [ "$(grep -c 'bz_quest_wc3_model_anim_free((bzQuestWc3ModelAnim_t \*)model->meta.anim);' "$VK_WC3_C")" -lt 3 ]; then
    echo "test-wc3-bone-palette-layout: $VK_WC3_C's model_ready_cb() no longer frees model->meta.anim via bz_quest_wc3_model_anim_free() on all of its early-return paths (anim-arena leak regression - expected at least 3 call sites)" >&2
    FAIL=1
fi
if ! grep -q 'bz_quest_wc3_model_anim_free((bzQuestWc3ModelAnim_t \*)m->meta.anim);' "$VK_WC3_C"; then
    echo "test-wc3-bone-palette-layout: $VK_WC3_C's model_cache_destroy() no longer frees the cached model's anim arena via bz_quest_wc3_model_anim_free() (anim-arena leak regression on eviction/shutdown)" >&2
    FAIL=1
fi

# 6. Vertex shader must declare the bone-palette UBO at set 1 binding 0 and
#    consume both new vertex inputs - a partial revert (e.g. UBO added but
#    inputs unused, or vice versa) would compile but silently skin nothing.
if ! grep -q 'layout(set = 1, binding = 0) uniform BonePalette' "$VERT"; then
    echo "test-wc3-bone-palette-layout: $VERT no longer declares the bone-palette UBO at set 1 binding 0" >&2
    FAIL=1
fi
if ! grep -q 'layout(location = 2) in uvec4 inBoneIndex;' "$VERT" || ! grep -q 'layout(location = 3) in vec4 inBoneWeight;' "$VERT"; then
    echo "test-wc3-bone-palette-layout: $VERT no longer declares inBoneIndex (uvec4, location 2) / inBoneWeight (vec4, location 3)" >&2
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "test-wc3-bone-palette-layout: OK (bone-palette UBO layout, vertex attribute formats, and anim-arena ownership contract all intact)"
