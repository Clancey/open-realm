#!/bin/sh
# platform/android/quest/scripts/test-wc3-fog-selection-layout.sh
#
# Structurally guards layer 5D's fog/selection GPU contract where host-unit
# tests cannot reach:
#   1. the Quest-native build must compile/embed the four new fog/marker
#      shaders and the new Vulkan module at all;
#   2. the fog image must stay one-byte-per-cell VK_FORMAT_R8_UNORM, with the
#      image view matching that format;
#   3. the fog shader interface must stay set 0 / binding 0 and the upload path
#      must preserve explicit row-length handling for padded rows;
#   4. the shared eye-pass order must stay terrain opaque -> model opaque ->
#      terrain blended -> model blended -> fog overlay -> selection markers
#      (fog LAST among world/overlay draws, so it composites over blended
#      geometry like water/transparent doodads instead of being drawn under it);
#   5. selection markers must stay depth-tested but depth-write-disabled so they
#      do not show through opaque geometry and do not corrupt later depth use;
#   6. the fog overlay's vertex shader and its C push-constant struct must both
#      carry the shared world/tabletop transform (centerX/centerZ/scale) and
#      apply it to gl_Position - the fix for layer 5D's inherited
#      terrain/entity/fog coordinate-space mismatch (see
#      bz_quest_wc3_render.h's header comment).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

BUILD_SHADERS=platform/android/quest/scripts/build-shaders.sh
CMAKE=platform/android/quest/app/src/main/cpp/CMakeLists.txt
RENDERER=platform/android/quest/app/src/main/cpp/bz_quest_renderer.c
VK_FOG_C=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3_fog.c
FOG_VERT=platform/android/quest/app/src/main/cpp/shaders/warcraft_fog_vert.vert
FOG_FRAG=platform/android/quest/app/src/main/cpp/shaders/warcraft_fog_frag.frag
MARKER_VERT=platform/android/quest/app/src/main/cpp/shaders/warcraft_marker_vert.vert
MARKER_FRAG=platform/android/quest/app/src/main/cpp/shaders/warcraft_marker_frag.frag
FAIL=0

for f in "$BUILD_SHADERS" "$CMAKE" "$RENDERER" "$VK_FOG_C" "$FOG_VERT" "$FOG_FRAG" "$MARKER_VERT" "$MARKER_FRAG"; do
    if [ ! -f "$f" ]; then
        echo "test-wc3-fog-selection-layout: expected file missing: $f" >&2
        exit 1
    fi
done

for shader in \
    'compile_one warcraft_fog_vert vertex vert' \
    'compile_one warcraft_fog_frag fragment frag' \
    'compile_one warcraft_marker_vert vertex vert' \
    'compile_one warcraft_marker_frag fragment frag'
do
    if ! grep -q "$shader" "$BUILD_SHADERS"; then
        echo "test-wc3-fog-selection-layout: $BUILD_SHADERS missing '$shader'" >&2
        FAIL=1
    fi
done

for dep in \
    'shaders/warcraft_fog_vert.vert' \
    'shaders/warcraft_fog_frag.frag' \
    'shaders/warcraft_marker_vert.vert' \
    'shaders/warcraft_marker_frag.frag' \
    'bz_quest_wc3_fog.c' \
    'bz_quest_vk_wc3_fog.c'
do
    if ! grep -q "$dep" "$CMAKE"; then
        echo "test-wc3-fog-selection-layout: $CMAKE missing '$dep'" >&2
        FAIL=1
    fi
done

if ! grep -q 'imageInfo.format = VK_FORMAT_R8_UNORM;' "$VK_FOG_C" || ! grep -q 'viewInfo.format = VK_FORMAT_R8_UNORM;' "$VK_FOG_C"; then
    echo "test-wc3-fog-selection-layout: $VK_FOG_C no longer keeps the fog image/view at VK_FORMAT_R8_UNORM" >&2
    FAIL=1
fi
if ! grep -q 'copy.bufferRowLength = ctx->rowLength;' "$VK_FOG_C"; then
    echo "test-wc3-fog-selection-layout: $VK_FOG_C no longer wires VkBufferImageCopy.bufferRowLength from the padded-row upload context" >&2
    FAIL=1
fi
if ! grep -q 'layout(set = 0, binding = 0) uniform sampler2D uFogMask;' "$FOG_FRAG"; then
    echo "test-wc3-fog-selection-layout: $FOG_FRAG no longer declares the fog sampler at set 0 binding 0" >&2
    FAIL=1
fi
if ! grep -q 'depthStencil.depthTestEnable = VK_TRUE;' "$VK_FOG_C" || ! grep -q 'depthStencil.depthWriteEnable = VK_FALSE;' "$VK_FOG_C"; then
    echo "test-wc3-fog-selection-layout: $VK_FOG_C no longer keeps selection markers depth-tested and depth-write-disabled" >&2
    FAIL=1
fi
if ! grep -q 'float transform\[4\];' "$VK_FOG_C"; then
    echo "test-wc3-fog-selection-layout: $VK_FOG_C's FogPushConsts_t no longer carries the shared world/tabletop transform" >&2
    FAIL=1
fi
if ! grep -q 'pc.transform\[0\] = vkFog->transform.centerX;' "$VK_FOG_C"; then
    echo "test-wc3-fog-selection-layout: $VK_FOG_C no longer populates the fog transform push constant from vkFog->transform" >&2
    FAIL=1
fi
if ! grep -q 'vec4 transform;' "$FOG_VERT"; then
    echo "test-wc3-fog-selection-layout: $FOG_VERT no longer declares the shared world/tabletop transform push constant" >&2
    FAIL=1
fi
if ! grep -q 'renderX = (worldX - pc.transform.x) \* pc.transform.z;' "$FOG_VERT"; then
    echo "test-wc3-fog-selection-layout: $FOG_VERT no longer applies the shared transform to the fog quad's on-screen position" >&2
    FAIL=1
fi

line_of() {
    grep -n "$1" "$RENDERER" | head -n1 | cut -d: -f1
}
TERRAIN_OPAQUE=$(line_of 'bz_quest_vk_wc3_terrain_record_opaque')
MODEL_OPAQUE=$(line_of 'bz_quest_vk_wc3_record_opaque')
TERRAIN_BLEND=$(line_of 'bz_quest_vk_wc3_terrain_record_blended')
MODEL_BLEND=$(line_of 'bz_quest_vk_wc3_record_blended')
FOG_OVERLAY=$(line_of 'bz_quest_vk_wc3_fog_record_overlay')
SELECTION=$(line_of 'bz_quest_vk_wc3_fog_record_selection')
for pair in "$TERRAIN_OPAQUE" "$MODEL_OPAQUE" "$TERRAIN_BLEND" "$MODEL_BLEND" "$FOG_OVERLAY" "$SELECTION"; do
    if [ -z "$pair" ]; then
        echo "test-wc3-fog-selection-layout: could not locate one of the shared render-pass call sites in $RENDERER" >&2
        FAIL=1
        break
    fi
done
if [ "$FAIL" -eq 0 ] && ! [ "$TERRAIN_OPAQUE" -lt "$MODEL_OPAQUE" ] || ! [ "$MODEL_OPAQUE" -lt "$TERRAIN_BLEND" ] || ! [ "$TERRAIN_BLEND" -lt "$MODEL_BLEND" ] || ! [ "$MODEL_BLEND" -lt "$FOG_OVERLAY" ] || ! [ "$FOG_OVERLAY" -lt "$SELECTION" ]; then
    echo "test-wc3-fog-selection-layout: $RENDERER no longer records terrain opaque -> model opaque -> terrain blended -> model blended -> fog overlay -> selection markers in that order" >&2
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

echo "test-wc3-fog-selection-layout: OK (fog/selection shaders, R8 image format, row-length upload path, and shared render-pass ordering intact)"
