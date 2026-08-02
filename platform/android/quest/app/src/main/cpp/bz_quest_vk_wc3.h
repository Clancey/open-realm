/*
 * bz_quest_vk_wc3.h - layer 5A: Vulkan GPU resource ownership/caching/
 * upload and static-geoset draw submission for Warcraft III models.
 *
 * Owns its OWN device resources entirely separately from bz_quest_vk.h's
 * procedural test-scene pipeline/vertex buffer (per this slice's task
 * requirement: "Keep procedural scene resources and Warcraft resources
 * separately owned so missing data/model errors cannot corrupt the
 * diagnostic renderer"). Reaches into `bzQuestVk_t`'s public fields
 * (device/queue/commandPool/renderPass/targets - all public in
 * bz_quest_vk.h, not opaque) to record draws into the *same* render pass
 * instance/framebuffer/command buffer bz_quest_vk_render_target() would
 * otherwise use for the procedural scene, but never touches
 * vk->vertexBuffer/vk->pipeline/vk->pipelineLayout/vk->vertexShader/
 * vk->fragmentShader - those remain exclusively the procedural scene's.
 *
 * All uploads happen synchronously on the calling thread (the Quest XR/
 * render thread - the same thread bz_quest_vk_render_target() already
 * requires, see that header's contract) via one shared, bounded staging
 * buffer: `bz_quest_vk_wc3_ensure_model()`/`_ensure_texture()` map the
 * staging buffer, memcpy, submit a one-time-submit command buffer copying
 * into the real device-local buffer/image, and vkQueueWaitIdle() before
 * returning - the same "block until GPU work completes" discipline
 * bz_quest_vk_render_target() already uses for the exact same
 * XR_KHR_vulkan_enable2 reason (no hidden worker thread, no async
 * multi-frame-in-flight upload queue this slice would need to reason about
 * more carefully - seebz_quest_vk_wc3_upload_budget_t's doc comment for the
 * per-frame bound this buys). No Vulkan call happens from any other
 * thread - bz_quest_wc3_capture.c's ABI-calling code never touches Vulkan,
 * and this module never touches the tabletop asset ABI.
 */
#ifndef BZ_QUEST_VK_WC3_H
#define BZ_QUEST_VK_WC3_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "bz_quest_vk.h"
#include "bz_quest_wc3_cache.h"
#include "bz_quest_wc3_particles.h"
#include "bz_quest_wc3_render.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Bounded by bz_quest_wc3_cache.h's BZ_QUEST_WC3_CACHE_CAPACITY (a
     * fixed-size slots[] array - see bz_quest_wc3_cache_init()'s [1,
     * BZ_QUEST_WC3_CACHE_CAPACITY] contract), which itself equals
     * BZ_QUEST_WC3_MAX_UNIQUE_MODELS_PER_FRAME (128). Using the full
     * capacity for both the model and (separately-instantiated) texture
     * cache is a real capacity ceiling, not a per-frame cap (see
     * bz_quest_wc3_cache.h's FIFO-eviction contract for what happens once
     * these fill: the least-recently-inserted entry is evicted, not an
     * unbounded allocation). */
    BZ_QUEST_VK_WC3_MODEL_CACHE_CAPACITY = BZ_QUEST_WC3_CACHE_CAPACITY,
    BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY = BZ_QUEST_WC3_CACHE_CAPACITY,
    /* bz_quest_wc3_cache_acquire()'s documented order (bz_quest_wc3_cache.h)
     * calls create() BEFORE evicting the oldest entry on a miss at
     * capacity, so it can stay transactional (a failed create() never
     * destroys a still-good cached entry). That means the texture
     * descriptor pool backing texture_create()'s vkAllocateDescriptorSets()
     * call (bz_quest_vk_wc3.c) must be sized for ONE MORE live descriptor
     * set than BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY, not the bare
     * capacity: at the instant the (capacity+1)-th distinct texture is
     * created, `capacity` older sets are still allocated (eviction hasn't
     * run yet), so a pool sized to exactly `capacity` sets always fails
     * that allocation - permanently, since a failed create() never reaches
     * the eviction step that would have freed a set - pinning the cache at
     * `capacity` entries forever. A single spare slot fixes this: the
     * (capacity+1)-th create() succeeds using the spare slot, eviction then
     * runs and frees the oldest set, restoring the pool to `capacity`
     * live sets before the next miss. This constant, not
     * BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY, MUST be used for both
     * VkDescriptorPoolSize::descriptorCount and
     * VkDescriptorPoolCreateInfo::maxSets in bz_quest_vk_wc3_create() - see
     * scripts/test-wc3-descriptor-pool-headroom.sh, which fails the build
     * if a future edit reverts either call site to the bare capacity
     * constant. */
    BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY = BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY + 1,
    /* 7 blend modes * 2 cull modes * 2 depth-test * 2 depth-write = 56
     * theoretically distinct pipelines; real MDX materials only exercise a
     * handful of these combinations, so this is a generous cap, not a
     * per-frame budget - see bz_quest_vk_wc3.c's lazy pipeline-variant
     * cache (never evicted; a genuinely-exhausted cap logs once and reuses
     * the OPAQUE/back-cull/depth-test-on/depth-write-on variant so a
     * pathological asset degrades to an over-drawn-but-still-visible
     * result instead of failing to draw at all). */
    BZ_QUEST_VK_WC3_MAX_PIPELINE_VARIANTS = 56,
    /* Bounds new-upload work per bz_quest_vk_wc3_capture_and_upload() call
     * (see that function's doc comment) so a frame that suddenly touches
     * many never-before-seen models/textures (e.g. right after a map load)
     * cannot stall the render thread for an unbounded number of staging
     * round-trips in one frame - the remainder is simply picked up on
     * later frames (the *entity* is still skipped that frame, matching
     * this slice's "no valid Warcraft render items yet -> diagnostic
     * scene" contract, not a partial/corrupt draw). */
    BZ_QUEST_VK_WC3_MAX_NEW_MODEL_UPLOADS_PER_FRAME = 4,
    BZ_QUEST_VK_WC3_MAX_NEW_TEXTURE_UPLOADS_PER_FRAME = 8,
    /* One extra slot beyond bz_quest_wc3_render.h's
     * BZ_QUEST_WC3_MAX_SKINNED_DRAWS_PER_FRAME budget, permanently reserved
     * for a single identity bone palette (slot 0) - see bz_quest_vk_wc3.c's
     * "palette slot 0" comment. A geoset with no bzQuestWc3ModelAnim_t (a
     * genuinely static model) or one that overflows the per-frame skinned-
     * draw budget binds slot 0 (identity), reproducing the exact same
     * `matrixPalette[i] = node_matrices[...]` / `Matrix4_identity()` fill
     * r_mdx_geoset.c:348-361 already performs for an unused palette slot -
     * so one GPU-skinning vertex shader draws both animated and static
     * geometry uniformly, matching bzQuestWc3Vertex_t's own header comment. */
    BZ_QUEST_VK_WC3_PALETTE_SLOT_COUNT = BZ_QUEST_WC3_MAX_SKINNED_DRAWS_PER_FRAME + 1,
};

typedef struct {
    VkBuffer vertexBuffer, indexBuffer;
    VkDeviceMemory vertexMemory, indexMemory;
    bzQuestWc3ModelMeta_t meta; /* geoset vertex/index ranges + resolved layers */
} bzQuestVkWc3Model_t;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet descriptorSet; /* combined-image-sampler, set 0 binding 0 */
} bzQuestVkWc3Texture_t;

typedef struct {
    uint32_t blendMode; /* bzTTBlendMode_t */
    bool twoSided;
    bool depthTestEnable;
    bool depthWriteEnable;
    VkPipeline pipeline;
} bzQuestVkWc3PipelineVariant_t;

typedef struct bzQuestVkWc3_s {
    const bzQuestVk_t *vk; /* borrowed - device/queue/commandPool/renderPass, never destroyed here */

    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    VkSampler sampler;
    VkPipelineLayout pipelineLayout;
    VkShaderModule vertexShader;
    VkShaderModule fragmentShader;

    /*
     * Bone-palette dynamic-offset UBO (layer 5C GPU skinning) - set 1,
     * binding 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, vertex stage
     * only. Unlike the per-texture descriptor sets above (one set per
     * cached texture, created/destroyed with the texture cache entry),
     * this is a SINGLE descriptor set allocated once at create() and bound
     * with a different dynamic offset per draw - see bz_quest_wc3_render.h's
     * BZ_QUEST_WC3_MAX_SKINNED_DRAWS_PER_FRAME comment and this file's
     * build_frame_dynamic_material()/draw_layer(). paletteBuffer is a single
     * HOST_VISIBLE|HOST_COHERENT, persistently-mapped allocation (no
     * staging round-trip - this data changes every frame, unlike the
     * device-local model/texture data above, so host-visible direct-write
     * is the appropriate choice, matching the task's "bounded per-frame
     * updates" requirement without a second GPU copy per frame).
     * paletteSlotStride is BZ_QUEST_WC3_MAX_MATRIX_PALETTE (128) mat4s
     * (8192 bytes), rounded up to the device's own
     * VkPhysicalDeviceLimits::minUniformBufferOffsetAlignment at create()
     * time (never hardcoded - see bz_quest_vk_wc3_create()), and checked
     * against VkPhysicalDeviceLimits::maxUniformBufferRange so a single
     * dynamic-offset bind's range can never exceed what the device
     * guarantees to support.
     */
    VkDescriptorSetLayout paletteDescriptorSetLayout;
    VkDescriptorPool paletteDescriptorPool;
    VkDescriptorSet paletteDescriptorSet;
    VkBuffer paletteBuffer;
    VkDeviceMemory paletteMemory;
    void *paletteMapped;
    VkDeviceSize paletteSlotStride;
    /* Bounded by BZ_QUEST_WC3_MAX_SKINNED_DRAWS_PER_FRAME (see
     * build_frame_dynamic_material()'s doc comment) - reset to 0 at the top of
     * every bz_quest_vk_wc3_capture_and_upload() call. Slot 0 is never
     * counted here (permanently reserved identity - see this struct's
     * paletteBuffer comment); real per-frame slots start at 1. */
    uint32_t paletteSlotsUsedThisFrame;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    void *stagingMapped;
    VkDeviceSize stagingCapacity;
    VkCommandBuffer uploadCommandBuffer; /* one-time-submit, reused sequentially */

    bzQuestWc3Cache_t modelCache;    /* handle = bzQuestVkWc3Model_t* */
    bzQuestWc3Cache_t textureCache;  /* handle = bzQuestVkWc3Texture_t* */

    bzQuestVkWc3PipelineVariant_t pipelineVariants[BZ_QUEST_VK_WC3_MAX_PIPELINE_VARIANTS];
    uint32_t pipelineVariantCount;

    /* Set immediately before bz_quest_wc3_cache_acquire() so this cache's
     * injected create-callback (whose only per-instance state is this
     * struct, passed as userdata) can see which model/texture is currently
     * being considered - see bz_quest_vk_wc3.c's "pending capture" comment. */
    const bzQuestWc3Model_t *pendingModel;
    const char *pendingTextureIdentity;
    uint32_t pendingTextureWidth, pendingTextureHeight, pendingTextureRowBytes;
    const uint8_t *pendingTexturePixels;
    uint32_t pendingTextureDataBytes;

    uint32_t newModelUploadsThisFrame;
    uint32_t newTextureUploadsThisFrame;

    /* Layer 9: PRE2 particle emitter simulation - see bz_quest_wc3_particles.h. Owned here
     * (a plain value, not a pointer) since particles are conceptually part of "wc3 models";
     * bz_quest_vk_wc3_particles.c (the Vulkan-owning draw module) reads it via
     * bz_quest_vk_wc3_particle_pool() below rather than owning a second copy. `lastClockMsec`/
     * `haveLastClockMsec` let build_frame_dynamic_material() compute one shared
     * previousClockMsec/currentClockMsec delta per FRAME (never per render item - matches
     * desktop's own single tr.viewDef.time/deltaTime, see bz_quest_wc3_particles.h's header
     * comment) across calls. `poolMapEpoch`/`havePoolMapEpoch` detect a real map reload (never
     * a mere snapshot-generation bump) to reset the pool - see
     * bz_quest_wc3_particles_pool_reset()'s "no stale effects across resets" contract. */
    bzQuestWc3ParticlePool_t particlePool;
    uint32_t lastParticleClockMsec;
    bool haveLastParticleClockMsec;
    uint64_t particlePoolMapEpoch;
    bool havePoolMapEpoch;
} bzQuestVkWc3_t;

/*
 * Creates the descriptor set layout/pool/sampler/pipeline layout/shaders
 * and initializes both caches (model/texture) with real Vulkan create/
 * destroy callbacks. Must run after bz_quest_vk_create_render_resources()
 * (needs vk->renderPass/vk->device); `vk` is borrowed and must outlive
 * `out`.
 */
bool bz_quest_vk_wc3_create(const bzQuestVk_t *vk, bzQuestVkWc3_t *out);

/*
 * Runs bz_quest_wc3_capture_frame() (see that header) with callbacks wired
 * to this module's ensure-model/ensure-texture upload paths, bounded by
 * BZ_QUEST_VK_WC3_MAX_NEW_MODEL_UPLOADS_PER_FRAME/
 * BZ_QUEST_VK_WC3_MAX_NEW_TEXTURE_UPLOADS_PER_FRAME (see those constants'
 * doc comments), and fills `outRenderList`. Must run once per frame (not
 * once per eye) on the Quest XR/render thread, before either eye's
 * bz_quest_vk_wc3_render_target() call. `mapEpoch` (BZ_TT identity-of-the-
 * currently-loaded-map, see bz_quest_wc3_capture.h's bz_quest_map_epoch())
 * drives this frame's particle-pool reset check (see
 * bzQuestVkWc3_t::particlePoolMapEpoch's doc comment) - callers already
 * compute this once per frame for layer 6's interaction state machine
 * (bz_quest_renderer.c's renderer->interaction.mapEpoch), so no extra ABI/
 * snapshot round trip is introduced here.
 */
void bz_quest_vk_wc3_capture_and_upload(bzQuestVkWc3_t *vk3, bzQuestWc3RenderList_t *outRenderList,
                                        uint64_t mapEpoch);

/*
 * Read-only accessor for bz_quest_vk_wc3_particles.c (the Vulkan-owning
 * particle draw module) to build this frame's GPU vertex buffer from -
 * mirrors bz_quest_vk_wc3_hud_has_frame()/_frame()'s own "one module reads
 * another's already-computed per-frame state via an accessor" precedent
 * (bz_quest_vk_wc3_hud.h). Never NULL (points at `vk3`'s own field).
 */
const bzQuestWc3ParticlePool_t *bz_quest_vk_wc3_particle_pool(const bzQuestVkWc3_t *vk3);

/*
 * Read-only texture-cache lookup by identity string, exposed so
 * bz_quest_vk_wc3_particles.c can bind an already-uploaded material texture
 * for a particle draw run without owning (or duplicating the upload of) a
 * second texture cache - a PRE2 emitter's texture is decoded/uploaded
 * through the exact same bz_quest_wc3_capture.c onTextureReady path a
 * material layer's texture already is (see bz_quest_wc3_capture.c's emitter
 * texture-resolution code), landing in this SAME cache. Returns NULL (never
 * an error) when the texture has not finished uploading yet this frame -
 * matching this file's own "hit path never triggers an upload, a miss is
 * transient not an error" contract (see this header's "Miss path" doc
 * comment on the model/texture caches above).
 */
const bzQuestVkWc3Texture_t *bz_quest_vk_wc3_find_texture(const bzQuestVkWc3_t *vk3, const char *identity);

/*
 * Records only the non-blended MDX layers into an already-begun render pass
 * and gathers the blended pass's back-to-front draw list for a later
 * bz_quest_vk_wc3_record_blended() call. Calling these two functions back to
 * back reproduces bz_quest_vk_wc3_render_target()'s previous monolithic draw
 * order exactly; the split exists so bz_quest_renderer.c can interleave
 * terrain opaque -> model opaque -> terrain blended -> model blended in one
 * shared eye render pass.
 */
void bz_quest_vk_wc3_record_opaque(bzQuestVkWc3_t *vk3, VkCommandBuffer cmd, const float viewProj[16],
                                   const float cameraWorldPos[3], const bzQuestWc3RenderList_t *list);
void bz_quest_vk_wc3_record_blended(bzQuestVkWc3_t *vk3, VkCommandBuffer cmd, const float viewProj[16],
                                    const bzQuestWc3RenderList_t *list);

/*
 * Records draws for every item in `list` into the target's already-begun
 * render pass instance (viewIndex/imageIndex identify the same
 * bzQuestVkTarget_t bz_quest_vk_render_target() would use - see
 * bz_quest_vk.h's public bzQuestVk_t.targets field), sharing the fence-
 * wait/command-buffer-begin/render-pass-begin/submit/fence-wait sequence
 * bz_quest_vk_render_target() uses for the exact same
 * XR_KHR_vulkan_enable2 synchronization reason (see that function's doc
 * comment) - this is a parallel entry point, not a call *through*
 * bz_quest_vk_render_target(), so the procedural scene's pipeline/vertex
 * buffer are never bound here. `viewProj` is this eye's combined view*
 * projection matrix (bz_quest_pure.h layout); `cameraWorldPos` is this
 * eye's tracked world-space position, used only to sort blended-layer
 * draws back-to-front (see this file's transparency-ordering doc comment
 * in bz_quest_vk_wc3.c). Items whose model identity is not (yet) present
 * in the model cache are skipped for this frame (not drawn with a
 * placeholder) - they will draw once `bz_quest_vk_wc3_capture_and_upload()`
 * has uploaded them, bounded by the per-frame upload caps above.
 */
bool bz_quest_vk_wc3_render_target(bzQuestVkWc3_t *vk3, uint32_t viewIndex, uint32_t imageIndex,
                                   uint32_t width, uint32_t height, const float viewProj[16],
                                   const float cameraWorldPos[3], const bzQuestWc3RenderList_t *list);

/*
 * Reverse-dependency-order teardown: vkDeviceWaitIdle()s first (mirroring
 * bz_quest_vk_destroy()'s own rationale), shuts down both caches (which
 * destroys every still-cached model/texture via their injected destroy
 * callbacks - see bz_quest_wc3_cache.h's shutdown contract), then destroys
 * the staging buffer/sampler/descriptor pool+layout/pipeline layout/
 * shaders/pipeline variants. Safe on a partially-initialized `vk3` (every
 * handle checked against VK_NULL_HANDLE, matching bz_quest_vk_destroy()'s
 * own safety contract).
 */
void bz_quest_vk_wc3_destroy(bzQuestVkWc3_t *vk3);

/*
 * Maps a bzTTBlendMode_t value (mirrored as BZ_QUEST_TTA_BLEND_* in
 * bz_quest_vk_wc3.c - see that file's header comment) to a Vulkan color-
 * blend-attachment state and this blend mode's own depth-write default,
 * reproducing games/warcraft-3/renderer/mdx/r_mdx_geoset.c's
 * MDLX_SetBlendMode() exactly (see bz_quest_vk_wc3.c's header comment for
 * the full per-case citation table). Exposed (not `static`) so
 * bz_quest_vk_wc3_particles.c's own pipeline-variant cache can reuse the
 * identical, already-proven 7-way mapping for PRE2 particle emitters'
 * translated blend mode, rather than a second copy of this switch - DRY,
 * matching AGENTS.md's "no duplicated logic" rule.
 */
void bz_quest_vk_wc3_blend_state_for_mode(uint32_t blendMode, VkPipelineColorBlendAttachmentState *outBlend,
                                          bool *outDepthWriteDefault);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_VK_WC3_H */
