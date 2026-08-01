/*
 * bz_quest_vk_wc3_fog.c - see bz_quest_vk_wc3_fog.h.
 *
 * One sampled R8_UNORM image stores the desktop client's exact fog byte levels
 * (0/128/255) at one byte per cell instead of wasting a full RGBA8 texel; the
 * fragment shader reads the normalized channel directly and maps it to overlay
 * alpha. Selection markers use a separate, tiny procedural annulus mesh and a
 * flat-tint pipeline so no fake asset/configstring path is introduced.
 */
#include "bz_quest_vk_wc3_fog.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"
#include "bz_quest_shaders_generated.h"

#define BZ_QUEST_VK_WC3_FOG_MARKER_INNER_RADIUS 0.82f
#define BZ_QUEST_VK_WC3_FOG_MARKER_LIFT_EPSILON 0.5f

typedef struct {
    float viewProj[16];
    float bounds[4];
    float fogParams[4];
} FogPushConsts_t;

typedef struct {
    float mvp[16];
    float tint[4];
} MarkerPushConsts_t;

typedef void (*bzQuestVkWc3FogRecordFn)(VkCommandBuffer cmd, void *ctx);

typedef struct {
    bzQuestVkWc3Fog_t *vkFog;
    uint32_t width, height, rowLength;
    bool existingImageContent;
} FogUploadCtx_t;

typedef struct {
    bzQuestVkWc3Fog_t *vkFog;
    uint32_t vertexBytes, indexBytes;
} MarkerUploadCtx_t;

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
    BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: no memory type matches bits=0x%x flags=0x%x", typeBits,
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
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateShaderModule failed");
        return false;
    }
    return true;
}

static bool run_one_time_upload(bzQuestVkWc3Fog_t *vkFog, bzQuestVkWc3FogRecordFn record, void *ctx) {
    if (vkResetCommandBuffer(vkFog->uploadCommandBuffer, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkResetCommandBuffer (upload) failed");
        return false;
    }
    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(vkFog->uploadCommandBuffer, &beginInfo) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkBeginCommandBuffer (upload) failed");
        return false;
    }
    record(vkFog->uploadCommandBuffer, ctx);
    if (vkEndCommandBuffer(vkFog->uploadCommandBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkEndCommandBuffer (upload) failed");
        return false;
    }
    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &vkFog->uploadCommandBuffer;
    if (vkQueueSubmit(vkFog->vk->queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkQueueSubmit (upload) failed");
        return false;
    }
    if (vkQueueWaitIdle(vkFog->vk->queue) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkQueueWaitIdle (upload) failed");
        return false;
    }
    return true;
}

static bool create_device_local_buffer(bzQuestVkWc3Fog_t *vkFog, VkDeviceSize size,
                                       VkBufferUsageFlags usage, VkBuffer *outBuffer,
                                       VkDeviceMemory *outMemory) {
    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vkFog->vk->device, &bufferInfo, NULL, outBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateBuffer failed");
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(vkFog->vk->device, *outBuffer, &memReq);
    uint32_t memoryTypeIndex = 0;
    if (!find_memory_type(vkFog->vk->physicalDevice, memReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryTypeIndex)) {
        vkDestroyBuffer(vkFog->vk->device, *outBuffer, NULL);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(vkFog->vk->device, &allocInfo, NULL, outMemory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkAllocateMemory failed");
        vkDestroyBuffer(vkFog->vk->device, *outBuffer, NULL);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(vkFog->vk->device, *outBuffer, *outMemory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkBindBufferMemory failed");
        vkDestroyBuffer(vkFog->vk->device, *outBuffer, NULL);
        vkFreeMemory(vkFog->vk->device, *outMemory, NULL);
        *outBuffer = VK_NULL_HANDLE;
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static void destroy_fog_image(bzQuestVkWc3Fog_t *vkFog) {
    if (vkFog->imageView != VK_NULL_HANDLE) vkDestroyImageView(vkFog->vk->device, vkFog->imageView, NULL);
    if (vkFog->image != VK_NULL_HANDLE) vkDestroyImage(vkFog->vk->device, vkFog->image, NULL);
    if (vkFog->imageMemory != VK_NULL_HANDLE) vkFreeMemory(vkFog->vk->device, vkFog->imageMemory, NULL);
    vkFog->imageView = VK_NULL_HANDLE;
    vkFog->image = VK_NULL_HANDLE;
    vkFog->imageMemory = VK_NULL_HANDLE;
    vkFog->width = vkFog->height = vkFog->rowBytes = vkFog->lastPackedBytes = 0;
}

static bool update_descriptor_image(bzQuestVkWc3Fog_t *vkFog) {
    VkDescriptorImageInfo imageInfo = {0};
    imageInfo.sampler = vkFog->sampler;
    imageInfo.imageView = vkFog->imageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vkFog->descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(vkFog->vk->device, 1, &write, 0, NULL);
    return true;
}

static void record_fog_upload(VkCommandBuffer cmd, void *ctxPtr) {
    FogUploadCtx_t *ctx = (FogUploadCtx_t *)ctxPtr;
    VkImageMemoryBarrier toTransfer = {0};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = ctx->existingImageContent ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = ctx->vkFog->image;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    toTransfer.srcAccessMask = ctx->existingImageContent ? VK_ACCESS_SHADER_READ_BIT : 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         ctx->existingImageContent ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toTransfer);

    VkBufferImageCopy copy = {0};
    copy.bufferRowLength = ctx->rowLength;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = ctx->width;
    copy.imageExtent.height = ctx->height;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, ctx->vkFog->stagingBuffer, ctx->vkFog->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier toRead = {0};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = ctx->vkFog->image;
    toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toRead.subresourceRange.levelCount = 1;
    toRead.subresourceRange.layerCount = 1;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                         0, NULL, 1, &toRead);
}

static void record_marker_upload(VkCommandBuffer cmd, void *ctxPtr) {
    MarkerUploadCtx_t *ctx = (MarkerUploadCtx_t *)ctxPtr;
    VkBufferCopy vertexCopy = {0, 0, ctx->vertexBytes};
    vkCmdCopyBuffer(cmd, ctx->vkFog->stagingBuffer, ctx->vkFog->markerVertexBuffer, 1, &vertexCopy);
    VkBufferCopy indexCopy = {ctx->vertexBytes, 0, ctx->indexBytes};
    vkCmdCopyBuffer(cmd, ctx->vkFog->stagingBuffer, ctx->vkFog->markerIndexBuffer, 1, &indexCopy);
    VkBufferMemoryBarrier barriers[2] = {0};
    barriers[0].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].buffer = ctx->vkFog->markerVertexBuffer;
    barriers[0].size = ctx->vertexBytes;
    barriers[1].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_INDEX_READ_BIT;
    barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[1].buffer = ctx->vkFog->markerIndexBuffer;
    barriers[1].size = ctx->indexBytes;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 2, barriers, 0, NULL);
}

static bool create_fog_image(bzQuestVkWc3Fog_t *vkFog, uint32_t width, uint32_t height) {
    VkImageCreateInfo imageInfo = {0};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vkFog->vk->device, &imageInfo, NULL, &vkFog->image) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateImage failed");
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(vkFog->vk->device, vkFog->image, &memReq);
    uint32_t memoryTypeIndex = 0;
    if (!find_memory_type(vkFog->vk->physicalDevice, memReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memoryTypeIndex))
        return false;
    VkMemoryAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(vkFog->vk->device, &allocInfo, NULL, &vkFog->imageMemory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkAllocateMemory (image) failed");
        return false;
    }
    if (vkBindImageMemory(vkFog->vk->device, vkFog->image, vkFog->imageMemory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkBindImageMemory failed");
        return false;
    }
    VkImageViewCreateInfo viewInfo = {0};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = vkFog->image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vkFog->vk->device, &viewInfo, NULL, &vkFog->imageView) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateImageView failed");
        return false;
    }
    vkFog->width = width;
    vkFog->height = height;
    vkFog->rowBytes = (width + 3u) & ~3u;
    return update_descriptor_image(vkFog);
}

static bool upload_fog_pixels(bzQuestVkWc3Fog_t *vkFog, uint32_t width, uint32_t height,
                              bool existingImageContent) {
    uint32_t rowBytes = (width + 3u) & ~3u;
    uint32_t bytes = rowBytes * height;
    void *mapped = NULL;
    if (vkMapMemory(vkFog->vk->device, vkFog->stagingMemory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkMapMemory (staging) failed");
        return false;
    }
    if (!bz_quest_wc3_fog_pack_texture(vkFog->capture->visible, vkFog->capture->explored, width, height, rowBytes,
                                       (uint8_t *)mapped, bytes)) {
        vkUnmapMemory(vkFog->vk->device, vkFog->stagingMemory);
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: fog packing failed for %ux%u rowBytes=%u", width, height, rowBytes);
        return false;
    }
    vkUnmapMemory(vkFog->vk->device, vkFog->stagingMemory);
    FogUploadCtx_t ctx = {vkFog, width, height, rowBytes, existingImageContent};
    return run_one_time_upload(vkFog, record_fog_upload, &ctx);
}

static bool create_marker_mesh(bzQuestVkWc3Fog_t *vkFog) {
    bzQuestVkWc3MarkerVertex_t verts[BZ_QUEST_VK_WC3_FOG_MARKER_VERTEX_COUNT];
    uint32_t indices[BZ_QUEST_VK_WC3_FOG_MARKER_INDEX_COUNT];
    for (uint32_t i = 0; i < BZ_QUEST_VK_WC3_FOG_MARKER_SEGMENTS; i++) {
        float t = (float)i / (float)BZ_QUEST_VK_WC3_FOG_MARKER_SEGMENTS;
        float angle = t * 6.28318530718f;
        float c = cosf(angle), s = sinf(angle);
        verts[i * 2 + 0].position[0] = c * BZ_QUEST_VK_WC3_FOG_MARKER_INNER_RADIUS;
        verts[i * 2 + 0].position[1] = 0.0f;
        verts[i * 2 + 0].position[2] = s * BZ_QUEST_VK_WC3_FOG_MARKER_INNER_RADIUS;
        verts[i * 2 + 1].position[0] = c;
        verts[i * 2 + 1].position[1] = 0.0f;
        verts[i * 2 + 1].position[2] = s;
    }
    for (uint32_t i = 0; i < BZ_QUEST_VK_WC3_FOG_MARKER_SEGMENTS; i++) {
        uint32_t next = (i + 1) % BZ_QUEST_VK_WC3_FOG_MARKER_SEGMENTS;
        uint32_t ii = i * 6;
        uint32_t inner0 = i * 2, outer0 = inner0 + 1, inner1 = next * 2, outer1 = inner1 + 1;
        indices[ii + 0] = inner0;
        indices[ii + 1] = outer0;
        indices[ii + 2] = outer1;
        indices[ii + 3] = inner0;
        indices[ii + 4] = outer1;
        indices[ii + 5] = inner1;
    }

    VkDeviceSize vertexBytes = sizeof(verts), indexBytes = sizeof(indices);
    if (!create_device_local_buffer(vkFog, vertexBytes,
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    &vkFog->markerVertexBuffer, &vkFog->markerVertexMemory))
        return false;
    if (!create_device_local_buffer(vkFog, indexBytes,
                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    &vkFog->markerIndexBuffer, &vkFog->markerIndexMemory))
        return false;

    void *mapped = NULL;
    if (vkMapMemory(vkFog->vk->device, vkFog->stagingMemory, 0, vertexBytes + indexBytes, 0, &mapped) !=
        VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkMapMemory (marker staging) failed");
        return false;
    }
    memcpy(mapped, verts, (size_t)vertexBytes);
    memcpy((uint8_t *)mapped + vertexBytes, indices, (size_t)indexBytes);
    vkUnmapMemory(vkFog->vk->device, vkFog->stagingMemory);

    MarkerUploadCtx_t ctx = {vkFog, (uint32_t)vertexBytes, (uint32_t)indexBytes};
    return run_one_time_upload(vkFog, record_marker_upload, &ctx);
}

static bool create_fog_pipeline(bzQuestVkWc3Fog_t *vkFog) {
    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vkFog->fogVertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = vkFog->fogFragmentShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput = {0};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {0};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewportState = {0};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization = {0};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil = {0};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blendAttachment = {0};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = blendAttachment.dstAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend = {0};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;
    static const VkDynamicState kDynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {0};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = kDynamicStates;
    VkGraphicsPipelineCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamicState;
    info.layout = vkFog->fogPipelineLayout;
    info.renderPass = vkFog->vk->renderPass;
    info.subpass = 0;
    if (vkCreateGraphicsPipelines(vkFog->vk->device, VK_NULL_HANDLE, 1, &info, NULL, &vkFog->fogPipeline) !=
        VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateGraphicsPipelines (fog) failed");
        return false;
    }
    return true;
}

static bool create_marker_pipeline(bzQuestVkWc3Fog_t *vkFog) {
    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vkFog->markerVertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = vkFog->markerFragmentShader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding = {0, sizeof(bzQuestVkWc3MarkerVertex_t), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(bzQuestVkWc3MarkerVertex_t, position)};
    VkPipelineVertexInputStateCreateInfo vertexInput = {0};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attr;
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
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil = {0};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blendAttachment = {0};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blendAttachment.dstColorBlendFactor = blendAttachment.dstAlphaBlendFactor =
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo colorBlend = {0};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;
    static const VkDynamicState kDynamicStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState = {0};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = kDynamicStates;
    VkGraphicsPipelineCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamicState;
    info.layout = vkFog->markerPipelineLayout;
    info.renderPass = vkFog->vk->renderPass;
    info.subpass = 0;
    if (vkCreateGraphicsPipelines(vkFog->vk->device, VK_NULL_HANDLE, 1, &info, NULL,
                                  &vkFog->markerPipeline) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateGraphicsPipelines (marker) failed");
        return false;
    }
    return true;
}

bool bz_quest_vk_wc3_fog_create(const bzQuestVk_t *vk, bzQuestVkWc3Fog_t *out) {
    memset(out, 0, sizeof(*out));
    out->vk = vk;
    out->capture = (bzQuestWc3FogCapture_t *)calloc(1, sizeof(*out->capture));
    out->currentPacked = (uint8_t *)malloc(BZ_QUEST_WC3_FOG_MAX_CELLS);
    out->lastPacked = (uint8_t *)malloc(BZ_QUEST_WC3_FOG_MAX_CELLS);
    if (!out->capture || !out->currentPacked || !out->lastPacked) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: fog scratch allocation failed");
        goto fail;
    }

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
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateDescriptorSetLayout failed");
        goto fail;
    }

    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(vk->device, &poolInfo, NULL, &out->descriptorPool) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateDescriptorPool failed");
        goto fail;
    }

    VkDescriptorSetAllocateInfo dsAlloc = {0};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = out->descriptorPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &out->descriptorSetLayout;
    if (vkAllocateDescriptorSets(vk->device, &dsAlloc, &out->descriptorSet) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkAllocateDescriptorSets failed");
        goto fail;
    }

    VkSamplerCreateInfo samplerInfo = {0};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    if (vkCreateSampler(vk->device, &samplerInfo, NULL, &out->sampler) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateSampler failed");
        goto fail;
    }

    if (!create_shader_module(vk->device, g_bz_quest_warcraft_fog_vert_spv, g_bz_quest_warcraft_fog_vert_spv_len,
                              &out->fogVertexShader) ||
        !create_shader_module(vk->device, g_bz_quest_warcraft_fog_frag_spv, g_bz_quest_warcraft_fog_frag_spv_len,
                              &out->fogFragmentShader) ||
        !create_shader_module(vk->device, g_bz_quest_warcraft_marker_vert_spv,
                              g_bz_quest_warcraft_marker_vert_spv_len, &out->markerVertexShader) ||
        !create_shader_module(vk->device, g_bz_quest_warcraft_marker_frag_spv,
                              g_bz_quest_warcraft_marker_frag_spv_len, &out->markerFragmentShader))
        goto fail;

    VkPushConstantRange fogPc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FogPushConsts_t)};
    VkPipelineLayoutCreateInfo fogLayoutInfo = {0};
    fogLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    fogLayoutInfo.setLayoutCount = 1;
    fogLayoutInfo.pSetLayouts = &out->descriptorSetLayout;
    fogLayoutInfo.pushConstantRangeCount = 1;
    fogLayoutInfo.pPushConstantRanges = &fogPc;
    if (vkCreatePipelineLayout(vk->device, &fogLayoutInfo, NULL, &out->fogPipelineLayout) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreatePipelineLayout (fog) failed");
        goto fail;
    }

    VkPushConstantRange markerPc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                    sizeof(MarkerPushConsts_t)};
    VkPipelineLayoutCreateInfo markerLayoutInfo = {0};
    markerLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    markerLayoutInfo.pushConstantRangeCount = 1;
    markerLayoutInfo.pPushConstantRanges = &markerPc;
    if (vkCreatePipelineLayout(vk->device, &markerLayoutInfo, NULL, &out->markerPipelineLayout) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreatePipelineLayout (marker) failed");
        goto fail;
    }

    VkBufferCreateInfo stagingInfo = {0};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = BZ_QUEST_VK_WC3_FOG_STAGING_BYTES;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk->device, &stagingInfo, NULL, &out->stagingBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkCreateBuffer (staging) failed");
        goto fail;
    }
    VkMemoryRequirements stagingReq;
    vkGetBufferMemoryRequirements(vk->device, out->stagingBuffer, &stagingReq);
    uint32_t stagingType = 0;
    if (!find_memory_type(vk->physicalDevice, stagingReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &stagingType))
        goto fail;
    VkMemoryAllocateInfo stagingAlloc = {0};
    stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAlloc.allocationSize = stagingReq.size;
    stagingAlloc.memoryTypeIndex = stagingType;
    if (vkAllocateMemory(vk->device, &stagingAlloc, NULL, &out->stagingMemory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkAllocateMemory (staging) failed");
        goto fail;
    }
    if (vkBindBufferMemory(vk->device, out->stagingBuffer, out->stagingMemory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkBindBufferMemory (staging) failed");
        goto fail;
    }

    VkCommandBufferAllocateInfo cmdAlloc = {0};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = vk->commandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vk->device, &cmdAlloc, &out->uploadCommandBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkAllocateCommandBuffers failed");
        goto fail;
    }

    if (!create_marker_mesh(out) || !create_fog_pipeline(out) || !create_marker_pipeline(out)) goto fail;
    return true;

fail:
    bz_quest_vk_wc3_fog_destroy(out);
    return false;
}

void bz_quest_vk_wc3_fog_capture_and_upload(bzQuestVkWc3Fog_t *vkFog) {
    if (!bz_quest_wc3_capture_fog(vkFog->capture)) {
        vkFog->haveFog = false;
        vkFog->targetMode = 0;
        if (vkFog->image != VK_NULL_HANDLE && vkDeviceWaitIdle(vkFog->vk->device) == VK_SUCCESS) destroy_fog_image(vkFog);
        return;
    }
    vkFog->bounds = vkFog->capture->bounds;
    vkFog->targetMode = vkFog->capture->targetMode;

    uint32_t cells = bz_quest_wc3_fog_cell_count(vkFog->capture->width, vkFog->capture->height);
    if (!bz_quest_wc3_fog_pack_texture(vkFog->capture->visible, vkFog->capture->explored,
                                       vkFog->capture->width, vkFog->capture->height, vkFog->capture->width,
                                       vkFog->currentPacked, BZ_QUEST_WC3_FOG_MAX_CELLS)) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: tight fog pack failed for %ux%u", vkFog->capture->width,
                      vkFog->capture->height);
        vkFog->haveFog = false;
        return;
    }

    bool dimsChanged = vkFog->width != vkFog->capture->width || vkFog->height != vkFog->capture->height;
    bool dirty = dimsChanged || !vkFog->haveFog ||
                 bz_quest_wc3_fog_bytes_differ(vkFog->currentPacked, cells, vkFog->lastPacked, vkFog->lastPackedBytes);
    if (dimsChanged && vkDeviceWaitIdle(vkFog->vk->device) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_fog: vkDeviceWaitIdle before fog resize failed");
        vkFog->haveFog = false;
        return;
    }
    if (dimsChanged) {
        destroy_fog_image(vkFog);
        if (!create_fog_image(vkFog, vkFog->capture->width, vkFog->capture->height)) {
            vkFog->haveFog = false;
            destroy_fog_image(vkFog);
            return;
        }
    }
    if (dirty && !upload_fog_pixels(vkFog, vkFog->capture->width, vkFog->capture->height,
                                  vkFog->haveFog && !dimsChanged)) {
        vkFog->haveFog = false;
        return;
    }
    if (dirty) {
        memcpy(vkFog->lastPacked, vkFog->currentPacked, cells);
        vkFog->lastPackedBytes = cells;
    }
    vkFog->haveFog = true;
}

bool bz_quest_vk_wc3_fog_has_overlay(const bzQuestVkWc3Fog_t *vkFog) {
    return vkFog && vkFog->haveFog && vkFog->imageView != VK_NULL_HANDLE && vkFog->width > 0 && vkFog->height > 0;
}

void bz_quest_vk_wc3_fog_record_overlay(bzQuestVkWc3Fog_t *vkFog, VkCommandBuffer cmd,
                                        const float viewProj[16]) {
    if (!bz_quest_vk_wc3_fog_has_overlay(vkFog) || vkFog->fogPipeline == VK_NULL_HANDLE) return;
    FogPushConsts_t pc;
    memcpy(pc.viewProj, viewProj, sizeof(pc.viewProj));
    pc.bounds[0] = vkFog->bounds.minX;
    pc.bounds[1] = vkFog->bounds.minY;
    pc.bounds[2] = vkFog->bounds.maxX;
    pc.bounds[3] = vkFog->bounds.maxY;
    pc.fogParams[0] = (float)vkFog->width;
    pc.fogParams[1] = (float)vkFog->height;
    pc.fogParams[2] = BZ_QUEST_WC3_FOG_CELL_SIZE;
    pc.fogParams[3] = 0.0f;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkFog->fogPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkFog->fogPipelineLayout, 0, 1,
                            &vkFog->descriptorSet, 0, NULL);
    vkCmdPushConstants(cmd, vkFog->fogPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 4, 1, 0, 0);
}

void bz_quest_vk_wc3_fog_record_selection(bzQuestVkWc3Fog_t *vkFog, VkCommandBuffer cmd,
                                          const float viewProj[16], const bzQuestWc3RenderList_t *list) {
    if (!vkFog || !list || vkFog->markerPipeline == VK_NULL_HANDLE) return;
    VkDeviceSize offset = 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkFog->markerPipeline);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vkFog->markerVertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, vkFog->markerIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
    for (uint32_t i = 0; i < list->count; i++) {
        const bzQuestWc3RenderItem_t *item = &list->items[i];
        if (!item->selected) continue;
        bzQuestWc3SelectionMarker_t marker;
        if (!bz_quest_wc3_selection_marker_from_translation(item->world[12], item->world[13], item->world[14],
                                                            item->radius, item->tintR, item->tintG, item->tintB,
                                                            item->tintA, &marker))
            continue;
        marker.world[13] += BZ_QUEST_VK_WC3_FOG_MARKER_LIFT_EPSILON;
        MarkerPushConsts_t pc;
        bz_quest_mat4_multiply(viewProj, marker.world, pc.mvp);
        memcpy(pc.tint, marker.tint, sizeof(pc.tint));
        vkCmdPushConstants(cmd, vkFog->markerPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, BZ_QUEST_VK_WC3_FOG_MARKER_INDEX_COUNT, 1, 0, 0, 0);
    }
}

void bz_quest_vk_wc3_fog_destroy(bzQuestVkWc3Fog_t *vkFog) {
    if (!vkFog || !vkFog->vk) {
        if (vkFog) {
            free(vkFog->capture);
            free(vkFog->currentPacked);
            free(vkFog->lastPacked);
            memset(vkFog, 0, sizeof(*vkFog));
        }
        return;
    }
    vkDeviceWaitIdle(vkFog->vk->device);
    destroy_fog_image(vkFog);
    if (vkFog->markerVertexBuffer != VK_NULL_HANDLE) vkDestroyBuffer(vkFog->vk->device, vkFog->markerVertexBuffer, NULL);
    if (vkFog->markerIndexBuffer != VK_NULL_HANDLE) vkDestroyBuffer(vkFog->vk->device, vkFog->markerIndexBuffer, NULL);
    if (vkFog->markerVertexMemory != VK_NULL_HANDLE) vkFreeMemory(vkFog->vk->device, vkFog->markerVertexMemory, NULL);
    if (vkFog->markerIndexMemory != VK_NULL_HANDLE) vkFreeMemory(vkFog->vk->device, vkFog->markerIndexMemory, NULL);
    if (vkFog->stagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(vkFog->vk->device, vkFog->stagingBuffer, NULL);
    if (vkFog->stagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkFog->vk->device, vkFog->stagingMemory, NULL);
    if (vkFog->fogPipeline != VK_NULL_HANDLE) vkDestroyPipeline(vkFog->vk->device, vkFog->fogPipeline, NULL);
    if (vkFog->markerPipeline != VK_NULL_HANDLE) vkDestroyPipeline(vkFog->vk->device, vkFog->markerPipeline, NULL);
    if (vkFog->fogPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vkFog->vk->device, vkFog->fogPipelineLayout, NULL);
    if (vkFog->markerPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vkFog->vk->device, vkFog->markerPipelineLayout, NULL);
    if (vkFog->fogVertexShader != VK_NULL_HANDLE) vkDestroyShaderModule(vkFog->vk->device, vkFog->fogVertexShader, NULL);
    if (vkFog->fogFragmentShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(vkFog->vk->device, vkFog->fogFragmentShader, NULL);
    if (vkFog->markerVertexShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(vkFog->vk->device, vkFog->markerVertexShader, NULL);
    if (vkFog->markerFragmentShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(vkFog->vk->device, vkFog->markerFragmentShader, NULL);
    if (vkFog->sampler != VK_NULL_HANDLE) vkDestroySampler(vkFog->vk->device, vkFog->sampler, NULL);
    if (vkFog->descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(vkFog->vk->device, vkFog->descriptorPool, NULL);
    if (vkFog->descriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vkFog->vk->device, vkFog->descriptorSetLayout, NULL);
    if (vkFog->uploadCommandBuffer != VK_NULL_HANDLE)
        vkFreeCommandBuffers(vkFog->vk->device, vkFog->vk->commandPool, 1, &vkFog->uploadCommandBuffer);
    free(vkFog->capture);
    free(vkFog->currentPacked);
    free(vkFog->lastPacked);
    memset(vkFog, 0, sizeof(*vkFog));
}
