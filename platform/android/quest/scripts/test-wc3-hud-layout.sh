#!/bin/sh
# platform/android/quest/scripts/test-wc3-hud-layout.sh
#
# Structurally guards layer 5E's status/command-card HUD GPU contract where
# host-unit tests cannot reach (bz_quest_wc3_hud.h/.c and
# bz_quest_wc3_hud_font.h/.c's pure layout/font logic ARE covered by
# test-quest-host-tests - this script only covers the impure Vulkan module
# and its wiring):
#   1. the Quest-native build must compile/embed the four new HUD shaders and
#      the new bz_quest_vk_wc3_hud.c/bz_quest_wc3_hud.c/bz_quest_wc3_hud_font.c
#      modules at all;
#   2. the font atlas image must stay one-byte-per-texel VK_FORMAT_R8_UNORM,
#      with the image view matching that format (mirrors the fog mask's own
#      R8 convention - see test-wc3-fog-selection-layout.sh);
#   3. the text shader interface must stay set 0 / binding 0 for the font
#      atlas sampler;
#   4. both HUD pipelines must stay depth-tested but depth-write-disabled
#      (readable over the board, never itself occluding later draws - same
#      rationale as layer 5D's selection markers) and must use
#      VK_CULL_MODE_NONE (arbitrary panel-local right/down/normal basis, no
#      guaranteed winding) with VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST (indexed
#      quads, not a strip);
#   5. both pipelines' push constant must stay a single vertex-stage `mat4
#      mvp` (bzQuestVkWc3HudPushConsts_t) - no per-vertex world-space math;
#   6. the shared eye-pass order must stay ... -> fog overlay -> selection
#      markers -> HUD (HUD LAST of all world/overlay draws, so the
#      bridge-authored status/command-card panel is always legible on top of
#      the fogged, selection-marked board).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

BUILD_SHADERS=platform/android/quest/scripts/build-shaders.sh
CMAKE=platform/android/quest/app/src/main/cpp/CMakeLists.txt
RENDERER=platform/android/quest/app/src/main/cpp/bz_quest_renderer.c
RENDERER_H=platform/android/quest/app/src/main/cpp/bz_quest_renderer.h
VK_HUD_C=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3_hud.c
HUD_C=platform/android/quest/app/src/main/cpp/bz_quest_wc3_hud.c
HUD_FONT_C=platform/android/quest/app/src/main/cpp/bz_quest_wc3_hud_font.c
PANEL_VERT=platform/android/quest/app/src/main/cpp/shaders/warcraft_hud_panel_vert.vert
PANEL_FRAG=platform/android/quest/app/src/main/cpp/shaders/warcraft_hud_panel_frag.frag
TEXT_VERT=platform/android/quest/app/src/main/cpp/shaders/warcraft_hud_text_vert.vert
TEXT_FRAG=platform/android/quest/app/src/main/cpp/shaders/warcraft_hud_text_frag.frag
FAIL=0

for f in "$BUILD_SHADERS" "$CMAKE" "$RENDERER" "$RENDERER_H" "$VK_HUD_C" "$HUD_C" "$HUD_FONT_C" \
    "$PANEL_VERT" "$PANEL_FRAG" "$TEXT_VERT" "$TEXT_FRAG"; do
    if [ ! -f "$f" ]; then
        echo "test-wc3-hud-layout: expected file missing: $f" >&2
        exit 1
    fi
done

for shader in \
    'compile_one warcraft_hud_panel_vert vertex vert' \
    'compile_one warcraft_hud_panel_frag fragment frag' \
    'compile_one warcraft_hud_text_vert vertex vert' \
    'compile_one warcraft_hud_text_frag fragment frag'
do
    if ! grep -q "$shader" "$BUILD_SHADERS"; then
        echo "test-wc3-hud-layout: $BUILD_SHADERS missing '$shader'" >&2
        FAIL=1
    fi
done

for dep in \
    'shaders/warcraft_hud_panel_vert.vert' \
    'shaders/warcraft_hud_panel_frag.frag' \
    'shaders/warcraft_hud_text_vert.vert' \
    'shaders/warcraft_hud_text_frag.frag' \
    'bz_quest_wc3_hud_font.c' \
    'bz_quest_wc3_hud.c' \
    'bz_quest_vk_wc3_hud.c'
do
    if ! grep -q "$dep" "$CMAKE"; then
        echo "test-wc3-hud-layout: $CMAKE missing '$dep'" >&2
        FAIL=1
    fi
done

if ! grep -q 'imageInfo.format = VK_FORMAT_R8_UNORM;' "$VK_HUD_C" || ! grep -q 'viewInfo.format = VK_FORMAT_R8_UNORM;' "$VK_HUD_C"; then
    echo "test-wc3-hud-layout: $VK_HUD_C no longer keeps the font atlas image/view at VK_FORMAT_R8_UNORM" >&2
    FAIL=1
fi
if ! grep -q 'layout(set = 0, binding = 0) uniform sampler2D uFontAtlas;' "$TEXT_FRAG"; then
    echo "test-wc3-hud-layout: $TEXT_FRAG no longer declares the font atlas sampler at set 0 binding 0" >&2
    FAIL=1
fi

DEPTH_TEST_COUNT=$(grep -c 'depthStencil.depthTestEnable = VK_TRUE;' "$VK_HUD_C" || true)
DEPTH_WRITE_COUNT=$(grep -c 'depthStencil.depthWriteEnable = VK_FALSE;' "$VK_HUD_C" || true)
CULL_NONE_COUNT=$(grep -c 'rasterization.cullMode = VK_CULL_MODE_NONE;' "$VK_HUD_C" || true)
TOPOLOGY_COUNT=$(grep -c 'inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;' "$VK_HUD_C" || true)
if [ "$DEPTH_TEST_COUNT" -ne 2 ] || [ "$DEPTH_WRITE_COUNT" -ne 2 ]; then
    echo "test-wc3-hud-layout: $VK_HUD_C no longer keeps both HUD pipelines depth-tested and depth-write-disabled" >&2
    FAIL=1
fi
if [ "$CULL_NONE_COUNT" -ne 2 ]; then
    echo "test-wc3-hud-layout: $VK_HUD_C no longer keeps both HUD pipelines at VK_CULL_MODE_NONE" >&2
    FAIL=1
fi
if [ "$TOPOLOGY_COUNT" -ne 2 ]; then
    echo "test-wc3-hud-layout: $VK_HUD_C no longer keeps both HUD pipelines at VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST" >&2
    FAIL=1
fi

if ! grep -q 'float mvp\[16\];' "$VK_HUD_C"; then
    echo "test-wc3-hud-layout: $VK_HUD_C's bzQuestVkWc3HudPushConsts_t no longer carries a single mat4 mvp" >&2
    FAIL=1
fi
PC_COUNT=$(grep -c 'VkPushConstantRange .*Pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(bzQuestVkWc3HudPushConsts_t)};' "$VK_HUD_C" || true)
if [ "$PC_COUNT" -ne 2 ]; then
    echo "test-wc3-hud-layout: $VK_HUD_C no longer wires a vertex-only mvp push constant range for both pipelines" >&2
    FAIL=1
fi

if ! grep -q 'bz_quest_vk_wc3_hud_create' "$RENDERER" || ! grep -q 'bz_quest_vk_wc3_hud_capture_and_upload' "$RENDERER" \
    || ! grep -q 'bz_quest_vk_wc3_hud_record' "$RENDERER" || ! grep -q 'bz_quest_vk_wc3_hud_destroy' "$RENDERER"; then
    echo "test-wc3-hud-layout: $RENDERER no longer wires create/capture_and_upload/record/destroy for the HUD module" >&2
    FAIL=1
fi
if ! grep -q 'bzQuestVkWc3Hud_t wc3Hud;' "$RENDERER_H"; then
    echo "test-wc3-hud-layout: $RENDERER_H no longer owns a bzQuestVkWc3Hud_t wc3Hud member" >&2
    FAIL=1
fi

# --- PR #24 review defect 2: create_font_atlas() must clean up every
# partial VkImage/memory/view on failure instead of leaking it on retry
# (see destroy_font_image()'s and create_font_atlas()'s doc comments in
# $VK_HUD_C). Each of the function's 7 failure points after image creation
# begins must call destroy_font_image(vkHud) before returning false, and
# the whole file must have exactly one more call site: the unconditional
# teardown in bz_quest_vk_wc3_hud_destroy().
CREATE_FONT_ATLAS_BODY=$(awk '/^static bool create_font_atlas/,/^}/' "$VK_HUD_C")
CREATE_ATLAS_CLEANUP_COUNT=$(printf '%s\n' "$CREATE_FONT_ATLAS_BODY" | grep -c 'destroy_font_image(vkHud);' || true)
TOTAL_CLEANUP_COUNT=$(grep -c 'destroy_font_image(vkHud);' "$VK_HUD_C" || true)
if [ "$CREATE_ATLAS_CLEANUP_COUNT" -ne 7 ]; then
    echo "test-wc3-hud-layout: $VK_HUD_C's create_font_atlas() no longer cleans up via destroy_font_image(vkHud) at all 7 failure points (found $CREATE_ATLAS_CLEANUP_COUNT)" >&2
    FAIL=1
fi
if [ "$TOTAL_CLEANUP_COUNT" -ne 8 ]; then
    echo "test-wc3-hud-layout: $VK_HUD_C should call destroy_font_image(vkHud) exactly 8 times total (7 in create_font_atlas + 1 in bz_quest_vk_wc3_hud_destroy), found $TOTAL_CLEANUP_COUNT" >&2
    FAIL=1
fi
DESTROY_DEF_LINE=$(grep -n '^static void destroy_font_image' "$VK_HUD_C" | head -n1 | cut -d: -f1)
CREATE_ATLAS_DEF_LINE=$(grep -n '^static bool create_font_atlas' "$VK_HUD_C" | head -n1 | cut -d: -f1)
if [ -z "$DESTROY_DEF_LINE" ] || [ -z "$CREATE_ATLAS_DEF_LINE" ] || ! [ "$DESTROY_DEF_LINE" -lt "$CREATE_ATLAS_DEF_LINE" ]; then
    echo "test-wc3-hud-layout: $VK_HUD_C must define destroy_font_image() before create_font_atlas() so the latter can call it directly" >&2
    FAIL=1
fi

# --- PR #24 review defect 3: build_text_vertices() must not discard
# bz_quest_wc3_hud_font_layout_text()'s truncation signal, and must scan
# for/log unsupported bytes via bz_quest_wc3_hud_font_glyph_uv() - both
# routed through this file's own once-per-condition dedup helper (see
# bz_quest_vk_wc3.c's identical convention).
if ! grep -q 'static bool vk_hud_log_once' "$VK_HUD_C" || ! grep -q 'define VK_WC3_HUD_LOG_ONCE' "$VK_HUD_C"; then
    echo "test-wc3-hud-layout: $VK_HUD_C no longer defines its own file-local vk_hud_log_once/VK_WC3_HUD_LOG_ONCE dedup helper" >&2
    FAIL=1
fi
if grep -q 'bz_quest_wc3_hud_font_layout_text(run->text' "$VK_HUD_C" && \
   ! grep -q 'fitEntirely = bz_quest_wc3_hud_font_layout_text(run->text' "$VK_HUD_C"; then
    echo "test-wc3-hud-layout: $VK_HUD_C's build_text_vertices() must capture bz_quest_wc3_hud_font_layout_text()'s truncation return instead of discarding it" >&2
    FAIL=1
fi
if ! grep -q 'bz_quest_wc3_hud_font_glyph_uv(uc,' "$VK_HUD_C"; then
    echo "test-wc3-hud-layout: $VK_HUD_C's build_text_vertices() no longer scans each run's bytes via bz_quest_wc3_hud_font_glyph_uv() for unsupported-byte diagnostics" >&2
    FAIL=1
fi
if ! grep -q 'VK_WC3_HUD_LOG_ONCE("hud-text-truncated"' "$VK_HUD_C" || \
   ! grep -q 'VK_WC3_HUD_LOG_ONCE("hud-text-unsupported-byte"' "$VK_HUD_C" || \
   ! grep -q 'VK_WC3_HUD_LOG_ONCE("hud-status-text-truncated"' "$VK_HUD_C"; then
    echo "test-wc3-hud-layout: $VK_HUD_C must log truncated text runs, unsupported bytes, and statusTextTruncated once each via VK_WC3_HUD_LOG_ONCE" >&2
    FAIL=1
fi

line_of() {
    grep -n "$1" "$RENDERER" | head -n1 | cut -d: -f1
}
FOG_OVERLAY=$(line_of 'bz_quest_vk_wc3_fog_record_overlay')
SELECTION=$(line_of 'bz_quest_vk_wc3_fog_record_selection')
HUD_RECORD=$(line_of 'bz_quest_vk_wc3_hud_record(&renderer->wc3Hud')
for pair in "$FOG_OVERLAY" "$SELECTION" "$HUD_RECORD"; do
    if [ -z "$pair" ]; then
        echo "test-wc3-hud-layout: could not locate one of the shared render-pass call sites in $RENDERER" >&2
        FAIL=1
        break
    fi
done
if [ "$FAIL" -eq 0 ] && { ! [ "$FOG_OVERLAY" -lt "$SELECTION" ] || ! [ "$SELECTION" -lt "$HUD_RECORD" ]; }; then
    echo "test-wc3-hud-layout: $RENDERER no longer records fog overlay -> selection markers -> HUD in that order" >&2
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi

echo "test-wc3-hud-layout: OK (HUD shaders, R8 font atlas format, depth/cull/topology flags, mvp-only push constants, and render-pass ordering intact)"
