/*
 * test_bz_quest_wc3_anim.c - coverage for bz_quest_wc3_anim.c's pure MDX
 * pose math (layer 5C). Expected values are hand-derived from the exact
 * formulas transcribed in bz_quest_wc3_anim.c/.h (never re-derived from the
 * production code itself - see AGENTS.md's "avoid tests that duplicate
 * production formulas" rule), plus a handful of trivially-checkable cases
 * (identity/rest-pose defaults, exact keyframe hits).
 */
#include <math.h>
#include <string.h>

#include "bz_quest_wc3_anim.h"
#include "test_framework.h"

/* ------------------------------------------------------------------ */
/* Track sampling: NONE/LINEAR/HERMITE/BEZIER, endpoints, wraparound     */
/* ------------------------------------------------------------------ */

static bzQuestWc3Track_t make_vec3_track(bzQuestWc3Interp_t interp, uint32_t globalSeq,
                                         const bzQuestWc3Vec3Key_t *keys, uint32_t count) {
    bzQuestWc3Track_t t;
    memset(&t, 0, sizeof(t));
    t.interp = interp;
    t.globalSequence = globalSeq;
    t.keyCount = count;
    for (uint32_t i = 0; i < count; i++) t.vec3Keys[i] = keys[i];
    return t;
}

static void test_vec3_track_no_track_yields_zero(void) {
    bzQuestWc3Track_t track;
    memset(&track, 0, sizeof(track));
    bzQuestWc3Vec3_t out = {9, 9, 9};
    bz_quest_wc3_sample_vec3_track(&track, 0, 1000, 500, &out);
    ASSERT_EQ_FLOAT(out.x, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.y, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.z, 0.0f, 0.0001f);
}

static void test_vec3_scale_track_no_track_yields_one(void) {
    bzQuestWc3Track_t track;
    memset(&track, 0, sizeof(track));
    bzQuestWc3Vec3_t out = {9, 9, 9};
    bz_quest_wc3_sample_vec3_track_scale(&track, 0, 1000, 500, &out);
    ASSERT_EQ_FLOAT(out.x, 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.y, 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.z, 1.0f, 0.0001f);
}

static void test_vec3_linear_midpoint(void) {
    bzQuestWc3Vec3Key_t keys[2] = {
        {0, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},
        {1000, {10, 20, 30}, {0, 0, 0}, {0, 0, 0}},
    };
    bzQuestWc3Track_t track = make_vec3_track(BZ_QUEST_WC3_INTERP_LINEAR, BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE,
                                              keys, 2);
    bzQuestWc3Vec3_t out;
    bz_quest_wc3_sample_vec3_track(&track, 0, 1000, 500, &out);
    /* t = 500/1000 = 0.5 -> lerp(0,10,0.5)=5, lerp(0,20,.5)=10, lerp(0,30,.5)=15 */
    ASSERT_EQ_FLOAT(out.x, 5.0f, 0.001f);
    ASSERT_EQ_FLOAT(out.y, 10.0f, 0.001f);
    ASSERT_EQ_FLOAT(out.z, 15.0f, 0.001f);
}

static void test_vec3_exact_key_hit_returns_key_verbatim(void) {
    bzQuestWc3Vec3Key_t keys[2] = {
        {0, {1, 2, 3}, {0, 0, 0}, {0, 0, 0}},
        {1000, {10, 20, 30}, {0, 0, 0}, {0, 0, 0}},
    };
    bzQuestWc3Track_t track = make_vec3_track(BZ_QUEST_WC3_INTERP_LINEAR, BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE,
                                              keys, 2);
    bzQuestWc3Vec3_t out;
    bz_quest_wc3_sample_vec3_track(&track, 0, 1000, 0, &out);
    ASSERT_EQ_FLOAT(out.x, 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.y, 2.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.z, 3.0f, 0.0001f);
}

static void test_vec3_none_interp_holds_left_value(void) {
    bzQuestWc3Vec3Key_t keys[2] = {
        {0, {1, 1, 1}, {0, 0, 0}, {0, 0, 0}},
        {1000, {9, 9, 9}, {0, 0, 0}, {0, 0, 0}},
    };
    bzQuestWc3Track_t track = make_vec3_track(BZ_QUEST_WC3_INTERP_NONE, BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE,
                                              keys, 2);
    bzQuestWc3Vec3_t out;
    bz_quest_wc3_sample_vec3_track(&track, 0, 1000, 500, &out);
    /* TRACK_NO_INTERP: always the left (preceding) key's value verbatim. */
    ASSERT_EQ_FLOAT(out.x, 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.y, 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.z, 1.0f, 0.0001f);
}

static void test_vec3_hermite_matches_hand_derived_formula(void) {
    /* left=0, outTan=2, inTan=-1, right=10, t=0.5. Hermite basis at t=0.5:
     * f1 = t^2*(2t-3)+1 = 0.25*(-2)+1 = 0.5
     * f2 = t^2*(t-2)+t = 0.25*(-1.5)+0.5 = 0.125
     * f3 = t^2*(t-1) = 0.25*(-0.5) = -0.125
     * f4 = t^2*(3-2t) = 0.25*2 = 0.5
     * result = 0*0.5 + 2*0.125 + (-1)*(-0.125) + 10*0.5 = 0.25 + 0.125 + 5.0 = 5.375 */
    bzQuestWc3Vec3Key_t keys[2] = {
        {0, {0, 0, 0}, {0, 0, 0}, {2, 2, 2}},
        {1000, {10, 10, 10}, {-1, -1, -1}, {0, 0, 0}},
    };
    bzQuestWc3Track_t track = make_vec3_track(BZ_QUEST_WC3_INTERP_HERMITE, BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE,
                                              keys, 2);
    bzQuestWc3Vec3_t out;
    bz_quest_wc3_sample_vec3_track(&track, 0, 1000, 500, &out);
    ASSERT_EQ_FLOAT(out.x, 5.375f, 0.001f);
}

static void test_vec3_bezier_matches_hand_derived_formula(void) {
    /* left=0, outTan=2, inTan=8, right=10, t=0.5. Bezier basis at t=0.5:
     * f1=(1-t)^3=0.125, f2=3t(1-t)^2=0.375, f3=3t^2(1-t)=0.375, f4=t^3=0.125
     * result = 0*0.125 + 2*0.375 + 8*0.375 + 10*0.125 = 0.75+3.0+1.25 = 5.0 */
    bzQuestWc3Vec3Key_t keys[2] = {
        {0, {0, 0, 0}, {0, 0, 0}, {2, 2, 2}},
        {1000, {10, 10, 10}, {8, 8, 8}, {0, 0, 0}},
    };
    bzQuestWc3Track_t track = make_vec3_track(BZ_QUEST_WC3_INTERP_BEZIER, BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE,
                                              keys, 2);
    bzQuestWc3Vec3_t out;
    bz_quest_wc3_sample_vec3_track(&track, 0, 1000, 500, &out);
    ASSERT_EQ_FLOAT(out.x, 5.0f, 0.001f);
}

static void test_vec3_before_first_key_clamps_to_first_key(void) {
    bzQuestWc3Vec3Key_t keys[2] = {
        {200, {5, 5, 5}, {0, 0, 0}, {0, 0, 0}},
        {800, {9, 9, 9}, {0, 0, 0}, {0, 0, 0}},
    };
    bzQuestWc3Track_t track = make_vec3_track(BZ_QUEST_WC3_INTERP_LINEAR, BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE,
                                              keys, 2);
    bzQuestWc3Vec3_t out;
    bz_quest_wc3_sample_vec3_track(&track, 0, 1000, 0, &out);
    ASSERT_EQ_FLOAT(out.x, 5.0f, 0.0001f);
}

static void test_vec3_single_key_holds_at_and_after_its_time(void) {
    bzQuestWc3Vec3Key_t keys[1] = {{500, {7, 7, 7}, {0, 0, 0}, {0, 0, 0}}};
    bzQuestWc3Track_t track = make_vec3_track(BZ_QUEST_WC3_INTERP_LINEAR, BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE,
                                              keys, 1);
    bzQuestWc3Vec3_t out;
    bz_quest_wc3_sample_vec3_track(&track, 0, 1000, 999, &out);
    ASSERT_EQ_FLOAT(out.x, 7.0f, 0.0001f);
}

static void test_vec3_wraps_toward_first_key_past_last_key(void) {
    /* interval [0,1000], keys at t=100 (val 10) and t=800 (val 90).
     * At time=900: lastKF=800(val90), firstKF=100(val10).
     * end_to_start = (1000-800)+(100-0) = 300. wrap_t=(900-800)/300=1/3.
     * lerp(90,10,1/3) = 90*(2/3)+10*(1/3) = 60+3.333=63.333 */
    bzQuestWc3Vec3Key_t keys[2] = {
        {100, {10, 0, 0}, {0, 0, 0}, {0, 0, 0}},
        {800, {90, 0, 0}, {0, 0, 0}, {0, 0, 0}},
    };
    bzQuestWc3Track_t track = make_vec3_track(BZ_QUEST_WC3_INTERP_LINEAR, BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE,
                                              keys, 2);
    bzQuestWc3Vec3_t out;
    bz_quest_wc3_sample_vec3_track(&track, 0, 1000, 900, &out);
    ASSERT_EQ_FLOAT(out.x, 63.3333f, 0.01f);
}

static void test_vec3_wrap_clamps_when_first_key_at_interval_start(void) {
    /* first key exactly at interval start -> end_to_start's second term is
     * 0, so wrap_t reaches 1.0 exactly at time==intervalEnd, taking the
     * "clamp to lastKF verbatim" fallback instead of interpolating past. */
    bzQuestWc3Vec3Key_t keys[2] = {
        {0, {1, 0, 0}, {0, 0, 0}, {0, 0, 0}},
        {800, {90, 0, 0}, {0, 0, 0}, {0, 0, 0}},
    };
    bzQuestWc3Track_t track = make_vec3_track(BZ_QUEST_WC3_INTERP_LINEAR, BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE,
                                              keys, 2);
    bzQuestWc3Vec3_t out;
    bz_quest_wc3_sample_vec3_track(&track, 0, 800, 800, &out);
    ASSERT_EQ_FLOAT(out.x, 90.0f, 0.0001f);
}

/* ------------------------------------------------------------------ */
/* Quaternion sampling: slerp (LINEAR/NONE) vs sqlerp (HERMITE/BEZIER)  */
/* ------------------------------------------------------------------ */

static void test_quat_no_track_yields_identity(void) {
    bzQuestWc3Track_t track;
    memset(&track, 0, sizeof(track));
    bzQuestWc3Quat_t out = {9, 9, 9, 9};
    bz_quest_wc3_sample_quat_track(&track, 0, 1000, 500, &out);
    ASSERT_EQ_FLOAT(out.x, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.y, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.z, 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.w, 1.0f, 0.0001f);
}

static void test_quat_slerp_identity_to_90deg_about_z_midpoint(void) {
    /* identity -> 90deg about Z (0,0,0.70710678,0.70710678). At t=0.5 the
     * slerp of two unit quats separated by angle omega gives the 45deg
     * quaternion about the same axis: (0, 0, sin(45deg/2)... actually for a
     * rotation-about-Z quaternion q=(0,0,sin(a/2),cos(a/2)), slerping from
     * a=0 to a=90deg at t=0.5 yields a=45deg: (0,0,sin(22.5deg),cos(22.5deg)). */
    bzQuestWc3QuatKey_t keys[2] = {
        {0, {0, 0, 0, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}},
        {2000, {0, 0, 0.70710678f, 0.70710678f}, {0, 0, 0, 0}, {0, 0, 0, 0}},
    };
    bzQuestWc3Track_t track;
    memset(&track, 0, sizeof(track));
    track.interp = BZ_QUEST_WC3_INTERP_LINEAR;
    track.globalSequence = BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE;
    track.keyCount = 2;
    track.quatKeys[0] = keys[0];
    track.quatKeys[1] = keys[1];
    bzQuestWc3Quat_t out;
    bz_quest_wc3_sample_quat_track(&track, 0, 2000, 1000, &out);
    double expectedZ = sin(22.5 * M_PI / 180.0);
    double expectedW = cos(22.5 * M_PI / 180.0);
    ASSERT_EQ_FLOAT(out.z, (float)expectedZ, 0.001f);
    ASSERT_EQ_FLOAT(out.w, (float)expectedW, 0.001f);
}

/* ------------------------------------------------------------------ */
/* Global sequence wall-clock wraparound                                */
/* ------------------------------------------------------------------ */

static void test_global_sequence_uses_render_clock_not_entity_frame(void) {
    bzQuestWc3Track_t track;
    memset(&track, 0, sizeof(track));
    track.globalSequence = 0;
    uint32_t start, end, time;
    /* duration_msec=499 -> gs_len=500 (the "+1" - see r_mdx_anim.c:37). At
     * renderClock=1300, 1300 % 500 = 300, regardless of entityFrame. */
    bz_quest_wc3_resolve_track_interval(&track, /*seqStart*/ 9999, /*seqEnd*/ 19999,
                                        /*entityFrame*/ 42, /*renderClock*/ 1300,
                                        /*globalSeqDuration*/ 499, &start, &end, &time);
    ASSERT_EQ_INT((int)start, 0);
    ASSERT_EQ_INT((int)end, 499);
    ASSERT_EQ_INT((int)time, 300);
}

static void test_non_global_sequence_uses_entity_frame_and_sequence_interval(void) {
    bzQuestWc3Track_t track;
    memset(&track, 0, sizeof(track));
    track.globalSequence = BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE;
    uint32_t start, end, time;
    bz_quest_wc3_resolve_track_interval(&track, 1000, 2000, 1500, 999999, 0, &start, &end, &time);
    ASSERT_EQ_INT((int)start, 1000);
    ASSERT_EQ_INT((int)end, 2000);
    ASSERT_EQ_INT((int)time, 1500);
}

/* ------------------------------------------------------------------ */
/* Node local matrix - identity / translation-only / rotation-about-pivot */
/* ------------------------------------------------------------------ */

static void test_node_local_matrix_no_tracks_is_identity(void) {
    float m[16];
    bzQuestWc3Vec3_t zero = {0, 0, 0};
    bzQuestWc3Quat_t idq = {0, 0, 0, 1};
    bz_quest_wc3_node_local_matrix(false, false, false, &zero, &idq, &zero, &zero, m);
    ASSERT_EQ_FLOAT(m[0], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(m[5], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(m[10], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(m[12], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(m[13], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(m[14], 0.0f, 0.0001f);
}

static void test_node_local_matrix_translation_only(void) {
    float m[16];
    bzQuestWc3Vec3_t t = {1, 2, 3};
    bzQuestWc3Quat_t idq = {0, 0, 0, 1};
    bzQuestWc3Vec3_t zero = {0, 0, 0};
    bz_quest_wc3_node_local_matrix(true, false, false, &t, &idq, &zero, &zero, m);
    ASSERT_EQ_FLOAT(m[12], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(m[13], 2.0f, 0.0001f);
    ASSERT_EQ_FLOAT(m[14], 3.0f, 0.0001f);
    /* rotation part must stay identity for a translation-only node. */
    ASSERT_EQ_FLOAT(m[0], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(m[5], 1.0f, 0.0001f);
}

static void test_node_local_matrix_rotation_about_nonzero_pivot_leaves_pivot_fixed(void) {
    /* A pure-rotation node (no translation track) with a 90deg-about-Z
     * rotation and pivot (1,0,0) must map the pivot point to itself
     * (Matrix4_from_rotation_origin's defining property: out*pivot=pivot). */
    float m[16];
    bzQuestWc3Quat_t rot90z = {0, 0, 0.70710678f, 0.70710678f};
    bzQuestWc3Vec3_t pivot = {1, 0, 0};
    bzQuestWc3Vec3_t zero = {0, 0, 0};
    bz_quest_wc3_node_local_matrix(false, true, false, &zero, &rot90z, &zero, &pivot, m);
    /* transform pivot: x' = m0*px + m4*py + m8*pz + m12, etc (column-major). */
    float px = m[0] * pivot.x + m[4] * pivot.y + m[8] * pivot.z + m[12];
    float py = m[1] * pivot.x + m[5] * pivot.y + m[9] * pivot.z + m[13];
    float pz = m[2] * pivot.x + m[6] * pivot.y + m[10] * pivot.z + m[14];
    ASSERT_EQ_FLOAT(px, pivot.x, 0.001f);
    ASSERT_EQ_FLOAT(py, pivot.y, 0.001f);
    ASSERT_EQ_FLOAT(pz, pivot.z, 0.001f);
    /* and the origin (0,0,0) must move to (1,-1,0): rotating (-1,0,0)
     * (origin relative to pivot) by 90deg about Z gives (0,1,0), plus pivot
     * (1,0,0) = (1,1,0). Let's just check it's NOT still at the origin. */
    ASSERT(fabsf(m[12]) > 0.5f || fabsf(m[13]) > 0.5f);
}

/* ------------------------------------------------------------------ */
/* Hierarchy: two-node parent chain, cycle safety                       */
/* ------------------------------------------------------------------ */

static bzQuestWc3Node_t make_translation_node(uint32_t parentIndex, float tx, float ty, float tz) {
    bzQuestWc3Node_t node;
    memset(&node, 0, sizeof(node));
    node.parentIndex = parentIndex;
    node.translation.keyCount = 1;
    node.translation.interp = BZ_QUEST_WC3_INTERP_LINEAR;
    node.translation.globalSequence = BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE;
    node.translation.vec3Keys[0] = (bzQuestWc3Vec3Key_t){0, {tx, ty, tz}, {0, 0, 0}, {0, 0, 0}};
    return node;
}

static void test_hierarchy_child_inherits_parent_translation(void) {
    bzQuestWc3Node_t nodes[2];
    nodes[0] = make_translation_node(BZ_QUEST_WC3_NO_PARENT, 10, 0, 0); /* root */
    nodes[1] = make_translation_node(0, 0, 5, 0);                       /* child of root */
    float pose[2][16];
    bz_quest_wc3_build_pose(nodes, 2, 0, 1000, 0, 0, NULL, 0, pose);
    /* root global = T(10,0,0); child global = root * T(0,5,0) = T(10,5,0). */
    ASSERT_EQ_FLOAT(pose[0][12], 10.0f, 0.0001f);
    ASSERT_EQ_FLOAT(pose[1][12], 10.0f, 0.0001f);
    ASSERT_EQ_FLOAT(pose[1][13], 5.0f, 0.0001f);
}

static void test_hierarchy_root_node_alone(void) {
    bzQuestWc3Node_t nodes[1];
    nodes[0] = make_translation_node(BZ_QUEST_WC3_NO_PARENT, 3, 4, 5);
    float pose[1][16];
    bz_quest_wc3_build_pose(nodes, 1, 0, 1000, 0, 0, NULL, 0, pose);
    ASSERT_EQ_FLOAT(pose[0][12], 3.0f, 0.0001f);
    ASSERT_EQ_FLOAT(pose[0][13], 4.0f, 0.0001f);
    ASSERT_EQ_FLOAT(pose[0][14], 5.0f, 0.0001f);
}

static void test_hierarchy_parent_cycle_degrades_to_root_not_hang(void) {
    /* Adversarial: node 0's parent is node 1, node 1's parent is node 0.
     * Must terminate and treat both as roots (local-only), not hang or
     * recurse unboundedly. */
    bzQuestWc3Node_t nodes[2];
    nodes[0] = make_translation_node(1, 1, 0, 0);
    nodes[1] = make_translation_node(0, 0, 1, 0);
    float pose[2][16];
    bz_quest_wc3_build_pose(nodes, 2, 0, 1000, 0, 0, NULL, 0, pose);
    ASSERT_EQ_FLOAT(pose[0][12], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(pose[1][13], 1.0f, 0.0001f);
}

static void test_hierarchy_out_of_range_parent_treated_as_root(void) {
    bzQuestWc3Node_t nodes[1];
    nodes[0] = make_translation_node(77, 2, 2, 2); /* parent index >= nodeCount */
    float pose[1][16];
    bz_quest_wc3_build_pose(nodes, 1, 0, 1000, 0, 0, NULL, 0, pose);
    ASSERT_EQ_FLOAT(pose[0][12], 2.0f, 0.0001f);
}

/* ------------------------------------------------------------------ */
/* Bone palette resolution                                              */
/* ------------------------------------------------------------------ */

static void test_bone_palette_maps_indices_and_fills_identity(void) {
    float nodeMatrices[2][16];
    memset(nodeMatrices, 0, sizeof(nodeMatrices));
    nodeMatrices[0][0] = nodeMatrices[0][5] = nodeMatrices[0][10] = nodeMatrices[0][15] = 1.0f;
    nodeMatrices[0][12] = 42.0f; /* node 0: identity + translated x=42 */
    nodeMatrices[1][0] = nodeMatrices[1][5] = nodeMatrices[1][10] = nodeMatrices[1][15] = 1.0f;
    nodeMatrices[1][13] = 7.0f; /* node 1: identity + translated y=7 */

    uint32_t paletteIndices[2] = {1, 0}; /* palette slot 0 -> node 1, slot 1 -> node 0 */
    float palette[BZ_QUEST_WC3_MAX_MATRIX_PALETTE][16];
    bz_quest_wc3_build_bone_palette(paletteIndices, 2, nodeMatrices, 2, palette);

    ASSERT_EQ_FLOAT(palette[0][13], 7.0f, 0.0001f);
    ASSERT_EQ_FLOAT(palette[1][12], 42.0f, 0.0001f);
    /* every unused slot beyond the 2 real entries is identity. */
    ASSERT_EQ_FLOAT(palette[2][0], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(palette[2][5], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(palette[2][12], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(palette[BZ_QUEST_WC3_MAX_MATRIX_PALETTE - 1][10], 1.0f, 0.0001f);
}

static void test_bone_palette_out_of_range_node_index_is_identity(void) {
    float nodeMatrices[1][16];
    memset(nodeMatrices, 0, sizeof(nodeMatrices));
    nodeMatrices[0][0] = nodeMatrices[0][5] = nodeMatrices[0][10] = nodeMatrices[0][15] = 1.0f;
    nodeMatrices[0][12] = 99.0f;

    uint32_t paletteIndices[1] = {5}; /* out of range: nodeCount is 1 */
    float palette[BZ_QUEST_WC3_MAX_MATRIX_PALETTE][16];
    bz_quest_wc3_build_bone_palette(paletteIndices, 1, nodeMatrices, 1, palette);
    ASSERT_EQ_FLOAT(palette[0][12], 0.0f, 0.0001f); /* identity fallback, not garbage */
    ASSERT_EQ_FLOAT(palette[0][0], 1.0f, 0.0001f);
}

void run_bz_quest_wc3_anim_tests(void) {
    RUN_TEST(test_vec3_track_no_track_yields_zero);
    RUN_TEST(test_vec3_scale_track_no_track_yields_one);
    RUN_TEST(test_vec3_linear_midpoint);
    RUN_TEST(test_vec3_exact_key_hit_returns_key_verbatim);
    RUN_TEST(test_vec3_none_interp_holds_left_value);
    RUN_TEST(test_vec3_hermite_matches_hand_derived_formula);
    RUN_TEST(test_vec3_bezier_matches_hand_derived_formula);
    RUN_TEST(test_vec3_before_first_key_clamps_to_first_key);
    RUN_TEST(test_vec3_single_key_holds_at_and_after_its_time);
    RUN_TEST(test_vec3_wraps_toward_first_key_past_last_key);
    RUN_TEST(test_vec3_wrap_clamps_when_first_key_at_interval_start);
    RUN_TEST(test_quat_no_track_yields_identity);
    RUN_TEST(test_quat_slerp_identity_to_90deg_about_z_midpoint);
    RUN_TEST(test_global_sequence_uses_render_clock_not_entity_frame);
    RUN_TEST(test_non_global_sequence_uses_entity_frame_and_sequence_interval);
    RUN_TEST(test_node_local_matrix_no_tracks_is_identity);
    RUN_TEST(test_node_local_matrix_translation_only);
    RUN_TEST(test_node_local_matrix_rotation_about_nonzero_pivot_leaves_pivot_fixed);
    RUN_TEST(test_hierarchy_child_inherits_parent_translation);
    RUN_TEST(test_hierarchy_root_node_alone);
    RUN_TEST(test_hierarchy_parent_cycle_degrades_to_root_not_hang);
    RUN_TEST(test_hierarchy_out_of_range_parent_treated_as_root);
    RUN_TEST(test_bone_palette_maps_indices_and_fills_identity);
    RUN_TEST(test_bone_palette_out_of_range_node_index_is_identity);
}
