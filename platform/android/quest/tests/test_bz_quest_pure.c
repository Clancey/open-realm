/*
 * test_bz_quest_pure.c - coverage for bz_quest_pure.c's projection/view
 * matrix, swapchain-format-selection, extension-requirement, and
 * passthrough-capability helpers. Each covers both a normal path and its
 * inverse/error path, per AGENTS.md's test discipline.
 */
#include "bz_quest_pure.h"
#include "test_framework.h"

#include <math.h>

#define BZ_QUEST_TEST_PI 3.14159265358979323846f

static void test_fov_projection_normal(void) {
    /* Symmetric 90-degree-wide FOV: angleLeft=-45deg, angleRight=+45deg,
     * angleUp=+45deg, angleDown=-45deg -> tanWidth = tan45 - tan(-45) = 2,
     * tanHeight = tan(-45) - tan45 = -2 (Vulkan's Y-down clip space negates
     * this vs. an OpenGL-style up-down subtraction - see bz_quest_pure.c). */
    float out[16];
    bool ok = bz_quest_fov_projection_vk(-BZ_QUEST_TEST_PI / 4.0f, BZ_QUEST_TEST_PI / 4.0f,
                                          BZ_QUEST_TEST_PI / 4.0f, -BZ_QUEST_TEST_PI / 4.0f, 0.1f,
                                          100.0f, out);
    ASSERT(ok);
    ASSERT_EQ_FLOAT(out[0], 1.0f, 0.0001f);   /* 2/tanWidth = 2/2 */
    ASSERT_EQ_FLOAT(out[5], -1.0f, 0.0001f);  /* 2/tanHeight = 2/-2 */
    ASSERT_EQ_FLOAT(out[8], 0.0f, 0.0001f);   /* symmetric: (right+left)/width = 0 */
    ASSERT_EQ_FLOAT(out[9], 0.0f, 0.0001f);   /* symmetric: (up+down)/height = 0 */
    ASSERT_EQ_FLOAT(out[11], -1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[15], 0.0f, 0.0001f);
    /* Vulkan [0,1] depth range: view space looks down -Z (right-handed), so
     * a point at the near/far plane has view-space z = -nearZ/-farZ. NDC
     * depth = clipZ/clipW = (m10*z + m14) / (m11*z) should map those to 0/1
     * respectively. */
    float depthNear = (out[10] * -0.1f + out[14]) / (out[11] * -0.1f);
    float depthFar = (out[10] * -100.0f + out[14]) / (out[11] * -100.0f);
    ASSERT_EQ_FLOAT(depthNear, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(depthFar, 1.0f, 0.001f);
}

static void test_fov_projection_asymmetric(void) {
    /* Asymmetric FOV (as OpenXR actually reports per-eye) must not collapse
     * the (right+left)/width and (up+down)/height terms to zero. */
    float out[16];
    bool ok = bz_quest_fov_projection_vk(-0.6f, 0.8f, 0.7f, -0.5f, 0.05f, 50.0f, out);
    ASSERT(ok);
    ASSERT(out[8] != 0.0f);
    ASSERT(out[9] != 0.0f);
}

static void test_fov_projection_rejects_invalid_z(void) {
    float out[16];
    memset(out, 0x7f, sizeof(out));
    /* nearZ <= 0 */
    ASSERT(!bz_quest_fov_projection_vk(-0.5f, 0.5f, 0.5f, -0.5f, 0.0f, 10.0f, out));
    ASSERT(!bz_quest_fov_projection_vk(-0.5f, 0.5f, 0.5f, -0.5f, -1.0f, 10.0f, out));
    /* farZ <= nearZ */
    ASSERT(!bz_quest_fov_projection_vk(-0.5f, 0.5f, 0.5f, -0.5f, 10.0f, 10.0f, out));
    ASSERT(!bz_quest_fov_projection_vk(-0.5f, 0.5f, 0.5f, -0.5f, 10.0f, 5.0f, out));
}

static void test_fov_projection_rejects_degenerate_angles(void) {
    float out[16];
    /* angleLeft == angleRight -> zero-width frustum. */
    ASSERT(!bz_quest_fov_projection_vk(0.5f, 0.5f, 0.5f, -0.5f, 0.1f, 100.0f, out));
    /* angleUp == angleDown -> zero-height frustum. */
    ASSERT(!bz_quest_fov_projection_vk(-0.5f, 0.5f, 0.5f, 0.5f, 0.1f, 100.0f, out));
}

static void test_pose_to_view_matrix_identity(void) {
    float out[16];
    bool ok = bz_quest_pose_to_view_matrix(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, out);
    ASSERT(ok);
    static const float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    for (int i = 0; i < 16; i++) ASSERT_EQ_FLOAT(out[i], identity[i], 0.0001f);
}

static void test_pose_to_view_matrix_translation(void) {
    float out[16];
    bool ok = bz_quest_pose_to_view_matrix(1.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0.0f, 1.0f, out);
    ASSERT(ok);
    ASSERT_EQ_FLOAT(out[12], -1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[13], -2.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[14], -3.0f, 0.0001f);
}

static void test_pose_to_view_matrix_rotation(void) {
    /* 90-degree rotation about Y: q = (0, sin45, 0, cos45). */
    const float s = 0.70710678f;
    float out[16];
    bool ok = bz_quest_pose_to_view_matrix(0.0f, 0.0f, 0.0f, 0.0f, s, 0.0f, s, out);
    ASSERT(ok);
    ASSERT_EQ_FLOAT(out[2], 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(out[8], -1.0f, 0.001f);
    ASSERT_EQ_FLOAT(out[5], 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(out[10], 0.0f, 0.001f);
}

static void test_pose_to_view_matrix_rejects_non_unit_quaternion(void) {
    float out[16];
    memset(out, 0x7f, sizeof(out));
    ASSERT(!bz_quest_pose_to_view_matrix(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, out));
}

static void test_mat4_multiply_identity(void) {
    static const float identity[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    static const float m[16] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        9, 10, 11, 12,
        13, 14, 15, 16,
    };
    float out[16];
    bz_quest_mat4_multiply(identity, m, out);
    for (int i = 0; i < 16; i++) ASSERT_EQ_FLOAT(out[i], m[i], 0.0001f);
}

static void test_mat4_multiply_translation_then_scale(void) {
    /* scale(2,2,2) * translate(1,0,0) applied to point (0,0,0,1) should
     * yield (2,0,0,1): translate first (column-major, rightmost operand
     * applies first to a column vector), then scale. */
    static const float scale[16] = {
        2, 0, 0, 0,
        0, 2, 0, 0,
        0, 0, 2, 0,
        0, 0, 0, 1,
    };
    static const float translate[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        1, 0, 0, 1,
    };
    float combined[16];
    bz_quest_mat4_multiply(scale, translate, combined);
    /* combined * (0,0,0,1) = combined's last column. */
    ASSERT_EQ_FLOAT(combined[12], 2.0f, 0.0001f);
    ASSERT_EQ_FLOAT(combined[13], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(combined[14], 0.0f, 0.0001f);
}

static void test_quat_forward_identity(void) {
    /* Identity quaternion: forward stays -Z, unchanged. */
    float out[3];
    bz_quest_quat_forward(0.0f, 0.0f, 0.0f, 1.0f, out);
    ASSERT_EQ_FLOAT(out[0], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[1], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[2], -1.0f, 0.0001f);
}

static void test_quat_forward_yaw_90(void) {
    /* 90-degree rotation about Y: q = (0, sin45, 0, cos45). Rotating -Z by a
     * +90-degree yaw should point at -X (right-handed Y-up convention). */
    const float s = 0.70710678f;
    float out[3];
    bz_quest_quat_forward(0.0f, s, 0.0f, s, out);
    ASSERT_EQ_FLOAT(out[0], -1.0f, 0.001f);
    ASSERT_EQ_FLOAT(out[1], 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(out[2], 0.0f, 0.001f);
}

static void test_quat_forward_matches_view_matrix_rotation_column(void) {
    /* Cross-check against bz_quest_pose_to_view_matrix()'s rotation matrix
     * (see this function's header comment): for any quaternion, out must
     * equal the negation of that matrix's local-Z column, stored at indices
     * 2/6/10 (view[col*4+row] with row=2 across columns 0..2) - the same
     * rotation, two call sites, verified numerically before writing this
     * assertion (not just derived on paper). */
    const float qx = 0.18257419f, qy = 0.36514837f, qz = 0.54772256f, qw = 0.73029674f; /* unit quat */
    float viewMatrix[16];
    ASSERT(bz_quest_pose_to_view_matrix(0.0f, 0.0f, 0.0f, qx, qy, qz, qw, viewMatrix));
    float forward[3];
    bz_quest_quat_forward(qx, qy, qz, qw, forward);
    ASSERT_EQ_FLOAT(forward[0], -viewMatrix[2], 0.001f);
    ASSERT_EQ_FLOAT(forward[1], -viewMatrix[6], 0.001f);
    ASSERT_EQ_FLOAT(forward[2], -viewMatrix[10], 0.001f);
}

static void test_quat_forward_stays_unit_length(void) {
    const float qx = 0.18257419f, qy = 0.36514837f, qz = 0.54772256f, qw = 0.73029674f;
    float out[3];
    bz_quest_quat_forward(qx, qy, qz, qw, out);
    const float lenSq = out[0] * out[0] + out[1] * out[1] + out[2] * out[2];
    ASSERT_EQ_FLOAT(lenSq, 1.0f, 0.001f);
}

static void test_select_swapchain_format_prefers_order(void) {
    const int64_t runtime[] = { 99, 43, 37 }; /* order as the runtime enumerated them */
    const int64_t preferred[] = { 43, 37 };   /* our preference order */
    int64_t chosen = -1;
    bool ok = bz_quest_select_swapchain_format(runtime, 3, preferred, 2, &chosen);
    ASSERT(ok);
    ASSERT_EQ_INT(chosen, 43); /* first preferred match, not runtime[0] */
}

static void test_select_swapchain_format_none_supported(void) {
    const int64_t runtime[] = { 99, 100 };
    const int64_t preferred[] = { 43, 37 };
    int64_t chosen = 12345;
    bool ok = bz_quest_select_swapchain_format(runtime, 2, preferred, 2, &chosen);
    ASSERT(!ok);
    ASSERT_EQ_INT(chosen, 12345); /* left untouched on failure */
}

static void test_check_required_names_all_present(void) {
    const char *available[] = { "XR_KHR_vulkan_enable2", "XR_FB_passthrough", "XR_KHR_android_create_instance" };
    const char *required[] = { "XR_FB_passthrough", "XR_KHR_vulkan_enable2" };
    ASSERT(bz_quest_check_required_names(available, 3, required, 2, NULL));
}

static void test_check_required_names_reports_first_missing(void) {
    const char *available[] = { "XR_KHR_vulkan_enable2" };
    const char *required[] = { "XR_KHR_vulkan_enable2", "XR_FB_passthrough" };
    const char *missing = NULL;
    ASSERT(!bz_quest_check_required_names(available, 1, required, 2, &missing));
    ASSERT_NOT_NULL(missing);
    ASSERT_STR_EQ(missing, "XR_FB_passthrough");
}

static void test_xr_version_to_vk_api_version_normal(void) {
    /* XrVersion 1.1.0 (XR_MAKE_VERSION(1,1,0)) -> Vulkan 1.1 (VK_MAKE_API_VERSION(0,1,1,0)). */
    const uint64_t xrVersion1_1 = ((uint64_t)1 << 48) | ((uint64_t)1 << 32);
    uint32_t vkVersion = bz_quest_xr_version_to_vk_api_version(xrVersion1_1);
    ASSERT_EQ_INT(vkVersion, (1u << 22) | (1u << 12));
}

static void test_xr_version_to_vk_api_version_ignores_patch(void) {
    /* Patch bits (the low 32 bits) must not leak into the Vulkan result -
     * only major.minor describe a version *bound*. */
    const uint64_t xrVersion1_2_p99 = ((uint64_t)1 << 48) | ((uint64_t)2 << 32) | 99u;
    uint32_t vkVersion = bz_quest_xr_version_to_vk_api_version(xrVersion1_2_p99);
    ASSERT_EQ_INT(vkVersion, (1u << 22) | (2u << 12));
}

static void test_passthrough_capable(void) {
    const uint64_t colorAndReconstruction = 0x1 | 0x2;
    ASSERT(bz_quest_passthrough_capable(colorAndReconstruction, 0x1));
    ASSERT(bz_quest_passthrough_capable(colorAndReconstruction, 0x1 | 0x2));
}

static void test_passthrough_not_capable(void) {
    const uint64_t reconstructionOnly = 0x1;
    ASSERT(!bz_quest_passthrough_capable(reconstructionOnly, 0x1 | 0x2));
    ASSERT(!bz_quest_passthrough_capable(0, 0x1));
}

/* Regression test for the "projection layer fully occludes passthrough"
 * bug: without XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT set, the
 * compositor ignores the layer's alpha channel entirely and treats it as
 * fully opaque. This bit must always be present, unconditionally. */
static void test_projection_layer_flags_always_includes_blend_source_alpha(void) {
    const uint64_t kBlendTextureSourceAlphaBit = 0x00000002ULL;
    ASSERT((bz_quest_projection_layer_flags(false) & kBlendTextureSourceAlphaBit) != 0);
    ASSERT((bz_quest_projection_layer_flags(true) & kBlendTextureSourceAlphaBit) != 0);
}

/* tabletop_frag.frag writes straight (non-premultiplied) alpha - see that
 * file's and bz_quest_pure.h's comments - so the renderer must request
 * unpremultipliedAlpha=true, which must set
 * XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT. Also checks the false
 * case is NOT set, so a future accidental "always OR it in" regression
 * would fail this test too. */
static void test_projection_layer_flags_unpremultiplied_bit_matches_argument(void) {
    const uint64_t kUnpremultipliedAlphaBit = 0x00000004ULL;
    ASSERT((bz_quest_projection_layer_flags(true) & kUnpremultipliedAlphaBit) != 0);
    ASSERT((bz_quest_projection_layer_flags(false) & kUnpremultipliedAlphaBit) == 0);
}

/* Regression test for the "resumed app hangs forever before session
 * reaches RUNNING" race: xrPollEvent only runs after ALooper_pollOnce
 * returns, so the loop must poll non-blocking whenever the app is resumed
 * (and the renderer initialized successfully), not only once a session is
 * already confirmed running - see bz_quest_pure.h's comment on
 * bz_quest_looper_timeout_millis(). */
static void test_looper_timeout_polls_when_wants_polling_but_not_yet_running(void) {
    ASSERT_EQ_INT(bz_quest_looper_timeout_millis(/*wantsXrEventPolling=*/true, /*xrSessionRunning=*/false), 0);
}

static void test_looper_timeout_polls_when_running_but_not_wants_polling_flag(void) {
    /* Defensive: a running session must keep polling even if some caller
     * ever forgot to flip the wantsXrEventPolling flag back - session-
     * running frames must never stall regardless of that bookkeeping. */
    ASSERT_EQ_INT(bz_quest_looper_timeout_millis(/*wantsXrEventPolling=*/false, /*xrSessionRunning=*/true), 0);
}

static void test_looper_timeout_polls_when_both_true(void) {
    ASSERT_EQ_INT(bz_quest_looper_timeout_millis(/*wantsXrEventPolling=*/true, /*xrSessionRunning=*/true), 0);
}

static void test_looper_timeout_blocks_when_fully_backgrounded(void) {
    ASSERT_EQ_INT(bz_quest_looper_timeout_millis(/*wantsXrEventPolling=*/false, /*xrSessionRunning=*/false), -1);
}

void run_bz_quest_pure_tests(void) {
    RUN_TEST(test_fov_projection_normal);
    RUN_TEST(test_fov_projection_asymmetric);
    RUN_TEST(test_fov_projection_rejects_invalid_z);
    RUN_TEST(test_fov_projection_rejects_degenerate_angles);
    RUN_TEST(test_pose_to_view_matrix_identity);
    RUN_TEST(test_pose_to_view_matrix_translation);
    RUN_TEST(test_pose_to_view_matrix_rotation);
    RUN_TEST(test_pose_to_view_matrix_rejects_non_unit_quaternion);
    RUN_TEST(test_mat4_multiply_identity);
    RUN_TEST(test_mat4_multiply_translation_then_scale);
    RUN_TEST(test_quat_forward_identity);
    RUN_TEST(test_quat_forward_yaw_90);
    RUN_TEST(test_quat_forward_matches_view_matrix_rotation_column);
    RUN_TEST(test_quat_forward_stays_unit_length);
    RUN_TEST(test_select_swapchain_format_prefers_order);
    RUN_TEST(test_select_swapchain_format_none_supported);
    RUN_TEST(test_check_required_names_all_present);
    RUN_TEST(test_check_required_names_reports_first_missing);
    RUN_TEST(test_passthrough_capable);
    RUN_TEST(test_passthrough_not_capable);
    RUN_TEST(test_xr_version_to_vk_api_version_normal);
    RUN_TEST(test_xr_version_to_vk_api_version_ignores_patch);
    RUN_TEST(test_projection_layer_flags_always_includes_blend_source_alpha);
    RUN_TEST(test_projection_layer_flags_unpremultiplied_bit_matches_argument);
    RUN_TEST(test_looper_timeout_polls_when_wants_polling_but_not_yet_running);
    RUN_TEST(test_looper_timeout_polls_when_running_but_not_wants_polling_flag);
    RUN_TEST(test_looper_timeout_polls_when_both_true);
    RUN_TEST(test_looper_timeout_blocks_when_fully_backgrounded);
}
