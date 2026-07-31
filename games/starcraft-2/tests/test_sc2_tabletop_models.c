/*
 * test_sc2_tabletop_models.c - Layer 2B1 ABI, parser boundary, cache, and lifecycle coverage.
 */
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "games/starcraft-2/common/sc2_m3.h"
#include "games/starcraft-2/visionos/sc2_tabletop_assets_internal.h"
#include "games/starcraft-2/visionos/sc2_tabletop_models.h"
#include "test_framework.h"

int _tests_run = 0, _tests_failed = 0;
static uint32_t g_reads;

_Static_assert(BZ_SC2M_OK == 0 && BZ_SC2M_ERR_NOT_INITIALIZED == 1 && BZ_SC2M_ERR_TERMINAL == 2 &&
               BZ_SC2M_ERR_ABI_VERSION == 3 && BZ_SC2M_ERR_INVALID_ARGUMENT == 4 &&
               BZ_SC2M_ERR_PATH_CONFINEMENT == 5 && BZ_SC2M_ERR_NOT_FOUND == 6 &&
               BZ_SC2M_ERR_MALFORMED == 7 && BZ_SC2M_ERR_UNSUPPORTED == 8 &&
               BZ_SC2M_ERR_TOO_LARGE == 9 && BZ_SC2M_ERR_OUT_OF_MEMORY == 10, "result ABI changed");
_Static_assert(sizeof(bzSC2MVertex_t) == 48, "vertex ABI changed");
_Static_assert(sizeof(bzSC2MModelInfo_t) == 480 && sizeof(bzSC2MDivisionInfo_t) == 24 &&
               sizeof(bzSC2MRegionInfo_t) == 32 && sizeof(bzSC2MBatchInfo_t) == 12, "geometry ABI changed");
_Static_assert(sizeof(bzSC2MMaterialReferenceInfo_t) == 12 &&
               sizeof(bzSC2MStandardMaterialInfo_t) == 196 &&
               sizeof(bzSC2MCompositeSectionInfo_t) == 16 && sizeof(bzSC2MLayerInfo_t) == 444,
               "material ABI changed");

/* A zero-geometry MODL v23 is valid (retail effect/decal models use this shape). */
static void *build_empty_m3(uint32_t *size, uint32_t version) {
    uint8_t *data = calloc(1, 2048);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *refs;
    memcpy(head->id, "43DM", 4);
    head->ofsRefs = 1900;
    head->nRefs = 1;
    head->MODL = (Reference){ .nEntries = 1, .ref = 0 };
    refs = (struct ReferenceEntry *)(data + head->ofsRefs);
    memcpy(refs[0].id, "MODL", 4);
    refs[0].offset = 128;
    refs[0].nEntries = 1;
    refs[0].version = version;
    *size = 2048;
    return data;
}

static void *fixture_read_file(const char *identity, uint32_t *size) {
    if (!strcmp(identity, "Assets\\Models\\empty.m3")) { g_reads++; return build_empty_m3(size, 23); }
    if (!strcmp(identity, "Assets\\Models\\v24.m3")) { g_reads++; return build_empty_m3(size, 24); }
    if (!strcmp(identity, "Assets\\Models\\bad.m3")) {
        uint8_t *data = calloc(1, 32);
        *size = 32;
        return data;
    }
    return NULL;
}

static void fixture_free_file(void *data) { free(data); }
void BZ_SC2_TTA_Source(bzSC2ASource_t *source) {
    *source = (bzSC2ASource_t){ .path_is_confined = sc2_tta_path_is_confined,
        .read_file = fixture_read_file, .free_file = fixture_free_file };
}

static void test_lifecycle_and_abi(void) {
    const bzSC2Model_t *model = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "Assets\\Models\\empty.m3");
    ASSERT(BZ_SC2Model_IsPlaceholder(model));
    ASSERT_EQ_INT(BZ_SC2Model_Status(model), BZ_SC2M_ERR_NOT_INITIALIZED);
    BZ_SC2Model_Release(model);
    ASSERT_EQ_INT(BZ_SC2M_AbiVersion(), 1);
    BZ_SC2A_Init();
    BZ_SC2M_Init();
    BZ_SC2M_BeginRegistration(7);
    model = BZ_SC2M_RegisterModel(99, "Assets\\Models\\empty.m3");
    ASSERT_EQ_INT(BZ_SC2Model_Status(model), BZ_SC2M_ERR_ABI_VERSION);
    BZ_SC2Model_Release(model);
    BZ_SC2M_Shutdown();
    BZ_SC2A_Shutdown();
}

static void test_valid_zero_geometry_and_cache(void) {
    bzSC2MModelInfo_t info;
    const bzSC2Model_t *a, *b;
    BZ_SC2A_Init(); BZ_SC2M_Init();
    uint64_t generation = BZ_SC2M_BeginRegistration(9);
    ASSERT(generation != 0);
    a = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "Assets/Models/empty.m3");
    ASSERT(!BZ_SC2Model_IsPlaceholder(a));
    ASSERT_EQ_INT(BZ_SC2Model_Status(a), BZ_SC2M_OK);
    ASSERT(BZ_SC2Model_Info(a, &info));
    ASSERT_EQ_INT(info.modl_version, 23);
    ASSERT_EQ_INT(info.vertex_count, 0);
    ASSERT_EQ_INT(info.layer_count, 0);
    b = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "Assets\\Models\\empty.m3");
    ASSERT(a == b);
    ASSERT_EQ_INT(BZ_SC2M_CacheMisses(), 1);
    ASSERT_EQ_INT(BZ_SC2M_CacheHits(), 1);
    BZ_SC2Model_Release(b);
    BZ_SC2M_BeginRegistration(10);
    ASSERT(BZ_SC2Model_Info(a, &info)); /* retained snapshot survives cache reload */
    BZ_SC2Model_Release(a);
    BZ_SC2M_Shutdown(); BZ_SC2A_Shutdown();
}

static void test_placeholders_and_confinement(void) {
    const bzSC2Model_t *model;
    BZ_SC2A_Init(); BZ_SC2M_Init(); BZ_SC2M_BeginRegistration(1);
    model = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "../escape.m3");
    ASSERT_EQ_INT(BZ_SC2Model_Status(model), BZ_SC2M_ERR_PATH_CONFINEMENT);
    BZ_SC2Model_Release(model);
    model = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "Assets\\Models\\missing.m3");
    ASSERT_EQ_INT(BZ_SC2Model_Status(model), BZ_SC2M_ERR_NOT_FOUND);
    BZ_SC2Model_Release(model);
    model = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "Assets\\Models\\bad.m3");
    ASSERT_EQ_INT(BZ_SC2Model_Status(model), BZ_SC2M_ERR_MALFORMED);
    BZ_SC2Model_Release(model);
    model = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "Assets\\Models\\v24.m3");
    ASSERT_EQ_INT(BZ_SC2Model_Status(model), BZ_SC2M_ERR_UNSUPPORTED);
    BZ_SC2Model_Release(model);
    ASSERT_EQ_INT(BZ_SC2M_PlaceholderLogs(), 3);
    BZ_SC2M_Shutdown(); BZ_SC2A_Shutdown();
}

static void *register_same_model(void *unused) {
    const bzSC2Model_t *model;
    (void)unused;
    model = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "Assets\\Models\\empty.m3");
    if (!model || BZ_SC2Model_IsPlaceholder(model)) return (void *)1;
    BZ_SC2Model_Release(model);
    return NULL;
}

static void test_concurrent_same_identity_publication(void) {
    pthread_t threads[8];
    g_reads = 0;
    BZ_SC2A_Init(); BZ_SC2M_Init(); BZ_SC2M_BeginRegistration(11);
    FOR_LOOP(i, 8) ASSERT_EQ_INT(pthread_create(&threads[i], NULL, register_same_model, NULL), 0);
    FOR_LOOP(i, 8) {
        void *result = NULL;
        ASSERT_EQ_INT(pthread_join(threads[i], &result), 0);
        ASSERT_NULL(result);
    }
    ASSERT_EQ_INT(g_reads, 1);
    ASSERT_EQ_INT(BZ_SC2M_CacheMisses(), 1);
    ASSERT_EQ_INT(BZ_SC2M_CacheHits(), 7);
    BZ_SC2M_Shutdown(); BZ_SC2A_Shutdown();
}

int main(void) {
    RUN_TEST(test_lifecycle_and_abi);
    RUN_TEST(test_valid_zero_geometry_and_cache);
    RUN_TEST(test_placeholders_and_confinement);
    RUN_TEST(test_concurrent_same_identity_publication);
    TEST_RESULTS();
}
