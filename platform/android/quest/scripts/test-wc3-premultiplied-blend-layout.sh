#!/bin/sh
# platform/android/quest/scripts/test-wc3-premultiplied-blend-layout.sh
#
# Structurally guards the premultiplied-compositing contract fixed for a
# High-severity reviewer finding on PR #28 (see bz_quest_vk.h's
# bz_quest_vk_straight_over_blend_state() and bz_quest_vk_wc3.c's
# bz_quest_vk_wc3_blend_state_for_mode() doc comments for the full
# derivation) where host-unit tests cannot reach (the pure blend-equation
# MATH itself is covered by test_bz_quest_pure.c's test_blend_* functions -
# this script instead guards that the real Vulkan pipeline-creation source
# actually uses the exact factor/flag values that math was verified
# against, and that no pipeline reverted to the fixed defect):
#   1. the XR projection layer must request PREMULTIPLIED alpha
#      (unpremultipliedAlpha=false) - the exact call site fixed;
#   2. bz_quest_vk_wc3_blend_state_for_mode() (models + particles, 7
#      blend modes) must use the corrected, non-mirrored alpha-coverage
#      factor pair for every mode whose color src factor is SRC_ALPHA/
#      DST_COLOR (ALPHA/ADD_ALPHA/MODULATE/MODULATE_2X) - never re-mirroring
#      the color factor onto alpha (the a-squared/coverage-erosion defect);
#   3. bz_quest_vk_straight_over_blend_state() (bz_quest_vk.c) - the one
#      shared "over" blend state every non-blend-mode-keyed WC3 overlay
#      pipeline uses - must have the same corrected, non-mirrored alpha
#      factor pair;
#   4. every one of the 6 pipelines that used to duplicate that inline
#      blend state (terrain water, fog overlay, selection markers, HUD
#      panel, HUD text, ray pointer) must call the shared helper instead -
#      no reintroduced duplicate literal;
#   5. no file anywhere under the Quest native source tree still mirrors
#      color factors onto the alpha channel via a literal `srcAlphaBlendFactor
#      = ... SRC_ALPHA` assignment (the exact defect pattern) outside the
#      one documented, deliberately-unchanged additive-family exception;
#   6. the 3 opaque/alpha-tested shader coverage-forcing sites (model
#      layers, particles, terrain ground/cliff) exist on both the CPU
#      (push-constant population) and shader (GLSL branch) sides.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

CPP_DIR=platform/android/quest/app/src/main/cpp
SHADER_DIR="$CPP_DIR/shaders"
RENDERER="$CPP_DIR/bz_quest_renderer.c"
VK_H="$CPP_DIR/bz_quest_vk.h"
VK_C="$CPP_DIR/bz_quest_vk.c"
VK_WC3_C="$CPP_DIR/bz_quest_vk_wc3.c"
VK_WC3_PARTICLES_C="$CPP_DIR/bz_quest_vk_wc3_particles.c"
VK_TERRAIN_C="$CPP_DIR/bz_quest_vk_wc3_terrain.c"
VK_FOG_C="$CPP_DIR/bz_quest_vk_wc3_fog.c"
VK_HUD_C="$CPP_DIR/bz_quest_vk_wc3_hud.c"
VK_POINTER_C="$CPP_DIR/bz_quest_vk_wc3_pointer.c"
FAIL=0

for f in "$RENDERER" "$VK_H" "$VK_C" "$VK_WC3_C" "$VK_WC3_PARTICLES_C" "$VK_TERRAIN_C" "$VK_FOG_C" \
    "$VK_HUD_C" "$VK_POINTER_C" "$SHADER_DIR/warcraft_frag.frag" "$SHADER_DIR/warcraft_particle_frag.frag" \
    "$SHADER_DIR/terrain_frag.frag"; do
    if [ ! -f "$f" ]; then
        echo "test-wc3-premultiplied-blend-layout: expected file missing: $f" >&2
        exit 1
    fi
done

# (1) XR projection layer requests premultiplied alpha.
if ! grep -q 'bz_quest_projection_layer_flags(/\*unpremultipliedAlpha=\*/false)' "$RENDERER"; then
    echo "test-wc3-premultiplied-blend-layout: $RENDERER no longer requests premultiplied alpha (unpremultipliedAlpha=false) for the XR projection layer" >&2
    FAIL=1
fi
if grep -q 'bz_quest_projection_layer_flags(/\*unpremultipliedAlpha=\*/true)' "$RENDERER"; then
    echo "test-wc3-premultiplied-blend-layout: $RENDERER still requests UNPREMULTIPLIED alpha somewhere - the render target is premultiplied end to end, this must never be true" >&2
    FAIL=1
fi

# (2) bz_quest_vk_wc3_blend_state_for_mode()'s per-mode alpha-coverage factors.
extract_fn() {
    file=$1
    name=$2
    awk -v fn="$name" '
        $0 ~ "^[A-Za-z_][A-Za-z0-9_ *]*" fn "\\(" { found=1 }
        found { print }
        found && /^}/ { exit }
    ' "$file"
}

blend_mode_body=$(extract_fn "$VK_WC3_C" "bz_quest_vk_wc3_blend_state_for_mode")
if [ -z "$blend_mode_body" ]; then
    echo "test-wc3-premultiplied-blend-layout: could not find bz_quest_vk_wc3_blend_state_for_mode in $VK_WC3_C (renamed/removed?)" >&2
    FAIL=1
else
    for tok in \
        'outBlend->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;' \
        'outBlend->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;' \
        'outBlend->srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;' \
        'outBlend->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;'
    do
        if ! printf '%s' "$blend_mode_body" | grep -qF "$tok"; then
            echo "test-wc3-premultiplied-blend-layout: bz_quest_vk_wc3_blend_state_for_mode() ($VK_WC3_C) missing expected corrected alpha-coverage token: $tok" >&2
            FAIL=1
        fi
    done
    # The one legitimate exception: ADDITIVE mirrors color/alpha onto ONE/ONE (never SRC_ALPHA) -
    # confirm the defect pattern itself (srcAlphaBlendFactor mirrored onto a SRC_ALPHA/DST_COLOR/
    # SRC_COLOR color factor) is nowhere in this function.
    if printf '%s' "$blend_mode_body" | grep -qE 'srcColorBlendFactor = outBlend->srcAlphaBlendFactor = VK_BLEND_FACTOR_(SRC_ALPHA|DST_COLOR)'; then
        echo "test-wc3-premultiplied-blend-layout: bz_quest_vk_wc3_blend_state_for_mode() ($VK_WC3_C) still mirrors a SRC_ALPHA/DST_COLOR color factor onto alpha (the a-squared/coverage-erosion defect)" >&2
        FAIL=1
    fi
fi

# (3) bz_quest_vk_straight_over_blend_state() - the shared "over" state.
straight_over_body=$(extract_fn "$VK_C" "bz_quest_vk_straight_over_blend_state")
if [ -z "$straight_over_body" ]; then
    echo "test-wc3-premultiplied-blend-layout: could not find bz_quest_vk_straight_over_blend_state in $VK_C (renamed/removed?)" >&2
    FAIL=1
else
    for tok in \
        'outBlend->srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;' \
        'outBlend->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;' \
        'outBlend->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;' \
        'outBlend->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;'
    do
        if ! printf '%s' "$straight_over_body" | grep -qF "$tok"; then
            echo "test-wc3-premultiplied-blend-layout: bz_quest_vk_straight_over_blend_state() ($VK_C) missing expected token: $tok" >&2
            FAIL=1
        fi
    done
fi
if ! grep -q 'void bz_quest_vk_straight_over_blend_state(VkPipelineColorBlendAttachmentState \*outBlend);' "$VK_H"; then
    echo "test-wc3-premultiplied-blend-layout: $VK_H no longer declares bz_quest_vk_straight_over_blend_state()" >&2
    FAIL=1
fi

# (4) Every one of the 6 pipelines that used to duplicate the inline blend state now calls the
# shared helper - count call sites (terrain water=1, fog overlay=1, selection markers=1, HUD
# panel=1, HUD text=1, pointer=1 = 6 total).
total_calls=0
for f in "$VK_TERRAIN_C" "$VK_FOG_C" "$VK_HUD_C" "$VK_POINTER_C"; do
    c=$(grep -c 'bz_quest_vk_straight_over_blend_state(&blendAttachment)' "$f" || true)
    total_calls=$((total_calls + c))
done
if [ "$total_calls" -ne 6 ]; then
    echo "test-wc3-premultiplied-blend-layout: expected exactly 6 bz_quest_vk_straight_over_blend_state() call sites across terrain/fog/hud/pointer, found $total_calls" >&2
    FAIL=1
fi

# (5) No file anywhere under the Quest native source tree still has the defect pattern (mirrored
# srcColorBlendFactor/srcAlphaBlendFactor assignment to SRC_ALPHA) outside the one documented
# ADDITIVE exception (which mirrors onto ONE, never SRC_ALPHA/DST_COLOR/SRC_COLOR).
defect_hits=$(grep -rn 'srcColorBlendFactor = .*srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA' "$CPP_DIR"/*.c 2>/dev/null || true)
if [ -n "$defect_hits" ]; then
    echo "test-wc3-premultiplied-blend-layout: found reintroduced mirrored-alpha defect pattern(s):" >&2
    echo "$defect_hits" >&2
    FAIL=1
fi

# (6) Opaque/alpha-tested coverage-forcing sites - CPU push-constant population + shader branch.
if ! grep -q 'blendState.blendEnable ? 0.0f : 1.0f' "$VK_WC3_C"; then
    echo "test-wc3-premultiplied-blend-layout: $VK_WC3_C's draw_layer() no longer forces materialParams.z=1.0 for blendEnable=false (opaque/cutout) layers" >&2
    FAIL=1
fi
if ! grep -q 'pc.coverageParams\[0\] = blendState.blendEnable ? 0.0f : 1.0f;' "$VK_WC3_PARTICLES_C"; then
    echo "test-wc3-premultiplied-blend-layout: $VK_WC3_PARTICLES_C no longer forces coverageParams[0]=1.0 for blendEnable=false particle runs" >&2
    FAIL=1
fi
if ! grep -q 'range_is_blended(range) ? 0.0f : 1.0f' "$VK_TERRAIN_C"; then
    echo "test-wc3-premultiplied-blend-layout: $VK_TERRAIN_C no longer forces the ground/cliff coverage flag to 1.0" >&2
    FAIL=1
fi
if ! grep -q 'pc.materialParams.z > 0.5' "$SHADER_DIR/warcraft_frag.frag"; then
    echo "test-wc3-premultiplied-blend-layout: $SHADER_DIR/warcraft_frag.frag no longer branches on materialParams.z to force coverage alpha" >&2
    FAIL=1
fi
if ! grep -q 'pc.coverageParams.x > 0.5' "$SHADER_DIR/warcraft_particle_frag.frag"; then
    echo "test-wc3-premultiplied-blend-layout: $SHADER_DIR/warcraft_particle_frag.frag no longer branches on coverageParams.x to force coverage alpha" >&2
    FAIL=1
fi
if ! grep -q 'pc.coverageParams.x > 0.5' "$SHADER_DIR/terrain_frag.frag"; then
    echo "test-wc3-premultiplied-blend-layout: $SHADER_DIR/terrain_frag.frag no longer branches on coverageParams.x to force coverage alpha" >&2
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

echo "test-wc3-premultiplied-blend-layout: OK"
