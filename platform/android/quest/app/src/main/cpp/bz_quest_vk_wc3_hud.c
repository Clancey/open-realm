/*
 * bz_quest_vk_wc3_hud.c - see bz_quest_vk_wc3_hud.h.
 */
#include "bz_quest_vk_wc3_hud.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"
#include "bz_quest_shaders_generated.h"

typedef struct {
    float mvp[16];
} bzQuestVkWc3HudPushConsts_t;

typedef void (*bzQuestVkWc3HudRecordFn)(VkCommandBuffer cmd, void *ctx);

typedef struct {
    bzQuestVkWc3Hud_t *vkHud;
} FontUploadCtx_t;

/* Bounded scratch for this frame's rebuilt vertex/index arrays - never
 * stack-allocated (see bz_quest_wc3_render.h's bzQuestWc3Model_t doc
 * comment for this same convention elsewhere in this Quest port); capture
 * is single-threaded, called once per bz_quest_renderer_frame(). */
static bzQuestVkWc3HudPanelVertex_t s_panelVerts[BZ_QUEST_VK_WC3_HUD_MAX_PANEL_VERTICES];
static uint32_t s_panelIndices[BZ_QUEST_VK_WC3_HUD_MAX_PANEL_INDICES];
static bzQuestVkWc3HudTextVertex_t s_textVerts[BZ_QUEST_VK_WC3_HUD_MAX_TEXT_VERTICES];
static uint32_t s_textIndices[BZ_QUEST_VK_WC3_HUD_MAX_TEXT_INDICES];

/* This file's own copy of capture.c's/bz_quest_vk_wc3.c's log-once idiom
 * (see either file's own copy for the same rationale) - deliberately
 * file-local rather than shared, matching this project's existing
 * convention of one dedup table per translation unit (each file's own
 * diagnostics are about that file's own decisions). Used below to turn
 * bz_quest_wc3_hud_font.h's "unsupported byte" / "truncated run" pure
 * return signals into the promised once-per-unique-condition visible logs,
 * since bz_quest_wc3_hud_font.c/bz_quest_wc3_hud.c are pure and never log
 * themselves (see those files' header comments) - this impure capture
 * layer is where that responsibility actually lives. */
enum { BZ_QUEST_VK_WC3_HUD_MAX_LOGGED_KEYS = 256 };
static char s_vkWc3HudLoggedKeys[BZ_QUEST_VK_WC3_HUD_MAX_LOGGED_KEYS][BZ_QUEST_WC3_MAX_IDENTITY + 32];
static uint32_t s_vkWc3HudLoggedKeyCount;

static bool vk_hud_log_once(const char *identity, const char *detail) {
    char key[BZ_QUEST_WC3_MAX_IDENTITY + 32];
    snprintf(key, sizeof(key), "%s|%s", identity, detail);
    for (uint32_t i = 0; i < s_vkWc3HudLoggedKeyCount; i++)
        if (strcmp(s_vkWc3HudLoggedKeys[i], key) == 0) return false;
    if (s_vkWc3HudLoggedKeyCount < BZ_QUEST_VK_WC3_HUD_MAX_LOGGED_KEYS) {
        strncpy(s_vkWc3HudLoggedKeys[s_vkWc3HudLoggedKeyCount], key, sizeof(s_vkWc3HudLoggedKeys[0]) - 1);
        s_vkWc3HudLoggedKeyCount++;
    }
    return true;
}
#define VK_WC3_HUD_LOG_ONCE(identity, detail, ...)                                                   \
    do {                                                                                             \
        if (vk_hud_log_once((identity), (detail))) fprintf(stderr, __VA_ARGS__);                     \
    } while (0)

static bool find_memory_type(VkPhysicalDevice physicalDevice, uint32_t typeBits, VkMemoryPropertyFlags required,
                             uint32_t *outIndex) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if (!(typeBits & (1u << i))) continue;
        if ((props.memoryTypes[i].propertyFlags & required) != required) continue;
        *outIndex = i;
        return true;
    }
    BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: no memory type matches bits=0x%x flags=0x%x", typeBits, (unsigned)required);
    return false;
}

static bool create_shader_module(VkDevice device, const uint32_t *code, uint32_t codeLen, VkShaderModule *out) {
    VkShaderModuleCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = codeLen;
    info.pCode = code;
    if (vkCreateShaderModule(device, &info, NULL, out) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateShaderModule failed");
        return false;
    }
    return true;
}

static bool run_one_time_upload(bzQuestVkWc3Hud_t *vkHud, bzQuestVkWc3HudRecordFn record, void *ctx) {
    if (vkResetCommandBuffer(vkHud->uploadCommandBuffer, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkResetCommandBuffer (upload) failed");
        return false;
    }
    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(vkHud->uploadCommandBuffer, &beginInfo) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkBeginCommandBuffer (upload) failed");
        return false;
    }
    record(vkHud->uploadCommandBuffer, ctx);
    if (vkEndCommandBuffer(vkHud->uploadCommandBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkEndCommandBuffer (upload) failed");
        return false;
    }
    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &vkHud->uploadCommandBuffer;
    if (vkQueueSubmit(vkHud->vk->queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkQueueSubmit (upload) failed");
        return false;
    }
    if (vkQueueWaitIdle(vkHud->vk->queue) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkQueueWaitIdle (upload) failed");
        return false;
    }
    return true;
}

static bool create_host_visible_buffer(bzQuestVkWc3Hud_t *vkHud, VkDeviceSize size, VkBufferUsageFlags usage,
                                       VkBuffer *outBuffer, VkDeviceMemory *outMemory, void **outMapped) {
    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vkHud->vk->device, &bufferInfo, NULL, outBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateBuffer (dynamic) failed");
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(vkHud->vk->device, *outBuffer, &memReq);
    uint32_t memoryTypeIndex = 0;
    if (!find_memory_type(vkHud->vk->physicalDevice, memReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &memoryTypeIndex)) {
        vkDestroyBuffer(vkHud->vk->device, *outBuffer, NULL);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(vkHud->vk->device, &allocInfo, NULL, outMemory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkAllocateMemory (dynamic) failed");
        vkDestroyBuffer(vkHud->vk->device, *outBuffer, NULL);
        *outBuffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(vkHud->vk->device, *outBuffer, *outMemory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkBindBufferMemory (dynamic) failed");
        vkDestroyBuffer(vkHud->vk->device, *outBuffer, NULL);
        vkFreeMemory(vkHud->vk->device, *outMemory, NULL);
        *outBuffer = VK_NULL_HANDLE;
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    /* Mapped exactly once, for this buffer's whole lifetime - see this
     * file's header comment on in-flight safety. */
    if (vkMapMemory(vkHud->vk->device, *outMemory, 0, size, 0, outMapped) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkMapMemory (dynamic, persistent) failed");
        vkDestroyBuffer(vkHud->vk->device, *outBuffer, NULL);
        vkFreeMemory(vkHud->vk->device, *outMemory, NULL);
        *outBuffer = VK_NULL_HANDLE;
        *outMemory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static void record_font_upload(VkCommandBuffer cmd, void *ctxPtr) {
    FontUploadCtx_t *ctx = (FontUploadCtx_t *)ctxPtr;
    bzQuestVkWc3Hud_t *vkHud = ctx->vkHud;
    VkImageMemoryBarrier toTransfer = {0};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = vkHud->fontImage;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.layerCount = 1;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                         1, &toTransfer);

    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = BZ_QUEST_HUD_FONT_ATLAS_WIDTH;
    copy.imageExtent.height = BZ_QUEST_HUD_FONT_ATLAS_HEIGHT;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, vkHud->stagingBuffer, vkHud->fontImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    VkImageMemoryBarrier toRead = {0};
    toRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toRead.image = vkHud->fontImage;
    toRead.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toRead.subresourceRange.levelCount = 1;
    toRead.subresourceRange.layerCount = 1;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                         NULL, 1, &toRead);
}

static bool update_descriptor_image(bzQuestVkWc3Hud_t *vkHud) {
    VkDescriptorImageInfo imageInfo = {0};
    imageInfo.sampler = vkHud->sampler;
    imageInfo.imageView = vkHud->fontImageView;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vkHud->descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(vkHud->vk->device, 1, &write, 0, NULL);
    return true;
}

/*
 * Tears down whatever of the font image/memory/view actually got created,
 * resetting every handle to VK_NULL_HANDLE (and haveFont to false) even if
 * called after only a PARTIAL create_font_atlas() (see that function's
 * comment on why every one of its failure paths below calls this instead
 * of just `return false`) - each `if` is independent because a failure
 * partway through image creation can leave some handles live and others
 * still VK_NULL_HANDLE. Also the ordinary full-teardown path from
 * bz_quest_vk_wc3_hud_destroy().
 */
static void destroy_font_image(bzQuestVkWc3Hud_t *vkHud) {
    if (vkHud->fontImageView != VK_NULL_HANDLE) vkDestroyImageView(vkHud->vk->device, vkHud->fontImageView, NULL);
    if (vkHud->fontImage != VK_NULL_HANDLE) vkDestroyImage(vkHud->vk->device, vkHud->fontImage, NULL);
    if (vkHud->fontImageMemory != VK_NULL_HANDLE) vkFreeMemory(vkHud->vk->device, vkHud->fontImageMemory, NULL);
    vkHud->fontImageView = VK_NULL_HANDLE;
    vkHud->fontImage = VK_NULL_HANDLE;
    vkHud->fontImageMemory = VK_NULL_HANDLE;
    vkHud->haveFont = false;
}

/*
 * Creates the font atlas image/view and uploads its one, fixed pixel
 * content (bz_quest_wc3_hud_font_build_atlas() - a pure, deterministic
 * build-time asset, never per-frame game state). Called once at
 * bz_quest_vk_wc3_hud_create() time and retried (same call, same
 * idempotent no-op-if-already-created guard) from
 * bz_quest_vk_wc3_hud_capture_and_upload() every frame until it succeeds,
 * per this slice's "pending resources retry without repeated ABI pixel
 * copies or log spam" requirement - logged once via BZ_QUEST_LOGE (Android
 * log already dedups identical repeated lines at the OS level; this is the
 * one-time, not per-frame-successful, code path so it cannot spam either
 * way).
 *
 * Every step below writes DIRECTLY into vkHud->fontImage/fontImageMemory/
 * fontImageView (one owner, never a shadow set of locals committed only on
 * success) - so on ANY failure past vkCreateImage, this function calls
 * destroy_font_image() before returning false, tearing down exactly
 * whatever got created and resetting every handle to VK_NULL_HANDLE. Without
 * this, a mid-sequence failure (e.g. vkAllocateMemory out of device
 * memory) would leave `vkHud->fontImage` pointing at a real, still-alive
 * VkImage while `haveFont` stays false - the NEXT frame's retry would then
 * call vkCreateImage again and overwrite that handle, leaking the image
 * (and its bound memory, if allocation had gotten that far) every single
 * frame until the retry eventually succeeds or the process exits.
 */
static bool create_font_atlas(bzQuestVkWc3Hud_t *vkHud) {
    if (vkHud->haveFont) return true;

    void *mapped = NULL;
    if (vkMapMemory(vkHud->vk->device, vkHud->stagingMemory, 0, BZ_QUEST_HUD_FONT_ATLAS_BYTES, 0, &mapped) !=
        VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkMapMemory (font staging) failed");
        return false;
    }
    bool packed = bz_quest_wc3_hud_font_build_atlas((uint8_t *)mapped, BZ_QUEST_HUD_FONT_ATLAS_BYTES);
    vkUnmapMemory(vkHud->vk->device, vkHud->stagingMemory);
    if (!packed) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: bz_quest_wc3_hud_font_build_atlas failed");
        return false;
    }

    VkImageCreateInfo imageInfo = {0};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8_UNORM;
    imageInfo.extent.width = BZ_QUEST_HUD_FONT_ATLAS_WIDTH;
    imageInfo.extent.height = BZ_QUEST_HUD_FONT_ATLAS_HEIGHT;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vkHud->vk->device, &imageInfo, NULL, &vkHud->fontImage) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateImage (font) failed");
        destroy_font_image(vkHud);
        return false;
    }
    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(vkHud->vk->device, vkHud->fontImage, &memReq);
    uint32_t memoryTypeIndex = 0;
    if (!find_memory_type(vkHud->vk->physicalDevice, memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                          &memoryTypeIndex)) {
        destroy_font_image(vkHud);
        return false;
    }
    VkMemoryAllocateInfo allocInfo = {0};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(vkHud->vk->device, &allocInfo, NULL, &vkHud->fontImageMemory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkAllocateMemory (font image) failed");
        destroy_font_image(vkHud);
        return false;
    }
    if (vkBindImageMemory(vkHud->vk->device, vkHud->fontImage, vkHud->fontImageMemory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkBindImageMemory (font) failed");
        destroy_font_image(vkHud);
        return false;
    }
    VkImageViewCreateInfo viewInfo = {0};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = vkHud->fontImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vkHud->vk->device, &viewInfo, NULL, &vkHud->fontImageView) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateImageView (font) failed");
        destroy_font_image(vkHud);
        return false;
    }
    if (!update_descriptor_image(vkHud)) {
        destroy_font_image(vkHud);
        return false;
    }

    FontUploadCtx_t ctx = {vkHud};
    if (!run_one_time_upload(vkHud, record_font_upload, &ctx)) {
        destroy_font_image(vkHud);
        return false;
    }
    vkHud->haveFont = true;
    return true;
}

static bool create_panel_pipeline(bzQuestVkWc3Hud_t *vkHud) {
    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vkHud->panelVertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = vkHud->panelFragmentShader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding = {0, sizeof(bzQuestVkWc3HudPanelVertex_t), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bzQuestVkWc3HudPanelVertex_t, pos)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(bzQuestVkWc3HudPanelVertex_t, color)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput = {0};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
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
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* Depth-tested (so the panel/board's own opaque geometry can still
     * occlude the HUD if the user gets very close to/behind it) but
     * depth-write-disabled, matching layer 5D's selection markers - the
     * HUD never needs to occlude anything drawn after it (nothing is). */
    VkPipelineDepthStencilStateCreateInfo depthStencil = {0};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blendAttachment;
    bz_quest_vk_straight_over_blend_state(&blendAttachment);
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
    info.layout = vkHud->panelPipelineLayout;
    info.renderPass = vkHud->vk->renderPass;
    info.subpass = 0;
    if (vkCreateGraphicsPipelines(vkHud->vk->device, VK_NULL_HANDLE, 1, &info, NULL, &vkHud->panelPipeline) !=
        VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateGraphicsPipelines (panel) failed");
        return false;
    }
    return true;
}

static bool create_text_pipeline(bzQuestVkWc3Hud_t *vkHud) {
    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vkHud->textVertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = vkHud->textFragmentShader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding = {0, sizeof(bzQuestVkWc3HudTextVertex_t), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(bzQuestVkWc3HudTextVertex_t, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(bzQuestVkWc3HudTextVertex_t, uv)},
    };
    VkPipelineVertexInputStateCreateInfo vertexInput = {0};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = 2;
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
    VkPipelineColorBlendAttachmentState blendAttachment;
    bz_quest_vk_straight_over_blend_state(&blendAttachment);
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
    info.layout = vkHud->textPipelineLayout;
    info.renderPass = vkHud->vk->renderPass;
    info.subpass = 0;
    if (vkCreateGraphicsPipelines(vkHud->vk->device, VK_NULL_HANDLE, 1, &info, NULL, &vkHud->textPipeline) !=
        VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateGraphicsPipelines (text) failed");
        return false;
    }
    return true;
}

bool bz_quest_vk_wc3_hud_create(const bzQuestVk_t *vk, bzQuestVkWc3Hud_t *out) {
    memset(out, 0, sizeof(*out));
    out->vk = vk;
    out->input = (bzQuestHudInput_t *)calloc(1, sizeof(*out->input));
    out->frame = (bzQuestHudFrame_t *)calloc(1, sizeof(*out->frame));
    if (!out->input || !out->frame) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: HUD scratch allocation failed");
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
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateDescriptorSetLayout failed");
        goto fail;
    }

    VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo poolInfo = {0};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(vk->device, &poolInfo, NULL, &out->descriptorPool) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateDescriptorPool failed");
        goto fail;
    }

    VkDescriptorSetAllocateInfo dsAlloc = {0};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = out->descriptorPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &out->descriptorSetLayout;
    if (vkAllocateDescriptorSets(vk->device, &dsAlloc, &out->descriptorSet) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkAllocateDescriptorSets failed");
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
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateSampler failed");
        goto fail;
    }

    if (!create_shader_module(vk->device, g_bz_quest_warcraft_hud_panel_vert_spv,
                              g_bz_quest_warcraft_hud_panel_vert_spv_len, &out->panelVertexShader) ||
        !create_shader_module(vk->device, g_bz_quest_warcraft_hud_panel_frag_spv,
                              g_bz_quest_warcraft_hud_panel_frag_spv_len, &out->panelFragmentShader) ||
        !create_shader_module(vk->device, g_bz_quest_warcraft_hud_text_vert_spv,
                              g_bz_quest_warcraft_hud_text_vert_spv_len, &out->textVertexShader) ||
        !create_shader_module(vk->device, g_bz_quest_warcraft_hud_text_frag_spv,
                              g_bz_quest_warcraft_hud_text_frag_spv_len, &out->textFragmentShader))
        goto fail;

    VkPushConstantRange panelPc = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(bzQuestVkWc3HudPushConsts_t)};
    VkPipelineLayoutCreateInfo panelLayoutInfo = {0};
    panelLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    panelLayoutInfo.pushConstantRangeCount = 1;
    panelLayoutInfo.pPushConstantRanges = &panelPc;
    if (vkCreatePipelineLayout(vk->device, &panelLayoutInfo, NULL, &out->panelPipelineLayout) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreatePipelineLayout (panel) failed");
        goto fail;
    }

    VkPushConstantRange textPc = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(bzQuestVkWc3HudPushConsts_t)};
    VkPipelineLayoutCreateInfo textLayoutInfo = {0};
    textLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    textLayoutInfo.setLayoutCount = 1;
    textLayoutInfo.pSetLayouts = &out->descriptorSetLayout;
    textLayoutInfo.pushConstantRangeCount = 1;
    textLayoutInfo.pPushConstantRanges = &textPc;
    if (vkCreatePipelineLayout(vk->device, &textLayoutInfo, NULL, &out->textPipelineLayout) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreatePipelineLayout (text) failed");
        goto fail;
    }

    VkBufferCreateInfo stagingInfo = {0};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = BZ_QUEST_VK_WC3_HUD_STAGING_BYTES;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk->device, &stagingInfo, NULL, &out->stagingBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkCreateBuffer (staging) failed");
        goto fail;
    }
    VkMemoryRequirements stagingReq;
    vkGetBufferMemoryRequirements(vk->device, out->stagingBuffer, &stagingReq);
    uint32_t stagingType = 0;
    if (!find_memory_type(vk->physicalDevice, stagingReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stagingType))
        goto fail;
    VkMemoryAllocateInfo stagingAlloc = {0};
    stagingAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingAlloc.allocationSize = stagingReq.size;
    stagingAlloc.memoryTypeIndex = stagingType;
    if (vkAllocateMemory(vk->device, &stagingAlloc, NULL, &out->stagingMemory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkAllocateMemory (staging) failed");
        goto fail;
    }
    if (vkBindBufferMemory(vk->device, out->stagingBuffer, out->stagingMemory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkBindBufferMemory (staging) failed");
        goto fail;
    }

    VkCommandBufferAllocateInfo cmdAlloc = {0};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = vk->commandPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vk->device, &cmdAlloc, &out->uploadCommandBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: vkAllocateCommandBuffers failed");
        goto fail;
    }

    if (!create_host_visible_buffer(out, BZ_QUEST_VK_WC3_HUD_MAX_PANEL_VERTICES * sizeof(bzQuestVkWc3HudPanelVertex_t),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &out->panelVertexBuffer,
                                    &out->panelVertexMemory, &out->panelVertexMapped) ||
        !create_host_visible_buffer(out, BZ_QUEST_VK_WC3_HUD_MAX_PANEL_INDICES * sizeof(uint32_t),
                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &out->panelIndexBuffer,
                                    &out->panelIndexMemory, &out->panelIndexMapped) ||
        !create_host_visible_buffer(out, BZ_QUEST_VK_WC3_HUD_MAX_TEXT_VERTICES * sizeof(bzQuestVkWc3HudTextVertex_t),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &out->textVertexBuffer,
                                    &out->textVertexMemory, &out->textVertexMapped) ||
        !create_host_visible_buffer(out, BZ_QUEST_VK_WC3_HUD_MAX_TEXT_INDICES * sizeof(uint32_t),
                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &out->textIndexBuffer, &out->textIndexMemory,
                                    &out->textIndexMapped))
        goto fail;

    if (!create_panel_pipeline(out) || !create_text_pipeline(out)) goto fail;

    /* Font atlas creation failure here is non-fatal to the whole module
     * (retried every frame from capture_and_upload() - see
     * create_font_atlas()'s comment); a fresh device/driver should never
     * actually fail this, but the retry path exists for parity with every
     * other GPU resource in this file. */
    if (!create_font_atlas(out)) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_hud: initial font atlas creation failed - will retry every frame");
    }
    return true;

fail:
    bz_quest_vk_wc3_hud_destroy(out);
    return false;
}

/* Builds a 4x4 column-major world matrix from `panel` (columns
 * right,down,normal,origin) - see bz_quest_wc3_hud.h's bzQuestHudPanelTransform_t
 * comment for why this exact composition lets rendering and hit-testing
 * share the same six vectors. */
static void hud_panel_world_matrix(const bzQuestHudPanelTransform_t *panel, float outWorld[16]) {
    outWorld[0] = panel->rightX;
    outWorld[1] = panel->rightY;
    outWorld[2] = panel->rightZ;
    outWorld[3] = 0.0f;
    outWorld[4] = panel->downX;
    outWorld[5] = panel->downY;
    outWorld[6] = panel->downZ;
    outWorld[7] = 0.0f;
    outWorld[8] = panel->normalX;
    outWorld[9] = panel->normalY;
    outWorld[10] = panel->normalZ;
    outWorld[11] = 0.0f;
    outWorld[12] = panel->originX;
    outWorld[13] = panel->originY;
    outWorld[14] = panel->originZ;
    outWorld[15] = 1.0f;
}

/* Rebuilds the panel-quad vertex/index arrays from `frame->quads` - one
 * flat-tint rectangle -> 4 vertices + 6 indices (two triangles), in
 * frame->quads[] order, panel-local (x, y, 0) positions unchanged (see
 * warcraft_hud_panel_vert.vert's header comment). */
static uint32_t build_panel_vertices(const bzQuestHudFrame_t *frame, bzQuestVkWc3HudPanelVertex_t *verts,
                                     uint32_t *indices, uint32_t *outIndexCount) {
    uint32_t vc = 0, ic = 0;
    for (uint32_t i = 0; i < frame->quadCount; i++) {
        const bzQuestHudQuad_t *q = &frame->quads[i];
        uint32_t base = vc;
        float corners[4][2] = {{q->x, q->y}, {q->x + q->w, q->y}, {q->x + q->w, q->y + q->h}, {q->x, q->y + q->h}};
        for (uint32_t c = 0; c < 4; c++) {
            verts[vc].pos[0] = corners[c][0];
            verts[vc].pos[1] = corners[c][1];
            verts[vc].pos[2] = 0.0f;
            verts[vc].color[0] = q->r;
            verts[vc].color[1] = q->g;
            verts[vc].color[2] = q->b;
            verts[vc].color[3] = q->a;
            vc++;
        }
        indices[ic++] = base + 0;
        indices[ic++] = base + 1;
        indices[ic++] = base + 2;
        indices[ic++] = base + 0;
        indices[ic++] = base + 2;
        indices[ic++] = base + 3;
    }
    *outIndexCount = ic;
    return vc;
}

/* Rebuilds the glyph vertex/index arrays from `frame->texts` by expanding
 * each run through bz_quest_wc3_hud_font_layout_text() - see
 * bz_quest_wc3_hud.h's bzQuestHudTextRun_t comment for why this expansion
 * happens here (at upload time) rather than in the pure layout module. */
static uint32_t build_text_vertices(const bzQuestHudFrame_t *frame, bzQuestVkWc3HudTextVertex_t *verts,
                                    uint32_t *indices, uint32_t *outIndexCount) {
    uint32_t vc = 0, ic = 0;
    bzQuestHudGlyphQuad_t glyphs[BZ_QUEST_HUD_FONT_MAX_GLYPHS_PER_RUN];
    for (uint32_t r = 0; r < frame->textCount; r++) {
        const bzQuestHudTextRun_t *run = &frame->texts[r];
        uint32_t glyphCount = 0;
        bool fitEntirely = bz_quest_wc3_hud_font_layout_text(run->text, run->x, run->y, run->scale, glyphs,
                                                             BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS_PER_RUN, &glyphCount);
        if (!fitEntirely) {
            /* Keyed by slot index r (not the text content, which changes
             * every frame for e.g. the resource line) so a persistently-
             * truncated run logs exactly once for its lifetime rather than
             * spamming once per differing string - see this function's
             * header comment and BZ_QUEST_HUD_MAX_STATUS_TEXT's comment
             * for why this is expected to be unreachable today. */
            char slot[16];
            snprintf(slot, sizeof(slot), "%u", r);
            VK_WC3_HUD_LOG_ONCE("hud-text-truncated", slot,
                                 "bz_quest_vk_wc3_hud: text run %u truncated at %u glyphs (BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS_PER_RUN)\n",
                                 r, BZ_QUEST_VK_WC3_HUD_MAX_GLYPHS_PER_RUN);
        }
        /* Independently scan the run's raw bytes for unsupported (non-7-bit-
         * ASCII) characters that bz_quest_wc3_hud_font_glyph_uv() already
         * silently remaps to '?' for rendering (see that function's doc
         * comment) - logged once per unique byte VALUE (not per run/slot,
         * since the same unsupported byte recurring across many frames/runs
         * is one diagnosable fact, not many). */
        for (const char *p = run->text; *p; p++) {
            unsigned char uc = (unsigned char)*p;
            float u0, v0, u1, v1;
            if (!bz_quest_wc3_hud_font_glyph_uv(uc, &u0, &v0, &u1, &v1)) {
                char byteKey[8];
                snprintf(byteKey, sizeof(byteKey), "%u", uc);
                VK_WC3_HUD_LOG_ONCE("hud-text-unsupported-byte", byteKey,
                                     "bz_quest_vk_wc3_hud: unsupported byte 0x%02x in HUD text, rendering as '?'\n", uc);
            }
        }
        for (uint32_t g = 0; g < glyphCount; g++) {
            if (vc + 4 > BZ_QUEST_VK_WC3_HUD_MAX_TEXT_VERTICES) break;
            const bzQuestHudGlyphQuad_t *gq = &glyphs[g];
            uint32_t base = vc;
            float positions[4][2] = {
                {gq->x, gq->y}, {gq->x + gq->w, gq->y}, {gq->x + gq->w, gq->y + gq->h}, {gq->x, gq->y + gq->h}};
            float uvs[4][2] = {{gq->u0, gq->v0}, {gq->u1, gq->v0}, {gq->u1, gq->v1}, {gq->u0, gq->v1}};
            for (uint32_t c = 0; c < 4; c++) {
                verts[vc].pos[0] = positions[c][0];
                verts[vc].pos[1] = positions[c][1];
                verts[vc].pos[2] = 0.0f;
                verts[vc].uv[0] = uvs[c][0];
                verts[vc].uv[1] = uvs[c][1];
                vc++;
            }
            indices[ic++] = base + 0;
            indices[ic++] = base + 1;
            indices[ic++] = base + 2;
            indices[ic++] = base + 0;
            indices[ic++] = base + 2;
            indices[ic++] = base + 3;
        }
    }
    *outIndexCount = ic;
    return vc;
}

void bz_quest_vk_wc3_hud_capture_and_upload(bzQuestVkWc3Hud_t *vkHud) {
    if (!vkHud->haveFont) create_font_atlas(vkHud);

    if (!bz_quest_wc3_capture_hud(vkHud->input)) {
        vkHud->haveFrame = false;
        vkHud->panelIndexCount = 0;
        vkHud->textIndexCount = 0;
        return;
    }
    bz_quest_wc3_hud_build(vkHud->input, vkHud->frame);
    vkHud->haveFrame = true;
    if (vkHud->frame->statusTextTruncated) {
        /* Fixed key: this is a single boolean condition (not per-value),
         * so it logs at most once for this vkHud's lifetime, the first
         * frame it is ever observed - see bzQuestHudFrame_t.
         * statusTextTruncated's comment for why this should be
         * unreachable given today's field sizes. */
        VK_WC3_HUD_LOG_ONCE("hud-status-text-truncated", "",
                             "bz_quest_vk_wc3_hud: status/resource text truncated - displayed values may be incomplete\n");
    }

    uint32_t panelIndexCount = 0;
    uint32_t panelVertexCount = build_panel_vertices(vkHud->frame, s_panelVerts, s_panelIndices, &panelIndexCount);
    uint32_t panelVertexBytes = panelVertexCount * (uint32_t)sizeof(bzQuestVkWc3HudPanelVertex_t);
    uint32_t panelIndexBytes = panelIndexCount * (uint32_t)sizeof(uint32_t);
    if (panelVertexBytes != vkHud->panelVertexBytes ||
        memcmp(vkHud->panelVertexMapped, s_panelVerts, panelVertexBytes) != 0) {
        memcpy(vkHud->panelVertexMapped, s_panelVerts, panelVertexBytes);
        vkHud->panelVertexBytes = panelVertexBytes;
    }
    if (panelIndexBytes != vkHud->panelIndexBytes ||
        memcmp(vkHud->panelIndexMapped, s_panelIndices, panelIndexBytes) != 0) {
        memcpy(vkHud->panelIndexMapped, s_panelIndices, panelIndexBytes);
        vkHud->panelIndexBytes = panelIndexBytes;
    }
    vkHud->panelIndexCount = panelIndexCount;

    uint32_t textIndexCount = 0;
    uint32_t textVertexCount = build_text_vertices(vkHud->frame, s_textVerts, s_textIndices, &textIndexCount);
    uint32_t textVertexBytes = textVertexCount * (uint32_t)sizeof(bzQuestVkWc3HudTextVertex_t);
    uint32_t textIndexBytes = textIndexCount * (uint32_t)sizeof(uint32_t);
    if (textVertexBytes != vkHud->textVertexBytes ||
        memcmp(vkHud->textVertexMapped, s_textVerts, textVertexBytes) != 0) {
        memcpy(vkHud->textVertexMapped, s_textVerts, textVertexBytes);
        vkHud->textVertexBytes = textVertexBytes;
    }
    if (textIndexBytes != vkHud->textIndexBytes || memcmp(vkHud->textIndexMapped, s_textIndices, textIndexBytes) != 0) {
        memcpy(vkHud->textIndexMapped, s_textIndices, textIndexBytes);
        vkHud->textIndexBytes = textIndexBytes;
    }
    vkHud->textIndexCount = textIndexCount;
}

bool bz_quest_vk_wc3_hud_has_frame(const bzQuestVkWc3Hud_t *vkHud) {
    return vkHud && vkHud->haveFrame;
}

const bzQuestHudFrame_t *bz_quest_vk_wc3_hud_frame(const bzQuestVkWc3Hud_t *vkHud) {
    return (vkHud && vkHud->haveFrame) ? vkHud->frame : NULL;
}

void bz_quest_vk_wc3_hud_record(bzQuestVkWc3Hud_t *vkHud, VkCommandBuffer cmd, const float viewProj[16]) {
    if (!bz_quest_vk_wc3_hud_has_frame(vkHud)) return;

    float world[16];
    hud_panel_world_matrix(&vkHud->frame->panel, world);
    bzQuestVkWc3HudPushConsts_t pc;
    bz_quest_mat4_multiply(viewProj, world, pc.mvp);

    if (vkHud->panelIndexCount > 0 && vkHud->panelPipeline != VK_NULL_HANDLE) {
        VkDeviceSize offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkHud->panelPipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vkHud->panelVertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, vkHud->panelIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdPushConstants(cmd, vkHud->panelPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, vkHud->panelIndexCount, 1, 0, 0, 0);
    }

    if (vkHud->textIndexCount > 0 && vkHud->textPipeline != VK_NULL_HANDLE && vkHud->haveFont) {
        VkDeviceSize offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkHud->textPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkHud->textPipelineLayout, 0, 1,
                               &vkHud->descriptorSet, 0, NULL);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vkHud->textVertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, vkHud->textIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdPushConstants(cmd, vkHud->textPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, vkHud->textIndexCount, 1, 0, 0, 0);
    }
}

static void destroy_mapped_buffer(bzQuestVkWc3Hud_t *vkHud, VkBuffer *buffer, VkDeviceMemory *memory,
                                  void **mapped) {
    if (*memory != VK_NULL_HANDLE && *mapped) vkUnmapMemory(vkHud->vk->device, *memory);
    if (*buffer != VK_NULL_HANDLE) vkDestroyBuffer(vkHud->vk->device, *buffer, NULL);
    if (*memory != VK_NULL_HANDLE) vkFreeMemory(vkHud->vk->device, *memory, NULL);
    *buffer = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
    *mapped = NULL;
}

void bz_quest_vk_wc3_hud_destroy(bzQuestVkWc3Hud_t *vkHud) {
    if (!vkHud || !vkHud->vk) {
        if (vkHud) {
            free(vkHud->input);
            free(vkHud->frame);
            memset(vkHud, 0, sizeof(*vkHud));
        }
        return;
    }
    vkDeviceWaitIdle(vkHud->vk->device);
    destroy_font_image(vkHud);
    destroy_mapped_buffer(vkHud, &vkHud->panelVertexBuffer, &vkHud->panelVertexMemory, &vkHud->panelVertexMapped);
    destroy_mapped_buffer(vkHud, &vkHud->panelIndexBuffer, &vkHud->panelIndexMemory, &vkHud->panelIndexMapped);
    destroy_mapped_buffer(vkHud, &vkHud->textVertexBuffer, &vkHud->textVertexMemory, &vkHud->textVertexMapped);
    destroy_mapped_buffer(vkHud, &vkHud->textIndexBuffer, &vkHud->textIndexMemory, &vkHud->textIndexMapped);
    if (vkHud->stagingBuffer != VK_NULL_HANDLE) vkDestroyBuffer(vkHud->vk->device, vkHud->stagingBuffer, NULL);
    if (vkHud->stagingMemory != VK_NULL_HANDLE) vkFreeMemory(vkHud->vk->device, vkHud->stagingMemory, NULL);
    if (vkHud->panelPipeline != VK_NULL_HANDLE) vkDestroyPipeline(vkHud->vk->device, vkHud->panelPipeline, NULL);
    if (vkHud->textPipeline != VK_NULL_HANDLE) vkDestroyPipeline(vkHud->vk->device, vkHud->textPipeline, NULL);
    if (vkHud->panelPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vkHud->vk->device, vkHud->panelPipelineLayout, NULL);
    if (vkHud->textPipelineLayout != VK_NULL_HANDLE)
        vkDestroyPipelineLayout(vkHud->vk->device, vkHud->textPipelineLayout, NULL);
    if (vkHud->panelVertexShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(vkHud->vk->device, vkHud->panelVertexShader, NULL);
    if (vkHud->panelFragmentShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(vkHud->vk->device, vkHud->panelFragmentShader, NULL);
    if (vkHud->textVertexShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(vkHud->vk->device, vkHud->textVertexShader, NULL);
    if (vkHud->textFragmentShader != VK_NULL_HANDLE)
        vkDestroyShaderModule(vkHud->vk->device, vkHud->textFragmentShader, NULL);
    if (vkHud->sampler != VK_NULL_HANDLE) vkDestroySampler(vkHud->vk->device, vkHud->sampler, NULL);
    if (vkHud->descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(vkHud->vk->device, vkHud->descriptorPool, NULL);
    if (vkHud->descriptorSetLayout != VK_NULL_HANDLE)
        vkDestroyDescriptorSetLayout(vkHud->vk->device, vkHud->descriptorSetLayout, NULL);
    if (vkHud->uploadCommandBuffer != VK_NULL_HANDLE)
        vkFreeCommandBuffers(vkHud->vk->device, vkHud->vk->commandPool, 1, &vkHud->uploadCommandBuffer);
    free(vkHud->input);
    free(vkHud->frame);
    memset(vkHud, 0, sizeof(*vkHud));
}
