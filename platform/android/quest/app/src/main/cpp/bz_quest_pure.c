/*
 * bz_quest_pure.c - see bz_quest_pure.h. No Android/Vulkan/OpenXR headers
 * are included here on purpose (see that header's top comment) so
 * platform/android/quest/tests/test_bz_quest_pure_main.c can build and run
 * this file with a plain host C compiler.
 */
#include "bz_quest_pure.h"

#include <math.h>
#include <string.h>

/* OpenXR guarantees XrPosef.orientation is normalized; this is the slack
 * allowed before bz_quest_pose_to_view_matrix() treats it as caller error
 * (see that function's header comment) rather than silently normalizing a
 * bad input. */
#define BZ_QUEST_QUAT_EPS 0.01f

bool bz_quest_fov_projection_vk(float angleLeft, float angleRight, float angleUp, float angleDown,
                                 float nearZ, float farZ, float out[16]) {
    if (nearZ <= 0.0f || farZ <= nearZ) return false;

    const float tanLeft = tanf(angleLeft);
    const float tanRight = tanf(angleRight);
    const float tanDown = tanf(angleDown);
    const float tanUp = tanf(angleUp);

    const float tanWidth = tanRight - tanLeft;
    /* Vulkan clip space has +Y down, unlike OpenGL's +Y up - see
     * Khronos OpenXR-SDK-Source's src/common/xr_linear.h
     * XrMatrix4x4f_CreateProjectionFov, whose GRAPHICS_VULKAN branch swaps
     * this subtraction order for exactly this reason (docs/quest-tabletop.md
     * cites the exact file/commit this was verified against). */
    const float tanHeight = tanDown - tanUp;
    if (tanWidth == 0.0f || tanHeight == 0.0f) return false;

    memset(out, 0, 16 * sizeof(float));
    out[0] = 2.0f / tanWidth;
    out[8] = (tanRight + tanLeft) / tanWidth;
    out[5] = 2.0f / tanHeight;
    out[9] = (tanUp + tanDown) / tanHeight;
    /* Vulkan's depth range is [0,1] (offsetZ=0 in xr_linear.h terms), not
     * OpenGL's [-1,1], so these two entries omit that header's offsetZ
     * term entirely rather than adding a zero. */
    out[10] = -farZ / (farZ - nearZ);
    out[14] = -(farZ * nearZ) / (farZ - nearZ);
    out[11] = -1.0f;
    return true;
}

bool bz_quest_pose_to_view_matrix(float eyeX, float eyeY, float eyeZ, float qx, float qy, float qz,
                                   float qw, float out[16]) {
    const float magSq = qx * qx + qy * qy + qz * qz + qw * qw;
    if (fabsf(magSq - 1.0f) > BZ_QUEST_QUAT_EPS) return false;

    /* Row-major rotation matrix R with v_world = R * v_local (R's columns
     * are the eye's local axes expressed in world space). */
    const float r00 = 1.0f - 2.0f * (qy * qy + qz * qz);
    const float r01 = 2.0f * (qx * qy - qz * qw);
    const float r02 = 2.0f * (qx * qz + qy * qw);
    const float r10 = 2.0f * (qx * qy + qz * qw);
    const float r11 = 1.0f - 2.0f * (qx * qx + qz * qz);
    const float r12 = 2.0f * (qy * qz - qx * qw);
    const float r20 = 2.0f * (qx * qz - qy * qw);
    const float r21 = 2.0f * (qy * qz + qx * qw);
    const float r22 = 1.0f - 2.0f * (qx * qx + qy * qy);

    /* View = transpose(R) with translation -transpose(R) * position, so the
     * result is directly usable as world-to-eye-space without a separate
     * matrix-inverse step (R is orthonormal, so transpose == inverse). */
    out[0] = r00;  out[1] = r01;  out[2] = r02;  out[3] = 0.0f;
    out[4] = r10;  out[5] = r11;  out[6] = r12;  out[7] = 0.0f;
    out[8] = r20;  out[9] = r21;  out[10] = r22; out[11] = 0.0f;
    out[12] = -(r00 * eyeX + r10 * eyeY + r20 * eyeZ);
    out[13] = -(r01 * eyeX + r11 * eyeY + r21 * eyeZ);
    out[14] = -(r02 * eyeX + r12 * eyeY + r22 * eyeZ);
    out[15] = 1.0f;
    return true;
}

void bz_quest_mat4_multiply(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) sum += a[k * 4 + row] * b[col * 4 + k];
            out[col * 4 + row] = sum;
        }
    }
}

bool bz_quest_select_swapchain_format(const int64_t *runtimeFormats, uint32_t runtimeCount,
                                      const int64_t *preferred, uint32_t preferredCount,
                                      int64_t *outFormat) {
    for (uint32_t p = 0; p < preferredCount; p++) {
        for (uint32_t r = 0; r < runtimeCount; r++) {
            if (runtimeFormats[r] == preferred[p]) {
                *outFormat = preferred[p];
                return true;
            }
        }
    }
    return false;
}

bool bz_quest_check_required_names(const char *const *available, uint32_t availableCount,
                                    const char *const *required, uint32_t requiredCount,
                                    const char **outMissing) {
    for (uint32_t i = 0; i < requiredCount; i++) {
        bool found = false;
        for (uint32_t j = 0; j < availableCount; j++) {
            if (strcmp(required[i], available[j]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            if (outMissing) *outMissing = required[i];
            return false;
        }
    }
    return true;
}

bool bz_quest_passthrough_capable(uint64_t capabilityFlags, uint64_t requiredBits) {
    return (capabilityFlags & requiredBits) == requiredBits;
}

uint32_t bz_quest_xr_version_to_vk_api_version(uint64_t xrEncodedVersion) {
    const uint16_t major = (uint16_t)((xrEncodedVersion >> 48) & 0xffffu);
    const uint16_t minor = (uint16_t)((xrEncodedVersion >> 32) & 0xffffu);
    /* VK_MAKE_API_VERSION(variant, major, minor, patch), variant=patch=0. */
    return ((uint32_t)major << 22) | ((uint32_t)minor << 12);
}

/* Literal values must match XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
 * (0x00000002) / XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
 * (0x00000004) from the OpenXR 1.1.49 openxr.h - see bz_quest_pure.h's
 * comment on bz_quest_projection_layer_flags() for why this file cannot
 * #include openxr.h directly (it must stay host-buildable). Callers in
 * bz_quest_renderer.c cross-check these macros against the real
 * XR_COMPOSITION_LAYER_*_BIT constants with a _Static_assert - see that
 * file - so any future spec/header drift fails the build instead of
 * silently diverging. */
uint64_t bz_quest_projection_layer_flags(bool unpremultipliedAlpha) {
    uint64_t flags = BZ_QUEST_XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT_VALUE;
    if (unpremultipliedAlpha) flags |= BZ_QUEST_XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT_VALUE;
    return flags;
}

int bz_quest_looper_timeout_millis(bool wantsXrEventPolling, bool xrSessionRunning) {
    return (wantsXrEventPolling || xrSessionRunning) ? 0 : -1;
}
