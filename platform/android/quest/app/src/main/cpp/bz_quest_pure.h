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

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_PURE_H */
