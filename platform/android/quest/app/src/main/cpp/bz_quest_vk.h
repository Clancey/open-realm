/*
 * bz_quest_vk.h - Vulkan instance/device/render-target/pipeline ownership
 * for the Quest layer-3 stereo frame loop. The Vulkan instance and device
 * are created *through* OpenXR (XR_KHR_vulkan_enable2's
 * xrCreateVulkanInstanceKHR/xrCreateVulkanDeviceKHR - see bz_quest_xr.h),
 * so this module takes a bzQuestXr_t* rather than owning its own copy of
 * the requirements query.
 *
 * Every function returns bool (true = success) and logs its own failure
 * via BZ_QUEST_LOGE before returning false, matching bz_quest_xr.h's
 * contract.
 */
#ifndef BZ_QUEST_VK_H
#define BZ_QUEST_VK_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "bz_quest_xr.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One set of per-swapchain-image render targets/command recording state,
 * indexed [view][imageIndex]. */
typedef struct bzQuestVkTarget_s {
    VkImageView colorView;
    VkImage depthImage;
    VkDeviceMemory depthMemory;
    VkImageView depthView;
    VkFramebuffer framebuffer;
    VkCommandBuffer commandBuffer;
    VkFence fence; /* signaled == safe to reuse this target's command buffer */
} bzQuestVkTarget_t;

typedef struct bzQuestVk_s {
    VkInstance instance;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    uint32_t queueFamilyIndex;
    VkQueue queue;

    VkRenderPass renderPass;
    VkFormat depthFormat;
    VkPipelineLayout pipelineLayout;
    VkPipeline pipeline;
    VkShaderModule vertexShader;
    VkShaderModule fragmentShader;

    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    uint32_t vertexCount;

    VkCommandPool commandPool;

    uint32_t targetCount[BZ_QUEST_VIEW_COUNT];
    bzQuestVkTarget_t targets[BZ_QUEST_VIEW_COUNT][BZ_QUEST_MAX_SWAPCHAIN_IMAGES];
} bzQuestVk_t;

/* xrCreateVulkanInstanceKHR: builds a VkInstanceCreateInfo with
 * VkApplicationInfo::apiVersion derived from `xrRequirements` (see
 * bz_quest_pure.h's bz_quest_xr_version_to_vk_api_version() for the
 * OpenXR-XrVersion -> Vulkan-VkVersion bit-layout conversion this needs)
 * and lets the OpenXR runtime create+own the resulting VkInstance. Requests
 * no extensions/layers of our own - XR_KHR_vulkan_enable2 has the runtime
 * inject whatever it requires into the instance it creates for us. */
bool bz_quest_vk_create_instance(const bzQuestXr_t *xr,
                                  const XrGraphicsRequirementsVulkanKHR *xrRequirements,
                                  bzQuestVk_t *vk);

/* xrGetVulkanGraphicsDevice2KHR + a graphics-capable queue family search,
 * then xrCreateVulkanDeviceKHR for a single-queue logical device (same
 * "let the runtime inject its required extensions" rationale as the
 * instance). */
bool bz_quest_vk_create_device(const bzQuestXr_t *xr, bzQuestVk_t *vk);

/* Creates the shared render pass (color + depth, single subpass,
 * VK_CULL_MODE_NONE pipeline - see bz_quest_scene.c's top comment for why
 * winding is deliberately not load-bearing here), the vertex/fragment
 * shader modules (from the SPIR-V byte arrays generated at build time by
 * platform/android/quest/scripts/build-shaders.sh - see
 * docs/quest-tabletop.md's "Shader build pipeline"), the graphics pipeline
 * (dynamic viewport/scissor
 * so both eyes share one pipeline despite per-view swapchain extents), and
 * uploads the bz_quest_scene.h test-scene vertex buffer via host-visible
 * memory (no staging copy - the scene is a few hundred vertices, not a
 * perf-sensitive path). `colorFormat` must be the format
 * bz_quest_xr_create_swapchains() actually selected. */
bool bz_quest_vk_create_render_resources(bzQuestVk_t *vk, int64_t colorFormat);

/* Creates one bzQuestVkTarget_t per image in `swapchain` (image view,
 * depth image+view, framebuffer, command buffer, fence-signaled-initially)
 * for view index `viewIndex`. Must run after
 * bz_quest_vk_create_render_resources() (needs vk->renderPass). */
bool bz_quest_vk_create_targets(bzQuestVk_t *vk, uint32_t viewIndex,
                                 const bzQuestXrSwapchain_t *swapchain);

/* Waits for the target's fence (bounding in-flight reuse of its command
 * buffer), records a clear + the test-scene draw with `mvp` as a vertex
 * push constant, and submits to vk->queue signaling the target's fence.
 * Per XR_KHR_vulkan_enable2's synchronization contract (no semaphore/fence
 * handoff to xrEndFrame - the spec requires submitted rendering to have
 * *completed* before the color image is used by xrEndFrame, not merely be
 * submitted), this call blocks on the just-submitted fence before
 * returning: see docs/quest-tabletop.md's "Current limitations" for the
 * CPU/GPU overlap this trades away and why it's the safe default absent
 * physical-device profiling. */
bool bz_quest_vk_render_target(bzQuestVk_t *vk, uint32_t viewIndex, uint32_t imageIndex,
                                uint32_t width, uint32_t height, const float mvp[16]);

/* Reverse-dependency-order teardown: per-target resources across every
 * view, then pipeline/shaders/render pass/vertex buffer/command
 * pool/device/instance. vkDeviceWaitIdle()s first so in-flight fences are
 * never destroyed while signaled-pending. Safe on a partially-initialized
 * vk (every handle checked against VK_NULL_HANDLE). */
void bz_quest_vk_destroy(bzQuestVk_t *vk);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_VK_H */
