/*
 * bz_quest_xr_bindings.h - layer 6: the PURE, host-testable interaction-
 * profile binding tables for the Quest Touch action set. Kept separate from
 * the impure bz_quest_xr_actions.c (which owns the real XrAction/XrPath
 * handles and calls xrSuggestInteractionProfileBindings) for the exact same
 * reason bz_quest_input_state.h is split from bz_quest_xr_actions.c: the
 * choice of WHICH component path a semantic action binds to on a given
 * interaction profile - and the validation that those path strings obey
 * OpenXR's path syntax and never bind a reserved/system control - is pure
 * data + logic that a plain host compiler can test
 * (tests/test_bz_quest_xr_bindings.c), with no OpenXR loader present. The
 * impure module just walks these tables, prepends "/user/hand/<side>", and
 * hands the assembled strings to xrStringToPath.
 *
 * No <openxr/openxr.h> type appears here - only plain C.
 */
#ifndef BZ_QUEST_XR_BINDINGS_H
#define BZ_QUEST_XR_BINDINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The two interaction profiles this layer suggests bindings for: the full
 * Meta Touch controller and the universally-guaranteed simple controller
 * fallback (select/menu click + aim/grip pose + haptic only). */
typedef enum {
    BZ_QUEST_XR_PROFILE_TOUCH = 0,
    BZ_QUEST_XR_PROFILE_SIMPLE,
    BZ_QUEST_XR_PROFILE_COUNT,
} bzQuestXrProfile_t;

/* Semantic actions, hand-agnostic (each pose/button/haptic action is created
 * once with two subaction paths - left+right - except MENU, which Meta Touch
 * exposes on the LEFT controller only; the right "menu" equivalent is the
 * reserved Oculus/system button, which this layer MUST NOT bind). */
typedef enum {
    BZ_QUEST_XR_ACTION_AIM_POSE = 0,
    BZ_QUEST_XR_ACTION_GRIP_POSE,
    BZ_QUEST_XR_ACTION_SELECT,          /* trigger (boolean action bound to a float source w/ default threshold) */
    BZ_QUEST_XR_ACTION_SQUEEZE,         /* grip/grab */
    BZ_QUEST_XR_ACTION_THUMBSTICK,      /* Vector2f */
    BZ_QUEST_XR_ACTION_THUMBSTICK_CLICK,
    BZ_QUEST_XR_ACTION_PRIMARY,         /* A (right) / X (left) */
    BZ_QUEST_XR_ACTION_SECONDARY,       /* B (right) / Y (left) - also the cancel binding */
    BZ_QUEST_XR_ACTION_MENU,            /* left menu click only */
    BZ_QUEST_XR_ACTION_HAPTIC,          /* output */
    BZ_QUEST_XR_ACTION_COUNT,
} bzQuestXrActionId_t;

typedef enum {
    BZ_QUEST_XR_SIDE_LEFT = 0,
    BZ_QUEST_XR_SIDE_RIGHT = 1,
    BZ_QUEST_XR_SIDE_COUNT = 2,
} bzQuestXrSide_t;

typedef enum {
    BZ_QUEST_XR_ACTION_TYPE_POSE = 0,
    BZ_QUEST_XR_ACTION_TYPE_BOOLEAN,
    BZ_QUEST_XR_ACTION_TYPE_FLOAT,
    BZ_QUEST_XR_ACTION_TYPE_VECTOR2F,
    BZ_QUEST_XR_ACTION_TYPE_VIBRATION,
} bzQuestXrActionType_t;

/* Full interaction-profile path string, e.g.
 * "/interaction_profiles/oculus/touch_controller". */
const char *bz_quest_xr_profile_path(bzQuestXrProfile_t profile);

/* OpenXR action name (xrCreateAction) - lowercase, no spaces, path-legal. */
const char *bz_quest_xr_action_name(bzQuestXrActionId_t action);

/* Human-readable localized name (xrCreateAction localizedActionName). */
const char *bz_quest_xr_action_localized(bzQuestXrActionId_t action);

bzQuestXrActionType_t bz_quest_xr_action_type(bzQuestXrActionId_t action);

/* Whether this action is created on the LEFT hand only (MENU) rather than
 * both hands. */
bool bz_quest_xr_action_left_only(bzQuestXrActionId_t action);

/*
 * Returns the component sub-path (the part AFTER "/user/hand/<side>") this
 * `action` binds to on `profile` for `side`, or NULL if the action has no
 * binding on that profile/side (e.g. everything but select/menu/aim/grip/
 * haptic on the simple controller, or a right-hand MENU on any profile).
 * Example return: "input/trigger/value", "input/a/click", "output/haptic".
 * The impure module prepends "/user/hand/left" or "/user/hand/right".
 */
const char *bz_quest_xr_binding_component(bzQuestXrProfile_t profile, bzQuestXrActionId_t action,
                                          bzQuestXrSide_t side);

/*
 * Validates a component/path token against OpenXR's path syntax the way the
 * runtime's xrStringToPath would (lowercase a-z, digits, '-', '.', '_', and
 * '/' separators; no leading/trailing/double slash; non-empty). Used by the
 * structural test to prove no table entry is malformed. Returns true if ok.
 */
bool bz_quest_xr_path_token_valid(const char *token);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_XR_BINDINGS_H */
