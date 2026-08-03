/*
 * bz_quest_vk_wc3_hud.h - layer 5E: Quest-local Vulkan ownership for the
 * Warcraft III tabletop status/command-card HUD (bz_quest_wc3_hud.h's pure
 * layout output rendered as world-space quads and font-atlas glyph text).
 *
 * Presentation only, matching this slice's scope: no OpenXR controller/hand
 * input is read here and no command is ever posted - bz_quest_wc3_hud.h's
 * exported bz_quest_wc3_hud_hit_test() plus this module's `frame`/`input`
 * (kept alive across frames below) are what a later input layer will call
 * directly; this file never calls that function itself.
 *
 * Two small pipelines, matching bz_quest_vk_wc3_fog.h's fog/marker split:
 *   - "panel": flat-tint quads (status/command backgrounds, per-button
 *     placeholder slots - see bz_quest_wc3_hud.h's header comment for why no
 *     real icon texture exists to sample instead), one persistent
 *     host-visible dynamic vertex/index buffer rebuilt (with a memcmp dirty
 *     check - no needless re-upload of visually-identical content) every
 *     frame from bzQuestHudFrame_t.quads.
 *   - "text": glyph-textured quads sampling the ONE font atlas texture
 *     (bz_quest_wc3_hud_font_build_atlas(), created once and retried on
 *     failure - see bz_quest_vk_wc3_hud_create()), expanded from
 *     bzQuestHudFrame_t.texts via bz_quest_wc3_hud_font_layout_text() into
 *     another persistent host-visible dynamic vertex/index buffer, same
 *     dirty-check discipline.
 *
 * Both dynamic buffers are HOST_VISIBLE|HOST_COHERENT and mapped exactly
 * once for their lifetime (bz_quest_vk_wc3_hud_create()) - "in-flight
 * Vulkan safety" here means never touching them between
 * vkCmdBindVertexBuffers/vkCmdDrawIndexed and the matching frame's
 * vkWaitForFences (bz_quest_renderer_record_frame() waits for the previous
 * frame's fence before this module's capture_and_upload() runs again - see
 * bz_quest_renderer.c), the same single-buffered-per-frame discipline
 * bz_quest_vk_wc3.c's bone-palette UBO already relies on; there is
 * deliberately no double-buffering here (small, bounded HUD content, and
 * this project's existing single in-flight frame model - see
 * docs/quest-tabletop.md).
 */
#ifndef BZ_QUEST_VK_WC3_HUD_H
#define BZ_QUEST_VK_WC3_HUD_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "bz_quest_vk.h"
#include "bz_quest_wc3_capture.h"
#include "bz_quest_wc3_hud.h"
#include "bz_quest_wc3_hud_font.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_VK_WC3_HUD_MAX_PANEL_VERTICES = BZ_QUEST_HUD_MAX_QUADS * 4,
    BZ_QUEST_VK_WC3_HUD_MAX_PANEL_INDICES = BZ_QUEST_HUD_MAX_QUADS * 6,
    /* One text run's characters are bounded by BZ_QUEST_HUD_MAX_STATUS_TEXT
     * (its NUL-terminated buffer size); the -1 excludes the guaranteed NUL. */
    BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS_PER_RUN = BZ_QUEST_HUD_MAX_STATUS_TEXT - 1,
    BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS =
        BZ_QUEST_HUD_MAX_TEXT_RUNS * BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS_PER_RUN,
    BZ_QUEST_VK_WC3_HUD_MAX_TEXT_VERTICES = BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS * 4,
    BZ_QUEST_VK_WC3_HUD_MAX_TEXT_INDICES = BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS * 6,
    BZ_QUEST_VK_WC3_HUD_STAGING_BYTES = BZ_QUEST_HUD_FONT_ATLAS_BYTES,
};

/* This is the one file that already includes both bz_quest_wc3_hud.h and
 * bz_quest_wc3_hud_font.h, so it's the only place that can safely enforce
 * the "safely below BZ_QUEST_HUD_FONT_MAX_GLYPHS_PER_RUN" cross-file
 * relationship BZ_QUEST_HUD_MAX_STATUS_TEXT's comment promises, without
 * making either pure module's header include the other (they are
 * deliberately independent - the font module knows nothing of HUD layout,
 * and vice versa). If this ever fires, BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS_PER_RUN
 * (derived above from BZ_QUEST_HUD_MAX_STATUS_TEXT) has grown past what a
 * single layout_text() call can hold - bump BZ_QUEST_HUD_FONT_MAX_GLYPHS_PER_RUN
 * accordingly. */
_Static_assert(BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS_PER_RUN <= BZ_QUEST_HUD_FONT_MAX_GLYPHS_PER_RUN,
               "BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS_PER_RUN must stay <= BZ_QUEST_HUD_FONT_MAX_GLYPHS_PER_RUN");

typedef struct {
    float pos[3];
    float color[4];
} bzQuestVkWc3HudPanelVertex_t;

typedef struct {
    float pos[3];
    float uv[2];
} bzQuestVkWc3HudTextVertex_t;

typedef struct bzQuestVkWc3Hud_s {
    const bzQuestVk_t *vk;
    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSet;
    VkSampler sampler;
    VkPipelineLayout panelPipelineLayout, textPipelineLayout;
    VkShaderModule panelVertexShader, panelFragmentShader;
    VkShaderModule textVertexShader, textFragmentShader;
    VkPipeline panelPipeline, textPipeline;
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkCommandBuffer uploadCommandBuffer;
    /* Font atlas: created once (retried next frame on failure - see
     * bz_quest_vk_wc3_hud_capture_and_upload()), never re-uploaded again -
     * the atlas is a fixed, deterministic build-time asset (bz_quest_wc3_
     * hud_font.h), not per-frame game state. */
    VkImage fontImage;
    VkDeviceMemory fontImageMemory;
    VkImageView fontImageView;
    bool haveFont;
    /* Persistently mapped for this struct's whole lifetime - see this
     * file's header comment on in-flight safety. */
    VkBuffer panelVertexBuffer, panelIndexBuffer;
    VkDeviceMemory panelVertexMemory, panelIndexMemory;
    void *panelVertexMapped, *panelIndexMapped;
    uint32_t panelVertexBytes, panelIndexBytes, panelIndexCount;
    VkBuffer textVertexBuffer, textIndexBuffer;
    VkDeviceMemory textVertexMemory, textIndexMemory;
    void *textVertexMapped, *textIndexMapped;
    uint32_t textVertexBytes, textIndexBytes, textIndexCount;
    bzQuestHudInput_t *input;
    bzQuestHudFrame_t *frame;
    bool haveFrame;
} bzQuestVkWc3Hud_t;

bool bz_quest_vk_wc3_hud_create(const bzQuestVk_t *vk, bzQuestVkWc3Hud_t *out);

/*
 * Captures this frame's authoritative HUD state (bz_quest_wc3_capture_hud()),
 * builds the deterministic layout (bz_quest_wc3_hud_build()), rebuilds the
 * panel/text vertex+index arrays from that SAME frame (never a second set
 * of literals - see bz_quest_wc3_hud.h's header comment), and re-uploads
 * only the buffers whose bytes actually changed. Also retries the one-time
 * font atlas creation if it failed at bz_quest_vk_wc3_hud_create() time
 * (logged once via BZ_QUEST_LOGE, not per-frame). Safe to call every frame;
 * on any capture failure (no snapshot yet) leaves `vkHud->haveFrame` false
 * and both draw counts at 0, so record() below draws nothing that frame.
 */
void bz_quest_vk_wc3_hud_capture_and_upload(bzQuestVkWc3Hud_t *vkHud);

bool bz_quest_vk_wc3_hud_has_frame(const bzQuestVkWc3Hud_t *vkHud);

/*
 * Returns the current frame's layout/hit-test contract (NULL if no snapshot
 * has ever been captured) - this is the exact pointer a later input layer
 * reads and passes to bz_quest_wc3_hud_hit_test(); this module never calls
 * that function itself (see this file's header comment on scope).
 */
const bzQuestHudFrame_t *bz_quest_vk_wc3_hud_frame(const bzQuestVkWc3Hud_t *vkHud);

void bz_quest_vk_wc3_hud_record(bzQuestVkWc3Hud_t *vkHud, VkCommandBuffer cmd, const float viewProj[16]);

void bz_quest_vk_wc3_hud_destroy(bzQuestVkWc3Hud_t *vkHud);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_VK_WC3_HUD_H */
