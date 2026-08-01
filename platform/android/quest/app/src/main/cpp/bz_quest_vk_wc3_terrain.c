#include "bz_quest_vk_wc3_terrain.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"
#include "bz_quest_shaders_generated.h"
#include "bz_quest_wc3_terrain_capture.h"

enum {
    BZ_QUEST_VK_WC3_TERRAIN_STAGING_BYTES = BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_BYTES,
    BZ_QUEST_VK_WC3_TERRAIN_MAX_BLENDED_DRAWS =
        BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS * (BZ_QUEST_WC3_TERRAIN_MAX_GROUND_TYPES + 1),
};

typedef struct {
    char chunkKey[BZ_QUEST_WC3_TERRAIN_MAX_KEY];
    uint32_t rangeIndex;
    float distanceSq;
} TerrainBlendedDraw_t;

static TerrainBlendedDraw_t s_blendedDraws[BZ_QUEST_VK_WC3_TERRAIN_MAX_BLENDED_DRAWS];
static uint32_t s_blendedDrawCount;
static char s_loggedRangeIdentity[BZ_QUEST_WC3_TERRAIN_MAX_KEY];

static bool find_memory_type(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                             VkMemoryPropertyFlags required, uint32_t *outIndex) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if (!(typeBits & (1u << i))) continue;
        if ((props.memoryTypes[i].propertyFlags & required) != required) continue;
        *outIndex = i;
        return true;
    }
    BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: no memory type matches bits=0x%x flags=0x%x", typeBits,
                  (unsigned)required);
    return false;
}

static bool create_shader_module(VkDevice device, const uint32_t *code, uint32_t codeLen,
                                 VkShaderModule *out) {
    VkShaderModuleCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = codeLen;
    info.pCode = code;
    if (vkCreateShaderModule(device, &info, NULL, out) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreateShaderModule failed");
        return false;
    }
    return true;
}

typedef void (*bzQuestVkWc3TerrainRecordFn)(VkCommandBuffer cmd, void *ctx);
static bool run_one_time_upload(bzQuestVkWc3Terrain_t *vkTerrain, bzQuestVkWc3TerrainRecordFn record,
                                void *ctx) {
    if (vkResetCommandBuffer(vkTerrain->uploadCommandBuffer, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkResetCommandBuffer (upload) failed");
        return false;
    }
    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(vkTerrain->uploadCommandBuffer, &beginInfo) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkBeginCommandBuffer (upload) failed");
        return false;
    }
    record(vkTerrain->uploadCommandBuffer, ctx);
    if (vkEndCommandBuffer(vkTerrain->uploadCommandBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkEndCommandBuffer (upload) failed");
        return false;
    }
    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &vkTerrain->uploadCommandBuffer;
    if (vkQueueSubmit(vkTerrain->vk->queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkQueueSubmit (upload) failed");
        return false;
    }
    if (vkQueueWaitIdle(vkTerrain->vk->queue) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkQueueWaitIdle (upload) failed");
        return false;
    }
    return true;
}

static void *cache_find(const bzQuestWc3Cache_t *cache, const char *identity) {
    for (uint32_t i = 0; i < cache->capacity; i++) {
        if (cache->slots[i].occupied && bz_quest_wc3_identity_equal(cache->slots[i].key.identity, identity))
            return cache->slots[i].handle;
    }
    return NULL;
}

static bool create_device_local_buffer(bzQuestVkWc3Terrain_t *vkTerrain, VkDeviceSize size,
                                       VkBufferUsageFlags usage, VkBuffer *outBuffer,
                                       VkDeviceMemory *outMemory) {
    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vkTerrain->vk->device, &bufferInfo, NULL, outBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreateBuffer failed");
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(vkTerrain->vk->device, *outBuffer, &memReq);
    uint32_t memoryTypeIndex = 0;
    if (!find_memory_type(vkTerrain->vk->physicalDevice, memReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryTypeIndex)) {
        vkDestroyBuffer(vkTerrain->vk->device, *outBuffer, NULL);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(vkTerrain->vk->device, &allocInfo, NULL, outMemory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkAllocateMemory failed");
        vkDestroyBuffer(vkTerrain->vk->device, *outBuffer, NULL);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(vkTerrain->vk->device, *outBuffer, *outMemory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkBindBufferMemory failed");
        vkDestroyBuffer(vkTerrain->vk->device, *outBuffer, NULL);
        vkFreeMemory(vkTerrain->vk->device, *outMemory, NULL);
        *outBuffer = VK_NULL_HANDLE;
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

typedef struct {
    bzQuestVkWc3Terrain_t *vkTerrain;
    uint32_t vertexBytes;
    uint32_t indexBytes;
    VkBuffer vertexBuffer, indexBuffer;
} TerrainChunkUploadCtx_t;

static void record_chunk_upload(VkCommandBuffer cmd, void *ctxPtr) {
    TerrainChunkUploadCtx_t *ctx = (TerrainChunkUploadCtx_t *)ctxPtr;
    VkBufferCopy vertexCopy = {0, 0, ctx->vertexBytes};
    vkCmdCopyBuffer(cmd, ctx->vkTerrain->stagingBuffer, ctx->vertexBuffer, 1, &vertexCopy);
    VkBufferCopy indexCopy = {ctx->vertexBytes, 0, ctx->indexBytes};
    vkCmdCopyBuffer(cmd, ctx->vkTerrain->stagingBuffer, ctx->indexBuffer, 1, &indexCopy);
}

static void compute_range_centers(const bzQuestWc3TerrainChunk_t *chunk, bzQuestVkWc3TerrainChunk_t *out) {
    memset(out->rangeCenters, 0, sizeof(out->rangeCenters));
    for (uint32_t r = 0; r < chunk->meta.drawRangeCount; r++) {
        const bzQuestWc3TerrainDrawRange_t *range = &chunk->meta.drawRanges[r];
        if (!range->indexCount) continue;
        double sum[3] = {0.0, 0.0, 0.0};
        uint32_t vertexCount = 0;
        for (uint32_t i = 0; i < range->indexCount; i++) {
            uint32_t index = chunk->indices[range->indexOffset + i];
            if (index >= chunk->meta.vertexCount) continue;
            const float *p = chunk->vertices[index].position;
            sum[0] += p[0];
            sum[1] += p[1];
            sum[2] += p[2];
            vertexCount++;
        }
        if (!vertexCount) continue;
        out->rangeCenters[r][0] = (float)(sum[0] / vertexCount);
        out->rangeCenters[r][1] = (float)(sum[1] / vertexCount);
        out->rangeCenters[r][2] = (float)(sum[2] / vertexCount);
    }
}

static void *chunk_cache_create(const bzQuestWc3CacheKey_t *key, void *userdata) {
    (void)key;
    bzQuestVkWc3Terrain_t *vkTerrain = (bzQuestVkWc3Terrain_t *)userdata;
    const bzQuestWc3TerrainChunk_t *chunk = vkTerrain->pendingChunk;
    if (!chunk) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: chunk_cache_create with no pending chunk");
        return NULL;
    }
    VkDeviceSize vertexBytes = (VkDeviceSize)chunk->meta.vertexCount * sizeof(bzQuestWc3TerrainVertex_t);
    VkDeviceSize indexBytes = (VkDeviceSize)chunk->meta.indexCount * sizeof(uint32_t);
    if (!vertexBytes || !indexBytes || vertexBytes + indexBytes > BZ_QUEST_VK_WC3_TERRAIN_STAGING_BYTES) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: chunk '%s' has empty or oversized geometry (%llu+%llu bytes)",
                      chunk->meta.terrainIdentity, (unsigned long long)vertexBytes,
                      (unsigned long long)indexBytes);
        return NULL;
    }

    VkBuffer vertexBuffer = VK_NULL_HANDLE, indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE, indexMemory = VK_NULL_HANDLE;
    if (!create_device_local_buffer(vkTerrain, vertexBytes,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    &vertexBuffer, &vertexMemory))
        return NULL;
    if (!create_device_local_buffer(vkTerrain, indexBytes,
                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    &indexBuffer, &indexMemory)) {
        vkDestroyBuffer(vkTerrain->vk->device, vertexBuffer, NULL);
        vkFreeMemory(vkTerrain->vk->device, vertexMemory, NULL);
        return NULL;
    }

    void *mapped = NULL;
    if (vkMapMemory(vkTerrain->vk->device, vkTerrain->stagingMemory, 0, vertexBytes + indexBytes, 0,
                    &mapped) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkMapMemory (chunk staging) failed");
        goto fail;
    }
    memcpy(mapped, chunk->vertices, (size_t)vertexBytes);
    memcpy((uint8_t *)mapped + vertexBytes, chunk->indices, (size_t)indexBytes);
    vkUnmapMemory(vkTerrain->vk->device, vkTerrain->stagingMemory);

    {
        TerrainChunkUploadCtx_t ctx = {vkTerrain, (uint32_t)vertexBytes, (uint32_t)indexBytes, vertexBuffer,
                                       indexBuffer};
        if (!run_one_time_upload(vkTerrain, record_chunk_upload, &ctx)) goto fail;
    }

    bzQuestVkWc3TerrainChunk_t *out = (bzQuestVkWc3TerrainChunk_t *)malloc(sizeof(*out));
    if (!out) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: malloc(chunk gpu) failed");
        goto fail;
    }
    memset(out, 0, sizeof(*out));
    out->vertexBuffer = vertexBuffer;
    out->indexBuffer = indexBuffer;
    out->vertexMemory = vertexMemory;
    out->indexMemory = indexMemory;
    out->meta = chunk->meta;
    compute_range_centers(chunk, out);
    return out;

fail:
    vkDestroyBuffer(vkTerrain->vk->device, vertexBuffer, NULL);
    vkDestroyBuffer(vkTerrain->vk->device, indexBuffer, NULL);
    vkFreeMemory(vkTerrain->vk->device, vertexMemory, NULL);
    vkFreeMemory(vkTerrain->vk->device, indexMemory, NULL);
    return NULL;
}

static void chunk_cache_destroy(void *handle, void *userdata) {
    bzQuestVkWc3Terrain_t *vkTerrain = (bzQuestVkWc3Terrain_t *)userdata;
    bzQuestVkWc3TerrainChunk_t *chunk = (bzQuestVkWc3TerrainChunk_t *)handle;
    if (chunk->vertexBuffer != VK_NULL_HANDLE) vkDestroyBuffer(vkTerrain->vk->device, chunk->vertexBuffer, NULL);
    if (chunk->indexBuffer != VK_NULL_HANDLE) vkDestroyBuffer(vkTerrain->vk->device, chunk->indexBuffer, NULL);
    if (chunk->vertexMemory != VK_NULL_HANDLE) vkFreeMemory(vkTerrain->vk->device, chunk->vertexMemory, NULL);
    if (chunk->indexMemory != VK_NULL_HANDLE) vkFreeMemory(vkTerrain->vk->device, chunk->indexMemory, NULL);
    free(chunk);
}

typedef struct {
    bzQuestVkWc3Terrain_t *vkTerrain;
    VkImage image;
    uint32_t width, height;
} TerrainTextureUploadCtx_t;

static void record_texture_upload(VkCommandBuffer cmd, void *ctxPtr) {
    TerrainTextureUploadCtx_t *ctx = (TerrainTextureUploadCtx_t *)ctxPtr;
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = ctx->width;
    copy.imageExtent.height = ctx->height;
    copy.imageExtent.depth = 1;

    VkImageMemoryBarrier toTransfer = {0};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.image = ctx->image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
                         0, NULL, 1, &toTransfer);

    vkCmdCopyBufferToImage(cmd, ctx->vkTerrain->stagingBuffer, ctx->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    VkImageMemoryBarrier toShaderRead = {0};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.image = ctx->image;
    toShaderRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toShaderRead.subresourceRange.levelCount = 1;
    toShaderRead.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                         0, NULL, 1, &toShaderRead);
}

static void free_texture_gpu(bzQuestVkWc3Terrain_t *vkTerrain, bzQuestVkWc3TerrainTexture_t *tex) {
    if (tex->view != VK_NULL_HANDLE) vkDestroyImageView(vkTerrain->vk->device, tex->view, NULL);
    if (tex->image != VK_NULL_HANDLE) vkDestroyImage(vkTerrain->vk->device, tex->image, NULL);
    if (tex->memory != VK_NULL_HANDLE) vkFreeMemory(vkTerrain->vk->device, tex->memory, NULL);
}

static void *texture_cache_create(const bzQuestWc3CacheKey_t *key, void *userdata) {
    (void)key;
    bzQuestVkWc3Terrain_t *vkTerrain = (bzQuestVkWc3Terrain_t *)userdata;
    if (!vkTerrain->pendingTextureIdentity || !vkTerrain->pendingTexturePixels || !vkTerrain->pendingTextureWidth ||
        !vkTerrain->pendingTextureHeight) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: texture_cache_create missing pending texture");
        return NULL;
    }
    uint32_t dataBytes = vkTerrain->pendingTextureRowBytes * vkTerrain->pendingTextureHeight;
    if (!dataBytes || dataBytes > BZ_QUEST_VK_WC3_TERRAIN_STAGING_BYTES) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: texture '%s' oversized (%u bytes)", vkTerrain->pendingTextureIdentity,
                      dataBytes);
        return NULL;
    }

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateInfo imageInfo = {0};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = vkTerrain->pendingTextureWidth;
    imageInfo.extent.height = vkTerrain->pendingTextureHeight;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(vkTerrain->vk->device, &imageInfo, NULL, &image) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreateImage failed");
        return NULL;
    }
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(vkTerrain->vk->device, image, &memReq);
    uint32_t memoryTypeIndex = 0;
    if (!find_memory_type(vkTerrain->vk->physicalDevice, memReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryTypeIndex)) {
        vkDestroyImage(vkTerrain->vk->device, image, NULL);
        return NULL;
    }
    VkMemoryAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(vkTerrain->vk->device, &allocInfo, NULL, &memory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkAllocateMemory (image) failed");
        vkDestroyImage(vkTerrain->vk->device, image, NULL);
        return NULL;
    }
    if (vkBindImageMemory(vkTerrain->vk->device, image, memory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkBindImageMemory failed");
        vkDestroyImage(vkTerrain->vk->device, image, NULL);
        vkFreeMemory(vkTerrain->vk->device, memory, NULL);
        return NULL;
    }

    void *mapped = NULL;
    if (vkMapMemory(vkTerrain->vk->device, vkTerrain->stagingMemory, 0, dataBytes, 0, &mapped) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkMapMemory (texture staging) failed");
        vkDestroyImage(vkTerrain->vk->device, image, NULL);
        vkFreeMemory(vkTerrain->vk->device, memory, NULL);
        return NULL;
    }
    memcpy(mapped, vkTerrain->pendingTexturePixels, dataBytes);
    vkUnmapMemory(vkTerrain->vk->device, vkTerrain->stagingMemory);

    {
        TerrainTextureUploadCtx_t ctx = {vkTerrain, image, vkTerrain->pendingTextureWidth,
                                         vkTerrain->pendingTextureHeight};
        if (!run_one_time_upload(vkTerrain, record_texture_upload, &ctx)) {
            vkDestroyImage(vkTerrain->vk->device, image, NULL);
            vkFreeMemory(vkTerrain->vk->device, memory, NULL);
            return NULL;
        }
    }

    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo viewInfo = {0};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vkTerrain->vk->device, &viewInfo, NULL, &view) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreateImageView failed");
        vkDestroyImage(vkTerrain->vk->device, image, NULL);
        vkFreeMemory(vkTerrain->vk->device, memory, NULL);
        return NULL;
    }

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocSetInfo = {0};
    allocSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocSetInfo.descriptorPool = vkTerrain->descriptorPool;
    allocSetInfo.descriptorSetCount = 1;
    allocSetInfo.pSetLayouts = &vkTerrain->descriptorSetLayout;
    if (vkAllocateDescriptorSets(vkTerrain->vk->device, &allocSetInfo, &descriptorSet) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkAllocateDescriptorSets failed");
        vkDestroyImageView(vkTerrain->vk->device, view, NULL);
        vkDestroyImage(vkTerrain->vk->device, image, NULL);
        vkFreeMemory(vkTerrain->vk->device, memory, NULL);
        return NULL;
    }

    VkDescriptorImageInfo imageDescriptor = {0};
    imageDescriptor.sampler = vkTerrain->sampler;
    imageDescriptor.imageView = view;
    imageDescriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageDescriptor;
    vkUpdateDescriptorSets(vkTerrain->vk->device, 1, &write, 0, NULL);

    bzQuestVkWc3TerrainTexture_t *out = (bzQuestVkWc3TerrainTexture_t *)malloc(sizeof(*out));
    if (!out) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: malloc(texture gpu) failed");
        VkDescriptorPool pools[1] = {vkTerrain->descriptorPool};
        vkFreeDescriptorSets(vkTerrain->vk->device, pools[0], 1, &descriptorSet);
        vkDestroyImageView(vkTerrain->vk->device, view, NULL);
        vkDestroyImage(vkTerrain->vk->device, image, NULL);
        vkFreeMemory(vkTerrain->vk->device, memory, NULL);
        return NULL;
    }
    out->image = image;
    out->memory = memory;
    out->view = view;
    out->descriptorSet = descriptorSet;
    return out;
}

static void texture_cache_destroy(void *handle, void *userdata) {
    bzQuestVkWc3Terrain_t *vkTerrain = (bzQuestVkWc3Terrain_t *)userdata;
    bzQuestVkWc3TerrainTexture_t *tex = (bzQuestVkWc3TerrainTexture_t *)handle;
    if (tex->descriptorSet != VK_NULL_HANDLE)
        vkFreeDescriptorSets(vkTerrain->vk->device, vkTerrain->descriptorPool, 1, &tex->descriptorSet);
    free_texture_gpu(vkTerrain, tex);
    free(tex);
}

static bool init_caches(bzQuestVkWc3Terrain_t *vkTerrain) {
    memset(&vkTerrain->chunkCache, 0, sizeof(vkTerrain->chunkCache));
    memset(&vkTerrain->textureCache, 0, sizeof(vkTerrain->textureCache));
    if (!bz_quest_wc3_cache_init(&vkTerrain->chunkCache, BZ_QUEST_VK_WC3_TERRAIN_CHUNK_CACHE_CAPACITY,
                                 chunk_cache_create, chunk_cache_destroy, vkTerrain)) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: chunk cache init failed");
        return false;
    }
    if (!bz_quest_wc3_cache_init(&vkTerrain->textureCache, BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_CACHE_CAPACITY,
                                 texture_cache_create, texture_cache_destroy, vkTerrain)) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: texture cache init failed");
        bz_quest_wc3_cache_shutdown(&vkTerrain->chunkCache);
        return false;
    }
    return true;
}

static bool reset_generation_caches(bzQuestVkWc3Terrain_t *vkTerrain) {
    if (vkDeviceWaitIdle(vkTerrain->vk->device) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkDeviceWaitIdle before generation swap failed");
        return false;
    }
    bz_quest_wc3_cache_shutdown(&vkTerrain->chunkCache);
    bz_quest_wc3_cache_shutdown(&vkTerrain->textureCache);
    return init_caches(vkTerrain);
}

static bool create_pipeline(bzQuestVkWc3Terrain_t *vkTerrain, bool blended, VkPipeline *out) {
    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vkTerrain->vertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = vkTerrain->fragmentShader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding = {0, sizeof(bzQuestWc3TerrainVertex_t), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bzQuestWc3TerrainVertex_t, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(bzQuestWc3TerrainVertex_t, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(bzQuestWc3TerrainVertex_t, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput = {0};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {0};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {0};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterization = {0};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {0};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = blended ? VK_FALSE : VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState blendAttachment = {0};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blended) {
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = blendAttachment.dstAlphaBlendFactor =
            VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlend = {0};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    static const VkDynamicState kDynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {0};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = kDynamicStates;

    VkGraphicsPipelineCreateInfo pipelineCreateInfo = {0};
    pipelineCreateInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineCreateInfo.stageCount = 2;
    pipelineCreateInfo.pStages = stages;
    pipelineCreateInfo.pVertexInputState = &vertexInput;
    pipelineCreateInfo.pInputAssemblyState = &inputAssembly;
    pipelineCreateInfo.pViewportState = &viewportState;
    pipelineCreateInfo.pRasterizationState = &rasterization;
    pipelineCreateInfo.pMultisampleState = &multisample;
    pipelineCreateInfo.pDepthStencilState = &depthStencil;
    pipelineCreateInfo.pColorBlendState = &colorBlend;
    pipelineCreateInfo.pDynamicState = &dynamicState;
    pipelineCreateInfo.layout = vkTerrain->pipelineLayout;
    pipelineCreateInfo.renderPass = vkTerrain->vk->renderPass;
    pipelineCreateInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(vkTerrain->vk->device, VK_NULL_HANDLE, 1, &pipelineCreateInfo, NULL, out) !=
        VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreateGraphicsPipelines failed");
        *out = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static const char *texture_identity_for_range(const bzQuestWc3TerrainInput_t *terrain,
                                              const bzQuestWc3TerrainDrawRange_t *range) {
    if (!terrain || !range) return NULL;
    switch (range->materialKind) {
        case BZ_QUEST_WC3_TERRAIN_MATERIAL_GROUND:
            if (range->referencedIndex < terrain->referencedGroundCount)
                return terrain->grounds[range->referencedIndex].identity;
            return NULL;
        case BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF:
            if (range->referencedIndex < terrain->referencedCliffCount)
                return terrain->cliffs[range->referencedIndex].identity;
            return NULL;
        case BZ_QUEST_WC3_TERRAIN_MATERIAL_WATER:
            return terrain->hasWater ? terrain->water.identity : NULL;
        default:
            return NULL;
    }
}

static bool range_is_blended(const bzQuestWc3TerrainDrawRange_t *range) {
    if (range->materialKind == BZ_QUEST_WC3_TERRAIN_MATERIAL_WATER) return true;
    if (range->materialKind == BZ_QUEST_WC3_TERRAIN_MATERIAL_GROUND && range->referencedIndex > 0) return true;
    return false;
}

static float distance_sq3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

static int compare_blended_draw_farthest_first(const void *a, const void *b) {
    const TerrainBlendedDraw_t *da = (const TerrainBlendedDraw_t *)a;
    const TerrainBlendedDraw_t *db = (const TerrainBlendedDraw_t *)b;
    if (da->distanceSq > db->distanceSq) return -1;
    if (da->distanceSq < db->distanceSq) return 1;
    return 0;
}

static void draw_range(VkCommandBuffer cmd, bzQuestVkWc3Terrain_t *vkTerrain,
                       const bzQuestVkWc3TerrainChunk_t *chunkGpu, const bzQuestWc3TerrainInput_t *terrain,
                       const bzQuestWc3TerrainDrawRange_t *range, const float viewProj[16]) {
    const char *textureIdentity = texture_identity_for_range(terrain, range);
    if (!textureIdentity || !textureIdentity[0]) {
        if (!s_loggedRangeIdentity[0] ||
            !bz_quest_wc3_identity_equal(s_loggedRangeIdentity, chunkGpu->meta.terrainIdentity)) {
            snprintf(s_loggedRangeIdentity, sizeof(s_loggedRangeIdentity), "%s", chunkGpu->meta.terrainIdentity);
            BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: range on chunk '%s' missing texture identity",
                          chunkGpu->meta.terrainIdentity);
        }
        return;
    }
    bzQuestVkWc3TerrainTexture_t *tex =
        (bzQuestVkWc3TerrainTexture_t *)cache_find(&vkTerrain->textureCache, textureIdentity);
    if (!tex) return;
    VkPipeline pipeline = range_is_blended(range) ? vkTerrain->blendedPipeline : vkTerrain->opaquePipeline;
    if (pipeline == VK_NULL_HANDLE) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkTerrain->pipelineLayout, 0, 1,
                            &tex->descriptorSet, 0, NULL);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &chunkGpu->vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, chunkGpu->indexBuffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, vkTerrain->pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 16, viewProj);
    vkCmdDrawIndexed(cmd, range->indexCount, 1, range->indexOffset, 0, 0);
}

static bool ensure_texture_uploaded(bzQuestVkWc3Terrain_t *vkTerrain, const char *identity, uint32_t width,
                                    uint32_t height, uint32_t rowBytes, const uint8_t *pixels) {
    bzQuestWc3CacheKey_t key = {0};
    snprintf(key.identity, sizeof(key.identity), "%s", identity ? identity : "");
    if (cache_find(&vkTerrain->textureCache, key.identity)) return true;
    if (vkTerrain->newTextureUploadsThisFrame >= BZ_QUEST_VK_WC3_TERRAIN_MAX_NEW_TEXTURE_UPLOADS_PER_FRAME)
        return false;
    vkTerrain->pendingTextureIdentity = identity;
    vkTerrain->pendingTextureWidth = width;
    vkTerrain->pendingTextureHeight = height;
    vkTerrain->pendingTextureRowBytes = rowBytes;
    vkTerrain->pendingTexturePixels = pixels;
    void *handle = NULL;
    if (!bz_quest_wc3_cache_acquire(&vkTerrain->textureCache, &key, &handle)) return false;
    vkTerrain->newTextureUploadsThisFrame++;
    return true;
}

static void terrain_ready_cb(const bzQuestWc3TerrainInput_t *terrain, void *userdata) {
    bzQuestVkWc3Terrain_t *vkTerrain = (bzQuestVkWc3Terrain_t *)userdata;
    if (!reset_generation_caches(vkTerrain)) return;
    vkTerrain->currentTerrain = *terrain;
    vkTerrain->haveCurrentTerrain = true;
    vkTerrain->currentChunkCount = 0;
    uint32_t chunkCountX = 0, chunkCountZ = 0;
    bz_quest_wc3_terrain_chunk_grid(terrain->tileWidth, terrain->tileHeight, &chunkCountX, &chunkCountZ);
    for (uint32_t z = 0; z < chunkCountZ; z++) {
        for (uint32_t x = 0; x < chunkCountX; x++) {
            if (vkTerrain->currentChunkCount >= BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS) return;
            uint32_t i = vkTerrain->currentChunkCount++;
            vkTerrain->currentChunkX[i] = x;
            vkTerrain->currentChunkZ[i] = z;
            bz_quest_wc3_terrain_chunk_key(terrain->identity, x, z, vkTerrain->currentChunkKeys[i]);
        }
    }
}

static void texture_ready_cb(const char *identity, uint32_t width, uint32_t height, uint32_t rowBytes,
                             const uint8_t *pixels, uint32_t dataBytes, void *userdata) {
    (void)dataBytes;
    bzQuestVkWc3Terrain_t *vkTerrain = (bzQuestVkWc3Terrain_t *)userdata;
    ensure_texture_uploaded(vkTerrain, identity, width, height, rowBytes, pixels);
}

static void upload_missing_chunks(bzQuestVkWc3Terrain_t *vkTerrain) {
    static bzQuestWc3TerrainChunk_t s_chunkScratch;

    if (!vkTerrain->haveCurrentTerrain) return;
    for (uint32_t i = 0; i < vkTerrain->currentChunkCount; i++) {
        const char *keyText = vkTerrain->currentChunkKeys[i];
        bzQuestWc3CacheKey_t key = {0};
        snprintf(key.identity, sizeof(key.identity), "%s", keyText);
        if (cache_find(&vkTerrain->chunkCache, key.identity)) continue;
        if (vkTerrain->newChunkUploadsThisFrame >= BZ_QUEST_VK_WC3_TERRAIN_MAX_NEW_CHUNK_UPLOADS_PER_FRAME) return;
        if (!bz_quest_wc3_terrain_build_chunk(&vkTerrain->currentTerrain, vkTerrain->currentChunkX[i],
                                              vkTerrain->currentChunkZ[i], &s_chunkScratch))
            continue;
        vkTerrain->pendingChunk = &s_chunkScratch;
        void *handle = NULL;
        if (!bz_quest_wc3_cache_acquire(&vkTerrain->chunkCache, &key, &handle)) continue;
        vkTerrain->newChunkUploadsThisFrame++;
    }
}

bool bz_quest_vk_wc3_terrain_create(const bzQuestVk_t *vk, bzQuestVkWc3Terrain_t *out) {
    memset(out, 0, sizeof(*out));
    out->vk = vk;

    VkDescriptorSetLayoutBinding binding = {0};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo = {0};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(vk->device, &layoutInfo, NULL, &out->descriptorSetLayout) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreateDescriptorSetLayout failed");
        goto fail;
    }

    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY};
    VkDescriptorPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY;
    if (vkCreateDescriptorPool(vk->device, &poolInfo, NULL, &out->descriptorPool) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreateDescriptorPool failed");
        goto fail;
    }

    VkSamplerCreateInfo samplerInfo = {0};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = samplerInfo.addressModeV = samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(vk->device, &samplerInfo, NULL, &out->sampler) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreateSampler failed");
        goto fail;
    }

    if (!create_shader_module(vk->device, g_bz_quest_terrain_vert_spv, g_bz_quest_terrain_vert_spv_len,
                              &out->vertexShader) ||
        !create_shader_module(vk->device, g_bz_quest_terrain_frag_spv, g_bz_quest_terrain_frag_spv_len,
                              &out->fragmentShader))
        goto fail;

    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 16};
    VkPipelineLayoutCreateInfo pipelineLayoutInfo = {0};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &out->descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(vk->device, &pipelineLayoutInfo, NULL, &out->pipelineLayout) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreatePipelineLayout failed");
        goto fail;
    }

    if (!create_pipeline(out, false, &out->opaquePipeline) || !create_pipeline(out, true, &out->blendedPipeline))
        goto fail;

    VkBufferCreateInfo stagingBufferInfo = {0};
    stagingBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingBufferInfo.size = BZ_QUEST_VK_WC3_TERRAIN_STAGING_BYTES;
    stagingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk->device, &stagingBufferInfo, NULL, &out->stagingBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkCreateBuffer (staging) failed");
        goto fail;
    }
    VkMemoryRequirements stagingMemReq;
    vkGetBufferMemoryRequirements(vk->device, out->stagingBuffer, &stagingMemReq);
    uint32_t stagingMemoryType = 0;
    if (!find_memory_type(vk->physicalDevice, stagingMemReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &stagingMemoryType))
        goto fail;
    VkMemoryAllocateInfo stagingAlloc = {0};
    stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAlloc.allocationSize = stagingMemReq.size;
    stagingAlloc.memoryTypeIndex = stagingMemoryType;
    if (vkAllocateMemory(vk->device, &stagingAlloc, NULL, &out->stagingMemory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkAllocateMemory (staging) failed");
        goto fail;
    }
    if (vkBindBufferMemory(vk->device, out->stagingBuffer, out->stagingMemory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkBindBufferMemory (staging) failed");
        goto fail;
    }

    VkCommandBufferAllocateInfo cmdAlloc = {0};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = vk->commandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vk->device, &cmdAlloc, &out->uploadCommandBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkAllocateCommandBuffers failed");
        goto fail;
    }

    if (!init_caches(out)) goto fail;
    return true;

fail:
    bz_quest_vk_wc3_terrain_destroy(out);
    return false;
}

void bz_quest_vk_wc3_terrain_capture_and_upload(bzQuestVkWc3Terrain_t *vkTerrain,
                                                bzQuestWc3TerrainRenderList_t *outRenderList) {
    memset(outRenderList, 0, sizeof(*outRenderList));
    vkTerrain->newChunkUploadsThisFrame = 0;
    vkTerrain->newTextureUploadsThisFrame = 0;

    bzQuestWc3TerrainCaptureCallbacks_t callbacks = {terrain_ready_cb, vkTerrain, texture_ready_cb, vkTerrain};
    if (!bz_quest_wc3_terrain_capture(&callbacks)) {
        vkTerrain->haveCurrentTerrain = false;
        vkTerrain->currentChunkCount = 0;
        return;
    }
    upload_missing_chunks(vkTerrain);

    if (!vkTerrain->haveCurrentTerrain) return;
    outRenderList->terrain = &vkTerrain->currentTerrain;
    for (uint32_t i = 0; i < vkTerrain->currentChunkCount; i++) {
        if (!cache_find(&vkTerrain->chunkCache, vkTerrain->currentChunkKeys[i])) continue;
        if (outRenderList->count >= BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS) break;
        snprintf(outRenderList->chunkKeys[outRenderList->count], sizeof(outRenderList->chunkKeys[0]), "%s",
                 vkTerrain->currentChunkKeys[i]);
        outRenderList->count++;
    }
}

void bz_quest_vk_wc3_terrain_record_opaque(bzQuestVkWc3Terrain_t *vkTerrain, VkCommandBuffer cmd,
                                           const float viewProj[16], const float cameraWorldPos[3],
                                           const bzQuestWc3TerrainRenderList_t *list) {
    s_blendedDrawCount = 0;
    if (!list || !list->terrain) return;

    for (uint32_t i = 0; i < list->count; i++) {
        bzQuestVkWc3TerrainChunk_t *chunkGpu =
            (bzQuestVkWc3TerrainChunk_t *)cache_find(&vkTerrain->chunkCache, list->chunkKeys[i]);
        if (!chunkGpu) continue;
        for (uint32_t r = 0; r < chunkGpu->meta.drawRangeCount; r++) {
            const bzQuestWc3TerrainDrawRange_t *range = &chunkGpu->meta.drawRanges[r];
            if (!range->indexCount) continue;
            if (!range_is_blended(range)) {
                draw_range(cmd, vkTerrain, chunkGpu, list->terrain, range, viewProj);
                continue;
            }
            if (s_blendedDrawCount >= BZ_QUEST_VK_WC3_TERRAIN_MAX_BLENDED_DRAWS) continue;
            TerrainBlendedDraw_t *draw = &s_blendedDraws[s_blendedDrawCount++];
            snprintf(draw->chunkKey, sizeof(draw->chunkKey), "%s", list->chunkKeys[i]);
            draw->rangeIndex = r;
            draw->distanceSq = distance_sq3(chunkGpu->rangeCenters[r], cameraWorldPos);
        }
    }
    if (s_blendedDrawCount > 1)
        qsort(s_blendedDraws, s_blendedDrawCount, sizeof(s_blendedDraws[0]), compare_blended_draw_farthest_first);
}

void bz_quest_vk_wc3_terrain_record_blended(bzQuestVkWc3Terrain_t *vkTerrain, VkCommandBuffer cmd,
                                            const float viewProj[16],
                                            const bzQuestWc3TerrainRenderList_t *list) {
    if (!list || !list->terrain) return;
    for (uint32_t i = 0; i < s_blendedDrawCount; i++) {
        bzQuestVkWc3TerrainChunk_t *chunkGpu =
            (bzQuestVkWc3TerrainChunk_t *)cache_find(&vkTerrain->chunkCache, s_blendedDraws[i].chunkKey);
        if (!chunkGpu) continue;
        if (s_blendedDraws[i].rangeIndex >= chunkGpu->meta.drawRangeCount) continue;
        draw_range(cmd, vkTerrain, chunkGpu, list->terrain, &chunkGpu->meta.drawRanges[s_blendedDraws[i].rangeIndex],
                   viewProj);
    }
}

bool bz_quest_vk_wc3_terrain_render_target(bzQuestVkWc3Terrain_t *vkTerrain, uint32_t viewIndex,
                                           uint32_t imageIndex, uint32_t width, uint32_t height,
                                           const float viewProj[16], const float cameraWorldPos[3],
                                           const bzQuestWc3TerrainRenderList_t *list) {
    const bzQuestVk_t *vk = vkTerrain->vk;
    if (viewIndex >= BZ_QUEST_VIEW_COUNT || imageIndex >= vk->targetCount[viewIndex]) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: render_target view=%u image=%u out of range", viewIndex,
                      imageIndex);
        return false;
    }
    const bzQuestVkTarget_t *target = &vk->targets[viewIndex][imageIndex];

    if (vkWaitForFences(vk->device, 1, &target->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkWaitForFences failed");
        return false;
    }
    if (vkResetFences(vk->device, 1, &target->fence) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkResetFences failed");
        return false;
    }
    if (vkResetCommandBuffer(target->commandBuffer, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkResetCommandBuffer failed");
        return false;
    }
    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(target->commandBuffer, &beginInfo) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkBeginCommandBuffer failed");
        return false;
    }

    VkClearValue clearValues[2] = {0};
    clearValues[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo renderPassBeginInfo = {0};
    renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassBeginInfo.renderPass = vk->renderPass;
    renderPassBeginInfo.framebuffer = target->framebuffer;
    renderPassBeginInfo.renderArea.extent.width = width;
    renderPassBeginInfo.renderArea.extent.height = height;
    renderPassBeginInfo.clearValueCount = 2;
    renderPassBeginInfo.pClearValues = clearValues;
    vkCmdBeginRenderPass(target->commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport = {0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
    vkCmdSetViewport(target->commandBuffer, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {width, height}};
    vkCmdSetScissor(target->commandBuffer, 0, 1, &scissor);

    bz_quest_vk_wc3_terrain_record_opaque(vkTerrain, target->commandBuffer, viewProj, cameraWorldPos, list);
    bz_quest_vk_wc3_terrain_record_blended(vkTerrain, target->commandBuffer, viewProj, list);

    vkCmdEndRenderPass(target->commandBuffer);
    if (vkEndCommandBuffer(target->commandBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkEndCommandBuffer failed");
        return false;
    }
    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &target->commandBuffer;
    if (vkQueueSubmit(vk->queue, 1, &submitInfo, target->fence) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkQueueSubmit failed");
        return false;
    }
    if (vkWaitForFences(vk->device, 1, &target->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_terrain: vkWaitForFences (post-submit) failed");
        return false;
    }
    return true;
}

void bz_quest_vk_wc3_terrain_destroy(bzQuestVkWc3Terrain_t *vkTerrain) {
    if (!vkTerrain) return;
    if (vkTerrain->vk && vkTerrain->vk->device != VK_NULL_HANDLE) vkDeviceWaitIdle(vkTerrain->vk->device);
    bz_quest_wc3_cache_shutdown(&vkTerrain->chunkCache);
    bz_quest_wc3_cache_shutdown(&vkTerrain->textureCache);
    if (vkTerrain->uploadCommandBuffer != VK_NULL_HANDLE)
        vkFreeCommandBuffers(vkTerrain->vk->device, vkTerrain->vk->commandPool, 1, &vkTerrain->uploadCommandBuffer);
    if (vkTerrain->stagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(vkTerrain->vk->device, vkTerrain->stagingBuffer, NULL);
    if (vkTerrain->stagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkTerrain->vk->device, vkTerrain->stagingMemory, NULL);
    if (vkTerrain->opaquePipeline != VK_NULL_HANDLE) vkDestroyPipeline(vkTerrain->vk->device, vkTerrain->opaquePipeline, NULL);
    if (vkTerrain->blendedPipeline != VK_NULL_HANDLE) vkDestroyPipeline(vkTerrain->vk->device, vkTerrain->blendedPipeline, NULL);
    if (vkTerrain->vertexShader != VK_NULL_HANDLE) vkDestroyShaderModule(vkTerrain->vk->device, vkTerrain->vertexShader, NULL);
    if (vkTerrain->fragmentShader != VK_NULL_HANDLE) vkDestroyShaderModule(vkTerrain->vk->device, vkTerrain->fragmentShader, NULL);
    if (vkTerrain->pipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(vkTerrain->vk->device, vkTerrain->pipelineLayout, NULL);
    if (vkTerrain->sampler != VK_NULL_HANDLE) vkDestroySampler(vkTerrain->vk->device, vkTerrain->sampler, NULL);
    if (vkTerrain->descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(vkTerrain->vk->device, vkTerrain->descriptorPool, NULL);
    if (vkTerrain->descriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vkTerrain->vk->device, vkTerrain->descriptorSetLayout, NULL);
    memset(vkTerrain, 0, sizeof(*vkTerrain));
}
