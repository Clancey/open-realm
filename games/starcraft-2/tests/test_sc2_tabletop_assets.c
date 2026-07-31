/*
 * test_sc2_tabletop_assets.c - Layer 2A SC2 asset ABI coverage: ABI constants, lifecycle,
 * bounds/dimension rejection, cache hit/miss, placeholder/log-once, retained-handle survival
 * across reload/shutdown, mask-layer copy, and path confinement.
 *
 * Self-contained: this is the only translation unit providing BZ_SC2_TTA_Source() here, so it
 * supplies its own fabricated terrain/image fixtures (via BZ_SC2A_TerrainAlloc/TerrainData and
 * a small in-memory DDS builder) instead of the real sc2_map.c parser or MPQ fixtures. It also
 * defines its own main(), so it links and runs standalone without the shared test_sc2_main.c
 * aggregator.
 */
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include "games/starcraft-2/visionos/sc2_tabletop_assets.h"
#include "games/starcraft-2/visionos/sc2_tabletop_assets_internal.h"
#include "test_framework.h"

int _tests_run = 0, _tests_failed = 0;

_Static_assert(BZ_SC2A_OK == 0 && BZ_SC2A_ERR_NOT_INITIALIZED == 1 && BZ_SC2A_ERR_TERMINAL == 2 &&
               BZ_SC2A_ERR_ABI_VERSION == 3 && BZ_SC2A_ERR_INVALID_ARGUMENT == 4 &&
               BZ_SC2A_ERR_PATH_CONFINEMENT == 5 && BZ_SC2A_ERR_NOT_FOUND == 6 &&
               BZ_SC2A_ERR_MALFORMED == 7 && BZ_SC2A_ERR_UNSUPPORTED == 8 &&
               BZ_SC2A_ERR_TOO_LARGE == 9 && BZ_SC2A_ERR_OUT_OF_MEMORY == 10, "result ABI changed");
_Static_assert(BZ_SC2A_PIXEL_DXT1 == 1 && BZ_SC2A_PIXEL_DXT3 == 2 && BZ_SC2A_PIXEL_DXT5 == 3 &&
               BZ_SC2A_PIXEL_RGB8 == 4 && BZ_SC2A_PIXEL_BGR8 == 5 && BZ_SC2A_PIXEL_RGBA8 == 6 &&
               BZ_SC2A_PIXEL_BGRA8 == 7, "pixel ABI changed");
_Static_assert(BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE == 1 && BZ_SC2A_TERRAIN_CHANNEL_NORMAL == 2,
               "terrain channel ABI changed");
_Static_assert(sizeof(bzSC2ATerrainInfo_t) == 104 && offsetof(bzSC2ATerrainInfo_t, malformed_flags) == 12,
               "terrain info ABI changed");
_Static_assert(sizeof(bzSC2ATerrainTextureInfo_t) == 520 && sizeof(bzSC2ACliffSetInfo_t) == 136 &&
               sizeof(bzSC2ACliffCellInfo_t) == 16 && sizeof(bzSC2AHeightSample_t) == 12 &&
               sizeof(bzSC2ACellInfo_t) == 4, "terrain record ABI changed");
_Static_assert(sizeof(bzSC2AImageInfo_t) == 32 && offsetof(bzSC2AImageInfo_t, origin) == 24 &&
               sizeof(bzSC2AImageMipInfo_t) == 20, "image ABI changed");

/* ---- Fixture DDS buffers (mirrors games/starcraft-2/tests/test_sc2_dds.c's header layout) -- */

#define TEST_DDPF_FOURCC 0x00000004u
#define TEST_DDPF_RGB    0x00000040u
#define TEST_DDSD_PITCH  0x00000008u

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* One valid single-mip DXT1 DDS file: 128-byte header + 8-byte block payload for a 4x4 image. */
static void *build_valid_dxt1(uint32_t *out_size) {
    uint8_t *buf = calloc(1, 128 + 8);
    buf[0] = 'D'; buf[1] = 'D'; buf[2] = 'S'; buf[3] = ' ';
    put_u32(buf + 4, 124);
    put_u32(buf + 12, 4);  /* height */
    put_u32(buf + 16, 4);  /* width */
    put_u32(buf + 28, 1);  /* mipMapCount */
    put_u32(buf + 76, 32); /* DDS_PIXELFORMAT.dwSize */
    put_u32(buf + 80, TEST_DDPF_FOURCC);
    buf[84] = 'D'; buf[85] = 'X'; buf[86] = 'T'; buf[87] = '1';
    memset(buf + 128, 0xAB, 8);
    *out_size = 128 + 8;
    return buf;
}

static void *build_rgb24_odd(uint32_t *out_size, bool padded) {
    uint32_t payload = padded ? 12 : 9;
    uint8_t *buf = calloc(1, 128 + payload);
    buf[0] = 'D'; buf[1] = 'D'; buf[2] = 'S'; buf[3] = ' ';
    put_u32(buf + 4, 124);
    if (padded) { put_u32(buf + 8, TEST_DDSD_PITCH); put_u32(buf + 20, 4); }
    put_u32(buf + 12, 3);
    put_u32(buf + 16, 1);
    put_u32(buf + 28, 1);
    put_u32(buf + 76, 32);
    put_u32(buf + 80, TEST_DDPF_RGB);
    put_u32(buf + 88, 24);
    put_u32(buf + 92, 0x000000FF);
    put_u32(buf + 96, 0x0000FF00);
    put_u32(buf + 100, 0x00FF0000);
    for (uint32_t i = 0; i < payload; i++) buf[128 + i] = (uint8_t)(i + 1);
    *out_size = 128 + payload;
    return buf;
}

static void *build_bad_magic(uint32_t *out_size) {
    uint8_t *buf = calloc(1, 128);
    buf[0] = 'X'; buf[1] = 'X'; buf[2] = 'X'; buf[3] = 'X';
    *out_size = 128;
    return buf;
}

/* Header-valid but width/height beyond SC2_DDS_MAX_DIMENSION (16384); classify_dds_failure()
 * must recognize this as TOO_LARGE without needing SC2_DdsParse() to say so directly. */
static void *build_huge_dimension(uint32_t *out_size) {
    uint8_t *buf = calloc(1, 128);
    buf[0] = 'D'; buf[1] = 'D'; buf[2] = 'S'; buf[3] = ' ';
    put_u32(buf + 4, 124);
    put_u32(buf + 12, 20000);
    put_u32(buf + 16, 20000);
    put_u32(buf + 28, 1);
    put_u32(buf + 76, 32);
    put_u32(buf + 80, TEST_DDPF_FOURCC);
    buf[84] = 'D'; buf[85] = 'X'; buf[86] = 'T'; buf[87] = '1';
    *out_size = 128;
    return buf;
}

/* ---- Fixture source table --------------------------------------------------------------- */

typedef enum { FIXTURE_SHAPE_OK, FIXTURE_SHAPE_TOO_LARGE, FIXTURE_SHAPE_MALFORMED } fixture_shape_t;

static uint32_t g_fixture_generation;
static uint32_t g_fixture_copy_count;
static fixture_shape_t g_fixture_shape = FIXTURE_SHAPE_OK;
static pthread_mutex_t g_terrain_image_hook_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_terrain_image_hook_cond = PTHREAD_COND_INITIALIZER;
static bool g_terrain_image_hook_blocked, g_terrain_image_hook_entered;
static char g_tex0_diffuse[BZ_SC2A_MAX_IDENTITY] = "Assets\\Textures\\good.dds";
static char g_tex0_normal[BZ_SC2A_MAX_IDENTITY] = "Assets\\Textures\\missing.dds";
static char g_tex1_diffuse[BZ_SC2A_MAX_IDENTITY] = "Assets\\Textures\\malformed.dds";
static char g_tex1_normal[BZ_SC2A_MAX_IDENTITY] = "Assets\\Textures\\huge.dds";
static const uint8_t g_fixture_mask[4] = { 0, 1, 2, 3 }; /* one decoded byte per texel, 2x2 layer */

static void *fixture_read_file(const char *identity, uint32_t *size) {
    if (!strcmp(identity, "Assets\\Textures\\good.dds")) return build_valid_dxt1(size);
    if (!strcmp(identity, "Assets\\Textures\\rgb-tight.dds")) return build_rgb24_odd(size, false);
    if (!strcmp(identity, "Assets\\Textures\\rgb-padded.dds")) return build_rgb24_odd(size, true);
    if (!strcmp(identity, "Assets\\Textures\\malformed.dds")) return build_bad_magic(size);
    if (!strcmp(identity, "Assets\\Textures\\huge.dds")) return build_huge_dimension(size);
    return NULL; /* "missing.dds", "../evil.dds", and anything else: simulate NOT_FOUND */
}

static void fixture_free_file(void *data) { free(data); }

static uintptr_t fixture_terrain_token(void) { return (uintptr_t)g_fixture_generation; }

/* Builds one small, fully-populated terrain snapshot: 2 terrain textures, 1 cliff set, 2 cliff
 * cells, a 2x2 cell grid, no HMAP samples, and one 2x2 decoded mask layer. Texture identities
 * are read from the g_tex*_* globals so individual tests can retarget them (missing file, bad
 * DDS, oversize DDS, path-confinement violation) without rebuilding the whole fixture shape. */
static bzSC2Terrain_t *fixture_copy_terrain(uintptr_t *source_token, bzSC2AResult_t *status) {
    bzSC2Terrain_t *terrain;
    bzSC2ATerrainTextureInfo_t *textures;
    bzSC2ACliffSetInfo_t *cliff_sets;
    bzSC2ACliffCellInfo_t *cliff_cells;
    bzSC2ACellInfo_t *cells;
    uint8_t *mask;
    uint32_t textures_bytes, cliff_sets_bytes, cliff_cells_bytes, cells_bytes, mask_bytes;
    (void)source_token;
    g_fixture_copy_count++;
    if (g_fixture_shape == FIXTURE_SHAPE_TOO_LARGE) { *status = BZ_SC2A_ERR_TOO_LARGE; return NULL; }
    if (g_fixture_shape == FIXTURE_SHAPE_MALFORMED) { *status = BZ_SC2A_ERR_MALFORMED; return NULL; }
    textures_bytes = 2 * (uint32_t)sizeof(bzSC2ATerrainTextureInfo_t);
    cliff_sets_bytes = 1 * (uint32_t)sizeof(bzSC2ACliffSetInfo_t);
    cliff_cells_bytes = 2 * (uint32_t)sizeof(bzSC2ACliffCellInfo_t);
    cells_bytes = 4 * (uint32_t)sizeof(bzSC2ACellInfo_t); /* 2x2 */
    mask_bytes = 4; /* 2x2 * 1 layer */
    terrain = BZ_SC2A_TerrainAlloc((size_t)textures_bytes + cliff_sets_bytes + cliff_cells_bytes + cells_bytes +
                                   mask_bytes);
    if (!terrain) { *status = BZ_SC2A_ERR_OUT_OF_MEMORY; return NULL; }
    terrain->textures_offset = 0;
    terrain->cliff_sets_offset = terrain->textures_offset + textures_bytes;
    terrain->cliff_cells_offset = terrain->cliff_sets_offset + cliff_sets_bytes;
    terrain->height_samples_offset = terrain->cliff_cells_offset + cliff_cells_bytes; /* no HMAP samples */
    terrain->cells_offset = terrain->height_samples_offset;
    terrain->mask_offset = terrain->cells_offset + cells_bytes;
    textures = BZ_SC2A_TerrainData(terrain, terrain->textures_offset, textures_bytes);
    cliff_sets = BZ_SC2A_TerrainData(terrain, terrain->cliff_sets_offset, cliff_sets_bytes);
    cliff_cells = BZ_SC2A_TerrainData(terrain, terrain->cliff_cells_offset, cliff_cells_bytes);
    cells = BZ_SC2A_TerrainData(terrain, terrain->cells_offset, cells_bytes);
    mask = BZ_SC2A_TerrainData(terrain, terrain->mask_offset, mask_bytes);
    textures[0] = (bzSC2ATerrainTextureInfo_t){ .index = 0 };
    snprintf(textures[0].diffuse_identity, sizeof(textures[0].diffuse_identity), "%s", g_tex0_diffuse);
    snprintf(textures[0].normal_identity, sizeof(textures[0].normal_identity), "%s", g_tex0_normal);
    textures[1] = (bzSC2ATerrainTextureInfo_t){ .index = 1 };
    snprintf(textures[1].diffuse_identity, sizeof(textures[1].diffuse_identity), "%s", g_tex1_diffuse);
    snprintf(textures[1].normal_identity, sizeof(textures[1].normal_identity), "%s", g_tex1_normal);
    cliff_sets[0] = (bzSC2ACliffSetInfo_t){ .index = 0 };
    snprintf(cliff_sets[0].name, sizeof(cliff_sets[0].name), "RampSet");
    snprintf(cliff_sets[0].mesh, sizeof(cliff_sets[0].mesh), "ramp.m3");
    cliff_cells[0] = (bzSC2ACliffCellInfo_t){ .flat_index = 0, .flags = 1, .cliff_set = 0, .variant = 0 };
    cliff_cells[1] = (bzSC2ACliffCellInfo_t){ .flat_index = 1, .flags = 2, .cliff_set = 0, .variant = 1 };
    for (uint32_t i = 0; i < 4; i++) cells[i] = (bzSC2ACellInfo_t){ .cliff_level = i % 3, .cell_flags = i };
    memcpy(mask, g_fixture_mask, mask_bytes);
    terrain->info = (bzSC2ATerrainInfo_t){
        .generation = g_fixture_generation,
        .availability_flags = BZ_SC2A_TERRAIN_HAS_LFCT | BZ_SC2A_TERRAIN_HAS_MASK,
        .unsupported_flags = BZ_SC2A_TERRAIN_UNSUPPORTED_VERTEX_COLOR | BZ_SC2A_TERRAIN_UNSUPPORTED_PAINTED_PATHING,
        .cell_width = 2, .cell_height = 2,
        .hmap_width = 0, .hmap_height = 0,
        .mask_width = 2, .mask_height = 2, .mask_layer_count = 1,
        .texture_count = 2, .cliff_set_count = 1, .cliff_cell_count = 2,
        .origin_x = 0.0f, .origin_y = 0.0f, .cell_size = 1.0f,
    };
    *status = BZ_SC2A_OK;
    return terrain;
}

void BZ_SC2_TTA_Source(bzSC2ASource_t *source) {
    *source = (bzSC2ASource_t){
        .path_is_confined = sc2_tta_path_is_confined,
        .read_file = fixture_read_file,
        .free_file = fixture_free_file,
        .terrain_token = fixture_terrain_token,
        .copy_terrain = fixture_copy_terrain,
    };
}

void BZ_SC2A_TestTerrainImageValidated(void) {
    pthread_mutex_lock(&g_terrain_image_hook_lock);
    if (g_terrain_image_hook_blocked) {
        g_terrain_image_hook_entered = true;
        pthread_cond_broadcast(&g_terrain_image_hook_cond);
        while (g_terrain_image_hook_blocked)
            pthread_cond_wait(&g_terrain_image_hook_cond, &g_terrain_image_hook_lock);
    }
    pthread_mutex_unlock(&g_terrain_image_hook_lock);
}

/* ---- Tests ------------------------------------------------------------------------------- */

static void test_abi_constants(void) {
    ASSERT_EQ_INT(BZ_SC2A_ABI_VERSION, 1);
    ASSERT_EQ_INT(BZ_SC2A_MAX_IDENTITY, 256);
    ASSERT_EQ_INT(BZ_SC2A_MAX_CATALOG_NAME, 64);
    ASSERT_EQ_INT(BZ_SC2A_TERRAIN_MAX_DIMENSION, 1024);
    ASSERT_EQ_INT(BZ_SC2A_MAX_TERRAIN_TEXTURES, 16);
    ASSERT_EQ_INT(BZ_SC2A_MAX_CLIFF_SETS, 8);
    ASSERT_EQ_INT(BZ_SC2A_MAX_CLIFF_CELLS, 16384);
    ASSERT_EQ_INT(BZ_SC2A_MASK_MAX_DIMENSION, 8192);
    ASSERT_EQ_INT(BZ_SC2A_MASK_MAX_BYTES, 256u * 1024u * 1024u);
    ASSERT_EQ_INT(BZ_SC2A_IMAGE_MAX_DIMENSION, 16384);
    ASSERT_EQ_INT(BZ_SC2A_IMAGE_MAX_BYTES, 256u * 1024u * 1024u);
    /* Append-only ordinals: OK must stay 0, OUT_OF_MEMORY must stay the last of the 11 entries. */
    ASSERT_EQ_INT(BZ_SC2A_OK, 0);
    ASSERT_EQ_INT(BZ_SC2A_ERR_OUT_OF_MEMORY, 10);
    ASSERT_EQ_INT(sizeof(bzSC2ATerrainInfo_t), 104);
    ASSERT_EQ_INT(sizeof(bzSC2ATerrainTextureInfo_t), 520);
    ASSERT_EQ_INT(sizeof(bzSC2ACliffSetInfo_t), 136);
    ASSERT_EQ_INT(sizeof(bzSC2ACliffCellInfo_t), 16);
    ASSERT_EQ_INT(sizeof(bzSC2AHeightSample_t), 12);
    ASSERT_EQ_INT(sizeof(bzSC2ACellInfo_t), 4);
    ASSERT_EQ_INT(sizeof(bzSC2AImageInfo_t), 32);
    ASSERT_EQ_INT(sizeof(bzSC2AImageMipInfo_t), 20);
    ASSERT_EQ_INT(offsetof(bzSC2ATerrainInfo_t, malformed_flags), 12);
    ASSERT_EQ_INT(offsetof(bzSC2AImageInfo_t, origin), 24);
    ASSERT_EQ_INT(sizeof(((bzSC2ATerrainTextureInfo_t *)0)->diffuse_identity), BZ_SC2A_MAX_IDENTITY);
    ASSERT_EQ_INT(sizeof(((bzSC2ACliffSetInfo_t *)0)->name), BZ_SC2A_MAX_CATALOG_NAME);
}

static void test_lifecycle_before_init(void) {
    const bzSC2Terrain_t *terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    ASSERT_NOT_NULL(terrain);
    ASSERT(BZ_SC2ATerrain_IsPlaceholder(terrain));
    ASSERT_EQ_INT(BZ_SC2ATerrain_Status(terrain), BZ_SC2A_ERR_NOT_INITIALIZED);
    BZ_SC2ATerrain_Release(terrain);
}

static void test_lifecycle_abi_mismatch(void) {
    const bzSC2Terrain_t *terrain;
    BZ_SC2A_Init();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION + 1);
    ASSERT_NOT_NULL(terrain);
    ASSERT(BZ_SC2ATerrain_IsPlaceholder(terrain));
    ASSERT_EQ_INT(BZ_SC2ATerrain_Status(terrain), BZ_SC2A_ERR_ABI_VERSION);
    BZ_SC2ATerrain_Release(terrain);
    ASSERT_EQ_INT(BZ_SC2A_AbiVersion(), BZ_SC2A_ABI_VERSION);
    BZ_SC2A_Shutdown();
}

static void test_lifecycle_shutdown_terminal(void) {
    const bzSC2Terrain_t *terrain;
    BZ_SC2A_Init();
    BZ_SC2A_Shutdown();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    ASSERT(BZ_SC2ATerrain_IsPlaceholder(terrain));
    ASSERT_EQ_INT(BZ_SC2ATerrain_Status(terrain), BZ_SC2A_ERR_TERMINAL);
    BZ_SC2ATerrain_Release(terrain);
}

static void test_publish_too_large_reports_placeholder(void) {
    const bzSC2Terrain_t *terrain;
    g_fixture_shape = FIXTURE_SHAPE_TOO_LARGE;
    g_fixture_generation = 1;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    ASSERT(BZ_SC2ATerrain_IsPlaceholder(terrain));
    ASSERT_EQ_INT(BZ_SC2ATerrain_Status(terrain), BZ_SC2A_ERR_TOO_LARGE);
    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
    g_fixture_shape = FIXTURE_SHAPE_OK;
}

static void test_publish_malformed_reports_placeholder(void) {
    const bzSC2Terrain_t *terrain;
    uint32_t copies = g_fixture_copy_count;
    g_fixture_shape = FIXTURE_SHAPE_MALFORMED;
    g_fixture_generation = 2;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    BZ_SC2A_PublishTerrainFromGame();
    ASSERT_EQ_INT(g_fixture_copy_count - copies, 1); /* failed tokens are cached, not retried per frame */
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    ASSERT(BZ_SC2ATerrain_IsPlaceholder(terrain));
    ASSERT_EQ_INT(BZ_SC2ATerrain_Status(terrain), BZ_SC2A_ERR_MALFORMED);
    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
    g_fixture_shape = FIXTURE_SHAPE_OK;
}

static void test_image_huge_dimension_reports_too_large(void) {
    const bzSC2Terrain_t *terrain;
    const bzSC2Image_t *image;
    g_fixture_generation = 3;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    ASSERT(!BZ_SC2ATerrain_IsPlaceholder(terrain));
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 1, BZ_SC2A_TERRAIN_CHANNEL_NORMAL);
    ASSERT_NOT_NULL(image);
    ASSERT(BZ_SC2AImage_IsPlaceholder(image));
    ASSERT_EQ_INT(BZ_SC2AImage_Status(image), BZ_SC2A_ERR_TOO_LARGE);
    BZ_SC2AImage_Release(image);
    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
}

static void test_cache_hit_miss_and_placeholder_log_once(void) {
    const bzSC2Terrain_t *terrain;
    const bzSC2Image_t *image1, *image2, *bad1, *bad2;

    g_fixture_generation = 4;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);

    image1 = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 0, BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT_NOT_NULL(image1);
    ASSERT(!BZ_SC2AImage_IsPlaceholder(image1));
    ASSERT_EQ_INT(BZ_SC2A_CacheMisses(), 1);
    image2 = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 0, BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT_EQ_INT(BZ_SC2A_CacheHits(), 1);
    ASSERT(image1 == image2);

    /* "missing.dds": not found -> cached placeholder, logged exactly once across two calls. */
    bad1 = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 0, BZ_SC2A_TERRAIN_CHANNEL_NORMAL);
    ASSERT(BZ_SC2AImage_IsPlaceholder(bad1));
    ASSERT_EQ_INT(BZ_SC2AImage_Status(bad1), BZ_SC2A_ERR_NOT_FOUND);
    ASSERT_EQ_INT(BZ_SC2A_PlaceholderLogs(), 1);
    bad2 = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 0, BZ_SC2A_TERRAIN_CHANNEL_NORMAL);
    ASSERT_EQ_INT(BZ_SC2A_PlaceholderLogs(), 1);
    ASSERT(bad1 == bad2);
    ASSERT_EQ_INT(BZ_SC2A_CacheMisses(), 2); /* one real miss, one placeholder-producing miss */
    ASSERT_EQ_INT(BZ_SC2A_CacheHits(), 2);   /* one real hit, one placeholder cache hit */

    BZ_SC2AImage_Release(image1); BZ_SC2AImage_Release(image2);
    BZ_SC2AImage_Release(bad1); BZ_SC2AImage_Release(bad2);
    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
}

static void test_retained_handles_survive_reload_and_shutdown(void) {
    const bzSC2Terrain_t *terrain1, *terrain2;
    const bzSC2Image_t *image1;
    bzSC2ATerrainInfo_t info;
    char identity[BZ_SC2A_MAX_IDENTITY];

    g_fixture_generation = 5;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain1 = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    ASSERT(!BZ_SC2ATerrain_IsPlaceholder(terrain1));
    image1 = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain1, 0, BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT(!BZ_SC2AImage_IsPlaceholder(image1));

    /* New session: re-Init bumps the ABI session counter and clears the publication cache, but
     * handles retained by the caller before that point must remain valid and readable. */
    g_fixture_generation = 6;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain2 = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    ASSERT(terrain1 != terrain2);
    ASSERT(BZ_SC2ATerrain_Info(terrain1, &info));
    ASSERT_EQ_INT(info.generation, 5);
    ASSERT(BZ_SC2AImage_Identity(image1, identity, sizeof(identity)));
    ASSERT_STR_EQ(identity, "Assets\\Textures\\good.dds");

    BZ_SC2A_Shutdown();
    /* Shutdown prevents new reads but must not invalidate outstanding retained handles. */
    ASSERT(BZ_SC2ATerrain_Info(terrain1, &info));
    ASSERT_EQ_INT(info.generation, 5);
    ASSERT(!BZ_SC2AImage_IsPlaceholder(image1));

    BZ_SC2ATerrain_Release(terrain1);
    BZ_SC2ATerrain_Release(terrain2);
    BZ_SC2AImage_Release(image1);
}

static void test_mask_layer_copy(void) {
    const bzSC2Terrain_t *terrain;
    uint8_t out[8];

    g_fixture_generation = 7;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);

    memset(out, 0xFF, sizeof(out));
    ASSERT_EQ_INT(BZ_SC2ATerrain_CopyTextureMaskLayer(terrain, 0, out, sizeof(out)), 4);
    ASSERT_EQ_INT(out[0], 0); ASSERT_EQ_INT(out[1], 1); ASSERT_EQ_INT(out[2], 2); ASSERT_EQ_INT(out[3], 3);
    ASSERT_EQ_INT(BZ_SC2ATerrain_CopyTextureMaskLayer(terrain, 1, out, sizeof(out)), 0); /* only 1 layer */
    ASSERT_EQ_INT(BZ_SC2ATerrain_CopyTextureMaskLayer(terrain, 0, out, 2), 2); /* clamped to cap */

    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
}

static void test_indexed_and_xy_bounds(void) {
    const bzSC2Terrain_t *terrain;
    bzSC2ATerrainTextureInfo_t tex;
    bzSC2ACliffSetInfo_t cliff_set;
    bzSC2ACellInfo_t cell;
    bzSC2AHeightSample_t sample;
    bzSC2ATerrainInfo_t info;

    g_fixture_generation = 8;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);

    ASSERT(BZ_SC2ATerrain_Info(terrain, &info));
    ASSERT_EQ_INT(info.availability_flags & BZ_SC2A_TERRAIN_HAS_MASK, BZ_SC2A_TERRAIN_HAS_MASK);
    ASSERT_EQ_INT(info.unsupported_flags & BZ_SC2A_TERRAIN_UNSUPPORTED_VERTEX_COLOR,
                  BZ_SC2A_TERRAIN_UNSUPPORTED_VERTEX_COLOR);
    ASSERT(BZ_SC2ATerrain_TextureInfo(terrain, 0, &tex));
    ASSERT_STR_EQ(tex.diffuse_identity, "Assets\\Textures\\good.dds");
    ASSERT(!BZ_SC2ATerrain_TextureInfo(terrain, 2, &tex)); /* texture_count == 2 */
    ASSERT(BZ_SC2ATerrain_CliffSetInfo(terrain, 0, &cliff_set));
    ASSERT_STR_EQ(cliff_set.name, "RampSet");
    ASSERT(!BZ_SC2ATerrain_CliffSetInfo(terrain, 1, &cliff_set)); /* cliff_set_count == 1 */
    ASSERT(BZ_SC2ATerrain_CellInfo(terrain, 1, 1, &cell));
    ASSERT(!BZ_SC2ATerrain_CellInfo(terrain, 2, 0, &cell)); /* cell_width == 2 */
    ASSERT(!BZ_SC2ATerrain_HeightSample(terrain, 0, 0, &sample)); /* fixture has no HMAP */

    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
}

static void test_image_info_and_mip_copy(void) {
    const bzSC2Terrain_t *terrain;
    const bzSC2Image_t *image, *generic;
    bzSC2AImageInfo_t info;
    bzSC2AImageMipInfo_t mip;
    uint8_t pixels[8];

    g_fixture_generation = 9;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 0, BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    generic = BZ_SC2A_RegisterImage(BZ_SC2A_ABI_VERSION, "Assets/Textures/good.dds");
    ASSERT(generic == image); /* terrain and model layers share one normalized DDS cache */

    ASSERT(BZ_SC2AImage_Info(image, &info));
    ASSERT_EQ_INT(info.format, BZ_SC2A_PIXEL_DXT1);
    ASSERT_EQ_INT(info.width, 4);
    ASSERT_EQ_INT(info.height, 4);
    ASSERT_EQ_INT(info.mip_count, 1);
    ASSERT_EQ_INT(info.data_bytes, 8);
    ASSERT_EQ_INT(info.origin, BZ_SC2A_ORIGIN_TOP_LEFT);
    ASSERT(BZ_SC2AImage_MipInfo(image, 0, &mip));
    ASSERT_EQ_INT(mip.size, 8);
    ASSERT_EQ_INT(mip.row_bytes, 8);
    ASSERT(!BZ_SC2AImage_MipInfo(image, 1, &mip)); /* only 1 mip level */
    ASSERT_EQ_INT(BZ_SC2AImage_CopyMip(image, 0, pixels, sizeof(pixels)), 8);
    ASSERT_EQ_INT(pixels[0], 0xAB);
    ASSERT_EQ_INT(BZ_SC2AImage_CopyMip(image, 0, pixels, 3), 3); /* clamped to cap */

    BZ_SC2AImage_Release(image);
    BZ_SC2AImage_Release(generic);
    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
}

static void test_image_rgb_pitch_contract(void) {
    const bzSC2Terrain_t *terrain;
    const bzSC2Image_t *image;
    bzSC2AImageInfo_t info;
    bzSC2AImageMipInfo_t mip;
    uint8_t pixels[9];

    g_fixture_generation = 90;
    snprintf(g_tex0_diffuse, sizeof(g_tex0_diffuse), "Assets\\Textures\\rgb-tight.dds");
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 0, BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT_EQ_INT(BZ_SC2AImage_Status(image), BZ_SC2A_OK);
    ASSERT(BZ_SC2AImage_Info(image, &info));
    ASSERT_EQ_INT(info.format, BZ_SC2A_PIXEL_RGB8);
    ASSERT_EQ_INT(info.width, 1);
    ASSERT_EQ_INT(info.height, 3);
    ASSERT_EQ_INT(info.data_bytes, 9);
    ASSERT(BZ_SC2AImage_MipInfo(image, 0, &mip));
    ASSERT_EQ_INT(mip.row_bytes, 3);
    ASSERT_EQ_INT(mip.size, 9);
    ASSERT_EQ_INT(BZ_SC2AImage_CopyMip(image, 0, pixels, sizeof(pixels)), 9);
    ASSERT_EQ_INT(pixels[0], 1);
    ASSERT_EQ_INT(pixels[8], 9);
    BZ_SC2AImage_Release(image);
    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();

    g_fixture_generation++;
    snprintf(g_tex0_diffuse, sizeof(g_tex0_diffuse), "Assets\\Textures\\rgb-padded.dds");
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 0, BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT(BZ_SC2AImage_IsPlaceholder(image));
    ASSERT_EQ_INT(BZ_SC2AImage_Status(image), BZ_SC2A_ERR_UNSUPPORTED);
    BZ_SC2AImage_Release(image);
    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
    snprintf(g_tex0_diffuse, sizeof(g_tex0_diffuse), "Assets\\Textures\\good.dds");
}

static void test_image_registration_errors_are_typed_placeholders(void) {
    const bzSC2Terrain_t *terrain;
    const bzSC2Image_t *image, *again;
    g_fixture_generation = 10;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION + 1, terrain, 0,
                                         BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT(BZ_SC2AImage_IsPlaceholder(image));
    ASSERT_EQ_INT(BZ_SC2AImage_Status(image), BZ_SC2A_ERR_ABI_VERSION);
    BZ_SC2AImage_Release(image);
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 0, (bzSC2ATerrainChannel_t)99);
    ASSERT(BZ_SC2AImage_IsPlaceholder(image));
    ASSERT_EQ_INT(BZ_SC2AImage_Status(image), BZ_SC2A_ERR_INVALID_ARGUMENT);
    BZ_SC2AImage_Release(image);
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, NULL, 0,
                                         BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT_EQ_INT(BZ_SC2AImage_Status(image), BZ_SC2A_ERR_INVALID_ARGUMENT);
    BZ_SC2AImage_Release(image);
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 99,
                                         BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT_EQ_INT(BZ_SC2AImage_Status(image), BZ_SC2A_ERR_INVALID_ARGUMENT);
    BZ_SC2AImage_Release(image);
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 1,
                                         BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    again = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 1,
                                         BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT(BZ_SC2AImage_IsPlaceholder(image));
    ASSERT_EQ_INT(BZ_SC2AImage_Status(image), BZ_SC2A_ERR_MALFORMED);
    ASSERT(image == again);
    ASSERT_EQ_INT(BZ_SC2A_CacheMisses(), 1);
    ASSERT_EQ_INT(BZ_SC2A_CacheHits(), 1);
    ASSERT_EQ_INT(BZ_SC2A_PlaceholderLogs(), 1);
    BZ_SC2AImage_Release(image);
    BZ_SC2AImage_Release(again);
    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
}

static void test_slash_normalization_and_reload_reuse_image_cache(void) {
    const bzSC2Terrain_t *terrain1, *terrain2;
    const bzSC2Image_t *image1, *image2;
    g_fixture_generation = 11;
    snprintf(g_tex0_diffuse, sizeof(g_tex0_diffuse), "Assets/Textures/good.dds");
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain1 = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    image1 = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain1, 0,
                                          BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT_EQ_INT(BZ_SC2A_CacheMisses(), 1);
    g_fixture_generation = 12;
    snprintf(g_tex0_diffuse, sizeof(g_tex0_diffuse), "Assets\\Textures\\good.dds");
    BZ_SC2A_PublishTerrainFromGame();
    terrain2 = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    image2 = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain2, 0,
                                          BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT(image1 == image2);
    ASSERT_EQ_INT(BZ_SC2A_CacheHits(), 1);
    ASSERT_EQ_INT(BZ_SC2A_CacheMisses(), 1);
    BZ_SC2AImage_Release(image1); BZ_SC2AImage_Release(image2);
    BZ_SC2ATerrain_Release(terrain1); BZ_SC2ATerrain_Release(terrain2);
    BZ_SC2A_Shutdown();
}

static void test_path_confinement_direct(void) {
    char overlong[BZ_SC2A_MAX_IDENTITY + 8];
    memset(overlong, 'a', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = '\0';

    ASSERT(!sc2_tta_path_is_confined(NULL));
    ASSERT(!sc2_tta_path_is_confined(""));
    ASSERT(!sc2_tta_path_is_confined("/abs/path.dds"));
    ASSERT(!sc2_tta_path_is_confined("\\abs\\path.dds"));
    ASSERT(!sc2_tta_path_is_confined("C:\\drive\\path.dds"));
    ASSERT(!sc2_tta_path_is_confined("\\\\unc\\share\\path.dds"));
    ASSERT(!sc2_tta_path_is_confined("..\\evil.dds"));
    ASSERT(!sc2_tta_path_is_confined("a\\..\\..\\b.dds"));
    ASSERT(!sc2_tta_path_is_confined("a/b/../../../c.dds"));
    ASSERT(!sc2_tta_path_is_confined("a\\.\\b.dds"));
    ASSERT(!sc2_tta_path_is_confined("a\\\\b.dds"));
    ASSERT(!sc2_tta_path_is_confined("a//b.dds"));
    ASSERT(!sc2_tta_path_is_confined("a\\/b.dds"));
    ASSERT(!sc2_tta_path_is_confined("Assets\\Textures\\bad\nname.dds"));
    ASSERT(!sc2_tta_path_is_confined("Assets\\Textures\\")); /* trailing separator */
    ASSERT(!sc2_tta_path_is_confined(overlong));
    ASSERT(sc2_tta_path_is_confined("Assets\\Textures\\good.dds"));
    ASSERT(sc2_tta_path_is_confined("Assets/Textures/good.dds")); /* forward slashes also parse */
    ASSERT(sc2_tta_path_is_confined("Assets/Textures\\good.dds")); /* mixed separators normalize safely */
}

static void test_path_confinement_end_to_end(void) {
    const bzSC2Terrain_t *terrain;
    const bzSC2Image_t *image;

    snprintf(g_tex1_diffuse, sizeof(g_tex1_diffuse), "..\\evil.dds");
    g_fixture_generation = 13;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, terrain, 1, BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    ASSERT_NOT_NULL(image);
    ASSERT(BZ_SC2AImage_IsPlaceholder(image));
    ASSERT_EQ_INT(BZ_SC2AImage_Status(image), BZ_SC2A_ERR_PATH_CONFINEMENT);

    BZ_SC2AImage_Release(image);
    BZ_SC2ATerrain_Release(terrain);
    BZ_SC2A_Shutdown();
    snprintf(g_tex1_diffuse, sizeof(g_tex1_diffuse), "Assets\\Textures\\malformed.dds"); /* restore */
}

static void test_overlong_image_identity_is_cached_once(void) {
    char overlong[BZ_SC2A_MAX_IDENTITY + 32];
    const bzSC2Image_t *first, *second;
    memset(overlong, 'a', sizeof(overlong) - 1); overlong[sizeof(overlong) - 1] = '\0';
    BZ_SC2A_Init();
    first = BZ_SC2A_RegisterImage(BZ_SC2A_ABI_VERSION, overlong);
    second = BZ_SC2A_RegisterImage(BZ_SC2A_ABI_VERSION, overlong);
    ASSERT_EQ_INT(BZ_SC2AImage_Status(first), BZ_SC2A_ERR_PATH_CONFINEMENT);
    ASSERT(first == second);
    ASSERT_EQ_INT(BZ_SC2A_CacheMisses(), 1);
    ASSERT_EQ_INT(BZ_SC2A_CacheHits(), 1);
    ASSERT_EQ_INT(BZ_SC2A_PlaceholderLogs(), 1);
    BZ_SC2AImage_Release(first); BZ_SC2AImage_Release(second);
    BZ_SC2A_Shutdown();
}

typedef struct {
    const bzSC2Terrain_t *terrain;
    const bzSC2Image_t *image;
} terrain_image_race_t;

static void *register_terrain_image_after_validation(void *opaque) {
    terrain_image_race_t *race = opaque;
    race->image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, race->terrain, 0,
                                               BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
    return NULL;
}

static void test_terrain_image_preserves_captured_generation(void) {
    terrain_image_race_t race = { 0 };
    pthread_t thread;
    g_fixture_generation = 15;
    BZ_SC2A_Init(); BZ_SC2A_PublishTerrainFromGame();
    race.terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    pthread_mutex_lock(&g_terrain_image_hook_lock);
    g_terrain_image_hook_blocked = true; g_terrain_image_hook_entered = false;
    pthread_mutex_unlock(&g_terrain_image_hook_lock);
    ASSERT_EQ_INT(pthread_create(&thread, NULL, register_terrain_image_after_validation, &race), 0);
    pthread_mutex_lock(&g_terrain_image_hook_lock);
    while (!g_terrain_image_hook_entered)
        pthread_cond_wait(&g_terrain_image_hook_cond, &g_terrain_image_hook_lock);
    pthread_mutex_unlock(&g_terrain_image_hook_lock);
    BZ_SC2A_Shutdown(); BZ_SC2A_Init();
    pthread_mutex_lock(&g_terrain_image_hook_lock);
    g_terrain_image_hook_blocked = false; pthread_cond_broadcast(&g_terrain_image_hook_cond);
    pthread_mutex_unlock(&g_terrain_image_hook_lock);
    ASSERT_EQ_INT(pthread_join(thread, NULL), 0);
    ASSERT_NOT_NULL(race.image);
    ASSERT_EQ_INT(BZ_SC2AImage_Status(race.image), BZ_SC2A_ERR_TERMINAL);
    BZ_SC2AImage_Release(race.image); BZ_SC2ATerrain_Release(race.terrain);
    BZ_SC2A_Shutdown();
}

typedef enum { CONCURRENT_LATEST, CONCURRENT_REGISTER, CONCURRENT_PUBLISH, CONCURRENT_SHUTDOWN } concurrent_op_t;
typedef struct {
    concurrent_op_t op;
    const bzSC2Terrain_t *terrain;
    int failures;
} concurrent_ctx_t;

/* Exercises the cache/lifecycle lock while retained immutable handles remain lock-free. */
static void *concurrent_asset_op(void *opaque) {
    concurrent_ctx_t *ctx = opaque;
    int iterations = ctx->op == CONCURRENT_SHUTDOWN ? 100 : 500;
    for (int i = 0; i < iterations; i++) {
        const bzSC2Terrain_t *terrain;
        const bzSC2Image_t *image;
        switch (ctx->op) {
        case CONCURRENT_LATEST:
            terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
            if (!terrain) ctx->failures++;
            else BZ_SC2ATerrain_Release(terrain);
            break;
        case CONCURRENT_REGISTER:
            image = BZ_SC2A_RegisterTerrainImage(BZ_SC2A_ABI_VERSION, ctx->terrain, 0,
                                                 BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE);
            if (!image) ctx->failures++;
            else BZ_SC2AImage_Release(image);
            break;
        case CONCURRENT_PUBLISH:
            g_fixture_generation++;
            BZ_SC2A_PublishTerrainFromGame();
            break;
        case CONCURRENT_SHUTDOWN:
            sched_yield();
            break;
        }
    }
    if (ctx->op == CONCURRENT_SHUTDOWN) BZ_SC2A_Shutdown();
    return NULL;
}

static void test_concurrent_cache_publication_and_shutdown(void) {
    const bzSC2Terrain_t *terminal;
    pthread_t threads[4];
    concurrent_ctx_t ctx[4] = {
        { .op = CONCURRENT_LATEST },
        { .op = CONCURRENT_REGISTER },
        { .op = CONCURRENT_PUBLISH },
        { .op = CONCURRENT_SHUTDOWN },
    };
    g_fixture_generation = 14;
    BZ_SC2A_Init();
    BZ_SC2A_PublishTerrainFromGame();
    ctx[1].terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    ASSERT_NOT_NULL(ctx[1].terrain);
    for (int i = 0; i < 4; i++) ASSERT_EQ_INT(pthread_create(threads + i, NULL, concurrent_asset_op, ctx + i), 0);
    for (int i = 0; i < 4; i++) ASSERT_EQ_INT(pthread_join(threads[i], NULL), 0);
    for (int i = 0; i < 4; i++) ASSERT_EQ_INT(ctx[i].failures, 0);
    BZ_SC2ATerrain_Release(ctx[1].terrain);
    terminal = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    ASSERT_EQ_INT(BZ_SC2ATerrain_Status(terminal), BZ_SC2A_ERR_TERMINAL);
    BZ_SC2ATerrain_Release(terminal);
}

int main(void) {
    RUN_TEST(test_abi_constants);
    RUN_TEST(test_lifecycle_before_init);
    RUN_TEST(test_lifecycle_abi_mismatch);
    RUN_TEST(test_lifecycle_shutdown_terminal);
    RUN_TEST(test_publish_too_large_reports_placeholder);
    RUN_TEST(test_publish_malformed_reports_placeholder);
    RUN_TEST(test_image_huge_dimension_reports_too_large);
    RUN_TEST(test_cache_hit_miss_and_placeholder_log_once);
    RUN_TEST(test_retained_handles_survive_reload_and_shutdown);
    RUN_TEST(test_mask_layer_copy);
    RUN_TEST(test_indexed_and_xy_bounds);
    RUN_TEST(test_image_info_and_mip_copy);
    RUN_TEST(test_image_rgb_pitch_contract);
    RUN_TEST(test_image_registration_errors_are_typed_placeholders);
    RUN_TEST(test_slash_normalization_and_reload_reuse_image_cache);
    RUN_TEST(test_path_confinement_direct);
    RUN_TEST(test_path_confinement_end_to_end);
    RUN_TEST(test_overlong_image_identity_is_cached_once);
    RUN_TEST(test_terrain_image_preserves_captured_generation);
    RUN_TEST(test_concurrent_cache_publication_and_shutdown);
    TEST_RESULTS();
}
