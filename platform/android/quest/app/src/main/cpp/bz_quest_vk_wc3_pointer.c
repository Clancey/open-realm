/*
 * bz_quest_vk_wc3_pointer.c - see bz_quest_vk_wc3_pointer.h.
 */
#include "bz_quest_vk_wc3_pointer.h"

#include <math.h>
#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_shaders_generated.h"

/* Beam half-width and reticle radius in meters - a ~4 mm beam and ~2 cm
 * reticle read clearly at arm's length without dominating the diorama. */
#define BZ_QUEST_VK_WC3_POINTER_BEAM_HALF_WIDTH 0.004f
#define BZ_QUEST_VK_WC3_POINTER_RETICLE_RADIUS 0.02f

typedef struct {
    float mvp[16];
    float tint[4];
} PointerPushConsts_t;

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
    BZ_QUEST_LOGE("bz_quest_vk_wc3_pointer: no memory type matches bits=0x%x flags=0x%x", typeBits,
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
        BZ_QUEST_LOGE("bz_quest_vk_wc3_pointer: vkCreateShaderModule failed");
        return false;
    }
    return true;
}

static bool create_pipeline(bzQuestVkWc3Pointer_t *p) {
    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = p->vertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = p->fragmentShader;
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding = {0, sizeof(bzQuestVkWc3PointerVertex_t),
                                               VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(bzQuestVkWc3PointerVertex_t, position)};
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
    rasterization.cullMode = VK_CULL_MODE_NONE; /* beam is viewed from any side */
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* Depth-tested (occluded by nearer board geometry) but no depth write, so
     * translucent beams never corrupt the depth buffer - exactly the layer 5D
     * selection-marker discipline. */
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
    info.layout = p->pipelineLayout;
    info.renderPass = p->vk->renderPass;
    info.subpass = 0;
    if (vkCreateGraphicsPipelines(p->vk->device, VK_NULL_HANDLE, 1, &info, NULL, &p->pipeline) !=
        VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_pointer: vkCreateGraphicsPipelines failed");
        return false;
    }
    return true;
}

bool bz_quest_vk_wc3_pointer_create(const bzQuestVk_t *vk, bzQuestVkWc3Pointer_t *out) {
    memset(out, 0, sizeof(*out));
    out->vk = vk;

    if (!create_shader_module(vk->device, g_bz_quest_warcraft_marker_vert_spv,
                              g_bz_quest_warcraft_marker_vert_spv_len, &out->vertexShader) ||
        !create_shader_module(vk->device, g_bz_quest_warcraft_marker_frag_spv,
                              g_bz_quest_warcraft_marker_frag_spv_len, &out->fragmentShader))
        goto fail;

    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(PointerPushConsts_t)};
    VkPipelineLayoutCreateInfo layoutInfo = {0};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(vk->device, &layoutInfo, NULL, &out->pipelineLayout) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_pointer: vkCreatePipelineLayout failed");
        goto fail;
    }

    if (!create_pipeline(out)) goto fail;

    VkBufferCreateInfo bufferInfo = {0};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(bzQuestVkWc3PointerVertex_t) * BZ_QUEST_VK_WC3_POINTER_MAX_VERTS;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk->device, &bufferInfo, NULL, &out->vertexBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_pointer: vkCreateBuffer failed");
        goto fail;
    }
    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(vk->device, out->vertexBuffer, &memReq);
    uint32_t memType = 0;
    if (!find_memory_type(vk->physicalDevice, memReq.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &memType))
        goto fail;
    VkMemoryAllocateInfo alloc = {0};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = memReq.size;
    alloc.memoryTypeIndex = memType;
    if (vkAllocateMemory(vk->device, &alloc, NULL, &out->vertexMemory) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_pointer: vkAllocateMemory failed");
        goto fail;
    }
    if (vkBindBufferMemory(vk->device, out->vertexBuffer, out->vertexMemory, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_pointer: vkBindBufferMemory failed");
        goto fail;
    }
    if (vkMapMemory(vk->device, out->vertexMemory, 0, VK_WHOLE_SIZE, 0, &out->mapped) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_vk_wc3_pointer: vkMapMemory failed");
        goto fail;
    }
    return true;

fail:
    bz_quest_vk_wc3_pointer_destroy(out);
    return false;
}

static void vec_sub(const float a[3], const float b[3], float out[3]) {
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}
static float vec_len(const float a[3]) { return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
static void vec_cross(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}
static void vec_normalize(float v[3]) {
    float l = vec_len(v);
    if (l > 1e-6f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

/* Two perpendicular unit axes spanning the plane orthogonal to `dir`. Picks a
 * seed axis not parallel to `dir` so the cross products never degenerate. */
static void perp_basis(const float dir[3], float u[3], float v[3]) {
    float seed[3] = {0.0f, 1.0f, 0.0f};
    if (fabsf(dir[1]) > 0.9f) {
        seed[0] = 1.0f;
        seed[1] = 0.0f;
    }
    vec_cross(dir, seed, u);
    vec_normalize(u);
    vec_cross(dir, u, v);
    vec_normalize(v);
}

static void push_vertex(bzQuestVkWc3PointerVertex_t *verts, uint32_t *n, const float p[3]) {
    if (*n >= BZ_QUEST_VK_WC3_POINTER_MAX_VERTS) return;
    verts[*n].position[0] = p[0];
    verts[*n].position[1] = p[1];
    verts[*n].position[2] = p[2];
    (*n)++;
}

static void push_quad(bzQuestVkWc3PointerVertex_t *verts, uint32_t *n, const float a[3], const float b[3],
                      const float c[3], const float d[3]) {
    push_vertex(verts, n, a);
    push_vertex(verts, n, b);
    push_vertex(verts, n, c);
    push_vertex(verts, n, a);
    push_vertex(verts, n, c);
    push_vertex(verts, n, d);
}

/* Emits one hand's beam (2 crossed quads) + optional reticle disc, returning
 * the vertex count written. */
static uint32_t build_hand(bzQuestVkWc3PointerVertex_t *verts, uint32_t *n,
                           const bzQuestVkWc3PointerHand_t *hand) {
    uint32_t start = *n;
    float dir[3];
    vec_sub(hand->rayEnd, hand->rayStart, dir);
    if (vec_len(dir) < 1e-5f) {
        dir[0] = 0.0f;
        dir[1] = 0.0f;
        dir[2] = -1.0f;
    }
    float ndir[3] = {dir[0], dir[1], dir[2]};
    vec_normalize(ndir);
    float u[3], v[3];
    perp_basis(ndir, u, v);
    const float w = BZ_QUEST_VK_WC3_POINTER_BEAM_HALF_WIDTH;
    /* Crossed quads: one spanning ±u, one spanning ±v, both from start to end. */
    for (int axis = 0; axis < 2; ++axis) {
        const float *e = axis == 0 ? u : v;
        float s0[3], s1[3], e0[3], e1[3];
        for (int k = 0; k < 3; ++k) {
            s0[k] = hand->rayStart[k] - e[k] * w;
            s1[k] = hand->rayStart[k] + e[k] * w;
            e1[k] = hand->rayEnd[k] + e[k] * w;
            e0[k] = hand->rayEnd[k] - e[k] * w;
        }
        push_quad(verts, n, s0, s1, e1, e0);
    }

    if (hand->hasReticle) {
        const float r = BZ_QUEST_VK_WC3_POINTER_RETICLE_RADIUS;
        for (int i = 0; i < BZ_QUEST_VK_WC3_POINTER_RETICLE_SEGMENTS; ++i) {
            float a0 = (float)i / BZ_QUEST_VK_WC3_POINTER_RETICLE_SEGMENTS * 2.0f * (float)M_PI;
            float a1 = (float)(i + 1) / BZ_QUEST_VK_WC3_POINTER_RETICLE_SEGMENTS * 2.0f * (float)M_PI;
            float p0[3], p1[3];
            for (int k = 0; k < 3; ++k) {
                p0[k] = hand->reticle[k] + r * (cosf(a0) * u[k] + sinf(a0) * v[k]);
                p1[k] = hand->reticle[k] + r * (cosf(a1) * u[k] + sinf(a1) * v[k]);
            }
            push_vertex(verts, n, hand->reticle);
            push_vertex(verts, n, p0);
            push_vertex(verts, n, p1);
        }
    }
    return *n - start;
}

void bz_quest_vk_wc3_pointer_update(bzQuestVkWc3Pointer_t *vkPointer,
                                    const bzQuestVkWc3PointerHand_t hands[BZ_QUEST_INPUT_HAND_COUNT]) {
    bzQuestVkWc3PointerVertex_t verts[BZ_QUEST_VK_WC3_POINTER_MAX_VERTS];
    uint32_t n = 0;
    vkPointer->haveGeometry = false;
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h) {
        vkPointer->draw[h].visible = false;
        if (!hands[h].visible) continue;
        uint32_t first = n;
        uint32_t count = build_hand(verts, &n, &hands[h]);
        if (count == 0) continue;
        vkPointer->draw[h].visible = true;
        vkPointer->draw[h].firstVertex = first;
        vkPointer->draw[h].vertexCount = count;
        memcpy(vkPointer->draw[h].tint, hands[h].tint, sizeof(vkPointer->draw[h].tint));
        vkPointer->haveGeometry = true;
    }
    if (n > 0 && vkPointer->mapped) {
        memcpy(vkPointer->mapped, verts, sizeof(bzQuestVkWc3PointerVertex_t) * n);
    }
}

bool bz_quest_vk_wc3_pointer_has_geometry(const bzQuestVkWc3Pointer_t *vkPointer) {
    return vkPointer->haveGeometry;
}

void bz_quest_vk_wc3_pointer_record(bzQuestVkWc3Pointer_t *vkPointer, VkCommandBuffer cmd,
                                    const float viewProj[16]) {
    if (!vkPointer->haveGeometry || vkPointer->pipeline == VK_NULL_HANDLE) return;
    VkDeviceSize offset = 0;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPointer->pipeline);
    vkCmdBindVertexBuffers(cmd, 0, 1, &vkPointer->vertexBuffer, &offset);
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h) {
        if (!vkPointer->draw[h].visible) continue;
        PointerPushConsts_t pc;
        memcpy(pc.mvp, viewProj, sizeof(pc.mvp));
        memcpy(pc.tint, vkPointer->draw[h].tint, sizeof(pc.tint));
        vkCmdPushConstants(cmd, vkPointer->pipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        vkCmdDraw(cmd, vkPointer->draw[h].vertexCount, 1, vkPointer->draw[h].firstVertex, 0);
    }
}

void bz_quest_vk_wc3_pointer_destroy(bzQuestVkWc3Pointer_t *vkPointer) {
    if (!vkPointer->vk) return;
    VkDevice device = vkPointer->vk->device;
    if (vkPointer->mapped) {
        vkUnmapMemory(device, vkPointer->vertexMemory);
        vkPointer->mapped = NULL;
    }
    if (vkPointer->vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, vkPointer->vertexBuffer, NULL);
        vkPointer->vertexBuffer = VK_NULL_HANDLE;
    }
    if (vkPointer->vertexMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, vkPointer->vertexMemory, NULL);
        vkPointer->vertexMemory = VK_NULL_HANDLE;
    }
    if (vkPointer->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, vkPointer->pipeline, NULL);
        vkPointer->pipeline = VK_NULL_HANDLE;
    }
    if (vkPointer->pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, vkPointer->pipelineLayout, NULL);
        vkPointer->pipelineLayout = VK_NULL_HANDLE;
    }
    if (vkPointer->vertexShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, vkPointer->vertexShader, NULL);
        vkPointer->vertexShader = VK_NULL_HANDLE;
    }
    if (vkPointer->fragmentShader != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, vkPointer->fragmentShader, NULL);
        vkPointer->fragmentShader = VK_NULL_HANDLE;
    }
}
