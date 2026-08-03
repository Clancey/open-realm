/*
 * bz_quest_vk_wc3_terrain.h - layer 5B: Quest-local Vulkan ownership for
 * Warcraft III terrain chunks/textures.
 *
 * Owns its own descriptor set layout/pool/sampler/pipeline layout/shaders/
 * staging buffer/upload command buffer, entirely separate from both
 * bz_quest_vk.h's procedural scene resources and bz_quest_vk_wc3.h's model
 * resources. Terrain reuse differs from models: a new map generation cannot
 * reuse any old chunk geometry meaningfully, so this module clears both its
 * chunk and texture caches wholesale on a generation swap (after
 * vkDeviceWaitIdle()) instead of trying to preserve per-entry reuse across
 * maps. This keeps ownership/fence safety obvious: no in-flight chunk/image is
 * destroyed before the device goes idle.
 */
#ifndef BZ_QUEST_VK_WC3_TERRAIN_H
#define BZ_QUEST_VK_WC3_TERRAIN_H

#include <stdbool.h>
#include <stdint.h>

#include <vulkan/vulkan.h>

#include "bz_quest_vk.h"
#include "bz_quest_wc3_cache.h"
#include "bz_quest_wc3_terrain.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_VK_WC3_TERRAIN_CHUNK_CACHE_CAPACITY = BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS,
    BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_CACHE_CAPACITY = BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES,
    BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY = BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_CACHE_CAPACITY + 1,
    BZ_QUEST_VK_WC3_TERRAIN_MAX_NEW_CHUNK_UPLOADS_PER_FRAME = 4,
    BZ_QUEST_VK_WC3_TERRAIN_MAX_NEW_TEXTURE_UPLOADS_PER_FRAME = 4,
};

typedef struct {
    VkBuffer vertexBuffer, indexBuffer;
    VkDeviceMemory vertexMemory, indexMemory;
    bzQuestWc3TerrainChunkMeta_t meta;
    float rangeCenters[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES][3];
} bzQuestVkWc3TerrainChunk_t;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet descriptorSet;
} bzQuestVkWc3TerrainTexture_t;

typedef struct {
    uint32_t count;
    char chunkKeys[BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS][BZ_QUEST_WC3_TERRAIN_MAX_KEY];
    const bzQuestWc3TerrainInput_t *terrain;
} bzQuestWc3TerrainRenderList_t;

typedef struct bzQuestVkWc3Terrain_s {
    const bzQuestVk_t *vk;
    VkDescriptorSetLayout descriptorSetLayout;
    VkDescriptorPool descriptorPool;
    VkSampler sampler;
    VkPipelineLayout pipelineLayout;
    VkShaderModule vertexShader;
    VkShaderModule fragmentShader;
    VkPipeline opaquePipeline;
    VkPipeline blendedPipeline;
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkCommandBuffer uploadCommandBuffer;
    bzQuestWc3Cache_t chunkCache;
    bzQuestWc3Cache_t textureCache;
    const bzQuestWc3TerrainChunk_t *pendingChunk;
    const char *pendingTextureIdentity;
    uint32_t pendingTextureWidth, pendingTextureHeight, pendingTextureRowBytes;
    const uint8_t *pendingTexturePixels;
    bzQuestWc3TerrainInput_t currentTerrain;
    bool haveCurrentTerrain;
    uint32_t currentChunkCount;
    uint32_t currentChunkX[BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS];
    uint32_t currentChunkZ[BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS];
    char currentChunkKeys[BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS][BZ_QUEST_WC3_TERRAIN_MAX_KEY];
    uint32_t newChunkUploadsThisFrame;
    uint32_t newTextureUploadsThisFrame;
} bzQuestVkWc3Terrain_t;

bool bz_quest_vk_wc3_terrain_create(const bzQuestVk_t *vk, bzQuestVkWc3Terrain_t *out);
void bz_quest_vk_wc3_terrain_capture_and_upload(bzQuestVkWc3Terrain_t *vkTerrain,
                                                bzQuestWc3TerrainRenderList_t *outRenderList);
void bz_quest_vk_wc3_terrain_record_opaque(bzQuestVkWc3Terrain_t *vkTerrain, VkCommandBuffer cmd,
                                           const float viewProj[16], const float cameraWorldPos[3],
                                           const bzQuestWc3TerrainRenderList_t *list);
void bz_quest_vk_wc3_terrain_record_blended(bzQuestVkWc3Terrain_t *vkTerrain, VkCommandBuffer cmd,
                                            const float viewProj[16],
                                            const bzQuestWc3TerrainRenderList_t *list);
bool bz_quest_vk_wc3_terrain_render_target(bzQuestVkWc3Terrain_t *vkTerrain, uint32_t viewIndex,
                                           uint32_t imageIndex, uint32_t width, uint32_t height,
                                           const float viewProj[16], const float cameraWorldPos[3],
                                           const bzQuestWc3TerrainRenderList_t *list);
void bz_quest_vk_wc3_terrain_destroy(bzQuestVkWc3Terrain_t *vkTerrain);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_VK_WC3_TERRAIN_H */
