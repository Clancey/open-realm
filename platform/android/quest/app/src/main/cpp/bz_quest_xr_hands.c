/*
 * bz_quest_xr_hands.c - see bz_quest_xr_hands.h.
 */
#include "bz_quest_xr_hands.h"

#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"

/* Resolves one XR_EXT_hand_tracking function pointer, mirroring
 * bz_quest_xr.c's bz_quest_xr_resolve() discipline exactly (that helper is
 * private/static to bz_quest_xr.c, so this is a second, identically-shaped
 * 6-line resolver rather than exposing bz_quest_xr.c's internals - the
 * duplicated part is the generic xrGetInstanceProcAddr-plus-log wrapper,
 * never the actual capability/tracker-creation policy, which is not
 * duplicated anywhere). */
static bool bz_quest_xr_hands_resolve(XrInstance instance, const char *name, PFN_xrVoidFunction *out) {
    XrResult result = xrGetInstanceProcAddr(instance, name, out);
    if (result != XR_SUCCESS || *out == NULL) {
        BZ_QUEST_LOGE("xrGetInstanceProcAddr(%s) failed: %d", name, (int)result);
        return false;
    }
    return true;
}

bool bz_quest_xr_hands_create(bzQuestXr_t *xr, bzQuestXrHands_t *hands) {
    memset(hands, 0, sizeof(*hands));
    if (!xr->handCapability.supported) {
        BZ_QUEST_LOGI(
            "XR_EXT_hand_tracking not supported this session (runtime/device capability, or the "
            "manifest's com.oculus.permission.HAND_TRACKING/oculus.software.handtracking flags, or the "
            "user's OS-level hand-tracking setting) - hand input disabled, Touch controllers remain the "
            "only input source");
        return true; /* optional capability - never a startup failure */
    }

    bool proceduresOk = true;
    proceduresOk &= bz_quest_xr_hands_resolve(xr->instance, "xrCreateHandTrackerEXT",
                                              (PFN_xrVoidFunction *)&hands->createHandTrackerEXT);
    proceduresOk &= bz_quest_xr_hands_resolve(xr->instance, "xrDestroyHandTrackerEXT",
                                              (PFN_xrVoidFunction *)&hands->destroyHandTrackerEXT);
    proceduresOk &= bz_quest_xr_hands_resolve(xr->instance, "xrLocateHandJointsEXT",
                                              (PFN_xrVoidFunction *)&hands->locateHandJointsEXT);
    if (!proceduresOk) {
        /* The extension string enumerated+enabled but the runtime failed to
         * resolve its own advertised functions - a genuine runtime defect
         * (already logged above, once per function). Degrade to hands-
         * disabled rather than fail renderer init: Touch input must keep
         * working regardless (see docs/quest-tabletop.md's "hand support
         * must not be required for startup"). */
        memset(hands, 0, sizeof(*hands));
        return true;
    }
    hands->proceduresResolved = true;

    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h) {
        XrHandTrackerCreateInfoEXT createInfo;
        memset(&createInfo, 0, sizeof(createInfo));
        createInfo.type = XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT;
        createInfo.hand = (h == BZ_QUEST_INPUT_HAND_LEFT) ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        XrResult result = hands->createHandTrackerEXT(xr->session, &createInfo, &hands->tracker[h]);
        if (result != XR_SUCCESS) {
            BZ_QUEST_LOGE("xrCreateHandTrackerEXT(hand=%d) failed: %d - hand input disabled for this hand",
                          h, (int)result);
            hands->tracker[h] = XR_NULL_HANDLE;
        }
    }
    BZ_QUEST_LOGI("hand tracking enabled (%s)",
                  xr->handCapability.aimSupported ? "XR_FB_hand_tracking_aim" : "XR_EXT_hand_tracking only");
    return true;
}

/* Unpacks one joint's position + validity, mirroring bz_quest_xr_actions.c's
 * bz_quest_xr_locate_pose()'s XR_SPACE_LOCATION_POSITION_VALID_BIT check
 * convention exactly. Orientation is never read for these joints - see
 * bz_quest_hand_input.h's header comment for why. */
static void bz_quest_xr_hands_unpack_joint(const XrHandJointLocationEXT *joint, bool *outValid,
                                          float outPos[3]) {
    *outValid = (joint->locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
    outPos[0] = joint->pose.position.x;
    outPos[1] = joint->pose.position.y;
    outPos[2] = joint->pose.position.z;
}

/* Locates one hand's joints (+ chained FB aim state when supported) at
 * `displayTime` and unpacks the result into `in` - the plain POD
 * bz_quest_hand_sample_build() consumes. Returns false only on a hard,
 * once-logged xrLocateHandJointsEXT failure. */
static bool bz_quest_xr_hands_locate_one(bzQuestXr_t *xr, bzQuestXrHands_t *hands, int h, XrTime displayTime,
                                         bzQuestHandSampleInput_t *in) {
    XrHandJointLocationEXT joints[XR_HAND_JOINT_COUNT_EXT];
    memset(joints, 0, sizeof(joints));
    XrHandJointLocationsEXT locations;
    memset(&locations, 0, sizeof(locations));
    locations.type = XR_TYPE_HAND_JOINT_LOCATIONS_EXT;
    locations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    locations.jointLocations = joints;

    XrHandTrackingAimStateFB aimState;
    memset(&aimState, 0, sizeof(aimState));
    if (xr->handCapability.aimSupported) {
        aimState.type = XR_TYPE_HAND_TRACKING_AIM_STATE_FB;
        locations.next = &aimState;
    }

    XrHandJointsLocateInfoEXT locateInfo;
    memset(&locateInfo, 0, sizeof(locateInfo));
    locateInfo.type = XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT;
    locateInfo.baseSpace = xr->appSpace;
    locateInfo.time = displayTime;

    XrResult result = hands->locateHandJointsEXT(hands->tracker[h], &locateInfo, &locations);
    if (result != XR_SUCCESS) {
        if (!hands->loggedLocateFailure[h]) {
            BZ_QUEST_LOGE("xrLocateHandJointsEXT(hand=%d) failed: %d", h, (int)result);
            hands->loggedLocateFailure[h] = true;
        }
        return false;
    }
    hands->loggedLocateFailure[h] = false;

    in->trackerActive = locations.isActive;
    bz_quest_xr_hands_unpack_joint(&joints[XR_HAND_JOINT_WRIST_EXT], &in->joints.wristValid,
                                   in->joints.wristPos);
    bz_quest_xr_hands_unpack_joint(&joints[XR_HAND_JOINT_INDEX_METACARPAL_EXT],
                                   &in->joints.indexMetacarpalValid, in->joints.indexMetacarpalPos);
    bz_quest_xr_hands_unpack_joint(&joints[XR_HAND_JOINT_INDEX_TIP_EXT], &in->joints.indexTipValid,
                                   in->joints.indexTipPos);
    bz_quest_xr_hands_unpack_joint(&joints[XR_HAND_JOINT_THUMB_TIP_EXT], &in->joints.thumbTipValid,
                                   in->joints.thumbTipPos);

    if (xr->handCapability.aimSupported) {
        const XrHandTrackingAimFlagsFB need =
            XR_HAND_TRACKING_AIM_COMPUTED_BIT_FB | XR_HAND_TRACKING_AIM_VALID_BIT_FB;
        in->aim.valid = (aimState.status & need) == need;
        if (in->aim.valid) {
            in->aim.aimOrigin[0] = aimState.aimPose.position.x;
            in->aim.aimOrigin[1] = aimState.aimPose.position.y;
            in->aim.aimOrigin[2] = aimState.aimPose.position.z;
            bz_quest_quat_forward(aimState.aimPose.orientation.x, aimState.aimPose.orientation.y,
                                  aimState.aimPose.orientation.z, aimState.aimPose.orientation.w,
                                  in->aim.aimDir);
            in->aim.indexPinching = (aimState.status & XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB) != 0;
            in->aim.middlePinching = (aimState.status & XR_HAND_TRACKING_AIM_MIDDLE_PINCHING_BIT_FB) != 0;
        }
    }
    return true;
}

bool bz_quest_xr_hands_sync(bzQuestXr_t *xr, bzQuestXrHands_t *hands, XrTime displayTime,
                            bzQuestInputFrame_t *frame) {
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h)
        memset(&frame->handSample[h], 0, sizeof(frame->handSample[h]));

    if (!hands->proceduresResolved || !xr->sessionRunning || xr->sessionState != XR_SESSION_STATE_FOCUSED)
        return true; /* mirrors bz_quest_xr_actions_sync()'s exact focus/running gate */

    bool ok = true;
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h) {
        if (hands->tracker[h] == XR_NULL_HANDLE) continue;

        bzQuestHandSampleInput_t in;
        memset(&in, 0, sizeof(in));
        in.capability = xr->handCapability.aimSupported ? BZ_QUEST_HAND_CAPABILITY_FB_AIM
                                                        : BZ_QUEST_HAND_CAPABILITY_EXT_ONLY;
        if (!bz_quest_xr_hands_locate_one(xr, hands, h, displayTime, &in)) {
            ok = false;
            continue;
        }
        bz_quest_hand_sample_build(&in, &hands->pinchState[h], &frame->handSample[h]);
    }
    return ok;
}

void bz_quest_xr_hands_destroy(bzQuestXrHands_t *hands) {
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h) {
        if (hands->tracker[h] != XR_NULL_HANDLE && hands->destroyHandTrackerEXT) {
            hands->destroyHandTrackerEXT(hands->tracker[h]);
            hands->tracker[h] = XR_NULL_HANDLE;
        }
    }
}
