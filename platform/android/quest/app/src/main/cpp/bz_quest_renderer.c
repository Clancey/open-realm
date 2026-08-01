/*
 * bz_quest_renderer.c - see bz_quest_renderer.h.
 */
#include "bz_quest_renderer.h"

#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"

/* Preference order: non-sRGB first (the render pass writes vertex colors
 * straight through with no gamma-correction step - see tabletop_frag.frag),
 * falling back to sRGB variants of the same channel order rather than
 * failing outright if that's all the runtime offers. */
static const int64_t BZ_QUEST_PREFERRED_COLOR_FORMATS[] = {
    VK_FORMAT_R8G8B8A8_UNORM,
    VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_R8G8B8A8_SRGB,
    VK_FORMAT_B8G8R8A8_SRGB,
};

/* Cross-checks bz_quest_pure.h's mirrored XR_COMPOSITION_LAYER_*_BIT literal
 * values (needed there because that file must stay host-buildable without
 * openxr.h - see its comment) against the real constants this file *can*
 * see, at file scope so it runs once per build rather than once per frame.
 * A future OpenXR header update that ever renumbered these bits would fail
 * the build here instead of silently miscompositing passthrough. */
_Static_assert(BZ_QUEST_XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT_VALUE ==
                   XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
               "bz_quest_pure.h's mirrored XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT "
               "value has drifted from openxr.h");
_Static_assert(BZ_QUEST_XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT_VALUE ==
                   XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
               "bz_quest_pure.h's mirrored XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT "
               "value has drifted from openxr.h");

bool bz_quest_renderer_init(void *vm, void *context, bzQuestRenderer_t *renderer) {
    memset(renderer, 0, sizeof(*renderer));
    bzQuestXr_t *xr = &renderer->xr;
    bzQuestVk_t *vk = &renderer->vk;

    if (!bz_quest_xr_init_loader(vm, context)) goto fail;
    if (!bz_quest_xr_create_instance(vm, context, xr)) goto fail;
    if (!bz_quest_xr_get_system(xr)) goto fail;
    if (!bz_quest_xr_load_functions(xr)) goto fail;

    XrGraphicsRequirementsVulkanKHR vulkanRequirements;
    if (!bz_quest_xr_get_vulkan_requirements(xr, &vulkanRequirements)) goto fail;
    if (!bz_quest_vk_create_instance(xr, &vulkanRequirements, vk)) goto fail;
    if (!bz_quest_vk_create_device(xr, vk)) goto fail;

    if (!bz_quest_xr_select_blend_mode(xr)) goto fail;
    if (!bz_quest_xr_enumerate_views(xr)) goto fail;

    XrGraphicsBindingVulkanKHR binding;
    memset(&binding, 0, sizeof(binding));
    binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
    binding.instance = vk->instance;
    binding.physicalDevice = vk->physicalDevice;
    binding.device = vk->device;
    binding.queueFamilyIndex = vk->queueFamilyIndex;
    binding.queueIndex = 0;
    if (!bz_quest_xr_create_session(xr, &binding)) goto fail;
    if (!bz_quest_xr_create_space(xr)) goto fail;

    if (!bz_quest_xr_create_swapchains(
            xr, BZ_QUEST_PREFERRED_COLOR_FORMATS,
            sizeof(BZ_QUEST_PREFERRED_COLOR_FORMATS) / sizeof(BZ_QUEST_PREFERRED_COLOR_FORMATS[0])))
        goto fail;

    if (!bz_quest_vk_create_render_resources(vk, xr->swapchains[0].format)) goto fail;
    for (uint32_t i = 0; i < BZ_QUEST_VIEW_COUNT; i++) {
        if (!bz_quest_vk_create_targets(vk, i, &xr->swapchains[i])) goto fail;
    }

    if (!bz_quest_passthrough_create(xr, &renderer->passthrough)) goto fail;
    if (!bz_quest_passthrough_start(xr, &renderer->passthrough)) goto fail;

    if (!bz_quest_vk_wc3_create(vk, &renderer->wc3)) goto fail;
    if (!bz_quest_vk_wc3_terrain_create(vk, &renderer->wc3Terrain)) goto fail;
    if (!bz_quest_vk_wc3_fog_create(vk, &renderer->wc3Fog)) goto fail;
    if (!bz_quest_vk_wc3_hud_create(vk, &renderer->wc3Hud)) goto fail;

    BZ_QUEST_LOGI("renderer init complete");
    return true;

fail:
    BZ_QUEST_LOGE("renderer init failed - tearing down partial state");
    bz_quest_renderer_shutdown(renderer);
    return false;
}

/* Builds one eye's model-view-projection matrix from its tracked pose/FOV.
 * There is no model matrix here beyond identity: bz_quest_scene.c's
 * vertices are already authored directly in the LOCAL reference space
 * bz_quest_xr_create_space() creates, so mvp = projection * view. Returns
 * false if either pure-helper rejects its input (non-unit quaternion,
 * degenerate FOV) - see bz_quest_pure.h's contracts - in which case the
 * caller must skip rendering this eye rather than draw with a
 * partially-built matrix. */
static bool bz_quest_renderer_build_mvp(const XrView *view, float outMvp[16]) {
    float viewMatrix[16];
    float projMatrix[16];
    if (!bz_quest_pose_to_view_matrix(view->pose.position.x, view->pose.position.y,
                                      view->pose.position.z, view->pose.orientation.x,
                                      view->pose.orientation.y, view->pose.orientation.z,
                                      view->pose.orientation.w, viewMatrix))
        return false;
    if (!bz_quest_fov_projection_vk(view->fov.angleLeft, view->fov.angleRight, view->fov.angleUp,
                                    view->fov.angleDown, 0.05f, 100.0f, projMatrix))
        return false;
    bz_quest_mat4_multiply(projMatrix, viewMatrix, outMvp);
    return true;
}

/* One shared eye render pass for terrain+models, preserving each subsystem's
 * own opaque-before-blended discipline while making the blended-vs-blended
 * ordering limit explicit: terrain blended draws sort within terrain, model
 * blended draws sort within models, and the two systems are interleaved only
 * at the subsystem granularity (terrain blended first, model blended second),
 * matching bz_quest_vk_wc3.c's existing documented entity-level transparency
 * limit rather than pretending to do a global per-triangle merge sort. */
static bool bz_quest_renderer_render_warcraft_target(
    bzQuestRenderer_t *renderer, uint32_t viewIndex, uint32_t imageIndex, uint32_t width, uint32_t height,
    const float viewProj[16], const float cameraWorldPos[3], const bzQuestWc3TerrainRenderList_t *terrainList,
    const bzQuestWc3RenderList_t *wc3List) {
    const bzQuestVk_t *vk = &renderer->vk;
    if (viewIndex >= BZ_QUEST_VIEW_COUNT || imageIndex >= vk->targetCount[viewIndex]) {
        BZ_QUEST_LOGE("bz_quest_renderer: render target view=%u image=%u out of range", viewIndex, imageIndex);
        return false;
    }
    const bzQuestVkTarget_t *target = &vk->targets[viewIndex][imageIndex];

    if (vkWaitForFences(vk->device, 1, &target->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_renderer: vkWaitForFences failed");
        return false;
    }
    if (vkResetFences(vk->device, 1, &target->fence) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_renderer: vkResetFences failed");
        return false;
    }
    if (vkResetCommandBuffer(target->commandBuffer, 0) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_renderer: vkResetCommandBuffer failed");
        return false;
    }

    VkCommandBufferBeginInfo beginInfo = {0};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(target->commandBuffer, &beginInfo) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_renderer: vkBeginCommandBuffer failed");
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

    /* Fog overlay is recorded AFTER both opaque and blended world draws (not
     * between them, as an earlier revision mistakenly did): darkening the
     * scene before blended terrain/model passes run left water and
     * transparent doodads compositing back on TOP of the fog mask, fully
     * visible through unseen/explored-not-visible fog. Recording fog last -
     * immediately before selection markers - makes it a true final
     * visibility mask over the complete opaque+blended scene. Selection
     * markers run last so they stay readable atop fogged content but still
     * depth-test against the opaque geometry already in the depth buffer.
     * The layer 5E status/command-card HUD is recorded last of all - after
     * fog and selection markers - so the bridge-authored HUD is always
     * legible on top of the board, matching visionOS's own overlay-panel
     * placement (see docs/quest-tabletop.md's Layer 5E section). */
    bz_quest_vk_wc3_terrain_record_opaque(&renderer->wc3Terrain, target->commandBuffer, viewProj, cameraWorldPos,
                                          terrainList);
    bz_quest_vk_wc3_record_opaque(&renderer->wc3, target->commandBuffer, viewProj, cameraWorldPos, wc3List);
    bz_quest_vk_wc3_terrain_record_blended(&renderer->wc3Terrain, target->commandBuffer, viewProj, terrainList);
    bz_quest_vk_wc3_record_blended(&renderer->wc3, target->commandBuffer, viewProj, wc3List);
    bz_quest_vk_wc3_fog_record_overlay(&renderer->wc3Fog, target->commandBuffer, viewProj);
    bz_quest_vk_wc3_fog_record_selection(&renderer->wc3Fog, target->commandBuffer, viewProj, wc3List);
    bz_quest_vk_wc3_hud_record(&renderer->wc3Hud, target->commandBuffer, viewProj);

    vkCmdEndRenderPass(target->commandBuffer);
    if (vkEndCommandBuffer(target->commandBuffer) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_renderer: vkEndCommandBuffer failed");
        return false;
    }
    VkSubmitInfo submitInfo = {0};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &target->commandBuffer;
    if (vkQueueSubmit(vk->queue, 1, &submitInfo, target->fence) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_renderer: vkQueueSubmit failed");
        return false;
    }
    if (vkWaitForFences(vk->device, 1, &target->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        BZ_QUEST_LOGE("bz_quest_renderer: vkWaitForFences (post-submit) failed");
        return false;
    }
    return true;
}

bool bz_quest_renderer_frame(bzQuestRenderer_t *renderer) {
    bzQuestXr_t *xr = &renderer->xr;
    bzQuestVk_t *vk = &renderer->vk;

    bz_quest_xr_poll_events(xr);
    if (xr->exitRequested) return false;
    if (!bz_quest_xr_is_session_running(xr)) return true; /* idle: host should keep pumping Android events */

    XrFrameState frameState;
    if (!bz_quest_xr_wait_frame(xr, &frameState)) return false;
    if (!bz_quest_xr_begin_frame(xr)) return false;

    XrCompositionLayerProjectionView projectionViews[BZ_QUEST_VIEW_COUNT];
    memset(projectionViews, 0, sizeof(projectionViews));
    XrCompositionLayerPassthroughFB passthroughLayer;
    XrCompositionLayerProjection projectionLayer;
    memset(&projectionLayer, 0, sizeof(projectionLayer));
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t layerCount = 0;

    XrView views[BZ_QUEST_VIEW_COUNT];
    bool haveViews = frameState.shouldRender &&
                     bz_quest_xr_locate_views(xr, frameState.predictedDisplayTime, views);

    /* Captured once per frame (not once per eye) - see
     * bz_quest_vk_wc3_capture_and_upload()'s doc comment. Only bothered with
     * when the frame will actually render (haveViews): capturing here also
     * performs this frame's bounded new-model/new-texture Vulkan uploads, so
     * skipping it on a non-rendering frame is a correctness no-op, not a
     * missed upload - the same models/textures are captured again (and
     * uploaded then) the next frame that does render. */
    bzQuestWc3RenderList_t wc3RenderList;
    bzQuestWc3TerrainRenderList_t terrainRenderList;
    memset(&wc3RenderList, 0, sizeof(wc3RenderList));
    memset(&terrainRenderList, 0, sizeof(terrainRenderList));
    if (haveViews) {
        bz_quest_vk_wc3_capture_and_upload(&renderer->wc3, &wc3RenderList);
        bz_quest_vk_wc3_terrain_capture_and_upload(&renderer->wc3Terrain, &terrainRenderList);
        bz_quest_vk_wc3_fog_capture_and_upload(&renderer->wc3Fog);
        bz_quest_vk_wc3_hud_capture_and_upload(&renderer->wc3Hud);
    }

    if (haveViews) {
        for (uint32_t i = 0; i < BZ_QUEST_VIEW_COUNT; i++) {
            bzQuestXrSwapchain_t *swapchain = &xr->swapchains[i];
            uint32_t imageIndex = 0;
            if (!bz_quest_xr_acquire_swapchain_image(swapchain, &imageIndex)) {
                haveViews = false;
                break;
            }
            if (!bz_quest_xr_wait_swapchain_image(swapchain)) {
                haveViews = false;
                break;
            }

            float mvp[16];
            bool rendered = false;
            if (!bz_quest_renderer_build_mvp(&views[i], mvp)) {
                haveViews = false;
            } else if (wc3RenderList.count > 0 || terrainRenderList.count > 0 ||
                       bz_quest_vk_wc3_fog_has_overlay(&renderer->wc3Fog) ||
                       bz_quest_vk_wc3_hud_has_frame(&renderer->wc3Hud)) {
                /* Warcraft III terrain and/or models exist this frame: record
                 * both into one shared eye render pass, interleaving terrain
                 * opaque -> model opaque -> terrain blended -> model blended.
                 * `mvp` is this eye's view*projection matrix; models apply
                 * their world matrix inside bz_quest_vk_wc3.c, terrain bakes
                 * world position directly into its vertices. The layer 5E HUD
                 * check is included here too so the status/command-card panel
                 * still renders on a frame with a connected bridge snapshot
                 * but no selection/models yet (e.g. observing an empty
                 * board) - see bz_quest_wc3_hud.h's no-selection state. */
                const float cameraWorldPos[3] = {
                    views[i].pose.position.x,
                    views[i].pose.position.y,
                    views[i].pose.position.z,
                };
                rendered = bz_quest_renderer_render_warcraft_target(
                    renderer, i, imageIndex, swapchain->width, swapchain->height, mvp, cameraWorldPos,
                    &terrainRenderList, &wc3RenderList);
                if (!rendered) haveViews = false;
            } else {
                /* No valid Warcraft render items yet (e.g. no snapshot,
                 * still connecting, or every terrain chunk/model/texture is
                 * still uploading) - render the existing procedural test scene
                 * as an explicit diagnostic, never a silent unsupported-asset
                 * fallback. */
                rendered = bz_quest_vk_render_target(vk, i, imageIndex, swapchain->width,
                                                     swapchain->height, mvp);
                if (!rendered) haveViews = false;
            }

            if (!bz_quest_xr_release_swapchain_image(swapchain)) haveViews = false;
            if (!haveViews) break;

            projectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projectionViews[i].pose = views[i].pose;
            projectionViews[i].fov = views[i].fov;
            projectionViews[i].subImage.swapchain = swapchain->handle;
            projectionViews[i].subImage.imageRect.offset.x = 0;
            projectionViews[i].subImage.imageRect.offset.y = 0;
            projectionViews[i].subImage.imageRect.extent.width = (int32_t)swapchain->width;
            projectionViews[i].subImage.imageRect.extent.height = (int32_t)swapchain->height;
        }
    }

    if (haveViews) {
        /* Passthrough first (background), the head-tracked test scene's
         * projection layer second (foreground) - composition layer order
         * in XrFrameEndInfo.layers is back-to-front per the OpenXR spec's
         * "Layer Ordering" section. */
        if (bz_quest_passthrough_build_layer(&renderer->passthrough, xr->appSpace,
                                            &passthroughLayer)) {
            layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&passthroughLayer;
        }
        projectionLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projectionLayer.space = xr->appSpace;
        /* Straight (non-premultiplied) alpha: see bz_quest_pure.h's
         * bz_quest_projection_layer_flags() comment and
         * tabletop_frag.frag - without BLEND_TEXTURE_SOURCE_ALPHA_BIT the
         * compositor ignores alpha entirely and this layer fully occludes
         * XR_FB_passthrough beneath it. */
        projectionLayer.layerFlags = bz_quest_projection_layer_flags(/*unpremultipliedAlpha=*/true);
        projectionLayer.viewCount = BZ_QUEST_VIEW_COUNT;
        projectionLayer.views = projectionViews;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&projectionLayer;
    }

    return bz_quest_xr_end_frame(xr, frameState.predictedDisplayTime, layers, layerCount);
}

void bz_quest_renderer_shutdown(bzQuestRenderer_t *renderer) {
    bz_quest_vk_wc3_hud_destroy(&renderer->wc3Hud);
    bz_quest_vk_wc3_fog_destroy(&renderer->wc3Fog);
    bz_quest_vk_wc3_terrain_destroy(&renderer->wc3Terrain);
    bz_quest_vk_wc3_destroy(&renderer->wc3);
    bz_quest_passthrough_destroy(&renderer->xr, &renderer->passthrough);
    bz_quest_vk_destroy(&renderer->vk);
    bz_quest_xr_destroy(&renderer->xr);
}
