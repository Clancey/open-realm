/*
 * test_sc2_tabletop_models.c - Layer 2B1 ABI, parser boundary, cache, and lifecycle coverage.
 */
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include "games/starcraft-2/common/sc2_m3.h"
#include "games/starcraft-2/renderer/m3/r_m3_utils.h"
#include "games/starcraft-2/visionos/sc2_tabletop_assets_internal.h"
#include "games/starcraft-2/visionos/sc2_tabletop_models.h"
#include "games/starcraft-2/visionos/sc2_tabletop_models_internal.h"
#include "test_framework.h"

int _tests_run = 0, _tests_failed = 0;
static uint32_t g_reads;
static pthread_mutex_t g_provider_probe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_provider_probe_cond = PTHREAD_COND_INITIALIZER;
static bool g_provider_probe, g_provider_blocked;
static uint32_t g_provider_entries, g_provider_active, g_provider_max_active;

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

static void put_ref(void *data, size_t offset, Reference ref) {
    memcpy((uint8_t *)data + offset, &ref, sizeof(ref));
}

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
    memcpy(refs[0].id, "LDOM", 4);
    refs[0].offset = 128;
    refs[0].nEntries = 1;
    refs[0].version = version;
    *size = 2048;
    return data;
}

/* Exact MODL section lengths prove legacy versions do not borrow later fields or padding. */
static void *build_exact_empty_m3(uint32_t *size, uint32_t version, uint32_t stride) {
    *size = 128 + stride + sizeof(struct ReferenceEntry);
    uint8_t *data = calloc(1, *size);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *ref = (struct ReferenceEntry *)(data + 128 + stride);
    memcpy(head->id, "43DM", 4);
    head->ofsRefs = 128 + stride; head->nRefs = 1;
    head->MODL = (Reference){ .nEntries = 1, .ref = 0 };
    memcpy(ref->id, "LDOM", 4); ref->offset = 128; ref->nEntries = 1; ref->version = version;
    return data;
}

/* Material v15 consumes 268 bytes on disk; the section has ordinary 16-byte padding to 272. */
static void *build_material_v15_m3(uint32_t *size) {
    uint8_t *data = calloc(1, 1328);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *refs = (struct ReferenceEntry *)(data + 1296);
    memcpy(head->id, "43DM", 4);
    head->ofsRefs = 1296; head->nRefs = 2; head->MODL = (Reference){ .nEntries = 1, .ref = 0 };
    memcpy(refs[0].id, "LDOM", 4); refs[0].offset = 128; refs[0].nEntries = 1; refs[0].version = 23;
    memcpy(refs[1].id, "_TAM", 4); refs[1].offset = 1024; refs[1].nEntries = 1; refs[1].version = 15;
    put_ref(data + refs[0].offset, 312, (Reference){ .nEntries = 1, .ref = 1 });
    *size = 1328;
    return data;
}

/* WoL uses version-zero STG/STS records whose Reference fields are still present on disk. */
static void *build_version_zero_animation_tables_m3(uint32_t *size) {
    uint8_t *data = calloc(1, 1136);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *refs = (struct ReferenceEntry *)(data + 1088);
    memcpy(head->id, "43DM", 4);
    head->ofsRefs = 1088; head->nRefs = 3; head->MODL = (Reference){ .nEntries = 1, .ref = 0 };
    memcpy(refs[0].id, "LDOM", 4); refs[0].offset = 128; refs[0].nEntries = 1; refs[0].version = 23;
    memcpy(refs[1].id, "_GTS", 4); refs[1].offset = 1024; refs[1].nEntries = 1; refs[1].version = 0;
    memcpy(refs[2].id, "_STS", 4); refs[2].offset = 1056; refs[2].nEntries = 1; refs[2].version = 0;
    put_ref(data + refs[0].offset, 40, (Reference){ .nEntries = 1, .ref = 1 });
    put_ref(data + refs[0].offset, 68, (Reference){ .nEntries = 1, .ref = 2 });
    *size = 1136;
    return data;
}

/* One vertex and one triangle force a 6-byte U16 segment before 4-byte division records. */
static void *build_odd_indices_m3(uint32_t *size) {
    uint8_t *data = calloc(1, 1056);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *refs = (struct ReferenceEntry *)(data + 960);
    uint8_t *root = data + 32, *division = data + 848, *region = data + 912;
    memcpy(head->id, "43DM", 4);
    head->ofsRefs = 960; head->nRefs = 6; head->MODL = (Reference){ .nEntries = 1, .ref = 0 };
    memcpy(refs[0].id, "LDOM", 4); refs[0].offset = 32; refs[0].nEntries = 1; refs[0].version = 23;
    memcpy(refs[1].id, "__8U", 4); refs[1].offset = 816; refs[1].nEntries = 32; refs[1].version = 0;
    memcpy(refs[2].id, "_VID", 4); refs[2].offset = 848; refs[2].nEntries = 1; refs[2].version = 2;
    memcpy(refs[3].id, "_61U", 4); refs[3].offset = 904; refs[3].nEntries = 3; refs[3].version = 0;
    memcpy(refs[4].id, "NGER", 4); refs[4].offset = 912; refs[4].nEntries = 1; refs[4].version = 3;
    memcpy(refs[5].id, "_61U", 4); refs[5].offset = 948; refs[5].nEntries = 1; refs[5].version = 0;
    memcpy(root + 96, &(uint32_t){ 0x0180007d }, sizeof(uint32_t));
    put_ref(root, 100, (Reference){ .nEntries = 32, .ref = 1 });
    put_ref(root, 112, (Reference){ .nEntries = 1, .ref = 2 });
    put_ref(root, 124, (Reference){ .nEntries = 1, .ref = 5 });
    put_ref(division, 0, (Reference){ .nEntries = 3, .ref = 3 });
    put_ref(division, 12, (Reference){ .nEntries = 1, .ref = 4 });
    memcpy(division + 48, &(uint32_t){ 1 }, sizeof(uint32_t));
    memcpy(region + 12, &(uint32_t){ 1 }, sizeof(uint32_t));
    memcpy(region + 20, &(uint32_t){ 3 }, sizeof(uint32_t));
    memcpy(region + 28, &(uint16_t){ 1 }, sizeof(uint16_t));
    *size = 1056;
    return data;
}

static void *build_region_v2_m3(uint32_t *size) {
    uint8_t *data = build_odd_indices_m3(size);
    struct ReferenceEntry *refs = (struct ReferenceEntry *)(data + ((struct MD33 *)data)->ofsRefs);
    uint8_t *region = data + refs[4].offset;
    refs[4].version = 2;
    memset(region, 0, 48);
    memcpy(region + 6, &(uint16_t){ 1 }, sizeof(uint16_t));
    memcpy(region + 12, &(uint32_t){ 3 }, sizeof(uint32_t));
    memcpy(region + 20, &(uint16_t){ 1 }, sizeof(uint16_t));
    return data;
}

static void *build_two_divisions_m3(uint32_t *size) {
    *size = 952;
    uint8_t *data = calloc(1, *size);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *refs = (struct ReferenceEntry *)(data + 920);
    uint8_t *root = data + 32, *divisions = data + 816;
    memcpy(head->id, "43DM", 4);
    head->ofsRefs = 920; head->nRefs = 2; head->MODL = (Reference){ .nEntries = 1, .ref = 0 };
    memcpy(refs[0].id, "LDOM", 4); refs[0].offset = 32; refs[0].nEntries = 1; refs[0].version = 23;
    memcpy(refs[1].id, "_VID", 4); refs[1].offset = 816; refs[1].nEntries = 2; refs[1].version = 2;
    put_ref(root, 112, (Reference){ .nEntries = 2, .ref = 1 });
    memcpy(divisions + 48, &(uint32_t){ 0x11223344 }, sizeof(uint32_t));
    memcpy(divisions + 100, &(uint32_t){ 0x55667788 }, sizeof(uint32_t));
    return data;
}

static void *build_bone_count_m3(uint32_t *size, uint32_t bone_count) {
    uint32_t refs_offset = 912 + bone_count * 160;
    *size = refs_offset + 2 * sizeof(struct ReferenceEntry);
    uint8_t *data = calloc(1, *size);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *refs = (struct ReferenceEntry *)(data + refs_offset);
    uint8_t *root = data + 128;
    memcpy(head->id, "43DM", 4);
    head->ofsRefs = refs_offset; head->nRefs = 2; head->MODL = (Reference){ .nEntries = 1, .ref = 0 };
    memcpy(refs[0].id, "LDOM", 4); refs[0].offset = 128; refs[0].nEntries = 1; refs[0].version = 23;
    memcpy(refs[1].id, "ENOB", 4); refs[1].offset = 912; refs[1].nEntries = bone_count; refs[1].version = 1;
    put_ref(root, 80, (Reference){ .nEntries = bone_count, .ref = 1 });
    return data;
}

typedef enum { BAD_VERTEX_RANGE, BAD_INDEX_RANGE, BAD_FACE_VALUE, BAD_BONE_INDEX } bad_geometry_t;

static void *build_bad_geometry_m3(uint32_t *size, bad_geometry_t bad) {
    uint8_t *data = build_odd_indices_m3(size);
    switch (bad) {
    case BAD_VERTEX_RANGE: memcpy(data + 912 + 8, &(uint32_t){ 1 }, sizeof(uint32_t)); break;
    case BAD_INDEX_RANGE: memcpy(data + 912 + 16, &(uint32_t){ 1 }, sizeof(uint32_t)); break;
    case BAD_FACE_VALUE: memcpy(data + 904, &(uint16_t){ 1 }, sizeof(uint16_t)); break;
    case BAD_BONE_INDEX: data[816 + 16] = 1; break;
    }
    return data;
}

typedef enum {
    ANIM_VALID, ANIM_TWO_KEYS, ANIM_TRUNCATED_SEQUENCE_DATA, ANIM_OVERSIZED_KEYS,
    ANIM_MISMATCHED_KEYS_VALUES, ANIM_MISMATCHED_IDS_REFS,
} anim_fixture_t;

/* One STC v4 record with a scalar SequenceData block; mutations target each raw declaration. */
static void *build_animation_m3(uint32_t *size, anim_fixture_t fixture) {
    uint8_t *data = calloc(1, 1536);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *refs = (struct ReferenceEntry *)(data + 1424);
    uint8_t *root = data + 128, *stc = data + 1024, *sd = data + 1232;
    uint32_t sd_count = fixture == ANIM_TRUNCATED_SEQUENCE_DATA ? 2 : 1;
    uint32_t key_count = fixture == ANIM_OVERSIZED_KEYS ? 5 : fixture == ANIM_TWO_KEYS ? 2 :
                         fixture == ANIM_MISMATCHED_KEYS_VALUES ? 2 : 1;
    uint32_t value_count = fixture == ANIM_MISMATCHED_KEYS_VALUES ? 1 : key_count;
    uint32_t ids_count = fixture == ANIM_MISMATCHED_IDS_REFS ? 2 : 1;
    uint32_t refs_count = fixture == ANIM_MISMATCHED_IDS_REFS ? 1 : ids_count;
    memcpy(head->id, "43DM", 4);
    head->ofsRefs = 1424; head->nRefs = 7; head->MODL = (Reference){ .nEntries = 1, .ref = 0 };
    memcpy(refs[0].id, "LDOM", 4); refs[0].offset = 128; refs[0].nEntries = 1; refs[0].version = 23;
    memcpy(refs[1].id, "_CTS", 4); refs[1].offset = 1024; refs[1].nEntries = 1; refs[1].version = 4;
    memcpy(refs[2].id, "3RDS", 4); refs[2].offset = 1232; refs[2].nEntries = sd_count;
    memcpy(refs[3].id, "_23I", 4); refs[3].offset = 1264; refs[3].nEntries = key_count;
    memcpy(refs[4].id, "LAER", 4); refs[4].offset = 1280; refs[4].nEntries = value_count;
    memcpy(refs[5].id, "_23U", 4); refs[5].offset = 1392; refs[5].nEntries = ids_count;
    memcpy(refs[6].id, "_23U", 4); refs[6].offset = 1408; refs[6].nEntries = refs_count;
    put_ref(root, 28, (Reference){ .nEntries = 1, .ref = 1 });
    put_ref(stc, 20, (Reference){ .nEntries = ids_count, .ref = 5 });
    put_ref(stc, 32, (Reference){ .nEntries = refs_count, .ref = 6 });
    put_ref(stc, 48, (Reference){ .nEntries = sd_count, .ref = 2 });
    put_ref(sd, 0, (Reference){ .nEntries = key_count, .ref = 3 });
    put_ref(sd, 20, (Reference){ .nEntries = value_count, .ref = 4 });
    if (fixture == ANIM_TWO_KEYS) {
        memcpy(data + refs[3].offset, &(uint32_t){ 10 }, sizeof(uint32_t));
        memcpy(data + refs[3].offset + 4, &(uint32_t){ 20 }, sizeof(uint32_t));
    }
    *size = 1536;
    return data;
}

static void provider_probe_enter(void) {
    pthread_mutex_lock(&g_provider_probe_lock);
    if (g_provider_probe) {
        g_provider_entries++; g_provider_active++;
        if (g_provider_active > g_provider_max_active) g_provider_max_active = g_provider_active;
        pthread_cond_broadcast(&g_provider_probe_cond);
        while (g_provider_blocked) pthread_cond_wait(&g_provider_probe_cond, &g_provider_probe_lock);
        g_provider_active--;
    }
    pthread_mutex_unlock(&g_provider_probe_lock);
}

static void *fixture_read_file(const char *identity, uint32_t *size) {
    provider_probe_enter();
    pthread_mutex_lock(&g_provider_probe_lock); g_reads++; pthread_mutex_unlock(&g_provider_probe_lock);
    if (!strcmp(identity, "Assets\\Models\\empty.m3")) return build_empty_m3(size, 23);
    if (!strcmp(identity, "Assets\\Models\\v24.m3")) return build_empty_m3(size, 24);
    if (!strcmp(identity, "Assets\\Models\\odd.m3")) return build_odd_indices_m3(size);
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

static void assert_parser_rejects(void *data, uint32_t size) {
    m3Model_t *model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model);
    ASSERT_NULL(model->head);
    SC2_M3Free(model);
    free(data);
}

static void test_parser_root_and_disk_stride_declarations(void) {
    uint32_t size;
    void *data = build_empty_m3(&size, 23);
    struct MD33 *head = data;
    struct ReferenceEntry *refs = (struct ReferenceEntry *)((uint8_t *)data + head->ofsRefs);
    memcpy(refs[0].id, "NOPE", 4);
    assert_parser_rejects(data, size);

    data = build_empty_m3(&size, 23); head = data;
    refs = (struct ReferenceEntry *)((uint8_t *)data + head->ofsRefs);
    head->MODL.nEntries = refs[0].nEntries = 2;
    assert_parser_rejects(data, size);

    data = build_material_v15_m3(&size);
    m3Model_t *model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head);
    ASSERT_EQ_INT(model->materialStandardNum, 1);
    SC2_M3Free(model); free(data);

    data = build_material_v15_m3(&size); head = data;
    refs = (struct ReferenceEntry *)((uint8_t *)data + head->ofsRefs);
    memcpy(refs[1].id, "NOPE", 4);
    assert_parser_rejects(data, size);

    data = build_version_zero_animation_tables_m3(&size);
    model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head);
    ASSERT_EQ_INT(model->stgNum, 1); ASSERT_EQ_INT(model->stsNum, 1);
    SC2_M3Free(model); free(data);

    static const struct { uint32_t version, stride; } roots[] = {
        { 20, 748 }, { 21, 760 }, { 23, 784 },
    };
    FOR_LOOP(i, sizeof(roots) / sizeof(roots[0])) {
        data = build_exact_empty_m3(&size, roots[i].version, roots[i].stride);
        model = SC2_M3Parse(data, size);
        ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head); ASSERT_EQ_INT(model->type, roots[i].version);
        SC2_M3Free(model); free(data);
    }
    data = build_exact_empty_m3(&size, 22, 760); assert_parser_rejects(data, size);

    data = build_two_divisions_m3(&size); model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head); ASSERT_EQ_INT(model->divisionsNum, 2);
    ASSERT_EQ_INT(model->divisions[0].unknown, 0x11223344);
    ASSERT_EQ_INT(model->divisions[1].unknown, 0x55667788);
    SC2_M3Free(model); free(data);

    data = build_region_v2_m3(&size); model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head); ASSERT_EQ_INT(model->divisions[0].regionsNum, 1);
    ASSERT_EQ_INT(model->divisions[0].regions[0].firstVertexIndex, 0);
    ASSERT_EQ_INT(model->divisions[0].regions[0].verticesCount, 1);
    ASSERT_EQ_INT(model->divisions[0].regions[0].triangleIndicesCount, 3);
    SC2_M3Free(model); free(data);
}

static void test_parser_rejects_truncated_string_reference(void) {
    uint32_t size = 1072;
    uint8_t *data = calloc(1, size);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *refs = (struct ReferenceEntry *)(data + 1040);
    memcpy(head->id, "43DM", 4);
    head->ofsRefs = 1040; head->nRefs = 2; head->MODL = (Reference){ .nEntries = 1, .ref = 0 };
    memcpy(refs[0].id, "LDOM", 4); refs[0].offset = 128; refs[0].nEntries = 1; refs[0].version = 23;
    memcpy(refs[1].id, "RAHC", 4); refs[1].offset = 1024; refs[1].nEntries = 100;
    put_ref(data + 128, 0, (Reference){ .nEntries = 100, .ref = 1 });
    assert_parser_rejects(data, size);
}

static void test_parser_animation_declaration_inverses(void) {
    uint32_t size;
    void *data = build_animation_m3(&size, ANIM_VALID);
    m3Model_t *model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head); ASSERT_EQ_INT(model->stcNum, 1);
    SC2_M3Free(model); free(data);
    data = build_animation_m3(&size, ANIM_TRUNCATED_SEQUENCE_DATA); assert_parser_rejects(data, size);
    data = build_animation_m3(&size, ANIM_OVERSIZED_KEYS); assert_parser_rejects(data, size);
    data = build_animation_m3(&size, ANIM_MISMATCHED_KEYS_VALUES); assert_parser_rejects(data, size);
    data = build_animation_m3(&size, ANIM_MISMATCHED_IDS_REFS); assert_parser_rejects(data, size);
}

static void test_animation_value_versions_and_key_spans(void) {
    uint32_t size;
    uint8_t *data = build_animation_m3(&size, ANIM_VALID);
    struct MD33 *head = (struct MD33 *)data;
    struct ReferenceEntry *refs = (struct ReferenceEntry *)(data + head->ofsRefs);
    static const struct { char id[4]; uint32_t version; } valid[] = {
        { "GALF", 0 }, { "TNVE", 0 }, { "TNVE", 1 }, { "TNVE", 2 },
    };
    FOR_LOOP(i, sizeof(valid) / sizeof(valid[0])) {
        memcpy(refs[4].id, valid[i].id, 4); refs[4].version = valid[i].version;
        m3Model_t *model = SC2_M3Parse(data, size);
        ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head);
        SC2_M3Free(model);
    }
    memcpy(refs[4].id, "TNVE", 4); refs[4].version = 3;
    void *bad = malloc(size); memcpy(bad, data, size); assert_parser_rejects(bad, size);
    free(data);

    data = build_animation_m3(&size, ANIM_TWO_KEYS);
    m3Model_t *model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head);
    m3SequenceData_t sd;
    m3ReferenceRead_t read = { .reference = model->stc[0].sd[0],
        .element_size = sizeof(m3SequenceData_t) };
    ASSERT(SC2_M3ReferenceElement(model, &read, &sd));
    m3KeySpan_t span;
    ASSERT(!m3_find_key_span(model, sd.keys, 9, &span));
    ASSERT(m3_find_key_span(model, sd.keys, 10, &span));
    ASSERT_EQ_INT(span.left, 0); ASSERT_EQ_INT(span.right, 1); ASSERT_FLOAT_EQ(span.fraction, 0.f);
    ASSERT(m3_find_key_span(model, sd.keys, 15, &span));
    ASSERT_EQ_INT(span.left, 0); ASSERT_EQ_INT(span.right, 1);
    ASSERT_FLOAT_EQ(span.fraction, 0.5f);
    ASSERT(m3_find_key_span(model, sd.keys, 20, &span));
    ASSERT_EQ_INT(span.left, 1); ASSERT_EQ_INT(span.right, 1);
    SC2_M3Free(model); free(data);

    data = build_animation_m3(&size, ANIM_VALID); model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head);
    read = (m3ReferenceRead_t){ .reference = model->stc[0].sd[0],
        .element_size = sizeof(m3SequenceData_t) };
    ASSERT(SC2_M3ReferenceElement(model, &read, &sd));
    ASSERT(m3_find_key_span(model, sd.keys, 0, &span));
    ASSERT_EQ_INT(span.left, 0); ASSERT_EQ_INT(span.right, 0);
    SC2_M3Free(model); free(data);
}

static void test_odd_indices_keep_all_trailing_arrays_aligned(void) {
    const bzSC2Model_t *model;
    bzSC2MModelInfo_t info;
    bzSC2MDivisionInfo_t division;
    BZ_SC2A_Init(); BZ_SC2M_Init(); BZ_SC2M_BeginRegistration(12);
    model = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "Assets\\Models\\odd.m3");
    ASSERT_EQ_INT(BZ_SC2Model_Status(model), BZ_SC2M_OK);
    ASSERT(BZ_SC2Model_Info(model, &info));
    ASSERT_EQ_INT(info.index_count, 3); ASSERT_EQ_INT(info.division_count, 1);
    ASSERT_EQ_INT(model->vertices_offset % _Alignof(bzSC2MVertex_t), 0);
    ASSERT_EQ_INT(model->indices_offset % _Alignof(uint16_t), 0);
    ASSERT_EQ_INT(model->divisions_offset % _Alignof(bzSC2MDivisionInfo_t), 0);
    ASSERT_EQ_INT(model->regions_offset % _Alignof(bzSC2MRegionInfo_t), 0);
    ASSERT(BZ_SC2Model_DivisionInfo(model, 0, &division));
    ASSERT_EQ_INT(division.index_count, 3);
    BZ_SC2Model_Release(model);
    BZ_SC2M_Shutdown(); BZ_SC2A_Shutdown();
}

static void test_geometry_validation_rejects_ranges_and_faces(void) {
    uint32_t size;
    void *data = build_bad_geometry_m3(&size, BAD_VERTEX_RANGE); assert_parser_rejects(data, size);
    data = build_bad_geometry_m3(&size, BAD_INDEX_RANGE); assert_parser_rejects(data, size);
    data = build_bad_geometry_m3(&size, BAD_FACE_VALUE); assert_parser_rejects(data, size);
    data = build_bad_geometry_m3(&size, BAD_BONE_INDEX); assert_parser_rejects(data, size);
}

static void test_renderer_bone_capability(void) {
    uint32_t size;
    struct MD33 head = { 0 };
    m3Region_t region = { .firstBoneLookupIndex = 128, .boneLookupIndicesCount = 1 };
    m3Divisions_t division = { .regions = &region, .regionsNum = 1 };
    m3Model_t boundary = { .head = &head, .valid = true, .bonesNum = 128,
        .boneLookupNum = 129, .divisions = &division, .divisionsNum = 1 };
    void *data = build_bone_count_m3(&size, 128);
    m3Model_t *model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head); ASSERT_EQ_INT(model->bonesNum, 128);
    ASSERT(m3_renderer_model_supported(model));
    SC2_M3Free(model); free(data);

    data = build_bone_count_m3(&size, 129); model = SC2_M3Parse(data, size);
    ASSERT_NOT_NULL(model); ASSERT_NOT_NULL(model->head); ASSERT_EQ_INT(model->bonesNum, 129);
    ASSERT(!m3_renderer_model_supported(model));
    SC2_M3Free(model); free(data);

    ASSERT(!m3_renderer_model_supported(&boundary));
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

static void *register_model_miss(void *unused) {
    const bzSC2Model_t *model;
    (void)unused;
    model = BZ_SC2M_RegisterModel(BZ_SC2M_ABI_VERSION, "Assets\\Models\\empty.m3");
    if (model) BZ_SC2Model_Release(model);
    return NULL;
}

static void *register_image_miss(void *unused) {
    const bzSC2Image_t *image;
    (void)unused;
    image = BZ_SC2A_RegisterImage(BZ_SC2A_ABI_VERSION, "Assets\\Textures\\missing.dds");
    if (image) BZ_SC2AImage_Release(image);
    return NULL;
}

static void test_model_and_image_misses_share_provider_serialization(void) {
    pthread_t model_thread, image_thread;
    g_reads = 0;
    BZ_SC2A_Init(); BZ_SC2M_Init(); BZ_SC2M_BeginRegistration(13);
    pthread_mutex_lock(&g_provider_probe_lock);
    g_provider_probe = g_provider_blocked = true;
    g_provider_entries = g_provider_active = g_provider_max_active = 0;
    pthread_mutex_unlock(&g_provider_probe_lock);
    ASSERT_EQ_INT(pthread_create(&model_thread, NULL, register_model_miss, NULL), 0);
    ASSERT_EQ_INT(pthread_create(&image_thread, NULL, register_image_miss, NULL), 0);
    pthread_mutex_lock(&g_provider_probe_lock);
    while (!g_provider_entries) pthread_cond_wait(&g_provider_probe_cond, &g_provider_probe_lock);
    pthread_mutex_unlock(&g_provider_probe_lock);
    FOR_LOOP(i, 10000) sched_yield();
    pthread_mutex_lock(&g_provider_probe_lock);
    g_provider_blocked = false; pthread_cond_broadcast(&g_provider_probe_cond);
    pthread_mutex_unlock(&g_provider_probe_lock);
    ASSERT_EQ_INT(pthread_join(model_thread, NULL), 0);
    ASSERT_EQ_INT(pthread_join(image_thread, NULL), 0);
    pthread_mutex_lock(&g_provider_probe_lock);
    ASSERT_EQ_INT(g_provider_max_active, 1);
    g_provider_probe = false;
    pthread_mutex_unlock(&g_provider_probe_lock);
    BZ_SC2M_Shutdown(); BZ_SC2A_Shutdown();
}

int main(void) {
    RUN_TEST(test_lifecycle_and_abi);
    RUN_TEST(test_valid_zero_geometry_and_cache);
    RUN_TEST(test_placeholders_and_confinement);
    RUN_TEST(test_parser_root_and_disk_stride_declarations);
    RUN_TEST(test_parser_rejects_truncated_string_reference);
    RUN_TEST(test_parser_animation_declaration_inverses);
    RUN_TEST(test_animation_value_versions_and_key_spans);
    RUN_TEST(test_odd_indices_keep_all_trailing_arrays_aligned);
    RUN_TEST(test_geometry_validation_rejects_ranges_and_faces);
    RUN_TEST(test_renderer_bone_capability);
    RUN_TEST(test_concurrent_same_identity_publication);
    RUN_TEST(test_model_and_image_misses_share_provider_serialization);
    TEST_RESULTS();
}
