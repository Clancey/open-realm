#!/bin/sh
# platform/android/quest/scripts/test-wc3-particles-layout.sh
#
# Structurally guards layer 9's PRE2 particle-emitter GPU contract where
# host-unit tests cannot reach (bz_quest_wc3_particles.h/.c's pure pool/
# spawn/age/pack logic IS covered by test-quest-host-tests - this script
# only covers the impure Vulkan draw module, the ABI/capture wiring, the
# shader pair, the CMake/renderer wiring, and the pure module's own frame-
# critical-forbidden-call contract that test-quest-host-tests cannot
# observe from the outside):
#   1. the Quest-native build must compile the new bz_quest_wc3_particles.c
#      and bz_quest_vk_wc3_particles.c modules at all (CMakeLists.txt source
#      list + shader-pipeline DEPENDS);
#   2. the particle pipeline must introduce its OWN new shader pair
#      (warcraft_particle_vert/frag) - unlike layer 6's pointer, which
#      deliberately reuses an existing shader, particles need a distinct
#      camera-facing-billboard vertex format;
#   3. the particle pipeline must be keyed by blend mode only (7 variants),
#      always VK_CULL_MODE_NONE (a billboard has no meaningful winding) and
#      depth-test-ON (bz_quest_vk_wc3_blend_state_for_mode()'s own per-mode
#      depth-write default, reused - not a second copy of that mapping);
#   4. the vertex format must match bzQuestWc3ParticleVertex_t field-for-
#      field (5 attributes: position/color/size/uv-rect/axis);
#   5. the renderer must wire create (after fog, before hud) / capture (after
#      wc3's own capture_and_upload, passing mapEpoch) / record (after wc3's
#      own blended pass, before fog overlay - so fog still masks visible
#      particles) / destroy (after hud, before fog - reverse creation order);
#   6. the pure simulation module's frame-critical entry points (pool_reset/
#      age/emit/pack) and every static helper they call must never allocate,
#      lock, log, touch a file, or call the bridge ABI - matching each
#      function's own doc-comment claim exactly (see this file's extract_fn
#      for the anchored-declaration-line extraction this requires);
#   7. the impure Vulkan capture_and_upload/record entry points (and the
#      record path's own get_or_create_pipeline_variant helper, called every
#      frame per eye) must never allocate, lock, touch a file, or call the
#      bridge ABI either - vkCreateGraphicsPipelines is a documented, bounded
#      (<=7-ever) exception scoped to the pipeline-variant-cache helpers
#      only, matching bz_quest_vk_wc3.c's own established
#      get_or_create_pipeline_variant() precedent, not a blanket allowance.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

CMAKE=platform/android/quest/app/src/main/cpp/CMakeLists.txt
RENDERER=platform/android/quest/app/src/main/cpp/bz_quest_renderer.c
RENDERER_H=platform/android/quest/app/src/main/cpp/bz_quest_renderer.h
VK_WC3_C=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3.c
VK_PARTICLES_C=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3_particles.c
VK_PARTICLES_H=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3_particles.h
PURE_PARTICLES_C=platform/android/quest/app/src/main/cpp/bz_quest_wc3_particles.c
PURE_PARTICLES_H=platform/android/quest/app/src/main/cpp/bz_quest_wc3_particles.h
VERT_SHADER=platform/android/quest/app/src/main/cpp/shaders/warcraft_particle_vert.vert
FRAG_SHADER=platform/android/quest/app/src/main/cpp/shaders/warcraft_particle_frag.frag
FAIL=0

for f in "$CMAKE" "$RENDERER" "$RENDERER_H" "$VK_WC3_C" "$VK_PARTICLES_C" "$VK_PARTICLES_H" \
    "$PURE_PARTICLES_C" "$PURE_PARTICLES_H" "$VERT_SHADER" "$FRAG_SHADER"; do
    if [ ! -f "$f" ]; then
        echo "test-wc3-particles-layout: expected file missing: $f" >&2
        exit 1
    fi
done

# (1) CMakeLists source list + shader DEPENDS.
for dep in 'bz_quest_wc3_particles.c' 'bz_quest_vk_wc3_particles.c'; do
    if ! grep -q "$dep" "$CMAKE"; then
        echo "test-wc3-particles-layout: $CMAKE missing '$dep'" >&2
        FAIL=1
    fi
done
for dep in 'warcraft_particle_vert.vert' 'warcraft_particle_frag.frag'; do
    if ! grep -q "$dep" "$CMAKE"; then
        echo "test-wc3-particles-layout: $CMAKE's shader DEPENDS missing '$dep'" >&2
        FAIL=1
    fi
done
BUILD_SHADERS=platform/android/quest/scripts/build-shaders.sh
for dep in 'compile_one warcraft_particle_vert vertex vert' 'compile_one warcraft_particle_frag fragment frag' \
    'warcraft_particle_vert.spv.h' 'warcraft_particle_frag.spv.h'; do
    if ! grep -qF "$dep" "$BUILD_SHADERS"; then
        echo "test-wc3-particles-layout: $BUILD_SHADERS missing '$dep'" >&2
        FAIL=1
    fi
done

# (2) A genuinely new, distinct shader pair (not a reuse of an existing one).
if ! grep -q 'g_bz_quest_warcraft_particle_vert_spv' "$VK_PARTICLES_C" \
    || ! grep -q 'g_bz_quest_warcraft_particle_frag_spv' "$VK_PARTICLES_C"; then
    echo "test-wc3-particles-layout: $VK_PARTICLES_C no longer uses its own warcraft_particle shader pair" >&2
    FAIL=1
fi

# (3) Pipeline: blend-mode-only variants, cull-none, depth-test-on, reusing the shared mapping.
if ! grep -q 'BZ_QUEST_VK_WC3_PARTICLE_PIPELINE_VARIANTS = 7' "$VK_PARTICLES_H"; then
    echo "test-wc3-particles-layout: $VK_PARTICLES_H no longer declares 7 blend-mode pipeline variants" >&2
    FAIL=1
fi
if ! grep -q 'rasterization.cullMode = VK_CULL_MODE_NONE;' "$VK_PARTICLES_C"; then
    echo "test-wc3-particles-layout: $VK_PARTICLES_C no longer keeps the particle pipeline at VK_CULL_MODE_NONE" >&2
    FAIL=1
fi
if ! grep -q 'depthStencil.depthTestEnable = VK_TRUE;' "$VK_PARTICLES_C"; then
    echo "test-wc3-particles-layout: $VK_PARTICLES_C no longer keeps the particle pipeline depth-test-on" >&2
    FAIL=1
fi
if ! grep -q 'bz_quest_vk_wc3_blend_state_for_mode(blendMode, &blendAttachment, &depthWriteDefault);' "$VK_PARTICLES_C"; then
    echo "test-wc3-particles-layout: $VK_PARTICLES_C no longer reuses bz_quest_vk_wc3_blend_state_for_mode() (DRY blend mapping)" >&2
    FAIL=1
fi

# (4) Vertex format: 5 attributes matching bzQuestWc3ParticleVertex_t field-for-field.
ATTR_COUNT=$(grep -c 'VkVertexInputAttributeDescription attrs\[5\]' "$VK_PARTICLES_C" || true)
if [ "$ATTR_COUNT" -ne 1 ]; then
    echo "test-wc3-particles-layout: $VK_PARTICLES_C must declare exactly one 5-element vertex attribute array" >&2
    FAIL=1
fi
for field in 'posX' 'color' 'size' 'u0' 'axisX'; do
    if ! grep -q "offsetof(bzQuestWc3ParticleVertex_t, $field)" "$VK_PARTICLES_C"; then
        echo "test-wc3-particles-layout: $VK_PARTICLES_C vertex attributes no longer cover bzQuestWc3ParticleVertex_t::$field" >&2
        FAIL=1
    fi
done

# (5) Renderer wiring: create/capture/record/destroy present, and correctly ordered relative to
# fog/hud/wc3's own capture_and_upload/record_blended.
if ! grep -q 'bz_quest_vk_wc3_particles_create' "$RENDERER" \
    || ! grep -q 'bz_quest_vk_wc3_particles_capture_and_upload' "$RENDERER" \
    || ! grep -q 'bz_quest_vk_wc3_particles_record' "$RENDERER" \
    || ! grep -q 'bz_quest_vk_wc3_particles_destroy' "$RENDERER"; then
    echo "test-wc3-particles-layout: $RENDERER no longer wires create/capture/record/destroy for the particle module" >&2
    FAIL=1
fi
if ! grep -q 'bzQuestVkWc3Particles_t wc3Particles;' "$RENDERER_H"; then
    echo "test-wc3-particles-layout: $RENDERER_H no longer owns the layer 9 wc3Particles member" >&2
    FAIL=1
fi
if ! grep -q 'bz_quest_vk_wc3_capture_and_upload(&renderer->wc3, &wc3RenderList, renderer->interaction.mapEpoch);' "$RENDERER"; then
    echo "test-wc3-particles-layout: $RENDERER no longer passes mapEpoch to bz_quest_vk_wc3_capture_and_upload() (ABI v4 particle-pool-reset signature)" >&2
    FAIL=1
fi
if ! grep -q 'bz_quest_vk_wc3_particles_has_geometry(&renderer->wc3Particles)' "$RENDERER"; then
    echo "test-wc3-particles-layout: $RENDERER's per-eye render-or-skip check no longer consults particle geometry" >&2
    FAIL=1
fi

line_of() {
    grep -n "$1" "$RENDERER" | head -n1 | cut -d: -f1
}
FOG_CREATE=$(line_of 'bz_quest_vk_wc3_fog_create(vk, &renderer->wc3Fog)')
PARTICLES_CREATE=$(line_of 'bz_quest_vk_wc3_particles_create(vk, &renderer->wc3, &renderer->wc3Particles)')
HUD_CREATE=$(line_of 'bz_quest_vk_wc3_hud_create(vk, &renderer->wc3Hud)')
WC3_CAPTURE=$(line_of 'bz_quest_vk_wc3_capture_and_upload(&renderer->wc3, &wc3RenderList')
PARTICLES_CAPTURE=$(line_of 'bz_quest_vk_wc3_particles_capture_and_upload(&renderer->wc3Particles)')
WC3_BLENDED=$(line_of 'bz_quest_vk_wc3_record_blended(&renderer->wc3, target->commandBuffer')
PARTICLES_RECORD=$(line_of 'bz_quest_vk_wc3_particles_record(&renderer->wc3Particles, target->commandBuffer')
FOG_OVERLAY=$(line_of 'bz_quest_vk_wc3_fog_record_overlay(&renderer->wc3Fog, target->commandBuffer')
HUD_DESTROY=$(line_of 'bz_quest_vk_wc3_hud_destroy(&renderer->wc3Hud)')
PARTICLES_DESTROY=$(line_of 'bz_quest_vk_wc3_particles_destroy(&renderer->wc3Particles)')
FOG_DESTROY=$(line_of 'bz_quest_vk_wc3_fog_destroy(&renderer->wc3Fog)')

for pair_name in \
    "FOG_CREATE:$FOG_CREATE:PARTICLES_CREATE:$PARTICLES_CREATE" \
    "PARTICLES_CREATE:$PARTICLES_CREATE:HUD_CREATE:$HUD_CREATE" \
    "WC3_CAPTURE:$WC3_CAPTURE:PARTICLES_CAPTURE:$PARTICLES_CAPTURE" \
    "WC3_BLENDED:$WC3_BLENDED:PARTICLES_RECORD:$PARTICLES_RECORD" \
    "PARTICLES_RECORD:$PARTICLES_RECORD:FOG_OVERLAY:$FOG_OVERLAY" \
    "HUD_DESTROY:$HUD_DESTROY:PARTICLES_DESTROY:$PARTICLES_DESTROY" \
    "PARTICLES_DESTROY:$PARTICLES_DESTROY:FOG_DESTROY:$FOG_DESTROY"
do
    a_label=$(echo "$pair_name" | cut -d: -f1)
    a_line=$(echo "$pair_name" | cut -d: -f2)
    b_label=$(echo "$pair_name" | cut -d: -f3)
    b_line=$(echo "$pair_name" | cut -d: -f4)
    if [ -z "$a_line" ] || [ -z "$b_line" ]; then
        echo "test-wc3-particles-layout: could not locate $a_label/$b_label call sites in $RENDERER" >&2
        FAIL=1
    elif ! [ "$a_line" -lt "$b_line" ]; then
        echo "test-wc3-particles-layout: $RENDERER must order $a_label before $b_label (found $a_label=$a_line, $b_label=$b_line)" >&2
        FAIL=1
    fi
done

# (6)/(7) Frame-critical forbidden-call scan. Extraction anchors at a REAL declaration line -
# the function name immediately followed by '(' at or near the START of the line (an optional
# "static "/return-type prefix, never indented) - so a mere call site elsewhere in the file can
# never be mistaken for the function's own definition. Prints from that line to the next line
# whose sole content is a closing brace, matching this project's one-brace-per-line C style.
extract_fn() {
    file=$1
    name=$2
    awk -v fn="$name" '
        $0 ~ "^[A-Za-z_][A-Za-z0-9_ *]*" fn "\\(" { found=1 }
        found { print }
        found && /^}/ { exit }
    ' "$file"
}

check_forbidden() {
    label=$1
    body=$2
    shift 2
    for pat in "$@"; do
        # Word-boundary-aware: "printf(" must not match inside "snprintf("/"vfprintf(", "free("
        # must not match inside a longer identifier - require the character immediately before
        # the pattern (if any) is not itself an identifier character.
        if printf '%s' "$body" | grep -qE "(^|[^A-Za-z0-9_])$(printf '%s' "$pat" | sed 's/[.[\*^$()+?{|]/\\&/g')"; then
            echo "test-wc3-particles-layout: $label calls forbidden '$pat' - frame-critical particle evaluation/emission must never allocate, lock, log, touch a file, or call the bridge ABI (see this file's header comment)" >&2
            FAIL=1
        fi
    done
}

# SET A: strict, zero exceptions - the pure simulation module's frame-critical surface.
STRICT_FORBIDDEN='malloc( calloc( realloc( free( pthread_mutex_ BZ_QUEST_LOGE BZ_QUEST_LOGW BZ_QUEST_LOGI fprintf( printf( fopen( fread( BZ_TT'
PURE_FNS='bz_quest_wc3_particles_pool_reset bz_quest_wc3_particles_age bz_quest_wc3_particles_emit bz_quest_wc3_particles_pack next_u64 next_float01 mat4_transform_point pool_spawn_slot random_origin_yup random_direction_yup clamp_alpha_component lerp_color4 blend_color blend_float3 atlas_rect compare_sort_key'
for fn in $PURE_FNS; do
    body=$(extract_fn "$PURE_PARTICLES_C" "$fn")
    if [ -z "$body" ]; then
        echo "test-wc3-particles-layout: could not find $fn in $PURE_PARTICLES_C (renamed/removed?)" >&2
        FAIL=1
        continue
    fi
    # shellcheck disable=SC2086
    check_forbidden "$PURE_PARTICLES_C:$fn" "$body" $STRICT_FORBIDDEN
done

# SET B: the impure Vulkan capture_and_upload/record entry points, same strict set minus nothing
# (their OWN bodies never need to allocate/lock/log/touch a file/call the bridge ABI - packing
# writes directly into the already-mapped GPU buffer, and drawing only binds/pushes/vkCmdDraws).
IMPURE_FNS='bz_quest_vk_wc3_particles_capture_and_upload bz_quest_vk_wc3_particles_record'
for fn in $IMPURE_FNS; do
    body=$(extract_fn "$VK_PARTICLES_C" "$fn")
    if [ -z "$body" ]; then
        echo "test-wc3-particles-layout: could not find $fn in $VK_PARTICLES_C (renamed/removed?)" >&2
        FAIL=1
        continue
    fi
    # shellcheck disable=SC2086
    check_forbidden "$VK_PARTICLES_C:$fn" "$body" $STRICT_FORBIDDEN
done

# get_or_create_pipeline_variant is a helper called every frame (per eye) from record() - the
# SAME strict set applies EXCEPT vkCreateGraphicsPipelines/BZ_QUEST_LOGE, a documented, bounded
# (<=7-ever) exception matching bz_quest_vk_wc3.c's own established get_or_create_pipeline_variant
# precedent (a real cache, not unbounded per-frame work - see this file's header comment).
VARIANT_FORBIDDEN='malloc( calloc( realloc( free( pthread_mutex_ fprintf( printf( fopen( fread( BZ_TT'
variant_body=$(extract_fn "$VK_PARTICLES_C" "get_or_create_pipeline_variant")
if [ -z "$variant_body" ]; then
    echo "test-wc3-particles-layout: could not find get_or_create_pipeline_variant in $VK_PARTICLES_C (renamed/removed?)" >&2
    FAIL=1
else
    # shellcheck disable=SC2086
    check_forbidden "$VK_PARTICLES_C:get_or_create_pipeline_variant" "$variant_body" $VARIANT_FORBIDDEN
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

echo "test-wc3-particles-layout: OK"
