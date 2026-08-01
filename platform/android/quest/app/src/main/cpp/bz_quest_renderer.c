/*
 * bz_quest_renderer.c - see bz_quest_renderer.h.
 */
#include "bz_quest_renderer.h"

#include <math.h>
#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"
#include "platform/bridge/bz_tabletop_transport.h"

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

    /* Layer 6: OpenXR Touch action set (needs session+space, created above)
     * and the ray/reticle pipeline; the pure state machine starts at its
     * default board placement. */
    if (!bz_quest_xr_actions_create(xr, &renderer->xrActions)) goto fail;
    if (!bz_quest_vk_wc3_pointer_create(vk, &renderer->wc3Pointer)) goto fail;
    bz_quest_input_state_init(&renderer->inputState);

    BZ_QUEST_LOGI("renderer init complete");
    return true;

fail:
    BZ_QUEST_LOGE("renderer init failed - tearing down partial state");
    bz_quest_renderer_shutdown(renderer);
    return false;
}

/* Layer 6: maps the pure state machine's single decided command onto the
 * typed tabletop transport. `generation` is the SAME snapshot generation the
 * hit-test used for its staleness check (bz_quest_input_state.h) - the
 * transport rejects a stale post, which the caller turns into a "rejected"
 * haptic. No local player/entity/selection mutation: the effect appears only
 * in a later snapshot (server authoritative). */
static bzTTResult_t bz_quest_renderer_post_command(const bzQuestInputCommand_t *c, uint64_t generation) {
    switch (c->type) {
    case BZ_QUEST_INPUT_CMD_SELECT:
        return BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, generation, c->selectIds, c->selectCount);
    case BZ_QUEST_INPUT_CMD_SMART_ENTITY:
        return BZ_TT_PostSmartEntity(BZ_TABLETOP_ABI_VERSION, generation, c->targetEntity);
    case BZ_QUEST_INPUT_CMD_SMART_POINT:
        return BZ_TT_PostSmartPoint(BZ_TABLETOP_ABI_VERSION, generation, c->x, c->y);
    case BZ_QUEST_INPUT_CMD_BUTTON:
        return BZ_TT_PostButton(BZ_TABLETOP_ABI_VERSION, generation, c->code, strlen(c->code));
    case BZ_QUEST_INPUT_CMD_CANCEL:
        return BZ_TT_PostCancel(BZ_TABLETOP_ABI_VERSION, generation);
    case BZ_QUEST_INPUT_CMD_TARGET_POINT:
        return BZ_TT_PostTargetPoint(BZ_TABLETOP_ABI_VERSION, generation, c->x, c->y);
    default:
        return BZ_TT_OK; /* BZ_QUEST_INPUT_CMD_NONE - nothing to post */
    }
}

/* Ray/reticle tint per hit kind - warm for actionable HUD/entity hits, red for
 * a refused (disabled/stale) HUD slot, cyan for a bare terrain point, dim
 * white for no hit. RGBA straight-alpha (matches the marker/passthrough blend). */
static void bz_quest_renderer_pointer_tint(bzQuestInputHitKind_t kind, float out[4]) {
    static const float kAmber[4] = {1.0f, 0.75f, 0.20f, 0.90f};
    static const float kRed[4] = {0.90f, 0.20f, 0.20f, 0.85f};
    static const float kGreen[4] = {0.30f, 1.00f, 0.40f, 0.90f};
    static const float kCyan[4] = {0.30f, 0.80f, 1.00f, 0.85f};
    static const float kDim[4] = {0.80f, 0.80f, 0.85f, 0.50f};
    const float *c = kDim;
    switch (kind) {
    case BZ_QUEST_INPUT_HIT_HUD_BUTTON:
    case BZ_QUEST_INPUT_HIT_HUD_CANCEL: c = kAmber; break;
    case BZ_QUEST_INPUT_HIT_HUD_DISABLED: c = kRed; break;
    case BZ_QUEST_INPUT_HIT_ENTITY: c = kGreen; break;
    case BZ_QUEST_INPUT_HIT_TERRAIN: c = kCyan; break;
    default: c = kDim; break;
    }
    memcpy(out, c, sizeof(float) * 4);
}

/* Fallback ray length (meters) when a hand's ray hits nothing - the beam still
 * shows where the controller points. */
#define BZ_QUEST_RENDERER_POINTER_FALLBACK_LEN 2.5f

/* Layer 6 per-frame input: capture the interaction world, read the Touch
 * actions at `displayTime`, run the pure state machine, post the decided
 * command + haptic, and build this frame's ray/reticle geometry (in tracking
 * space) plus the board model matrix folded into the world view*projection.
 * Runs once per frame (not per eye). */
static void bz_quest_renderer_process_input(bzQuestRenderer_t *renderer, XrTime displayTime,
                                            float outBoardMatrix[16]) {
    bzQuestXr_t *xr = &renderer->xr;

    bz_quest_wc3_capture_interaction(&renderer->interaction);

    bzQuestInputFrame_t frame;
    memset(&frame, 0, sizeof(frame));
    int64_t last = renderer->lastDisplayTime;
    frame.dt = (last != 0 && displayTime > last) ? (float)((double)(displayTime - last) / 1e9) : 0.0f;
    renderer->lastDisplayTime = displayTime;

    bz_quest_xr_actions_sync(xr, &renderer->xrActions, displayTime, &frame);

    frame.world.hudFrame = bz_quest_vk_wc3_hud_has_frame(&renderer->wc3Hud)
                               ? bz_quest_vk_wc3_hud_frame(&renderer->wc3Hud)
                               : NULL;
    frame.world.entities = renderer->interaction.entities;
    frame.world.entityCount = renderer->interaction.entityCount;
    frame.world.transform = renderer->interaction.haveTransform ? &renderer->interaction.transform : NULL;
    frame.world.planeY = 0.0f;
    frame.world.generation = renderer->interaction.generation;
    frame.world.targetMode = renderer->interaction.targetMode;
    frame.selectedIds = renderer->interaction.selectedIds;
    frame.selectedCount = renderer->interaction.selectedCount;
    frame.mapEpoch = renderer->interaction.mapEpoch;

    bzQuestInputOutput_t out;
    memset(&out, 0, sizeof(out));
    bz_quest_input_state_update(&renderer->inputState, &frame, &out);

    if (out.hasCommand) {
        bzTTResult_t result = bz_quest_renderer_post_command(&out.command, frame.world.generation);
        bzQuestHapticPulse_t pulse = bz_quest_haptic_pulse(result == BZ_TT_OK);
        bz_quest_xr_actions_apply_haptic(xr, &renderer->xrActions, out.command.hand, &pulse);
    } else if (out.wantHaptic) {
        bzQuestHapticPulse_t pulse = bz_quest_haptic_pulse(out.hapticAccepted);
        bz_quest_xr_actions_apply_haptic(xr, &renderer->xrActions, out.hapticHand, &pulse);
    }

    bz_quest_board_transform_matrix(&renderer->inputState.board, outBoardMatrix);

    /* Build ray/reticle geometry in tracking space: reticle points come out of
     * the pure hit-test in COMPOSED space, so map them back into tracking
     * space with the SAME board transform the board geometry uses. */
    bzQuestVkWc3PointerHand_t hands[BZ_QUEST_INPUT_HAND_COUNT];
    memset(hands, 0, sizeof(hands));
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h) {
        const bzQuestInputHandSample_t *sample = &frame.hands[h];
        hands[h].visible = out.feedback.visible[h] && sample->aimValid;
        if (!hands[h].visible) continue;
        memcpy(hands[h].rayStart, sample->aimOrigin, sizeof(hands[h].rayStart));
        if (out.feedback.hasReticle[h]) {
            float tracking[3];
            bz_quest_board_transform_apply_point(&renderer->inputState.board, out.feedback.reticle[h],
                                                 tracking);
            memcpy(hands[h].rayEnd, tracking, sizeof(hands[h].rayEnd));
            memcpy(hands[h].reticle, tracking, sizeof(hands[h].reticle));
            hands[h].hasReticle = true;
        } else {
            for (int k = 0; k < 3; ++k)
                hands[h].rayEnd[k] =
                    sample->aimOrigin[k] + sample->aimDir[k] * BZ_QUEST_RENDERER_POINTER_FALLBACK_LEN;
            hands[h].hasReticle = false;
        }
        bz_quest_renderer_pointer_tint(out.feedback.hitKind[h], hands[h].tint);
    }
    bz_quest_vk_wc3_pointer_update(&renderer->wc3Pointer, hands);
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
    const float viewProj[16], const float pointerViewProj[16], const float cameraWorldPos[3],
    const bzQuestWc3TerrainRenderList_t *terrainList, const bzQuestWc3RenderList_t *wc3List) {
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
    /* Layer 6: ray pointers/reticles last, in PLAIN (non-board-folded)
     * view*projection - the controllers are physical objects in tracking
     * space; their reticle endpoints were already mapped out of composed
     * space by bz_quest_renderer_process_input(). Depth-tested so a beam is
     * occluded by nearer board geometry. */
    bz_quest_vk_wc3_pointer_record(&renderer->wc3Pointer, target->commandBuffer, pointerViewProj);

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

    /* Layer 6: read Touch actions + run the interaction state machine once per
     * frame (always while the session runs, even on a non-rendering frame, so
     * focus-loss/disconnect/map-change transient-state clears still fire). It
     * also produces this frame's board model matrix and ray/reticle geometry. */
    float boardMatrix[16];
    bz_quest_renderer_process_input(renderer, frameState.predictedDisplayTime, boardMatrix);

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
                       bz_quest_vk_wc3_hud_has_frame(&renderer->wc3Hud) ||
                       bz_quest_vk_wc3_pointer_has_geometry(&renderer->wc3Pointer)) {
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
                /* Fold the Quest-user board transform into this eye's
                 * view*projection so terrain, models, fog, selection markers,
                 * and the HUD panel all move/rotate/scale together
                 * (mvpBoard = mvp * board). The camera position the world
                 * shaders use must be expressed in the SAME composed space,
                 * so inverse-map the tracking-space head position through the
                 * board transform. The ray pointer is drawn with the plain
                 * mvp (physical controllers in tracking space). */
                const float cameraWorldPos[3] = {
                    views[i].pose.position.x,
                    views[i].pose.position.y,
                    views[i].pose.position.z,
                };
                float mvpBoard[16];
                bz_quest_mat4_multiply(mvp, boardMatrix, mvpBoard);
                float cameraComposed[3];
                bz_quest_board_transform_inverse_point(&renderer->inputState.board, cameraWorldPos,
                                                       cameraComposed);
                rendered = bz_quest_renderer_render_warcraft_target(
                    renderer, i, imageIndex, swapchain->width, swapchain->height, mvpBoard, mvp,
                    cameraComposed, &terrainRenderList, &wc3RenderList);
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
    /* Layer 6 first: clear any latched interaction state exactly once, then
     * release the ray pipeline and the OpenXR action set (before the session
     * is destroyed by bz_quest_xr_destroy() below). */
    bz_quest_input_state_clear(&renderer->inputState, /*resetBoard=*/true);
    bz_quest_vk_wc3_pointer_destroy(&renderer->wc3Pointer);
    bz_quest_xr_actions_destroy(&renderer->xr, &renderer->xrActions);
    bz_quest_vk_wc3_hud_destroy(&renderer->wc3Hud);
    bz_quest_vk_wc3_fog_destroy(&renderer->wc3Fog);
    bz_quest_vk_wc3_terrain_destroy(&renderer->wc3Terrain);
    bz_quest_vk_wc3_destroy(&renderer->wc3);
    bz_quest_passthrough_destroy(&renderer->xr, &renderer->passthrough);
    bz_quest_vk_destroy(&renderer->vk);
    bz_quest_xr_destroy(&renderer->xr);
}
