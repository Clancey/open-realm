/*
 * bz_quest_renderer.h - wires bz_quest_xr.h (OpenXR instance/session/
 * swapchains), bz_quest_vk.h (Vulkan device/pipeline/render targets), and
 * bz_quest_passthrough.h (XR_FB_passthrough) together into the layer-3
 * frame loop. bz_quest_host.c (the android_native_app_glue entry point)
 * only ever calls these three functions - it never touches an XrInstance,
 * VkDevice, or XrPassthroughFB directly, keeping every OpenXR/Vulkan type
 * out of the host's Android-lifecycle plumbing.
 */
#ifndef BZ_QUEST_RENDERER_H
#define BZ_QUEST_RENDERER_H

#include <stdbool.h>

#include "bz_quest_passthrough.h"
#include "bz_quest_vk.h"
#include "bz_quest_vk_wc3.h"
#include "bz_quest_vk_wc3_fog.h"
#include "bz_quest_vk_wc3_hud.h"
#include "bz_quest_vk_wc3_terrain.h"
#include "bz_quest_wc3_render.h"
#include "bz_quest_xr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bzQuestRenderer_s {
    bzQuestXr_t xr;
    bzQuestVk_t vk;
    bzQuestPassthrough_t passthrough;
    /* Layer 5A: static Warcraft III model rendering. Owns its own Vulkan
     * resources entirely separately from `vk` above - see bz_quest_vk_wc3.h's
     * header comment for why. */
    bzQuestVkWc3_t wc3;
    bzQuestVkWc3Terrain_t wc3Terrain;
    bzQuestVkWc3Fog_t wc3Fog;
    /* Layer 5E: WC3 status/command-card HUD. Renders last (after fog/
     * selection) so it is never occluded by board geometry - see
     * bz_quest_vk_wc3_hud.h's header comment for authority/scope. */
    bzQuestVkWc3Hud_t wc3Hud;
} bzQuestRenderer_t;

/*
 * Runs every init step in bz_quest_xr.h/bz_quest_vk.h/bz_quest_passthrough.h's
 * dependency order (loader -> instance -> system -> functions -> Vulkan
 * requirements -> Vulkan instance/device -> blend mode -> views -> session
 * -> space -> swapchains -> render resources/targets -> passthrough
 * create+start -> layer 5A's bz_quest_vk_wc3_create()). `vm`/`context` are
 * android_app->activity->{vm,clazz} (see bz_quest_xr_init_loader()'s
 * comment for why this header stays android_native_app_glue-free). Returns
 * false (with every partially created resource already torn down via
 * bz_quest_renderer_shutdown()) on the first failed step - there is no
 * partial-success state a caller could observe.
 */
bool bz_quest_renderer_init(void *vm, void *context, bzQuestRenderer_t *renderer);

/*
 * One iteration of the OpenXR frame loop: polls events (advancing the
 * session state machine), and if the session is running, does
 * wait/begin/locate/[acquire/wait/render/release per eye]/end. Renders
 * nothing (but still begins/ends an empty frame, per the spec's
 * requirement that xrWaitFrame/xrBeginFrame/xrEndFrame are always called
 * in lockstep once a session is running) when the session isn't yet
 * running, tracking isn't valid yet, or frameState.shouldRender is false.
 * Once per frame (not per eye), captures the latest Warcraft III render
 * list via bz_quest_vk_wc3_capture_and_upload(); each eye then renders
 * either that render list (bz_quest_vk_wc3_render_target(), when it is
 * non-empty) or bz_quest_vk_render_target()'s procedural diagnostic scene
 * (when it is empty - an explicit "nothing valid to render yet" fallback,
 * never a silent one, per this slice's task contract). Returns false once
 * `renderer->xr.exitRequested` is set (loss pending, exiting, or a fatal
 * step failure) - the host's android_main loop must stop calling this and
 * proceed to bz_quest_renderer_shutdown().
 */
bool bz_quest_renderer_frame(bzQuestRenderer_t *renderer);

/* Reverse-dependency-order teardown across all four modules: layer 5A's
 * bz_quest_vk_wc3_destroy(), then passthrough, then Vulkan, then OpenXR
 * (mirrors bz_quest_xr_destroy()'s note that an active session must
 * already be stopped - bz_quest_renderer_frame()'s event polling drives
 * that transition before this runs). Safe to call on a
 * partially-initialized renderer. */
void bz_quest_renderer_shutdown(bzQuestRenderer_t *renderer);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_RENDERER_H */
