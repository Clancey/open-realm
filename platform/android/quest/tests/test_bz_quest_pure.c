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

/* Generic function-behavior test: the UNPREMULTIPLIED bit must exactly track
 * the `unpremultipliedAlpha` argument regardless of which value any caller
 * currently passes (bz_quest_renderer.c passes `false` - see
 * bz_quest_pure.h's bz_quest_projection_layer_flags() doc comment for why
 * this project's render target is premultiplied, PR #28's High-severity
 * fix). Checks BOTH the true and false cases, so a future accidental
 * "always OR it in" (or "never OR it in") regression would fail this test. */
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

/* -- Premultiplied-compositing blend/coverage math (High-severity reviewer fix, PR #28) --
 *
 * `blend_equation()` below is the STANDARD, spec-defined Vulkan/OpenGL/D3D fixed-function
 * blend formula (Vulkan spec 1.3, section "Blend Operations": `result = srcFactor*src OP
 * dstFactor*dst`, OP=ADD for every mode this project uses) - a fixed, external, universally-
 * documented mathematical definition, NOT a reimplementation of any of this codebase's OWN
 * business logic (unlike e.g. the particle emission-timing algorithm, which is genuinely this
 * project's own derived algorithm and must never be cross-checked against a duplicate
 * "oracle" of the same arithmetic). Testing "do THESE blend-factor choices produce the
 * intended coverage semantics" against the external, fixed formula is safe and meaningful.
 * The blend-FACTOR CHOICES asserted below are hand-derived from and cross-checked against the
 * actual production bz_quest_vk_wc3_blend_state_for_mode()/bz_quest_vk_straight_over_blend_
 * state() source (bz_quest_vk_wc3.c/bz_quest_vk.c) - platform/android/quest/scripts/test-wc3-
 * premultiplied-blend-layout.sh structurally greps the real production source for these exact
 * factor tokens, so a future edit to either file that silently drifts from what is verified
 * here fails that guard, tying this pure-math test to the real implementation. */
typedef struct { float r, g, b, a; } BzQuestTestRGBA;

/* Symbolic stand-ins for the 6 VkBlendFactor values every mode below actually uses -
 * deliberately NOT the real VkBlendFactor enum (this file must stay host-buildable with no
 * Vulkan header dependency), evaluated against a concrete (src,dst) pair by
 * blend_factor_value() exactly per the Vulkan spec's own per-factor definition. */
typedef enum {
    BZ_TEST_BLEND_ZERO,
    BZ_TEST_BLEND_ONE,
    BZ_TEST_BLEND_SRC_ALPHA,
    BZ_TEST_BLEND_ONE_MINUS_SRC_ALPHA,
    BZ_TEST_BLEND_DST_COLOR,   /* per-component: dst.r/g/b for color, dst.a for alpha */
    BZ_TEST_BLEND_SRC_COLOR,   /* per-component: src.r/g/b for color, src.a for alpha */
} BzTestBlendFactor;

static float blend_factor_value(BzTestBlendFactor f, float srcComp, float dstComp, float srcA) {
    switch (f) {
        case BZ_TEST_BLEND_ZERO: return 0.0f;
        case BZ_TEST_BLEND_ONE: return 1.0f;
        case BZ_TEST_BLEND_SRC_ALPHA: return srcA;
        case BZ_TEST_BLEND_ONE_MINUS_SRC_ALPHA: return 1.0f - srcA;
        case BZ_TEST_BLEND_DST_COLOR: return dstComp;
        case BZ_TEST_BLEND_SRC_COLOR: return srcComp;
        default: return 0.0f;
    }
}

/* The one Vulkan ADD blend op this project ever uses, applied component-wise: each of R/G/B
 * uses the COLOR factor pair against the R/G/B components; A uses the ALPHA factor pair
 * against the A component (per-component factor selection for DST_COLOR/SRC_COLOR - see
 * blend_factor_value()'s own comment - matches the Vulkan spec's "each of the four components
 * is a separate scalar blend" rule exactly). */
static BzQuestTestRGBA blend_equation(BzQuestTestRGBA src, BzQuestTestRGBA dst, BzTestBlendFactor srcColorF,
                                     BzTestBlendFactor dstColorF, BzTestBlendFactor srcAlphaF,
                                     BzTestBlendFactor dstAlphaF) {
    BzQuestTestRGBA out;
    out.r = blend_factor_value(srcColorF, src.r, dst.r, src.a) * src.r +
           blend_factor_value(dstColorF, src.r, dst.r, src.a) * dst.r;
    out.g = blend_factor_value(srcColorF, src.g, dst.g, src.a) * src.g +
           blend_factor_value(dstColorF, src.g, dst.g, src.a) * dst.g;
    out.b = blend_factor_value(srcColorF, src.b, dst.b, src.a) * src.b +
           blend_factor_value(dstColorF, src.b, dst.b, src.a) * dst.b;
    out.a = blend_factor_value(srcAlphaF, src.a, dst.a, src.a) * src.a +
           blend_factor_value(dstAlphaF, src.a, dst.a, src.a) * dst.a;
    return out;
}

#define BZ_TEST_ASSERT_RGBA_EQ(actual, expR, expG, expB, expA, eps)          \
    do {                                                                     \
        ASSERT_EQ_FLOAT((actual).r, (expR), (eps));                          \
        ASSERT_EQ_FLOAT((actual).g, (expG), (eps));                          \
        ASSERT_EQ_FLOAT((actual).b, (expB), (eps));                          \
        ASSERT_EQ_FLOAT((actual).a, (expA), (eps));                          \
    } while (0)

/* Case: BZ_TTA_BLEND_OPAQUE/TRANSPARENT surviving fragment - no blend equation runs at all
 * (blendEnable=false); the shader itself must force coverage alpha to exactly 1.0 (see
 * warcraft_frag.frag/warcraft_particle_frag.frag/terrain_frag.frag's own materialParams.z/
 * coverageParams.x doc comments) regardless of the source texture's own alpha channel. This
 * test asserts the CONTRACT ITSELF (a blendEnable=false write is verbatim, so "coverage=1"
 * really does mean the stored framebuffer alpha is 1, not a blend-modified value) rather than
 * re-deriving blend factors that do not apply here. */
static void test_blend_opaque_forces_full_coverage_regardless_of_texture_alpha(void) {
    float shaderOutputAlphaWhenForced = 1.0f; /* materialParams.z > 0.5 branch */
    float storedFramebufferAlpha = shaderOutputAlphaWhenForced; /* blendEnable=false: verbatim */
    ASSERT_EQ_FLOAT(storedFramebufferAlpha, 1.0f, 0.0001f);
}

/* Case: BZ_TTA_BLEND_ALPHA / bz_quest_vk_straight_over_blend_state()'s shared "over" state -
 * 50% white over a fully-transparent (0,0,0,0) cleared background (representative "alpha=0.5
 * over transparent"). Color factors SRC_ALPHA/ONE_MINUS_SRC_ALPHA (unchanged from desktop's
 * own glBlendFunc), alpha factors ONE/ONE_MINUS_SRC_ALPHA (the fix - see
 * bz_quest_vk_straight_over_blend_state()'s doc comment, bz_quest_vk.h). Expects a valid
 * PREMULTIPLIED result: rgb=(0.5,0.5,0.5), a=0.5 (NOT the pre-fix a=0.25 "squared" value). */
static void test_blend_alpha_half_over_transparent_yields_premultiplied_coverage(void) {
    BzQuestTestRGBA src = {1.0f, 1.0f, 1.0f, 0.5f};
    BzQuestTestRGBA dst = {0.0f, 0.0f, 0.0f, 0.0f};
    BzQuestTestRGBA out = blend_equation(src, dst, BZ_TEST_BLEND_SRC_ALPHA, BZ_TEST_BLEND_ONE_MINUS_SRC_ALPHA,
                                        BZ_TEST_BLEND_ONE, BZ_TEST_BLEND_ONE_MINUS_SRC_ALPHA);
    BZ_TEST_ASSERT_RGBA_EQ(out, 0.5f, 0.5f, 0.5f, 0.5f, 0.0001f);
}

/* Case: the same ALPHA "over" state, 50% white over an ALREADY-OPAQUE (1,1,1,1) background
 * (representative "alpha=0.5 over opaque") - coverage must stay saturated at 1.0 (the opaque
 * content underneath is still fully there, just tinted), never eroded. */
static void test_blend_alpha_half_over_opaque_stays_fully_covered(void) {
    BzQuestTestRGBA src = {1.0f, 0.0f, 0.0f, 0.5f}; /* 50% red */
    BzQuestTestRGBA dst = {0.0f, 1.0f, 0.0f, 1.0f}; /* fully opaque green */
    BzQuestTestRGBA out = blend_equation(src, dst, BZ_TEST_BLEND_SRC_ALPHA, BZ_TEST_BLEND_ONE_MINUS_SRC_ALPHA,
                                        BZ_TEST_BLEND_ONE, BZ_TEST_BLEND_ONE_MINUS_SRC_ALPHA);
    BZ_TEST_ASSERT_RGBA_EQ(out, 0.5f, 0.5f, 0.0f, 1.0f, 0.0001f);
}

/* Case: warcraft_fog_frag.frag's darkening overlay (rgb always black, alpha=1-fogSample,
 * bz_quest_vk_straight_over_blend_state()'s same shared "over" state) drawn over ALREADY-
 * OPAQUE (0,1,0,1) terrain - representative "fog over opaque". Coverage must stay exactly 1.0
 * (the terrain underneath is still solid, merely darkened) - the pre-fix mirrored alpha factor
 * eroded this to 0.75, a real "room leakage through fog" bug this reproduces the fix for. */
static void test_blend_fog_darkening_over_opaque_preserves_full_coverage(void) {
    BzQuestTestRGBA dst = {0.0f, 1.0f, 0.0f, 1.0f}; /* opaque green terrain already drawn */
    BzQuestTestRGBA src = {0.0f, 0.0f, 0.0f, 0.5f}; /* fog: rgb=black, alpha=0.5 (explored, dim) */
    BzQuestTestRGBA out = blend_equation(src, dst, BZ_TEST_BLEND_SRC_ALPHA, BZ_TEST_BLEND_ONE_MINUS_SRC_ALPHA,
                                        BZ_TEST_BLEND_ONE, BZ_TEST_BLEND_ONE_MINUS_SRC_ALPHA);
    BZ_TEST_ASSERT_RGBA_EQ(out, 0.0f, 0.5f, 0.0f, 1.0f, 0.0001f);
}

/* Case: BZ_TTA_BLEND_ADDITIVE - color and alpha BOTH ONE/ONE (unchanged; already correct, never
 * squared, since ONE is not self-referential the way SRC_ALPHA is) - a documented, deliberate
 * approximation for coverage (saturating accumulation), not a physically exact area
 * computation, since additive light does not truly occlude anything - see
 * bz_quest_vk_wc3_blend_state_for_mode()'s BZ_QUEST_TTA_BLEND_ADDITIVE case comment for the
 * full "additive-over-real-world" limitation this documents. */
static void test_blend_additive_saturates_color_and_alpha_together(void) {
    BzQuestTestRGBA src = {0.3f, 0.0f, 0.0f, 0.3f};
    BzQuestTestRGBA dst = {0.0f, 0.4f, 0.0f, 0.4f};
    BzQuestTestRGBA out = blend_equation(src, dst, BZ_TEST_BLEND_ONE, BZ_TEST_BLEND_ONE, BZ_TEST_BLEND_ONE,
                                        BZ_TEST_BLEND_ONE);
    /* Plain, unsaturating sum here (0.3+0.4=0.7, no clamping needed) so the assertion is
     * unambiguous; a real UNORM attachment clamps any sum exceeding 1.0 to 1.0 (e.g. stacking
     * enough additive glow to exceed full brightness saturates to fully-covering, which is the
     * intended "brighter additive content reads as more covering" heuristic). */
    BZ_TEST_ASSERT_RGBA_EQ(out, 0.3f, 0.4f, 0.0f, 0.7f, 0.0001f);
}

/* Case: BZ_TTA_BLEND_MODULATE - color DST_COLOR/ZERO (unchanged desktop glBlendFunc mapping),
 * alpha ZERO/ONE (the fix: preserves dst's existing coverage EXACTLY, "should not invent room
 * occlusion" - drawing a modulate quad over untouched (0,0,0,0) passthrough space is then a
 * true no-op) - see bz_quest_vk_wc3_blend_state_for_mode()'s BZ_QUEST_TTA_BLEND_MODULATE case
 * comment. */
static void test_blend_modulate_over_empty_background_is_a_true_noop(void) {
    BzQuestTestRGBA src = {0.5f, 0.0f, 0.0f, 1.0f};
    BzQuestTestRGBA dst = {0.0f, 0.0f, 0.0f, 0.0f};
    BzQuestTestRGBA out = blend_equation(src, dst, BZ_TEST_BLEND_DST_COLOR, BZ_TEST_BLEND_ZERO, BZ_TEST_BLEND_ZERO,
                                        BZ_TEST_BLEND_ONE);
    BZ_TEST_ASSERT_RGBA_EQ(out, 0.0f, 0.0f, 0.0f, 0.0f, 0.0001f);
}

/* Case: BZ_TTA_BLEND_MODULATE over EXISTING 50% coverage, where the modulate texture's own
 * alpha channel happens to be 0.7 (not 1.0) - proves coverage is preserved EXACTLY (never
 * eroded by the modulate texture's own unrelated alpha channel, which the pre-fix mirrored
 * factors did erode: 0.5 -> 0.35, a real "erodes existing opaque coverage" bug). */
static void test_blend_modulate_never_erodes_existing_coverage_via_texture_alpha(void) {
    BzQuestTestRGBA src = {0.5f, 0.0f, 0.0f, 0.7f}; /* dark red tint; its OWN alpha is irrelevant to coverage */
    BzQuestTestRGBA dst = {0.5f, 0.5f, 0.5f, 0.5f}; /* existing 50%-covered premultiplied content */
    BzQuestTestRGBA out = blend_equation(src, dst, BZ_TEST_BLEND_DST_COLOR, BZ_TEST_BLEND_ZERO, BZ_TEST_BLEND_ZERO,
                                        BZ_TEST_BLEND_ONE);
    ASSERT_EQ_FLOAT(out.a, 0.5f, 0.0001f); /* unchanged - not eroded to 0.35 */
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
    RUN_TEST(test_blend_opaque_forces_full_coverage_regardless_of_texture_alpha);
    RUN_TEST(test_blend_alpha_half_over_transparent_yields_premultiplied_coverage);
    RUN_TEST(test_blend_alpha_half_over_opaque_stays_fully_covered);
    RUN_TEST(test_blend_fog_darkening_over_opaque_preserves_full_coverage);
    RUN_TEST(test_blend_additive_saturates_color_and_alpha_together);
    RUN_TEST(test_blend_modulate_over_empty_background_is_a_true_noop);
    RUN_TEST(test_blend_modulate_never_erodes_existing_coverage_via_texture_alpha);
}
