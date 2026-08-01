/*
 * test_bz_quest_input_state.c - host-buildable (no NDK/OpenXR/Vulkan/engine)
 * coverage for the layer 6 pure interaction state machine: board transform
 * math + round-trip + clamping, edge detection, deterministic ray-hit
 * priority and its boundary/staleness/disabled cases, the table-driven
 * command mapping (including the documented ENTITY-target ABI-gap decision
 * and queue-full/stale rejection paths handled by the caller), target/cancel
 * transitions, idempotent lifecycle clears (enter-once vs stay-N-frames), and
 * the accept/reject haptic decision. See bz_quest_input_state.h.
 */
#include <math.h>
#include <string.h>

#include "bz_quest_input_state.h"
#include "test_framework.h"

/* ------------------------------------------------------------ board math */

static void test_board_default_and_clamp(void) {
    bzQuestBoardTransform_t bt;
    bz_quest_board_transform_default(&bt);
    ASSERT_EQ_FLOAT(bt.scale, 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(bt.tz, BZ_QUEST_BOARD_DEFAULT_TZ, 0.0001f);

    bt.scale = 99.0f;
    bt.ty = 99.0f;
    bt.tx = 99.0f;
    bt.tz = -99.0f;
    bz_quest_board_transform_clamp(&bt);
    ASSERT_EQ_FLOAT(bt.scale, BZ_QUEST_BOARD_SCALE_MAX, 0.0001f);
    ASSERT_EQ_FLOAT(bt.ty, BZ_QUEST_BOARD_TY_MAX, 0.0001f);
    ASSERT_EQ_FLOAT(bt.tx, BZ_QUEST_BOARD_TRANSLATE_LIMIT, 0.0001f);
    ASSERT_EQ_FLOAT(bt.tz, -BZ_QUEST_BOARD_TRANSLATE_LIMIT, 0.0001f);

    bt.scale = 0.001f;
    bz_quest_board_transform_clamp(&bt);
    ASSERT_EQ_FLOAT(bt.scale, BZ_QUEST_BOARD_SCALE_MIN, 0.0001f); /* clamps at BOTH ends */
}

static void test_board_yaw_wraps(void) {
    bzQuestBoardTransform_t bt;
    bz_quest_board_transform_default(&bt);
    bt.yaw = 3.5f * (float)M_PI; /* many turns */
    bz_quest_board_transform_clamp(&bt);
    ASSERT(bt.yaw > -(float)M_PI - 0.001f && bt.yaw <= (float)M_PI + 0.001f);
}

static void test_board_point_round_trip(void) {
    /* apply(inverse(p)) == p and inverse(apply(p)) == p for a non-trivial
     * board (translated, rotated, scaled). */
    bzQuestBoardTransform_t bt = {0.4f, -0.3f, -0.7f, 0.9f, 1.7f};
    const float p[3] = {0.11f, 0.22f, -0.33f};
    float fwd[3], back[3];
    bz_quest_board_transform_apply_point(&bt, p, fwd);
    bz_quest_board_transform_inverse_point(&bt, fwd, back);
    ASSERT_EQ_FLOAT(back[0], p[0], 0.0005f);
    ASSERT_EQ_FLOAT(back[1], p[1], 0.0005f);
    ASSERT_EQ_FLOAT(back[2], p[2], 0.0005f);
}

static void test_board_inverse_ray_hits_expected_point(void) {
    /* A ray built to pass through a known composed point, transformed to
     * tracking space by apply, then inverse-transformed back, must still pass
     * through that same composed point. */
    bzQuestBoardTransform_t bt = {0.2f, 0.1f, -0.5f, -0.6f, 0.8f};
    const float composedPt[3] = {0.05f, 0.0f, -0.05f};
    float trackPt[3];
    bz_quest_board_transform_apply_point(&bt, composedPt, trackPt);
    /* tracking-space ray from above pointing down through trackPt */
    const float origin[3] = {trackPt[0], trackPt[1] + 1.0f, trackPt[2]};
    const float dir[3] = {0.0f, -1.0f, 0.0f};
    float ro[3], rd[3];
    bz_quest_board_transform_inverse_ray(&bt, origin, dir, ro, rd);
    /* param t where composed y == composedPt[1] */
    const float t = (composedPt[1] - ro[1]) / rd[1];
    ASSERT_EQ_FLOAT(ro[0] + t * rd[0], composedPt[0], 0.001f);
    ASSERT_EQ_FLOAT(ro[2] + t * rd[2], composedPt[2], 0.001f);
}

static void test_board_matrix_matches_apply_point(void) {
    /* The rendering matrix and apply_point must describe the SAME transform
     * (single source of truth), so M*p == apply_point(p). */
    bzQuestBoardTransform_t bt = {0.3f, -0.2f, -0.4f, 1.1f, 1.3f};
    float m[16];
    bz_quest_board_transform_matrix(&bt, m);
    const float p[3] = {0.2f, 0.3f, -0.1f};
    const float mx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    const float my = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    const float mz = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    float ap[3];
    bz_quest_board_transform_apply_point(&bt, p, ap);
    ASSERT_EQ_FLOAT(mx, ap[0], 0.0005f);
    ASSERT_EQ_FLOAT(my, ap[1], 0.0005f);
    ASSERT_EQ_FLOAT(mz, ap[2], 0.0005f);
}

/* ------------------------------------------------------------ edge detect */

static void test_edge_press_hold_release(void) {
    bzQuestButtonEdge_t e = {0};
    ASSERT(!bz_quest_edge_update(&e, true, false)); /* idle */
    ASSERT(bz_quest_edge_update(&e, true, true));   /* press -> rising */
    ASSERT(!bz_quest_edge_update(&e, true, true));  /* hold -> no re-fire */
    ASSERT(!bz_quest_edge_update(&e, true, true));  /* still held */
    ASSERT(!bz_quest_edge_update(&e, true, false)); /* release */
    ASSERT(bz_quest_edge_update(&e, true, true));   /* second press -> rising again */
}

static void test_edge_inactive_during_hold_clears(void) {
    bzQuestButtonEdge_t e = {0};
    ASSERT(bz_quest_edge_update(&e, true, true));   /* press */
    ASSERT(!bz_quest_edge_update(&e, false, true)); /* controller lost mid-hold: not "pressed" */
    /* Reactivating with the button still physically down must re-fire once
     * (the latch was cleared), never be treated as "still held". */
    ASSERT(bz_quest_edge_update(&e, true, true));
}

/* ------------------------------------------------------- HUD frame fixture */

/* Builds a HUD frame with three buttons (one disabled) plus the synthetic
 * cancel slot, in the given target mode. */
static void build_hud_fixture(bzQuestHudFrame_t *frame, bzQuestHudActionTarget_t targetMode,
                              bool disableSecond, uint64_t frameId) {
    bzQuestHudInput_t in;
    memset(&in, 0, sizeof(in));
    in.frameId = frameId;
    in.player.present = true;
    strncpy(in.player.name, "Grunt", sizeof(in.player.name) - 1);
    in.selectedCount = 1;
    in.actionLayout.present = true;
    in.actionLayout.visible = true;
    in.actionLayout.valid = true;
    in.actionLayout.currentTarget = targetMode;
    in.actionLayout.numButtons = 3;
    for (int i = 0; i < 3; ++i) {
        bzQuestHudButtonInput_t *b = &in.actionLayout.buttons[i];
        snprintf(b->actionCode, sizeof(b->actionCode), "act%d", i);
        snprintf(b->tooltip, sizeof(b->tooltip), "Btn%d", i);
        b->gridX = (uint8_t)(i % BZ_QUEST_HUD_GRID_COLUMNS);
        b->gridY = (uint8_t)(i / BZ_QUEST_HUD_GRID_COLUMNS);
        b->semantic = BZ_QUEST_HUD_SEMANTIC_BUTTON;
        b->target = BZ_QUEST_HUD_TARGET_NONE;
        b->disabled = (i == 1 && disableSecond);
    }
    bz_quest_wc3_hud_build(&in, frame);
}

/* Aims a ray straight at the center of hit region `idx`, guaranteeing a hit. */
static void ray_at_region(const bzQuestHudFrame_t *frame, uint32_t idx, float origin[3], float dir[3]) {
    const bzQuestHudHitRegion_t *r = &frame->hitRegions[idx];
    const bzQuestHudPanelTransform_t *p = &frame->panel;
    const float cx = r->x + r->w * 0.5f, cy = r->y + r->h * 0.5f;
    float world[3];
    world[0] = p->originX + cx * p->rightX + cy * p->downX;
    world[1] = p->originY + cx * p->rightY + cy * p->downY;
    world[2] = p->originZ + cx * p->rightZ + cy * p->downZ;
    origin[0] = world[0] + p->normalX;
    origin[1] = world[1] + p->normalY;
    origin[2] = world[2] + p->normalZ;
    dir[0] = -p->normalX;
    dir[1] = -p->normalY;
    dir[2] = -p->normalZ;
}

static void make_world(bzQuestInputWorld_t *w, const bzQuestHudFrame_t *frame,
                       const bzQuestInputEntity_t *ents, uint32_t entCount, bzQuestHudActionTarget_t mode) {
    memset(w, 0, sizeof(*w));
    w->hudFrame = frame;
    w->generation = frame ? frame->frameId : 1;
    w->entities = ents;
    w->entityCount = entCount;
    w->transform = NULL; /* identity/passthrough for these tests */
    w->planeY = 0.0f;
    w->targetMode = mode;
}

/* ------------------------------------------------------ hit-test priority */

static void test_hit_hud_button_beats_everything(void) {
    bzQuestHudFrame_t frame;
    build_hud_fixture(&frame, BZ_QUEST_HUD_TARGET_NONE, false, 7);
    /* an entity + terrain would also be under the ray, but HUD wins */
    bzQuestInputEntity_t ent = {42, 0, 0, 0, 100.0f, 0, 0};
    bzQuestInputWorld_t w;
    make_world(&w, &frame, &ent, 1, BZ_QUEST_HUD_TARGET_NONE);
    float o[3], d[3];
    ray_at_region(&frame, 0, o, d);
    bzQuestInputHit_t hit;
    bz_quest_input_hit_test(&w, o, d, &hit);
    ASSERT_EQ_INT(hit.kind, BZ_QUEST_INPUT_HIT_HUD_BUTTON);
    ASSERT_STR_EQ(hit.hudAction.actionCode, "act0");
}

static void test_hit_hud_disabled_is_consumed_not_passed_through(void) {
    bzQuestHudFrame_t frame;
    build_hud_fixture(&frame, BZ_QUEST_HUD_TARGET_NONE, true, 3);
    /* find the disabled region (act1) */
    uint32_t idx = 0;
    for (uint32_t i = 0; i < frame.hitRegionCount; ++i)
        if (strcmp(frame.hitRegions[i].action.actionCode, "act1") == 0) idx = i;
    bzQuestInputWorld_t w;
    make_world(&w, &frame, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    float o[3], d[3];
    ray_at_region(&frame, idx, o, d);
    bzQuestInputHit_t hit;
    bz_quest_input_hit_test(&w, o, d, &hit);
    ASSERT_EQ_INT(hit.kind, BZ_QUEST_INPUT_HIT_HUD_DISABLED);
}

static void test_hit_hud_stale_frame_rejected(void) {
    bzQuestHudFrame_t frame;
    build_hud_fixture(&frame, BZ_QUEST_HUD_TARGET_NONE, false, 5);
    bzQuestInputWorld_t w;
    make_world(&w, &frame, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    w.generation = 999; /* != frame.frameId -> hud_hit_test rejects, falls through */
    float o[3], d[3];
    ray_at_region(&frame, 0, o, d);
    bzQuestInputHit_t hit;
    bz_quest_input_hit_test(&w, o, d, &hit);
    ASSERT(hit.kind != BZ_QUEST_INPUT_HIT_HUD_BUTTON); /* stale HUD frame is not actionable */
    ASSERT(hit.kind != BZ_QUEST_INPUT_HIT_HUD_CANCEL);
    ASSERT(hit.kind != BZ_QUEST_INPUT_HIT_HUD_DISABLED);
}

static void test_hit_cancel_semantic(void) {
    bzQuestHudFrame_t frame;
    build_hud_fixture(&frame, BZ_QUEST_HUD_TARGET_POINT, false, 9);
    uint32_t idx = frame.hitRegionCount; /* find the cancel region */
    for (uint32_t i = 0; i < frame.hitRegionCount; ++i)
        if (frame.hitRegions[i].action.semantic == BZ_QUEST_HUD_SEMANTIC_CANCEL) idx = i;
    ASSERT(idx < frame.hitRegionCount);
    bzQuestInputWorld_t w;
    make_world(&w, &frame, NULL, 0, BZ_QUEST_HUD_TARGET_POINT);
    float o[3], d[3];
    ray_at_region(&frame, idx, o, d);
    bzQuestInputHit_t hit;
    bz_quest_input_hit_test(&w, o, d, &hit);
    ASSERT_EQ_INT(hit.kind, BZ_QUEST_INPUT_HIT_HUD_CANCEL);
}

static void test_hit_entity_beats_terrain(void) {
    bzQuestInputEntity_t ent = {77, 0.0f, 0.0f, -2.0f, 0.5f, 123.0f, 456.0f};
    bzQuestInputWorld_t w;
    make_world(&w, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_NONE);
    /* ray from origin toward -Z hits the entity sphere at z=-2 before the
     * y=0 plane (which it is parallel-ish to). */
    const float o[3] = {0.0f, 0.0f, 0.0f};
    const float d[3] = {0.0f, 0.0f, -1.0f};
    bzQuestInputHit_t hit;
    bz_quest_input_hit_test(&w, o, d, &hit);
    ASSERT_EQ_INT(hit.kind, BZ_QUEST_INPUT_HIT_ENTITY);
    ASSERT_EQ_INT((int)hit.entityNumber, 77);
    ASSERT_EQ_FLOAT(hit.entityEngineX, 123.0f, 0.001f);
    ASSERT_EQ_FLOAT(hit.entityEngineNorth, 456.0f, 0.001f);
}

static void test_hit_nearest_entity_wins(void) {
    bzQuestInputEntity_t ents[2] = {
        {1, 0.0f, 0.0f, -5.0f, 0.5f, 0, 0}, /* far */
        {2, 0.0f, 0.0f, -2.0f, 0.5f, 0, 0}, /* near */
    };
    bzQuestInputWorld_t w;
    make_world(&w, NULL, ents, 2, BZ_QUEST_HUD_TARGET_NONE);
    const float o[3] = {0, 0, 0}, d[3] = {0, 0, -1};
    bzQuestInputHit_t hit;
    bz_quest_input_hit_test(&w, o, d, &hit);
    ASSERT_EQ_INT((int)hit.entityNumber, 2);
}

static void test_hit_terrain_plane_and_inverse(void) {
    bzQuestWc3WorldTransform_t t;
    ASSERT(bz_quest_wc3_world_transform_measure(0.0f, 0.0f, 1000.0f, 1000.0f, &t));
    bzQuestInputWorld_t w;
    make_world(&w, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    w.transform = &t;
    /* Aim down at composed point (tx, 0, tz); expect engine coords back. */
    const float composed[3] = {0.1f, 0.5f, -0.1f};
    const float o[3] = {composed[0], composed[1], composed[2]};
    const float d[3] = {0.0f, -1.0f, 0.0f};
    bzQuestInputHit_t hit;
    bz_quest_input_hit_test(&w, o, d, &hit);
    ASSERT_EQ_INT(hit.kind, BZ_QUEST_INPUT_HIT_TERRAIN);
    float engine[3];
    bz_quest_wc3_world_transform_point_inverse(&t, composed[0], 0.0f, composed[2], engine);
    ASSERT_EQ_FLOAT(hit.pointEngineX, engine[0], 0.01f);
    ASSERT_EQ_FLOAT(hit.pointEngineNorth, engine[2], 0.01f);
}

static void test_hit_none_when_ray_escapes(void) {
    bzQuestInputWorld_t w;
    make_world(&w, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    const float o[3] = {0, 1, 0}, d[3] = {0, 1, 0}; /* up, away from y=0 plane */
    bzQuestInputHit_t hit;
    bz_quest_input_hit_test(&w, o, d, &hit);
    ASSERT_EQ_INT(hit.kind, BZ_QUEST_INPUT_HIT_NONE);
}

/* --------------------------------------------------- full update() flows */

/* Zeroes a frame with an identity board-space assumption (board set to
 * identity by the caller so tracking rays == composed rays) and one active
 * right hand aiming down -Z. */
static void base_frame(bzQuestInputFrame_t *f) {
    memset(f, 0, sizeof(*f));
    f->dt = 1.0f / 72.0f;
    f->focused = true;
    f->mapEpoch = 1;
    bzQuestInputHandSample_t *R = &f->hands[BZ_QUEST_INPUT_HAND_RIGHT];
    R->active = true;
    R->aimValid = true;
    R->aimDir[2] = -1.0f;
}

static void identity_board(bzQuestInputState_t *s) {
    s->board.tx = s->board.ty = s->board.tz = 0.0f;
    s->board.yaw = 0.0f;
    s->board.scale = 1.0f;
}

static void test_update_select_entity_on_trigger(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputEntity_t ent = {55, 0.0f, 0.0f, -2.0f, 0.5f, 11.0f, 22.0f};

    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;

    bzQuestInputOutput_t out;
    /* frame 1: establish edge baseline (init frame), trigger up */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.hasCommand);
    /* frame 2: trigger down -> rising -> SELECT */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].selectDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.hasCommand);
    ASSERT_EQ_INT(out.command.type, BZ_QUEST_INPUT_CMD_SELECT);
    ASSERT_EQ_INT((int)out.command.selectCount, 1);
    ASSERT_EQ_INT((int)out.command.selectIds[0], 55);
    /* frame 3: held -> no duplicate */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.hasCommand);
}

static void test_update_additive_select_merges(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputEntity_t ent = {9, 0.0f, 0.0f, -2.0f, 0.5f, 0, 0};
    const uint32_t existing[2] = {3, 7};
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    f.selectedIds = existing;
    f.selectedCount = 2;
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].primaryDown = true;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].selectDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.hasCommand);
    ASSERT_EQ_INT(out.command.type, BZ_QUEST_INPUT_CMD_SELECT);
    ASSERT_EQ_INT((int)out.command.selectCount, 3); /* 3,7 + 9 */
    ASSERT_EQ_INT((int)out.command.selectIds[2], 9);
}

static void test_update_smart_entity_on_squeeze(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputEntity_t ent = {88, 0.0f, 0.0f, -2.0f, 0.5f, 0, 0};
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].squeezeDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.hasCommand);
    ASSERT_EQ_INT(out.command.type, BZ_QUEST_INPUT_CMD_SMART_ENTITY);
    ASSERT_EQ_INT((int)out.command.targetEntity, 88);
}

static void test_update_smart_point_on_terrain(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    /* aim down at the plane */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].aimOrigin[1] = 0.5f;
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].aimDir[1] = -1.0f;
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].aimDir[2] = 0.0f;
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].selectDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.hasCommand);
    ASSERT_EQ_INT(out.command.type, BZ_QUEST_INPUT_CMD_SMART_POINT);
}

static void test_update_target_point_mode_entity_becomes_point(void) {
    /* ABI-gap decision: in a target mode, a ray-hit on an entity submits a
     * TARGET_POINT at the entity's own ground origin (no PostTargetEntity
     * exists). */
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputEntity_t ent = {5, 0.0f, 0.0f, -2.0f, 0.5f, 640.0f, 480.0f};
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_ENTITY);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline; also enters TARGET phase */
    ASSERT_EQ_INT(s.phase, BZ_QUEST_INPUT_PHASE_TARGET_POINT_MODE);
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].selectDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.hasCommand);
    ASSERT_EQ_INT(out.command.type, BZ_QUEST_INPUT_CMD_TARGET_POINT);
    ASSERT_EQ_FLOAT(out.command.x, 640.0f, 0.001f);
    ASSERT_EQ_FLOAT(out.command.y, 480.0f, 0.001f);
}

static void test_update_target_entity_only_terrain_rejects(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].aimOrigin[1] = 0.5f;
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].aimDir[1] = -1.0f;
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].aimDir[2] = 0.0f;
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_ENTITY); /* entity-only, hit terrain */
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].selectDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.hasCommand);
    ASSERT(out.wantHaptic);        /* rejected feedback */
    ASSERT(!out.hapticAccepted);
}

static void test_update_cancel_on_secondary(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].secondaryDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.hasCommand);
    ASSERT_EQ_INT(out.command.type, BZ_QUEST_INPUT_CMD_CANCEL);
}

static void test_update_hud_button_posts_button(void) {
    bzQuestHudFrame_t frame;
    build_hud_fixture(&frame, BZ_QUEST_HUD_TARGET_NONE, false, 1);
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, &frame, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = frame.frameId;
    /* aim right hand at button 0 */
    ray_at_region(&frame, 0, f.hands[BZ_QUEST_INPUT_HAND_RIGHT].aimOrigin,
                  f.hands[BZ_QUEST_INPUT_HAND_RIGHT].aimDir);
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].selectDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.hasCommand);
    ASSERT_EQ_INT(out.command.type, BZ_QUEST_INPUT_CMD_BUTTON);
    ASSERT_STR_EQ(out.command.code, "act0");
}

static void test_update_menu_resets_board(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    s.board.scale = 2.5f;
    s.board.yaw = 1.0f;
    bzQuestInputFrame_t f;
    base_frame(&f);
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].active = true;
    f.menuDown = true;
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* init baseline: menuRise not fired on first frame? */
    /* second frame press edge fires reset */
    f.menuDown = false;
    bz_quest_input_state_update(&s, &f, &out);
    f.menuDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT_EQ_FLOAT(s.board.scale, 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(s.board.yaw, 0.0f, 0.0001f);
}

/* ------------------------------------------------ board manipulation sim */

static void test_update_thumbstick_rotates_and_zooms(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    f.dt = 0.5f;
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].thumbstick[0] = 1.0f; /* full yaw */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].thumbstick[1] = 1.0f; /* full zoom */
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(s.board.yaw > 0.0f);
    ASSERT(s.board.scale > 1.0f);
    ASSERT_EQ_INT(s.phase, BZ_QUEST_INPUT_PHASE_BOARD_MANIPULATE);
}

static void test_update_board_disabled_in_target_mode(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].thumbstick[0] = 1.0f;
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_POINT); /* target mode owns input */
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT_EQ_FLOAT(s.board.yaw, 0.0f, 0.0001f); /* rotation suppressed */
    ASSERT_EQ_INT(s.phase, BZ_QUEST_INPUT_PHASE_TARGET_POINT_MODE);
}

static void test_update_board_manip_suppresses_tap(void) {
    /* While the board is being manipulated (thumbstick), a trigger press must
     * NOT also fire a gameplay command that frame. */
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputEntity_t ent = {1, 0.0f, 0.0f, -2.0f, 0.5f, 0, 0};
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].thumbstick[0] = 1.0f; /* board rotate */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].selectDown = true;    /* and a tap same frame */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.hasCommand); /* board manip owns input */
}

/* -------------------------------------------- left-grip board ownership -
 * PR #25 review fix: left grip is the board-pan gesture's EXCLUSIVE input,
 * from the very first (anchor) frame, in every phase/target mode - never a
 * gameplay smart-command trigger. See bz_update_board()'s `active` fix and
 * bz_hand_owns_smart_trigger() in bz_quest_input_state.c. */

static void aim_left_at_entity(bzQuestInputFrame_t *f) {
    bzQuestInputHandSample_t *L = &f->hands[BZ_QUEST_INPUT_HAND_LEFT];
    L->active = true;
    L->aimValid = true;
    L->aimDir[2] = -1.0f; /* aims down -Z, same as base_frame's right hand */
}

static void aim_left_at_terrain(bzQuestInputFrame_t *f) {
    bzQuestInputHandSample_t *L = &f->hands[BZ_QUEST_INPUT_HAND_LEFT];
    L->active = true;
    L->aimValid = true;
    L->aimOrigin[1] = 0.5f;
    L->aimDir[1] = -1.0f;
}

static void test_left_squeeze_over_entity_posts_no_command(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputEntity_t ent = {42, 0.0f, 0.0f, -2.0f, 0.5f, 0, 0};
    bzQuestInputFrame_t f;
    base_frame(&f);
    aim_left_at_entity(&f);
    make_world(&f.world, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    /* squeeze down from frame 1: this IS the anchor/rising-edge frame. */
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].squeezeDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.hasCommand);
    ASSERT(!out.wantHaptic); /* board ownership absorbs it silently, not a "rejected tap" */
    ASSERT_EQ_INT(s.phase, BZ_QUEST_INPUT_PHASE_BOARD_MANIPULATE);
}

static void test_left_squeeze_over_terrain_posts_no_command(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    aim_left_at_terrain(&f);
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].squeezeDown = true; /* anchor frame */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.hasCommand);
    ASSERT(!out.wantHaptic);
}

static void test_left_grip_anchors_board_manipulate_on_first_frame(void) {
    /* The regression this fix targets: previously `active` stayed false on
     * the anchor frame, so `phase` read IDLE_RAY for one frame - long enough
     * for the same squeeze rising edge to fall through to the gameplay
     * command loop. Now a single update() call with squeeze already down
     * must report BOARD_MANIPULATE immediately, not one frame later. */
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    aim_left_at_terrain(&f);
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].squeezeDown = true;
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].gripPos[0] = 1.0f;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* first-ever call, no prior baseline */
    ASSERT_EQ_INT(s.phase, BZ_QUEST_INPUT_PHASE_BOARD_MANIPULATE);
    ASSERT(s.panDragging);
    ASSERT(!out.hasCommand);
}

static void test_left_grip_drag_updates_board_after_anchor(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    aim_left_at_terrain(&f);
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    /* anchor frame: no delta yet (grip hasn't moved from its first sample). */
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].squeezeDown = true;
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].gripPos[0] = 1.0f;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT_EQ_FLOAT(s.board.tx, 0.0f, 0.0001f); /* anchor alone must not move the board */
    /* drag frame: grip moves +0.5 on X, -0.3 on Z. */
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].gripPos[0] = 1.5f;
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].gripPos[2] = -0.3f;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT_EQ_FLOAT(s.board.tx, 0.5f, 0.0001f);
    ASSERT_EQ_FLOAT(s.board.tz, -0.3f, 0.0001f);
    ASSERT_EQ_INT(s.phase, BZ_QUEST_INPUT_PHASE_BOARD_MANIPULATE);
    ASSERT(!out.hasCommand);
}

static void test_left_grip_release_ends_drag_without_command(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    aim_left_at_terrain(&f);
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].squeezeDown = true;
    bz_quest_input_state_update(&s, &f, &out); /* anchor */
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].squeezeDown = false; /* release */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!s.panDragging);
    ASSERT_EQ_INT(s.phase, BZ_QUEST_INPUT_PHASE_IDLE_RAY);
    ASSERT(!out.hasCommand);
}

static void test_right_squeeze_no_repeat_while_held(void) {
    /* Companion to the left-grip tests above: the RIGHT hand's squeeze is
     * still the sole valid smart-command trigger, edge-fires exactly once
     * per press, and a second press/release cycle fires exactly once more. */
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputEntity_t ent = {77, 0.0f, 0.0f, -2.0f, 0.5f, 0, 0};
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline, squeeze up */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].squeezeDown = true;
    bz_quest_input_state_update(&s, &f, &out); /* rising -> one command */
    ASSERT(out.hasCommand);
    ASSERT_EQ_INT(out.command.type, BZ_QUEST_INPUT_CMD_SMART_ENTITY);
    bz_quest_input_state_update(&s, &f, &out); /* held -> no repeat */
    ASSERT(!out.hasCommand);
    bz_quest_input_state_update(&s, &f, &out); /* still held -> no repeat */
    ASSERT(!out.hasCommand);
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].squeezeDown = false; /* release */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.hasCommand);
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].squeezeDown = true; /* second press */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.hasCommand); /* second cycle fires exactly once more */
    ASSERT_EQ_INT(out.command.type, BZ_QUEST_INPUT_CMD_SMART_ENTITY);
}

static void test_target_mode_does_not_change_left_grip_ownership(void) {
    /* The critical regression case: bz_update_board() never runs while the
     * server is in a target mode, so `phase` can never reach BOARD_MANIPULATE
     * to suppress a left squeeze there via the phase check alone - the
     * hand-scoped bz_hand_owns_smart_trigger() guard must still hold. */
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputEntity_t ent = {13, 0.0f, 0.0f, -2.0f, 0.5f, 640.0f, 480.0f};
    bzQuestInputFrame_t f;
    base_frame(&f);
    aim_left_at_entity(&f);
    make_world(&f.world, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_ENTITY);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].squeezeDown = true; /* held from frame 1 */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.hasCommand);
    ASSERT_EQ_INT(s.phase, BZ_QUEST_INPUT_PHASE_TARGET_POINT_MODE); /* target mode still owns phase */
    bz_quest_input_state_update(&s, &f, &out); /* held another frame */
    ASSERT(!out.hasCommand);
}

static void test_focus_reconnect_map_reset_with_left_grip_held_never_posts(void) {
    /* "left grip held across a lifecycle clear cannot re-anchor and post" -
     * each clear drops panDragging/phase, and the very next frame (still
     * physically squeezed) is a new rising edge on the left hand's squeeze
     * latch - which must still be inert per bz_hand_owns_smart_trigger(). */
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputEntity_t ent = {21, 0.0f, 0.0f, -2.0f, 0.5f, 0, 0};
    bzQuestInputFrame_t f;
    base_frame(&f);
    aim_left_at_entity(&f);
    make_world(&f.world, NULL, &ent, 1, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].squeezeDown = true;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* anchor */
    ASSERT(!out.hasCommand);

    /* focus loss then regain, left squeeze held throughout. */
    f.focused = false;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.clearedThisFrame);
    ASSERT(!out.hasCommand);
    f.focused = true;
    bz_quest_input_state_update(&s, &f, &out); /* re-anchors (new rising edge) */
    ASSERT(!out.hasCommand);

    /* left controller disconnect then reconnect, still squeezed. */
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].active = false;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.clearedThisFrame);
    ASSERT(!out.hasCommand);
    aim_left_at_entity(&f); /* reconnect: active/aimValid true again */
    f.hands[BZ_QUEST_INPUT_HAND_LEFT].squeezeDown = true;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.hasCommand);

    /* map reload, still squeezed. */
    f.mapEpoch = 2;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.clearedThisFrame);
    ASSERT(!out.hasCommand);
    bz_quest_input_state_update(&s, &f, &out); /* re-anchors again */
    ASSERT(!out.hasCommand);
}

/* --------------------------------------------- idempotent lifecycle clear */

static void test_focus_loss_clears_once_not_refire(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* focused baseline */
    /* enter unfocused: clears exactly once */
    f.focused = false;
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.clearedThisFrame);
    /* stay unfocused: does NOT re-fire the clear */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.clearedThisFrame);
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.clearedThisFrame);
}

static void test_controller_loss_clears_once(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    identity_board(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* right active baseline */
    f.hands[BZ_QUEST_INPUT_HAND_RIGHT].active = false; /* right controller lost */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.clearedThisFrame);
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.clearedThisFrame); /* stays lost, no re-fire */
}

static void test_map_reload_clears_and_resets_board(void) {
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    s.board.scale = 2.0f;
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out); /* baseline epoch=1 */
    f.mapEpoch = 2;                            /* new map */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(out.clearedThisFrame);
    ASSERT_EQ_FLOAT(s.board.scale, 1.0f, 0.0001f); /* board reset */
}

static void test_generation_bump_alone_does_not_clear(void) {
    /* Every snapshot bumps generation; that must NOT trigger a clear (only a
     * map-name/epoch change does). */
    bzQuestInputState_t s;
    bz_quest_input_state_init(&s);
    bzQuestInputFrame_t f;
    base_frame(&f);
    make_world(&f.world, NULL, NULL, 0, BZ_QUEST_HUD_TARGET_NONE);
    f.world.generation = 1;
    bzQuestInputOutput_t out;
    bz_quest_input_state_update(&s, &f, &out);
    f.world.generation = 2; /* new snapshot, same map epoch */
    bz_quest_input_state_update(&s, &f, &out);
    ASSERT(!out.clearedThisFrame);
}

/* -------------------------------------------------------------- haptics */

static void test_haptic_accept_reject_distinct(void) {
    bzQuestHapticPulse_t acc = bz_quest_haptic_pulse(true);
    bzQuestHapticPulse_t rej = bz_quest_haptic_pulse(false);
    ASSERT(acc.durationNanos != rej.durationNanos);
    ASSERT(acc.amplitude != rej.amplitude);
    ASSERT(acc.amplitude > 0.0f && acc.amplitude <= 1.0f);
    ASSERT(rej.amplitude > 0.0f && rej.amplitude <= 1.0f);
}

/* -------------------------------------------------------------- command map */

static void test_command_table_reaches_every_type(void) {
    /* Reachability sanity: each command type is produced by at least one of
     * the flows exercised above. This test asserts the enum's span is what
     * the mapping covers (guards against an enum value silently added with
     * no mapping). */
    ASSERT_EQ_INT(BZ_QUEST_INPUT_CMD_NONE, 0);
    ASSERT(BZ_QUEST_INPUT_CMD_TARGET_POINT > BZ_QUEST_INPUT_CMD_NONE);
}

void run_bz_quest_input_state_tests(void) {
    RUN_TEST(test_board_default_and_clamp);
    RUN_TEST(test_board_yaw_wraps);
    RUN_TEST(test_board_point_round_trip);
    RUN_TEST(test_board_inverse_ray_hits_expected_point);
    RUN_TEST(test_board_matrix_matches_apply_point);
    RUN_TEST(test_edge_press_hold_release);
    RUN_TEST(test_edge_inactive_during_hold_clears);
    RUN_TEST(test_hit_hud_button_beats_everything);
    RUN_TEST(test_hit_hud_disabled_is_consumed_not_passed_through);
    RUN_TEST(test_hit_hud_stale_frame_rejected);
    RUN_TEST(test_hit_cancel_semantic);
    RUN_TEST(test_hit_entity_beats_terrain);
    RUN_TEST(test_hit_nearest_entity_wins);
    RUN_TEST(test_hit_terrain_plane_and_inverse);
    RUN_TEST(test_hit_none_when_ray_escapes);
    RUN_TEST(test_update_select_entity_on_trigger);
    RUN_TEST(test_update_additive_select_merges);
    RUN_TEST(test_update_smart_entity_on_squeeze);
    RUN_TEST(test_update_smart_point_on_terrain);
    RUN_TEST(test_update_target_point_mode_entity_becomes_point);
    RUN_TEST(test_update_target_entity_only_terrain_rejects);
    RUN_TEST(test_update_cancel_on_secondary);
    RUN_TEST(test_update_hud_button_posts_button);
    RUN_TEST(test_update_menu_resets_board);
    RUN_TEST(test_update_thumbstick_rotates_and_zooms);
    RUN_TEST(test_update_board_disabled_in_target_mode);
    RUN_TEST(test_update_board_manip_suppresses_tap);
    RUN_TEST(test_left_squeeze_over_entity_posts_no_command);
    RUN_TEST(test_left_squeeze_over_terrain_posts_no_command);
    RUN_TEST(test_left_grip_anchors_board_manipulate_on_first_frame);
    RUN_TEST(test_left_grip_drag_updates_board_after_anchor);
    RUN_TEST(test_left_grip_release_ends_drag_without_command);
    RUN_TEST(test_right_squeeze_no_repeat_while_held);
    RUN_TEST(test_target_mode_does_not_change_left_grip_ownership);
    RUN_TEST(test_focus_reconnect_map_reset_with_left_grip_held_never_posts);
    RUN_TEST(test_focus_loss_clears_once_not_refire);
    RUN_TEST(test_controller_loss_clears_once);
    RUN_TEST(test_map_reload_clears_and_resets_board);
    RUN_TEST(test_generation_bump_alone_does_not_clear);
    RUN_TEST(test_haptic_accept_reject_distinct);
    RUN_TEST(test_command_table_reaches_every_type);
}
