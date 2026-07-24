#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "games/warcraft-3/game/g_local.h"
#include "platform/bridge/bz_tabletop_transport.h"

#define FOURCC(a,b,c,d) ((DWORD)(a) | (DWORD)(b) << 8 | (DWORD)(c) << 16 | (DWORD)(d) << 24)

struct bzTTSnapshot {
    char configstrings[16][BZ_TT_MAX_CONFIGSTRING_LEN];
};

struct world_state world;
static struct mapInfo_s test_mapinfo[2];
struct level_locals level = { .mapinfo = test_mapinfo };
struct metadataMapSnapshot_s { uint64_t token; unsigned map; };
static struct metadataMapSnapshot_s metadata_maps[2] = { { 1, 0 }, { 2, 1 } };
static const metadataMapSnapshot_t *metadata_map = metadata_maps;
static bool test_tft;
static bool cliff_specific;
static bool cliff_generic = true;
static bool water_available = true;
static atomic_uint water_reads;
static bool team_available = true;
static atomic_uint team_reads;
static pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_cond = PTHREAD_COND_INITIALIZER;
static bool read_blocked;
static unsigned read_waiters;

void test_assets_set_tft(bool enabled) { test_tft = enabled; }
void test_assets_set_cliff_specific(bool enabled) { cliff_specific = enabled; }
void test_assets_set_cliff_generic(bool enabled) { cliff_generic = enabled; }
void test_assets_set_water_available(bool available) {
    water_available = available; atomic_store(&water_reads, 0);
}
unsigned test_assets_water_reads(void) { return atomic_load(&water_reads); }
void test_assets_set_team_available(bool available) {
    team_available = available; atomic_store(&team_reads, 0);
}
unsigned test_assets_team_reads(void) { return atomic_load(&team_reads); }
void test_assets_set_metadata_map(unsigned index) {
    level.mapinfo = index < 2 ? test_mapinfo + index : NULL;
    metadata_map = index < 2 ? metadata_maps + index : NULL;
}

void G_MetadataPublishMap(LPCMAPINFO mapinfo) { (void)mapinfo; }
const metadataMapSnapshot_t *G_MetadataMapAcquire(void) { return metadata_map; }
void G_MetadataMapRelease(const metadataMapSnapshot_t *snapshot) { (void)snapshot; }
uint64_t G_MetadataMapToken(const metadataMapSnapshot_t *snapshot) { return snapshot ? snapshot->token : 0; }
DWORD G_MetadataMapClass(const metadataMapSnapshot_t *snapshot, DWORD class_id) {
    return snapshot && snapshot->map == 1 && class_id == FOURCC('h','p','e','a')
        ? FOURCC('h','p','e','2') : class_id;
}
void test_assets_block_reads(bool blocked) {
    pthread_mutex_lock(&read_lock);
    read_blocked = blocked;
    if (!blocked) pthread_cond_broadcast(&read_cond);
    pthread_mutex_unlock(&read_lock);
}
void test_assets_wait_for_blocked_reads(unsigned count) {
    pthread_mutex_lock(&read_lock);
    while (read_waiters < count) pthread_cond_wait(&read_cond, &read_lock);
    pthread_mutex_unlock(&read_lock);
}

void test_assets_set_configstring(struct bzTTSnapshot *snapshot, uint32_t index, const char *value) {
    if (snapshot && index < 16)
        snprintf(snapshot->configstrings[index], sizeof(snapshot->configstrings[index]), "%s", value);
}

bool BZ_TTSnapshot_ConfigString(const bzTTSnapshot_t *snapshot, uint32_t index, char *out, size_t cap) {
    if (!snapshot || !out || !cap || index >= 16 || !snapshot->configstrings[index][0])
        return false;
    snprintf(out, cap, "%s", snapshot->configstrings[index]);
    return true;
}

sheetMetaData_t UnitsMetaData[1];
sheetMetaData_t DestructableMetaData[1];
sheetMetaData_t DoodadsMetaData[1];
sheetMetaData_t ItemsMetaData[1];
sheetRow_t *Doodads = (sheetRow_t *)(uintptr_t)3;

sheetRow_t *FS_ParseSLK(LPCSTR identity) {
    if (!strcmp(identity, "TerrainArt\\Terrain.slk")) return (sheetRow_t *)(uintptr_t)1;
    if (!strcmp(identity, "TerrainArt\\CliffTypes.slk")) return (sheetRow_t *)(uintptr_t)2;
    return NULL;
}

LPCSTR FS_FindSheetCell(sheetRow_t *sheet, LPCSTR row, LPCSTR column) {
    static const char *no_path_doodads[] = {
        "LPwh", "LOfl", "LOth", "LPrs", "LPlp", "LOtz", "LOsm", "AWfs", "LPcw", "AOsr",
    };
    if (sheet == (sheetRow_t *)(uintptr_t)1 && !strcmp(row, "Ldrt")) {
        if (!strcmp(column, "dir")) return test_tft ? "TerrainArt\\TFT" : "TerrainArt\\ROC";
        if (!strcmp(column, "file")) return "Dirt";
    }
    if (sheet == (sheetRow_t *)(uintptr_t)1 && !strcmp(row, "Lgrs")) {
        if (!strcmp(column, "dir")) return "TerrainArt\\ROC";
        if (!strcmp(column, "file")) return "Grass";
    }
    if (sheet == (sheetRow_t *)(uintptr_t)2 && !strcmp(row, "CLif")) {
        if (!strcmp(column, "texDir")) return "ReplaceableTextures\\Cliff";
        if (!strcmp(column, "texFile")) return "Cliff0";
    }
    if (sheet == Doodads && !strcmp(row, "DOOD")) {
        if (!strcmp(column, "file")) return "DoodadModel";
        if (!strcmp(column, "pathTex")) return "PathTextures\\2x3Doodad.tga";
        if (!strcmp(column, "vertR01")) return "128";
        if (!strcmp(column, "vertG01")) return "192";
        if (!strcmp(column, "vertB01")) return "255";
    }
    if (sheet == Doodads)
        for (size_t i = 0; i < sizeof(no_path_doodads) / sizeof(*no_path_doodads); i++)
            if (!strcmp(row, no_path_doodads[i])) {
                if (!strcmp(column, "file")) return i == 9 ? "ScorchedRemains" : "DoodadModel";
                if (!strcmp(column, "pathTex")) return "none";
                if (!strcmp(column, "vertR01") || !strcmp(column, "vertG01") ||
                    !strcmp(column, "vertB01")) return "255";
            }
    if (sheet == Doodads && !strcmp(row, "Dmis")) {
        if (!strcmp(column, "file")) return "MissingPathDoodad";
        if (!strcmp(column, "pathTex")) return "PathTextures\\missing.tga";
    }
    return NULL;
}

bool FS_FileExists(LPCSTR identity) {
    if (!strcmp(identity, "ReplaceableTextures\\Cliff\\L_Cliff0.blp")) return cliff_specific;
    return cliff_generic && !strcmp(identity, "ReplaceableTextures\\Cliff\\Cliff0.blp");
}

static LPCSTR unit_field(DWORD class_id, LPCSTR name) {
    if (class_id == FOURCC('h','p','e','a') || class_id == FOURCC('h','p','e','2')) {
        if (!strcmp(name, "umdl")) return "Units\\Human\\Peasant.mdx";
        if (!strcmp(name, "ubdg")) return "0";
        if (!strcmp(name, "ucol")) return class_id == FOURCC('h','p','e','2') ? "24" : "16";
        if (!strcmp(name, "uclr") || !strcmp(name, "uclg") || !strcmp(name, "uclb")) return "255";
    }
    if (class_id == FOURCC('h','t','o','w')) {
        if (!strcmp(name, "umdl")) return "Units\\Human\\TownHall.mdx";
        if (!strcmp(name, "ubdg") || !strcmp(name, "utcc")) return "1";
        if (!strcmp(name, "upat")) return "PathTextures\\4x6Building.tga";
        if (!strcmp(name, "utco")) return "3";
        if (!strcmp(name, "uclr")) return "200";
        if (!strcmp(name, "uclg")) return "150";
        if (!strcmp(name, "uclb")) return "100";
    }
    if (class_id == FOURCC('n','g','o','l')) {
        if (!strcmp(name, "umdl")) return "Units\\Creeps\\GoldMine.mdx";
        if (!strcmp(name, "uabt")) return "resource";
        if (!strcmp(name, "upat")) return "PathTextures\\16x16Goldmine.tga";
    }
    if (class_id == FOURCC('h','f','o','o') && !strcmp(name, "umdl"))
        return "Units\\Human\\Footman.mdx";
    return NULL;
}

static LPCSTR destructable_field(DWORD class_id, LPCSTR name) {
    if (class_id == FOURCC('L','T','l','t') || class_id == FOURCC('B','0','0','1') ||
        class_id == FOURCC('B','b','a','d') || class_id == FOURCC('B','m','a','l') ||
        class_id == FOURCC('B','e','s','c')) {
        if (!strcmp(name, "bfil")) return "Doodads\\Terrain\\Model.mdx";
        if (!strcmp(name, "btar")) return class_id == FOURCC('L','T','l','t') ? "tree" : "wall";
        if (!strcmp(name, "bptx"))
            return class_id == FOURCC('L','T','l','t') ? "PathTextures\\2x2Tree.tga" :
                   class_id == FOURCC('B','0','0','1') ? "PathTextures\\3x4Destructable.tga" :
                   class_id == FOURCC('B','m','a','l') ? "PathTextures\\malformed.tga" :
                   class_id == FOURCC('B','e','s','c') ? "..\\outside.tga" : NULL;
        if (!strcmp(name, "bvcr")) return "220";
        if (!strcmp(name, "bvcg")) return "230";
        if (!strcmp(name, "bvcb")) return "240";
    }
    return NULL;
}

LPCSTR UnitStringField(sheetMetaData_t *metadata, DWORD class_id, LPCSTR name) {
    return metadata == DestructableMetaData ? destructable_field(class_id, name) :
           metadata == UnitsMetaData ? unit_field(class_id, name) : NULL;
}

LPCSTR UnitStringFieldBase(sheetMetaData_t *metadata, DWORD class_id, LPCSTR name) {
    return UnitStringField(metadata, class_id, name);
}

LONG UnitIntegerField(sheetMetaData_t *metadata, DWORD class_id, LPCSTR name) {
    LPCSTR value = UnitStringField(metadata, class_id, name);
    return value ? strtol(value, NULL, 10) : 0;
}
LONG UnitIntegerFieldBase(sheetMetaData_t *metadata, DWORD class_id, LPCSTR name) {
    return UnitIntegerField(metadata, class_id, name);
}

BOOL UnitBooleanField(sheetMetaData_t *metadata, DWORD class_id, LPCSTR name) {
    LPCSTR value = UnitStringField(metadata, class_id, name);
    return value && (!strcmp(value, "TRUE") || atoi(value));
}
BOOL UnitBooleanFieldBase(sheetMetaData_t *metadata, DWORD class_id, LPCSTR name) {
    return UnitBooleanField(metadata, class_id, name);
}

FLOAT UnitRealField(sheetMetaData_t *metadata, DWORD class_id, LPCSTR name) {
    LPCSTR value = UnitStringField(metadata, class_id, name);
    return value ? strtof(value, NULL) : 0;
}
FLOAT UnitRealFieldBase(sheetMetaData_t *metadata, DWORD class_id, LPCSTR name) {
    return UnitRealField(metadata, class_id, name);
}

static HANDLE make_tga(uint16_t width, uint16_t height, LPDWORD size) {
    size_t bytes = 18u + (size_t)width * height;
    uint8_t *data = calloc(1, bytes);
    if (!data) return NULL;
    data[2] = 3; data[12] = (uint8_t)width; data[13] = (uint8_t)(width >> 8);
    data[14] = (uint8_t)height; data[15] = (uint8_t)(height >> 8); data[16] = 8;
    *size = (DWORD)bytes;
    return data;
}

HANDLE FS_ReadFile(LPCSTR identity, LPDWORD size) {
    char path[512];
    FILE *file;
    long length;
    void *data;
    unsigned team;
    int end;
    bool team_color, team_glow;
    if (!identity || !size) return NULL;
    pthread_mutex_lock(&read_lock);
    if (read_blocked) {
        read_waiters++;
        pthread_cond_broadcast(&read_cond);
        while (read_blocked) pthread_cond_wait(&read_cond, &read_lock);
        read_waiters--;
    }
    pthread_mutex_unlock(&read_lock);
    if (!strcmp(identity, "PathTextures\\2x3Doodad.tga")) return make_tga(2, 3, size);
    if (!strcmp(identity, "PathTextures\\4x6Building.tga")) return make_tga(4, 6, size);
    if (!strcmp(identity, "PathTextures\\16x16Goldmine.tga")) return make_tga(16, 16, size);
    if (!strcmp(identity, "PathTextures\\2x2Tree.tga")) return make_tga(2, 2, size);
    if (!strcmp(identity, "PathTextures\\3x4Destructable.tga")) return make_tga(3, 4, size);
    if (!strcmp(identity, "PathTextures\\malformed.tga")) {
        *size = 8;
        return calloc(1, *size);
    }
    if (!strcmp(identity, "TerrainArt\\ROC\\Dirt.blp") ||
        !strcmp(identity, "TerrainArt\\ROC\\Grass.blp") ||
        !strcmp(identity, "ReplaceableTextures\\Cliff\\L_Cliff0.blp") ||
        !strcmp(identity, "ReplaceableTextures\\Cliff\\Cliff0.blp"))
        identity = "TestUI/Textures/solid_white.blp";
    else if (!strcmp(identity, "ReplaceableTextures\\Water\\Water12.blp")) {
        atomic_fetch_add(&water_reads, 1);
        if (!water_available) return NULL;
        identity = "TestUI/Textures/solid_white.blp";
    }
    end = 0;
    team_color = sscanf(identity, "ReplaceableTextures\\TeamColor\\TeamColor%2u.blp%n", &team, &end) == 1 &&
                 !identity[end] && team < MAX_PLAYERS;
    end = 0;
    team_glow = sscanf(identity, "ReplaceableTextures\\TeamGlow\\TeamGlow%2u.blp%n", &team, &end) == 1 &&
                !identity[end] && team < MAX_PLAYERS;
    if (team_color || team_glow) {
        atomic_fetch_add(&team_reads, 1);
        if (!team_available) return NULL;
        identity = team_glow ? "TestUI/Textures/orientation_2x2.blp" : "TestUI/Textures/solid_white.blp";
    }
    else if (!strcmp(identity, "TerrainArt\\TFT\\Dirt.blp"))
        identity = "TestUI/Textures/orientation_2x2.blp";
    else if (!strcmp(identity, "Doodads\\LordaeronSummer\\Plants\\Wheat\\Wheat.mdx") ||
             !strcmp(identity, "Doodads\\Ashenvale\\Plants\\AshenBush0\\AshenBush0.mdx") ||
             !strcmp(identity, "Doodads\\LordaeronSummer\\Props\\Cage\\Cage.mdx") ||
             !strcmp(identity, "Doodads\\LordaeronSummer\\Props\\TorchHuman\\TorchHuman.mdx") ||
             !strcmp(identity, "TestUI/Models/quad_sprite.mdx"))
        identity = "TestUI/Models/quad_sprite.mdx";
    if (!strcmp(identity, "TestUI/Textures/variant.blp"))
        identity = test_tft ? "TestUI/Textures/orientation_2x2.blp" : "TestUI/Textures/solid_white.blp";
    snprintf(path, sizeof(path), "build/tests/resources/%s", identity);
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file); return NULL;
    }
    data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data); fclose(file); return NULL;
    }
    fclose(file);
    *size = (DWORD)length;
    return data;
}

void FS_FreeFile(void *data) { free(data); }
