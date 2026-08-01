/*
 * bz_quest_vk_wc3_pointer.h - layer 6: Quest-local Vulkan ownership for the
 * Touch-controller ray pointers + hit reticles. A tiny procedural module that
 * draws, per visible hand, a thin crossed-quad "beam" from the controller aim
 * origin to either the ray hit point or a fixed fallback length, plus a small
 * disc reticle at the hit point when the ray hit something. It reuses the
 * layer 5D selection-marker shaders verbatim (warcraft_marker_vert/frag:
 * push_constant {mat4 mvp; vec4 tint;}, position-only vertex) - the ray/
 * reticle needs exactly that unlit-tinted-geometry pipeline, so introducing a
 * new shader pair would duplicate it for no benefit (see docs/quest-tabletop.md
 * Layer 6). Positions are written each frame in LOCAL tracking space by
 * bz_quest_vk_wc3_pointer_update() and drawn with the plain per-eye
 * view*projection (NOT the board-folded matrix): the controllers are physical
 * objects in tracking space, while their reticle endpoints are already mapped
 * out of composed space into tracking space by the caller (bz_quest_renderer.c)
 * using the same board transform the board geometry uses. Depth-tested against
 * the shared render pass' depth buffer, alpha-blended over passthrough exactly
 * like the selection markers. No per-frame logging.
 */
#ifndef BZ_QUEST_VK_WC3_POINTER_H
#define BZ_QUEST_VK_WC3_POINTER_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "bz_quest_input_state.h"
#include "bz_quest_vk.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Beam = 2 crossed quads * 2 triangles * 3 verts = 12; reticle disc =
     * BZ_QUEST_VK_WC3_POINTER_RETICLE_SEGMENTS triangles * 3. Sized with a
     * little headroom so a geometry tweak can't silently overrun. */
    BZ_QUEST_VK_WC3_POINTER_RETICLE_SEGMENTS = 16,
    BZ_QUEST_VK_WC3_POINTER_VERTS_PER_HAND = 12 + BZ_QUEST_VK_WC3_POINTER_RETICLE_SEGMENTS * 3,
    BZ_QUEST_VK_WC3_POINTER_MAX_VERTS =
        BZ_QUEST_VK_WC3_POINTER_VERTS_PER_HAND * BZ_QUEST_INPUT_HAND_COUNT,
};

typedef struct {
    float position[3];
} bzQuestVkWc3PointerVertex_t;

/* Per-hand pointer geometry the renderer hands to update(), already resolved
 * into LOCAL tracking space. `rayStart` is the controller aim origin;
 * `rayEnd` is the reticle hit point (when hasReticle) or a fallback point
 * along the aim ray. `tint` is the RGBA the whole beam+reticle draws in
 * (chosen by hit kind - e.g. warm amber for a HUD/entity hit, cool cyan for a
 * terrain point, dimmed for no-hit). */
typedef struct {
    bool visible;
    float rayStart[3];
    float rayEnd[3];
    bool hasReticle;
    float reticle[3];
    float tint[4];
} bzQuestVkWc3PointerHand_t;

typedef struct bzQuestVkWc3Pointer_s {
    const bzQuestVk_t *vk;
    VkShaderModule vertexShader, fragmentShader;
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
    /* One host-visible, persistently-mapped dynamic vertex buffer rewritten
     * every frame (the geometry is a handful of verts; a staged device-local
     * upload per frame would be pure overhead). */
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexMemory;
    void *mapped;
    /* Per-hand draw ranges written by update(), consumed by record(). */
    struct {
        bool visible;
        uint32_t firstVertex;
        uint32_t vertexCount;
        float tint[4];
    } draw[BZ_QUEST_INPUT_HAND_COUNT];
    bool haveGeometry;
} bzQuestVkWc3Pointer_t;

bool bz_quest_vk_wc3_pointer_create(const bzQuestVk_t *vk, bzQuestVkWc3Pointer_t *out);
/* Rewrites the vertex buffer + per-hand draw ranges for this frame. Call once
 * per frame (not per eye) before recording. */
void bz_quest_vk_wc3_pointer_update(bzQuestVkWc3Pointer_t *vkPointer,
                                    const bzQuestVkWc3PointerHand_t hands[BZ_QUEST_INPUT_HAND_COUNT]);
bool bz_quest_vk_wc3_pointer_has_geometry(const bzQuestVkWc3Pointer_t *vkPointer);
/* Records the beam+reticle draws into `cmd` (inside the shared render pass)
 * using the plain per-eye view*projection. */
void bz_quest_vk_wc3_pointer_record(bzQuestVkWc3Pointer_t *vkPointer, VkCommandBuffer cmd,
                                    const float viewProj[16]);
void bz_quest_vk_wc3_pointer_destroy(bzQuestVkWc3Pointer_t *vkPointer);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_VK_WC3_POINTER_H */
