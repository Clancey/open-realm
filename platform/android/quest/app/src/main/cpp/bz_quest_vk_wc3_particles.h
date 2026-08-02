/*
 * bz_quest_vk_wc3_particles.h - layer 9: Quest-local Vulkan ownership for
 * PRE2 particle-emitter billboard rendering.
 *
 * Owns its own pipeline-variant cache (keyed by blend mode only - see
 * bzQuestVkWc3ParticlePipelineVariant_t) and one persistently-mapped,
 * host-visible dynamic vertex buffer rewritten every frame from the pure
 * bz_quest_wc3_particles.h pool's bz_quest_wc3_particles_pack() output -
 * matching bz_quest_vk_wc3_pointer.c's own "geometry changes every frame,
 * a staged device-local upload would be pure overhead" rationale exactly
 * (particles are even more dynamic than the ray pointer: hundreds of
 * independently-aging points, not a handful of hand-driven beams).
 *
 * Deliberately does NOT own a second texture cache: a PRE2 emitter's
 * texture is decoded/uploaded through the exact same bz_quest_wc3_capture.c
 * onTextureReady path a material layer's texture already is (see that
 * file's emitter texture-resolution code), landing in bz_quest_vk_wc3.c's
 * OWN existing model texture cache - this module reads it back via the
 * read-only bz_quest_vk_wc3_find_texture() accessor rather than
 * duplicating the upload (DRY - a texture shared between a unit's material
 * and its own weapon-glow emitter is uploaded exactly once). This is a
 * deliberate, narrow exception to the "entirely separately owned" rule the
 * procedural-scene-vs-wc3-renderer split establishes (bz_quest_vk_wc3.h's
 * own header comment) - that separation is about keeping wc3 asset
 * rendering from ever corrupting the unrelated diagnostic scene, not a
 * blanket rule against reuse within the wc3 asset family itself; particle
 * emitters are themselves PRE2 chunks inside wc3 MDX models, not a
 * separate asset family.
 *
 * Billboard expansion happens entirely in warcraft_particle_vert.vert
 * (mirrors renderer/r_particles.c's vs_particle exactly: camera-right/up
 * axes derived from the view*projection matrix's own rows, not a separate
 * inverse-view uniform - see that shader's header comment), so this
 * module's vertex buffer carries only each of the 6 per-particle corners'
 * (position, color, size, atlas-uv, axis) - bz_quest_wc3_particles.h's own
 * bzQuestWc3ParticleVertex_t, reused verbatim as this module's GPU vertex
 * type (no separate Vulkan-specific vertex struct).
 */
#ifndef BZ_QUEST_VK_WC3_PARTICLES_H
#define BZ_QUEST_VK_WC3_PARTICLES_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "bz_quest_vk.h"
#include "bz_quest_vk_wc3.h"
#include "bz_quest_wc3_particles.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* One variant per bzTTBlendMode_t value (7 total: OPAQUE/TRANSPARENT/ALPHA/ADDITIVE/
     * ADD_ALPHA/MODULATE/MODULATE_2X) - see bz_quest_vk_wc3_blend_state_for_mode(). Unlike the
     * model renderer's pipeline-variant cache (56 = 7 blend * 2 cull * 2 depth-test * 2
     * depth-write), particles always use cull-none (a billboard has no meaningful winding) and
     * depth-test-on with each blend mode's own depth-write default (no MDX geoset flags apply
     * to a particle emitter), so blend mode alone selects the variant. */
    BZ_QUEST_VK_WC3_PARTICLE_PIPELINE_VARIANTS = 7,
    /* Vertex buffer capacity: BZ_QUEST_WC3_MAX_PARTICLES particles * 6 verts/particle. */
    BZ_QUEST_VK_WC3_PARTICLE_MAX_VERTS = BZ_QUEST_WC3_MAX_PARTICLES * 6,
};

typedef struct {
    uint32_t blendMode; /* bzTTBlendMode_t */
    VkPipeline pipeline;
} bzQuestVkWc3ParticlePipelineVariant_t;

typedef struct bzQuestVkWc3Particles_s {
    const bzQuestVk_t *vk;           /* borrowed - device/queue/renderPass, never destroyed here */
    const bzQuestVkWc3_t *vk3;       /* borrowed - descriptor set layout + texture cache lookup only */
    VkPipelineLayout pipelineLayout;
    VkShaderModule vertexShader, fragmentShader;
    bzQuestVkWc3ParticlePipelineVariant_t pipelineVariants[BZ_QUEST_VK_WC3_PARTICLE_PIPELINE_VARIANTS];
    uint32_t pipelineVariantCount;

    /* Host-visible, persistently-mapped, rewritten every frame - see this file's header
     * comment on why (no staged device-local upload for data this dynamic). */
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexMemory;
    void *mapped;

    /* This frame's packed draw runs (bz_quest_wc3_particles_pack() output), consumed by
     * bz_quest_vk_wc3_particles_record(). */
    bzQuestWc3ParticleDrawRun_t runs[BZ_QUEST_WC3_MAX_PARTICLE_DRAW_RUNS];
    uint32_t runCount;
} bzQuestVkWc3Particles_t;

/*
 * Creates the pipeline layout (reusing `vk3->descriptorSetLayout` - the SAME set-0 combined-
 * image-sampler layout the model renderer's own per-texture descriptor sets satisfy, so a
 * texture uploaded for a material layer binds here with no separate allocation), shaders, and
 * the persistent vertex buffer. Must run after bz_quest_vk_wc3_create() (needs
 * `vk3->descriptorSetLayout`); `vk`/`vk3` are borrowed and must outlive `out`.
 */
bool bz_quest_vk_wc3_particles_create(const bzQuestVk_t *vk, const bzQuestVkWc3_t *vk3,
                                     bzQuestVkWc3Particles_t *out);

/*
 * Packs `vk3`'s particle pool (bz_quest_vk_wc3_particle_pool()) into this module's vertex
 * buffer + draw runs. Must run once per frame (not once per eye), after
 * bz_quest_vk_wc3_capture_and_upload() (which itself ages the pool - see that function's doc
 * comment) so this frame's spawns/expirations are already reflected.
 */
void bz_quest_vk_wc3_particles_capture_and_upload(bzQuestVkWc3Particles_t *vkParticles);

bool bz_quest_vk_wc3_particles_has_geometry(const bzQuestVkWc3Particles_t *vkParticles);

/*
 * Records one draw call per run (binding that run's own blend-mode pipeline variant and
 * texture descriptor set) into `cmd`'s already-begun render pass, using `viewProj` (this eye's
 * board-folded view*projection - the SAME composed space models/terrain/fog use, not the
 * pointer's plain tracking-space matrix - see bz_quest_renderer.c's mvpBoard). A run whose
 * texture is not (yet) resident in `vk3`'s texture cache is skipped for this frame (transient,
 * matches the model renderer's own "hit path never triggers an upload, a miss just doesn't
 * draw yet" contract), never a placeholder-textured draw.
 */
void bz_quest_vk_wc3_particles_record(bzQuestVkWc3Particles_t *vkParticles, VkCommandBuffer cmd,
                                     const float viewProj[16]);

/*
 * Destroys the vertex buffer, pipeline variants, pipeline layout, and shaders. Never touches
 * `vk3`'s descriptor set layout/texture cache (borrowed, owned elsewhere). Safe on a partially-
 * initialized `vkParticles` (every handle checked against VK_NULL_HANDLE).
 */
void bz_quest_vk_wc3_particles_destroy(bzQuestVkWc3Particles_t *vkParticles);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_VK_WC3_PARTICLES_H */
