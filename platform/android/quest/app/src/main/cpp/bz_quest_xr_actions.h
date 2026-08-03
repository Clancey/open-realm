/*
 * bz_quest_xr_actions.h - layer 6: the IMPURE OpenXR Touch-controller action
 * layer. Owns the one XrActionSet attached to bz_quest_xr.h's session, every
 * semantic XrAction (aim/grip pose, select, squeeze, thumbstick +click,
 * primary/secondary buttons, left menu, haptic output), the per-hand aim/grip
 * XrSpaces, and the two suggested interaction-profile binding sets (Meta Touch
 * + the simple-controller fallback). This is the ONLY place XrAction/XrSpace/
 * XrPath appear for layer 6 - the binding TABLES (which path each semantic
 * action maps to) live in the pure, host-tested bz_quest_xr_bindings.h, and
 * the state machine / hit priority / command mapping live in the pure, host-
 * tested bz_quest_input_state.h. This module just: (1) at session-create time,
 * creates the action set/actions/spaces and suggests both binding tables, then
 * attaches; (2) once per frame, xrSyncActions + xrGetActionState* +
 * xrLocateSpace and unpacks the results into a plain bzQuestInputFrame_t the
 * pure update consumes; (3) applies xrApplyHapticFeedback pulses the pure
 * module decides. Mirrors bz_quest_xr.c's discipline exactly: every function
 * returns bool and logs its own failure via BZ_QUEST_LOGE before returning
 * false; teardown is reverse-order and XR_NULL_HANDLE-guarded.
 */
#ifndef BZ_QUEST_XR_ACTIONS_H
#define BZ_QUEST_XR_ACTIONS_H

#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_input_state.h"
#include "bz_quest_xr.h"
#include "bz_quest_xr_bindings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bzQuestXrActions_s {
    XrActionSet actionSet;
    /* One XrAction per semantic action. Pose/button/analog actions are
     * created with both hands' subaction paths; MENU is left-only (see
     * bz_quest_xr_bindings.h - the right "menu" is the reserved system
     * button, never bound). */
    XrAction actions[BZ_QUEST_XR_ACTION_COUNT];
    XrPath handPath[BZ_QUEST_XR_SIDE_COUNT]; /* /user/hand/left, /user/hand/right */
    XrSpace aimSpace[BZ_QUEST_XR_SIDE_COUNT];
    XrSpace gripSpace[BZ_QUEST_XR_SIDE_COUNT];
    bool attached;
    /* Log-once latches so a persistent runtime error (e.g. a haptic call that
     * keeps failing) never spams logcat per frame (AGENTS.md / docs/quest-
     * tabletop.md "no per-frame logging"). */
    bool loggedSyncFailure;
    bool loggedHapticFailure;
} bzQuestXrActions_t;

/*
 * Creates the action set + every action, suggests BOTH interaction-profile
 * binding tables (Meta Touch and the simple-controller fallback), creates the
 * per-hand aim/grip action spaces, and attaches the action set to the session.
 * Must be called AFTER bz_quest_xr_create_session()/_create_space() and BEFORE
 * the first bz_quest_xr_actions_sync(). Hard-fails (logs + returns false, with
 * any partial state left for bz_quest_xr_actions_destroy() to clean up) on the
 * first failed OpenXR call - there is no partial-success state.
 */
bool bz_quest_xr_actions_create(bzQuestXr_t *xr, bzQuestXrActions_t *actions);

/*
 * One per-frame action read. When the session is running AND focused, calls
 * xrSyncActions once, then xrGetActionStateXxx/xrLocateSpace for every action at
 * `displayTime` (the same predicted display time used to locate the eye
 * views), unpacking into `frame->hands[]`, `frame->menuDown`, `frame->focused`.
 * When not focused it leaves every hand inactive and focused=false (so the
 * pure state machine idempotently clears its transient state) and does NOT
 * call xrSyncActions (undefined/wasteful while unfocused per the spec). Only
 * the controller-sample portion of `frame` is written here; the caller fills
 * `frame->world`, `selectedIds`, `mapEpoch`, and `dt`. Returns false only on a
 * hard OpenXR error (logged once).
 */
bool bz_quest_xr_actions_sync(bzQuestXr_t *xr, bzQuestXrActions_t *actions, XrTime displayTime,
                              bzQuestInputFrame_t *frame);

/* Applies one haptic pulse to `hand`'s controller (BZ_QUEST_INPUT_HAND_LEFT/
 * RIGHT). Logs once on failure; never fatal. */
void bz_quest_xr_actions_apply_haptic(bzQuestXr_t *xr, bzQuestXrActions_t *actions, uint8_t hand,
                                      const bzQuestHapticPulse_t *pulse);

/* Reverse-order, XR_NULL_HANDLE-guarded teardown: aim/grip spaces then the
 * action set (which destroys its child actions). Safe on partial state. */
void bz_quest_xr_actions_destroy(bzQuestXr_t *xr, bzQuestXrActions_t *actions);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_XR_ACTIONS_H */
