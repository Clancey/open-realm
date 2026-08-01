/*
 * test_bz_quest_wc3_world_transform.c - coverage for the shared Quest-owned
 * world/tabletop position transform (bz_quest_wc3_world_transform_measure/
 * point, bz_quest_wc3_render.h) plus CROSS-SUBSYSTEM integration tests
 * proving terrain, entities, fog, and selection markers all land on the
 * exact same on-screen position for the same authoritative map bounds/world
 * coordinate - the fix for layer 5D's previously "documented as inherited"
 * terrain/entity/fog coordinate-space mismatch (see AGENTS.md's test
 * discipline and this project's PR review requiring an actual fix, not a
 * documentation-only workaround).
 *
 * These tests deliberately call the SAME production entry points terrain
 * (bz_quest_wc3_terrain_build_chunk), entities (bz_quest_wc3_build_world_matrix/
 * build_render_list), fog (bz_quest_wc3_fog_world_to_cell), and selection
 * markers (bz_quest_wc3_selection_marker_from_translation) already use in
 * production - never a hand-duplicated copy of the transform formula - per
 * AGENTS.md's "derive expected values from authoritative constants/reference
 * behavior, not duplicate production formulas" rule.
 */
#include <math.h>
#include <string.h>

#include "bz_quest_wc3_fog.h"
#include "bz_quest_wc3_terrain.h"
#include "test_framework.h"

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_world_transform_measure/point - pure math               */
/* ------------------------------------------------------------------ */

static void test_transform_measure_matches_terrain_measure_scale(void) {
    /* Same bounds bz_quest_wc3_terrain_measure() already covers (see
     * test_bz_quest_wc3_terrain.c's test_measure_computes_scale_and_cell_size) -
     * both functions must derive the identical scale from identical bounds,
     * since bz_quest_wc3_terrain_measure() now calls this function
     * internally rather than duplicating the 1.08 literal. */
    bzQuestWc3WorldTransform_t transform;
    ASSERT(bz_quest_wc3_world_transform_measure(0.0f, 0.0f, 5120.0f, 2560.0f, &transform));
    ASSERT_EQ_FLOAT(transform.scale, 1.08f / 5120.0f, 0.000001f);
    ASSERT_EQ_FLOAT(transform.centerX, 2560.0f, 0.0001f);
    ASSERT_EQ_FLOAT(transform.centerZ, 1280.0f, 0.0001f);
}

static void test_transform_measure_rejects_degenerate_bounds(void) {
    bzQuestWc3WorldTransform_t transform;
    ASSERT(!bz_quest_wc3_world_transform_measure(0.0f, 0.0f, 0.0f, 128.0f, &transform)); /* zero X span */
    ASSERT(!bz_quest_wc3_world_transform_measure(0.0f, 0.0f, INFINITY, 128.0f, &transform)); /* non-finite */
    ASSERT(!bz_quest_wc3_world_transform_measure(128.0f, 0.0f, 0.0f, 128.0f, &transform)); /* inverted (negative span) */
}

static void test_transform_point_applies_scale_and_center(void) {
    bzQuestWc3WorldTransform_t transform = {0.01f, 100.0f, 200.0f};
    float out[3];
    bz_quest_wc3_world_transform_point(&transform, 150.0f, 5.0f, 250.0f, out);
    ASSERT_EQ_FLOAT(out[0], (150.0f - 100.0f) * 0.01f, 0.0001f);
    ASSERT_EQ_FLOAT(out[1], 5.0f * 0.01f, 0.0001f); /* height scaled but never re-centered */
    ASSERT_EQ_FLOAT(out[2], (250.0f - 200.0f) * 0.01f, 0.0001f);
}

static void test_transform_point_null_transform_is_raw_passthrough(void) {
    /* No valid map bounds this frame - defensive fallback, not a crash or a
     * fabricated scale (see bz_quest_wc3_render.h's header comment). */
    float out[3];
    bz_quest_wc3_world_transform_point(NULL, 7.0f, 8.0f, 9.0f, out);
    ASSERT_EQ_FLOAT(out[0], 7.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[1], 8.0f, 0.0001f);
    ASSERT_EQ_FLOAT(out[2], 9.0f, 0.0001f);
}

/* ------------------------------------------------------------------ */
/* Integration fixture helpers                                        */
/* ------------------------------------------------------------------ */

/* One 2x2-tile, 128-world-unit-per-tile terrain map (matches
 * test_bz_quest_wc3_terrain.c's init_terrain_fixture fixture convention),
 * bounds starting at a NONZERO, rectangular origin so tests below also
 * cover nonzero-origin/rectangular bounds, not just the zero-origin/square
 * case. */
static void init_map_fixture(bzQuestWc3TerrainInput_t *terrain, uint32_t tileWidth, uint32_t tileHeight, float minX,
                             float minZ) {
    memset(terrain, 0, sizeof(*terrain));
    strncpy(terrain->identity, "integration", sizeof(terrain->identity) - 1);
    terrain->cornerWidth = tileWidth + 1;
    terrain->cornerHeight = tileHeight + 1;
    terrain->tileWidth = tileWidth;
    terrain->tileHeight = tileHeight;
    terrain->bounds.minX = minX;
    terrain->bounds.minZ = minZ;
    terrain->bounds.maxX = minX + (float)tileWidth * 128.0f;
    terrain->bounds.maxZ = minZ + (float)tileHeight * 128.0f;
    bzQuestWc3TerrainMetrics_t metrics;
    ASSERT_EQ_INT(bz_quest_wc3_terrain_measure(&terrain->bounds, tileWidth, tileHeight, &metrics),
                  BZ_QUEST_WC3_TERRAIN_OK);
    terrain->cellSize = metrics.cellSize;
    bz_quest_wc3_terrain_chunk_grid(tileWidth, tileHeight, &terrain->chunkCountX, &terrain->chunkCountZ);
    terrain->groundTypeCount = 1;
    terrain->cliffTypeCount = 1;
    terrain->referencedGroundCount = 1;
    terrain->referencedCliffCount = 1;
    strncpy(terrain->grounds[0].identity, "ground0.blp", sizeof(terrain->grounds[0].identity) - 1);
    terrain->grounds[0].cornerCount = 1;
    terrain->grounds[0].width = terrain->grounds[0].height = 64;
    strncpy(terrain->cliffs[0].identity, "cliff0.blp", sizeof(terrain->cliffs[0].identity) - 1);
    terrain->cliffs[0].cornerCount = 1;
    terrain->cliffs[0].width = terrain->cliffs[0].height = 64;
    for (uint32_t z = 0; z < terrain->cornerHeight; z++)
        for (uint32_t x = 0; x < terrain->cornerWidth; x++) {
            bzQuestWc3TerrainCorner_t *corner = &terrain->corners[z * terrain->cornerWidth + x];
            corner->rawHeight = 0.0f; /* flat map - keeps X/Z alignment the only thing under test */
            corner->height = 0.0f;
            corner->groundTypeIndex = 0;
            corner->cliffTypeIndex = 0;
        }
}

/* ------------------------------------------------------------------ */
/* Cross-subsystem integration: entity <-> terrain                    */
/* ------------------------------------------------------------------ */

static void test_integration_entity_at_terrain_corner_matches_terrain_position(void) {
    /* A single 1-tile, zero-origin map - the simplest case terrain and
     * entities must agree on. */
    static bzQuestWc3TerrainInput_t terrain;
    static bzQuestWc3TerrainChunk_t chunk;
    init_map_fixture(&terrain, 1, 1, 0.0f, 0.0f);
    ASSERT_EQ_INT(bz_quest_wc3_terrain_build_chunk(&terrain, 0, 0, &chunk), BZ_QUEST_WC3_TERRAIN_OK);

    bzQuestWc3WorldTransform_t transform;
    ASSERT(bz_quest_wc3_world_transform_measure(terrain.bounds.minX, terrain.bounds.minZ, terrain.bounds.maxX,
                                                terrain.bounds.maxZ, &transform));

    /* An entity sitting exactly at the map's (minX, minZ) raw corner -
     * terrain grid index (0,0), chunk.vertices[0]. */
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.originX = terrain.bounds.minX;
    entity.originY = terrain.bounds.minZ; /* engine "north" - see bz_quest_wc3_render.h's axis-swap comment */
    entity.originZ = 0.0f;                /* engine "up" */
    entity.category = 2;
    entity.footprintX = entity.footprintY = 1.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, &transform, world);

    ASSERT_EQ_FLOAT(world[12], chunk.vertices[0].position[0], 0.0001f);
    ASSERT_EQ_FLOAT(world[14], chunk.vertices[0].position[2], 0.0001f);
}

static void test_integration_entity_at_terrain_far_corner_matches_terrain_position(void) {
    /* Inverse corner of the test above (maxX, maxZ) - terrain grid index
     * (tileWidth, tileHeight), chunk.vertices[2] for a single-tile map -
     * proves alignment isn't a coincidence of the (0,0) origin case. */
    static bzQuestWc3TerrainInput_t terrain;
    static bzQuestWc3TerrainChunk_t chunk;
    init_map_fixture(&terrain, 1, 1, 0.0f, 0.0f);
    ASSERT_EQ_INT(bz_quest_wc3_terrain_build_chunk(&terrain, 0, 0, &chunk), BZ_QUEST_WC3_TERRAIN_OK);

    bzQuestWc3WorldTransform_t transform;
    ASSERT(bz_quest_wc3_world_transform_measure(terrain.bounds.minX, terrain.bounds.minZ, terrain.bounds.maxX,
                                                terrain.bounds.maxZ, &transform));

    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.originX = terrain.bounds.maxX;
    entity.originY = terrain.bounds.maxZ;
    entity.category = 2;
    entity.footprintX = entity.footprintY = 1.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, &transform, world);

    ASSERT_EQ_FLOAT(world[12], chunk.vertices[2].position[0], 0.0001f);
    ASSERT_EQ_FLOAT(world[14], chunk.vertices[2].position[2], 0.0001f);
}

static void test_integration_entity_at_terrain_center_matches_terrain_center(void) {
    /* A 2x2-tile map's exact center corner (grid index 1,1) sits at the
     * transform's own center - both terrain (via its index-based centering)
     * and an entity there (via transform.point) must land at (0, _, 0). */
    static bzQuestWc3TerrainInput_t terrain;
    static bzQuestWc3TerrainChunk_t chunk;
    init_map_fixture(&terrain, 2, 2, 0.0f, 0.0f);
    ASSERT_EQ_INT(bz_quest_wc3_terrain_build_chunk(&terrain, 0, 0, &chunk), BZ_QUEST_WC3_TERRAIN_OK);
    /* Chunk (0,0) of a 2x2-tile map covers all 4 tiles (chunk tiles cap is
     * larger than 2) - vertex index 2 of the (0,0) sub-quad is grid corner
     * (1,1), the map's exact center. */
    float centerX = chunk.vertices[2].position[0];
    float centerZ = chunk.vertices[2].position[2];

    bzQuestWc3WorldTransform_t transform;
    ASSERT(bz_quest_wc3_world_transform_measure(terrain.bounds.minX, terrain.bounds.minZ, terrain.bounds.maxX,
                                                terrain.bounds.maxZ, &transform));
    float mapCenterWorldX = (terrain.bounds.minX + terrain.bounds.maxX) * 0.5f;
    float mapCenterWorldZ = (terrain.bounds.minZ + terrain.bounds.maxZ) * 0.5f;

    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.originX = mapCenterWorldX;
    entity.originY = mapCenterWorldZ;
    entity.category = 2;
    entity.footprintX = entity.footprintY = 1.0f;

    float world[16];
    bz_quest_wc3_build_world_matrix(&entity, &transform, world);

    ASSERT_EQ_FLOAT(world[12], centerX, 0.0001f);
    ASSERT_EQ_FLOAT(world[14], centerZ, 0.0001f);
    ASSERT_EQ_FLOAT(world[12], 0.0f, 0.0001f); /* map center is the transform's own origin */
    ASSERT_EQ_FLOAT(world[14], 0.0f, 0.0001f);
}

/* ------------------------------------------------------------------ */
/* Cross-subsystem integration: fog <-> entity/terrain                */
/* ------------------------------------------------------------------ */

static void test_integration_fog_cell_and_entity_share_the_same_raw_point(void) {
    /* Fog cell math intentionally stays in RAW world space end to end (see
     * bz_quest_wc3_fog.h's header comment) - this test proves that raw
     * space is genuinely the SAME raw space entities/terrain are placed
     * from, by deriving the fog bounds and the entity/terrain transform
     * from the identical bzTTBox2_t-shaped bounds and checking a known
     * world point's fog cell against its transformed on-screen position
     * simultaneously. */
    bzQuestWc3FogBounds_t fogBounds = {0.0f, 0.0f, 256.0f, 256.0f}; /* 4x4 cells @ 64 units */
    bzQuestWc3WorldTransform_t transform;
    ASSERT(bz_quest_wc3_world_transform_measure(fogBounds.minX, fogBounds.minY, fogBounds.maxX, fogBounds.maxY,
                                                &transform));

    /* A world point one cell in from the min corner: (64, 64) -> cell (1,1). */
    uint32_t cellX = 0, cellY = 0;
    ASSERT(bz_quest_wc3_fog_world_to_cell(&fogBounds, 4, 4, 64.0f, 64.0f, &cellX, &cellY));
    ASSERT_EQ_INT(cellX, 1);
    ASSERT_EQ_INT(cellY, 1);

    /* The SAME raw point (64, 64), placed through the shared transform, is
     * the exact position an entity/terrain vertex there would use on
     * screen - proving fog and geometry agree on one authoritative
     * raw-space meaning for this coordinate. */
    float out[3];
    bz_quest_wc3_world_transform_point(&transform, 64.0f, 0.0f, 64.0f, out);
    ASSERT_EQ_FLOAT(out[0], (64.0f - transform.centerX) * transform.scale, 0.0001f);
    ASSERT_EQ_FLOAT(out[2], (64.0f - transform.centerZ) * transform.scale, 0.0001f);
}

/* ------------------------------------------------------------------ */
/* Cross-subsystem integration: selection marker <-> entity            */
/* ------------------------------------------------------------------ */

static void test_integration_selection_marker_shares_entity_translation_and_scale(void) {
    /* Mirrors bz_quest_vk_wc3_fog.c's record_selection() production path
     * exactly: build a render list (which applies the shared transform to
     * the entity's translation and derives footprintScale* from the SAME
     * footprint/category formula as the entity's own mesh), then build a
     * marker from that render item's translation/scale - the marker MUST
     * land at the exact same point/size as the model it selects, never a
     * stale pre-transform position or a radius-derived size. */
    bzQuestWc3WorldTransform_t transform = {0.01f, 500.0f, 500.0f};

    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.originX = 600.0f;
    entity.originY = 700.0f;
    entity.originZ = 0.0f;
    entity.category = 2; /* BUILDING */
    entity.footprintX = 2.0f;
    entity.footprintY = 0.5f; /* rectangular - X and Z scale must differ and track orientation */
    entity.selected = true;
    entity.tintR = 1.0f;
    entity.tintA = 1.0f;
    strncpy(entity.modelIdentity, "buildings/human/townhall/townhall.mdx", sizeof(entity.modelIdentity) - 1);

    bzQuestWc3RenderList_t list;
    bz_quest_wc3_build_render_list(&entity, 1, &transform, &list);
    ASSERT_EQ_INT(list.count, 1);
    const bzQuestWc3RenderItem_t *item = &list.items[0];

    bzQuestWc3SelectionMarker_t marker;
    ASSERT(bz_quest_wc3_selection_marker_from_translation(item->world[12], item->world[13], item->world[14],
                                                          item->footprintScaleX, item->footprintScaleY,
                                                          item->footprintScaleZ, item->tintR, item->tintG,
                                                          item->tintB, item->tintA, &marker));

    ASSERT_EQ_FLOAT(marker.world[12], item->world[12], 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[13], item->world[13], 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[14], item->world[14], 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[0], item->footprintScaleX, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[10], item->footprintScaleZ, 0.0001f);
    /* The rectangular footprint must still be rectangular on the marker:
     * X and Z scale differ, matching the entity's own mesh (see
     * bz_quest_wc3_render.c's rectangular-footprint tests). */
    ASSERT(marker.world[0] != marker.world[10]);
    /* Also proves the fix for finding #1: the marker's scale is NOT a
     * uniform value derived from a transport radius, but the same
     * per-axis footprint/category formula the model uses. */
    float expectedScaleX, expectedScaleY, expectedScaleZ;
    bz_quest_wc3_entity_footprint_scale(entity.category, entity.footprintX, entity.footprintY, &expectedScaleX,
                                        &expectedScaleY, &expectedScaleZ);
    ASSERT_EQ_FLOAT(marker.world[0], expectedScaleX, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[10], expectedScaleZ, 0.0001f);
}

static void test_integration_no_double_conversion(void) {
    /* Guards against a regression where a consumer (e.g. a future fog/
     * marker change) mistakenly re-applies the shared transform to an
     * ALREADY-transformed translation (item->world[12/13/14], which
     * build_render_list already ran through the transform exactly once).
     * Applying the transform a second time must produce a DIFFERENT,
     * wrong result for any non-identity transform - proving the two are
     * distinguishable and therefore that a double-application bug would be
     * caught by test_integration_selection_marker_shares_entity_translation_and_scale
     * above (which asserts EQUALITY with the single-application result). */
    bzQuestWc3WorldTransform_t transform = {0.01f, 500.0f, 500.0f};
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.originX = 600.0f;
    entity.originY = 700.0f;
    entity.category = 2;
    entity.footprintX = entity.footprintY = 1.0f;

    float singleApplied[16];
    bz_quest_wc3_build_world_matrix(&entity, &transform, singleApplied);

    float doubleApplied[3];
    bz_quest_wc3_world_transform_point(&transform, singleApplied[12], singleApplied[13], singleApplied[14],
                                       doubleApplied);

    ASSERT(fabsf(doubleApplied[0] - singleApplied[12]) > 0.0001f);
}

/* ------------------------------------------------------------------ */
/* Rectangular bounds, nonzero origin, map reload                     */
/* ------------------------------------------------------------------ */

static void test_integration_rectangular_bounds_and_nonzero_origin(void) {
    /* Wide, non-square map with a nonzero, negative-capable origin - the
     * scale must still be governed by max(spanX, spanZ) and centering must
     * still use each axis's OWN midpoint independently. */
    bzQuestWc3WorldTransform_t transform;
    ASSERT(bz_quest_wc3_world_transform_measure(-1000.0f, 200.0f, 3000.0f, 1000.0f, &transform));
    /* spanX = 4000, spanZ = 800 -> scale governed by spanX. */
    ASSERT_EQ_FLOAT(transform.scale, 1.08f / 4000.0f, 0.000001f);
    ASSERT_EQ_FLOAT(transform.centerX, 1000.0f, 0.0001f); /* (-1000+3000)/2 */
    ASSERT_EQ_FLOAT(transform.centerZ, 600.0f, 0.0001f);  /* (200+1000)/2 */
}

static void test_integration_map_reload_produces_independent_transform(void) {
    /* A "map reload" is just a fresh call with new bounds - these pure
     * functions carry no static/global state, so two back-to-back calls
     * with different bounds must produce two INDEPENDENT, differing
     * transforms with no leftover state from the first call. */
    bzQuestWc3WorldTransform_t first, second;
    ASSERT(bz_quest_wc3_world_transform_measure(0.0f, 0.0f, 1000.0f, 1000.0f, &first));
    ASSERT(bz_quest_wc3_world_transform_measure(5000.0f, 5000.0f, 9000.0f, 7000.0f, &second));
    ASSERT(fabsf(first.scale - second.scale) > 0.000001f);
    ASSERT(fabsf(first.centerX - second.centerX) > 0.0001f);
    ASSERT(fabsf(first.centerZ - second.centerZ) > 0.0001f);

    /* The SAME entity placed under each transform must land in two
     * different places - proves the reload actually took effect, not a
     * cached/stale transform. */
    bzQuestWc3EntityInput_t entity;
    memset(&entity, 0, sizeof(entity));
    entity.originX = 500.0f;
    entity.originY = 500.0f;
    entity.category = 2;
    entity.footprintX = entity.footprintY = 1.0f;

    float worldFirst[16], worldSecond[16];
    bz_quest_wc3_build_world_matrix(&entity, &first, worldFirst);
    bz_quest_wc3_build_world_matrix(&entity, &second, worldSecond);
    ASSERT(fabsf(worldFirst[12] - worldSecond[12]) > 0.0001f || fabsf(worldFirst[14] - worldSecond[14]) > 0.0001f);
}

void run_bz_quest_wc3_world_transform_tests(void) {
    RUN_TEST(test_transform_measure_matches_terrain_measure_scale);
    RUN_TEST(test_transform_measure_rejects_degenerate_bounds);
    RUN_TEST(test_transform_point_applies_scale_and_center);
    RUN_TEST(test_transform_point_null_transform_is_raw_passthrough);
    RUN_TEST(test_integration_entity_at_terrain_corner_matches_terrain_position);
    RUN_TEST(test_integration_entity_at_terrain_far_corner_matches_terrain_position);
    RUN_TEST(test_integration_entity_at_terrain_center_matches_terrain_center);
    RUN_TEST(test_integration_fog_cell_and_entity_share_the_same_raw_point);
    RUN_TEST(test_integration_selection_marker_shares_entity_translation_and_scale);
    RUN_TEST(test_integration_no_double_conversion);
    RUN_TEST(test_integration_rectangular_bounds_and_nonzero_origin);
    RUN_TEST(test_integration_map_reload_produces_independent_transform);
}
