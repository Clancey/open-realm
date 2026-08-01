/*
 * test_bz_quest_wc3_hud.c - coverage for layer 5E's pure HUD layout/hit-
 * test contract (bz_quest_wc3_hud.h).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bz_quest_wc3_hud.h"
#include "test_framework.h"

static bzQuestHudButtonInput_t make_button(uint8_t gridX, uint8_t gridY, const char *code, bool hidden,
                                            bool disabled, bzQuestHudActionSemantic_t semantic) {
    bzQuestHudButtonInput_t b;
    memset(&b, 0, sizeof(b));
    b.gridX = gridX;
    b.gridY = gridY;
    strncpy(b.actionCode, code, sizeof(b.actionCode) - 1);
    strncpy(b.tooltip, code, sizeof(b.tooltip) - 1);
    b.hidden = hidden;
    b.disabled = disabled;
    b.semantic = semantic;
    b.target = BZ_QUEST_HUD_TARGET_NONE;
    return b;
}

static bzQuestHudInput_t make_input_with_player(void) {
    bzQuestHudInput_t in;
    memset(&in, 0, sizeof(in));
    in.player.present = true;
    strncpy(in.player.name, "Player1", sizeof(in.player.name) - 1);
    in.player.gold = 100;
    in.player.lumber = 50;
    in.player.foodUsed = 5;
    in.player.foodCap = 10;
    in.player.heroTokens = 1;
    in.selectedCount = 2;
    in.frameId = 42;
    return in;
}

/* --- Panel transform ---------------------------------------------------- */

static void test_panel_transform_is_deterministic_and_map_independent(void) {
    bzQuestHudPanelTransform_t a, b;
    bz_quest_wc3_hud_panel_transform(&a);
    bz_quest_wc3_hud_panel_transform(&b);
    ASSERT(memcmp(&a, &b, sizeof(a)) == 0);
    /* right/down must be orthogonal (dot == 0) and non-degenerate, or hit-testing's
     * un-inverted local-coordinate projection would be wrong. */
    float dot = a.rightX * a.downX + a.rightY * a.downY + a.rightZ * a.downZ;
    ASSERT(dot > -1e-5f && dot < 1e-5f);
    ASSERT(a.rightX * a.rightX + a.rightY * a.rightY + a.rightZ * a.rightZ > 0.0f);
    ASSERT(a.downX * a.downX + a.downY * a.downY + a.downZ * a.downZ > 0.0f);
}

/* --- No-selection / loading / error / status paths ---------------------- */

static void test_build_with_no_player_shows_loading_status_only(void) {
    bzQuestHudInput_t in;
    memset(&in, 0, sizeof(in));
    in.frameId = 7;
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.frameId, 7);
    ASSERT_EQ_INT((int)frame.quadCount, 1); /* status bg only, no command card */
    ASSERT_EQ_INT((int)frame.textCount, 1);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 0);
    ASSERT(strcmp(frame.texts[0].text, "No player data") == 0);
}

static void test_build_with_player_no_selection_shows_resources_and_zero_selected(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.selectedCount = 0;
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.quadCount, 1);
    ASSERT_EQ_INT((int)frame.textCount, 2);
    ASSERT(strstr(frame.texts[0].text, "Selected:0") != NULL);
    ASSERT(strstr(frame.texts[1].text, "Gold:100") != NULL);
    ASSERT(strstr(frame.texts[1].text, "Lumber:50") != NULL);
}

static void test_build_game_result_adds_third_status_line(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.player.gameResult = BZ_QUEST_HUD_GAME_RESULT_VICTORY;
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.textCount, 3);
    ASSERT(strcmp(frame.texts[2].text, "VICTORY") == 0);
}

/* --- Status/resource text capacity (PR #24 review defect 1) ------------- */

static void test_build_max_value_resources_render_complete_untruncated(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.player.gold = UINT32_MAX;
    in.player.lumber = UINT32_MAX;
    in.player.foodUsed = UINT32_MAX;
    in.player.foodCap = UINT32_MAX;
    in.player.heroTokens = UINT32_MAX;
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT(frame.statusTextTruncated == false);
    char expected[BZ_QUEST_HUD_MAX_STATUS_TEXT];
    snprintf(expected, sizeof(expected), "Gold:%u Lumber:%u Food:%u/%u Tokens:%u", UINT32_MAX, UINT32_MAX,
             UINT32_MAX, UINT32_MAX, UINT32_MAX);
    ASSERT(strcmp(frame.texts[1].text, expected) == 0);
    /* Each field must appear with its full value, never a shortened
     * prefix (e.g. "Gold:4" instead of "Gold:4294967295") - this is the
     * exact defect the review found: a too-small buffer silently changed
     * displayed numeric values. */
    ASSERT(strstr(frame.texts[1].text, "Gold:4294967295") != NULL);
    ASSERT(strstr(frame.texts[1].text, "Lumber:4294967295") != NULL);
    ASSERT(strstr(frame.texts[1].text, "Food:4294967295/4294967295") != NULL);
    ASSERT(strstr(frame.texts[1].text, "Tokens:4294967295") != NULL);
}

static void test_build_max_selected_count_and_long_name_render_complete(void) {
    bzQuestHudInput_t in = make_input_with_player();
    /* BZ_QUEST_HUD_MAX_NAME=32, so 31 visible chars is the longest name
     * bz_quest_wc3_hud_build() can ever see (already NUL-bounded upstream -
     * see bzQuestHudInput_t.player.name). */
    strncpy(in.player.name, "ThisIsAVeryLongPlayerNameXXXXXX", sizeof(in.player.name) - 1);
    in.player.name[sizeof(in.player.name) - 1] = '\0';
    in.selectedCount = UINT32_MAX;
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT(frame.statusTextTruncated == false);
    char expected[BZ_QUEST_HUD_MAX_STATUS_TEXT];
    snprintf(expected, sizeof(expected), "%s  Selected:%u", in.player.name, UINT32_MAX);
    ASSERT(strcmp(frame.texts[0].text, expected) == 0);
    ASSERT(strstr(frame.texts[0].text, "Selected:4294967295") != NULL);
    ASSERT(strstr(frame.texts[0].text, in.player.name) != NULL);
}

static void test_build_command_card_absent_when_not_present(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = false;
    in.actionLayout.visible = true;
    in.actionLayout.valid = true;
    in.actionLayout.numButtons = 1;
    in.actionLayout.buttons[0] = make_button(0, 0, "hpea", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.quadCount, 1); /* status only */
    ASSERT_EQ_INT((int)frame.hitRegionCount, 0);
}

static void test_build_command_card_absent_when_not_visible_or_not_valid(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = true;
    in.actionLayout.visible = false;
    in.actionLayout.valid = true;
    in.actionLayout.numButtons = 1;
    in.actionLayout.buttons[0] = make_button(0, 0, "hpea", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 0);

    in.actionLayout.visible = true;
    in.actionLayout.valid = false;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 0);
}

/* --- Slot ordering / visibility / enabled state -------------------------- */

static void test_build_sorts_buttons_row_major_by_grid_y_then_x(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.numButtons = 3;
    in.actionLayout.buttons[0] = make_button(0, 2, "third", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    in.actionLayout.buttons[1] = make_button(0, 0, "first", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    in.actionLayout.buttons[2] = make_button(0, 1, "second", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 3);
    ASSERT(strcmp(frame.hitRegions[0].action.actionCode, "first") == 0);
    ASSERT(strcmp(frame.hitRegions[1].action.actionCode, "second") == 0);
    ASSERT(strcmp(frame.hitRegions[2].action.actionCode, "third") == 0);
}

static void test_build_hidden_button_produces_no_quad_or_hit_region(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.numButtons = 2;
    in.actionLayout.buttons[0] = make_button(0, 0, "visible", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    in.actionLayout.buttons[1] = make_button(1, 0, "hidden", true, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 1);
    ASSERT(strcmp(frame.hitRegions[0].action.actionCode, "visible") == 0);
}

static void test_build_disabled_button_still_shown_but_not_enabled(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.numButtons = 1;
    in.actionLayout.buttons[0] = make_button(0, 0, "dis", false, true, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 1);
    ASSERT(!frame.hitRegions[0].enabled);
}

static void test_build_unsupported_semantic_treated_as_disabled(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.numButtons = 1;
    in.actionLayout.buttons[0] = make_button(0, 0, "unsup", false, false, BZ_QUEST_HUD_SEMANTIC_UNSUPPORTED);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 1);
    ASSERT(!frame.hitRegions[0].enabled);
}

static void test_build_overlapping_grid_slots_still_each_get_a_distinct_region(void) {
    /* Two buttons authored at the SAME grid coordinate (a malformed/edge-case
     * authoritative layout) must not crash or silently merge - each still
     * gets its own quad/hit region in stable array order. */
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.numButtons = 2;
    in.actionLayout.buttons[0] = make_button(0, 0, "a", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    in.actionLayout.buttons[1] = make_button(0, 0, "b", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 2);
    ASSERT(strcmp(frame.hitRegions[0].action.actionCode, "a") == 0);
    ASSERT(strcmp(frame.hitRegions[1].action.actionCode, "b") == 0);
}

/* --- Target / cancel region ---------------------------------------------- */

static void test_build_no_cancel_region_when_target_none(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.currentTarget = BZ_QUEST_HUD_TARGET_NONE;
    in.actionLayout.numButtons = 1;
    in.actionLayout.buttons[0] = make_button(0, 0, "b", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 1);
    for (uint32_t i = 0; i < frame.hitRegionCount; i++)
        ASSERT(frame.hitRegions[i].action.semantic != BZ_QUEST_HUD_SEMANTIC_CANCEL);
}

static void test_build_cancel_region_appears_when_target_active(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.currentTarget = BZ_QUEST_HUD_TARGET_POINT;
    in.actionLayout.numButtons = 1;
    in.actionLayout.buttons[0] = make_button(0, 0, "b", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 2);
    const bzQuestHudHitRegion_t *cancel = &frame.hitRegions[1];
    ASSERT_EQ_INT(cancel->action.semantic, BZ_QUEST_HUD_SEMANTIC_CANCEL);
    ASSERT(cancel->enabled);
}

static void test_build_cancel_region_present_even_with_zero_buttons(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.currentTarget = BZ_QUEST_HUD_TARGET_ENTITY;
    in.actionLayout.numButtons = 0;
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 1);
    ASSERT_EQ_INT(frame.hitRegions[0].action.semantic, BZ_QUEST_HUD_SEMANTIC_CANCEL);
}

/* --- Map reload / repeated-build independence ---------------------------- */

static void test_build_is_stateless_across_successive_different_calls(void) {
    bzQuestHudInput_t first = make_input_with_player();
    first.actionLayout.present = first.actionLayout.visible = first.actionLayout.valid = true;
    first.actionLayout.numButtons = 4;
    for (int i = 0; i < 4; i++)
        first.actionLayout.buttons[i] = make_button((uint8_t)i, 0, "a", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frameA;
    bz_quest_wc3_hud_build(&first, &frameA);
    ASSERT_EQ_INT((int)frameA.hitRegionCount, 4);

    bzQuestHudInput_t second = make_input_with_player();
    second.frameId = 999; /* simulates a map reload: fresh generation, different shape */
    second.actionLayout.present = second.actionLayout.visible = second.actionLayout.valid = true;
    second.actionLayout.numButtons = 1;
    second.actionLayout.buttons[0] = make_button(0, 0, "solo", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frameB;
    bz_quest_wc3_hud_build(&second, &frameB);
    ASSERT_EQ_INT((int)frameB.hitRegionCount, 1);
    ASSERT_EQ_INT((int)frameB.frameId, 999);
    /* frameA must be untouched by building frameB - proves no hidden shared state. */
    ASSERT_EQ_INT((int)frameA.hitRegionCount, 4);
}

/* --- Hit-test: boundary inclusion, stale frames, misses ------------------- */

static void hit_test_setup(bzQuestHudFrame_t *frame) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.numButtons = 1;
    in.actionLayout.buttons[0] = make_button(0, 0, "hpea", false, false, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bz_quest_wc3_hud_build(&in, frame);
}

static void test_hit_test_rejects_stale_frame_id(void) {
    bzQuestHudFrame_t frame;
    hit_test_setup(&frame);
    bzQuestHudActionId_t action;
    /* Ray straight from in front of the panel toward it along -normal. */
    float rx = frame.panel.originX - frame.panel.normalX * 5.0f;
    float ry = frame.panel.originY - frame.panel.normalY * 5.0f;
    float rz = frame.panel.originZ - frame.panel.normalZ * 5.0f;
    ASSERT(!bz_quest_wc3_hud_hit_test(&frame, frame.frameId + 1, rx, ry, rz, frame.panel.normalX,
                                      frame.panel.normalY, frame.panel.normalZ, &action));
}

static void test_hit_test_hits_button_center(void) {
    bzQuestHudFrame_t frame;
    hit_test_setup(&frame);
    ASSERT_EQ_INT((int)frame.hitRegionCount, 1);
    const bzQuestHudHitRegion_t *hr = &frame.hitRegions[0];
    float lx = hr->x + hr->w * 0.5f, ly = hr->y + hr->h * 0.5f;
    const bzQuestHudPanelTransform_t *p = &frame.panel;
    float worldX = p->originX + lx * p->rightX + ly * p->downX;
    float worldY = p->originY + lx * p->rightY + ly * p->downY;
    float worldZ = p->originZ + lx * p->rightZ + ly * p->downZ;
    /* Ray starts 10 units back along the normal and points straight at the plane. */
    float rx = worldX + p->normalX * 10.0f, ry = worldY + p->normalY * 10.0f, rz = worldZ + p->normalZ * 10.0f;
    bzQuestHudActionId_t action;
    ASSERT(bz_quest_wc3_hud_hit_test(&frame, frame.frameId, rx, ry, rz, -p->normalX, -p->normalY, -p->normalZ,
                                      &action));
    ASSERT(strcmp(action.actionCode, "hpea") == 0);
    ASSERT_EQ_INT(action.semantic, BZ_QUEST_HUD_SEMANTIC_BUTTON);
}

static void test_hit_test_misses_outside_any_region(void) {
    bzQuestHudFrame_t frame;
    hit_test_setup(&frame);
    const bzQuestHudPanelTransform_t *p = &frame.panel;
    /* Far outside any authored rect (well beyond the panel's local extent). */
    float lx = 500.0f, ly = 500.0f;
    float worldX = p->originX + lx * p->rightX + ly * p->downX;
    float worldY = p->originY + lx * p->rightY + ly * p->downY;
    float worldZ = p->originZ + lx * p->rightZ + ly * p->downZ;
    float rx = worldX + p->normalX * 10.0f, ry = worldY + p->normalY * 10.0f, rz = worldZ + p->normalZ * 10.0f;
    bzQuestHudActionId_t action;
    ASSERT(!bz_quest_wc3_hud_hit_test(&frame, frame.frameId, rx, ry, rz, -p->normalX, -p->normalY, -p->normalZ,
                                       &action));
}

static void test_hit_test_boundary_min_edge_inside_max_edge_outside(void) {
    /* Uses a synthetic axis-aligned panel transform (not the real
     * bz_quest_wc3_hud_panel_transform() tilt) so the world<->local
     * round-trip is exact, isolating this test to the hit-region rect
     * inclusion logic itself rather than floating-point tilt precision. */
    bzQuestHudFrame_t frame;
    hit_test_setup(&frame);
    frame.panel.originX = 0.0f; frame.panel.originY = 0.0f; frame.panel.originZ = 0.0f;
    frame.panel.rightX = 1.0f; frame.panel.rightY = 0.0f; frame.panel.rightZ = 0.0f;
    frame.panel.downX = 0.0f; frame.panel.downY = 1.0f; frame.panel.downZ = 0.0f;
    frame.panel.normalX = 0.0f; frame.panel.normalY = 0.0f; frame.panel.normalZ = 1.0f;
    const bzQuestHudHitRegion_t *hr = &frame.hitRegions[0];
    bzQuestHudActionId_t action;

    /* Min corner (x, y) is inclusive. */
    ASSERT(bz_quest_wc3_hud_hit_test(&frame, frame.frameId, hr->x, hr->y, 10.0f, 0.0f, 0.0f, -1.0f, &action));

    /* Max corner (x+w, y+h) is exclusive - falls into "no region" since nothing else occupies it. */
    ASSERT(!bz_quest_wc3_hud_hit_test(&frame, frame.frameId, hr->x + hr->w, hr->y + hr->h, 10.0f, 0.0f, 0.0f, -1.0f,
                                       &action));
}

static void test_hit_test_rejects_parallel_ray(void) {
    bzQuestHudFrame_t frame;
    hit_test_setup(&frame);
    bzQuestHudActionId_t action;
    ASSERT(!bz_quest_wc3_hud_hit_test(&frame, frame.frameId, 0.0f, 0.0f, 0.0f, frame.panel.rightX, frame.panel.rightY,
                                       frame.panel.rightZ, &action));
}

static void test_hit_test_rejects_intersection_behind_origin(void) {
    bzQuestHudFrame_t frame;
    hit_test_setup(&frame);
    const bzQuestHudPanelTransform_t *p = &frame.panel;
    bzQuestHudActionId_t action;
    /* Ray pointing AWAY from the panel (origin already past the plane, direction continues away). */
    float rx = p->originX - p->normalX * 5.0f, ry = p->originY - p->normalY * 5.0f, rz = p->originZ - p->normalZ * 5.0f;
    ASSERT(!bz_quest_wc3_hud_hit_test(&frame, frame.frameId, rx, ry, rz, -p->normalX, -p->normalY, -p->normalZ,
                                       &action));
}

static void test_hit_test_disabled_hit_region_still_reports_hit(void) {
    bzQuestHudInput_t in = make_input_with_player();
    in.actionLayout.present = in.actionLayout.visible = in.actionLayout.valid = true;
    in.actionLayout.numButtons = 1;
    in.actionLayout.buttons[0] = make_button(0, 0, "dis", false, true, BZ_QUEST_HUD_SEMANTIC_BUTTON);
    bzQuestHudFrame_t frame;
    bz_quest_wc3_hud_build(&in, &frame);
    const bzQuestHudHitRegion_t *hr = &frame.hitRegions[0];
    ASSERT(!hr->enabled);
    const bzQuestHudPanelTransform_t *p = &frame.panel;
    float lx = hr->x + hr->w * 0.5f, ly = hr->y + hr->h * 0.5f;
    float wx = p->originX + lx * p->rightX + ly * p->downX;
    float wy = p->originY + lx * p->rightY + ly * p->downY;
    float wz = p->originZ + lx * p->rightZ + ly * p->downZ;
    bzQuestHudActionId_t action;
    /* Hit-testing reports WHAT was hit regardless of enabled state - the caller
     * decides feasibility (mirrors TabletopActionValidation's separate gate). */
    ASSERT(bz_quest_wc3_hud_hit_test(&frame, frame.frameId, wx + p->normalX * 10.0f, wy + p->normalY * 10.0f,
                                      wz + p->normalZ * 10.0f, -p->normalX, -p->normalY, -p->normalZ, &action));
    ASSERT(strcmp(action.actionCode, "dis") == 0);
}

void run_bz_quest_wc3_hud_tests(void) {
    RUN_TEST(test_panel_transform_is_deterministic_and_map_independent);
    RUN_TEST(test_build_with_no_player_shows_loading_status_only);
    RUN_TEST(test_build_with_player_no_selection_shows_resources_and_zero_selected);
    RUN_TEST(test_build_game_result_adds_third_status_line);
    RUN_TEST(test_build_max_value_resources_render_complete_untruncated);
    RUN_TEST(test_build_max_selected_count_and_long_name_render_complete);
    RUN_TEST(test_build_command_card_absent_when_not_present);
    RUN_TEST(test_build_command_card_absent_when_not_visible_or_not_valid);
    RUN_TEST(test_build_sorts_buttons_row_major_by_grid_y_then_x);
    RUN_TEST(test_build_hidden_button_produces_no_quad_or_hit_region);
    RUN_TEST(test_build_disabled_button_still_shown_but_not_enabled);
    RUN_TEST(test_build_unsupported_semantic_treated_as_disabled);
    RUN_TEST(test_build_overlapping_grid_slots_still_each_get_a_distinct_region);
    RUN_TEST(test_build_no_cancel_region_when_target_none);
    RUN_TEST(test_build_cancel_region_appears_when_target_active);
    RUN_TEST(test_build_cancel_region_present_even_with_zero_buttons);
    RUN_TEST(test_build_is_stateless_across_successive_different_calls);
    RUN_TEST(test_hit_test_rejects_stale_frame_id);
    RUN_TEST(test_hit_test_hits_button_center);
    RUN_TEST(test_hit_test_misses_outside_any_region);
    RUN_TEST(test_hit_test_boundary_min_edge_inside_max_edge_outside);
    RUN_TEST(test_hit_test_rejects_parallel_ray);
    RUN_TEST(test_hit_test_rejects_intersection_behind_origin);
    RUN_TEST(test_hit_test_disabled_hit_region_still_reports_hit);
}
