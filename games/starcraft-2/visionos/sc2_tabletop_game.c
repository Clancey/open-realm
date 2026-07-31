/*
 * sc2_tabletop_game.c - StarCraft II side of the tabletop transport: snapshot lifecycle plus the
 * Layer 2A asset ABI's game-owned "source" (path confinement, file I/O, and terrain snapshotting).
 * This never touches platform/bridge/bz_tabletop_assets.h; that is the separate, Warcraft-shaped
 * v2 ABI and must not be reused here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/bridge/bz_tabletop_game.h"
#include "sc2_tabletop_assets_internal.h"
#include "sc2_tabletop_models.h"
#include "../common/sc2_map.h"

static uint32_t sc2_pack_color32(COLOR32 color) {
    return (uint32_t)color.r | ((uint32_t)color.g << 8) | ((uint32_t)color.b << 16) | ((uint32_t)color.a << 24);
}

static uint32_t sc2_terrain_availability_flags(sc2Map_t const *map) {
    uint32_t flags = 0;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_HEIGHT_MAP] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_HMAP;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_HEIGHT_MAP] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_SMAP;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_CELL_FLAGS] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_LFCT;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_CLIFF_LEVEL] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_CLIF;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_TEXTURE_MASKS] == SC2_LAYER_STATUS_OK)
        flags |= BZ_SC2A_TERRAIN_HAS_MASK;
    if (map->t3Terrain.fog_enabled) flags |= BZ_SC2A_TERRAIN_HAS_FOG;
    if (map->lighting.enabled) flags |= BZ_SC2A_TERRAIN_HAS_LIGHTING;
    return flags;
}

static uint32_t sc2_terrain_malformed_flags(sc2Map_t const *map) {
    uint32_t flags = 0;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_HEIGHT_MAP] == SC2_LAYER_STATUS_MALFORMED)
        flags |= BZ_SC2A_TERRAIN_HAS_HMAP;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_HEIGHT_MAP] == SC2_LAYER_STATUS_MALFORMED)
        flags |= BZ_SC2A_TERRAIN_HAS_SMAP;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_CELL_FLAGS] == SC2_LAYER_STATUS_MALFORMED)
        flags |= BZ_SC2A_TERRAIN_HAS_LFCT;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_CLIFF_LEVEL] == SC2_LAYER_STATUS_MALFORMED)
        flags |= BZ_SC2A_TERRAIN_HAS_CLIF;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_TEXTURE_MASKS] == SC2_LAYER_STATUS_MALFORMED)
        flags |= BZ_SC2A_TERRAIN_HAS_MASK;
    return flags;
}

/* WATER/PATHING/FLUFF_DOODAD/HARD_TILE reflect sc2_map.c's own per-map detection: the parser
 * sets SC2_LAYER_STATUS_UNSUPPORTED only when that file is actually present but left undecoded
 * (see sc2_parse_unsupported_terrain_layers). VERTEX_COLOR and PAINTED_PATHING have no matching
 * sc2TerrainLayerId_t entry at all - the parser cannot even detect their presence - so those two
 * bits are unconditionally set rather than left to look falsely "supported". */
static uint32_t sc2_terrain_unsupported_flags(sc2Map_t const *map) {
    uint32_t flags = BZ_SC2A_TERRAIN_UNSUPPORTED_VERTEX_COLOR | BZ_SC2A_TERRAIN_UNSUPPORTED_PAINTED_PATHING;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_WATER] == SC2_LAYER_STATUS_UNSUPPORTED)
        flags |= BZ_SC2A_TERRAIN_UNSUPPORTED_WATER;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_PATHING_INFO] == SC2_LAYER_STATUS_UNSUPPORTED)
        flags |= BZ_SC2A_TERRAIN_UNSUPPORTED_PATHING;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_FLUFF_DOODAD] == SC2_LAYER_STATUS_UNSUPPORTED)
        flags |= BZ_SC2A_TERRAIN_UNSUPPORTED_FLUFF_DOODAD;
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_HARD_TILE] == SC2_LAYER_STATUS_UNSUPPORTED)
        flags |= BZ_SC2A_TERRAIN_UNSUPPORTED_HARD_TILE;
    return flags;
}

/* sc2Map_t.generation already uniquely identifies one loaded map lifetime (bumped once per
 * SC2_MapLoad() call, never reset by SC2_MapShutdown()), so unlike WC3's pointer-hash token this
 * can be used directly: no map loaded yet reads as generation 0, which BZ_SC2A_PublishTerrainFromGame
 * already treats as "nothing to publish". */
static uintptr_t sc2_terrain_token(void) {
    sc2Map_t const *map = SC2_MapCurrent();
    return map && map->generation ? (uintptr_t)map->generation : 0;
}

static bzSC2Terrain_t *sc2_copy_terrain(uintptr_t *source_token, bzSC2AResult_t *status) {
    sc2Map_t const *map = SC2_MapCurrent();
    uint32_t cell_width, cell_height, hmap_width = 0, hmap_height = 0;
    uint32_t mask_width = 0, mask_height = 0, mask_layers = 0;
    uint32_t texture_count, cliff_set_count, cliff_cell_count;
    uint32_t textures_bytes, cliff_sets_bytes, cliff_cells_bytes, height_bytes, cells_bytes, mask_bytes;
    size_t total;
    bzSC2Terrain_t *terrain;
    bzSC2ATerrainTextureInfo_t *textures;
    bzSC2ACliffSetInfo_t *cliff_sets;
    bzSC2ACliffCellInfo_t *cliff_cells;
    bzSC2AHeightSample_t *height_samples;
    bzSC2ACellInfo_t *cells;
    uint8_t *mask;
    (void)source_token;
    if (!map) { *status = BZ_SC2A_ERR_NOT_FOUND; return NULL; }
    cell_width = sc2_map_cell_width(map);
    cell_height = sc2_map_cell_height(map);
    if (!cell_width || !cell_height) { *status = BZ_SC2A_ERR_MALFORMED; return NULL; }
    if (cell_width > BZ_SC2A_TERRAIN_MAX_DIMENSION || cell_height > BZ_SC2A_TERRAIN_MAX_DIMENSION) {
        *status = BZ_SC2A_ERR_TOO_LARGE;
        return NULL;
    }
    texture_count = map->t3Terrain.num_terrain_textures;
    cliff_set_count = map->t3Terrain.num_cliff_sets;
    cliff_cell_count = map->t3Terrain.num_cliff_cells;
    if (texture_count > BZ_SC2A_MAX_TERRAIN_TEXTURES || cliff_set_count > BZ_SC2A_MAX_CLIFF_SETS ||
        cliff_cell_count > BZ_SC2A_MAX_CLIFF_CELLS) {
        *status = BZ_SC2A_ERR_MALFORMED;
        return NULL;
    }
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_HEIGHT_MAP] == SC2_LAYER_STATUS_OK && map->t3HeightMap) {
        hmap_width = map->t3HeightMap->width;
        hmap_height = map->t3HeightMap->height;
        /* HMAP is a corner grid (one wider/taller than the cell grid it bounds). */
        if (hmap_width > BZ_SC2A_TERRAIN_MAX_DIMENSION + 1 || hmap_height > BZ_SC2A_TERRAIN_MAX_DIMENSION + 1) {
            *status = BZ_SC2A_ERR_TOO_LARGE;
            return NULL;
        }
    }
    if (map->terrain_layer_status[SC2_TERRAIN_LAYER_TEXTURE_MASKS] == SC2_LAYER_STATUS_OK && map->t3TextureMasks) {
        mask_width = map->t3TextureMasks->width;
        mask_height = map->t3TextureMasks->height;
        mask_layers = sc2_map_mask_layer_count(map);
        /* MASK is authored at 8x cell resolution; the old cell-grid cap rejected retail TRaynor01. */
        if (mask_width > BZ_SC2A_MASK_MAX_DIMENSION || mask_height > BZ_SC2A_MASK_MAX_DIMENSION ||
            mask_layers > BZ_SC2A_MAX_TERRAIN_TEXTURES ||
            (uint64_t)mask_width * mask_height * mask_layers > BZ_SC2A_MASK_MAX_BYTES) {
            *status = BZ_SC2A_ERR_TOO_LARGE;
            return NULL;
        }
    }
    textures_bytes = texture_count * (uint32_t)sizeof(bzSC2ATerrainTextureInfo_t);
    cliff_sets_bytes = cliff_set_count * (uint32_t)sizeof(bzSC2ACliffSetInfo_t);
    cliff_cells_bytes = cliff_cell_count * (uint32_t)sizeof(bzSC2ACliffCellInfo_t);
    height_bytes = hmap_width * hmap_height * (uint32_t)sizeof(bzSC2AHeightSample_t);
    cells_bytes = cell_width * cell_height * (uint32_t)sizeof(bzSC2ACellInfo_t);
    mask_bytes = mask_width * mask_height * mask_layers;
    total = (size_t)textures_bytes + cliff_sets_bytes + cliff_cells_bytes + height_bytes + cells_bytes + mask_bytes;
    terrain = BZ_SC2A_TerrainAlloc(total);
    if (!terrain) {
        *status = BZ_SC2A_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    terrain->textures_offset = 0;
    terrain->cliff_sets_offset = terrain->textures_offset + textures_bytes;
    terrain->cliff_cells_offset = terrain->cliff_sets_offset + cliff_sets_bytes;
    terrain->height_samples_offset = terrain->cliff_cells_offset + cliff_cells_bytes;
    terrain->cells_offset = terrain->height_samples_offset + height_bytes;
    terrain->mask_offset = terrain->cells_offset + cells_bytes;
    textures = BZ_SC2A_TerrainData(terrain, terrain->textures_offset, textures_bytes);
    cliff_sets = BZ_SC2A_TerrainData(terrain, terrain->cliff_sets_offset, cliff_sets_bytes);
    cliff_cells = BZ_SC2A_TerrainData(terrain, terrain->cliff_cells_offset, cliff_cells_bytes);
    height_samples = BZ_SC2A_TerrainData(terrain, terrain->height_samples_offset, height_bytes);
    cells = BZ_SC2A_TerrainData(terrain, terrain->cells_offset, cells_bytes);
    mask = BZ_SC2A_TerrainData(terrain, terrain->mask_offset, mask_bytes);
    if ((textures_bytes && !textures) || (cliff_sets_bytes && !cliff_sets) ||
        (cliff_cells_bytes && !cliff_cells) || (height_bytes && !height_samples) ||
        (cells_bytes && !cells) || (mask_bytes && !mask)) {
        free(terrain);
        *status = BZ_SC2A_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    for (uint32_t i = 0; i < texture_count; i++) {
        textures[i].index = i;
        snprintf(textures[i].diffuse_identity, sizeof(textures[i].diffuse_identity), "%s",
                 map->t3Terrain.terrain_textures[i].diffuse);
        snprintf(textures[i].normal_identity, sizeof(textures[i].normal_identity), "%s",
                 map->t3Terrain.terrain_textures[i].normal);
    }
    for (uint32_t i = 0; i < cliff_set_count; i++) {
        cliff_sets[i].index = i;
        snprintf(cliff_sets[i].name, sizeof(cliff_sets[i].name), "%s", map->t3Terrain.cliff_sets[i].name);
        snprintf(cliff_sets[i].mesh, sizeof(cliff_sets[i].mesh), "%s", map->t3Terrain.cliff_sets[i].mesh);
    }
    for (uint32_t i = 0; i < cliff_cell_count; i++)
        cliff_cells[i] = (bzSC2ACliffCellInfo_t){
            .flat_index = map->t3Terrain.cliff_cells[i].index, .flags = map->t3Terrain.cliff_cells[i].flags,
            .cliff_set = map->t3Terrain.cliff_cells[i].cliff_set, .variant = map->t3Terrain.cliff_cells[i].variant,
        };
    for (uint32_t y = 0; y < hmap_height; y++)
        for (uint32_t x = 0; x < hmap_width; x++) {
            sc2MapHeightSample_t const *src = &map->t3HeightMap->data[x + y * hmap_width];
            bzSC2AHeightSample_t *dst = &height_samples[y * hmap_width + x];
            dst->height = sc2_map_height_at_grid(map, x, y);
            dst->adjustment = sc2_map_height_adjust_at_grid(map, x, y);
            dst->raw_mask = src->extra; /* sc2MapHeightSample_t.extra; docs call it "mask", 0x00-0x03 */
        }
    /* MapInfo is the authoritative cell grid; CLIF/LFCT keep their own file-shaped dimensions
     * (see docs/embedded-map-files.md), so out-of-range cells default to level 0/no flags. */
    for (uint32_t y = 0; y < cell_height; y++)
        for (uint32_t x = 0; x < cell_width; x++) {
            bzSC2ACellInfo_t *dst = &cells[y * cell_width + x];
            uint32_t cliff_level = 0, cell_flags = 0;
            if (map->t3SyncCliffLevel &&
                map->terrain_layer_status[SC2_TERRAIN_LAYER_SYNC_CLIFF_LEVEL] == SC2_LAYER_STATUS_OK &&
                x < map->t3SyncCliffLevel->width && y < map->t3SyncCliffLevel->height) {
                uint16_t raw = map->t3SyncCliffLevel->data[x + y * map->t3SyncCliffLevel->width];
                cliff_level = raw < 0x40 ? raw : (uint32_t)(raw >> 6);
            }
            if (map->t3CellFlags && map->terrain_layer_status[SC2_TERRAIN_LAYER_CELL_FLAGS] == SC2_LAYER_STATUS_OK &&
                x < map->t3CellFlags->width && y < map->t3CellFlags->height)
                cell_flags = map->t3CellFlags->data[x + y * map->t3CellFlags->width];
            dst->cliff_level = (uint16_t)cliff_level;
            dst->cell_flags = (uint8_t)cell_flags;
        }
    for (uint32_t layer = 0; layer < mask_layers; layer++)
        sc2_map_mask_decode_layer(map, layer, mask + (size_t)layer * mask_width * mask_height);
    terrain->info = (bzSC2ATerrainInfo_t){
        .generation = map->generation,
        .availability_flags = sc2_terrain_availability_flags(map),
        .malformed_flags = sc2_terrain_malformed_flags(map),
        .unsupported_flags = sc2_terrain_unsupported_flags(map),
        .cell_width = cell_width, .cell_height = cell_height,
        .hmap_width = hmap_width, .hmap_height = hmap_height,
        .mask_width = mask_width, .mask_height = mask_height, .mask_layer_count = mask_layers,
        .texture_count = texture_count, .cliff_set_count = cliff_set_count, .cliff_cell_count = cliff_cell_count,
        .origin_x = map->origin.x, .origin_y = map->origin.y, .cell_size = map->cell_size,
        .height_quantize_bias = map->t3Terrain.height_quantize_bias,
        .height_quantize_scale = map->t3Terrain.height_quantize_scale,
        .standard_height = map->t3Terrain.standard_height,
        .fog_enabled = map->t3Terrain.fog_enabled,
        .fog_density = map->t3Terrain.fog_density,
        .fog_falloff = map->t3Terrain.fog_falloff,
        .fog_start_height = map->t3Terrain.fog_start_height,
        .fog_color = sc2_pack_color32(map->t3Terrain.fog_color),
    };
    *status = BZ_SC2A_OK;
    return terrain;
}

void BZ_SC2_TTA_Source(bzSC2ASource_t *source) {
    if (!source) return;
    *source = (bzSC2ASource_t){
        .path_is_confined = sc2_tta_path_is_confined,
        .read_file = FS_ReadFile,
        .free_file = FS_FreeFile,
        .terrain_token = sc2_terrain_token,
        .copy_terrain = sc2_copy_terrain,
    };
}

void BZ_GameTabletopInit(void) { BZ_SC2A_Init(); BZ_SC2M_Init(); }
void BZ_GameTabletopShutdown(void) { BZ_SC2M_Shutdown(); BZ_SC2A_Shutdown(); }
void BZ_GameTabletopPublish(void) {
    sc2Map_t const *map = SC2_MapCurrent();
    BZ_SC2A_PublishTerrainFromGame();
    BZ_SC2M_BeginRegistration(map ? map->generation : 0);
}
