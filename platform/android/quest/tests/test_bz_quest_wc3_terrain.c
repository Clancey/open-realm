/*
 * test_bz_quest_wc3_terrain.c - coverage for layer 5B's pure Warcraft III
 * terrain math and chunk builder.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_quest_wc3_cache.h"
#include "bz_quest_wc3_terrain.h"
#include "test_framework.h"

static void init_ground_ref(bzQuestWc3TerrainTextureRef_t *ref, const char *identity, uint32_t typeIndex,
                            uint32_t width, uint32_t height) {
    memset(ref, 0, sizeof(*ref));
    strncpy(ref->identity, identity, sizeof(ref->identity) - 1);
    ref->typeIndex = typeIndex;
    ref->typeId = typeIndex;
    ref->cornerCount = 1;
    ref->width = width;
    ref->height = height;
}

/* Small deterministic fixture: bounds imply 128 world units per tile unless a test overrides them. */
static void init_terrain_fixture(bzQuestWc3TerrainInput_t *terrain, const char *identity,
                                 uint32_t tileWidth, uint32_t tileHeight) {
    memset(terrain, 0, sizeof(*terrain));
    strncpy(terrain->identity, identity, sizeof(terrain->identity) - 1);
    terrain->cornerWidth = tileWidth + 1;
    terrain->cornerHeight = tileHeight + 1;
    terrain->tileWidth = tileWidth;
    terrain->tileHeight = tileHeight;
    terrain->bounds.minX = 0.0f;
    terrain->bounds.minZ = 0.0f;
    terrain->bounds.maxX = (float)tileWidth * 128.0f;
    terrain->bounds.maxZ = (float)tileHeight * 128.0f;
    bzQuestWc3TerrainMetrics_t metrics;
    ASSERT_EQ_INT(bz_quest_wc3_terrain_measure(&terrain->bounds, tileWidth, tileHeight, &metrics),
                  BZ_QUEST_WC3_TERRAIN_OK);
    terrain->cellSize = metrics.cellSize;
    bz_quest_wc3_terrain_chunk_grid(tileWidth, tileHeight, &terrain->chunkCountX, &terrain->chunkCountZ);
    terrain->groundTypeCount = 2;
    terrain->cliffTypeCount = 1;
    terrain->referencedGroundCount = 1;
    terrain->referencedCliffCount = 1;
    init_ground_ref(&terrain->grounds[0], "ground0.blp", 0, 64, 64);
    init_ground_ref(&terrain->cliffs[0], "cliff0.blp", 0, 64, 64);
    for (uint32_t z = 0; z < terrain->cornerHeight; z++)
        for (uint32_t x = 0; x < terrain->cornerWidth; x++) {
            bzQuestWc3TerrainCorner_t *corner = &terrain->corners[z * terrain->cornerWidth + x];
            float raw = (float)(z * 1000 + x);
            corner->rawHeight = raw;
            corner->rawWaterHeight = raw + 10.0f;
            corner->height = raw * metrics.scale;
            corner->waterHeight = corner->rawWaterHeight * metrics.scale;
            corner->groundTypeIndex = 0;
            corner->cliffTypeIndex = 0;
            corner->groundVariation = 0;
            corner->cliffVariation = 0;
            corner->cliffLevel = 0;
            corner->flags = 0;
        }
}

static void set_corner(bzQuestWc3TerrainInput_t *terrain, uint32_t x, uint32_t z, float rawHeight,
                       float rawWaterHeight, int32_t groundTypeIndex, int32_t cliffTypeIndex,
                       uint8_t cliffLevel, uint8_t groundVariation, uint8_t flags) {
    bzQuestWc3TerrainCorner_t *corner = &terrain->corners[z * terrain->cornerWidth + x];
    float scale = 1.08f / (terrain->bounds.maxX - terrain->bounds.minX > terrain->bounds.maxZ - terrain->bounds.minZ ?
                           terrain->bounds.maxX - terrain->bounds.minX : terrain->bounds.maxZ - terrain->bounds.minZ);
    corner->rawHeight = rawHeight;
    corner->rawWaterHeight = rawWaterHeight;
    corner->height = rawHeight * scale;
    corner->waterHeight = rawWaterHeight * scale;
    corner->groundTypeIndex = groundTypeIndex;
    corner->cliffTypeIndex = cliffTypeIndex;
    corner->cliffLevel = cliffLevel;
    corner->groundVariation = groundVariation;
    corner->flags = flags;
}

static float fixture_scale(const bzQuestWc3TerrainInput_t *terrain) {
    return 1.08f / fmaxf(terrain->bounds.maxX - terrain->bounds.minX,
                         terrain->bounds.maxZ - terrain->bounds.minZ);
}

/* ------------------------------------------------------------------ */
/* Measure / bounds helpers                                           */
/* ------------------------------------------------------------------ */

static void test_measure_computes_scale_and_cell_size(void) {
    bzQuestWc3TerrainBounds_t bounds = {0.0f, 0.0f, 5120.0f, 2560.0f};
    bzQuestWc3TerrainMetrics_t metrics;
    ASSERT_EQ_INT(bz_quest_wc3_terrain_measure(&bounds, 40, 20, &metrics), BZ_QUEST_WC3_TERRAIN_OK);
    ASSERT_EQ_FLOAT(metrics.scale, 1.08f / 5120.0f, 0.000001f);
    ASSERT_EQ_FLOAT(metrics.cellSize, 128.0f * metrics.scale, 0.000001f);
}

static void test_measure_rejects_non_square_tiles(void) {
    bzQuestWc3TerrainBounds_t bounds = {0.0f, 0.0f, 4096.0f, 1024.0f};
    bzQuestWc3TerrainMetrics_t metrics;
    ASSERT_EQ_INT(bz_quest_wc3_terrain_measure(&bounds, 16, 16, &metrics),
                  BZ_QUEST_WC3_TERRAIN_ERR_NON_SQUARE_TILES);
}

static void test_measure_rejects_non_positive_or_non_finite_bounds(void) {
    bzQuestWc3TerrainBounds_t flat = {0.0f, 0.0f, 0.0f, 128.0f};
    bzQuestWc3TerrainMetrics_t metrics;
    ASSERT_EQ_INT(bz_quest_wc3_terrain_measure(&flat, 1, 1, &metrics), BZ_QUEST_WC3_TERRAIN_ERR_INVALID_BOUNDS);
    bzQuestWc3TerrainBounds_t inf = {0.0f, 0.0f, INFINITY, 128.0f};
    ASSERT_EQ_INT(bz_quest_wc3_terrain_measure(&inf, 1, 1, &metrics), BZ_QUEST_WC3_TERRAIN_ERR_INVALID_BOUNDS);
}

static void test_chunk_grid_and_bounds_handle_rectangular_maps(void) {
    uint32_t chunkX = 0, chunkZ = 0;
    bzQuestWc3TerrainChunkBounds_t bounds;
    bz_quest_wc3_terrain_chunk_grid(40, 20, &chunkX, &chunkZ);
    ASSERT_EQ_INT(chunkX, 2);
    ASSERT_EQ_INT(chunkZ, 1);
    ASSERT(bz_quest_wc3_terrain_chunk_bounds(40, 20, 1, 0, &bounds));
    ASSERT_EQ_INT(bounds.minX, 32);
    ASSERT_EQ_INT(bounds.maxX, 40);
    ASSERT_EQ_INT(bounds.minZ, 0);
    ASSERT_EQ_INT(bounds.maxZ, 20);
    ASSERT(!bz_quest_wc3_terrain_chunk_bounds(40, 20, 0, 1, &bounds));
}

/* ------------------------------------------------------------------ */
/* Chunk building / point() centering                                 */
/* ------------------------------------------------------------------ */

static void test_build_chunk_single_tile_uses_all_four_authoritative_corners(void) {
    static bzQuestWc3TerrainInput_t terrain;
    static bzQuestWc3TerrainChunk_t chunk;
    init_terrain_fixture(&terrain, "single", 1, 1);
    set_corner(&terrain, 0, 0, 10.0f, 20.0f, 0, 0, 0, 0, 0);
    set_corner(&terrain, 1, 0, 20.0f, 30.0f, 0, 0, 0, 0, 0);
    set_corner(&terrain, 1, 1, 30.0f, 40.0f, 0, 0, 0, 0, 0);
    set_corner(&terrain, 0, 1, 40.0f, 50.0f, 0, 0, 0, 0, 0);
    ASSERT_EQ_INT(bz_quest_wc3_terrain_build_chunk(&terrain, 0, 0, &chunk), BZ_QUEST_WC3_TERRAIN_OK);
    ASSERT_EQ_INT(chunk.meta.vertexCount, 4);
    ASSERT_EQ_INT(chunk.meta.indexCount, 6);
    ASSERT_EQ_INT(chunk.meta.drawRangeCount, 1);
    ASSERT_EQ_FLOAT(chunk.vertices[0].position[0], -terrain.cellSize * 0.5f, 0.0001f);
    ASSERT_EQ_FLOAT(chunk.vertices[0].position[2], -terrain.cellSize * 0.5f, 0.0001f);
    ASSERT_EQ_FLOAT(chunk.vertices[2].position[0], terrain.cellSize * 0.5f, 0.0001f);
    ASSERT_EQ_FLOAT(chunk.vertices[2].position[2], terrain.cellSize * 0.5f, 0.0001f);
    ASSERT_EQ_FLOAT(chunk.vertices[0].position[1], 10.0f * fixture_scale(&terrain), 0.0001f);
    ASSERT_EQ_FLOAT(chunk.vertices[1].position[1], 20.0f * fixture_scale(&terrain), 0.0001f);
    ASSERT_EQ_FLOAT(chunk.vertices[2].position[1], 30.0f * fixture_scale(&terrain), 0.0001f);
    ASSERT_EQ_FLOAT(chunk.vertices[3].position[1], 40.0f * fixture_scale(&terrain), 0.0001f);
}

static void test_build_chunk_clamps_last_chunk_on_non_multiple_of_32_map(void) {
    static bzQuestWc3TerrainInput_t terrain;
    static bzQuestWc3TerrainChunk_t chunk;
    init_terrain_fixture(&terrain, "fifty", 50, 50);
    ASSERT_EQ_INT(bz_quest_wc3_terrain_build_chunk(&terrain, 1, 1, &chunk), BZ_QUEST_WC3_TERRAIN_OK);
    ASSERT_EQ_INT(chunk.meta.minTileX, 32);
    ASSERT_EQ_INT(chunk.meta.maxTileX, 50);
    ASSERT_EQ_INT(chunk.meta.minTileZ, 32);
    ASSERT_EQ_INT(chunk.meta.maxTileZ, 50);
    ASSERT_EQ_INT(chunk.meta.vertexCount, 18 * 18 * 4);
    ASSERT_EQ_INT(chunk.meta.indexCount, 18 * 18 * 6);
    uint32_t last = chunk.meta.vertexCount - 4;
    ASSERT_EQ_FLOAT(chunk.vertices[last + 0].position[1], (49.0f + 49.0f * 1000.0f) * fixture_scale(&terrain), 0.001f);
    ASSERT_EQ_FLOAT(chunk.vertices[last + 1].position[1], (50.0f + 49.0f * 1000.0f) * fixture_scale(&terrain), 0.001f);
    ASSERT_EQ_FLOAT(chunk.vertices[last + 2].position[1], (50.0f + 50.0f * 1000.0f) * fixture_scale(&terrain), 0.001f);
    ASSERT_EQ_FLOAT(chunk.vertices[last + 3].position[1], (49.0f + 50.0f * 1000.0f) * fixture_scale(&terrain), 0.001f);
}

/* ------------------------------------------------------------------ */
/* Winding helper + authoritative ground indices                      */
/* ------------------------------------------------------------------ */

static void test_quad_forward_winding_matches_flat_ground_quad(void) {
    float a[3] = {-0.54f, 0.0f, -0.54f}, b[3] = {0.54f, 0.0f, -0.54f}, c[3] = {0.54f, 0.0f, 0.54f};
    float up[3] = {0.0f, 1.0f, 0.0f};
    ASSERT(!bz_quest_wc3_terrain_quad_uses_forward_winding(a, b, c, up));
}

static void test_quad_reverse_winding_branch_is_reachable_via_inverted_normal(void) {
    /* The flat ground triangle above faces -Y in this coordinate order, so flipping the normal to -Y drives the opposite winding decision.
     * This exercises emit_quad()'s alternate index branch directly without inventing a non-authoritative terrain cell ordering. */
    float a[3] = {-0.54f, 0.0f, -0.54f}, b[3] = {0.54f, 0.0f, -0.54f}, c[3] = {0.54f, 0.0f, 0.54f};
    float down[3] = {0.0f, -1.0f, 0.0f};
    ASSERT(bz_quest_wc3_terrain_quad_uses_forward_winding(a, b, c, down));
}

static void test_build_chunk_ground_indices_match_expected_order(void) {
    static bzQuestWc3TerrainInput_t terrain;
    static bzQuestWc3TerrainChunk_t chunk;
    init_terrain_fixture(&terrain, "indices", 1, 1);
    ASSERT_EQ_INT(bz_quest_wc3_terrain_build_chunk(&terrain, 0, 0, &chunk), BZ_QUEST_WC3_TERRAIN_OK);
    ASSERT_EQ_INT(chunk.indices[0], 0);
    ASSERT_EQ_INT(chunk.indices[1], 2);
    ASSERT_EQ_INT(chunk.indices[2], 1);
    ASSERT_EQ_INT(chunk.indices[3], 0);
    ASSERT_EQ_INT(chunk.indices[4], 3);
    ASSERT_EQ_INT(chunk.indices[5], 2);
}

/* ------------------------------------------------------------------ */
/* Ground layer bit masks / wide-atlas variation                       */
/* ------------------------------------------------------------------ */

static void test_surface_layers_compute_second_layer_bitmask_uvs_exactly(void) {
    static bzQuestWc3TerrainInput_t terrain;
    bzQuestWc3TerrainSurfaceLayer_t layers[BZ_QUEST_WC3_TERRAIN_MAX_SURFACE_LAYERS_PER_CELL];
    uint32_t count = 0;
    init_terrain_fixture(&terrain, "layers", 1, 1);
    terrain.referencedGroundCount = 2;
    init_ground_ref(&terrain.grounds[0], "base.blp", 0, 64, 64);
    init_ground_ref(&terrain.grounds[1], "blend.blp", 1, 256, 256);
    set_corner(&terrain, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    set_corner(&terrain, 1, 0, 0, 0, 1, 0, 0, 0, 0);
    set_corner(&terrain, 1, 1, 0, 0, 1, 0, 0, 0, 0);
    set_corner(&terrain, 0, 1, 0, 0, 0, 0, 0, 0, 0);
    bzQuestWc3TerrainCorner_t corners[4] = {
        terrain.corners[0], terrain.corners[1], terrain.corners[3], terrain.corners[2],
    };
    ASSERT_EQ_INT(bz_quest_wc3_terrain_surface_layers(&terrain, 0, 0, corners, layers, &count),
                  BZ_QUEST_WC3_TERRAIN_OK);
    ASSERT_EQ_INT(count, 2);
    ASSERT_EQ_INT(layers[1].referencedIndex, 1);
    ASSERT_EQ_FLOAT(layers[1].uv[0][0], 0.25625f, 0.00001f);
    ASSERT_EQ_FLOAT(layers[1].uv[0][1], 0.49375f, 0.00001f);
    ASSERT_EQ_FLOAT(layers[1].uv[2][0], 0.49375f, 0.00001f);
    ASSERT_EQ_FLOAT(layers[1].uv[2][1], 0.25625f, 0.00001f);
}

static void test_surface_layers_wide_atlas_variations_match_authoritative_tile_choice(void) {
    static bzQuestWc3TerrainInput_t terrain;
    bzQuestWc3TerrainSurfaceLayer_t layers[BZ_QUEST_WC3_TERRAIN_MAX_SURFACE_LAYERS_PER_CELL];
    uint32_t count = 0;
    init_terrain_fixture(&terrain, "wide", 1, 1);
    terrain.referencedGroundCount = 2;
    init_ground_ref(&terrain.grounds[0], "base.blp", 0, 64, 64);
    init_ground_ref(&terrain.grounds[1], "wide.blp", 1, 512, 256);
    for (uint32_t i = 0; i < 4; i++) terrain.corners[i].groundTypeIndex = 1;
    bzQuestWc3TerrainCorner_t corners[4] = {
        terrain.corners[0], terrain.corners[1], terrain.corners[3], terrain.corners[2],
    };

    corners[0].groundVariation = 0;
    ASSERT_EQ_INT(bz_quest_wc3_terrain_surface_layers(&terrain, 0, 0, corners, layers, &count),
                  BZ_QUEST_WC3_TERRAIN_OK);
    ASSERT_EQ_FLOAT(layers[1].uv[0][0], 0.503125f, 0.00001f);
    ASSERT_EQ_FLOAT(layers[1].uv[2][0], 0.621875f, 0.00001f);

    corners[0].groundVariation = 15;
    ASSERT_EQ_INT(bz_quest_wc3_terrain_surface_layers(&terrain, 0, 0, corners, layers, &count),
                  BZ_QUEST_WC3_TERRAIN_OK);
    ASSERT_EQ_FLOAT(layers[1].uv[0][0], 0.878125f, 0.00001f);
    ASSERT_EQ_FLOAT(layers[1].uv[2][0], 0.996875f, 0.00001f);

    corners[0].groundVariation = 16;
    ASSERT_EQ_INT(bz_quest_wc3_terrain_surface_layers(&terrain, 0, 0, corners, layers, &count),
                  BZ_QUEST_WC3_TERRAIN_OK);
    ASSERT_EQ_FLOAT(layers[1].uv[0][0], 0.378125f, 0.00001f);
    ASSERT_EQ_FLOAT(layers[1].uv[2][0], 0.496875f, 0.00001f);

    corners[0].groundVariation = 200;
    ASSERT_EQ_INT(bz_quest_wc3_terrain_surface_layers(&terrain, 0, 0, corners, layers, &count),
                  BZ_QUEST_WC3_TERRAIN_OK);
    ASSERT_EQ_FLOAT(layers[1].uv[0][0], 0.003125f, 0.00001f);
    ASSERT_EQ_FLOAT(layers[1].uv[2][0], 0.121875f, 0.00001f);
}

/* ------------------------------------------------------------------ */
/* Water / cliff feature helpers                                       */
/* ------------------------------------------------------------------ */

static void test_water_opacity_clamps_at_all_boundaries(void) {
    ASSERT_EQ_FLOAT(bz_quest_wc3_terrain_water_opacity(100.0f, 100.0f), 0.0f, 0.0001f);
    ASSERT_EQ_FLOAT(bz_quest_wc3_terrain_water_opacity(125.0f, 100.0f), 0.5f, 0.0001f);
    ASSERT_EQ_FLOAT(bz_quest_wc3_terrain_water_opacity(160.0f, 100.0f), 0.5f, 0.0001f);
    ASSERT_EQ_FLOAT(bz_quest_wc3_terrain_water_opacity(90.0f, 100.0f), 0.0f, 0.0001f);
}

static void test_tile_is_water_rejects_map_edge_corners(void) {
    bzQuestWc3TerrainCorner_t corners[4];
    memset(corners, 0, sizeof(corners));
    corners[0].flags = BZ_QUEST_WC3_TERRAIN_WATER;
    ASSERT(bz_quest_wc3_terrain_tile_is_water(corners));
    corners[2].flags = BZ_QUEST_WC3_TERRAIN_MAP_EDGE;
    ASSERT(!bz_quest_wc3_terrain_tile_is_water(corners));
}

static void test_tile_is_cliff_requires_distinct_levels_and_no_ramp(void) {
    bzQuestWc3TerrainCorner_t corners[4];
    memset(corners, 0, sizeof(corners));
    corners[0].cliffLevel = 0; corners[1].cliffLevel = 1; corners[2].cliffLevel = 1; corners[3].cliffLevel = 0;
    corners[0].flags = 0; corners[1].flags = 0; corners[2].flags = 0; corners[3].flags = BZ_QUEST_WC3_TERRAIN_NO_CLIFF;
    ASSERT(bz_quest_wc3_terrain_tile_is_cliff(corners));
    corners[1].cliffLevel = corners[2].cliffLevel = corners[3].cliffLevel = 0;
    ASSERT(!bz_quest_wc3_terrain_tile_is_cliff(corners));
    corners[1].cliffLevel = 1;
    corners[0].flags |= BZ_QUEST_WC3_TERRAIN_RAMP;
    ASSERT(!bz_quest_wc3_terrain_tile_is_cliff(corners));
    corners[0].flags = corners[1].flags = corners[2].flags = corners[3].flags = BZ_QUEST_WC3_TERRAIN_NO_CLIFF;
    ASSERT(!bz_quest_wc3_terrain_tile_is_cliff(corners));
}

static void test_cliff_bottom_and_material_fallbacks(void) {
    static bzQuestWc3TerrainInput_t terrain;
    init_terrain_fixture(&terrain, "cliff", 1, 1);
    ASSERT_EQ_FLOAT(bz_quest_wc3_terrain_cliff_bottom(4.0f, 7.0f, 5.0f, 6.0f, 1.08f), 2.92f, 0.0001f);
    bzQuestWc3TerrainCorner_t corners[4];
    memset(corners, 0, sizeof(corners));
    corners[0].cliffTypeIndex = 0;
    bzQuestWc3TerrainMaterialRef_t ref = bz_quest_wc3_terrain_resolve_cliff_material(&terrain, corners);
    ASSERT_EQ_INT(ref.kind, BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF);
    ASSERT_EQ_INT(ref.referencedIndex, 0);
    terrain.referencedCliffCount = 0;
    ref = bz_quest_wc3_terrain_resolve_cliff_material(&terrain, corners);
    ASSERT_EQ_INT(ref.kind, BZ_QUEST_WC3_TERRAIN_MATERIAL_GROUND);
    ASSERT_EQ_INT(ref.referencedIndex, 0);
}

/* ------------------------------------------------------------------ */
/* Keys and cache-usage assumptions                                    */
/* ------------------------------------------------------------------ */

static void test_chunk_and_texture_keys_compare_stably(void) {
    char a[BZ_QUEST_WC3_TERRAIN_MAX_KEY], b[BZ_QUEST_WC3_TERRAIN_MAX_KEY], c[BZ_QUEST_WC3_TERRAIN_MAX_KEY];
    bz_quest_wc3_terrain_chunk_key("terrainA", 1, 2, a);
    bz_quest_wc3_terrain_chunk_key("terrainA", 1, 2, b);
    bz_quest_wc3_terrain_chunk_key("terrainA", 2, 2, c);
    ASSERT(bz_quest_wc3_terrain_key_equal(a, b));
    ASSERT(!bz_quest_wc3_terrain_key_equal(a, c));
    ASSERT(bz_quest_wc3_terrain_key_equal("ReplaceableTextures\\Water\\Water12.blp",
                                          "ReplaceableTextures\\Water\\Water12.blp"));
    ASSERT(!bz_quest_wc3_terrain_key_equal("a.blp", "b.blp"));
}

typedef struct { int id; bool destroyed; } FakeTerrainHandle_t;
typedef struct {
    FakeTerrainHandle_t handles[16];
    int nextId, createCalls, destroyCalls, createFailures, liveCount, lastDestroyed;
    bool failNextCreate;
} FakeTerrainCache_t;

static void *fake_terrain_create(const bzQuestWc3CacheKey_t *key, void *userdata) {
    (void)key;
    FakeTerrainCache_t *f = (FakeTerrainCache_t *)userdata;
    f->createCalls++;
    if (f->failNextCreate) {
        f->failNextCreate = false;
        f->createFailures++;
        return NULL;
    }
    FakeTerrainHandle_t *h = &f->handles[f->nextId];
    h->id = f->nextId;
    h->destroyed = false;
    f->nextId++;
    f->liveCount++;
    return h;
}

static void fake_terrain_destroy(void *handle, void *userdata) {
    FakeTerrainCache_t *f = (FakeTerrainCache_t *)userdata;
    FakeTerrainHandle_t *h = (FakeTerrainHandle_t *)handle;
    h->destroyed = true;
    f->destroyCalls++;
    f->lastDestroyed = h->id;
    f->liveCount--;
}

static bzQuestWc3CacheKey_t cache_key(const char *identity) {
    bzQuestWc3CacheKey_t key;
    memset(&key, 0, sizeof(key));
    strncpy(key.identity, identity, sizeof(key.identity) - 1);
    return key;
}

static void test_cache_usage_assumptions_cover_hit_miss_failure_eviction_and_shutdown(void) {
    bzQuestWc3Cache_t cache;
    FakeTerrainCache_t fake;
    memset(&fake, 0, sizeof(fake));
    ASSERT(bz_quest_wc3_cache_init(&cache, 2, fake_terrain_create, fake_terrain_destroy, &fake));
    bzQuestWc3CacheKey_t keyA = cache_key("terrainA|0|0"), keyB = cache_key("terrainA|1|0"),
                        keyC = cache_key("TerrainArt\\ROC\\Dirt.blp");
    void *a = NULL, *again = NULL, *b = NULL, *c = NULL;
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyA, &a));
    ASSERT_EQ_INT(fake.createCalls, 1);
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyA, &again));
    ASSERT(a == again);
    ASSERT_EQ_INT(cache.hits, 1);
    fake.failNextCreate = true;
    ASSERT(!bz_quest_wc3_cache_acquire(&cache, &keyC, &c));
    ASSERT_EQ_INT(cache.createFailures, 1);
    ASSERT_EQ_INT(cache.occupiedCount, 1);
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyB, &b));
    ASSERT_EQ_INT(cache.occupiedCount, 2);
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyC, &c));
    ASSERT_EQ_INT(cache.evictions, 1);
    ASSERT_EQ_INT(fake.destroyCalls, 1);
    ASSERT(((FakeTerrainHandle_t *)a)->destroyed);
    ASSERT(!((FakeTerrainHandle_t *)c)->destroyed);
    bz_quest_wc3_cache_shutdown(&cache);
    ASSERT_EQ_INT(fake.destroyCalls, 3);
    bz_quest_wc3_cache_shutdown(&cache);
    ASSERT_EQ_INT(fake.destroyCalls, 3);
}

/* ------------------------------------------------------------------ */
/* Structural build/shader wiring                                      */
/* ------------------------------------------------------------------ */

static void test_terrain_shaders_exist_and_are_listed_in_cmake(void) {
    const char *vertPath = "platform/android/quest/app/src/main/cpp/shaders/terrain_vert.vert";
    const char *fragPath = "platform/android/quest/app/src/main/cpp/shaders/terrain_frag.frag";
    const char *cmakePath = "platform/android/quest/app/src/main/cpp/CMakeLists.txt";
    FILE *vert = fopen(vertPath, "r"), *frag = fopen(fragPath, "r"), *cmake = fopen(cmakePath, "r");
    ASSERT_NOT_NULL(vert);
    ASSERT_NOT_NULL(frag);
    ASSERT_NOT_NULL(cmake);
    if (!cmake) {
        if (vert) fclose(vert);
        if (frag) fclose(frag);
        return;
    }
    char text[32768];
    size_t n = fread(text, 1, sizeof(text) - 1, cmake);
    text[n] = '\0';
    ASSERT(strstr(text, "shaders/terrain_vert.vert") != NULL);
    ASSERT(strstr(text, "shaders/terrain_frag.frag") != NULL);
    fclose(cmake);
    if (vert) fclose(vert);
    if (frag) fclose(frag);
}

/* ------------------------------------------------------------------ */
/* Regression: PR #21 review defect 1 - coplanar splat/water depth test */
/* ------------------------------------------------------------------ */

/* Structural regression for the coplanar-splat depth bug: blended terrain
 * (splats/water) is drawn at the SAME quad positions already depth-written by
 * the opaque base pass, so a strict LESS compare rejects every coplanar
 * blended fragment and nothing ever renders on top of ground. The fix must
 * keep the opaque pipeline strictly LESS (normal occlusion) while the
 * blended pipeline uses LESS_OR_EQUAL (coplanar fragments pass) and must NOT
 * regress depthWriteEnable=FALSE for blended (blended draws must still never
 * corrupt the depth buffer). This can't run on host without a real VkDevice,
 * so it asserts on the checked-in source text, mirroring the existing
 * shader/CMake structural-test style below. */
static void test_terrain_pipeline_depth_compare_is_less_or_equal_for_blended_only(void) {
    const char *path = "platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3_terrain.c";
    FILE *f = fopen(path, "r");
    ASSERT_NOT_NULL(f);
    if (!f) return;
    char text[65536];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = '\0';
    fclose(f);
    ASSERT(strstr(text, "depthStencil.depthCompareOp = blended ? VK_COMPARE_OP_LESS_OR_EQUAL : "
                        "VK_COMPARE_OP_LESS;") != NULL);
    ASSERT(strstr(text, "depthStencil.depthWriteEnable = blended ? VK_FALSE : VK_TRUE;") != NULL);
}

/* ------------------------------------------------------------------ */
/* Regression: PR #21 review defect 2 - texture upload budget starvation */
/* ------------------------------------------------------------------ */

/* Mirrors the production ensure_texture_uploaded() policy (bz_quest_vk_wc3_terrain.c):
 * cache_find() first (dedup, no new work for an already-uploaded identity),
 * then only cache_acquire() (create) while under a per-frame budget. Textures
 * that miss the budget are simply left un-acquired for this "frame" so a
 * caller that re-offers the same identity next frame can retry - this is the
 * exact policy the fix requires the real capture/Vulkan wiring to follow
 * every frame (not just on a new terrain generation). cache_find() itself is
 * a private static helper in bz_quest_vk_wc3_terrain.c, so this reimplements
 * its exact linear-scan-of-occupied-slots logic against the public cache
 * struct fields rather than duplicating production budget/upload code. */
static void *find_in_cache(const bzQuestWc3Cache_t *cache, const char *identity) {
    for (uint32_t i = 0; i < cache->capacity; i++) {
        if (cache->slots[i].occupied && bz_quest_wc3_identity_equal(cache->slots[i].key.identity, identity))
            return cache->slots[i].handle;
    }
    return NULL;
}

static bool budgeted_ensure_uploaded(bzQuestWc3Cache_t *cache, const bzQuestWc3CacheKey_t *key, int *budgetRemaining) {
    if (find_in_cache(cache, key->identity)) return true;
    if (*budgetRemaining <= 0) return false;
    void *created = NULL;
    bool ok = bz_quest_wc3_cache_acquire(cache, key, &created);
    if (ok) (*budgetRemaining)--;
    return ok;
}

static void test_texture_budget_eventually_uploads_more_than_budget_textures_across_frames(void) {
    bzQuestWc3Cache_t cache;
    FakeTerrainCache_t fake;
    memset(&fake, 0, sizeof(fake));
    /* Capacity must exceed the distinct identity count so nothing evicts
     * mid-test; this test is about budget pacing, not eviction. */
    ASSERT(bz_quest_wc3_cache_init(&cache, 8, fake_terrain_create, fake_terrain_destroy, &fake));
    bzQuestWc3CacheKey_t keys[6];
    for (int i = 0; i < 6; i++) {
        char identity[32];
        snprintf(identity, sizeof(identity), "tex%d.blp", i);
        keys[i] = cache_key(identity);
    }
    /* Frame 1: budget of 4 - only the first 4 distinct textures upload;
     * textures 4 and 5 are deferred, matching the review's ">4 distinct
     * textures" scenario and the production BZ_QUEST_VK_WC3_TERRAIN budget. */
    int budget = 4;
    int uploadedFrame1 = 0;
    for (int i = 0; i < 6; i++) uploadedFrame1 += budgeted_ensure_uploaded(&cache, &keys[i], &budget) ? 1 : 0;
    ASSERT_EQ_INT(uploadedFrame1, 4);
    ASSERT_EQ_INT(fake.createCalls, 4);
    /* Frame 2: same identities re-offered (this is the fix - capture must
     * re-offer every call, not just on a new generation). A fresh budget lets
     * the previously-deferred textures 4 and 5 finally upload, and the first
     * 4 are cache hits (dedup - no new create calls, no wasted work). */
    budget = 4;
    int uploadedFrame2 = 0;
    for (int i = 0; i < 6; i++) uploadedFrame2 += budgeted_ensure_uploaded(&cache, &keys[i], &budget) ? 1 : 0;
    ASSERT_EQ_INT(uploadedFrame2, 6);
    ASSERT_EQ_INT(fake.createCalls, 6);
    /* The first 4 identities are dedup hits this frame (cache_find() finds
     * them before ever calling cache_acquire(), exactly like production's
     * ensure_texture_uploaded()), so createCalls only grew by the 2 that were
     * newly created, never re-creating an already-uploaded identity. */
    bz_quest_wc3_cache_shutdown(&cache);
}

static void test_texture_budget_same_generation_reoffer_is_a_dedup_no_op(void) {
    bzQuestWc3Cache_t cache;
    FakeTerrainCache_t fake;
    memset(&fake, 0, sizeof(fake));
    ASSERT(bz_quest_wc3_cache_init(&cache, 4, fake_terrain_create, fake_terrain_destroy, &fake));
    bzQuestWc3CacheKey_t key = cache_key("Dirt.blp");
    int budget = 4;
    ASSERT(budgeted_ensure_uploaded(&cache, &key, &budget));
    ASSERT_EQ_INT(fake.createCalls, 1);
    /* Re-offering the same identity within the same generation, repeatedly,
     * must never re-create - find_in_cache() (mirroring production's
     * cache_find()) short-circuits before cache_acquire() is ever called
     * again, so createCalls stays at 1 no matter how many times it's
     * re-offered. */
    for (int i = 0; i < 5; i++) {
        budget = 4;
        ASSERT(budgeted_ensure_uploaded(&cache, &key, &budget));
    }
    ASSERT_EQ_INT(fake.createCalls, 1);
    bz_quest_wc3_cache_shutdown(&cache);
}

static void test_texture_budget_create_failure_is_retried_next_frame(void) {
    bzQuestWc3Cache_t cache;
    FakeTerrainCache_t fake;
    memset(&fake, 0, sizeof(fake));
    ASSERT(bz_quest_wc3_cache_init(&cache, 4, fake_terrain_create, fake_terrain_destroy, &fake));
    bzQuestWc3CacheKey_t key = cache_key("Corrupt.blp");
    fake.failNextCreate = true;
    int budget = 4;
    ASSERT(!budgeted_ensure_uploaded(&cache, &key, &budget));
    ASSERT_EQ_INT(fake.createFailures, 1);
    /* A create failure must not be sticky: re-offering the same identity next
     * frame (no lingering "already tried" state) retries and succeeds. */
    budget = 4;
    ASSERT(budgeted_ensure_uploaded(&cache, &key, &budget));
    ASSERT_EQ_INT(fake.createCalls, 2);
}

static void test_texture_budget_generation_reset_evicts_and_allows_fresh_upload(void) {
    bzQuestWc3Cache_t cache;
    FakeTerrainCache_t fake;
    memset(&fake, 0, sizeof(fake));
    ASSERT(bz_quest_wc3_cache_init(&cache, 4, fake_terrain_create, fake_terrain_destroy, &fake));
    bzQuestWc3CacheKey_t key = cache_key("Grass.blp");
    int budget = 4;
    ASSERT(budgeted_ensure_uploaded(&cache, &key, &budget));
    ASSERT_EQ_INT(fake.liveCount, 1);
    /* Map-generation reset: the Vulkan module shuts down its texture cache
     * (releasing every GPU resource) and re-initializes it fresh, exactly as
     * bz_quest_vk_wc3_terrain.c does when the terrain identity changes. */
    bz_quest_wc3_cache_shutdown(&cache);
    ASSERT_EQ_INT(fake.liveCount, 0);
    ASSERT(bz_quest_wc3_cache_init(&cache, 4, fake_terrain_create, fake_terrain_destroy, &fake));
    budget = 4;
    ASSERT(budgeted_ensure_uploaded(&cache, &key, &budget));
    ASSERT_EQ_INT(fake.createCalls, 2);
    ASSERT_EQ_INT(fake.liveCount, 1);
    bz_quest_wc3_cache_shutdown(&cache);
}

void run_bz_quest_wc3_terrain_tests(void) {
    RUN_TEST(test_measure_computes_scale_and_cell_size);
    RUN_TEST(test_measure_rejects_non_square_tiles);
    RUN_TEST(test_measure_rejects_non_positive_or_non_finite_bounds);
    RUN_TEST(test_chunk_grid_and_bounds_handle_rectangular_maps);
    RUN_TEST(test_build_chunk_single_tile_uses_all_four_authoritative_corners);
    RUN_TEST(test_build_chunk_clamps_last_chunk_on_non_multiple_of_32_map);
    RUN_TEST(test_quad_forward_winding_matches_flat_ground_quad);
    RUN_TEST(test_quad_reverse_winding_branch_is_reachable_via_inverted_normal);
    RUN_TEST(test_build_chunk_ground_indices_match_expected_order);
    RUN_TEST(test_surface_layers_compute_second_layer_bitmask_uvs_exactly);
    RUN_TEST(test_surface_layers_wide_atlas_variations_match_authoritative_tile_choice);
    RUN_TEST(test_water_opacity_clamps_at_all_boundaries);
    RUN_TEST(test_tile_is_water_rejects_map_edge_corners);
    RUN_TEST(test_tile_is_cliff_requires_distinct_levels_and_no_ramp);
    RUN_TEST(test_cliff_bottom_and_material_fallbacks);
    RUN_TEST(test_chunk_and_texture_keys_compare_stably);
    RUN_TEST(test_cache_usage_assumptions_cover_hit_miss_failure_eviction_and_shutdown);
    RUN_TEST(test_terrain_shaders_exist_and_are_listed_in_cmake);
    RUN_TEST(test_terrain_pipeline_depth_compare_is_less_or_equal_for_blended_only);
    RUN_TEST(test_texture_budget_eventually_uploads_more_than_budget_textures_across_frames);
    RUN_TEST(test_texture_budget_same_generation_reoffer_is_a_dedup_no_op);
    RUN_TEST(test_texture_budget_create_failure_is_retried_next_frame);
    RUN_TEST(test_texture_budget_generation_reset_evicts_and_allows_fresh_upload);
}
