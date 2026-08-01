/*
 * test_bz_quest_xr_bindings.c - layer 6 structural test for the pure OpenXR
 * interaction-profile binding tables. Proves (with no OpenXR runtime present)
 * that every table entry obeys OpenXR path syntax, that the two profiles do
 * not silently diverge on the shared pose/haptic actions, that the simple-
 * controller fallback is a strict subset, that no reserved/system control is
 * bound, and that action name strings are path-legal.
 */
#include "test_framework.h"

#include "bz_quest_xr_bindings.h"

#include <string.h>

/* full profile path strings are correct and distinct */
static void test_profile_paths(void) {
    ASSERT_STR_EQ(bz_quest_xr_profile_path(BZ_QUEST_XR_PROFILE_TOUCH),
                  "/interaction_profiles/oculus/touch_controller");
    ASSERT_STR_EQ(bz_quest_xr_profile_path(BZ_QUEST_XR_PROFILE_SIMPLE),
                  "/interaction_profiles/khr/simple_controller");
    ASSERT(bz_quest_xr_profile_path(BZ_QUEST_XR_PROFILE_COUNT) == NULL);
}

/* every action name is lowercase / path-legal (OpenXR xrCreateAction rule) */
static void test_action_names_path_legal(void) {
    for (int a = 0; a < BZ_QUEST_XR_ACTION_COUNT; ++a) {
        const char *name = bz_quest_xr_action_name((bzQuestXrActionId_t)a);
        ASSERT(name != NULL);
        ASSERT(name[0] != '\0');
        ASSERT(bz_quest_xr_path_token_valid(name));
        ASSERT(bz_quest_xr_action_localized((bzQuestXrActionId_t)a) != NULL);
    }
}

/* only MENU is left-only; it must be boolean; poses are POSE; haptic is vibration */
static void test_action_metadata(void) {
    for (int a = 0; a < BZ_QUEST_XR_ACTION_COUNT; ++a) {
        bool leftOnly = bz_quest_xr_action_left_only((bzQuestXrActionId_t)a);
        if (a == BZ_QUEST_XR_ACTION_MENU) ASSERT(leftOnly);
        else ASSERT(!leftOnly);
    }
    ASSERT_EQ_INT(bz_quest_xr_action_type(BZ_QUEST_XR_ACTION_AIM_POSE), BZ_QUEST_XR_ACTION_TYPE_POSE);
    ASSERT_EQ_INT(bz_quest_xr_action_type(BZ_QUEST_XR_ACTION_GRIP_POSE), BZ_QUEST_XR_ACTION_TYPE_POSE);
    ASSERT_EQ_INT(bz_quest_xr_action_type(BZ_QUEST_XR_ACTION_THUMBSTICK), BZ_QUEST_XR_ACTION_TYPE_VECTOR2F);
    ASSERT_EQ_INT(bz_quest_xr_action_type(BZ_QUEST_XR_ACTION_HAPTIC), BZ_QUEST_XR_ACTION_TYPE_VIBRATION);
    ASSERT_EQ_INT(bz_quest_xr_action_type(BZ_QUEST_XR_ACTION_SELECT), BZ_QUEST_XR_ACTION_TYPE_BOOLEAN);
}

/* every non-NULL binding component is a legal OpenXR path token */
static void test_all_components_path_legal(void) {
    for (int p = 0; p < BZ_QUEST_XR_PROFILE_COUNT; ++p) {
        for (int a = 0; a < BZ_QUEST_XR_ACTION_COUNT; ++a) {
            for (int s = 0; s < BZ_QUEST_XR_SIDE_COUNT; ++s) {
                const char *c = bz_quest_xr_binding_component((bzQuestXrProfile_t)p, (bzQuestXrActionId_t)a,
                                                              (bzQuestXrSide_t)s);
                if (c) ASSERT(bz_quest_xr_path_token_valid(c));
            }
        }
    }
}

/* no table entry ever binds the reserved Oculus/system button or the "system"
 * component - a hard OpenXR/Meta rule */
static void test_no_reserved_system_binding(void) {
    for (int p = 0; p < BZ_QUEST_XR_PROFILE_COUNT; ++p) {
        for (int a = 0; a < BZ_QUEST_XR_ACTION_COUNT; ++a) {
            for (int s = 0; s < BZ_QUEST_XR_SIDE_COUNT; ++s) {
                const char *c = bz_quest_xr_binding_component((bzQuestXrProfile_t)p, (bzQuestXrActionId_t)a,
                                                              (bzQuestXrSide_t)s);
                if (!c) continue;
                ASSERT(strstr(c, "system") == NULL);
                ASSERT(strstr(c, "oculus") == NULL);
            }
        }
    }
}

/* MENU is bound on the LEFT hand only, never the right (the right "menu"
 * equivalent is the reserved system button) - on BOTH profiles */
static void test_menu_left_only_binding(void) {
    for (int p = 0; p < BZ_QUEST_XR_PROFILE_COUNT; ++p) {
        ASSERT(bz_quest_xr_binding_component((bzQuestXrProfile_t)p, BZ_QUEST_XR_ACTION_MENU,
                                             BZ_QUEST_XR_SIDE_LEFT) != NULL);
        ASSERT(bz_quest_xr_binding_component((bzQuestXrProfile_t)p, BZ_QUEST_XR_ACTION_MENU,
                                             BZ_QUEST_XR_SIDE_RIGHT) == NULL);
    }
}

/* both profiles define the SAME aim/grip pose + haptic on BOTH hands - the
 * fallback must not silently drop the pose/haptic semantics the whole layer
 * depends on */
static void test_shared_pose_haptic_on_both_profiles(void) {
    const bzQuestXrActionId_t shared[] = {BZ_QUEST_XR_ACTION_AIM_POSE, BZ_QUEST_XR_ACTION_GRIP_POSE,
                                          BZ_QUEST_XR_ACTION_HAPTIC};
    for (size_t i = 0; i < sizeof(shared) / sizeof(shared[0]); ++i) {
        for (int s = 0; s < BZ_QUEST_XR_SIDE_COUNT; ++s) {
            const char *t = bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_TOUCH, shared[i],
                                                          (bzQuestXrSide_t)s);
            const char *m = bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_SIMPLE, shared[i],
                                                          (bzQuestXrSide_t)s);
            ASSERT(t != NULL);
            ASSERT(m != NULL);
            ASSERT_STR_EQ(t, m); /* identical component path on both profiles */
        }
    }
}

/* simple controller is a strict subset: any action it binds is also bound on
 * touch, and it binds ONLY select/menu/aim/grip/haptic */
static void test_simple_is_strict_subset(void) {
    for (int a = 0; a < BZ_QUEST_XR_ACTION_COUNT; ++a) {
        for (int s = 0; s < BZ_QUEST_XR_SIDE_COUNT; ++s) {
            const char *simple = bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_SIMPLE,
                                                               (bzQuestXrActionId_t)a, (bzQuestXrSide_t)s);
            if (!simple) continue;
            const char *touch = bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_TOUCH,
                                                              (bzQuestXrActionId_t)a, (bzQuestXrSide_t)s);
            ASSERT(touch != NULL); /* nothing bound on simple that touch lacks */
            bool allowed = a == BZ_QUEST_XR_ACTION_AIM_POSE || a == BZ_QUEST_XR_ACTION_GRIP_POSE ||
                           a == BZ_QUEST_XR_ACTION_SELECT || a == BZ_QUEST_XR_ACTION_MENU ||
                           a == BZ_QUEST_XR_ACTION_HAPTIC;
            ASSERT(allowed);
        }
    }
}

/* touch binds thumbstick/squeeze/primary/secondary that simple does NOT (the
 * degraded-fallback contract) */
static void test_touch_has_extended_bindings(void) {
    const bzQuestXrActionId_t extended[] = {BZ_QUEST_XR_ACTION_SQUEEZE, BZ_QUEST_XR_ACTION_THUMBSTICK,
                                            BZ_QUEST_XR_ACTION_THUMBSTICK_CLICK, BZ_QUEST_XR_ACTION_PRIMARY,
                                            BZ_QUEST_XR_ACTION_SECONDARY};
    for (size_t i = 0; i < sizeof(extended) / sizeof(extended[0]); ++i) {
        ASSERT(bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_TOUCH, extended[i], BZ_QUEST_XR_SIDE_RIGHT) !=
               NULL);
        ASSERT(bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_SIMPLE, extended[i], BZ_QUEST_XR_SIDE_RIGHT) ==
               NULL);
    }
}

/* A/X and B/Y really differ by hand on touch (per-hand component paths) */
static void test_face_buttons_differ_by_hand(void) {
    const char *xL = bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_TOUCH, BZ_QUEST_XR_ACTION_PRIMARY,
                                                   BZ_QUEST_XR_SIDE_LEFT);
    const char *aR = bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_TOUCH, BZ_QUEST_XR_ACTION_PRIMARY,
                                                   BZ_QUEST_XR_SIDE_RIGHT);
    ASSERT_STR_EQ(xL, "input/x/click");
    ASSERT_STR_EQ(aR, "input/a/click");
    const char *yL = bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_TOUCH, BZ_QUEST_XR_ACTION_SECONDARY,
                                                   BZ_QUEST_XR_SIDE_LEFT);
    const char *bR = bz_quest_xr_binding_component(BZ_QUEST_XR_PROFILE_TOUCH, BZ_QUEST_XR_ACTION_SECONDARY,
                                                   BZ_QUEST_XR_SIDE_RIGHT);
    ASSERT_STR_EQ(yL, "input/y/click");
    ASSERT_STR_EQ(bR, "input/b/click");
}

/* the path-token validator rejects the malformed shapes it is meant to catch */
static void test_path_validator_rejects_bad(void) {
    ASSERT(!bz_quest_xr_path_token_valid(NULL));
    ASSERT(!bz_quest_xr_path_token_valid(""));
    ASSERT(!bz_quest_xr_path_token_valid("/input/trigger")); /* leading slash */
    ASSERT(!bz_quest_xr_path_token_valid("input/trigger/")); /* trailing slash */
    ASSERT(!bz_quest_xr_path_token_valid("input//trigger")); /* double slash */
    ASSERT(!bz_quest_xr_path_token_valid("input/Trigger"));  /* uppercase */
    ASSERT(!bz_quest_xr_path_token_valid("input trigger"));  /* space */
    ASSERT(bz_quest_xr_path_token_valid("input/trigger/value"));
    ASSERT(bz_quest_xr_path_token_valid("output/haptic"));
}

/* no duplicate binding component within a single (profile, side): two distinct
 * semantic actions must never map to the same physical control */
static void test_no_duplicate_component_per_side(void) {
    for (int p = 0; p < BZ_QUEST_XR_PROFILE_COUNT; ++p) {
        for (int s = 0; s < BZ_QUEST_XR_SIDE_COUNT; ++s) {
            for (int a = 0; a < BZ_QUEST_XR_ACTION_COUNT; ++a) {
                const char *ca = bz_quest_xr_binding_component((bzQuestXrProfile_t)p, (bzQuestXrActionId_t)a,
                                                               (bzQuestXrSide_t)s);
                if (!ca) continue;
                /* haptic is output, poses are pose - only compare within input space is unnecessary;
                 * a duplicate string on the same side is always a conflict */
                for (int b = a + 1; b < BZ_QUEST_XR_ACTION_COUNT; ++b) {
                    const char *cb = bz_quest_xr_binding_component((bzQuestXrProfile_t)p,
                                                                   (bzQuestXrActionId_t)b, (bzQuestXrSide_t)s);
                    if (!cb) continue;
                    ASSERT(strcmp(ca, cb) != 0);
                }
            }
        }
    }
}

void run_bz_quest_xr_bindings_tests(void) {
    RUN_TEST(test_profile_paths);
    RUN_TEST(test_action_names_path_legal);
    RUN_TEST(test_action_metadata);
    RUN_TEST(test_all_components_path_legal);
    RUN_TEST(test_no_reserved_system_binding);
    RUN_TEST(test_menu_left_only_binding);
    RUN_TEST(test_shared_pose_haptic_on_both_profiles);
    RUN_TEST(test_simple_is_strict_subset);
    RUN_TEST(test_touch_has_extended_bindings);
    RUN_TEST(test_face_buttons_differ_by_hand);
    RUN_TEST(test_path_validator_rejects_bad);
    RUN_TEST(test_no_duplicate_component_per_side);
}
