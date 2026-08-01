/*
 * bz_quest_wc3_fog.h - layer 5D: platform-independent Warcraft III fog-of-war
 * classification/packing math plus selection-marker transform helpers.
 *
 * Like bz_quest_wc3_render.h and bz_quest_wc3_terrain.h, every type/function
 * here is plain C POD/math only: no transport ABI, Vulkan, OpenXR, Android,
 * or Quest-host types appear. The impure capture/Vulkan modules copy ABI data
 * into these structs, then call the helpers below.
 *
 * Fog cell size is a fixed Warcraft III compile-time constant, not a transport
 * field: common/common.h:20-21 defines FOW_CELLS_PER_TILE_SIDE = 2 and
 * FOW_CELL_SIZE = TILE_SIZE / FOW_CELLS_PER_TILE_SIDE, while
 * games/warcraft-3/common/mapinfo.h:6 defines TILE_SIZE = 128, so one fog cell
 * is exactly 64.0 world units. World<->cell conversion below mirrors
 * games/warcraft-3/game/g_fow.c's G_FowWorldToCellX/Y floor((x-min)/64) rule;
 * cell->world returns the CELL CENTER (min + (cell + 0.5) * 64), the inverse
 * convention that round-trips through that floor() mapping.
 *
 * IMPORTANT: layer 5B's terrain renderer and layer 5A/5C's entity renderer
 * already use DIFFERENT coordinate spaces (terrain compressed/centered into a
 * ~1.08-unit box; entities/models/fog remain raw world units with the Y<->Z
 * axis swap). This module intentionally inherits the ENTITY convention
 * unchanged; fixing the pre-existing terrain/entity mismatch is out of scope
 * for layer 5D and would otherwise introduce a third convention.
 */
#ifndef BZ_QUEST_WC3_FOG_H
#define BZ_QUEST_WC3_FOG_H

#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_wc3_terrain.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_WC3_FOG_MAX_CELLS_PER_AXIS = BZ_QUEST_WC3_TERRAIN_MAX_TILES * 2,
    BZ_QUEST_WC3_FOG_MAX_CELLS =
        BZ_QUEST_WC3_FOG_MAX_CELLS_PER_AXIS * BZ_QUEST_WC3_FOG_MAX_CELLS_PER_AXIS,
};

#define BZ_QUEST_WC3_FOG_CELL_SIZE 64.0f

typedef enum {
    BZ_QUEST_WC3_FOG_VISIBLE = 0,
    BZ_QUEST_WC3_FOG_EXPLORED_NOT_VISIBLE = 1,
    BZ_QUEST_WC3_FOG_UNSEEN = 2,
} bzQuestWc3FogCellState_t;

typedef struct {
    float minX, minY, maxX, maxY;
} bzQuestWc3FogBounds_t;

typedef struct {
    float world[16];
    float tint[4];
} bzQuestWc3SelectionMarker_t;

bzQuestWc3FogCellState_t bz_quest_wc3_fog_classify_cell(uint8_t visible, uint8_t explored);
uint8_t bz_quest_wc3_fog_pack_value(uint8_t visible, uint8_t explored);
bool bz_quest_wc3_fog_grid_supported(uint32_t width, uint32_t height);
uint32_t bz_quest_wc3_fog_cell_count(uint32_t width, uint32_t height);
bool bz_quest_wc3_fog_cell_index(uint32_t width, uint32_t height, uint32_t cellX, uint32_t cellY,
                                 uint32_t *outIndex);
bool bz_quest_wc3_fog_world_to_cell(const bzQuestWc3FogBounds_t *bounds, uint32_t width, uint32_t height,
                                    float worldX, float worldY, uint32_t *outCellX, uint32_t *outCellY);
bool bz_quest_wc3_fog_cell_center(const bzQuestWc3FogBounds_t *bounds, uint32_t width, uint32_t height,
                                  uint32_t cellX, uint32_t cellY, float *outWorldX, float *outWorldY);
bool bz_quest_wc3_fog_pack_texture(const uint8_t *visible, const uint8_t *explored, uint32_t width,
                                   uint32_t height, uint32_t dstRowBytes, uint8_t *dst,
                                   uint32_t dstCapacity);
bool bz_quest_wc3_fog_bytes_differ(const uint8_t *a, uint32_t aLen, const uint8_t *b, uint32_t bLen);
bool bz_quest_wc3_selection_marker_from_translation(float tx, float ty, float tz, float radius, float tintR,
                                                    float tintG, float tintB, float tintA,
                                                    bzQuestWc3SelectionMarker_t *out);
bool bz_quest_wc3_selection_marker_from_entity(float originX, float originY, float originZ, float radius,
                                               float tintR, float tintG, float tintB, float tintA,
                                               bzQuestWc3SelectionMarker_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_FOG_H */
