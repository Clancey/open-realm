/*
 * bz_quest_xr_bindings.c - see bz_quest_xr_bindings.h. Pure tables + a small
 * path-syntax validator. No OpenXR/engine link.
 */
#include "bz_quest_xr_bindings.h"

#include <string.h>

const char *bz_quest_xr_profile_path(bzQuestXrProfile_t profile) {
    switch (profile) {
    case BZ_QUEST_XR_PROFILE_TOUCH: return "/interaction_profiles/oculus/touch_controller";
    case BZ_QUEST_XR_PROFILE_SIMPLE: return "/interaction_profiles/khr/simple_controller";
    default: return NULL;
    }
}

/* Per-action metadata: name (path-legal), localized label, type, left-only. */
typedef struct {
    const char *name;
    const char *localized;
    bzQuestXrActionType_t type;
    bool leftOnly;
} bzQuestXrActionMeta_t;

static const bzQuestXrActionMeta_t kActionMeta[BZ_QUEST_XR_ACTION_COUNT] = {
    [BZ_QUEST_XR_ACTION_AIM_POSE] = {"aim_pose", "Aim Pose", BZ_QUEST_XR_ACTION_TYPE_POSE, false},
    [BZ_QUEST_XR_ACTION_GRIP_POSE] = {"grip_pose", "Grip Pose", BZ_QUEST_XR_ACTION_TYPE_POSE, false},
    [BZ_QUEST_XR_ACTION_SELECT] = {"select", "Select", BZ_QUEST_XR_ACTION_TYPE_BOOLEAN, false},
    [BZ_QUEST_XR_ACTION_SQUEEZE] = {"squeeze", "Squeeze", BZ_QUEST_XR_ACTION_TYPE_BOOLEAN, false},
    [BZ_QUEST_XR_ACTION_THUMBSTICK] = {"thumbstick", "Thumbstick", BZ_QUEST_XR_ACTION_TYPE_VECTOR2F, false},
    [BZ_QUEST_XR_ACTION_THUMBSTICK_CLICK] = {"thumbstick_click", "Thumbstick Click",
                                             BZ_QUEST_XR_ACTION_TYPE_BOOLEAN, false},
    [BZ_QUEST_XR_ACTION_PRIMARY] = {"primary_button", "Primary Button", BZ_QUEST_XR_ACTION_TYPE_BOOLEAN, false},
    [BZ_QUEST_XR_ACTION_SECONDARY] = {"secondary_button", "Secondary Button",
                                      BZ_QUEST_XR_ACTION_TYPE_BOOLEAN, false},
    [BZ_QUEST_XR_ACTION_MENU] = {"menu", "Menu", BZ_QUEST_XR_ACTION_TYPE_BOOLEAN, true},
    [BZ_QUEST_XR_ACTION_HAPTIC] = {"haptic", "Haptic Feedback", BZ_QUEST_XR_ACTION_TYPE_VIBRATION, false},
};

const char *bz_quest_xr_action_name(bzQuestXrActionId_t action) {
    return (action < BZ_QUEST_XR_ACTION_COUNT) ? kActionMeta[action].name : NULL;
}
const char *bz_quest_xr_action_localized(bzQuestXrActionId_t action) {
    return (action < BZ_QUEST_XR_ACTION_COUNT) ? kActionMeta[action].localized : NULL;
}
bzQuestXrActionType_t bz_quest_xr_action_type(bzQuestXrActionId_t action) {
    return kActionMeta[action].type;
}
bool bz_quest_xr_action_left_only(bzQuestXrActionId_t action) {
    return (action < BZ_QUEST_XR_ACTION_COUNT) && kActionMeta[action].leftOnly;
}

/* Touch controller component paths, per action, per side (L,R). NULL = unbound
 * on that side. A/X and B/Y differ by hand; menu is left-only; the reserved
 * Oculus/system button is deliberately absent - NEVER bound. */
static const char *const kTouchComponent[BZ_QUEST_XR_ACTION_COUNT][BZ_QUEST_XR_SIDE_COUNT] = {
    [BZ_QUEST_XR_ACTION_AIM_POSE] = {"input/aim/pose", "input/aim/pose"},
    [BZ_QUEST_XR_ACTION_GRIP_POSE] = {"input/grip/pose", "input/grip/pose"},
    [BZ_QUEST_XR_ACTION_SELECT] = {"input/trigger/value", "input/trigger/value"},
    [BZ_QUEST_XR_ACTION_SQUEEZE] = {"input/squeeze/value", "input/squeeze/value"},
    [BZ_QUEST_XR_ACTION_THUMBSTICK] = {"input/thumbstick", "input/thumbstick"},
    [BZ_QUEST_XR_ACTION_THUMBSTICK_CLICK] = {"input/thumbstick/click", "input/thumbstick/click"},
    /* left X, right A */
    [BZ_QUEST_XR_ACTION_PRIMARY] = {"input/x/click", "input/a/click"},
    /* left Y, right B */
    [BZ_QUEST_XR_ACTION_SECONDARY] = {"input/y/click", "input/b/click"},
    [BZ_QUEST_XR_ACTION_MENU] = {"input/menu/click", NULL},
    [BZ_QUEST_XR_ACTION_HAPTIC] = {"output/haptic", "output/haptic"},
};

/* Simple controller only guarantees select/menu click, aim/grip pose, and
 * haptic - everything else is unbound (the state machine degrades: no
 * thumbstick pan/rotate/zoom, no squeeze smart-order, cancel falls back to
 * the on-panel HUD cancel region reached via the one select click). */
static const char *const kSimpleComponent[BZ_QUEST_XR_ACTION_COUNT][BZ_QUEST_XR_SIDE_COUNT] = {
    [BZ_QUEST_XR_ACTION_AIM_POSE] = {"input/aim/pose", "input/aim/pose"},
    [BZ_QUEST_XR_ACTION_GRIP_POSE] = {"input/grip/pose", "input/grip/pose"},
    [BZ_QUEST_XR_ACTION_SELECT] = {"input/select/click", "input/select/click"},
    [BZ_QUEST_XR_ACTION_MENU] = {"input/menu/click", NULL},
    [BZ_QUEST_XR_ACTION_HAPTIC] = {"output/haptic", "output/haptic"},
    /* all others implicitly NULL */
};

const char *bz_quest_xr_binding_component(bzQuestXrProfile_t profile, bzQuestXrActionId_t action,
                                          bzQuestXrSide_t side) {
    if (action >= BZ_QUEST_XR_ACTION_COUNT || side >= BZ_QUEST_XR_SIDE_COUNT) return NULL;
    switch (profile) {
    case BZ_QUEST_XR_PROFILE_TOUCH: return kTouchComponent[action][side];
    case BZ_QUEST_XR_PROFILE_SIMPLE: return kSimpleComponent[action][side];
    default: return NULL;
    }
}

bool bz_quest_xr_path_token_valid(const char *token) {
    if (!token || !*token) return false;
    size_t n = strlen(token);
    if (token[0] == '/' || token[n - 1] == '/') return false; /* no leading/trailing slash */
    for (size_t i = 0; i < n; ++i) {
        char c = token[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
                  c == '/';
        if (!ok) return false;
        if (c == '/' && i > 0 && token[i - 1] == '/') return false; /* no double slash */
    }
    return true;
}
