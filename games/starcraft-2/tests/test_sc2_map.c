/*
 * test_sc2_map.c - StarCraft II map fixture coverage.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "common.h"
#include "games/starcraft-2/common/sc2_map.h"
#include "test_framework.h"

#ifndef TEST_SC2_MPQ
#define TEST_SC2_MPQ "build/tests/test-sc2.SC2Maps"
#endif

#define TEST_SC2_SRC_DIR "games/starcraft-2/tests/resources-src"
#define TEST_SC2_TINY_DIR TEST_SC2_SRC_DIR "/Maps/Test/Tiny.SC2Map"
#define TEST_SC2_SHORT_TERRAIN_DIMENSIONS 0
#define TEST_SC2_ZERO_TERRAIN_DIMENSIONS  1
#define TEST_SC2_HUGE_TERRAIN_DIMENSIONS  2

static BOOL sc2_tests_initialized;
static DWORD short_terrain_dimensions;
static DWORD listed_count;
static PATHSTR listed_map;

void Key_Init(void) {
}

void Key_WriteBindings(FILE *file) {
    (void)file;
}

void Cmd_ForwardToServer(LPCSTR text) {
    (void)text;
}

void CL_SetGameplayBindings(void) {
}

void CL_BeginLoadingMap(LPCSTR mapName) {
    (void)mapName;
}

void CL_Shutdown(void) {
}

void SV_Map(LPCSTR pFilename) {
    (void)pFilename;
}

void SV_Shutdown(void) {
}

void Sys_Quit(void) {
}

/* Com_Quit() calls this; this binary never drives BZ_RuntimeInit(), so it
 * has nothing to tear down. */
void BZ_RuntimeShutdown(void) {
}

void PF_Sleep(DWORD msec) {
    (void)msec;
}

static void setup_sc2_tests(void) {
    if (sc2_tests_initialized) {
        return;
    }

    LPCSTR argv[] = { "test_sc2", "-config", "" };
    Com_Init(3, argv);
    ASSERT(FS_AddArchive(TEST_SC2_MPQ) != NULL);
    sc2_tests_initialized = true;
}

static void use_sc2_fs_host(void) {
    SC2_MapSetHost(&(sc2MapHost_t){
        .read_file = FS_ReadFile,
        .free_file = FS_FreeFile,
        .mem_alloc = MemAlloc,
        .mem_free = MemFree,
    });
}

static HANDLE read_test_disk_path(LPCSTR filename, LPDWORD size) {
    FILE *file;
    long file_size;
    LPBYTE data;
    struct stat st;

    if (size) *size = 0;
    if (!filename || !*filename)
        return NULL;
    if (stat(filename, &st) != 0 || !S_ISREG(st.st_mode))
        return NULL;
    file = fopen(filename, "rb");
    if (!file)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (file_size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    data = MemAlloc(file_size ? file_size : 1);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (file_size > 0 && fread(data, 1, file_size, file) != (size_t)file_size) {
        MemFree(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    if (size) *size = (DWORD)file_size;
    return data;
}

static void normalize_disk_path(LPSTR path) {
    if (!path) return;
    for (LPSTR p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

static HANDLE read_test_disk_file(LPCSTR filename, LPDWORD size) {
    char path[MAX_PATHLEN * 2];
    HANDLE data;

    data = read_test_disk_path(filename, size);
    if (data)
        return data;

    snprintf(path, sizeof(path), "%s", filename ? filename : "");
    normalize_disk_path(path);
    data = read_test_disk_path(path, size);
    if (data)
        return data;

    snprintf(path, sizeof(path), "%s/%s", TEST_SC2_SRC_DIR, filename ? filename : "");
    normalize_disk_path(path);
    return read_test_disk_path(path, size);
}

static void use_sc2_disk_host(void) {
    SC2_MapSetHost(&(sc2MapHost_t){
        .read_file = read_test_disk_file,
        .free_file = MemFree,
        .mem_alloc = MemAlloc,
        .mem_free = MemFree,
    });
}

static BOOL test_path_leaf_is(LPCSTR filename, LPCSTR leaf) {
    LPCSTR base;

    if (!filename || !leaf)
        return false;
    base = filename + strlen(filename);
    while (base > filename && base[-1] != '/' && base[-1] != '\\') {
        base--;
    }
    return !strcmp(base, leaf);
}

static HANDLE read_test_no_manifest_file(LPCSTR filename, LPDWORD size) {
    if (test_path_leaf_is(filename, "GameData.xml")) {
        if (size) *size = 0;
        return NULL;
    }
    return read_test_disk_file(filename, size);
}

static void use_sc2_no_manifest_disk_host(void) {
    SC2_MapSetHost(&(sc2MapHost_t){
        .read_file = read_test_no_manifest_file,
        .free_file = MemFree,
        .mem_alloc = MemAlloc,
        .mem_free = MemFree,
    });
}

/* Minimal (fourcc-only) buffers for the 4 documented-but-not-yet-parsed embedded terrain
 * files (docs/embedded-map-files.md), so SC2_MapLoad's presence detection has something to
 * find without needing new on-disk fixtures. */
static HANDLE make_unsupported_layer_file(DWORD fourcc, LPDWORD size) {
    LPDWORD data = MemAlloc(sizeof(DWORD));

    if (!data)
        return NULL;
    *data = fourcc;
    if (size) *size = sizeof(DWORD);
    return data;
}

static HANDLE read_test_unsupported_terrain_layers_file(LPCSTR filename, LPDWORD size) {
    if (test_path_leaf_is(filename, "t3SyncPathingInfo"))
        return make_unsupported_layer_file(MAKEFOURCC('P','A','T','H'), size);
    if (test_path_leaf_is(filename, "t3Water"))
        return make_unsupported_layer_file(MAKEFOURCC('W','A','T','R'), size);
    if (test_path_leaf_is(filename, "t3FluffDoodad"))
        return make_unsupported_layer_file(MAKEFOURCC('D','L','F','T'), size);
    if (test_path_leaf_is(filename, "t3HardTile"))
        return make_unsupported_layer_file(MAKEFOURCC('H','R','D','T'), size);
    return read_test_disk_file(filename, size);
}

static void use_sc2_unsupported_terrain_host(void) {
    SC2_MapSetHost(&(sc2MapHost_t){
        .read_file = read_test_unsupported_terrain_layers_file,
        .free_file = MemFree,
        .mem_alloc = MemAlloc,
        .mem_free = MemFree,
    });
}

static DWORD short_terrain_width(DWORD width) {
    if (short_terrain_dimensions == TEST_SC2_ZERO_TERRAIN_DIMENSIONS)
        return 0;
    if (short_terrain_dimensions == TEST_SC2_HUGE_TERRAIN_DIMENSIONS)
        return 0xffffffffu;
    return width;
}

static DWORD short_terrain_height(DWORD height) {
    if (short_terrain_dimensions == TEST_SC2_ZERO_TERRAIN_DIMENSIONS)
        return 0;
    if (short_terrain_dimensions == TEST_SC2_HUGE_TERRAIN_DIMENSIONS)
        return 0xffffffffu;
    return height;
}

static HANDLE make_short_height_map(LPDWORD size) {
    sc2MapHeightMap_t *layer = MemAlloc(sizeof(*layer));

    if (!layer)
        return NULL;
    memset(layer, 0, sizeof(*layer));
    layer->fourcc = MAKEFOURCC('H','M','A','P');
    layer->width = short_terrain_width(9);
    layer->height = short_terrain_height(7);
    if (size) *size = sizeof(*layer);
    return layer;
}

static HANDLE make_short_sync_height_map(LPDWORD size) {
    sc2MapSyncHeightMap_t *layer = MemAlloc(sizeof(*layer));

    if (!layer)
        return NULL;
    memset(layer, 0, sizeof(*layer));
    layer->fourcc = MAKEFOURCC('S','M','A','P');
    layer->width = short_terrain_width(9);
    layer->height = short_terrain_height(7);
    if (size) *size = sizeof(*layer);
    return layer;
}

static HANDLE make_short_cell_flags(LPDWORD size) {
    sc2MapCellFlags_t *layer = MemAlloc(sizeof(*layer));

    if (!layer)
        return NULL;
    memset(layer, 0, sizeof(*layer));
    layer->fourcc = MAKEFOURCC('L','F','C','T');
    layer->width = short_terrain_width(8);
    layer->height = short_terrain_height(6);
    if (size) *size = sizeof(*layer);
    return layer;
}

static HANDLE make_short_sync_cliff_level(LPDWORD size) {
    sc2MapSyncCliffLevel_t *layer = MemAlloc(sizeof(*layer));

    if (!layer)
        return NULL;
    memset(layer, 0, sizeof(*layer));
    layer->fourcc = MAKEFOURCC('C','L','I','F');
    layer->width = short_terrain_width(8);
    layer->height = short_terrain_height(6);
    if (size) *size = sizeof(*layer);
    return layer;
}

static HANDLE make_short_texture_masks(LPDWORD size) {
    sc2MapTextureMasks_t *layer = MemAlloc(sizeof(*layer));

    if (!layer)
        return NULL;
    memset(layer, 0, sizeof(*layer));
    layer->fourcc = MAKEFOURCC('M','A','S','K');
    layer->width = short_terrain_width(4);
    layer->height = short_terrain_height(4);
    if (size) *size = sizeof(*layer);
    return layer;
}

static HANDLE read_test_short_terrain_file(LPCSTR filename, LPDWORD size) {
    if (size) *size = 0;
    if (test_path_leaf_is(filename, "t3HeightMap"))
        return make_short_height_map(size);
    if (test_path_leaf_is(filename, "t3SyncHeightMap"))
        return make_short_sync_height_map(size);
    if (test_path_leaf_is(filename, "t3CellFlags"))
        return make_short_cell_flags(size);
    if (test_path_leaf_is(filename, "t3SyncCliffLevel"))
        return make_short_sync_cliff_level(size);
    if (test_path_leaf_is(filename, "t3TextureMasks"))
        return make_short_texture_masks(size);
    return read_test_disk_file(filename, size);
}

static void use_sc2_short_terrain_host(DWORD dimensions) {
    short_terrain_dimensions = dimensions;
    SC2_MapSetHost(&(sc2MapHost_t){
        .read_file = read_test_short_terrain_file,
        .free_file = MemFree,
        .mem_alloc = MemAlloc,
        .mem_free = MemFree,
    });
}

static void collect_map(LPCSTR path, void *userData) {
    (void)userData;
    listed_count++;
    if (path && !strcmp(path, "Maps\\Test\\Tiny.SC2Map")) {
        snprintf(listed_map, sizeof(listed_map), "%s", path ? path : "");
    }
}

static void test_sc2_fixture_archive_lists_map_root(void) {
    setup_sc2_tests();
    use_sc2_fs_host();
    listed_count = 0;
    listed_map[0] = '\0';

    ASSERT(FS_ListMaps(collect_map, NULL) >= 1);
    ASSERT(listed_count >= 1);
    ASSERT_STR_EQ(listed_map, "Maps\\Test\\Tiny.SC2Map");
}

static void test_sc2_fixture_short_name_resolves(void) {
    PATHSTR path;

    setup_sc2_tests();
    use_sc2_fs_host();
    ASSERT_EQ_INT(FS_ResolveMapPath("Tiny", path, sizeof(path)), FS_MAP_RESOLVE_OK);
    ASSERT_STR_EQ(path, "Maps\\Test\\Tiny.SC2Map");
}

static void assert_tiny_map_catalog_overrides(sc2Map_t *map) {
    ASSERT_EQ_INT(map->catalog.footprints, 3);
    ASSERT_STR_EQ(map->objects[1].model, "Assets\\Units\\Terran\\MarineManifestModel\\MarineManifestModel.m3");
    ASSERT_STR_EQ(map->objects[1].footprint, "FootprintMarine");
    ASSERT_STR_EQ(map->objects[1].mover, "Ground");
    ASSERT_EQ_INT(map->objects[1].unit_flags, SC2_UNIT_FLAG_MOVABLE);
    ASSERT_EQ_FLOAT(map->objects[1].radius, 0.875f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[1].footprint_width, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[1].footprint_height, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[1].footprint_radius, 0.5f, 0.001f);
    ASSERT_STR_EQ(map->objects[3].footprint, "FootprintDoodad1x1");
    ASSERT_EQ_FLOAT(map->objects[3].footprint_width, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[3].footprint_height, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[3].footprint_radius, 0.7072f, 0.001f);
    ASSERT_STR_EQ(map->objects[6].model, "Assets\\Buildings\\Terran\\SupplyDepotCatalogModel\\SupplyDepotCatalogModel.m3");
    ASSERT_STR_EQ(map->objects[6].footprint, "Footprint2x2");
    ASSERT_STR_EQ(map->objects[6].mover, "None");
    ASSERT_EQ_INT(map->objects[6].unit_flags, SC2_UNIT_FLAG_STRUCTURE);
    ASSERT_EQ_FLOAT(map->objects[6].footprint_width, 2.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[6].footprint_height, 2.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[6].footprint_radius, 1.4143f, 0.001f);
    ASSERT_STR_EQ(map->t3Terrain.terrain_textures[0].diffuse, "Assets\\Textures\\Terrain\\FixtureGrass_Diffuse.dds");
    ASSERT_STR_EQ(map->t3Terrain.terrain_textures[0].normal, "Assets\\Textures\\Terrain\\FixtureGrass_Diffuse_normal.dds");
    ASSERT_STR_EQ(map->t3Terrain.cliff_sets[0].mesh, "CliffNatural0");
}

static void assert_tiny_map_known_file_catalog_fallback(sc2Map_t *map) {
    ASSERT_EQ_INT(map->catalog.footprints, 3);
    ASSERT_STR_EQ(map->objects[1].model, "Assets\\Units\\Terran\\MarineCatalogModel\\MarineCatalogModel.m3");
    ASSERT_STR_EQ(map->objects[1].footprint, "FootprintMarine");
    ASSERT_STR_EQ(map->objects[1].mover, "Ground");
    ASSERT_EQ_FLOAT(map->objects[1].radius, 0.75f, 0.001f);
    ASSERT_STR_EQ(map->objects[6].model, "Assets\\Buildings\\Terran\\SupplyDepotCatalogModel\\SupplyDepotCatalogModel.m3");
    ASSERT_EQ_FLOAT(map->objects[6].footprint_radius, 1.4143f, 0.001f);
    ASSERT_STR_EQ(map->t3Terrain.terrain_textures[0].diffuse, "Assets\\Textures\\Terrain\\FixtureGrass_Diffuse.dds");
    ASSERT_STR_EQ(map->t3Terrain.cliff_sets[0].mesh, "CliffNatural0");
}

static void test_sc2_map_loads_xml_objects_and_terrain(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_fs_host();
    ASSERT(SC2_MapLoad("Maps\\Test\\Tiny.SC2Map"));
    map = SC2_MapCurrent();

    ASSERT_STR_EQ(map->map_name, "SC2 Tiny Fixture");
    ASSERT_EQ_INT(map->MapInfo.fourcc, MAKEFOURCC('I','p','a','M'));
    ASSERT_EQ_INT(map->MapInfo.width, 8);
    ASSERT_EQ_INT(map->MapInfo.height, 6);
    ASSERT_STR_EQ((char const *)map->MapInfo.data, "SC2 Tiny Fixture");
    ASSERT_EQ_INT(map->num_objects, 7);
    assert_tiny_map_catalog_overrides(map);

    ASSERT_STR_EQ(map->objects[0].name, "StartGame02");
    ASSERT_EQ_INT(map->objects[0].id, 10);
    ASSERT_EQ_INT(map->objects[0].type, SC2_OBJECT_CAMERA);
    ASSERT_EQ_FLOAT(map->objects[0].position.x, 10.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[0].position.y, 10.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[0].camera.target.x, 10.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[0].camera.target.y, 10.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[0].camera.distance, 34.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[0].camera.pitch, 56.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[0].camera.yaw, 179.9584f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[0].camera.fov, 27.7998f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[0].camera.znear, 0.0998f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[0].camera.zfar, 400.0f, 0.001f);

    ASSERT_STR_EQ(map->objects[1].name, "Marine");
    ASSERT_EQ_INT(map->objects[1].id, 1);
    ASSERT_EQ_INT(map->objects[1].type, SC2_OBJECT_UNIT);
    ASSERT_EQ_FLOAT(map->objects[1].position.x, 3.5f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[1].position.y, 3.5f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[1].position.z, 0.25f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[1].angle, 0.75f, 0.001f);
    ASSERT_EQ_INT(map->objects[1].player, 1);
    ASSERT_EQ_INT(map->objects[1].section, 7);
    ASSERT_EQ_INT(map->objects[1].resources, 50);

    ASSERT_STR_EQ(map->objects[3].name, "BillboardTall");
    ASSERT_EQ_INT(map->objects[3].id, 3);
    ASSERT_EQ_INT(map->objects[3].type, SC2_OBJECT_DOODAD);
    ASSERT_STR_EQ(map->objects[3].model, "Assets\\Doodads\\BillboardTall\\BillboardTall.m3");
    ASSERT_EQ_FLOAT(map->objects[3].position.z, 8.0f, 0.001f);
    ASSERT_EQ_INT(map->objects[3].flags, SC2_OBJECT_HEIGHT_ABSOLUTE | SC2_OBJECT_FORCE_PLACEMENT);
    ASSERT_EQ_INT(map->objects[3].tint_color.r, 10);
    ASSERT_EQ_INT(map->objects[3].tint_color.g, 20);
    ASSERT_EQ_INT(map->objects[3].tint_color.b, 30);
    ASSERT_EQ_INT(map->objects[3].tint_color.a, 128);

    ASSERT_STR_EQ(map->objects[4].name, "MineralField");
    ASSERT_EQ_INT(map->objects[4].type, SC2_OBJECT_DOODAD);
    ASSERT_STR_EQ(map->objects[4].model, "Assets\\Doodads\\Terran\\MineralField\\MineralField.m3");
    ASSERT_EQ_FLOAT(map->objects[4].position.x, 4.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[4].position.y, 3.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[4].position.z, 0.0f, 0.001f);

    ASSERT_STR_EQ(map->objects[5].name, "StartPoint01");
    ASSERT_EQ_INT(map->objects[5].id, 5);
    ASSERT_EQ_INT(map->objects[5].type, SC2_OBJECT_POINT);
    ASSERT_STR_EQ(map->objects[5].type_name, "StartLocation");
    ASSERT_STR_EQ(map->objects[5].model, "Assets\\Editor\\StartLocation\\StartLocation.m3");
    ASSERT_STR_EQ(map->objects[5].anim_props, "Stand");
    ASSERT_STR_EQ(map->objects[5].sound, "Assets\\Sounds\\StartLocation.ogg");
    ASSERT_STR_EQ(map->objects[5].attach_id, "StartAttach");
    ASSERT_EQ_INT(map->objects[5].object_id, 1);
    ASSERT_STR_EQ(map->objects[5].object_type, "Unit");
    ASSERT_EQ_FLOAT(map->objects[5].pathing_soft_radius, 1.5f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[5].pathing_hard_radius, 0.75f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[5].position.x, 2.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[5].angle, 0.5f, 0.001f);
    ASSERT_EQ_INT(map->objects[5].section, 9);
    ASSERT_EQ_INT(map->objects[5].color.r, 200);
    ASSERT_EQ_INT(map->objects[5].color.g, 180);
    ASSERT_EQ_INT(map->objects[5].color.b, 160);
    ASSERT_EQ_INT(map->objects[5].color.a, 255);

    ASSERT_STR_EQ(map->objects[6].name, "SupplyDepot");
    ASSERT_EQ_INT(map->objects[6].id, 6);
    ASSERT_EQ_INT(map->objects[6].type, SC2_OBJECT_UNIT);
    ASSERT_EQ_FLOAT(map->objects[6].position.x, 6.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->objects[6].position.y, 1.0f, 0.001f);
    ASSERT_EQ_INT(map->objects[6].player, 2);

    ASSERT_STR_EQ(map->t3Terrain.tile_set, "Fixture");
    ASSERT_EQ_FLOAT(map->t3Terrain.height_quantize_bias, 0.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->t3Terrain.height_quantize_scale, 1.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->t3Terrain.standard_height, 0.0f, 0.001f);
    ASSERT_EQ_INT(map->t3Terrain.fog_enabled, true);
    ASSERT_EQ_FLOAT(map->t3Terrain.fog_density, 0.25f, 0.001f);
    ASSERT_EQ_FLOAT(map->t3Terrain.fog_falloff, 0.5f, 0.001f);
    ASSERT_EQ_FLOAT(map->t3Terrain.fog_start_height, -1.5f, 0.001f);
    ASSERT_EQ_INT(map->t3Terrain.fog_color.a, 255);
    ASSERT_EQ_INT(map->t3Terrain.fog_color.r, 10);
    ASSERT_EQ_INT(map->t3Terrain.fog_color.g, 20);
    ASSERT_EQ_INT(map->t3Terrain.fog_color.b, 30);
    ASSERT_EQ_INT(map->t3Terrain.num_terrain_textures, 2);
    ASSERT_STR_EQ(map->t3Terrain.terrain_textures[1].diffuse, "Assets\\Textures\\Terrain\\FixtureDirt_Diffuse.dds");

    ASSERT_EQ_INT(map->t3Terrain.num_cliff_sets, 1);
    ASSERT_STR_EQ(map->t3Terrain.cliff_sets[0].name, "FixtureCliff0");
    ASSERT_EQ_INT(map->t3Terrain.num_cliff_cells, 2);
    ASSERT_EQ_INT(map->t3Terrain.cliff_cells[0].index, 0);
    ASSERT_EQ_INT(map->t3Terrain.cliff_cells[0].flags, 1);
    ASSERT_EQ_INT(map->t3Terrain.cliff_cells[0].cliff_set, 0);
    ASSERT_EQ_INT(map->t3Terrain.cliff_cells[0].variant, 2);
    ASSERT_EQ_INT(map->t3Terrain.cliff_cells[1].index, 1);
    ASSERT_EQ_INT(map->t3Terrain.cliff_cells[1].flags, 3);

    ASSERT_EQ_INT(map->lighting.enabled, true);
    ASSERT_STR_EQ(map->lighting.id, "Fixture");
    ASSERT_EQ_FLOAT(map->lighting.ambient_color.x, 0.1f, 0.001f);
    ASSERT_EQ_FLOAT(map->lighting.ambient_color.y, 0.2f, 0.001f);
    ASSERT_EQ_FLOAT(map->lighting.ambient_color.z, 0.3f, 0.001f);
    ASSERT_EQ_INT(map->lighting.directional[SC2_LIGHT_KEY].enabled, true);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_KEY].color.x, 0.4f, 0.001f);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_KEY].color.y, 0.5f, 0.001f);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_KEY].color.z, 0.6f, 0.001f);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_KEY].color_multiplier, 2.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_KEY].spec_color_multiplier, 3.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_KEY].direction.z, -1.0f, 0.001f);
    ASSERT_EQ_INT(map->lighting.directional[SC2_LIGHT_FILL].enabled, true);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_FILL].color_multiplier, 4.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_FILL].direction.x, 1.0f, 0.001f);
    ASSERT_EQ_INT(map->lighting.directional[SC2_LIGHT_BACK].enabled, true);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_BACK].color_multiplier, 5.0f, 0.001f);
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_BACK].direction.y, 1.0f, 0.001f);
}

static void test_sc2_map_loads_binary_terrain_layers(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_fs_host();
    ASSERT(SC2_MapLoad("Maps\\Test\\Tiny.SC2Map"));
    map = SC2_MapCurrent();

    ASSERT_NOT_NULL(map->t3CellFlags);
    if (map->t3CellFlags) {
        ASSERT_EQ_INT(map->t3CellFlags->fourcc, MAKEFOURCC('L','F','C','T'));
        ASSERT_EQ_INT(map->t3CellFlags->version, 101);
        ASSERT_EQ_INT(map->t3CellFlags->width, 8);
        ASSERT_EQ_INT(map->t3CellFlags->height, 6);
        ASSERT_EQ_INT(map->t3CellFlags->data[10], 0x1a);
        ASSERT_EQ_INT(map->t3CellFlags->data[29], 0x2d);
    }

    ASSERT_NOT_NULL(map->t3SyncCliffLevel);
    if (map->t3SyncCliffLevel) {
        ASSERT_EQ_INT(map->t3SyncCliffLevel->fourcc, MAKEFOURCC('C','L','I','F'));
        ASSERT_EQ_INT(map->t3SyncCliffLevel->version, 100);
        ASSERT_EQ_INT(map->t3SyncCliffLevel->width, 8);
        ASSERT_EQ_INT(map->t3SyncCliffLevel->height, 6);
        ASSERT_EQ_INT(map->t3SyncCliffLevel->data[10], 11);
        ASSERT_EQ_INT(map->t3SyncCliffLevel->data[29], 30);
    }

    ASSERT_NOT_NULL(map->t3HeightMap);
    if (map->t3HeightMap) {
        ASSERT_EQ_INT(map->t3HeightMap->fourcc, MAKEFOURCC('H','M','A','P'));
        ASSERT_EQ_INT(map->t3HeightMap->version, 101);
        ASSERT_EQ_INT(map->t3HeightMap->width, 9);
        ASSERT_EQ_INT(map->t3HeightMap->height, 7);
        ASSERT_EQ_INT(map->t3HeightMap->data[0].adjustment, 0);
        ASSERT_EQ_INT(map->t3HeightMap->data[0].height, 1);
        ASSERT_EQ_INT(map->t3HeightMap->data[0].extra, 0);
        ASSERT_EQ_INT(map->t3HeightMap->data[42].adjustment, 0);
        ASSERT_EQ_INT(map->t3HeightMap->data[42].height, 13);
        ASSERT_EQ_INT(map->t3HeightMap->data[42].extra, 0);
        ASSERT_EQ_FLOAT(SC2_MapHeightAtPoint(0.0f, 0.0f), 0.0f, 0.001f);
        ASSERT_EQ_FLOAT(SC2_MapHeightAtPoint(6.0f, 4.0f), 12.0f, 0.001f);
        ASSERT_EQ_FLOAT(SC2_MapHeightAtPoint(map->objects[1].position.x,
                                             map->objects[1].position.y),
                        10.0f,
                        0.001f);
        ASSERT_EQ_FLOAT(SC2_MapHeightAtPoint(map->objects[1].position.x,
                                             map->objects[1].position.y) + map->objects[1].position.z,
                        10.25f,
                        0.001f);
    }
    ASSERT_NOT_NULL(map->t3SyncHeightMap);
    if (map->t3SyncHeightMap) {
        ASSERT_EQ_INT(map->t3SyncHeightMap->fourcc, MAKEFOURCC('S','M','A','P'));
        ASSERT_EQ_INT(map->t3SyncHeightMap->version, 102);
        ASSERT_EQ_INT(map->t3SyncHeightMap->width, 9);
        ASSERT_EQ_INT(map->t3SyncHeightMap->height, 7);
        ASSERT_EQ_INT(map->t3SyncHeightMap->data[42].height, 128);
    }

    ASSERT_NOT_NULL(map->t3TextureMasks);
    if (map->t3TextureMasks) {
        ASSERT_EQ_INT(map->t3TextureMasks->fourcc, MAKEFOURCC('M','A','S','K'));
        ASSERT_EQ_INT(map->t3TextureMasks->version, 102);
        ASSERT_EQ_INT(map->t3TextureMasks->width, 4);
        ASSERT_EQ_INT(map->t3TextureMasks->height, 4);
        ASSERT_EQ_INT(map->t3TextureMasksSize, 80);
        ASSERT_EQ_INT(map->t3TextureMasks->data[0], 0x12);
        ASSERT_EQ_INT(map->t3TextureMasks->data[8], 0xab);
    }
}

static void test_sc2_map_loads_directory_fixture_without_generated_layers(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_disk_host();
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();

    ASSERT_STR_EQ(map->map_name, "SC2 Tiny Fixture");
    ASSERT_EQ_INT(map->MapInfo.fourcc, MAKEFOURCC('M','a','p','I'));
    ASSERT_EQ_INT(map->MapInfo.width, 8);
    ASSERT_EQ_INT(map->MapInfo.height, 6);
    ASSERT_EQ_INT(map->num_objects, 7);
    assert_tiny_map_catalog_overrides(map);

    ASSERT_STR_EQ(map->objects[0].name, "StartGame02");
    ASSERT_EQ_INT(map->objects[0].type, SC2_OBJECT_CAMERA);
    ASSERT_EQ_FLOAT(map->objects[0].camera.distance, 34.0f, 0.001f);
    ASSERT_STR_EQ(map->objects[1].name, "Marine");
    ASSERT_EQ_INT(map->objects[1].type, SC2_OBJECT_UNIT);
    ASSERT_EQ_FLOAT(map->objects[1].position.x, 3.5f, 0.001f);
    ASSERT_EQ_INT(map->objects[1].player, 1);
    ASSERT_STR_EQ(map->objects[4].name, "MineralField");
    ASSERT_EQ_INT(map->objects[4].type, SC2_OBJECT_DOODAD);
    ASSERT_STR_EQ(map->objects[4].model, "Assets\\Doodads\\Terran\\MineralField\\MineralField.m3");
    ASSERT_STR_EQ(map->objects[6].name, "SupplyDepot");
    ASSERT_STR_EQ(map->objects[6].mover, "None");
    ASSERT_EQ_FLOAT(map->objects[6].footprint_radius, 1.4143f, 0.001f);

    ASSERT_STR_EQ(map->t3Terrain.tile_set, "Fixture");
    ASSERT_EQ_FLOAT(map->t3Terrain.height_quantize_scale, 1.0f, 0.001f);
    ASSERT_EQ_INT(map->t3Terrain.num_terrain_textures, 2);
    ASSERT_EQ_INT(map->t3Terrain.num_cliff_sets, 1);
    ASSERT_STR_EQ(map->t3Terrain.cliff_sets[0].name, "FixtureCliff0");
    ASSERT_EQ_INT(map->t3Terrain.num_cliff_cells, 2);
    ASSERT_EQ_INT(map->t3Terrain.cliff_cells[0].variant, 2);
    ASSERT_EQ_INT(map->lighting.enabled, true);
    ASSERT_STR_EQ(map->lighting.id, "Fixture");
    ASSERT_EQ_FLOAT(map->lighting.directional[SC2_LIGHT_KEY].color_multiplier, 2.0f, 0.001f);

    ASSERT_NULL(map->t3CellFlags);
    ASSERT_NULL(map->t3SyncCliffLevel);
    ASSERT_NULL(map->t3HeightMap);
    ASSERT_NULL(map->t3SyncHeightMap);
    ASSERT_NULL(map->t3TextureMasks);
    ASSERT_EQ_INT(map->t3TextureMasksSize, 0);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_catalog_known_files_fallback_without_manifest(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_no_manifest_disk_host();
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();

    ASSERT_STR_EQ(map->map_name, "SC2 Tiny Fixture");
    ASSERT_EQ_INT(map->num_objects, 7);
    assert_tiny_map_known_file_catalog_fallback(map);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void assert_tiny_map_loaded_without_binary_terrain_layers(sc2Map_t *map) {
    ASSERT_STR_EQ(map->map_name, "SC2 Tiny Fixture");
    ASSERT_EQ_INT(map->MapInfo.width, 8);
    ASSERT_EQ_INT(map->MapInfo.height, 6);
    ASSERT_EQ_INT(map->num_objects, 7);
    ASSERT_STR_EQ(map->objects[1].name, "Marine");
    ASSERT_STR_EQ(map->t3Terrain.tile_set, "Fixture");

    ASSERT_NULL(map->t3HeightMap);
    ASSERT_NULL(map->t3SyncHeightMap);
    ASSERT_NULL(map->t3CellFlags);
    ASSERT_NULL(map->t3SyncCliffLevel);
    ASSERT_NULL(map->t3TextureMasks);
    ASSERT_EQ_INT(map->t3TextureMasksSize, 0);
}

static void test_sc2_map_rejects_short_binary_terrain_layers(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_short_terrain_host(TEST_SC2_SHORT_TERRAIN_DIMENSIONS);
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();

    assert_tiny_map_loaded_without_binary_terrain_layers(map);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_rejects_zero_dimension_binary_terrain_layers(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_short_terrain_host(TEST_SC2_ZERO_TERRAIN_DIMENSIONS);
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();

    assert_tiny_map_loaded_without_binary_terrain_layers(map);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_rejects_huge_dimension_binary_terrain_layers(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_short_terrain_host(TEST_SC2_HUGE_TERRAIN_DIMENSIONS);
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();

    assert_tiny_map_loaded_without_binary_terrain_layers(map);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static sc2MapTextureMasks_t *build_texture_masks(DWORD width, DWORD height, BYTE const *payload,
                                                  DWORD payload_size, LPDWORD out_total_size) {
    DWORD total = sizeof(sc2MapTextureMasks_t) + payload_size;
    sc2MapTextureMasks_t *masks = MemAlloc(total);

    memset(masks, 0, total);
    masks->fourcc = MAKEFOURCC('M','A','S','K');
    masks->version = 102;
    masks->width = width;
    masks->height = height;
    if (payload && payload_size)
        memcpy(masks->data, payload, payload_size);
    if (out_total_size) *out_total_size = total;
    return masks;
}

typedef enum {
    TEST_MASK_ODD_VALID,
    TEST_MASK_ODD_TRAILING_BYTE
} testMaskPayloadMode_t;

static testMaskPayloadMode_t test_mask_payload_mode;

static HANDLE read_test_odd_mask_file(LPCSTR filename, LPDWORD size) {
    static BYTE const payload[] = {
        0x12, 0x34, 0x56, 0x78, 0x90,
        0xAB, 0xCD, 0xEF, 0x12, 0x30,
        0xFF
    };
    DWORD payload_size = test_mask_payload_mode == TEST_MASK_ODD_VALID ? 10 : 11;

    if (test_path_leaf_is(filename, "t3TextureMasks"))
        return build_texture_masks(3, 3, payload, payload_size, size);
    return read_test_disk_file(filename, size);
}

static void use_sc2_odd_mask_host(testMaskPayloadMode_t mode) {
    test_mask_payload_mode = mode;
    SC2_MapSetHost(&(sc2MapHost_t){
        .read_file = read_test_odd_mask_file,
        .free_file = MemFree,
        .mem_alloc = MemAlloc,
        .mem_free = MemFree,
    });
}

static HANDLE read_test_bad_height_magic_file(LPCSTR filename, LPDWORD size) {
    LPDWORD data = FS_ReadFile(filename, size);

    if (data && test_path_leaf_is(filename, "t3HeightMap"))
        *data = MAKEFOURCC('B','A','D','!');
    return data;
}

static void use_sc2_bad_height_magic_host(void) {
    SC2_MapSetHost(&(sc2MapHost_t){
        .read_file = read_test_bad_height_magic_file,
        .free_file = FS_FreeFile,
        .mem_alloc = MemAlloc,
        .mem_free = MemFree,
    });
}

static HANDLE read_test_bad_height_version_file(LPCSTR filename, LPDWORD size) {
    LPDWORD data = FS_ReadFile(filename, size);

    if (data && test_path_leaf_is(filename, "t3HeightMap"))
        data[1] = 100;
    return data;
}

static void use_sc2_bad_height_version_host(void) {
    SC2_MapSetHost(&(sc2MapHost_t){
        .read_file = read_test_bad_height_version_file,
        .free_file = FS_FreeFile,
        .mem_alloc = MemAlloc,
        .mem_free = MemFree,
    });
}

static void test_sc2_mask_decode_packed_real_fixture(void) {
    sc2Map_t *map;
    BYTE values[16];

    setup_sc2_tests();
    use_sc2_fs_host();
    ASSERT(SC2_MapLoad("Maps\\Test\\Tiny.SC2Map"));
    map = SC2_MapCurrent();

    ASSERT_EQ_INT(sc2_map_mask_layer_stride(map), 8);  /* (4*4)/2, packed - too small for a 64x64 block */
    ASSERT_EQ_INT(sc2_map_mask_layer_count(map), 2);

    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(map, 0, values);
    ASSERT_EQ_INT(values[0], 0x12 >> 4);
    ASSERT_EQ_INT(values[1], 0x12 & 0x0F);

    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(map, 1, values);
    ASSERT_EQ_INT(values[0], 0xab >> 4);
    ASSERT_EQ_INT(values[1], 0xab & 0x0F);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_mask_decode_packed_synthetic_two_layers(void) {
    sc2Map_t map;
    sc2MapTextureMasks_t *masks;
    BYTE payload[16] = {
        0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF1, 0x23, /* layer 0: 16 nibbles for a 4x4 grid */
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, /* layer 1 */
    };
    DWORD total_size;
    BYTE values[16];

    masks = build_texture_masks(4, 4, payload, sizeof(payload), &total_size);
    memset(&map, 0, sizeof(map));
    map.t3TextureMasks = masks;
    map.t3TextureMasksSize = total_size;
    map.t3Terrain.num_terrain_textures = 2;

    ASSERT_EQ_INT(sc2_map_mask_layer_stride(&map), 8);
    ASSERT_EQ_INT(sc2_map_mask_layer_count(&map), 2);

    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(&map, 0, values);
    ASSERT_EQ_INT(values[0], 0x3);
    ASSERT_EQ_INT(values[1], 0x4);
    ASSERT_EQ_INT(values[15], 0x3);

    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(&map, 1, values);
    ASSERT_EQ_INT(values[0], 0x1);
    ASSERT_EQ_INT(values[1], 0x1);
    ASSERT_EQ_INT(values[15], 0x8);

    MemFree(masks);
}

static void test_sc2_mask_decode_block_layout_with_clipping(void) {
    sc2Map_t map;
    sc2MapTextureMasks_t *masks;
    BYTE payload[8192];
    DWORD total_size;
    BYTE values[65 * 65];

    /* width/height=65 needs a 2x2 grid of 64x64 blocks (block_layer_size=2*2*64*32=8192), which
     * exceeds the flat packed-layer size ((65*65)/2=2112), so this exercises the block-decode
     * path rather than the flat one. */
    memset(payload, 0, sizeof(payload));
    payload[0] = 0x9A;               /* block 0 (bx=0,by=0): pixel(0,0)=0x9, pixel(1,0)=0xA */
    payload[2048 + 0] = 0x5C;         /* block 1 (bx=1,by=0): pixel(64,0)=0x5, pixel(65,0)=0xC (clipped) */
    masks = build_texture_masks(65, 65, payload, sizeof(payload), &total_size);
    memset(&map, 0, sizeof(map));
    map.t3TextureMasks = masks;
    map.t3TextureMasksSize = total_size;
    map.t3Terrain.num_terrain_textures = 1;

    ASSERT_EQ_INT(sc2_map_mask_layer_stride(&map), sizeof(payload));
    ASSERT_EQ_INT(sc2_map_mask_layer_count(&map), 1);

    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(&map, 0, values);
    ASSERT_EQ_INT(values[0], 0x9);  /* pixel(0,0) from block 0, high nibble of 0x9A */
    ASSERT_EQ_INT(values[1], 0xA);  /* pixel(1,0) from block 0, low nibble of 0x9A */
    ASSERT_EQ_INT(values[64], 0x5); /* pixel(64,0): last in-bounds column, from block 1 (high nibble of 0x5C) */
    /* Flat index 65 == 0 + 1*65 is pixel(0,1). Block 1's local (x=1,y=0) virtual pixel is (65,0),
     * which is out of bounds (width=65) and must be clipped rather than aliasing into this same
     * flat index with its low nibble (0xC). The legitimate value here comes from block 0's
     * (x=0,y=1) byte, which was left at 0 in the payload, so a clipping regression would flip
     * this from 0 to 0xC. */
    ASSERT_EQ_INT(values[65], 0x0);

    MemFree(masks);
}

static void test_sc2_mask_decode_odd_dimensions_two_layers(void) {
    sc2Map_t map;
    sc2MapTextureMasks_t *masks;
    BYTE payload[10] = {
        0x12, 0x34, 0x56, 0x78, 0x90, /* 3x3 = 9 nibbles, each layer needs 5 bytes */
        0xAB, 0xCD, 0xEF, 0x12, 0x30
    };
    DWORD total_size;
    BYTE values[9];

    masks = build_texture_masks(3, 3, payload, sizeof(payload), &total_size);
    memset(&map, 0, sizeof(map));
    map.t3TextureMasks = masks;
    map.t3TextureMasksSize = total_size;
    map.t3Terrain.num_terrain_textures = 2;

    ASSERT_EQ_INT(sc2_map_mask_layer_stride(&map), 5);
    ASSERT_EQ_INT(sc2_map_mask_layer_count(&map), 2);
    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(&map, 0, values);
    ASSERT_EQ_INT(values[0], 0x1);
    ASSERT_EQ_INT(values[1], 0x2);
    ASSERT_EQ_INT(values[7], 0x8);
    ASSERT_EQ_INT(values[8], 0x9);
    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(&map, 1, values);
    ASSERT_EQ_INT(values[0], 0xA);
    ASSERT_EQ_INT(values[1], 0xB);
    ASSERT_EQ_INT(values[7], 0x2);
    ASSERT_EQ_INT(values[8], 0x3);

    MemFree(masks);
}

static void test_sc2_mask_packed_layers_do_not_alias_block_stride(void) {
    sc2Map_t map;
    sc2MapTextureMasks_t *masks;
    BYTE payload[4096];
    DWORD total_size;
    BYTE values[65 * 63];

    /* Two packed 65x63 layers total 4096 bytes, exactly one padded block layer. The texture list
     * proves there are two layers, so divisibility alone must not select the block layout. */
    memset(payload, 0, sizeof(payload));
    payload[0] = 0x12;
    payload[2048] = 0xAB;
    masks = build_texture_masks(65, 63, payload, sizeof(payload), &total_size);
    memset(&map, 0, sizeof(map));
    map.t3TextureMasks = masks;
    map.t3TextureMasksSize = total_size;
    map.t3Terrain.num_terrain_textures = 2;

    ASSERT_EQ_INT(sc2_map_mask_layer_stride(&map), 2048);
    ASSERT_EQ_INT(sc2_map_mask_layer_count(&map), 2);
    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(&map, 1, values);
    ASSERT_EQ_INT(values[0], 0xA);
    ASSERT_EQ_INT(values[1], 0xB);

    MemFree(masks);
}

static void test_sc2_mask_rejects_partial_trailing_layer(void) {
    sc2Map_t map;
    sc2MapTextureMasks_t *masks;
    BYTE payload[6] = { 0x12, 0x34, 0x56, 0x78, 0x90, 0xAB };
    DWORD total_size;

    masks = build_texture_masks(3, 3, payload, sizeof(payload), &total_size);
    memset(&map, 0, sizeof(map));
    map.t3TextureMasks = masks;
    map.t3TextureMasksSize = total_size;
    map.t3Terrain.num_terrain_textures = 1;
    ASSERT_EQ_INT(sc2_map_mask_layer_stride(&map), 0);
    ASSERT_EQ_INT(sc2_map_mask_layer_count(&map), 0);
    MemFree(masks);
}

static void test_sc2_mask_decode_layer_out_of_bounds_is_noop(void) {
    sc2Map_t map;
    sc2MapTextureMasks_t *masks;
    BYTE payload[8] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 };
    DWORD total_size;
    BYTE values[16];

    masks = build_texture_masks(4, 4, payload, sizeof(payload), &total_size);
    memset(&map, 0, sizeof(map));
    map.t3TextureMasks = masks;
    map.t3TextureMasksSize = total_size;
    map.t3Terrain.num_terrain_textures = 1;
    ASSERT_EQ_INT(sc2_map_mask_layer_count(&map), 1);

    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(&map, 5, values); /* only 1 layer present; index 5 is out of bounds */
    FOR_LOOP(i, sizeof(values)) {
        ASSERT_EQ_INT(values[i], 0xEE);
    }

    /* NULL/empty t3TextureMasks must also be a no-op, never a crash. */
    memset(&map, 0, sizeof(map));
    memset(values, 0xEE, sizeof(values));
    sc2_map_mask_decode_layer(&map, 0, values);
    ASSERT_EQ_INT(values[0], 0xEE);

    MemFree(masks);
}

static void test_sc2_map_generation_increments_monotonically(void) {
    DWORD gen0, gen1, gen2, gen3;

    setup_sc2_tests();
    use_sc2_fs_host();
    gen0 = SC2_MapCurrent()->generation;

    ASSERT(SC2_MapLoad("Maps\\Test\\Tiny.SC2Map"));
    gen1 = SC2_MapCurrent()->generation;
    ASSERT(gen1 > gen0);

    ASSERT(SC2_MapLoad("Maps\\Test\\Tiny.SC2Map"));
    gen2 = SC2_MapCurrent()->generation;
    ASSERT(gen2 > gen1);

    /* sc2_source_open() always succeeds (falls back to treating mapFilename as a bare base path
     * when it isn't an openable archive), so SC2_MapLoad() returns true even for a nonexistent
     * map — it just yields an empty map (objects=0). This is pre-existing behavior; the point
     * here is only that generation still advances on this load attempt. */
    ASSERT(SC2_MapLoad("Maps\\Test\\DoesNotExist.SC2Map"));
    gen3 = SC2_MapCurrent()->generation;
    ASSERT(gen3 > gen2);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_terrain_layer_status_ok_for_present_layers(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_fs_host();
    ASSERT(SC2_MapLoad("Maps\\Test\\Tiny.SC2Map"));
    map = SC2_MapCurrent();

    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_HEIGHT_MAP], SC2_LAYER_STATUS_OK);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_HEIGHT_MAP], SC2_LAYER_STATUS_OK);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_CELL_FLAGS], SC2_LAYER_STATUS_OK);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_CLIFF_LEVEL], SC2_LAYER_STATUS_OK);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_TEXTURE_MASKS], SC2_LAYER_STATUS_OK);
    /* This fixture has no PATH/WATR/DLFT/HRDT files at all. */
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_PATHING_INFO], SC2_LAYER_STATUS_ABSENT);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_WATER], SC2_LAYER_STATUS_ABSENT);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_FLUFF_DOODAD], SC2_LAYER_STATUS_ABSENT);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_HARD_TILE], SC2_LAYER_STATUS_ABSENT);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_terrain_layer_status_absent_without_binary_layers(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_disk_host();
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();

    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_HEIGHT_MAP], SC2_LAYER_STATUS_ABSENT);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_HEIGHT_MAP], SC2_LAYER_STATUS_ABSENT);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_CELL_FLAGS], SC2_LAYER_STATUS_ABSENT);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_CLIFF_LEVEL], SC2_LAYER_STATUS_ABSENT);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_TEXTURE_MASKS], SC2_LAYER_STATUS_ABSENT);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_terrain_layer_status_malformed_for_short_layers(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_short_terrain_host(TEST_SC2_SHORT_TERRAIN_DIMENSIONS);
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();

    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_HEIGHT_MAP], SC2_LAYER_STATUS_MALFORMED);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_HEIGHT_MAP], SC2_LAYER_STATUS_MALFORMED);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_CELL_FLAGS], SC2_LAYER_STATUS_MALFORMED);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_CLIFF_LEVEL], SC2_LAYER_STATUS_MALFORMED);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_TEXTURE_MASKS], SC2_LAYER_STATUS_MALFORMED);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_terrain_layer_status_malformed_for_bad_magic(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_bad_height_magic_host();
    ASSERT(SC2_MapLoad("Maps\\Test\\Tiny.SC2Map"));
    map = SC2_MapCurrent();
    ASSERT_NULL(map->t3HeightMap);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_HEIGHT_MAP], SC2_LAYER_STATUS_MALFORMED);
    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_terrain_layer_status_malformed_for_bad_version(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_bad_height_version_host();
    ASSERT(SC2_MapLoad("Maps\\Test\\Tiny.SC2Map"));
    map = SC2_MapCurrent();
    ASSERT_NULL(map->t3HeightMap);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_HEIGHT_MAP], SC2_LAYER_STATUS_MALFORMED);
    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_accepts_odd_multilayer_mask(void) {
    sc2Map_t *map;
    BYTE values[9];

    setup_sc2_tests();
    use_sc2_odd_mask_host(TEST_MASK_ODD_VALID);
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_TEXTURE_MASKS], SC2_LAYER_STATUS_OK);
    ASSERT_EQ_INT(sc2_map_mask_layer_count(map), 2);
    sc2_map_mask_decode_layer(map, 1, values);
    ASSERT_EQ_INT(values[0], 0xA);
    ASSERT_EQ_INT(values[8], 0x3);
    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_rejects_partial_mask_layer(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_odd_mask_host(TEST_MASK_ODD_TRAILING_BYTE);
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();
    ASSERT_NULL(map->t3TextureMasks);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_TEXTURE_MASKS], SC2_LAYER_STATUS_MALFORMED);
    SC2_MapShutdown();
    use_sc2_fs_host();
}

static void test_sc2_map_detects_unsupported_embedded_terrain_files(void) {
    sc2Map_t *map;

    setup_sc2_tests();
    use_sc2_unsupported_terrain_host();
    ASSERT(SC2_MapLoad(TEST_SC2_TINY_DIR));
    map = SC2_MapCurrent();

    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_PATHING_INFO], SC2_LAYER_STATUS_UNSUPPORTED);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_WATER], SC2_LAYER_STATUS_UNSUPPORTED);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_FLUFF_DOODAD], SC2_LAYER_STATUS_UNSUPPORTED);
    ASSERT_EQ_INT(map->terrain_layer_status[SC2_TERRAIN_LAYER_HARD_TILE], SC2_LAYER_STATUS_UNSUPPORTED);
    /* Detecting these files must not affect unrelated, already-parsed layers. */
    ASSERT_STR_EQ(map->map_name, "SC2 Tiny Fixture");
    ASSERT_EQ_INT(map->num_objects, 7);

    SC2_MapShutdown();
    use_sc2_fs_host();
}

void run_sc2_map_tests(void) {
    RUN_TEST(test_sc2_fixture_archive_lists_map_root);
    RUN_TEST(test_sc2_fixture_short_name_resolves);
    RUN_TEST(test_sc2_map_loads_xml_objects_and_terrain);
    RUN_TEST(test_sc2_map_loads_binary_terrain_layers);
    RUN_TEST(test_sc2_map_loads_directory_fixture_without_generated_layers);
    RUN_TEST(test_sc2_map_catalog_known_files_fallback_without_manifest);
    RUN_TEST(test_sc2_map_rejects_short_binary_terrain_layers);
    RUN_TEST(test_sc2_map_rejects_zero_dimension_binary_terrain_layers);
    RUN_TEST(test_sc2_map_rejects_huge_dimension_binary_terrain_layers);
    RUN_TEST(test_sc2_mask_decode_packed_real_fixture);
    RUN_TEST(test_sc2_mask_decode_packed_synthetic_two_layers);
    RUN_TEST(test_sc2_mask_decode_block_layout_with_clipping);
    RUN_TEST(test_sc2_mask_decode_odd_dimensions_two_layers);
    RUN_TEST(test_sc2_mask_packed_layers_do_not_alias_block_stride);
    RUN_TEST(test_sc2_mask_rejects_partial_trailing_layer);
    RUN_TEST(test_sc2_mask_decode_layer_out_of_bounds_is_noop);
    RUN_TEST(test_sc2_map_generation_increments_monotonically);
    RUN_TEST(test_sc2_map_terrain_layer_status_ok_for_present_layers);
    RUN_TEST(test_sc2_map_terrain_layer_status_absent_without_binary_layers);
    RUN_TEST(test_sc2_map_terrain_layer_status_malformed_for_short_layers);
    RUN_TEST(test_sc2_map_terrain_layer_status_malformed_for_bad_magic);
    RUN_TEST(test_sc2_map_terrain_layer_status_malformed_for_bad_version);
    RUN_TEST(test_sc2_map_accepts_odd_multilayer_mask);
    RUN_TEST(test_sc2_map_rejects_partial_mask_layer);
    RUN_TEST(test_sc2_map_detects_unsupported_embedded_terrain_files);
    SC2_MapShutdown();
    FS_Shutdown();
}
