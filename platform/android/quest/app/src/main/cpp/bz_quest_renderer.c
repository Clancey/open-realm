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
            if (!bz_quest_renderer_build_mvp(&views[i], mvp)) {
                haveViews = false;
            } else if (!bz_quest_vk_render_target(vk, i, imageIndex, swapchain->width,
                                                  swapchain->height, mvp)) {
                haveViews = false;
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
    bz_quest_passthrough_destroy(&renderer->xr, &renderer->passthrough);
    bz_quest_vk_destroy(&renderer->vk);
    bz_quest_xr_destroy(&renderer->xr);
}
