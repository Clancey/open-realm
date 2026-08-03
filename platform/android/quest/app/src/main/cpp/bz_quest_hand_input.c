/*
 * bz_quest_hand_input.c - see bz_quest_hand_input.h. Pure gesture-building
 * logic: no OpenXR/Vulkan/engine types, no allocation, no locking, no I/O,
 * no logging - a plain host C compiler builds this (see that header's
 * "Frame-critical / real-time discipline" note and
 * platform/android/quest/scripts/test-quest-hand-tracking-layout.sh, which
 * greps bz_quest_hand_sample_build()'s body AND the two static helpers it
 * calls every frame, bz_hand_build_fb_aim()/bz_hand_build_ext_only(), for
 * exactly those - all three run on the XR render thread every frame a hand
 * sample is built).
 */
#include "bz_quest_hand_input.h"

#include <math.h>
#include <string.h>

static float bz_hand_distance(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* Normalizes `v` into `out`; returns false (leaving `out` untouched) if `v`
 * is degenerate (near-zero length, < 0.1mm) - the two joints defining this
 * ray basis coincided or were not usefully separated, so no direction can
 * be derived. Never silently returns a garbage/zero direction. */
static bool bz_hand_normalize(const float v[3], float out[3]) {
    const float lenSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (lenSq < 1e-8f) return false;
    const float invLen = 1.0f / sqrtf(lenSq);
    out[0] = v[0] * invLen;
    out[1] = v[1] * invLen;
    out[2] = v[2] * invLen;
    return true;
}

/* FB_AIM tier: trust the runtime's own standardized aim pose + *Pinching
 * status bits directly - see this file's header comment for why this
 * module never re-derives pinch from a strength/distance at this level. */
static void bz_hand_build_fb_aim(const bzQuestHandSampleInput_t *in, bzQuestInputHandSample_t *out) {
    if (!in->aim.valid) return; /* aimValid/select/squeeze all stay false - not computed this frame */
    out->aimValid = true;
    memcpy(out->aimOrigin, in->aim.aimOrigin, sizeof(out->aimOrigin));
    memcpy(out->aimDir, in->aim.aimDir, sizeof(out->aimDir));
    out->selectDown = in->aim.indexPinching;
    out->squeezeDown = in->aim.middlePinching;
}

/* EXT_ONLY tier: ray basis grounded in tracked index-finger joint
 * POSITIONS only (see this file's header comment for why not orientation),
 * and an explicit, hysteresis-debounced pinch-distance threshold between
 * the index tip and thumb tip. */
static void bz_hand_build_ext_only(const bzQuestHandSampleInput_t *in, bzQuestHandPinchState_t *pinchState,
                                   bzQuestInputHandSample_t *out) {
    const bzQuestHandJoints_t *j = &in->joints;
    if (j->indexMetacarpalValid && j->indexTipValid) {
        const float dir[3] = {
            j->indexTipPos[0] - j->indexMetacarpalPos[0],
            j->indexTipPos[1] - j->indexMetacarpalPos[1],
            j->indexTipPos[2] - j->indexMetacarpalPos[2],
        };
        if (bz_hand_normalize(dir, out->aimDir)) {
            out->aimValid = true;
            memcpy(out->aimOrigin, j->indexTipPos, sizeof(out->aimOrigin));
        }
    }

    if (out->aimValid && j->thumbTipValid) {
        const float dist = bz_hand_distance(j->indexTipPos, j->thumbTipPos);
        /* Hysteresis: engage at the tighter threshold, release at the wider
         * one, so a fingertip gap sitting between the two never chatters
         * selectDown on/off frame to frame (see BZ_QUEST_HAND_PINCH_ENGAGE_M/
         * _RELEASE_M's header comment). */
        if (pinchState->pinching) {
            if (dist > BZ_QUEST_HAND_PINCH_RELEASE_M) pinchState->pinching = false;
        } else {
            if (dist < BZ_QUEST_HAND_PINCH_ENGAGE_M) pinchState->pinching = true;
        }
        out->selectDown = pinchState->pinching;
    } else {
        pinchState->pinching = false; /* joints not usable this frame: never latch a stale pinch */
    }
    /* squeezeDown stays false (out was zeroed by the caller): no evidence-
     * backed second pinch/grab signal exists without FB aim. */
}

void bz_quest_hand_sample_build(const bzQuestHandSampleInput_t *in, bzQuestHandPinchState_t *pinchState,
                                bzQuestInputHandSample_t *out) {
    memset(out, 0, sizeof(*out));
    if (in->capability == BZ_QUEST_HAND_CAPABILITY_NONE || !in->trackerActive) {
        pinchState->pinching = false; /* not tracked: never carry a stale latch into reacquisition */
        return;
    }
    out->active = true;
    if (in->joints.wristValid) memcpy(out->gripPos, in->joints.wristPos, sizeof(out->gripPos));

    if (in->capability == BZ_QUEST_HAND_CAPABILITY_FB_AIM) bz_hand_build_fb_aim(in, out);
    else bz_hand_build_ext_only(in, pinchState, out);
}
