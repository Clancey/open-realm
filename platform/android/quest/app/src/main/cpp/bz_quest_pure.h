/*
 * bz_quest_pure.h - platform-independent math/selection helpers for the
 * Quest OpenXR/Vulkan renderer (layer 3).
 *
 * Every function here takes and returns plain float/int/const-char* types
 * only - never an XrFovf/XrPosef/VkFormat/XrExtensionProperties etc. This is
 * deliberate: it lets platform/android/quest/tests/test_bz_quest_pure.c
 * build and run these exact decision paths on the host with a plain C
 * compiler, no NDK/Android SDK/OpenXR loader/Vulkan headers required. The
 * real Android-side callers (bz_quest_xr.c, bz_quest_vk.c) unpack the
 * matching OpenXR/Vulkan struct fields into these plain arguments and pass
 * the SDK-typed values back out again - see those files' call sites.
 */
#ifndef BZ_QUEST_PURE_H
#define BZ_QUEST_PURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Builds a Vulkan-clip-space (right-handed view space, [0,1] depth range,
 * Y flipped for Vulkan's top-left NDC origin) perspective projection matrix
 * from an OpenXR asymmetric field-of-view (XrFovf's four angles, radians).
 * Column-major 4x4, matching Vulkan's/OpenXR's expected memory layout, so
 * bz_quest_vk.c can pass `out` straight into a push-constant/uniform buffer.
 * Mirrors Khronos OpenXR-SDK-Source's src/common/xr_linear.h
 * XrMatrix4x4f_CreateProjectionFov (verified from that file, main branch,
 * 2026-05-28 - see docs/quest-tabletop.md), adapted for Vulkan's flipped Y
 * and [0,1] Z range instead of that header's OpenGL variant.
 *
 * Returns false (leaving `out` unmodified) if nearZ/farZ are non-positive or
 * farZ <= nearZ, or if any angle pair is degenerate (left == right, or
 * up == down) - a caller passing a zero-width frustum has a bug upstream,
 * not something to silently clamp.
 */
bool bz_quest_fov_projection_vk(float angleLeft, float angleRight, float angleUp, float angleDown,
                                 float nearZ, float farZ, float out[16]);

/*
 * Builds a view matrix (world-to-eye-space) from an eye pose expressed as a
 * position (eyeX/Y/Z) and orientation quaternion (qx,qy,qz,qw - OpenXR's
 * XrQuaternionf field order). This is the inverse of the eye's model
 * (camera-to-world) transform: transpose(rotation) then translate by
 * -rotation^T * position, so `out` can be used directly as the "view" half
 * of an MVP without a separate matrix inversion step.
 *
 * Returns false (leaving `out` unmodified) if the quaternion's magnitude is
 * not within BZ_QUEST_QUAT_EPS of 1.0 - OpenXR guarantees normalized pose
 * orientations, so a non-unit quaternion here means an upstream bug (e.g. an
 * uninitialized XrPosef), not something to silently re-normalize.
 */
bool bz_quest_pose_to_view_matrix(float eyeX, float eyeY, float eyeZ, float qx, float qy, float qz,
                                   float qw, float out[16]);

/* Column-major 4x4 matrix multiply: out = a * b. `out` must not alias `a` or `b`. */
void bz_quest_mat4_multiply(const float a[16], const float b[16], float out[16]);

/*
 * Rotates the canonical local -Z "forward" basis vector by quaternion
 * (qx,qy,qz,qw) and writes the resulting world-space direction to `out` -
 * every OpenXR pose this renderer treats as an aim direction (controller
 * aim pose, XR_FB_hand_tracking_aim's aimPose) shares this same "-Z is
 * forward" convention. Provably the same rotation
 * bz_quest_pose_to_view_matrix()'s rotation matrix encodes (out equals the
 * negation of that matrix's third column for v=(0,0,-1) - verified by hand
 * expansion, see docs/quest-tabletop.md), so this is not a second,
 * independently-trusted quaternion convention. Does not require a unit
 * quaternion precondition check like bz_quest_pose_to_view_matrix() - a
 * slightly denormalized input still yields a direction of near-unit length,
 * which is all an aim ray needs (unlike a view matrix, where a non-unit
 * rotation would visibly skew the whole scene).
 *
 * Pulled out as a single shared implementation (AGENTS.md's DRY rule) so
 * bz_quest_xr_actions.c's controller aim ray and bz_quest_xr_hands.c's
 * XR_FB_hand_tracking_aim ray both compute this identically instead of
 * maintaining two copies of the same quaternion-vector rotation.
 */
void bz_quest_quat_forward(float qx, float qy, float qz, float qw, float out[3]);

/*
 * Picks the first runtime-supported format (in caller-supplied preference
 * order) from `preferred`/`preferredCount` that also appears in
 * `runtimeFormats`/`runtimeCount` (as returned by
 * xrEnumerateSwapchainFormats). Writes the match to *outFormat and returns
 * true; returns false (leaving *outFormat unmodified) if none of the
 * preferred formats are supported - the caller must treat that as a hard
 * startup failure (see bz_quest_vk.c), never silently fall back to
 * runtimeFormats[0], which may be a format this renderer's fixed pipeline
 * (sRGB color, D32 depth) cannot interpret correctly.
 */
bool bz_quest_select_swapchain_format(const int64_t *runtimeFormats, uint32_t runtimeCount,
                                      const int64_t *preferred, uint32_t preferredCount,
                                      int64_t *outFormat);

/*
 * Verifies every name in `required`/`requiredCount` appears in
 * `available`/`availableCount` (both plain C-string arrays, e.g. unpacked
 * from XrExtensionProperties.extensionName / VkExtensionProperties.
 * extensionName by the caller). Returns true iff all are present. On
 * failure, if `outMissing` is non-NULL, *outMissing is set to the first
 * required name not found (a pointer into the caller's `required` array,
 * not a copy) so the caller can log exactly which one to explain the
 * failure, instead of just "some extension is missing".
 */
bool bz_quest_check_required_names(const char *const *available, uint32_t availableCount,
                                    const char *const *required, uint32_t requiredCount,
                                    const char **outMissing);

/*
 * Converts an XrVersion-encoded Vulkan API version bound (as returned in
 * XrGraphicsRequirementsVulkanKHR.minApiVersionSupported/
 * maxApiVersionSupported) into a packed Vulkan VkApplicationInfo::
 * apiVersion. These two SDKs both call their macros XR_MAKE_VERSION/
 * VK_MAKE_API_VERSION, but use different bit layouts (OpenXR: 16/16/32-bit
 * major/minor/patch; Vulkan: 3/7/10/12-bit variant/major/minor/patch) - the
 * OpenXR spec for XR_KHR_vulkan_enable2 states minApiVersionSupported/
 * maxApiVersionSupported reuse *OpenXR's* XR_MAKE_VERSION encoding to
 * describe a Vulkan version bound, so a caller cannot pass the raw
 * XrVersion straight into VkApplicationInfo::apiVersion - see
 * docs/quest-tabletop.md for the exact spec text this was verified
 * against. The returned value always uses Vulkan variant 0 and patch 0
 * (only major.minor is meaningful for a version *bound*).
 */
uint32_t bz_quest_xr_version_to_vk_api_version(uint64_t xrEncodedVersion);

/*
 * Tests whether `capabilityFlags` (as returned in
 * XrSystemPassthroughProperties2FB.capabilities) grants every bit set in
 * `requiredBits`. A trivial bitmask AND, pulled out into its own testable
 * function rather than an inline `&` at the call site so the "Quest 3/3S MR
 * is required, not an optional fallback" contract documented in
 * docs/quest-tabletop.md has one obvious, covered decision point instead of
 * being duplicated at every capability check.
 */
bool bz_quest_passthrough_capable(uint64_t capabilityFlags, uint64_t requiredBits);

/*
 * Returns the XrCompositionLayerFlags bits an XrCompositionLayerProjection
 * must set so its per-fragment alpha is honored by the compositor instead
 * of being ignored (in which case the layer would be treated as fully
 * opaque and would completely occlude XR_FB_passthrough beneath it,
 * regardless of what alpha the fragment shader wrote).
 *
 * Per the OpenXR 1.1 spec's "Composition Layers" chapter (XrCompositionLayerBaseHeader
 * flags description, registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
 * #XrCompositionLayerFlags, verified against the extracted
 * org.khronos.openxr:openxr_loader_for_android 1.1.49 openxr.h - see
 * docs/quest-tabletop.md):
 *
 *   - XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT must be set, or
 *     the runtime ignores the layer's alpha channel entirely and treats
 *     every texel as fully opaque (alpha=1) no matter what the shader
 *     wrote - this is the bit that makes alpha matter at all.
 *   - XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT must be set ONLY when
 *     the render target's final RGBA is *straight* (non-premultiplied - a
 *     half-transparent red pixel stored as full-intensity red with
 *     alpha=0.5, not half-intensity red); it must be OMITTED (as this
 *     project's caller now always does - `unpremultipliedAlpha=false`, PR
 *     #28) when the render target is *premultiplied* (that same pixel
 *     stored as half-intensity red, alpha=0.5).
 *
 * This project's render target IS premultiplied, by construction: every WC3
 * render pass (bz_quest_vk_wc3.c's model/particle 7-mode blend table,
 * bz_quest_vk_wc3_terrain.c/_fog.c/_hud.c/_pointer.c's shared
 * bz_quest_vk_straight_over_blend_state()) uses `srcColorBlendFactor=
 * SRC_ALPHA, dstColorBlendFactor=ONE_MINUS_SRC_ALPHA` starting from this
 * render pass's (0,0,0,0)-cleared background (bz_quest_vk_create_render_
 * resources()'s clear value) - the standard Porter-Duff "over" operator for
 * a straight-color shader input, which happens to accumulate a valid
 * PREMULTIPLIED result in the render target with no extra shader work,
 * PROVIDED the alpha channel accumulates via its own separately-correct
 * coverage factor pair (srcAlphaBlendFactor=ONE, NOT mirroring the color
 * factor - see that function's own doc comment for the a-squared defect
 * this avoids). tabletop_frag.frag's simpler `vec4(fragColor, 1.0)` (always
 * alpha 0 or 1) is trivially premultiplied too (multiplying by 0 or 1
 * changes nothing), so this ONE shared `unpremultipliedAlpha=false` call
 * correctly covers both the diagnostic scene and every WC3 pass.
 *
 * HISTORY (High-severity reviewer finding, PR #28): an earlier revision
 * passed `unpremultipliedAlpha=true` here, reasoning (correctly, at the
 * time) that tabletop_frag.frag's own straight, always-0-or-1 alpha needed
 * it - but every blended WC3 pass added since then mirrors its color
 * factors onto the alpha channel that decision assumed didn't exist, which
 * both squares the accumulated coverage alpha AND then gets misinterpreted
 * a second time by the compositor believing straight input, double-
 * darkening semi-transparent content and leaking passthrough through fog/
 * water/HUD/selection markers. Fixed by adopting ONE coherent premultiplied
 * contract end to end instead - see docs/quest-tabletop.md's premultiplied-
 * contract section for the full numeric reproduction and derivation.
 */
uint64_t bz_quest_projection_layer_flags(bool unpremultipliedAlpha);

/* Exposed (not just used internally by bz_quest_pure.c) so callers building
 * XrCompositionLayerFlags directly - see bz_quest_renderer.c - can
 * _Static_assert these mirrored literals still match the real
 * XR_COMPOSITION_LAYER_*_BIT constants from openxr.h at compile time,
 * catching any future spec/header drift instead of silently diverging. */
#define BZ_QUEST_XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT_VALUE 0x00000002ULL
#define BZ_QUEST_XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT_VALUE 0x00000004ULL

/*
 * Picks the ALooper_pollOnce() timeout (milliseconds; -1 means "block
 * indefinitely") bz_quest_host.c's android_main loop should use this
 * iteration.
 *
 * Returns 0 (poll without blocking, spending CPU to keep the loop spinning)
 * if EITHER `wantsXrEventPolling` or `xrSessionRunning` is true; returns -1
 * (block until the next real Android input/lifecycle event, costing
 * nothing) only when both are false.
 *
 * `xrSessionRunning` alone is not sufficient: xrPollEvent (called from
 * bz_quest_renderer_frame -> bz_quest_xr_poll_events) is the *only* way to
 * observe XR_SESSION_STATE_READY and drive xrBeginSession - and that call
 * only happens after ALooper_pollOnce returns. If the loop only polled
 * without blocking while a session was already running, an app resumed
 * from the background (androidResumed=true, xrSessionRunning still false
 * because xrBeginSession hasn't run yet) would have ALooper_pollOnce block
 * indefinitely (-1) waiting for an Android event that may never arrive
 * (e.g. no touch/key input), starving xrPollEvent forever and never
 * observing the READY event that would start the session - a permanent
 * hang after every resume with no user input. Polling non-blocking
 * whenever the app is resumed closes that race, matching the lifecycle
 * pattern OpenXR-SDK-Source's hello_xr sample uses (poll non-blocking
 * whenever the app is not fully backgrounded, not only once a session is
 * confirmed running).
 *
 * `wantsXrEventPolling` must be `androidResumed && rendererInitSucceeded`
 * at the call site, not `androidResumed` alone - if renderer init failed
 * (see bz_quest_host.c's bz_quest_ensure_renderer_init()), there is no XR
 * instance/session to poll events for, so spinning at a 0ms timeout while
 * merely resumed would be a pure busy-loop with nothing to do, violating
 * the "no busy loop" requirement for that failure case.
 */
int bz_quest_looper_timeout_millis(bool wantsXrEventPolling, bool xrSessionRunning);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_PURE_H */
