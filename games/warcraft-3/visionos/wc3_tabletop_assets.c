#include "wc3_tabletop_assets_internal.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "../common/wc3_asset_path.h"
#include "../game/g_local.h"

#define WC3_TTA_MAX_TERRAIN_CORNERS (1025u * 1025u)
#define WC3_TTA_PATH_CELL_SIZE 32u

extern sheetRow_t *Doodads;

static sheetRow_t *ground_sheet, *cliff_sheet;
static pthread_mutex_t terrain_sheet_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    void *data;
    DWORD size;
} wc3ModelRead_t;

static bool wc3_read_model(const char *identity, void *opaque) {
    wc3ModelRead_t *read = opaque;
    read->data = FS_ReadFile(identity, &read->size);
    if (read->data && !read->size) {
        FS_FreeFile(read->data);
        read->data = NULL;
    }
    return read->data != NULL;
}

static size_t terrain_align_size(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static bzTTAsset_t *wc3_load_asset(const char *identity, bzTTAssetKind_t kind,
                                    const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status) {
    wc3ModelRead_t read = { 0 };
    char resolved[512];
    wc3ModelResolve_t resolve = {
        .identity = identity, .probe = wc3_read_model, .context = &read,
        .out = resolved, .cap = sizeof(resolved),
    };
    bzTTAsset_t *asset;
    if (kind == BZ_TTA_ASSET_MODEL)
        wc3_resolve_model_identity(&resolve);
    else {
        snprintf(resolved, sizeof(resolved), "%s", identity);
        read.data = FS_ReadFile(identity, &read.size);
    }
    if (!read.data || !read.size) {
        if (read.data) FS_FreeFile(read.data);
        *status = BZ_TTA_ERR_NOT_FOUND;
        return NULL;
    }
    asset = kind == BZ_TTA_ASSET_IMAGE
        ? BZ_WC3_TTA_DecodeBLP(read.data, read.size, resolved, metadata, status)
        : BZ_WC3_TTA_DecodeMDX(read.data, read.size, resolved, metadata, status);
    FS_FreeFile(read.data);
    return asset;
}

static void wc3_fourcc_name(uint32_t type_id, char name[5]) {
    memcpy(name, &type_id, 4);
    name[4] = '\0';
}

static bool wc3_path(char *out, size_t cap, const char *format, const char *dir,
                     uint8_t tileset, const char *file) {
    int count = tileset ? snprintf(out, cap, format, dir, tileset, file) :
                          snprintf(out, cap, format, dir, file);
    return count > 0 && (size_t)count < cap && wc3_tta_path_is_confined(out);
}

typedef struct {
    bzTTTeamTextureKind_t kind;
    const char *dir, *file;
} wc3TeamTextureDef_t;

/* Classic archives author one image of each semantic kind for every player color. */
static uint32_t wc3_team_texture_count(bzTTTeamTextureKind_t kind) {
    return kind == BZ_TTA_TEAM_TEXTURE_COLOR || kind == BZ_TTA_TEAM_TEXTURE_GLOW ? MAX_PLAYERS : 0;
}

/* Keep Warcraft path construction behind the provider; callers submit only semantic IDs. */
static bzTTAResult_t wc3_resolve_team_texture_identity(const bzTTTeamTextureResolve_t *resolve) {
    static const wc3TeamTextureDef_t defs[] = {
        { BZ_TTA_TEAM_TEXTURE_COLOR, "ReplaceableTextures\\TeamColor", "TeamColor" },
        { BZ_TTA_TEAM_TEXTURE_GLOW, "ReplaceableTextures\\TeamGlow", "TeamGlow" },
    };
    int count;
    if (!resolve || !resolve->identity || !resolve->cap || resolve->team_color >= MAX_PLAYERS)
        return BZ_TTA_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < sizeof(defs) / sizeof(defs[0]); i++) {
        if (defs[i].kind != resolve->kind) continue;
        count = snprintf(resolve->identity, resolve->cap, "%s\\%s%02u.blp",
                         defs[i].dir, defs[i].file, resolve->team_color);
        return count > 0 && (size_t)count < resolve->cap && wc3_tta_path_is_confined(resolve->identity)
            ? BZ_TTA_OK : BZ_TTA_ERR_PATH_CONFINEMENT;
    }
    return BZ_TTA_ERR_INVALID_ARGUMENT;
}

/* Terrain identity translation stays behind the game callback so Swift never knows Warcraft paths. */
static bzTTAResult_t wc3_resolve_terrain_identity(bzTTTerrainTextureKind_t kind, uint32_t type_id,
                                                  uint8_t tileset, char *identity, size_t cap) {
    char row[5];
    const char *dir, *file;
    wc3_fourcc_name(type_id, row);
    if (!identity || !cap) return BZ_TTA_ERR_INVALID_ARGUMENT;
    if (kind == BZ_TTA_TERRAIN_TEXTURE_WATER) {
        if (type_id) return BZ_TTA_ERR_INVALID_ARGUMENT;
        return wc3_path(identity, cap, "%s\\%s.blp", "ReplaceableTextures\\Water", 0, "Water12")
            ? BZ_TTA_OK : BZ_TTA_ERR_PATH_CONFINEMENT;
    }
    if (kind == BZ_TTA_TERRAIN_TEXTURE_GROUND) {
        pthread_mutex_lock(&terrain_sheet_lock);
        if (!ground_sheet) {
            pthread_mutex_unlock(&terrain_sheet_lock);
            return BZ_TTA_ERR_NOT_INITIALIZED;
        }
        dir = FS_FindSheetCell(ground_sheet, row, "dir");
        file = FS_FindSheetCell(ground_sheet, row, "file");
        pthread_mutex_unlock(&terrain_sheet_lock);
        if (!dir || !*dir || !file || !*file) return BZ_TTA_ERR_NOT_FOUND;
        return wc3_path(identity, cap, "%s\\%s.blp", dir, 0, file)
            ? BZ_TTA_OK : BZ_TTA_ERR_PATH_CONFINEMENT;
    }
    if (kind != BZ_TTA_TERRAIN_TEXTURE_CLIFF) return BZ_TTA_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&terrain_sheet_lock);
    if (!cliff_sheet) {
        pthread_mutex_unlock(&terrain_sheet_lock);
        return BZ_TTA_ERR_NOT_INITIALIZED;
    }
    dir = FS_FindSheetCell(cliff_sheet, row, "texDir");
    file = FS_FindSheetCell(cliff_sheet, row, "texFile");
    pthread_mutex_unlock(&terrain_sheet_lock);
    if (!dir || !*dir || !file || !*file) return BZ_TTA_ERR_NOT_FOUND;
    if (tileset && wc3_path(identity, cap, "%s\\%c_%s.blp", dir, tileset, file) &&
        FS_FileExists(identity))
        return BZ_TTA_OK;
    if (!wc3_path(identity, cap, "%s\\%s.blp", dir, 0, file))
        return BZ_TTA_ERR_PATH_CONFINEMENT;
    return FS_FileExists(identity) ? BZ_TTA_OK : BZ_TTA_ERR_NOT_FOUND;
}

static bool wc3_metadata_path(const char *path) {
    /* Doodads.slk uses "none" for authored non-pathing rows such as LPwh and AOsr. */
    return path && *path && strcmp(path, "_") && strcmp(path, "-") && strcmp(path, "none");
}

static uint16_t wc3_u16(const uint8_t *data) {
    return (uint16_t)data[0] | (uint16_t)data[1] << 8;
}

/* Path textures need only their bounded TGA dimensions; decoding pixels would duplicate game state. */
static bzTTAResult_t wc3_path_footprint(const char *path, bool required, bzTTAssetMetadata_t *metadata) {
    const uint8_t *data;
    DWORD size = 0;
    uint32_t width, height, pixel_bytes;
    size_t offset, pixels;
    if (!wc3_metadata_path(path))
        return required ? BZ_TTA_ERR_NOT_FOUND : BZ_TTA_OK;
    if (!wc3_tta_path_is_confined(path)) return BZ_TTA_ERR_PATH_CONFINEMENT;
    data = FS_ReadFile(path, &size);
    if (!data) return BZ_TTA_ERR_NOT_FOUND;
    if (size < 18 || (data[2] != 2 && data[2] != 3) || data[1] ||
        (data[16] != 8 && data[16] != 24 && data[16] != 32)) {
        FS_FreeFile((void *)data);
        return BZ_TTA_ERR_MALFORMED;
    }
    width = wc3_u16(data + 12); height = wc3_u16(data + 14); pixel_bytes = data[16] / 8;
    offset = 18u + data[0];
    if (!width || !height || offset > size || width > SIZE_MAX / height ||
        (pixels = (size_t)width * height) > (size - offset) / pixel_bytes) {
        FS_FreeFile((void *)data);
        return BZ_TTA_ERR_MALFORMED;
    }
    FS_FreeFile((void *)data);
    metadata->footprint_x = (float)(width * WC3_TTA_PATH_CELL_SIZE);
    metadata->footprint_y = (float)(height * WC3_TTA_PATH_CELL_SIZE);
    return BZ_TTA_OK;
}

static float wc3_tint(LPCSTR value) {
    long component;
    char *end;
    if (!value || !*value || !strcmp(value, "_") || !strcmp(value, "-")) return 1.0f;
    component = strtol(value, &end, 10);
    return *end || component < 0 || component > 255 ? 1.0f : (float)component / 255.0f;
}

static bool wc3_contains_word(LPCSTR value, LPCSTR word) {
    size_t length;
    if (!value || !word || !(length = strlen(word))) return false;
    for (const char *at = value; (at = strstr(at, word)); at += length)
        if ((at == value || at[-1] == ',' || at[-1] == ' ') &&
            (!at[length] || at[length] == ',' || at[length] == ' '))
            return true;
    return false;
}

static void wc3_unit_tint(uint32_t class_id, bzTTAssetMetadata_t *metadata) {
    metadata->tint_r = wc3_tint(UnitStringFieldBase(UnitsMetaData, class_id, "uclr"));
    metadata->tint_g = wc3_tint(UnitStringFieldBase(UnitsMetaData, class_id, "uclg"));
    metadata->tint_b = wc3_tint(UnitStringFieldBase(UnitsMetaData, class_id, "uclb"));
    if (UnitBooleanFieldBase(UnitsMetaData, class_id, "utcc"))
        metadata->team_color = (uint32_t)UnitIntegerFieldBase(UnitsMetaData, class_id, "utco");
}

/* Spawn-table precedence is authoritative and avoids reading live server edicts. */
static bzTTAResult_t wc3_resolve_entity_metadata(uint32_t class_id, bzTTAssetMetadata_t *metadata) {
    char row[5];
    LPCSTR value, path;
    bzTTAResult_t status;
    const metadataMapSnapshot_t *snapshot;
    uint32_t table_id;
    if (!metadata || !class_id) return BZ_TTA_ERR_INVALID_ARGUMENT;
    snapshot = G_MetadataMapAcquire();
    if (!snapshot) return BZ_TTA_ERR_NOT_INITIALIZED;
    table_id = G_MetadataMapClass(snapshot, class_id);
    wc3_fourcc_name(class_id, row);
    metadata->class_id = class_id;
    metadata->team_color = BZ_TTA_TEAM_COLOR_NONE;
    metadata->tint_r = metadata->tint_g = metadata->tint_b = metadata->tint_a = 1.0f;
    if (Doodads && FS_FindSheetCell(Doodads, row, "file")) {
        metadata->category = BZ_TTA_CATEGORY_DOODAD;
        metadata->tint_r = wc3_tint(FS_FindSheetCell(Doodads, row, "vertR01"));
        metadata->tint_g = wc3_tint(FS_FindSheetCell(Doodads, row, "vertG01"));
        metadata->tint_b = wc3_tint(FS_FindSheetCell(Doodads, row, "vertB01"));
        status = wc3_path_footprint(FS_FindSheetCell(Doodads, row, "pathTex"), false, metadata);
        goto done;
    }
    if (UnitStringFieldBase(DestructableMetaData, table_id, "bfil")) {
        value = UnitStringFieldBase(DestructableMetaData, table_id, "btar");
        metadata->category = wc3_contains_word(value, "tree") ? BZ_TTA_CATEGORY_RESOURCE :
                                                                BZ_TTA_CATEGORY_DESTRUCTABLE;
        metadata->tint_r = wc3_tint(UnitStringFieldBase(DestructableMetaData, table_id, "bvcr"));
        metadata->tint_g = wc3_tint(UnitStringFieldBase(DestructableMetaData, table_id, "bvcg"));
        metadata->tint_b = wc3_tint(UnitStringFieldBase(DestructableMetaData, table_id, "bvcb"));
        status = wc3_path_footprint(UnitStringFieldBase(DestructableMetaData, table_id, "bptx"),
                                    true, metadata);
        goto done;
    }
    if (!UnitStringFieldBase(UnitsMetaData, table_id, "umdl")) {
        status = BZ_TTA_ERR_NOT_FOUND;
        goto done;
    }
    value = UnitStringFieldBase(UnitsMetaData, table_id, "uabt");
    metadata->category = (value && !strcmp(value, "resource")) ||
                         UnitBooleanFieldBase(UnitsMetaData, table_id, "uibo") ||
                         UnitBooleanFieldBase(UnitsMetaData, table_id, "ucbo") ?
                             BZ_TTA_CATEGORY_RESOURCE :
                         UnitBooleanFieldBase(UnitsMetaData, table_id, "ubdg") ?
                             BZ_TTA_CATEGORY_BUILDING : BZ_TTA_CATEGORY_MOBILE;
    wc3_unit_tint(table_id, metadata);
    if (metadata->category != BZ_TTA_CATEGORY_MOBILE) {
        path = UnitStringFieldBase(UnitsMetaData, table_id, "upat");
        status = wc3_path_footprint(path, true, metadata);
        goto done;
    }
    status = UnitStringFieldBase(UnitsMetaData, table_id, "ucol") ? BZ_TTA_OK : BZ_TTA_ERR_NOT_FOUND;
    /* ucol is the collision radius; the ABI footprint fields are full world-space dimensions. */
    metadata->footprint_x = metadata->footprint_y =
        2.0f * (float)UnitRealFieldBase(UnitsMetaData, table_id, "ucol");
    status = status == BZ_TTA_OK && metadata->footprint_x > 0 ? BZ_TTA_OK : BZ_TTA_ERR_NOT_FOUND;
done:
    G_MetadataMapRelease(snapshot);
    return status;
}

static uintptr_t wc3_metadata_token(void) {
    const metadataMapSnapshot_t *snapshot = G_MetadataMapAcquire();
    uintptr_t token = (uintptr_t)G_MetadataMapToken(snapshot);
    G_MetadataMapRelease(snapshot);
    return token;
}

/* Pointers plus dimensions identify one loaded immutable map lifetime without hashing every corner per frame. */
static uintptr_t wc3_terrain_token(void) {
    uintptr_t token;
    if (!world.map || !world.map->vertices || world.map->width < 2 || world.map->height < 2)
        return 0;
    token = (uintptr_t)world.map ^ ((uintptr_t)world.map->vertices >> 3);
    token ^= ((uintptr_t)world.map->width << 17) ^ ((uintptr_t)world.map->height << 1);
    token ^= ((uintptr_t)world.map->grounds >> 5) ^ ((uintptr_t)world.map->cliffs << 7);
    token ^= ((uintptr_t)world.map->num_grounds << 25) ^ ((uintptr_t)world.map->num_cliffs << 9);
    return token ? token : 1;
}

static void wc3_log_bad_terrain(uintptr_t token, LPCWAR3MAP map, size_t corner_index,
                                LPCWAR3MAPVERTEX corner, LPCSTR reason) {
    static uintptr_t last_token;
    pthread_mutex_lock(&terrain_sheet_lock);
    if (last_token != token) {
        last_token = token;
        fprintf(stderr,
                "WC3TabletopAssets: terrain %s token=0x%llx map=%p vertices=%p width=%u height=%u "
                "grounds=%u/%p cliffs=%u/%p corners=%zu",
                reason, (unsigned long long)token, (void *)map, map ? map->vertices : NULL,
                map ? map->width : 0, map ? map->height : 0, map ? map->num_grounds : 0,
                map ? (void *)map->grounds : NULL, map ? map->num_cliffs : 0,
                map ? (void *)map->cliffs : NULL,
                map && map->height && map->width <= SIZE_MAX / map->height ?
                    (size_t)map->width * map->height : 0);
        if (corner)
            fprintf(stderr, " first_bad=%zu(%zu,%zu) ground=%u/%u cliff=%u/%u",
                    corner_index, corner_index % map->width, corner_index / map->width,
                    corner->ground, map->num_grounds, corner->cliff, map->num_cliffs);
        fprintf(stderr, "\n");
    }
    pthread_mutex_unlock(&terrain_sheet_lock);
}

static bzTTTerrain_t *wc3_copy_terrain(uintptr_t *source_token, bzTTAResult_t *status) {
    LPCWAR3MAP map = world.map;
    bzTTTerrain_t *terrain;
    bzTTTerrainCorner_t *corners;
    bzTTTerrainTypeRecord_t *grounds, *cliffs;
    size_t corner_count, corners_bytes, grounds_bytes, cliffs_bytes, payload;
    if (!map || !map->vertices || map->width < 2 || map->height < 2 ||
        map->num_grounds > 256 || map->num_cliffs > 256 ||
        (map->num_grounds && !map->grounds) || (map->num_cliffs && !map->cliffs) ||
        map->width > SIZE_MAX / map->height ||
        (corner_count = (size_t)map->width * map->height) > WC3_TTA_MAX_TERRAIN_CORNERS) {
        wc3_log_bad_terrain(*source_token, map, 0, NULL, "shape invalid");
        *status = BZ_TTA_ERR_MALFORMED;
        return NULL;
    }
    for (size_t i = 0; i < corner_count; i++) {
        LPCWAR3MAPVERTEX corner = (LPCWAR3MAPVERTEX)map->vertices + i;
        /* W3E reserves cliff index 15 as the no-cliff sentinel, not a table index. */
        if (corner->ground >= map->num_grounds ||
            (corner->cliff != 0x0f && corner->cliff >= map->num_cliffs)) {
            wc3_log_bad_terrain(*source_token, map, i, corner, "corner index invalid");
            *status = BZ_TTA_ERR_MALFORMED;
            return NULL;
        }
    }
    corners_bytes = corner_count * sizeof(*corners);
    grounds_bytes = (size_t)map->num_grounds * sizeof(*grounds);
    cliffs_bytes = (size_t)map->num_cliffs * sizeof(*cliffs);
    payload = terrain_align_size(corners_bytes, _Alignof(bzTTTerrainTypeRecord_t));
    if (payload > SIZE_MAX - grounds_bytes || payload + grounds_bytes > SIZE_MAX - cliffs_bytes) {
        *status = BZ_TTA_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    terrain = BZ_TTA_TerrainAlloc(payload + grounds_bytes + cliffs_bytes);
    if (!terrain) {
        *status = BZ_TTA_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    terrain->source_token = *source_token;
    terrain->tileset = map->tileset;
    terrain->info = (bzTTTerrainInfo_t){
        .width = map->width, .height = map->height,
        .tile_width = map->width - 1, .tile_height = map->height - 1,
        .chunk_tiles = BZ_TTA_TERRAIN_CHUNK_TILES,
        .chunk_count_x = (map->width - 2 + BZ_TTA_TERRAIN_CHUNK_TILES) / BZ_TTA_TERRAIN_CHUNK_TILES,
        .chunk_count_y = (map->height - 2 + BZ_TTA_TERRAIN_CHUNK_TILES) / BZ_TTA_TERRAIN_CHUNK_TILES,
        .min_x = map->center.x, .min_y = map->center.y,
        .max_x = map->center.x + (map->width - 1) * TILE_SIZE,
        .max_y = map->center.y + (map->height - 1) * TILE_SIZE,
        .ground_type_count = map->num_grounds, .cliff_type_count = map->num_cliffs,
    };
    terrain->corners_offset = 0;
    terrain->grounds_offset = (uint32_t)terrain_align_size(corners_bytes, _Alignof(uint32_t));
    terrain->cliffs_offset = terrain->grounds_offset + (uint32_t)grounds_bytes;
    corners = BZ_TTA_TerrainData(terrain, terrain->corners_offset, corners_bytes);
    grounds = BZ_TTA_TerrainData(terrain, terrain->grounds_offset, grounds_bytes);
    cliffs = BZ_TTA_TerrainData(terrain, terrain->cliffs_offset, cliffs_bytes);
    if (!corners || (grounds_bytes && !grounds) || (cliffs_bytes && !cliffs)) {
        free(terrain);
        *status = BZ_TTA_ERR_MALFORMED;
        return NULL;
    }
    for (uint32_t i = 0; i < map->num_grounds; i++) grounds[i].id = map->grounds[i];
    for (uint32_t i = 0; i < map->num_cliffs; i++) cliffs[i].id = map->cliffs[i];
    for (size_t i = 0; i < corner_count; i++) {
        LPCWAR3MAPVERTEX src = (LPCWAR3MAPVERTEX)map->vertices + i;
        bzTTTerrainCorner_t *dst = corners + i;
        dst->height = DECODE_HEIGHT(src->accurate_height) + src->level * TILE_SIZE - HEIGHT_COR;
        /* Match desktop terrain: raw W3E water levels include an 80-unit format bias. */
        dst->water_height = DECODE_HEIGHT(src->waterlevel) - WATER_HEIGHT_COR;
        dst->ground_id = grounds[src->ground].id;
        dst->cliff_id = src->cliff == 0x0f ? 0 : cliffs[src->cliff].id;
        dst->ground_variation = src->groundVariation;
        dst->cliff_variation = src->cliffVariation;
        dst->cliff_level = src->level;
        dst->flags = (src->mapedge ? BZ_TTA_TERRAIN_MAP_EDGE : 0) |
                     (src->ramp ? BZ_TTA_TERRAIN_RAMP : 0) |
                     (src->blight ? BZ_TTA_TERRAIN_BLIGHT : 0) |
                     (src->water ? BZ_TTA_TERRAIN_WATER : 0) |
                     (src->boundary ? BZ_TTA_TERRAIN_BOUNDARY : 0) |
                     (src->cliff == 0x0f ? BZ_TTA_TERRAIN_NO_CLIFF : 0);
        grounds[src->ground].corner_count++;
        if (src->cliff != 0x0f) cliffs[src->cliff].corner_count++;
        if (src->water) terrain->water_corner_count++;
    }
    /* Match desktop IsTileWater: any wet corner renders unless any corner suppresses the tile at the map edge. */
    for (uint32_t y = 0; y < map->height - 1; y++) for (uint32_t x = 0; x < map->width - 1; x++) {
        uint32_t i = y * map->width + x;
        uint8_t flags = corners[i].flags | corners[i + 1].flags |
                        corners[i + map->width].flags | corners[i + map->width + 1].flags;
        if ((flags & BZ_TTA_TERRAIN_WATER) && !(flags & BZ_TTA_TERRAIN_MAP_EDGE))
            terrain->water_tile_count++;
    }
    pthread_mutex_lock(&terrain_sheet_lock);
    ground_sheet = FS_ParseSLK("TerrainArt\\Terrain.slk");
    cliff_sheet = FS_ParseSLK("TerrainArt\\CliffTypes.slk");
    pthread_mutex_unlock(&terrain_sheet_lock);
    *status = BZ_TTA_OK;
    return terrain;
}

void BZ_WC3_TTA_Source(bzTTAssetSource_t *source) {
    if (!source) return;
    *source = (bzTTAssetSource_t){
        .path_is_confined = wc3_tta_path_is_confined,
        .load_asset = wc3_load_asset,
        .terrain_token = wc3_terrain_token,
        .copy_terrain = wc3_copy_terrain,
        .resolve_terrain_identity = wc3_resolve_terrain_identity,
        .team_texture_count = wc3_team_texture_count,
        .resolve_team_texture_identity = wc3_resolve_team_texture_identity,
        .metadata_token = wc3_metadata_token,
        .resolve_entity_metadata = wc3_resolve_entity_metadata,
    };
}
