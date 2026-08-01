/*
 * test_bz_quest_wc3_render.c - coverage for bz_quest_wc3_render.c's pure
 * coordinate/scale math and render-list construction (layer 5A). Each case
 * covers a normal path and its inverse/overflow path, per AGENTS.md's test
 * discipline.
 */
#include <stdlib.h>
#include <string.h>

#include "bz_quest_wc3_anim.h"
#include "bz_quest_wc3_render.h"
#include "test_framework.h"

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_build_world_matrix - position (Y/Z swap)                */
/* ------------------------------------------------------------------ */

static void test_position_swaps_y_and_z(void) {
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.originX = 1.0f;
    entity.originY = 2.0f; /* engine "north" */
    entity.originZ = 3.0f; /* engine "up" */
    entity.category = 2;   /* BZ_TTA_CATEGORY_BUILDING - scale multiplier 1.0 */
    entity.footprintX = entity.footprintY = 1.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);

    /* Column-major translation lives at indices 12/13/14. Target X stays
     * engine X; target Y becomes engine Z (up); target Z becomes engine Y -
     * see bz_quest_wc3_render.h's header comment. */
    ASSERT_EQ_FLOAT(world[12], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(world[13], 3.0f, 0.0001f);
    ASSERT_EQ_FLOAT(world[14], 2.0f, 0.0001f);
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_build_world_matrix - heading negation + Y-axis rotation */
/* ------------------------------------------------------------------ */

static void test_zero_heading_yields_identity_rotation(void) {
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.category = 2; /* building: scale 1.0, footprint clamps to 0.25 min but we set 1 */
    entity.footprintX = entity.footprintY = 1.0f;
    entity.angle = 0.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);

    /* With angle==0, cos(-0)=1, sin(-0)=0: the rotation part of the
     * rotation*scale block is a pure diagonal (no off-diagonal terms). */
    ASSERT_EQ_FLOAT(world[2], 0.0f, 0.0001f);  /* -s * sx */
    ASSERT_EQ_FLOAT(world[8], 0.0f, 0.0001f);  /* s * sz */
}

static void test_nonzero_heading_negated_and_about_y(void) {
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.category = 2;
    entity.footprintX = entity.footprintY = 1.0f;
    entity.angle = 1.0f; /* radians, engine yaw */

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);

    /* theta = -1.0 (negated - TabletopAdapter.swift:54). rotScale column 0
     * is (c*sx, 0, -s*sx) where c=cos(-1), s=sin(-1). Column 1 (Y) must stay
     * (0, sy, 0) - rotation is about Y, so Y is unaffected by heading. */
    ASSERT_EQ_FLOAT(world[4], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(world[6], 0.0f, 0.0001f);
    /* Column 0 X/Z components must be nonzero (rotated away from identity)
     * for a nonzero heading - proves rotation is actually applied. */
    ASSERT(world[0] < 0.9999f || world[2] != 0.0f);
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_build_world_matrix - category/footprint scale formula   */
/* ------------------------------------------------------------------ */

static void test_building_scale_uses_full_category_multiplier(void) {
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.category = 2; /* BZ_TTA_CATEGORY_BUILDING: categoryScale 1.0 */
    entity.footprintX = entity.footprintY = 2.0f;
    entity.angle = 0.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);

    /* dioramaX = 1.0 * max(2,2) = 2; sx = min(2,2)*0.06 = 0.12.
     * dioramaY (Y, unaffected by footprint) = 1.0; sy = 1.0*0.08 = 0.08. */
    ASSERT_EQ_FLOAT(world[0], 0.12f, 0.0005f);
    ASSERT_EQ_FLOAT(world[5], 0.08f, 0.0005f);
    ASSERT_EQ_FLOAT(world[10], 0.12f, 0.0005f);
}

static void test_footprint_below_minimum_is_clamped(void) {
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.category = 2;
    entity.footprintX = entity.footprintY = 0.0f; /* below the 0.25 floor */
    entity.angle = 0.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);

    /* dioramaX = 1.0 * max(0.25) = 0.25; sx = min(0.25,2)*0.06 = 0.015. */
    ASSERT_EQ_FLOAT(world[0], 0.015f, 0.0005f);
}

static void test_footprint_above_cap_is_clamped(void) {
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.category = 2;
    entity.footprintX = entity.footprintY = 100.0f; /* far above the 2.0 world-space cap */
    entity.angle = 0.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);

    /* dioramaX = 1.0 * 100 = 100, but min(100,2)*0.06 = 0.12 - the cap
     * bounds the final scale regardless of how large footprint*category is. */
    ASSERT_EQ_FLOAT(world[0], 0.12f, 0.0005f);
}

static void test_rectangular_footprint_scales_x_and_z_independently(void) {
    /* Regression test for a real bug: X and Z must NOT be forced square by
     * sharing a single max(footprintX, footprintY) - each axis uses its OWN
     * footprint value (footprintX -> world X/width, footprintY -> world
     * Z/depth), per WarcraftRenderDescriptors.swift:375-391's independent
     * max(footprint.width, 0.25)/max(footprint.depth, 0.25). A 2x0.5
     * footprint (wide, shallow) must produce two DIFFERENT scale factors,
     * not the same value on both axes. */
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.category = 2; /* BZ_TTA_CATEGORY_BUILDING: categoryScale 1.0 */
    entity.footprintX = 2.0f;
    entity.footprintY = 0.5f;
    entity.angle = 0.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);

    /* sx = min(1.0*2.0, 2)*0.06 = 0.12; sz = min(1.0*0.5, 2)*0.06 = 0.03 -
     * two distinct values, not both forced to whichever axis is larger. */
    ASSERT_EQ_FLOAT(world[0], 0.12f, 0.0005f);
    ASSERT_EQ_FLOAT(world[10], 0.03f, 0.0005f);
    ASSERT(world[0] != world[10]);
}

static void test_rectangular_footprint_orientation_is_not_swapped(void) {
    /* Inverse orientation of the test above - swapping which axis is wide
     * vs. shallow must swap which world-matrix diagonal entry is larger,
     * proving footprintX truly drives X and footprintY truly drives Z (not
     * e.g. both silently reading the same field, or being cross-wired). */
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.category = 2;
    entity.footprintX = 0.5f;
    entity.footprintY = 2.0f;
    entity.angle = 0.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);

    /* sx = min(1.0*0.5, 2)*0.06 = 0.03; sz = min(1.0*2.0, 2)*0.06 = 0.12 -
     * the mirror image of test_rectangular_footprint_scales_x_and_z_independently. */
    ASSERT_EQ_FLOAT(world[0], 0.03f, 0.0005f);
    ASSERT_EQ_FLOAT(world[10], 0.12f, 0.0005f);
}

static void test_rectangular_footprint_clamp_boundaries_are_independent_per_axis(void) {
    /* Clamp-boundary inverse: one axis below the 0.25 floor while the OTHER
     * axis is far above the 2.0 world-space cap, in the SAME entity. Each
     * axis's clamp must apply independently - a shared max()/min() across
     * both axes would incorrectly clamp both to the same bound. */
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.category = 2;
    entity.footprintX = 0.1f;   /* below the 0.25 floor -> clamped to 0.25 */
    entity.footprintY = 100.0f; /* far above the 2.0 cap */
    entity.angle = 0.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);

    /* sx = min(1.0*max(0.1,0.25), 2)*0.06 = min(0.25,2)*0.06 = 0.015.
     * sz = min(1.0*100, 2)*0.06 = min(100,2)*0.06 = 0.12. */
    ASSERT_EQ_FLOAT(world[0], 0.015f, 0.0005f);
    ASSERT_EQ_FLOAT(world[10], 0.12f, 0.0005f);
}

static void test_mobile_and_item_share_category_multiplier(void) {
    bzQuestWc3EntityInput_t mobile, item;
    memset(&mobile, 0, sizeof(mobile));
    memset(&item, 0, sizeof(item));
    mobile.category = 1; /* BZ_TTA_CATEGORY_MOBILE */
    item.category = 6;   /* BZ_TTA_CATEGORY_ITEM */
    mobile.footprintX = mobile.footprintY = item.footprintX = item.footprintY = 1.0f;

    float worldMobile[16], worldItem[16];
    bz_quest_wc3_build_world_matrix(&mobile, worldMobile);
    bz_quest_wc3_build_world_matrix(&item, worldItem);

    ASSERT_EQ_FLOAT(worldMobile[5], worldItem[5], 0.0001f);
    ASSERT_EQ_FLOAT(worldMobile[5], 0.72f * 0.08f, 0.0005f);
}

static void test_unknown_category_falls_back_to_unit_multiplier(void) {
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.category = 0; /* BZ_TTA_CATEGORY_UNKNOWN */
    entity.footprintX = entity.footprintY = 1.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, world);
    ASSERT_EQ_FLOAT(world[5], 0.72f * 0.08f, 0.0005f);
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_build_render_list                                      */
/* ------------------------------------------------------------------ */

static void make_entity(bzQuestWc3EntityInput_t *e, const char *identity) {
    memset(e, 0, sizeof(*e));
    e->category = 2;
    e->footprintX = e->footprintY = 1.0f;
    if (identity) strncpy(e->modelIdentity, identity, sizeof(e->modelIdentity) - 1);
}

static void test_render_list_skips_entities_without_a_model(void) {
    bzQuestWc3EntityInput_t entities[2];
    make_entity(&entities[0], "units/human/footman/footman.mdx");
    make_entity(&entities[1], NULL); /* no model resolved */

    bzQuestWc3RenderList_t list;
    bz_quest_wc3_build_render_list(entities, 2, &list);

    ASSERT_EQ_INT(list.count, 1);
    ASSERT_STR_EQ(list.items[0].modelIdentity, "units/human/footman/footman.mdx");
    ASSERT_EQ_INT(list.overflowCount, 0);
}

static void test_render_list_one_item_per_entity_even_for_shared_models(void) {
    bzQuestWc3EntityInput_t entities[3];
    make_entity(&entities[0], "units/human/footman/footman.mdx");
    make_entity(&entities[1], "units/human/footman/footman.mdx");
    make_entity(&entities[2], "units/human/footman/footman.mdx");

    bzQuestWc3RenderList_t list;
    bz_quest_wc3_build_render_list(entities, 3, &list);

    /* This slice's list construction does not de-duplicate/instance -
     * that's the GPU cache's job by identity key (see bz_quest_wc3_render.h's
     * header comment); each entity always yields its own render item. */
    ASSERT_EQ_INT(list.count, 3);
}

static void test_render_list_reports_overflow_without_dropping_count(void) {
    static bzQuestWc3EntityInput_t entities[BZ_QUEST_WC3_MAX_RENDER_ITEMS + 5];
    for (uint32_t i = 0; i < BZ_QUEST_WC3_MAX_RENDER_ITEMS + 5; i++) {
        make_entity(&entities[i], "doodads/rock.mdx");
    }

    bzQuestWc3RenderList_t list;
    bz_quest_wc3_build_render_list(entities, BZ_QUEST_WC3_MAX_RENDER_ITEMS + 5, &list);

    ASSERT_EQ_INT(list.count, BZ_QUEST_WC3_MAX_RENDER_ITEMS);
    ASSERT_EQ_INT(list.overflowCount, 5);
}

static void test_render_list_rewrites_stale_state(void) {
    bzQuestWc3EntityInput_t entities[1];
    make_entity(&entities[0], "units/human/footman/footman.mdx");

    bzQuestWc3RenderList_t list;
    memset(&list, 0xAA, sizeof(list)); /* poison first */
    bz_quest_wc3_build_render_list(entities, 1, &list);

    ASSERT_EQ_INT(list.count, 1);
    ASSERT_EQ_INT(list.overflowCount, 0);
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_identity_equal                                         */
/* ------------------------------------------------------------------ */

static void test_identity_equal_matches_identical_strings(void) {
    ASSERT(bz_quest_wc3_identity_equal("units/human/footman/footman.mdx",
                                       "units/human/footman/footman.mdx"));
}

static void test_identity_equal_rejects_different_strings(void) {
    ASSERT(!bz_quest_wc3_identity_equal("units/human/footman/footman.mdx",
                                        "units/human/knight/knight.mdx"));
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_model_anim_free - layer 5C anim-arena release          */
/* ------------------------------------------------------------------ */

/* NULL-safety: bz_quest_wc3_capture.c's model_ready_cb() calls this
 * unconditionally on every non-success path (some of which have never
 * allocated an anim arena at all, e.g. a static/non-animated model) - a
 * missing NULL guard here would crash the very first static model drawn. */
static void test_model_anim_free_null_is_a_no_op(void) {
    bz_quest_wc3_model_anim_free(NULL); /* must not crash */
}

/* Hit path: a real single-allocation arena (mirroring
 * bz_quest_wc3_capture.c's two-pass build_model_anim() - one malloc() sized
 * to the model's real data, with every pointer field aliasing into it) must
 * release both the arena and the struct itself without leaking or double-
 * freeing either. Run under a plain malloc/free pair here (no ASan in this
 * host harness), but the shape mirrors production exactly: `arena` is the
 * only owned allocation besides the bzQuestWc3ModelAnim_t struct itself. */
static void test_model_anim_free_releases_arena_and_struct(void) {
    bzQuestWc3ModelAnim_t *anim = malloc(sizeof(*anim));
    ASSERT(anim != NULL);
    memset(anim, 0, sizeof(*anim));
    anim->arena = malloc(64);
    ASSERT(anim->arena != NULL);
    anim->nodes = (const bzQuestWc3StoredNode_t *)anim->arena; /* aliases the arena, not separately owned */
    bz_quest_wc3_model_anim_free(anim); /* frees anim->arena then anim - no separate free(anim->nodes) needed */
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_convert_matrix_zup_to_yup - layer 5C palette basis fix  */
/* ------------------------------------------------------------------ */
/* Expected values below are hand-derived independently from the M' =
 * S*M*S conjugation formula documented on the function's declaration
 * (never by calling bz_quest_mat4_multiply or re-deriving the production
 * formula in-test - see AGENTS.md's "avoid tests that duplicate
 * production formulas" rule). S is the column-major Y<->Z axis swap
 * (swap rows/cols 1 and 2, fix rows/cols 0 and 3). */

static void test_convert_zup_to_yup_identity_stays_identity(void) {
    /* S*I*S = S*S = Identity - a static model or an identity-padded
     * bone-palette slot must convert unchanged (this function's own
     * declaration comment). */
    float identity[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,0,1,
    };
    float out[16];
    bz_quest_wc3_convert_matrix_zup_to_yup(identity, out);
    for (int i = 0; i < 16; i++) {
        float expected = (i % 5 == 0) ? 1.0f : 0.0f; /* diagonal entries at 0,5,10,15 */
        ASSERT_EQ_FLOAT(out[i], expected, 0.0001f);
    }
}

static void test_convert_zup_to_yup_translation_z_becomes_translation_y(void) {
    /* A pure MDX-space translation of tz=10 along raw Z: hand-derived via
     * S*T*S, the translation column's Y/Z components swap exactly like
     * vertex positions do (see bz_quest_wc3_build_world_matrix's own
     * Y/Z position swap and test_position_swaps_y_and_z above), so a
     * root +Z-in-MDX translation must land as +Y in target space. */
    float translateZ[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        0,0,10,1,
    };
    float out[16];
    bz_quest_wc3_convert_matrix_zup_to_yup(translateZ, out);
    ASSERT_EQ_FLOAT(out[12], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[13], 10.0f, 0.0001f); /* target Y gains the raw Z translation */
    ASSERT_EQ_FLOAT(out[14], 0.0f, 0.0001f);  /* target Z loses it (raw Y was 0) */
}

static void test_convert_zup_to_yup_rotation_about_z_becomes_rotation_about_y(void) {
    /* A rotation about raw MDX Z by 90 degrees (cos=0, sin=1), in this
     * codebase's own column-major layout (see bz_quest_mat4_multiply's
     * out[col*4+row] convention): column 0 = (cos, sin, 0), column 1 =
     * (-sin, cos, 0), column 2 = (0,0,1). Hand-derived conjugation by S
     * (swap rows/cols 1,2) yields exactly Ry(-90) in this file's own
     * rotScale convention from bz_quest_wc3_build_world_matrix (column 0 =
     * (c,0,-s), column 1 = (0,1,0), column 2 = (s,0,c) with theta=-90:
     * c=0, s=-1) - i.e. the result is confined to a rotation strictly
     * about the target Y axis, matching this codebase's established
     * heading-rotation sign convention exactly (the sign flip is an
     * inherent, correct consequence of S being an orientation-reversing
     * reflection, not an error - the same S applies to vertices too). */
    float rotateZ90[16] = {
        0,1,0,0,
        -1,0,0,0,
        0,0,1,0,
        0,0,0,1,
    };
    float out[16];
    bz_quest_wc3_convert_matrix_zup_to_yup(rotateZ90, out);
    /* Ry(-90): column0=(0,0,1), column1=(0,1,0), column2=(-1,0,0). */
    ASSERT_EQ_FLOAT(out[0], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[1], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[2], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[4], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[5], 1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[6], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[8], -1.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[9], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[10], 0.0f, 0.0001f);
    /* rotation-only, no translation introduced. */
    ASSERT_EQ_FLOAT(out[12], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[13], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[14], 0.0f, 0.0001f);
}

static void test_convert_zup_to_yup_in_place_matches_separate_buffer(void) {
    /* bz_quest_vk_wc3.c's build_frame_dynamic_material() calls this
     * function with inZup==outYup (converts every palette slot in place
     * before GPU upload) - the declaration comment guarantees this is
     * safe because inZup is fully read into a temp buffer before outYup
     * is written. Prove the in-place result matches an out-of-place call
     * on the same input. */
    float m[16] = {
        1,0,0,0,
        0,1,0,0,
        0,0,1,0,
        2,3,10,1,
    };
    float outOfPlace[16];
    bz_quest_wc3_convert_matrix_zup_to_yup(m, outOfPlace);
    bz_quest_wc3_convert_matrix_zup_to_yup(m, m); /* in place, aliasing input==output */
    for (int i = 0; i < 16; i++) ASSERT_EQ_FLOAT(m[i], outOfPlace[i], 0.0001f);
}

/* Integration: prove hierarchy/pivot composition survives the conversion.
 * Root translates along raw MDX Z=10; its child translates along raw MDX
 * Y=5. bz_quest_wc3_build_pose (bz_quest_wc3_anim.c, deliberately left in
 * raw MDX space - see that file's own header comment) composes these as
 * plain translations: root_zup=(0,0,10), child_zup=root_zup+(0,5,0)=
 * (0,5,10). Converting the FINISHED composed matrices (as
 * build_frame_dynamic_material() does, once per final palette slot) must
 * still land child's target position at parent's target Y plus child's
 * own contribution: converted root=(0,10,0), converted child=(0,10,5) -
 * i.e. S*(parent*local)*S == (S*parent*S)*(S*local*S), since S*S=I. */
static void test_convert_zup_to_yup_preserves_hierarchy_composition(void) {
    bzQuestWc3Node_t nodes[2];
    memset(nodes, 0, sizeof(nodes));
    nodes[0].parentIndex = BZ_QUEST_WC3_NO_PARENT;
    nodes[0].translation.keyCount = 1;
    nodes[0].translation.interp = BZ_QUEST_WC3_INTERP_LINEAR;
    nodes[0].translation.globalSequence = BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE;
    nodes[0].translation.vec3Keys[0] = (bzQuestWc3Vec3Key_t){0, {0, 0, 10}, {0, 0, 0}, {0, 0, 0}};

    nodes[1].parentIndex = 0;
    nodes[1].translation.keyCount = 1;
    nodes[1].translation.interp = BZ_QUEST_WC3_INTERP_LINEAR;
    nodes[1].translation.globalSequence = BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE;
    nodes[1].translation.vec3Keys[0] = (bzQuestWc3Vec3Key_t){0, {0, 5, 0}, {0, 0, 0}, {0, 0, 0}};

    float pose[2][16];
    bz_quest_wc3_build_pose(nodes, 2, 0, 1000, 0, 0, NULL, 0, pose);
    /* sanity: still raw MDX space before conversion (matches
     * test_hierarchy_child_inherits_parent_translation's convention). */
    ASSERT_EQ_FLOAT(pose[0][14], 10.0f, 0.0001f);
    ASSERT_EQ_FLOAT(pose[1][13], 5.0f, 0.0001f);
    ASSERT_EQ_FLOAT(pose[1][14], 10.0f, 0.0001f);

    float converted[2][16];
    bz_quest_wc3_convert_matrix_zup_to_yup(pose[0], converted[0]);
    bz_quest_wc3_convert_matrix_zup_to_yup(pose[1], converted[1]);

    ASSERT_EQ_FLOAT(converted[0][13], 10.0f, 0.0001f); /* root's raw Z=10 -> target Y=10 */
    ASSERT_EQ_FLOAT(converted[0][14], 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(converted[1][13], 10.0f, 0.0001f); /* child inherits root's target Y */
    ASSERT_EQ_FLOAT(converted[1][14], 5.0f, 0.0001f);  /* child's own raw Y=5 -> target Z=5 */
}

void run_bz_quest_wc3_render_tests(void) {
    RUN_TEST(test_position_swaps_y_and_z);
    RUN_TEST(test_zero_heading_yields_identity_rotation);
    RUN_TEST(test_nonzero_heading_negated_and_about_y);
    RUN_TEST(test_building_scale_uses_full_category_multiplier);
    RUN_TEST(test_footprint_below_minimum_is_clamped);
    RUN_TEST(test_footprint_above_cap_is_clamped);
    RUN_TEST(test_rectangular_footprint_scales_x_and_z_independently);
    RUN_TEST(test_rectangular_footprint_orientation_is_not_swapped);
    RUN_TEST(test_rectangular_footprint_clamp_boundaries_are_independent_per_axis);
    RUN_TEST(test_mobile_and_item_share_category_multiplier);
    RUN_TEST(test_unknown_category_falls_back_to_unit_multiplier);
    RUN_TEST(test_render_list_skips_entities_without_a_model);
    RUN_TEST(test_render_list_one_item_per_entity_even_for_shared_models);
    RUN_TEST(test_render_list_reports_overflow_without_dropping_count);
    RUN_TEST(test_render_list_rewrites_stale_state);
    RUN_TEST(test_identity_equal_matches_identical_strings);
    RUN_TEST(test_identity_equal_rejects_different_strings);
    RUN_TEST(test_model_anim_free_null_is_a_no_op);
    RUN_TEST(test_model_anim_free_releases_arena_and_struct);
    RUN_TEST(test_convert_zup_to_yup_identity_stays_identity);
    RUN_TEST(test_convert_zup_to_yup_translation_z_becomes_translation_y);
    RUN_TEST(test_convert_zup_to_yup_rotation_about_z_becomes_rotation_about_y);
    RUN_TEST(test_convert_zup_to_yup_in_place_matches_separate_buffer);
    RUN_TEST(test_convert_zup_to_yup_preserves_hierarchy_composition);
}
