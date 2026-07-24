#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "platform/bridge/bz_tabletop_assets.h"
#include "platform/bridge/bz_tabletop_assets_internal.h"
#include "platform/bridge/bz_tabletop_transport.h"
#include "test_framework.h"
#include "wc3_tabletop_assets_internal.h"

#define FOURCC(a,b,c,d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)

struct bzTTSnapshot {
    char configstrings[16][BZ_TT_MAX_CONFIGSTRING_LEN];
};

void test_assets_set_tft(bool enabled);
void test_assets_block_reads(bool blocked);
void test_assets_wait_for_blocked_reads(unsigned count);
void test_assets_set_configstring(struct bzTTSnapshot *snapshot, uint32_t index, const char *value);

static void reset_assets(void) {
    BZ_TTA_Shutdown();
    BZ_TTA_Init();
}

static void test_abi_and_asymmetric_blp_orientation(void) {
    struct bzTTSnapshot snapshot = { 0 };
    const bzTTAsset_t *asset;
    bzTTImageInfo_t info;
    uint8_t pixels[16];
    reset_assets();
    ASSERT_EQ_INT(BZ_TTA_AbiVersion(), BZ_TABLETOP_ASSETS_ABI_VERSION);
    ASSERT_EQ_INT(sizeof(bzTTAssetMetadata_t), 36);
    ASSERT_EQ_INT(sizeof(bzTTImageInfo_t), 24);
    ASSERT_EQ_INT(sizeof(bzTTMaterialLayerInfo_t), 24);
    ASSERT_EQ_INT(sizeof(bzTTTerrainCorner_t), 20);
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/orientation_2x2.blp");
    asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_NOT_NULL(asset);
    ASSERT(!BZ_TTAsset_IsPlaceholder(asset));
    ASSERT(BZ_TTAsset_ImageInfo(asset, &info));
    ASSERT_EQ_INT(info.width, 2);
    ASSERT_EQ_INT(info.height, 2);
    ASSERT_EQ_INT(info.origin, BZ_TTA_ORIGIN_TOP_LEFT);
    ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(asset, pixels, sizeof(pixels)), sizeof(pixels));
    ASSERT_EQ_INT(pixels[0], 255); ASSERT_EQ_INT(pixels[1], 0); ASSERT_EQ_INT(pixels[2], 0);
    ASSERT_EQ_INT(pixels[4], 0); ASSERT_EQ_INT(pixels[5], 255); ASSERT_EQ_INT(pixels[6], 0);
    ASSERT_EQ_INT(pixels[8], 0); ASSERT_EQ_INT(pixels[9], 0); ASSERT_EQ_INT(pixels[10], 255);
    BZ_TTAsset_Release(asset);
}

static void test_blp1_paletted_decode(void) {
    enum { HEADER = 156, PALETTE = 1024, DATA = HEADER + PALETTE };
    uint8_t file[DATA + 2];
    uint32_t value;
    bzTTAResult_t status = BZ_TTA_OK;
    bzTTAsset_t *asset;
    uint8_t pixel[4];
    memset(file, 0, sizeof(file));
    value = 0x31504c42u; memcpy(file, &value, 4);
    value = 1; memcpy(file + 4, &value, 4);
    value = 8; memcpy(file + 8, &value, 4);
    value = 1; memcpy(file + 12, &value, 4); memcpy(file + 16, &value, 4);
    value = DATA; memcpy(file + 28, &value, 4);
    value = 2; memcpy(file + 92, &value, 4);
    file[HEADER + 0] = 3; file[HEADER + 1] = 2; file[HEADER + 2] = 1; file[HEADER + 3] = 255;
    file[DATA] = 0; file[DATA + 1] = 127;
    asset = BZ_WC3_TTA_DecodeBLP(file, sizeof(file), "test.blp", NULL, &status);
    ASSERT_NOT_NULL(asset);
    ASSERT_EQ_INT(status, BZ_TTA_OK);
    ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(asset, pixel, sizeof(pixel)), 4);
    ASSERT_EQ_INT(pixel[0], 1); ASSERT_EQ_INT(pixel[1], 2); ASSERT_EQ_INT(pixel[2], 3);
    ASSERT_EQ_INT(pixel[3], 127);
    BZ_TTAsset_Release(asset);
}

static void test_blp1_jpeg_dimension_limit(void) {
    enum { HEADER = 156 };
    uint8_t file[HEADER + 4] = { 0 };
    uint32_t value;
    bzTTAResult_t status = BZ_TTA_OK;
    value = FOURCC('B','L','P','1'); memcpy(file, &value, 4);
    value = 0; memcpy(file + 4, &value, 4);
    value = 9000; memcpy(file + 12, &value, 4);
    value = 1; memcpy(file + 16, &value, 4);
    value = sizeof(file); memcpy(file + 28, &value, 4);
    ASSERT_NULL(BZ_WC3_TTA_DecodeBLP(file, sizeof(file), "oversized-jpeg.blp", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
}

static void test_roc_tft_resolution_and_cache(void) {
    struct bzTTSnapshot snapshot = { 0 };
    const bzTTAsset_t *roc, *roc_again, *tft;
    uint8_t pixels[16];
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/variant.blp");
    test_assets_set_tft(false);
    reset_assets();
    roc = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                      BZ_TTA_ASSET_IMAGE, NULL);
    roc_again = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                            BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT(roc == roc_again);
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1);
    ASSERT_EQ_INT(BZ_TTA_CacheHits(), 1);
    ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(roc, pixels, sizeof(pixels)), 4);
    ASSERT_EQ_INT(pixels[0], 255); ASSERT_EQ_INT(pixels[1], 255); ASSERT_EQ_INT(pixels[2], 255);
    BZ_TTAsset_Release(roc); BZ_TTAsset_Release(roc_again);
    test_assets_set_tft(true);
    reset_assets();
    tft = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                      BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(tft, pixels, sizeof(pixels)), sizeof(pixels));
    ASSERT_EQ_INT(pixels[0], 255); ASSERT_EQ_INT(pixels[1], 0);
    BZ_TTAsset_Release(tft);
}

static void test_placeholder_path_confinement_and_log_once_cache(void) {
    struct bzTTSnapshot snapshot = { 0 };
    const bzTTAsset_t *missing, *again, *confined, *empty_component;
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/missing.blp");
    test_assets_set_configstring(&snapshot, 2, "../outside.blp");
    test_assets_set_configstring(&snapshot, 3, "TestUI//Textures/missing.blp");
    reset_assets();
    missing = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                          BZ_TTA_ASSET_IMAGE, NULL);
    again = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_IMAGE, NULL);
    confined = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 2,
                                           BZ_TTA_ASSET_IMAGE, NULL);
    empty_component = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 3,
                                                  BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_NOT_NULL(missing); ASSERT(missing == again); ASSERT(BZ_TTAsset_IsPlaceholder(missing));
    ASSERT_EQ_INT(BZ_TTAsset_Status(missing), BZ_TTA_ERR_NOT_FOUND);
    ASSERT(BZ_TTAsset_IsPlaceholder(confined));
    ASSERT_EQ_INT(BZ_TTAsset_Status(confined), BZ_TTA_ERR_PATH_CONFINEMENT);
    ASSERT_EQ_INT(BZ_TTAsset_Status(empty_component), BZ_TTA_ERR_PATH_CONFINEMENT);
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 3); ASSERT_EQ_INT(BZ_TTA_CacheHits(), 1);
    ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 3);
    BZ_TTAsset_Release(missing); BZ_TTAsset_Release(again); BZ_TTAsset_Release(confined);
    BZ_TTAsset_Release(empty_component);
}

static void test_mdx_geometry_materials_sequences_and_bounds(void) {
    struct bzTTSnapshot snapshot = { 0 };
    bzTTAssetMetadata_t metadata = {
        .category = BZ_TTA_CATEGORY_BUILDING, .class_id = 0x68746f77, .team_color = 3,
        .tint_r = 1, .tint_g = 0.5f, .tint_b = 0.25f, .tint_a = 1,
        .footprint_x = 64, .footprint_y = 96,
    };
    const bzTTAsset_t *asset;
    bzTTModelInfo_t model = { 0 };
    bzTTGeosetInfo_t geoset;
    bzTTMaterialInfo_t material;
    bzTTMaterialLayerInfo_t layer;
    bzTTModelTextureInfo_t texture;
    bzTTSequenceInfo_t sequence;
    bzTTNodeInfo_t node;
    bzTTAssetMetadata_t texture_metadata;
    const bzTTAsset_t *image;
    uint16_t indices[6];
    reset_assets();
    test_assets_set_configstring(&snapshot, 1, "TestUI/Models/quad_sprite.mdx");
    asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_MODEL, &metadata);
    ASSERT_NOT_NULL(asset); ASSERT(!BZ_TTAsset_IsPlaceholder(asset));
    ASSERT(BZ_TTAsset_ModelInfo(asset, &model));
    ASSERT_EQ_INT(model.geoset_count, 1); ASSERT_EQ_INT(model.material_count, 1);
    ASSERT_EQ_INT(model.layer_count, 1); ASSERT_EQ_INT(model.texture_count, 1);
    ASSERT_EQ_INT(model.sequence_count, 1); ASSERT_EQ_INT(model.node_count, 1);
    ASSERT_EQ_FLOAT(model.bounds.min.x, -0.5f, 0.001f);
    ASSERT_EQ_FLOAT(model.bounds.max.x, 0.5f, 0.001f);
    ASSERT(BZ_TTAsset_GeosetInfo(asset, 0, &geoset));
    ASSERT_EQ_INT(geoset.vertex_count, 4); ASSERT_EQ_INT(geoset.index_count, 6);
    ASSERT_EQ_INT(BZ_TTAsset_CopyGeosetIndices(asset, 0, indices, 6), 6);
    ASSERT_EQ_INT(indices[5], 3);
    ASSERT(BZ_TTAsset_MaterialInfo(asset, 0, &material));
    ASSERT(BZ_TTAsset_MaterialLayerInfo(asset, material.first_layer, &layer));
    ASSERT_EQ_INT(layer.texture_index, 0);
    ASSERT(BZ_TTAsset_ModelTextureInfo(asset, 0, &texture));
    ASSERT_STR_EQ(texture.identity, "TestUI/Textures/checker_8x8.blp");
    image = BZ_TTA_RegisterModelTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, asset, 0);
    ASSERT_NOT_NULL(image); ASSERT(!BZ_TTAsset_IsPlaceholder(image));
    ASSERT(BZ_TTAsset_Metadata(image, &texture_metadata));
    ASSERT_EQ_INT(texture_metadata.category, BZ_TTA_CATEGORY_BUILDING);
    ASSERT(BZ_TTAsset_SequenceInfo(asset, 0, &sequence)); ASSERT_STR_EQ(sequence.name, "Stand");
    ASSERT(BZ_TTAsset_NodeInfo(asset, 0, &node)); ASSERT_STR_EQ(node.name, "Bone_Root");
    BZ_TTAsset_Release(image); BZ_TTAsset_Release(asset);
}

/* Duplicate retail-shaped records inside MTLS/GEOS chunks to exercise inclusive record boundaries. */
static uint8_t *duplicate_mdx_records(const uint8_t *src, size_t size, size_t *out_size) {
    size_t extra = 0, pos = 4, dst_pos = 4;
    uint8_t *dst;
    while (pos + 8 <= size) {
        uint32_t tag, bytes;
        memcpy(&tag, src + pos, 4); memcpy(&bytes, src + pos + 4, 4);
        if ((size_t)bytes > size - pos - 8) return NULL;
        if (tag == FOURCC('M','T','L','S') || tag == FOURCC('G','E','O','S')) extra += bytes;
        pos += 8 + bytes;
    }
    if (pos != size || extra > SIZE_MAX - size || !(dst = malloc(size + extra))) return NULL;
    memcpy(dst, src, 4); pos = 4;
    while (pos < size) {
        uint32_t tag, bytes, output_bytes;
        memcpy(&tag, src + pos, 4); memcpy(&bytes, src + pos + 4, 4);
        output_bytes = bytes;
        if (tag == FOURCC('M','T','L','S') || tag == FOURCC('G','E','O','S')) output_bytes *= 2;
        memcpy(dst + dst_pos, &tag, 4); memcpy(dst + dst_pos + 4, &output_bytes, 4);
        memcpy(dst + dst_pos + 8, src + pos + 8, bytes);
        if (output_bytes != bytes) memcpy(dst + dst_pos + 8 + bytes, src + pos + 8, bytes);
        pos += 8 + bytes; dst_pos += 8 + output_bytes;
    }
    *out_size = dst_pos;
    return dst;
}

static void test_mdx_multiple_inclusive_records(void) {
    DWORD size;
    uint8_t *single = FS_ReadFile("TestUI/Models/quad_sprite.mdx", &size);
    size_t multi_size;
    uint8_t *multi;
    bzTTAResult_t status = BZ_TTA_OK;
    bzTTAsset_t *asset;
    bzTTModelInfo_t model = { 0 };
    ASSERT_NOT_NULL(single);
    multi = duplicate_mdx_records(single, size, &multi_size);
    FS_FreeFile(single);
    ASSERT_NOT_NULL(multi);
    asset = BZ_WC3_TTA_DecodeMDX(multi, multi_size, "multi.mdx", NULL, &status);
    free(multi);
    ASSERT_NOT_NULL(asset);
    ASSERT_EQ_INT(status, BZ_TTA_OK);
    ASSERT(BZ_TTAsset_ModelInfo(asset, &model));
    ASSERT_EQ_INT(model.geoset_count, 2); ASSERT_EQ_INT(model.material_count, 2);
    ASSERT_EQ_INT(model.layer_count, 2);
    BZ_TTAsset_Release(asset);
}

static void test_mdx_zero_counted_array_is_malformed(void) {
    uint8_t file[36] = { 'M','D','L','X', 'V','E','R','S', 4,0,0,0, 32,3,0,0,
                         'G','E','O','S', 12,0,0,0, 12,0,0,0, 'V','R','T','X', 0,0,0,0 };
    bzTTAResult_t status = BZ_TTA_OK;
    ASSERT_NULL(BZ_WC3_TTA_DecodeMDX(file, sizeof(file), "zero.mdx", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
}

static void test_malformed_blp_and_mdx_bounds(void) {
    uint8_t malformed_blp[32] = { 'B', 'L', 'P', '2' };
    uint8_t malformed_mdx[12] = { 'M', 'D', 'L', 'X', 'G', 'E', 'O', 'S', 0xff, 0xff, 0xff, 0x7f };
    bzTTAResult_t status = BZ_TTA_OK;
    ASSERT_NULL(BZ_WC3_TTA_DecodeBLP(malformed_blp, sizeof(malformed_blp), "bad.blp", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
    status = BZ_TTA_OK;
    ASSERT_NULL(BZ_WC3_TTA_DecodeMDX(malformed_mdx, sizeof(malformed_mdx), "bad.mdx", NULL, &status));
    ASSERT_EQ_INT(status, BZ_TTA_ERR_MALFORMED);
}

static void make_terrain(uint32_t width, uint32_t height, uintptr_t salt) {
    LPWAR3MAP map = calloc(1, sizeof(*map));
    map->width = width; map->height = height; map->center = (VECTOR2){ -2048, -1024 };
    map->num_grounds = 2; map->num_cliffs = 1;
    map->grounds = calloc(2, sizeof(DWORD)); map->cliffs = calloc(1, sizeof(DWORD));
    map->vertices = calloc((size_t)width * height, sizeof(WAR3MAPVERTEX));
    map->grounds[0] = FOURCC('L','d','r','t'); map->grounds[1] = FOURCC('L','g','r','s');
    map->cliffs[0] = FOURCC('C','L','i','f');
    for (uint32_t y = 0; y < height; y++) for (uint32_t x = 0; x < width; x++) {
        LPWAR3MAPVERTEX corner = (LPWAR3MAPVERTEX)map->vertices + y * width + x;
        corner->accurate_height = (USHORT)(0x2000 + x + y + salt);
        corner->waterlevel = (USHORT)(0x2000 + 8);
        corner->ground = (BYTE)((x + y) & 1); corner->groundVariation = (BYTE)(x & 31);
        corner->cliff = 0; corner->cliffVariation = (BYTE)(y & 7); corner->level = (BYTE)(y & 3);
        corner->water = x == 1 && y == 1; corner->ramp = x == 2 && y == 2;
    }
    world.map = map;
}

static void free_terrain(void) {
    if (!world.map) return;
    free(world.map->grounds); free(world.map->cliffs); free(world.map->vertices); free(world.map);
    world.map = NULL;
}

static void test_terrain_dimensions_corners_water_cliffs_and_chunks(void) {
    const bzTTTerrain_t *terrain, *again;
    bzTTTerrainInfo_t info;
    bzTTTerrainCorner_t corner;
    uint32_t ground, cliff;
    make_terrain(34, 34, 0);
    reset_assets();
    BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain(); again = BZ_TTA_LatestTerrain();
    ASSERT_NOT_NULL(terrain); ASSERT(terrain == again); ASSERT(BZ_TTTerrain_Info(terrain, &info));
    ASSERT_EQ_INT(info.width, 34); ASSERT_EQ_INT(info.tile_width, 33);
    ASSERT_EQ_INT(info.chunk_tiles, 32); ASSERT_EQ_INT(info.chunk_count_x, 2);
    ASSERT_EQ_FLOAT(info.min_x, -2048, 0.001f);
    ASSERT(BZ_TTTerrain_Corner(terrain, 1, 1, &corner));
    ASSERT(corner.flags & BZ_TTA_TERRAIN_WATER);
    ASSERT_EQ_INT(corner.ground_id, FOURCC('L','d','r','t'));
    ASSERT_EQ_FLOAT(corner.water_height, -78, 0.001f);
    ASSERT(BZ_TTTerrain_GroundType(terrain, 1, &ground)); ASSERT_EQ_INT(ground, FOURCC('L','g','r','s'));
    ASSERT(BZ_TTTerrain_CliffType(terrain, 0, &cliff)); ASSERT_EQ_INT(cliff, FOURCC('C','L','i','f'));
    BZ_TTTerrain_Release(terrain); BZ_TTTerrain_Release(again);
    free_terrain();
}

static void test_malformed_terrain_type_index(void) {
    const bzTTTerrain_t *terrain;
    make_terrain(4, 4, 0);
    ((LPWAR3MAPVERTEX)world.map->vertices)[0].ground = 7;
    reset_assets(); BZ_TTA_PublishTerrainFromGame(); BZ_TTA_PublishTerrainFromGame();
    terrain = BZ_TTA_LatestTerrain();
    ASSERT_NULL(terrain);
    free_terrain();
}

typedef struct {
    const bzTTAsset_t *asset;
    atomic_bool *running;
} readerCtx_t;

static void *asset_reader(void *opaque) {
    readerCtx_t *ctx = opaque;
    bzTTImageInfo_t info;
    uint8_t pixels[16];
    while (atomic_load(ctx->running)) {
        BZ_TTAsset_Retain(ctx->asset);
        BZ_TTAsset_ImageInfo(ctx->asset, &info);
        BZ_TTAsset_CopyImagePixels(ctx->asset, pixels, sizeof(pixels));
        BZ_TTAsset_Release(ctx->asset);
    }
    return NULL;
}

static void test_concurrent_readers_and_shutdown_lifetime(void) {
    struct bzTTSnapshot snapshot = { 0 };
    const bzTTAsset_t *asset;
    pthread_t threads[4];
    atomic_bool running = true;
    readerCtx_t ctx;
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/orientation_2x2.blp");
    reset_assets();
    asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_NOT_NULL(asset);
    ctx = (readerCtx_t){ asset, &running };
    for (int i = 0; i < 4; i++) ASSERT_EQ_INT(pthread_create(&threads[i], NULL, asset_reader, &ctx), 0);
    BZ_TTA_Shutdown(); /* Outstanding caller reference and reader retains remain valid. */
    atomic_store(&running, false);
    for (int i = 0; i < 4; i++) ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
    {
        uint8_t pixels[16];
        ASSERT_EQ_INT(BZ_TTAsset_CopyImagePixels(asset, pixels, sizeof(pixels)), sizeof(pixels));
    }
    BZ_TTAsset_Release(asset);
}

typedef struct {
    struct bzTTSnapshot *snapshot;
    const bzTTAsset_t *asset;
} registrationCtx_t;

static void *blocked_registration(void *opaque) {
    registrationCtx_t *ctx = opaque;
    ctx->asset = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, ctx->snapshot, 1,
                                              BZ_TTA_ASSET_IMAGE, NULL);
    return NULL;
}

static void test_inflight_load_cannot_cross_restart(void) {
    struct bzTTSnapshot snapshot = { 0 };
    registrationCtx_t ctx = { .snapshot = &snapshot };
    const bzTTAsset_t *fresh;
    pthread_t thread;
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/orientation_2x2.blp");
    reset_assets();
    test_assets_block_reads(true);
    ASSERT_EQ_INT(pthread_create(&thread, NULL, blocked_registration, &ctx), 0);
    test_assets_wait_for_blocked_reads(1);
    BZ_TTA_Shutdown(); BZ_TTA_Init();
    test_assets_block_reads(false);
    ASSERT_EQ_INT(pthread_join(thread, NULL), 0);
    ASSERT_NULL(ctx.asset);
    fresh = BZ_TTA_RegisterConfigString(BZ_TABLETOP_ASSETS_ABI_VERSION, &snapshot, 1,
                                        BZ_TTA_ASSET_IMAGE, NULL);
    ASSERT_NOT_NULL(fresh); ASSERT_EQ_INT(BZ_TTA_CacheMisses(), 1);
    BZ_TTAsset_Release(fresh);
}

static void test_concurrent_missing_asset_logs_once(void) {
    enum { THREADS = 8 };
    struct bzTTSnapshot snapshot = { 0 };
    registrationCtx_t ctx[THREADS];
    pthread_t threads[THREADS];
    test_assets_set_configstring(&snapshot, 1, "TestUI/Textures/concurrent-missing.blp");
    reset_assets(); test_assets_block_reads(true);
    for (int i = 0; i < THREADS; i++) {
        ctx[i] = (registrationCtx_t){ .snapshot = &snapshot };
        ASSERT_EQ_INT(pthread_create(&threads[i], NULL, blocked_registration, ctx + i), 0);
    }
    test_assets_wait_for_blocked_reads(THREADS);
    test_assets_block_reads(false);
    for (int i = 0; i < THREADS; i++) ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
    for (int i = 0; i < THREADS; i++) {
        ASSERT_NOT_NULL(ctx[i].asset); ASSERT(ctx[i].asset == ctx[0].asset);
        BZ_TTAsset_Release(ctx[i].asset);
    }
    ASSERT_EQ_INT(BZ_TTA_CacheMisses(), THREADS);
    ASSERT_EQ_INT(BZ_TTA_PlaceholderLogs(), 1);
}

typedef struct {
    atomic_bool *running;
    atomic_uint *reads;
} terrainReaderCtx_t;

static void *terrain_reader(void *opaque) {
    terrainReaderCtx_t *ctx = opaque;
    while (atomic_load(ctx->running)) {
        const bzTTTerrain_t *terrain = BZ_TTA_LatestTerrain();
        if (terrain) {
            bzTTTerrainCorner_t corner;
            BZ_TTTerrain_Corner(terrain, 0, 0, &corner);
            atomic_fetch_add(ctx->reads, 1);
            BZ_TTTerrain_Release(terrain);
        }
    }
    return NULL;
}

static void *terrain_publisher(void *opaque) {
    atomic_bool *running = opaque;
    while (atomic_load(running)) BZ_TTA_PublishTerrainFromGame();
    return NULL;
}

static void test_cleanup_publish_race(void) {
    pthread_t reader, publisher;
    atomic_bool running = true;
    atomic_uint reads = 0;
    terrainReaderCtx_t ctx = { &running, &reads };
    make_terrain(4, 4, 0);
    reset_assets(); BZ_TTA_PublishTerrainFromGame();
    ASSERT_EQ_INT(pthread_create(&reader, NULL, terrain_reader, &ctx), 0);
    ASSERT_EQ_INT(pthread_create(&publisher, NULL, terrain_publisher, &running), 0);
    for (int i = 0; i < 10000 && !atomic_load(&reads); i++) sched_yield();
    for (int i = 0; i < 8; i++) {
        BZ_TTA_Shutdown(); BZ_TTA_Init(); BZ_TTA_PublishTerrainFromGame();
    }
    atomic_store(&running, false);
    ASSERT_EQ_INT(pthread_join(reader, NULL), 0);
    ASSERT_EQ_INT(pthread_join(publisher, NULL), 0);
    ASSERT(atomic_load(&reads) > 0);
    free_terrain();
}

void run_bz_tabletop_assets_tests(void) {
    RUN_TEST(test_abi_and_asymmetric_blp_orientation);
    RUN_TEST(test_blp1_paletted_decode);
    RUN_TEST(test_blp1_jpeg_dimension_limit);
    RUN_TEST(test_roc_tft_resolution_and_cache);
    RUN_TEST(test_placeholder_path_confinement_and_log_once_cache);
    RUN_TEST(test_mdx_geometry_materials_sequences_and_bounds);
    RUN_TEST(test_mdx_multiple_inclusive_records);
    RUN_TEST(test_mdx_zero_counted_array_is_malformed);
    RUN_TEST(test_malformed_blp_and_mdx_bounds);
    RUN_TEST(test_terrain_dimensions_corners_water_cliffs_and_chunks);
    RUN_TEST(test_malformed_terrain_type_index);
    RUN_TEST(test_concurrent_readers_and_shutdown_lifetime);
    RUN_TEST(test_inflight_load_cannot_cross_restart);
    RUN_TEST(test_concurrent_missing_asset_logs_once);
    RUN_TEST(test_cleanup_publish_race);
    BZ_TTA_Shutdown();
}
