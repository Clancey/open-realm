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
 * IMPORTANT: fog cell math (bz_quest_wc3_fog_world_to_cell/cell_center/
 * classify_cell) stays in RAW engine world units end to end - it never
 * needs the shared world/tabletop transform, because
 * bzQuestWc3FogBounds_t is itself expressed in the same raw units as the
 * `worldX`/`worldY` arguments passed to it (both ultimately come from the
 * same authoritative map-bounds snapshot - see bz_quest_wc3_capture.c). The
 * shared bzQuestWc3WorldTransform_t (bz_quest_wc3_render.h) is only needed
 * where a position is placed on screen: the fog overlay quad's vertex
 * shader (see bz_quest_vk_wc3_fog.c's FogPushConsts_t - the quad's raw
 * world-space fragment varying is unaffected, only its on-screen gl_Position
 * is transformed, which is valid because the transform is affine) and the
 * selection-marker functions below.
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

/*
 * Builds a marker's world matrix from an ALREADY fully-positioned (swapped
 * and, if applicable, world-transformed) translation - used by
 * bz_quest_vk_wc3_fog.c's production path, which already has the render
 * item's finished `world[12/13/14]` (see bz_quest_wc3_build_world_matrix()).
 * `scaleX/scaleY/scaleZ` must be the SAME per-axis footprint/category scale
 * the selected entity's own model uses
 * (bz_quest_wc3_entity_footprint_scale(), bzQuestWc3RenderItem_t's
 * footprintScale* fields) - NOT the raw bzTTEntity_t.radius transport
 * field, which is tens of raw WC3 units and has no other proven role (see
 * bz_quest_wc3_render.h's header comment). Rejects any non-positive axis
 * scale (a marker cannot be drawn negative/zero size).
 */
bool bz_quest_wc3_selection_marker_from_translation(float tx, float ty, float tz, float scaleX, float scaleY,
                                                    float scaleZ, float tintR, float tintG, float tintB,
                                                    float tintA, bzQuestWc3SelectionMarker_t *out);

/*
 * Builds a marker directly from one entity's RAW engine-space origin plus
 * the shared world/tabletop transform (NULL meaning "no valid map bounds
 * this frame", raw passthrough) - mirrors
 * bz_quest_wc3_build_world_matrix()'s translation exactly: engine Y/Z swap
 * followed by bz_quest_wc3_world_transform_point(), so a marker built this
 * way always lands on the exact same point as the selected entity's own
 * model, never a stale pre-transform position. `scaleX/scaleY/scaleZ` -
 * same contract as bz_quest_wc3_selection_marker_from_translation() above.
 * (This entry point is currently exercised only by this file's host tests;
 * bz_quest_vk_wc3_fog.c's production path calls
 * bz_quest_wc3_selection_marker_from_translation() directly with an
 * already-built render-item translation instead of duplicating the
 * swap+transform here - both must therefore agree on the exact same
 * formula, which is why this function is implemented in terms of that one.)
 */
bool bz_quest_wc3_selection_marker_from_entity(float originX, float originY, float originZ,
                                               const bzQuestWc3WorldTransform_t *transform, float scaleX,
                                               float scaleY, float scaleZ, float tintR, float tintG,
                                               float tintB, float tintA, bzQuestWc3SelectionMarker_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_FOG_H */
