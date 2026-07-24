#include "wc3_tabletop_assets_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"

#define WC3_TTA_MAX_TERRAIN_CORNERS (1025u * 1025u)

static size_t terrain_align_size(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static bzTTAsset_t *wc3_load_asset(const char *identity, bzTTAssetKind_t kind,
                                    const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status) {
    DWORD size = 0;
    void *data = FS_ReadFile(identity, &size);
    PATHSTR alternate;
    bzTTAsset_t *asset;
    if (!data && kind == BZ_TTA_ASSET_MODEL) {
        size_t len = strlen(identity);
        if (len >= 4 && len < sizeof(alternate) && !strcasecmp(identity + len - 4, ".mdl")) {
            memcpy(alternate, identity, len + 1);
            alternate[len - 1] = 'x'; /* Matches the existing game model resolver. */
            data = FS_ReadFile(alternate, &size);
        }
    }
    if (!data || !size) {
        if (data) FS_FreeFile(data);
        *status = BZ_TTA_ERR_NOT_FOUND;
        return NULL;
    }
    asset = kind == BZ_TTA_ASSET_IMAGE
        ? BZ_WC3_TTA_DecodeBLP(data, size, identity, metadata, status)
        : BZ_WC3_TTA_DecodeMDX(data, size, identity, metadata, status);
    FS_FreeFile(data);
    return asset;
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

static bzTTTerrain_t *wc3_copy_terrain(uintptr_t *source_token, bzTTAResult_t *status) {
    LPCWAR3MAP map = world.map;
    bzTTTerrain_t *terrain;
    bzTTTerrainCorner_t *corners;
    uint32_t *grounds, *cliffs;
    size_t corner_count, corners_bytes, grounds_bytes, cliffs_bytes, payload;
    if (!map || !map->vertices || map->width < 2 || map->height < 2 ||
        map->num_grounds > 256 || map->num_cliffs > 256 ||
        (map->num_grounds && !map->grounds) || (map->num_cliffs && !map->cliffs) ||
        map->width > SIZE_MAX / map->height ||
        (corner_count = (size_t)map->width * map->height) > WC3_TTA_MAX_TERRAIN_CORNERS) {
        *status = BZ_TTA_ERR_MALFORMED;
        return NULL;
    }
    for (size_t i = 0; i < corner_count; i++) {
        LPCWAR3MAPVERTEX corner = (LPCWAR3MAPVERTEX)map->vertices + i;
        if (corner->ground >= map->num_grounds || corner->cliff >= map->num_cliffs) {
            *status = BZ_TTA_ERR_MALFORMED;
            return NULL;
        }
    }
    corners_bytes = corner_count * sizeof(*corners);
    grounds_bytes = (size_t)map->num_grounds * sizeof(*grounds);
    cliffs_bytes = (size_t)map->num_cliffs * sizeof(*cliffs);
    payload = terrain_align_size(corners_bytes, _Alignof(uint32_t));
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
    if (grounds_bytes) memcpy(grounds, map->grounds, grounds_bytes);
    if (cliffs_bytes) memcpy(cliffs, map->cliffs, cliffs_bytes);
    for (size_t i = 0; i < corner_count; i++) {
        LPCWAR3MAPVERTEX src = (LPCWAR3MAPVERTEX)map->vertices + i;
        bzTTTerrainCorner_t *dst = corners + i;
        dst->height = DECODE_HEIGHT(src->accurate_height) + src->level * TILE_SIZE - HEIGHT_COR;
        /* Match desktop terrain: raw W3E water levels include an 80-unit format bias. */
        dst->water_height = DECODE_HEIGHT(src->waterlevel) - WATER_HEIGHT_COR;
        dst->ground_id = map->grounds[src->ground];
        dst->cliff_id = map->cliffs[src->cliff];
        dst->ground_variation = src->groundVariation;
        dst->cliff_variation = src->cliffVariation;
        dst->cliff_level = src->level;
        dst->flags = (src->mapedge ? BZ_TTA_TERRAIN_MAP_EDGE : 0) |
                     (src->ramp ? BZ_TTA_TERRAIN_RAMP : 0) |
                     (src->blight ? BZ_TTA_TERRAIN_BLIGHT : 0) |
                     (src->water ? BZ_TTA_TERRAIN_WATER : 0) |
                     (src->boundary ? BZ_TTA_TERRAIN_BOUNDARY : 0);
    }
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
    };
}
