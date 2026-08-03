/*
 * bz_quest_xr_actions.c - see bz_quest_xr_actions.h.
 */
#include "bz_quest_xr_actions.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bz_quest_log.h"
#include "bz_quest_pure.h"

/* Upper bound on suggested bindings per profile: every action, both hands. */
#define BZ_QUEST_XR_MAX_BINDINGS (BZ_QUEST_XR_ACTION_COUNT * BZ_QUEST_XR_SIDE_COUNT)

static const char *const kSidePath[BZ_QUEST_XR_SIDE_COUNT] = {"/user/hand/left", "/user/hand/right"};

static XrActionType bz_quest_xr_action_type_to_openxr(bzQuestXrActionType_t type) {
    switch (type) {
    case BZ_QUEST_XR_ACTION_TYPE_POSE: return XR_ACTION_TYPE_POSE_INPUT;
    case BZ_QUEST_XR_ACTION_TYPE_BOOLEAN: return XR_ACTION_TYPE_BOOLEAN_INPUT;
    case BZ_QUEST_XR_ACTION_TYPE_FLOAT: return XR_ACTION_TYPE_FLOAT_INPUT;
    case BZ_QUEST_XR_ACTION_TYPE_VECTOR2F: return XR_ACTION_TYPE_VECTOR2F_INPUT;
    case BZ_QUEST_XR_ACTION_TYPE_VIBRATION: return XR_ACTION_TYPE_VIBRATION_OUTPUT;
    default: return XR_ACTION_TYPE_BOOLEAN_INPUT;
    }
}

/* Creates one XrAction. `leftOnly` restricts subaction paths to the left hand
 * (MENU); everything else gets both hands so a single action serves both
 * controllers via subaction-path-scoped state reads. */
static bool bz_quest_xr_create_one_action(bzQuestXrActions_t *a, bzQuestXrActionId_t id) {
    XrActionCreateInfo info = {0};
    info.type = XR_TYPE_ACTION_CREATE_INFO;
    info.actionType = bz_quest_xr_action_type_to_openxr(bz_quest_xr_action_type(id));
    strncpy(info.actionName, bz_quest_xr_action_name(id), sizeof(info.actionName) - 1);
    strncpy(info.localizedActionName, bz_quest_xr_action_localized(id),
            sizeof(info.localizedActionName) - 1);
    bool leftOnly = bz_quest_xr_action_left_only(id);
    XrPath subPaths[BZ_QUEST_XR_SIDE_COUNT] = {a->handPath[BZ_QUEST_XR_SIDE_LEFT],
                                               a->handPath[BZ_QUEST_XR_SIDE_RIGHT]};
    info.countSubactionPaths = leftOnly ? 1u : (uint32_t)BZ_QUEST_XR_SIDE_COUNT;
    info.subactionPaths = subPaths;
    XrResult result = xrCreateAction(a->actionSet, &info, &a->actions[id]);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrCreateAction(%s) failed: %d", bz_quest_xr_action_name(id), (int)result);
        return false;
    }
    return true;
}

/* Suggests one interaction profile's binding table (see bz_quest_xr_bindings.h
 * for the pure path tables this walks). Hard-fails on a bad path string or a
 * failed xrSuggestInteractionProfileBindings. */
static bool bz_quest_xr_suggest_profile(bzQuestXr_t *xr, bzQuestXrActions_t *a,
                                        bzQuestXrProfile_t profile) {
    XrActionSuggestedBinding bindings[BZ_QUEST_XR_MAX_BINDINGS];
    uint32_t count = 0;
    for (int id = 0; id < BZ_QUEST_XR_ACTION_COUNT; ++id) {
        for (int side = 0; side < BZ_QUEST_XR_SIDE_COUNT; ++side) {
            const char *component =
                bz_quest_xr_binding_component(profile, (bzQuestXrActionId_t)id, (bzQuestXrSide_t)side);
            if (!component) continue;
            char full[XR_MAX_PATH_LENGTH];
            snprintf(full, sizeof(full), "%s/%s", kSidePath[side], component);
            XrPath path = XR_NULL_PATH;
            XrResult sp = xrStringToPath(xr->instance, full, &path);
            if (sp != XR_SUCCESS) {
                BZ_QUEST_LOGE("xrStringToPath(%s) failed: %d", full, (int)sp);
                return false;
            }
            bindings[count].action = a->actions[id];
            bindings[count].binding = path;
            count++;
        }
    }

    XrPath profilePath = XR_NULL_PATH;
    XrResult pp = xrStringToPath(xr->instance, bz_quest_xr_profile_path(profile), &profilePath);
    if (pp != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrStringToPath(%s) failed: %d", bz_quest_xr_profile_path(profile), (int)pp);
        return false;
    }
    XrInteractionProfileSuggestedBinding suggest = {0};
    suggest.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    suggest.interactionProfile = profilePath;
    suggest.suggestedBindings = bindings;
    suggest.countSuggestedBindings = count;
    XrResult result = xrSuggestInteractionProfileBindings(xr->instance, &suggest);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrSuggestInteractionProfileBindings(%s) failed: %d",
                      bz_quest_xr_profile_path(profile), (int)result);
        return false;
    }
    return true;
}

static bool bz_quest_xr_create_pose_space(bzQuestXr_t *xr, bzQuestXrActions_t *a, XrAction action,
                                          int side, XrSpace *outSpace) {
    XrActionSpaceCreateInfo info = {0};
    info.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
    info.action = action;
    info.subactionPath = a->handPath[side];
    info.poseInActionSpace.orientation.w = 1.0f; /* identity pose */
    XrResult result = xrCreateActionSpace(xr->session, &info, outSpace);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrCreateActionSpace(side=%d) failed: %d", side, (int)result);
        return false;
    }
    return true;
}

bool bz_quest_xr_actions_create(bzQuestXr_t *xr, bzQuestXrActions_t *actions) {
    memset(actions, 0, sizeof(*actions));

    XrActionSetCreateInfo setInfo = {0};
    setInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    strncpy(setInfo.actionSetName, "tabletop", sizeof(setInfo.actionSetName) - 1);
    strncpy(setInfo.localizedActionSetName, "Tabletop Controls",
            sizeof(setInfo.localizedActionSetName) - 1);
    setInfo.priority = 0;
    XrResult result = xrCreateActionSet(xr->instance, &setInfo, &actions->actionSet);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrCreateActionSet failed: %d", (int)result);
        return false;
    }

    for (int side = 0; side < BZ_QUEST_XR_SIDE_COUNT; ++side) {
        XrResult sp = xrStringToPath(xr->instance, kSidePath[side], &actions->handPath[side]);
        if (sp != XR_SUCCESS) {
            BZ_QUEST_LOGE("xrStringToPath(%s) failed: %d", kSidePath[side], (int)sp);
            return false;
        }
    }

    for (int id = 0; id < BZ_QUEST_XR_ACTION_COUNT; ++id) {
        if (!bz_quest_xr_create_one_action(actions, (bzQuestXrActionId_t)id)) return false;
    }

    if (!bz_quest_xr_suggest_profile(xr, actions, BZ_QUEST_XR_PROFILE_TOUCH)) return false;
    if (!bz_quest_xr_suggest_profile(xr, actions, BZ_QUEST_XR_PROFILE_SIMPLE)) return false;

    for (int side = 0; side < BZ_QUEST_XR_SIDE_COUNT; ++side) {
        if (!bz_quest_xr_create_pose_space(xr, actions, actions->actions[BZ_QUEST_XR_ACTION_AIM_POSE], side,
                                           &actions->aimSpace[side]))
            return false;
        if (!bz_quest_xr_create_pose_space(xr, actions, actions->actions[BZ_QUEST_XR_ACTION_GRIP_POSE],
                                           side, &actions->gripSpace[side]))
            return false;
    }

    XrSessionActionSetsAttachInfo attach = {0};
    attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attach.countActionSets = 1;
    attach.actionSets = &actions->actionSet;
    result = xrAttachSessionActionSets(xr->session, &attach);
    if (result != XR_SUCCESS) {
        BZ_QUEST_LOGE("xrAttachSessionActionSets failed: %d", (int)result);
        return false;
    }
    actions->attached = true;
    BZ_QUEST_LOGI("xr actions attached (Touch + simple-controller fallback)");
    return true;
}

static bool bz_quest_xr_read_bool(XrSession session, XrAction action, XrPath sub, bool *outDown) {
    XrActionStateGetInfo get = {0};
    get.type = XR_TYPE_ACTION_STATE_GET_INFO;
    get.action = action;
    get.subactionPath = sub;
    XrActionStateBoolean state = {0};
    state.type = XR_TYPE_ACTION_STATE_BOOLEAN;
    if (xrGetActionStateBoolean(session, &get, &state) != XR_SUCCESS) {
        *outDown = false;
        return false;
    }
    /* isActive == false (device/profile lost) is NEVER "still pressed" - the
     * pure edge detector also forces this, but clearing here keeps the sample
     * itself honest. */
    *outDown = state.isActive && state.currentState;
    return true;
}

static void bz_quest_xr_read_vector2f(XrSession session, XrAction action, XrPath sub, float out[2]) {
    XrActionStateGetInfo get = {0};
    get.type = XR_TYPE_ACTION_STATE_GET_INFO;
    get.action = action;
    get.subactionPath = sub;
    XrActionStateVector2f state = {0};
    state.type = XR_TYPE_ACTION_STATE_VECTOR2F;
    if (xrGetActionStateVector2f(session, &get, &state) != XR_SUCCESS || !state.isActive) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        return;
    }
    out[0] = state.currentState.x;
    out[1] = state.currentState.y;
}

/* Locates a hand pose space at `time`; fills origin (+forward dir for aim).
 * Returns true only when BOTH position and orientation are valid+tracked. */
static bool bz_quest_xr_locate_pose(bzQuestXr_t *xr, XrSpace space, XrTime time, float outOrigin[3],
                                    float *outDir) {
    XrSpaceLocation loc = {0};
    loc.type = XR_TYPE_SPACE_LOCATION;
    if (xrLocateSpace(space, xr->appSpace, time, &loc) != XR_SUCCESS) return false;
    const XrSpaceLocationFlags need =
        XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return false;
    outOrigin[0] = loc.pose.position.x;
    outOrigin[1] = loc.pose.position.y;
    outOrigin[2] = loc.pose.position.z;
    if (outDir)
        bz_quest_quat_forward(loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z,
                             loc.pose.orientation.w, outDir);
    return true;
}

static bool bz_quest_xr_pose_active(XrSession session, XrAction action, XrPath sub) {
    XrActionStateGetInfo get = {0};
    get.type = XR_TYPE_ACTION_STATE_GET_INFO;
    get.action = action;
    get.subactionPath = sub;
    XrActionStatePose state = {0};
    state.type = XR_TYPE_ACTION_STATE_POSE;
    if (xrGetActionStatePose(session, &get, &state) != XR_SUCCESS) return false;
    return state.isActive;
}

bool bz_quest_xr_actions_sync(bzQuestXr_t *xr, bzQuestXrActions_t *actions, XrTime displayTime,
                              bzQuestInputFrame_t *frame) {
    /* Only the controller-sample portion is written here; caller owns world/
     * selection/dt. Start every hand inactive so an early return leaves the
     * pure state machine in its idempotent-clear path. */
    for (int h = 0; h < BZ_QUEST_INPUT_HAND_COUNT; ++h) {
        memset(&frame->hands[h], 0, sizeof(frame->hands[h]));
    }
    frame->menuDown = false;
    frame->focused = (xr->sessionState == XR_SESSION_STATE_FOCUSED);

    if (!xr->sessionRunning || !frame->focused || !actions->attached) {
        /* Not focused: do NOT xrSyncActions (undefined/wasteful per spec). */
        return true;
    }

    XrActiveActionSet active = {0};
    active.actionSet = actions->actionSet;
    active.subactionPath = XR_NULL_PATH;
    XrActionsSyncInfo sync = {0};
    sync.type = XR_TYPE_ACTIONS_SYNC_INFO;
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    XrResult result = xrSyncActions(xr->session, &sync);
    if (result != XR_SUCCESS && result != XR_SESSION_NOT_FOCUSED) {
        if (!actions->loggedSyncFailure) {
            BZ_QUEST_LOGE("xrSyncActions failed: %d", (int)result);
            actions->loggedSyncFailure = true;
        }
        return false;
    }
    actions->loggedSyncFailure = false;

    for (int side = 0; side < BZ_QUEST_XR_SIDE_COUNT; ++side) {
        bzQuestInputHandSample_t *hand = &frame->hands[side];
        XrPath sub = actions->handPath[side];
        /* The aim pose action being active is our proxy for "controller
         * present this frame"; when inactive, everything stays zero/false. */
        hand->active = bz_quest_xr_pose_active(xr->session, actions->actions[BZ_QUEST_XR_ACTION_AIM_POSE],
                                               sub);
        if (!hand->active) continue;

        hand->aimValid = bz_quest_xr_locate_pose(xr, actions->aimSpace[side], displayTime, hand->aimOrigin,
                                                  hand->aimDir);
        bz_quest_xr_locate_pose(xr, actions->gripSpace[side], displayTime, hand->gripPos, NULL);

        bz_quest_xr_read_bool(xr->session, actions->actions[BZ_QUEST_XR_ACTION_SELECT], sub,
                              &hand->selectDown);
        bz_quest_xr_read_bool(xr->session, actions->actions[BZ_QUEST_XR_ACTION_SQUEEZE], sub,
                              &hand->squeezeDown);
        bz_quest_xr_read_bool(xr->session, actions->actions[BZ_QUEST_XR_ACTION_PRIMARY], sub,
                              &hand->primaryDown);
        bz_quest_xr_read_bool(xr->session, actions->actions[BZ_QUEST_XR_ACTION_SECONDARY], sub,
                              &hand->secondaryDown);
        bz_quest_xr_read_vector2f(xr->session, actions->actions[BZ_QUEST_XR_ACTION_THUMBSTICK], sub,
                                  hand->thumbstick);
    }

    /* MENU is left-only. */
    bz_quest_xr_read_bool(xr->session, actions->actions[BZ_QUEST_XR_ACTION_MENU],
                          actions->handPath[BZ_QUEST_XR_SIDE_LEFT], &frame->menuDown);
    return true;
}

void bz_quest_xr_actions_apply_haptic(bzQuestXr_t *xr, bzQuestXrActions_t *actions, uint8_t hand,
                                      const bzQuestHapticPulse_t *pulse) {
    if (hand >= BZ_QUEST_XR_SIDE_COUNT || !actions->attached) return;
    XrHapticVibration vibration = {0};
    vibration.type = XR_TYPE_HAPTIC_VIBRATION;
    vibration.amplitude = pulse->amplitude;
    vibration.duration = (XrDuration)pulse->durationNanos;
    vibration.frequency = pulse->frequency > 0.0f ? pulse->frequency : XR_FREQUENCY_UNSPECIFIED;

    XrHapticActionInfo info = {0};
    info.type = XR_TYPE_HAPTIC_ACTION_INFO;
    info.action = actions->actions[BZ_QUEST_XR_ACTION_HAPTIC];
    info.subactionPath = actions->handPath[hand];
    XrResult result =
        xrApplyHapticFeedback(xr->session, &info, (const XrHapticBaseHeader *)&vibration);
    if (result != XR_SUCCESS && !actions->loggedHapticFailure) {
        BZ_QUEST_LOGE("xrApplyHapticFeedback(hand=%u) failed: %d", (unsigned)hand, (int)result);
        actions->loggedHapticFailure = true;
    }
}

void bz_quest_xr_actions_destroy(bzQuestXr_t *xr, bzQuestXrActions_t *actions) {
    (void)xr;
    for (int side = 0; side < BZ_QUEST_XR_SIDE_COUNT; ++side) {
        if (actions->aimSpace[side] != XR_NULL_HANDLE) {
            xrDestroySpace(actions->aimSpace[side]);
            actions->aimSpace[side] = XR_NULL_HANDLE;
        }
        if (actions->gripSpace[side] != XR_NULL_HANDLE) {
            xrDestroySpace(actions->gripSpace[side]);
            actions->gripSpace[side] = XR_NULL_HANDLE;
        }
    }
    if (actions->actionSet != XR_NULL_HANDLE) {
        xrDestroyActionSet(actions->actionSet); /* destroys child actions */
        actions->actionSet = XR_NULL_HANDLE;
    }
    actions->attached = false;
}
