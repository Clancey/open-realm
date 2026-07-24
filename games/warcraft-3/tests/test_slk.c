/*
 * test_slk.c — Tests for SLK data reading and unit-stat lookup.
 *
 * Two families of tests:
 *
 *  1. FS_FindSheetCell — verifies the pure-C linked-list traversal of
 *     sheetRow_t / sheetField_t, which is the fundamental operation
 *     underlying all unit-stat reads.
 *
 *  2. In-memory SLK parsing — verifies that parse_slk_string() correctly
 *     converts the SLK spreadsheet text format into the same linked-list
 *     structures (column headers, row keys, field values).
 *
 *  3. Unit stat accessors — verifies UnitIntegerField / UnitRealField
 *     through the mock UnitsMetaData tables set up by the test harness,
 *     covering the Peasant (hpea) and Footman (hfoo) test units.
 */

#include <pthread.h>
#include <stdatomic.h>

#include "test_framework.h"
#include "test_harness.h"
#include "g_metadata_schema.h"

/* Defined in g_metadata.c; swaps the sheet backing one metadata table. */
void G_SetConfigTable(sheetMetaData_t *metadatas, LPCSTR slk, sheetRow_t *table);

/* -----------------------------------------------------------------------
 * 1.  FS_FindSheetCell
 * --------------------------------------------------------------------- */

static void test_find_cell_existing_row_and_column(void) {
    sheetField_t f = {"spd", "270", NULL};
    sheetRow_t   r = {"hpea", &f, NULL};

    ASSERT_STR_EQ(FS_FindSheetCell(&r, "hpea", "spd"), "270");
}

static void test_find_cell_missing_row_returns_null(void) {
    sheetField_t f = {"spd", "270", NULL};
    sheetRow_t   r = {"hpea", &f, NULL};

    ASSERT_NULL(FS_FindSheetCell(&r, "hfoo", "spd"));
}

static void test_find_cell_missing_column_returns_null(void) {
    sheetField_t f = {"spd", "270", NULL};
    sheetRow_t   r = {"hpea", &f, NULL};

    ASSERT_NULL(FS_FindSheetCell(&r, "hpea", "hp"));
}

static void test_find_cell_case_insensitive_column(void) {
    /* Column names are matched case-insensitively per FS_FindSheetCell. */
    sheetField_t f = {"RealHP", "250", NULL};
    sheetRow_t   r = {"hpea", &f, NULL};

    ASSERT_STR_EQ(FS_FindSheetCell(&r, "hpea", "realHP"), "250");
    ASSERT_STR_EQ(FS_FindSheetCell(&r, "hpea", "REALHP"), "250");
}

static void test_find_cell_multiple_rows(void) {
    sheetField_t fa = {"spd", "270", NULL};
    sheetField_t fb = {"spd", "300", NULL};
    sheetRow_t   rb = {"hfoo", &fb, NULL};
    sheetRow_t   ra = {"hpea", &fa, &rb};

    ASSERT_STR_EQ(FS_FindSheetCell(&ra, "hpea", "spd"), "270");
    ASSERT_STR_EQ(FS_FindSheetCell(&ra, "hfoo", "spd"), "300");
}

static void test_find_cell_multiple_fields(void) {
    sheetField_t fb = {"realHP", "250", NULL};
    sheetField_t fa = {"spd",    "270", &fb};
    sheetRow_t   r  = {"hpea", &fa, NULL};

    ASSERT_STR_EQ(FS_FindSheetCell(&r, "hpea", "spd"),    "270");
    ASSERT_STR_EQ(FS_FindSheetCell(&r, "hpea", "realHP"), "250");
}

static void test_find_cell_null_sheet_returns_null(void) {
    ASSERT_NULL(FS_FindSheetCell(NULL, "hpea", "spd"));
}

/* -----------------------------------------------------------------------
 * 2.  In-memory SLK parsing (parse_slk_string)
 * --------------------------------------------------------------------- */

/* Minimal SLK snippet: two data rows, three columns. */
static const char slk_two_units[] =
    "ID;PWXL;N;EBB;Y3;X4\n"
    "C;Y1;X1;K\"unitBalanceID\"\n"
    "C;Y1;X2;K\"spd\"\n"
    "C;Y1;X3;K\"realHP\"\n"
    "C;Y1;X4;K\"bldtm\"\n"
    "C;Y2;X1;K\"hpea\"\n"
    "C;Y2;X2;K\"270\"\n"
    "C;Y2;X3;K\"250\"\n"
    "C;Y2;X4;K\"45\"\n"
    "C;Y3;X1;K\"hfoo\"\n"
    "C;Y3;X2;K\"270\"\n"
    "C;Y3;X3;K\"420\"\n"
    "C;Y3;X4;K\"60\"\n"
    "E\n";

static void test_slk_parse_returns_non_null(void) {
    sheetRow_t *rows = parse_slk_string(slk_two_units);
    ASSERT_NOT_NULL(rows);
    free_slk_rows(rows);
}

static void test_slk_parse_row_names(void) {
    sheetRow_t *rows = parse_slk_string(slk_two_units);
    ASSERT_NOT_NULL(rows);
    ASSERT_STR_EQ(rows->name, "hpea");
    ASSERT_NOT_NULL(rows->next);
    ASSERT_STR_EQ(rows->next->name, "hfoo");
    free_slk_rows(rows);
}

static void test_slk_parse_field_values(void) {
    sheetRow_t *rows = parse_slk_string(slk_two_units);
    ASSERT_NOT_NULL(rows);

    ASSERT_STR_EQ(FS_FindSheetCell(rows, "hpea", "spd"),    "270");
    ASSERT_STR_EQ(FS_FindSheetCell(rows, "hpea", "realHP"), "250");
    ASSERT_STR_EQ(FS_FindSheetCell(rows, "hpea", "bldtm"),  "45");
    ASSERT_STR_EQ(FS_FindSheetCell(rows, "hfoo", "realHP"), "420");
    ASSERT_STR_EQ(FS_FindSheetCell(rows, "hfoo", "bldtm"),  "60");

    free_slk_rows(rows);
}

static void test_slk_parse_missing_cell_returns_null(void) {
    sheetRow_t *rows = parse_slk_string(slk_two_units);
    ASSERT_NOT_NULL(rows);
    ASSERT_NULL(FS_FindSheetCell(rows, "hkni", "spd"));  /* row absent */
    ASSERT_NULL(FS_FindSheetCell(rows, "hpea", "armor")); /* column absent */
    free_slk_rows(rows);
}

static void test_slk_parse_empty_string_returns_null(void) {
    /* No C/F lines means no data rows. */
    sheetRow_t *rows = parse_slk_string("ID;PWXL\nE\n");
    ASSERT_NULL(rows);
}

/* -----------------------------------------------------------------------
 * 3.  Unit stat accessors via mock metadata tables
 * --------------------------------------------------------------------- */

static void test_unit_speed_peasant(void) {
    ASSERT_FLOAT_EQ(UNIT_SPEED(UNIT_ID("hpea")), 270.0f);
}

static void test_unit_speed_footman(void) {
    ASSERT_FLOAT_EQ(UNIT_SPEED(UNIT_ID("hfoo")), 270.0f);
}

static void test_unit_hp_peasant(void) {
    ASSERT_FLOAT_EQ(UNIT_HP(UNIT_ID("hpea")), 250.0f);
}

static void test_unit_hp_footman(void) {
    ASSERT_FLOAT_EQ(UNIT_HP(UNIT_ID("hfoo")), 420.0f);
}

static void test_unit_build_time_peasant(void) {
    ASSERT_EQ_INT(UNIT_BUILD_TIME(UNIT_ID("hpea")), 45);
}

static void test_unit_build_time_footman(void) {
    ASSERT_EQ_INT(UNIT_BUILD_TIME(UNIT_ID("hfoo")), 60);
}

static void test_unit_collision_peasant(void) {
    ASSERT_EQ_INT(UNIT_COLLISION(UNIT_ID("hpea")), 16);
}

static void test_unit_unknown_id_returns_zero(void) {
    /* Unknown unit ID must not crash and must return 0 / 0.0. */
    ASSERT_FLOAT_EQ(UNIT_SPEED(UNIT_ID("xxxx")),      0.0f);
    ASSERT_FLOAT_EQ(UNIT_HP(UNIT_ID("xxxx")),         0.0f);
    ASSERT_EQ_INT  (UNIT_BUILD_TIME(UNIT_ID("xxxx")), 0);
}

/* UnitMetaData.slk, not the ROC-generated registry, owns expansion sheet placement. */
static void test_unit_metadata_schema_tracks_archive_variant(void) {
    sheetField_t roc_ucol_slk = { "slk", "UnitData" };
    sheetField_t roc_ucol_field = { "field", "collision", &roc_ucol_slk };
    sheetRow_t roc_ucol = { "ucol", &roc_ucol_field };
    sheetField_t tft_ubdg_slk = { "slk", "UnitBalance" };
    sheetField_t tft_ubdg_field = { "field", "isbldg", &tft_ubdg_slk };
    sheetField_t tft_ucol_slk = { "slk", "UnitBalance" };
    sheetField_t tft_ucol_field = { "field", "collision", &tft_ucol_slk };
    sheetField_t tft_umpm_slk = { "slk", "UnitBalance" };
    sheetField_t tft_umpm_field = { "field", "manaN", &tft_umpm_slk };
    sheetRow_t tft_umpm = { "umpm", &tft_umpm_field };
    sheetRow_t tft_ubdg = { "ubdg", &tft_ubdg_field, &tft_umpm };
    sheetRow_t tft_ucol = { "ucol", &tft_ucol_field, &tft_ubdg };
    sheetField_t malformed_field = { "field", "collision" };
    sheetRow_t malformed = { "ucol", &malformed_field };
    sheetField_t htow_building = { "isbldg", "1" }, hfoo_collision = { "collision", "31" };
    sheetRow_t htow = { "htow", &htow_building }, hfoo = { "hfoo", &hfoo_collision, &htow };
    sheetMetaData_t metadata[] = {
        { "ucol", "collision", "UnitData", (sheetRow_t *)1 },
        { "ubdg", "isbldg", "UnitUI", (sheetRow_t *)1 },
        { "umpm", "realM", "UnitBalance", (sheetRow_t *)1 },
        { NULL },
    };

    G_ApplyMetaDataSchema(metadata, &roc_ucol);
    ASSERT_STR_EQ(metadata[0].slk, "UnitData");
    ASSERT_STR_EQ(metadata[1].slk, "UnitUI");
    ASSERT_NULL(metadata[0].table);
    ASSERT_NULL(metadata[1].table);

    G_ApplyMetaDataSchema(metadata, &tft_ucol);
    ASSERT_STR_EQ(metadata[0].field, "collision");
    ASSERT_STR_EQ(metadata[0].slk, "UnitBalance");
    ASSERT_STR_EQ(metadata[1].field, "isbldg");
    ASSERT_STR_EQ(metadata[1].slk, "UnitBalance");
    ASSERT_STR_EQ(metadata[2].field, "realM");
    ASSERT_STR_EQ(metadata[2].slk, "UnitBalance");
    G_SetConfigTable(metadata, "UnitBalance", &hfoo);
    ASSERT_FLOAT_EQ(UnitRealFieldBase(metadata, UNIT_ID("hfoo"), "ucol"), 31.0f);
    ASSERT(UnitBooleanFieldBase(metadata, UNIT_ID("htow"), "ubdg"));
    G_ApplyMetaDataSchema(metadata, &malformed);
    ASSERT_STR_EQ(metadata[0].slk, "");
    ASSERT_NULL(metadata[0].table);
    G_ApplyMetaDataSchema(metadata, NULL);
    ASSERT_STR_EQ(metadata[0].slk, "");
    ASSERT_NULL(metadata[0].table);
}

/* Every unit that failed real TFT Human02 must read the replacement UnitBalance columns. */
static void test_tft_metadata_schema_resolves_human02_units(void) {
    static LPCSTR ids[] = {
        "ngnw", "nmrl", "nmrm", "ngt2", "ngnh", "ofor", "nogr", "ohun", "ogru",
        "ngna", "ngno", "nC03", "nshe", "npig", "nmh0", "npgf", "nmh1", "hfoo",
        "nser", "obea", "hpea", "hbla", "hC02", "nvil", "Obla", "orai", "hrif",
        "nomg", "hwtw", "obar", "nmrr", "htow", "hlum", "halt", "nvlw", "Huth",
    };
    sheetField_t schema_slk[] = { { "slk", "UnitBalance" }, { "slk", "UnitBalance" } };
    sheetField_t schema_field[] = {
        { "field", "collision", &schema_slk[0] }, { "field", "isbldg", &schema_slk[1] },
    };
    sheetRow_t schema[] = { { "ucol", &schema_field[0], &schema[1] }, { "ubdg", &schema_field[1] } };
    sheetMetaData_t metadata[] = {
        { "ucol", "collision", "UnitData" }, { "ubdg", "isbldg", "UnitUI" }, { NULL },
    };
    unitData_t aliases[] = {
        { .originalUnitID = UNIT_ID("nrdk"), .newUnitID = UNIT_ID("nC03") },
        { .originalUnitID = UNIT_ID("hmtm"), .newUnitID = UNIT_ID("hC02") },
    };
    struct mapInfo_s map = { .num_userCreatedUnits = 2, .userCreatedUnits = aliases };
    const metadataMapSnapshot_t *snapshot;
    sheetField_t fields[sizeof(ids) / sizeof(ids[0])][2];
    sheetRow_t rows[sizeof(ids) / sizeof(ids[0])];

    for (DWORD i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        fields[i][0] = (sheetField_t){ "collision", "31", &fields[i][1] };
        fields[i][1] = (sheetField_t){ "isbldg", !strcmp(ids[i], "htow") ? "1" : "0" };
        rows[i] = (sheetRow_t){ ids[i], fields[i], i + 1 < sizeof(ids) / sizeof(ids[0]) ? &rows[i + 1] : NULL };
        if (!strcmp(ids[i], "nC03")) rows[i].name = "nrdk";
        else if (!strcmp(ids[i], "hC02")) rows[i].name = "hmtm";
    }
    G_ApplyMetaDataSchema(metadata, schema);
    G_SetConfigTable(metadata, "UnitBalance", rows);
    G_MetadataPublishMap(&map);
    snapshot = G_MetadataMapAcquire();
    for (DWORD i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        DWORD table_id = G_MetadataMapClass(snapshot, UNIT_ID(ids[i]));
        ASSERT_NOT_NULL(UnitStringFieldBase(metadata, table_id, "ucol"));
        ASSERT_NOT_NULL(UnitStringFieldBase(metadata, table_id, "ubdg"));
    }
    ASSERT_FLOAT_EQ(UnitRealFieldBase(metadata, UNIT_ID("hfoo"), "ucol"), 31.0f);
    ASSERT(UnitBooleanFieldBase(metadata, UNIT_ID("htow"), "ubdg"));
    G_MetadataMapRelease(snapshot);
    G_MetadataPublishMap(level.mapinfo);
}

typedef struct {
    DWORD class_id;
    char observed[5];
} classNameCtx_t;

static pthread_mutex_t class_name_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t class_name_cond = PTHREAD_COND_INITIALIZER;
static bool class_name_ready, class_name_release;

static void *hold_class_name(void *opaque) {
    classNameCtx_t *ctx = opaque;
    LPCSTR name = GetClassName(ctx->class_id);
    pthread_mutex_lock(&class_name_lock);
    class_name_ready = true;
    pthread_cond_broadcast(&class_name_cond);
    while (!class_name_release) pthread_cond_wait(&class_name_cond, &class_name_lock);
    pthread_mutex_unlock(&class_name_lock);
    memcpy(ctx->observed, name, sizeof(ctx->observed));
    return NULL;
}

/* Worker metadata readers must not overwrite another thread's transient FourCC row. */
static void test_class_name_is_thread_local(void) {
    classNameCtx_t ctx = { .class_id = UNIT_ID("hpea") };
    pthread_t thread;
    class_name_ready = class_name_release = false;
    ASSERT_EQ_INT(pthread_create(&thread, NULL, hold_class_name, &ctx), 0);
    pthread_mutex_lock(&class_name_lock);
    while (!class_name_ready) pthread_cond_wait(&class_name_cond, &class_name_lock);
    ASSERT_STR_EQ(GetClassName(UNIT_ID("hfoo")), "hfoo");
    class_name_release = true;
    pthread_cond_broadcast(&class_name_cond);
    pthread_mutex_unlock(&class_name_lock);
    ASSERT_EQ_INT(pthread_join(thread, NULL), 0);
    ASSERT_STR_EQ(ctx.observed, "hpea");
}

static void test_metadata_map_snapshot_survives_republication(void) {
    unitData_t first_unit = {
        .originalUnitID = UNIT_ID("hpea"), .newUnitID = UNIT_ID("h000"),
    };
    unitData_t second_unit = {
        .originalUnitID = UNIT_ID("hfoo"), .newUnitID = UNIT_ID("h000"),
    };
    struct mapInfo_s first = { .num_userCreatedUnits = 1, .userCreatedUnits = &first_unit };
    struct mapInfo_s second = { .num_userCreatedUnits = 1, .userCreatedUnits = &second_unit };
    const metadataMapSnapshot_t *old, *current;
    uint64_t old_token;
    G_MetadataPublishMap(&first);
    old = G_MetadataMapAcquire();
    old_token = G_MetadataMapToken(old);
    G_MetadataPublishMap(&second);
    current = G_MetadataMapAcquire();
    ASSERT_EQ_INT(G_MetadataMapClass(old, UNIT_ID("h000")), UNIT_ID("hpea"));
    ASSERT_EQ_INT(G_MetadataMapClass(current, UNIT_ID("h000")), UNIT_ID("hfoo"));
    ASSERT(G_MetadataMapToken(current) != old_token);
    G_MetadataMapRelease(old); G_MetadataMapRelease(current);
    G_MetadataPublishMap(level.mapinfo);
}

typedef struct {
    atomic_bool running, bad;
} metadataSnapshotCtx_t;

static void *read_metadata_snapshots(void *opaque) {
    metadataSnapshotCtx_t *ctx = opaque;
    while (atomic_load(&ctx->running)) {
        const metadataMapSnapshot_t *snapshot = G_MetadataMapAcquire();
        DWORD class_id = G_MetadataMapClass(snapshot, UNIT_ID("h000"));
        if (class_id != UNIT_ID("hpea") && class_id != UNIT_ID("hfoo"))
            atomic_store(&ctx->bad, true);
        G_MetadataMapRelease(snapshot);
    }
    return NULL;
}

static void test_metadata_map_snapshot_concurrent_publication(void) {
    unitData_t units[2] = {
        { .originalUnitID = UNIT_ID("hpea"), .newUnitID = UNIT_ID("h000") },
        { .originalUnitID = UNIT_ID("hfoo"), .newUnitID = UNIT_ID("h000") },
    };
    struct mapInfo_s maps[2] = {
        { .num_userCreatedUnits = 1, .userCreatedUnits = units },
        { .num_userCreatedUnits = 1, .userCreatedUnits = units + 1 },
    };
    metadataSnapshotCtx_t ctx;
    pthread_t thread;
    atomic_init(&ctx.running, true); atomic_init(&ctx.bad, false);
    G_MetadataPublishMap(maps);
    ASSERT_EQ_INT(pthread_create(&thread, NULL, read_metadata_snapshots, &ctx), 0);
    for (DWORD i = 0; i < 1000; i++) G_MetadataPublishMap(maps + (i & 1));
    atomic_store(&ctx.running, false);
    ASSERT_EQ_INT(pthread_join(thread, NULL), 0);
    ASSERT(!atomic_load(&ctx.bad));
    G_MetadataPublishMap(level.mapinfo);
}

/*
 * Max mana must come from the computed 'realM' column, not the base 'manaN'.
 * Heroes carry manaN == 0 (their pool is derived from Intelligence) but a
 * non-zero realM, mirroring how max HP uses 'realHP' rather than a base column.
 * Reading manaN left Maiev (Ewar: manaN 0 / realM 225) showing no mana.
 */
static void test_unit_mana_uses_realM_not_manaN(void) {
    static const char slk_mana[] =
        "ID;PWXL;N;EBB;Y3;X4\n"
        "C;Y1;X1;K\"unitBalanceID\"\n"
        "C;Y1;X2;K\"manaN\"\n"
        "C;Y1;X3;K\"realM\"\n"
        "C;Y1;X4;K\"mana0\"\n"
        "C;Y2;X1;K\"Ewar\"\n"   /* Maiev: hero, manaN 0, realM 225, mana0 100 */
        "C;Y2;X2;K\"0\"\n"
        "C;Y2;X3;K\"225\"\n"
        "C;Y2;X4;K\"100\"\n"
        "C;Y3;X1;K\"hsor\"\n"   /* Sorceress: caster, manaN == realM == 200 */
        "C;Y3;X2;K\"200\"\n"
        "C;Y3;X3;K\"200\"\n"
        "C;Y3;X4;K\"75\"\n"
        "E\n";
    sheetRow_t *rows = parse_slk_string(slk_mana);

    ASSERT_NOT_NULL(rows);
    G_SetConfigTable(UnitsMetaData, "UnitBalance", rows);

    /* The fix: a hero with manaN 0 still reports its real mana pool. */
    ASSERT_FLOAT_EQ(UNIT_MANA_MAXIMUM(UNIT_ID("Ewar")), 225.0f);
    ASSERT_FLOAT_EQ(UNIT_MANA_INITIAL(UNIT_ID("Ewar")), 100.0f);
    /* Regular casters (manaN == realM) are unaffected. */
    ASSERT_FLOAT_EQ(UNIT_MANA_MAXIMUM(UNIT_ID("hsor")), 200.0f);
    ASSERT_FLOAT_EQ(UNIT_MANA_INITIAL(UNIT_ID("hsor")), 75.0f);

    /* Restore the shared fixture tables for the rest of the suite. */
    setup_test_unit_data();
    free_slk_rows(rows);
}

/*
 * Armor (combat reduction and the info panel) must come from the computed
 * 'realdef' column, not the base 'def'. Heroes carry def == 0 (armor derives
 * from Agility) but a non-zero realdef, mirroring HP/mana realHP/realM.
 */
static void test_unit_armor_uses_realdef_not_def(void) {
    static const char slk_armor[] =
        "ID;PWXL;N;EBB;Y3;X4\n"
        "C;Y1;X1;K\"unitBalanceID\"\n"
        "C;Y1;X2;K\"def\"\n"
        "C;Y1;X3;K\"realdef\"\n"
        "C;Y1;X4;K\"spd\"\n"
        "C;Y2;X1;K\"Ewar\"\n"   /* Maiev: hero, def 0, realdef 4 */
        "C;Y2;X2;K\"0\"\n"
        "C;Y2;X3;K\"4\"\n"
        "C;Y2;X4;K\"270\"\n"
        "C;Y3;X1;K\"hfoo\"\n"   /* Footman: def == realdef == 2 */
        "C;Y3;X2;K\"2\"\n"
        "C;Y3;X3;K\"2\"\n"
        "C;Y3;X4;K\"270\"\n"
        "E\n";
    sheetRow_t *rows = parse_slk_string(slk_armor);

    ASSERT_NOT_NULL(rows);
    G_SetConfigTable(UnitsMetaData, "UnitBalance", rows);

    /* The fix: a hero with def 0 still reports its real (AGI-boosted) armor. */
    ASSERT_FLOAT_EQ(UNIT_ARMOR_VALUE(UNIT_ID("Ewar")), 4.0f);
    ASSERT_FLOAT_EQ(UNIT_ARMOR_VALUE(UNIT_ID("hfoo")), 2.0f);

    setup_test_unit_data();
    free_slk_rows(rows);
}

/* -----------------------------------------------------------------------
 * Suite runner
 * --------------------------------------------------------------------- */

BEGIN_SUITE(slk)
    RUN_TEST(test_find_cell_existing_row_and_column);
    RUN_TEST(test_find_cell_missing_row_returns_null);
    RUN_TEST(test_find_cell_missing_column_returns_null);
    RUN_TEST(test_find_cell_case_insensitive_column);
    RUN_TEST(test_find_cell_multiple_rows);
    RUN_TEST(test_find_cell_multiple_fields);
    RUN_TEST(test_find_cell_null_sheet_returns_null);

    RUN_TEST(test_slk_parse_returns_non_null);
    RUN_TEST(test_slk_parse_row_names);
    RUN_TEST(test_slk_parse_field_values);
    RUN_TEST(test_slk_parse_missing_cell_returns_null);
    RUN_TEST(test_slk_parse_empty_string_returns_null);

    RUN_TEST(test_unit_speed_peasant);
    RUN_TEST(test_unit_speed_footman);
    RUN_TEST(test_unit_hp_peasant);
    RUN_TEST(test_unit_hp_footman);
    RUN_TEST(test_unit_build_time_peasant);
    RUN_TEST(test_unit_build_time_footman);
    RUN_TEST(test_unit_collision_peasant);
    RUN_TEST(test_unit_unknown_id_returns_zero);
    RUN_TEST(test_unit_metadata_schema_tracks_archive_variant);
    RUN_TEST(test_tft_metadata_schema_resolves_human02_units);
    RUN_TEST(test_class_name_is_thread_local);
    RUN_TEST(test_metadata_map_snapshot_survives_republication);
    RUN_TEST(test_metadata_map_snapshot_concurrent_publication);
    RUN_TEST(test_unit_mana_uses_realM_not_manaN);
    RUN_TEST(test_unit_armor_uses_realdef_not_def);
END_SUITE()
