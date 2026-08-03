/*
 * bz_quest_vk_wc3_fog.h - layer 5D: Quest-local Vulkan ownership for Warcraft
 * III fog-of-war overlays and selection markers.
 */
#ifndef BZ_QUEST_VK_WC3_FOG_H
#define BZ_QUEST_VK_WC3_FOG_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "bz_quest_vk.h"
#include "bz_quest_wc3_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_VK_WC3_FOG_STAGING_BYTES = BZ_QUEST_WC3_FOG_MAX_CELLS,
    BZ_QUEST_VK_WC3_FOG_MARKER_SEGMENTS = 32,
    BZ_QUEST_VK_WC3_FOG_MARKER_VERTEX_COUNT = BZ_QUEST_VK_WC3_FOG_MARKER_SEGMENTS * 2,
    BZ_QUEST_VK_WC3_FOG_MARKER_INDEX_COUNT = BZ_QUEST_VK_WC3_FOG_MARKER_SEGMENTS * 6,
};

typedef struct {
    float position[3];
} bzQuestVkWc3MarkerVertex_t;

typedef struct bzQuestVkWc3Fog_s {
    const bzQuestVk_t *vk;
    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    VkDescriptorSet descriptorSet;
    VkSampler sampler;
    VkPipelineLayout fogPipelineLayout;
    VkPipelineLayout markerPipelineLayout;
    VkShaderModule fogVertexShader, fogFragmentShader;
    VkShaderModule markerVertexShader, markerFragmentShader;
    VkPipeline fogPipeline, markerPipeline;
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkCommandBuffer uploadCommandBuffer;
    VkImage image;
    VkDeviceMemory imageMemory;
    VkImageView imageView;
    VkBuffer markerVertexBuffer, markerIndexBuffer;
    VkDeviceMemory markerVertexMemory, markerIndexMemory;
    bzQuestWc3FogCapture_t *capture;
    uint8_t *currentPacked, *lastPacked;
    uint32_t width, height, rowBytes, lastPackedBytes;
    uint32_t targetMode;
    bzQuestWc3FogBounds_t bounds;
    /* Copied from capture->transform each frame (see
     * bz_quest_vk_wc3_fog_capture_and_upload()) - the shared world/tabletop
     * transform for THIS bounds, so record_overlay() can place the fog
     * quad's on-screen position (not its raw fragWorld cell-index varying)
     * in the same diorama space as terrain/entities. See
     * bz_quest_wc3_render.h's header comment. */
    bzQuestWc3WorldTransform_t transform;
    bool haveFog;
} bzQuestVkWc3Fog_t;

bool bz_quest_vk_wc3_fog_create(const bzQuestVk_t *vk, bzQuestVkWc3Fog_t *out);
void bz_quest_vk_wc3_fog_capture_and_upload(bzQuestVkWc3Fog_t *vkFog);
bool bz_quest_vk_wc3_fog_has_overlay(const bzQuestVkWc3Fog_t *vkFog);
void bz_quest_vk_wc3_fog_record_overlay(bzQuestVkWc3Fog_t *vkFog, VkCommandBuffer cmd,
                                        const float viewProj[16]);
void bz_quest_vk_wc3_fog_record_selection(bzQuestVkWc3Fog_t *vkFog, VkCommandBuffer cmd,
                                          const float viewProj[16], const bzQuestWc3RenderList_t *list);
void bz_quest_vk_wc3_fog_destroy(bzQuestVkWc3Fog_t *vkFog);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_VK_WC3_FOG_H */
