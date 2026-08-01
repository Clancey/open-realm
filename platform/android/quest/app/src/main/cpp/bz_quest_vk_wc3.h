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
 * bz_quest_vk_wc3_render_target() call.
 */
void bz_quest_vk_wc3_capture_and_upload(bzQuestVkWc3_t *vk3, bzQuestWc3RenderList_t *outRenderList);

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

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_VK_WC3_H */
