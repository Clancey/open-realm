/*
 * bz_quest_xr.h - OpenXR instance/system/session/swapchain lifecycle for the
 * Quest Vulkan stereo renderer (layer 3). This module owns every OpenXR
 * handle and core-spec call (instance, system, session, reference space,
 * swapchains, event polling/session-state machine, frame timing). Vulkan
 * device/pipeline/render-target ownership lives in bz_quest_vk.h/.c;
 * XR_FB_passthrough lifecycle lives in bz_quest_passthrough.h/.c;
 * bz_quest_renderer.c wires all three together per frame.
 *
 * Every function here returns bool (true = success) and logs its own
 * failure via BZ_QUEST_LOGE before returning false - callers must treat a
 * false return as a hard failure (see bz_quest_renderer.c), never retry
 * blindly or substitute a fake-initialized handle.
 */
#ifndef BZ_QUEST_XR_H
#define BZ_QUEST_XR_H

#include <stdbool.h>
#include <stdint.h>

#include <jni.h>
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN 1
#define XR_USE_PLATFORM_ANDROID 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stereo only (Quest's XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) - see
 * bz_quest_xr_enumerate_views()'s comment for why this is a hard
 * requirement, not a runtime-selected count. */
#define BZ_QUEST_VIEW_COUNT 2u
/* Meta Quest runtimes triple-buffer XR swapchains in practice; sized with
 * headroom rather than tightly to the observed count so a runtime that
 * returns a couple more images doesn't silently truncate the array. */
#define BZ_QUEST_MAX_SWAPCHAIN_IMAGES 8u

/* Explicit function pointers this module resolves once via
 * xrGetInstanceProcAddr (extension functions have no exported trampoline,
 * unlike the core 1.0 entry points the OpenXR loader exports directly -
 * see bz_quest_xr.c's bz_quest_xr_load_functions()). */
typedef struct bzQuestXrFns_s {
    PFN_xrGetVulkanGraphicsRequirements2KHR getVulkanGraphicsRequirements2KHR;
    PFN_xrCreateVulkanInstanceKHR createVulkanInstanceKHR;
    PFN_xrCreateVulkanDeviceKHR createVulkanDeviceKHR;
    PFN_xrGetVulkanGraphicsDevice2KHR getVulkanGraphicsDevice2KHR;
    PFN_xrCreatePassthroughFB createPassthroughFB;
    PFN_xrDestroyPassthroughFB destroyPassthroughFB;
    PFN_xrPassthroughStartFB passthroughStartFB;
    PFN_xrPassthroughPauseFB passthroughPauseFB;
    PFN_xrCreatePassthroughLayerFB createPassthroughLayerFB;
    PFN_xrDestroyPassthroughLayerFB destroyPassthroughLayerFB;
} bzQuestXrFns_t;

typedef struct bzQuestXrSwapchain_s {
    XrSwapchain handle;
    int64_t format;
    uint32_t width;
    uint32_t height;
    uint32_t imageCount;
    VkImage images[BZ_QUEST_MAX_SWAPCHAIN_IMAGES];
} bzQuestXrSwapchain_t;

/*
 * Layer 8: hand-tracking capability, negotiated ONCE by
 * bz_quest_xr_create_instance() (extEnabled/aimExtEnabled - was the
 * extension name actually present in xrEnumerateInstanceExtensionProperties
 * and requested at xrCreateInstance) and bz_quest_xr_get_system()
 * (supported/aimSupported - the final usable-capability gates
 * bz_quest_xr_hands.h's bz_quest_xr_hands_create()/_sync() actually check,
 * additionally requiring the system to report
 * XrSystemHandTrackingPropertiesEXT.supportsHandTracking). Both
 * XR_EXT_hand_tracking and XR_FB_hand_tracking_aim are OPTIONAL - unlike
 * XR_FB_passthrough, their absence is never a startup failure (see
 * docs/quest-tabletop.md's "Layer 8" capability-negotiation contract).
 * `aimSupported` additionally requires `supported`: XR_FB_hand_tracking_aim
 * depends="XR_VERSION_1_0+XR_EXT_hand_tracking" per the OpenXR registry
 * (xr.xml), verified against the extracted openxr_loader_for_android 1.1.49
 * headers - see docs/quest-tabletop.md.
 */
typedef struct {
    bool extEnabled;
    bool aimExtEnabled;
    bool supported;
    bool aimSupported;
} bzQuestXrHandCapability_t;

typedef struct bzQuestXr_s {
    XrInstance instance;
    XrSystemId systemId;
    XrSession session;
    XrSpace appSpace;
    bzQuestXrFns_t fns;

    uint64_t passthroughCapabilities; /* XrSystemPassthroughProperties2FB.capabilities */
    bzQuestXrHandCapability_t handCapability; /* XR_EXT_hand_tracking / XR_FB_hand_tracking_aim - see above */

    XrSessionState sessionState;
    bool sessionRunning;      /* true between xrBeginSession and xrEndSession */
    bool exitRequested;       /* instance loss pending or app requested exit */

    XrEnvironmentBlendMode blendMode;
    XrViewConfigurationView viewConfigs[BZ_QUEST_VIEW_COUNT];
    bzQuestXrSwapchain_t swapchains[BZ_QUEST_VIEW_COUNT];
} bzQuestXr_t;

/* Must be the first OpenXR call of any kind on Android (XR_KHR_
 * loader_init_android) - hands the loader the JavaVM/Context it needs to
 * find the runtime broker. `vm`/`context` are android_app->activity->
 * {vm,clazz}, passed as void* so this header does not need
 * android_native_app_glue.h. */
bool bz_quest_xr_init_loader(void *vm, void *context);

/*
 * Enumerates the runtime's supported instance extensions, verifies
 * XR_KHR_android_create_instance, XR_KHR_vulkan_enable2, and
 * XR_FB_passthrough are all present (hard failure if any are missing - see
 * bz_quest_pure.h's bz_quest_check_required_names(), which this calls), and
 * creates `xr->instance` with those three enabled.
 *
 * Layer 8: ALSO probes (never hard-fails on absence) XR_EXT_hand_tracking
 * and, only when that succeeds, XR_FB_hand_tracking_aim, recording which
 * were found into xr->handCapability.extEnabled/aimExtEnabled and enabling
 * exactly the ones present alongside the three required extensions above -
 * see bzQuestXrHandCapability_t's comment for the full negotiation contract.
 */
bool bz_quest_xr_create_instance(void *vm, void *context, bzQuestXr_t *xr);

/* xrGetSystem(HEAD_MOUNTED_DISPLAY) -> xr->systemId, then
 * xrGetSystemProperties chained with XrSystemPassthroughProperties2FB to
 * populate xr->passthroughCapabilities. Hard-fails if the runtime doesn't
 * report XR_PASSTHROUGH_CAPABILITY_BIT_FB - this layer's Quest 3/3S MR
 * prototype has no non-passthrough fallback path (see
 * docs/quest-tabletop.md and bz_quest_pure.h's bz_quest_passthrough_capable()).
 *
 * Layer 8: when xr->handCapability.extEnabled, ALSO chains
 * XrSystemHandTrackingPropertiesEXT onto the same query (never chained when
 * the extension was not enabled - chaining a struct for a disabled
 * extension is invalid per the OpenXR spec's extensibility rules) and
 * populates xr->handCapability.supported/aimSupported. Absence is logged
 * once, never a hard failure - see bzQuestXrHandCapability_t's comment. */
bool bz_quest_xr_get_system(bzQuestXr_t *xr);

/* Resolves every extension function pointer in xr->fns via
 * xrGetInstanceProcAddr, failing loudly if any resolve NULL. */
bool bz_quest_xr_load_functions(bzQuestXr_t *xr);

/* xrGetVulkanGraphicsRequirements2KHR - must be called before creating the
 * Vulkan instance/device (bz_quest_vk_create_instance/_device consume the
 * result to pick a compliant VkApplicationInfo::apiVersion). */
bool bz_quest_xr_get_vulkan_requirements(bzQuestXr_t *xr, XrGraphicsRequirementsVulkanKHR *outReq);

/*
 * Enumerates the runtime's supported environment blend modes for
 * XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO and requires
 * XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND (the mode passthrough compositing
 * needs) to be present, storing it in xr->blendMode. Per this layer's
 * explicit Quest 3/3S MR requirement (see docs/quest-tabletop.md), a
 * runtime that only offers OPAQUE/ADDITIVE fails this call rather than
 * silently falling back to a non-passthrough blend mode.
 */
bool bz_quest_xr_select_blend_mode(bzQuestXr_t *xr);

/*
 * Verifies XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO is in the runtime's
 * supported view configuration list (a hard requirement - this renderer
 * never falls back to a different view configuration type) and enumerates
 * its BZ_QUEST_VIEW_COUNT XrViewConfigurationViews into xr->viewConfigs.
 */
bool bz_quest_xr_enumerate_views(bzQuestXr_t *xr);

/* xrCreateSession with an XrGraphicsBindingVulkanKHR next-chain entry. */
bool bz_quest_xr_create_session(bzQuestXr_t *xr, const XrGraphicsBindingVulkanKHR *binding);

/* xrCreateReferenceSpace(LOCAL) -> xr->appSpace - see docs/quest-tabletop.md
 * for why LOCAL (guaranteed by every OpenXR runtime) was chosen over the
 * optional STAGE space for this test scene. */
bool bz_quest_xr_create_space(bzQuestXr_t *xr);

/*
 * Enumerates the runtime's supported swapchain formats, selects one via
 * bz_quest_pure.h's bz_quest_select_swapchain_format() from
 * `preferredFormats`/`preferredCount` (hard failure if none supported), and
 * creates one XrSwapchain per view at that view's recommended image
 * rect/sample-count-1 (no MSAA - see docs/quest-tabletop.md's "Current
 * limitations" for why), populating xr->swapchains[].
 */
bool bz_quest_xr_create_swapchains(bzQuestXr_t *xr, const int64_t *preferredFormats,
                                   uint32_t preferredCount);

/*
 * Pumps xrPollEvent to drain the queue, updating xr->sessionState and
 * driving the OpenXR session state machine: calls xrBeginSession on READY,
 * xrEndSession + clears xr->sessionRunning on STOPPING, and sets
 * xr->exitRequested on LOSS_PENDING/EXITING or XrEventDataInstanceLossPending
 * (see the OpenXR 1.0 spec's "Session lifecycle" section - each transition
 * has exactly one correct action, asserted individually rather than
 * inferred from a boolean).
 */
void bz_quest_xr_poll_events(bzQuestXr_t *xr);

/* True while frames should be submitted (xrBeginSession has succeeded and
 * xrEndSession has not yet been called for the current session). */
bool bz_quest_xr_is_session_running(const bzQuestXr_t *xr);

bool bz_quest_xr_wait_frame(bzQuestXr_t *xr, XrFrameState *outState);
bool bz_quest_xr_begin_frame(bzQuestXr_t *xr);
bool bz_quest_xr_locate_views(bzQuestXr_t *xr, XrTime displayTime, XrView outViews[BZ_QUEST_VIEW_COUNT]);
bool bz_quest_xr_end_frame(bzQuestXr_t *xr, XrTime displayTime,
                           const XrCompositionLayerBaseHeader *const *layers, uint32_t layerCount);

bool bz_quest_xr_acquire_swapchain_image(bzQuestXrSwapchain_t *swapchain, uint32_t *outIndex);
bool bz_quest_xr_wait_swapchain_image(bzQuestXrSwapchain_t *swapchain);
bool bz_quest_xr_release_swapchain_image(bzQuestXrSwapchain_t *swapchain);

/* Reverse-dependency-order teardown: swapchains, space, session, instance.
 * Safe to call on a partially-initialized xr (every handle is checked
 * against XR_NULL_HANDLE before destroying). Does not destroy an
 * XR_SESSION_STATE_STOPPING session that hasn't received xrEndSession yet -
 * callers must drain events (bz_quest_xr_poll_events) to STOPPING first. */
void bz_quest_xr_destroy(bzQuestXr_t *xr);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_XR_H */
