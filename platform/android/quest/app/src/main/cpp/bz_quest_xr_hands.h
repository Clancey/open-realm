/*
 * bz_quest_xr_hands.h - layer 8: the IMPURE OpenXR hand-tracking driver.
 * Owns XR_EXT_hand_tracking's XrHandTrackerEXT handles (one per hand) and,
 * when the runtime also negotiated XR_FB_hand_tracking_aim, chains
 * XrHandTrackingAimStateFB onto the same xrLocateHandJointsEXT call to read
 * Meta's standardized aim pose + pinch bits. This is the ONLY place
 * XrHandTrackerEXT/XrHandJointLocationEXT/XrHandTrackingAimStateFB appear -
 * the actual pinch/aim GESTURE decision is pure
 * (bz_quest_hand_input.h/bz_quest_hand_sample_build()), which this module
 * only feeds plain POD unpacked from these OpenXR structs.
 *
 * Mirrors bz_quest_xr_actions.c's discipline exactly: bz_quest_xr_hands_create()
 * returns bool and logs its own failure via BZ_QUEST_LOGE; teardown is
 * reverse-order and XR_NULL_HANDLE-guarded. UNLIKE bz_quest_xr_actions.c,
 * hand tracking is entirely OPTIONAL - bz_quest_xr_hands_create() never
 * fails the caller (always returns true) when hand tracking is unsupported,
 * degraded, or a per-hand tracker fails to create: Touch controllers must
 * keep working regardless (see docs/quest-tabletop.md's "hand support must
 * not be required for startup"). bz_quest_xr_hands_sync() mirrors
 * bz_quest_xr_actions_sync()'s exact focus/session-running gate (never
 * calls xrLocateHandJointsEXT while unfocused) and writes its result
 * straight into the SAME bzQuestInputFrame_t the action module already
 * populates (frame->handSample[]), so bz_quest_renderer.c calls both sync
 * functions back-to-back into one shared frame - see bz_quest_renderer.c.
 */
#ifndef BZ_QUEST_XR_HANDS_H
#define BZ_QUEST_XR_HANDS_H

#include <stdbool.h>

#include "bz_quest_hand_input.h"
#include "bz_quest_input_state.h"
#include "bz_quest_xr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bzQuestXrHands_s {
    PFN_xrCreateHandTrackerEXT createHandTrackerEXT;
    PFN_xrDestroyHandTrackerEXT destroyHandTrackerEXT;
    PFN_xrLocateHandJointsEXT locateHandJointsEXT;
    XrHandTrackerEXT tracker[BZ_QUEST_INPUT_HAND_COUNT]; /* XR_NULL_HANDLE if unavailable for that hand */
    /* EXT-only pinch-distance hysteresis latch, persisted here (Quest-local
     * sensor-fusion state - see bz_quest_hand_input.h's bzQuestHandPinchState_t
     * comment for why this is NOT part of bzQuestInputState_t). */
    bzQuestHandPinchState_t pinchState[BZ_QUEST_INPUT_HAND_COUNT];
    bool proceduresResolved;
    /* Log-once latches so a persistent runtime error never spams logcat per
     * frame (AGENTS.md / docs/quest-tabletop.md "no per-frame logging"). */
    bool loggedLocateFailure[BZ_QUEST_INPUT_HAND_COUNT];
} bzQuestXrHands_t;

/*
 * Optional, NEVER-fatal creation step: no-ops (returns true, every tracker
 * left XR_NULL_HANDLE) when !xr->handCapability.supported - see
 * bz_quest_xr.h's bz_quest_xr_create_instance()/_get_system() for how that
 * capability was negotiated. When supported, resolves the three
 * XR_EXT_hand_tracking function pointers via xrGetInstanceProcAddr and
 * creates one XrHandTrackerEXT per hand (XR_HAND_JOINT_SET_DEFAULT_EXT, the
 * 26-joint default set). Must be called AFTER bz_quest_xr_create_session()
 * (xrCreateHandTrackerEXT needs a session) - mirrors
 * bz_quest_xr_actions_create()'s ordering requirement. A per-hand
 * tracker-create failure disables only that hand (logged, tracker left
 * XR_NULL_HANDLE); a failed proc-pointer resolution disables hand tracking
 * entirely for this session (logged) - neither ever fails the whole
 * renderer init.
 */
bool bz_quest_xr_hands_create(bzQuestXr_t *xr, bzQuestXrHands_t *hands);

/*
 * One per-frame read, mirroring bz_quest_xr_actions_sync()'s exact focus/
 * session-running gate: when not running+focused, or hand tracking was
 * never created (proceduresResolved false), zeroes every
 * frame->handSample[] entry (inactive) and returns true without calling
 * xrLocateHandJointsEXT (never locate while unfocused - undefined/wasteful
 * per the OpenXR spec, same rationale as the action module). Otherwise, for
 * each hand whose tracker exists, calls xrLocateHandJointsEXT at
 * `displayTime` against `xr->appSpace` (the SAME predicted display time and
 * reference space bz_quest_xr_actions_sync() already uses for controller
 * poses), chains an XrHandTrackingAimStateFB when
 * xr->handCapability.aimSupported, unpacks the result into plain POD
 * (bz_quest_hand_input.h types - no OpenXR type escapes this function), and
 * calls bz_quest_hand_sample_build() to produce frame->handSample[hand].
 * Only the hand-tracking portion of `frame` is written here - the caller
 * still separately fills `frame->hands[]`/world/selection/dt (via
 * bz_quest_xr_actions_sync() and the interaction capture) exactly as
 * before. Returns false only on a hard, logged-once xrLocateHandJointsEXT
 * failure for a given hand (that hand's sample is simply left inactive -
 * never fatal to the caller, and the return value itself is informational
 * only, mirroring bz_quest_xr_actions_sync()'s own contract).
 */
bool bz_quest_xr_hands_sync(bzQuestXr_t *xr, bzQuestXrHands_t *hands, XrTime displayTime,
                            bzQuestInputFrame_t *frame);

/* Reverse-order, XR_NULL_HANDLE-guarded teardown of both hand trackers. Safe
 * on partial/never-created state (destroyHandTrackerEXT itself may be NULL
 * if procedure resolution never completed). */
void bz_quest_xr_hands_destroy(bzQuestXrHands_t *hands);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_XR_HANDS_H */
