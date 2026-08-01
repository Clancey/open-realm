/*
 * test_bz_quest_hand_input.c - host-buildable (no NDK/OpenXR/Vulkan/engine)
 * coverage for the layer 8 pure hand-tracking gesture builder
 * (bz_quest_hand_sample_build()): capability off/on (NONE/EXT_ONLY/FB_AIM),
 * both hands, valid/invalid/inactive joints, the FB aim path (index/middle
 * pinch bits, aim pose), the EXT-only fallback path (joint-position-only ray
 * basis, pinch-distance hysteresis, no invented second pinch), tracker
 * loss/reacquisition requiring a fresh pinch edge, and the intentionally
 * unsupported hand semantics (primary/secondary/thumbstick). See
 * bz_quest_hand_input.h.
 */
#include <math.h>
#include <string.h>

#include "bz_quest_hand_input.h"
#include "test_framework.h"

/* ------------------------------------------------------------- fixtures */

static void joints_reset(bzQuestHandJoints_t *j) { memset(j, 0, sizeof(*j)); }

/* A "natural pointing" hand: wrist at origin, index metacarpal a bit
 * forward+down from the wrist, index tip further along +Z (an arbitrary but
 * fixed, non-degenerate direction), thumb tip start far from the index tip
 * (not pinching). */
static void joints_pointing_not_pinching(bzQuestHandJoints_t *j) {
    joints_reset(j);
    j->wristValid = true;
    j->wristPos[0] = 0.0f; j->wristPos[1] = 1.2f; j->wristPos[2] = -0.3f;
    j->indexMetacarpalValid = true;
    j->indexMetacarpalPos[0] = 0.0f; j->indexMetacarpalPos[1] = 1.2f; j->indexMetacarpalPos[2] = -0.2f;
    j->indexTipValid = true;
    j->indexTipPos[0] = 0.0f; j->indexTipPos[1] = 1.2f; j->indexTipPos[2] = 0.0f; /* +Z from metacarpal */
    j->thumbTipValid = true;
    j->thumbTipPos[0] = 0.10f; j->thumbTipPos[1] = 1.2f; j->thumbTipPos[2] = 0.0f; /* 10cm away: not pinching */
}

static bzQuestHandSampleInput_t make_input(bzQuestHandCapability_t cap, bool trackerActive) {
    bzQuestHandSampleInput_t in;
    memset(&in, 0, sizeof(in));
    in.capability = cap;
    in.trackerActive = trackerActive;
    return in;
}

/* ----------------------------------------------------------- NONE/inactive */

static void test_capability_none_produces_fully_inactive_sample(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_NONE, true);
    joints_pointing_not_pinching(&in.joints);
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    memset(&out, 0xAA, sizeof(out)); /* poison to prove build() fully zeroes it */
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.active);
    ASSERT(!out.aimValid);
    ASSERT(!out.selectDown);
    ASSERT(!out.squeezeDown);
}

static void test_tracker_inactive_produces_inactive_sample_ext_only(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, false);
    joints_pointing_not_pinching(&in.joints);
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.active);
}

static void test_tracker_inactive_produces_inactive_sample_fb_aim(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_FB_AIM, false);
    in.aim.valid = true; /* even with (bogus) valid aim data, inactive tracker wins */
    in.aim.indexPinching = true;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.active);
    ASSERT(!out.selectDown);
}

static void test_none_capability_resets_pinch_latch(void) {
    /* A stale "was pinching" latch must never survive a NONE/inactive call -
     * see bz_quest_hand_sample_build()'s header comment on reacquisition. */
    bzQuestHandPinchState_t pinch = {0};
    pinch.pinching = true;
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_NONE, true);
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!pinch.pinching);
}

/* --------------------------------------------------------- EXT-only: ray */

static void test_ext_only_active_hand_reports_grip_from_wrist(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.active);
    ASSERT_EQ_FLOAT(out.gripPos[0], in.joints.wristPos[0], 0.0001f);
    ASSERT_EQ_FLOAT(out.gripPos[1], in.joints.wristPos[1], 0.0001f);
    ASSERT_EQ_FLOAT(out.gripPos[2], in.joints.wristPos[2], 0.0001f);
}

static void test_ext_only_ray_basis_from_index_joints(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.aimValid);
    /* Origin is the index fingertip. */
    ASSERT_EQ_FLOAT(out.aimOrigin[0], in.joints.indexTipPos[0], 0.0001f);
    ASSERT_EQ_FLOAT(out.aimOrigin[2], in.joints.indexTipPos[2], 0.0001f);
    /* Direction is the normalized metacarpal->tip vector: fixture points
     * along +Z, so dir must be (0,0,1) and unit length. */
    ASSERT_EQ_FLOAT(out.aimDir[0], 0.0f, 0.0005f);
    ASSERT_EQ_FLOAT(out.aimDir[1], 0.0f, 0.0005f);
    ASSERT_EQ_FLOAT(out.aimDir[2], 1.0f, 0.0005f);
    const float lenSq = out.aimDir[0] * out.aimDir[0] + out.aimDir[1] * out.aimDir[1] +
                        out.aimDir[2] * out.aimDir[2];
    ASSERT_EQ_FLOAT(lenSq, 1.0f, 0.001f);
}

static void test_ext_only_ray_invalid_when_metacarpal_invalid(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    in.joints.indexMetacarpalValid = false;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.active); /* the hand itself is still tracked/active */
    ASSERT(!out.aimValid); /* but no ray without both index joints valid */
}

static void test_ext_only_ray_invalid_when_index_tip_invalid(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    in.joints.indexTipValid = false;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.aimValid);
    ASSERT(!out.selectDown); /* pinch distance also needs the (invalid) index tip */
}

static void test_ext_only_ray_rejects_degenerate_direction(void) {
    /* Index metacarpal and tip coincide - no direction can be derived; must
     * not silently report a zero/garbage direction as valid. */
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    memcpy(in.joints.indexTipPos, in.joints.indexMetacarpalPos, sizeof(in.joints.indexTipPos));
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.aimValid);
}

/* ------------------------------------------------------ EXT-only: pinch */

static void test_ext_only_pinch_engages_below_threshold(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    /* Move thumb tip to just inside the engage threshold. */
    in.joints.thumbTipPos[0] = 0.0f;
    in.joints.thumbTipPos[2] = BZ_QUEST_HAND_PINCH_ENGAGE_M * 0.5f;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.selectDown);
    ASSERT(pinch.pinching);
}

static void test_ext_only_pinch_stays_released_above_threshold(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints); /* thumb 10cm away: well above both thresholds */
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.selectDown);
    ASSERT(!pinch.pinching);
}

static void test_ext_only_pinch_hysteresis_no_chatter_between_thresholds(void) {
    /* A fingertip gap strictly between ENGAGE and RELEASE must never toggle
     * the pinch state regardless of which side it approached from. */
    const float midGap = (BZ_QUEST_HAND_PINCH_ENGAGE_M + BZ_QUEST_HAND_PINCH_RELEASE_M) * 0.5f;
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    in.joints.thumbTipPos[0] = 0.0f;
    in.joints.thumbTipPos[2] = midGap;

    /* Approaching from "was pinching": stays pinching (has not crossed RELEASE). */
    bzQuestHandPinchState_t pinchWasOn = {0};
    pinchWasOn.pinching = true;
    bzQuestInputHandSample_t outOn;
    bz_quest_hand_sample_build(&in, &pinchWasOn, &outOn);
    ASSERT(outOn.selectDown);
    ASSERT(pinchWasOn.pinching);

    /* Approaching from "was released": stays released (has not crossed ENGAGE). */
    bzQuestHandPinchState_t pinchWasOff = {0};
    bzQuestInputHandSample_t outOff;
    bz_quest_hand_sample_build(&in, &pinchWasOff, &outOff);
    ASSERT(!outOff.selectDown);
    ASSERT(!pinchWasOff.pinching);

    /* Repeated calls at the same mid-gap distance are idempotent (no
     * internal chatter/self-toggle) for several frames in a row. */
    for (int i = 0; i < 5; ++i) {
        bz_quest_hand_sample_build(&in, &pinchWasOn, &outOn);
        ASSERT(outOn.selectDown);
        bz_quest_hand_sample_build(&in, &pinchWasOff, &outOff);
        ASSERT(!outOff.selectDown);
    }
}

static void test_ext_only_pinch_release_crosses_wider_band(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    bzQuestHandPinchState_t pinch = {0};
    pinch.pinching = true; /* was pinching */
    bzQuestInputHandSample_t out;

    /* Just past ENGAGE but before RELEASE: still latched pinching (hysteresis). */
    in.joints.thumbTipPos[0] = 0.0f;
    in.joints.thumbTipPos[2] = BZ_QUEST_HAND_PINCH_ENGAGE_M + 0.001f;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.selectDown);

    /* Past RELEASE: latch finally clears. */
    in.joints.thumbTipPos[2] = BZ_QUEST_HAND_PINCH_RELEASE_M + 0.001f;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.selectDown);
}

static void test_ext_only_pinch_requires_thumb_valid(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    in.joints.thumbTipPos[0] = 0.0f;
    in.joints.thumbTipPos[2] = 0.0f; /* would engage a pinch if trusted */
    in.joints.thumbTipValid = false;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.selectDown);
    ASSERT(!pinch.pinching);
}

static void test_ext_only_squeeze_always_false(void) {
    /* No evidence-backed second pinch/grab without FB aim - see this
     * module's header comment. Even a fully-engaged index pinch must never
     * set squeezeDown. */
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    in.joints.thumbTipPos[0] = 0.0f;
    in.joints.thumbTipPos[2] = 0.0f;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.selectDown);
    ASSERT(!out.squeezeDown);
}

static void test_ext_only_reacquisition_requires_fresh_engage(void) {
    /* A pinch that was latched "on" at a mid-gap distance (hysteresis), then
     * the tracker is lost and reacquired at the SAME mid-gap distance, must
     * NOT resume pinching - it must require a fresh ENGAGE crossing. */
    const float midGap = (BZ_QUEST_HAND_PINCH_ENGAGE_M + BZ_QUEST_HAND_PINCH_RELEASE_M) * 0.5f;
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&in.joints);
    in.joints.thumbTipPos[0] = 0.0f;
    in.joints.thumbTipPos[2] = midGap;

    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    /* Engage first via a tight gap, then move to the mid-gap (hysteresis
     * keeps it latched). */
    in.joints.thumbTipPos[2] = 0.0f;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.selectDown);
    in.joints.thumbTipPos[2] = midGap;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.selectDown); /* still latched via hysteresis */

    /* Tracker lost (inactive) - must clear the latch. */
    bzQuestHandSampleInput_t lost = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, false);
    bz_quest_hand_sample_build(&lost, &pinch, &out);
    ASSERT(!pinch.pinching);

    /* Reacquired at the SAME mid-gap distance: must NOT resume pinching -
     * a fresh ENGAGE crossing is required. */
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.selectDown);
    ASSERT(!pinch.pinching);
}

/* ------------------------------------------------------------- FB aim tier */

static void test_fb_aim_invalid_produces_no_output(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_FB_AIM, true);
    in.aim.valid = false;
    in.aim.indexPinching = true; /* must be ignored: aim not computed/valid this frame */
    in.aim.middlePinching = true;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.active); /* hand itself still tracked */
    ASSERT(!out.aimValid);
    ASSERT(!out.selectDown);
    ASSERT(!out.squeezeDown);
}

static void test_fb_aim_uses_runtime_index_pinch_bit_directly(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_FB_AIM, true);
    in.aim.valid = true;
    in.aim.aimOrigin[0] = 1.0f;
    in.aim.aimOrigin[1] = 2.0f;
    in.aim.aimOrigin[2] = 3.0f;
    in.aim.aimDir[2] = -1.0f;
    in.aim.indexPinching = true;
    in.aim.middlePinching = false;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.aimValid);
    ASSERT(out.selectDown);
    ASSERT(!out.squeezeDown);
    ASSERT_EQ_FLOAT(out.aimOrigin[0], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.aimOrigin[1], 2.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.aimOrigin[2], 3.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.aimDir[2], -1.0f, 0.0001f);
}

static void test_fb_aim_middle_pinch_maps_to_squeeze(void) {
    /* The evidence-backed second pinch/grab this layer's task contract
     * permits - see bz_quest_hand_input.h's header comment. */
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_FB_AIM, true);
    in.aim.valid = true;
    in.aim.indexPinching = false;
    in.aim.middlePinching = true;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.selectDown);
    ASSERT(out.squeezeDown);
}

static void test_fb_aim_both_pinches_independent(void) {
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_FB_AIM, true);
    in.aim.valid = true;
    in.aim.indexPinching = true;
    in.aim.middlePinching = true;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(out.selectDown);
    ASSERT(out.squeezeDown);
}

static void test_fb_aim_ignores_ext_only_joints(void) {
    /* At the FB_AIM tier, joint data (even if populated) must not influence
     * the ray/pinch decision - only the aim struct does. */
    bzQuestHandSampleInput_t in = make_input(BZ_QUEST_HAND_CAPABILITY_FB_AIM, true);
    joints_pointing_not_pinching(&in.joints);
    in.joints.thumbTipPos[0] = 0.0f;
    in.joints.thumbTipPos[2] = 0.0f; /* would engage an EXT-only pinch, must be ignored here */
    in.aim.valid = true;
    in.aim.indexPinching = false;
    bzQuestHandPinchState_t pinch = {0};
    bzQuestInputHandSample_t out;
    bz_quest_hand_sample_build(&in, &pinch, &out);
    ASSERT(!out.selectDown);
    ASSERT(!pinch.pinching); /* the EXT-only hysteresis latch is never touched at this tier */
}

/* --------------------------------------------------- unsupported semantics */

static void test_no_hand_analog_fields_always_zero(void) {
    /* primaryDown/secondaryDown/thumbstick have no hand-tracking analog at
     * either capability level - see bz_quest_hand_input.h's header comment. */
    bzQuestHandPinchState_t pinch;
    bzQuestInputHandSample_t out;

    memset(&pinch, 0, sizeof(pinch));
    bzQuestHandSampleInput_t extIn = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&extIn.joints);
    bz_quest_hand_sample_build(&extIn, &pinch, &out);
    ASSERT(!out.primaryDown);
    ASSERT(!out.secondaryDown);
    ASSERT_EQ_FLOAT(out.thumbstick[0], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.thumbstick[1], 0.0f, 0.0001f);

    memset(&pinch, 0, sizeof(pinch));
    bzQuestHandSampleInput_t aimIn = make_input(BZ_QUEST_HAND_CAPABILITY_FB_AIM, true);
    aimIn.aim.valid = true;
    aimIn.aim.indexPinching = true;
    aimIn.aim.middlePinching = true;
    bz_quest_hand_sample_build(&aimIn, &pinch, &out);
    ASSERT(!out.primaryDown);
    ASSERT(!out.secondaryDown);
    ASSERT_EQ_FLOAT(out.thumbstick[0], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out.thumbstick[1], 0.0f, 0.0001f);
}

/* -------------------------------------------------------------- both hands */

static void test_both_hands_build_independently(void) {
    /* Left EXT-only pinching, right FB_AIM not pinching - two separate
     * pinchState instances, two separate outputs, no cross-contamination. */
    bzQuestHandSampleInput_t left = make_input(BZ_QUEST_HAND_CAPABILITY_EXT_ONLY, true);
    joints_pointing_not_pinching(&left.joints);
    left.joints.thumbTipPos[0] = 0.0f;
    left.joints.thumbTipPos[2] = 0.0f; /* engaged */

    bzQuestHandSampleInput_t right = make_input(BZ_QUEST_HAND_CAPABILITY_FB_AIM, true);
    right.aim.valid = true;
    right.aim.indexPinching = false;

    bzQuestHandPinchState_t leftPinch = {0}, rightPinch = {0};
    bzQuestInputHandSample_t leftOut, rightOut;
    bz_quest_hand_sample_build(&left, &leftPinch, &leftOut);
    bz_quest_hand_sample_build(&right, &rightPinch, &rightOut);

    ASSERT(leftOut.active && leftOut.selectDown);
    ASSERT(rightOut.active && !rightOut.selectDown);
    ASSERT(leftPinch.pinching);
    ASSERT(!rightPinch.pinching);
}

void run_bz_quest_hand_input_tests(void) {
    RUN_TEST(test_capability_none_produces_fully_inactive_sample);
    RUN_TEST(test_tracker_inactive_produces_inactive_sample_ext_only);
    RUN_TEST(test_tracker_inactive_produces_inactive_sample_fb_aim);
    RUN_TEST(test_none_capability_resets_pinch_latch);
    RUN_TEST(test_ext_only_active_hand_reports_grip_from_wrist);
    RUN_TEST(test_ext_only_ray_basis_from_index_joints);
    RUN_TEST(test_ext_only_ray_invalid_when_metacarpal_invalid);
    RUN_TEST(test_ext_only_ray_invalid_when_index_tip_invalid);
    RUN_TEST(test_ext_only_ray_rejects_degenerate_direction);
    RUN_TEST(test_ext_only_pinch_engages_below_threshold);
    RUN_TEST(test_ext_only_pinch_stays_released_above_threshold);
    RUN_TEST(test_ext_only_pinch_hysteresis_no_chatter_between_thresholds);
    RUN_TEST(test_ext_only_pinch_release_crosses_wider_band);
    RUN_TEST(test_ext_only_pinch_requires_thumb_valid);
    RUN_TEST(test_ext_only_squeeze_always_false);
    RUN_TEST(test_ext_only_reacquisition_requires_fresh_engage);
    RUN_TEST(test_fb_aim_invalid_produces_no_output);
    RUN_TEST(test_fb_aim_uses_runtime_index_pinch_bit_directly);
    RUN_TEST(test_fb_aim_middle_pinch_maps_to_squeeze);
    RUN_TEST(test_fb_aim_both_pinches_independent);
    RUN_TEST(test_fb_aim_ignores_ext_only_joints);
    RUN_TEST(test_no_hand_analog_fields_always_zero);
    RUN_TEST(test_both_hands_build_independently);
}
