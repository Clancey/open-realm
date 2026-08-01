/*
 * bz_quest_wc3_fog.c - see bz_quest_wc3_fog.h.
 */
#include "bz_quest_wc3_fog.h"

#include <math.h>
#include <string.h>

static bool fog_bounds_valid(const bzQuestWc3FogBounds_t *bounds) {
    return bounds && bounds->maxX > bounds->minX && bounds->maxY > bounds->minY;
}

static uint32_t fog_row_bytes_for(uint32_t width, uint32_t dstRowBytes, uint32_t row) {
    (void)width;
    return row * dstRowBytes;
}

bzQuestWc3FogCellState_t bz_quest_wc3_fog_classify_cell(uint8_t visible, uint8_t explored) {
    return visible ? BZ_QUEST_WC3_FOG_VISIBLE : (explored ? BZ_QUEST_WC3_FOG_EXPLORED_NOT_VISIBLE
                                                          : BZ_QUEST_WC3_FOG_UNSEEN);
}

uint8_t bz_quest_wc3_fog_pack_value(uint8_t visible, uint8_t explored) {
    switch (bz_quest_wc3_fog_classify_cell(visible, explored)) {
        case BZ_QUEST_WC3_FOG_VISIBLE: return 255;
        case BZ_QUEST_WC3_FOG_EXPLORED_NOT_VISIBLE: return 128;
        default: return 0;
    }
}

bool bz_quest_wc3_fog_grid_supported(uint32_t width, uint32_t height) {
    return width > 0 && height > 0 && width <= BZ_QUEST_WC3_FOG_MAX_CELLS_PER_AXIS &&
           height <= BZ_QUEST_WC3_FOG_MAX_CELLS_PER_AXIS;
}

uint32_t bz_quest_wc3_fog_cell_count(uint32_t width, uint32_t height) {
    return bz_quest_wc3_fog_grid_supported(width, height) ? width * height : 0;
}

bool bz_quest_wc3_fog_cell_index(uint32_t width, uint32_t height, uint32_t cellX, uint32_t cellY,
                                 uint32_t *outIndex) {
    if (!outIndex || !bz_quest_wc3_fog_grid_supported(width, height) || cellX >= width || cellY >= height)
        return false;
    *outIndex = cellY * width + cellX;
    return true;
}

bool bz_quest_wc3_fog_world_to_cell(const bzQuestWc3FogBounds_t *bounds, uint32_t width, uint32_t height,
                                    float worldX, float worldY, uint32_t *outCellX, uint32_t *outCellY) {
    if (!outCellX || !outCellY || !fog_bounds_valid(bounds) || !bz_quest_wc3_fog_grid_supported(width, height))
        return false;
    int32_t cellX = (int32_t)floorf((worldX - bounds->minX) / BZ_QUEST_WC3_FOG_CELL_SIZE);
    int32_t cellY = (int32_t)floorf((worldY - bounds->minY) / BZ_QUEST_WC3_FOG_CELL_SIZE);
    if (cellX < 0) cellX = 0;
    else if ((uint32_t)cellX >= width) cellX = (int32_t)width - 1;
    if (cellY < 0) cellY = 0;
    else if ((uint32_t)cellY >= height) cellY = (int32_t)height - 1;
    *outCellX = (uint32_t)cellX;
    *outCellY = (uint32_t)cellY;
    return true;
}

bool bz_quest_wc3_fog_cell_center(const bzQuestWc3FogBounds_t *bounds, uint32_t width, uint32_t height,
                                  uint32_t cellX, uint32_t cellY, float *outWorldX, float *outWorldY) {
    if (!outWorldX || !outWorldY || !fog_bounds_valid(bounds)) return false;
    if (!bz_quest_wc3_fog_cell_index(width, height, cellX, cellY, &(uint32_t){0})) return false;
    *outWorldX = bounds->minX + ((float)cellX + 0.5f) * BZ_QUEST_WC3_FOG_CELL_SIZE;
    *outWorldY = bounds->minY + ((float)cellY + 0.5f) * BZ_QUEST_WC3_FOG_CELL_SIZE;
    return true;
}

bool bz_quest_wc3_fog_pack_texture(const uint8_t *visible, const uint8_t *explored, uint32_t width,
                                   uint32_t height, uint32_t dstRowBytes, uint8_t *dst,
                                   uint32_t dstCapacity) {
    uint32_t cells = bz_quest_wc3_fog_cell_count(width, height);
    if (!visible || !explored || !dst || !cells || dstRowBytes < width) return false;
    if (dstCapacity < height * dstRowBytes) return false;
    for (uint32_t y = 0; y < height; y++) {
        uint32_t rowOffset = fog_row_bytes_for(width, dstRowBytes, y);
        memset(dst + rowOffset, 0, dstRowBytes);
        for (uint32_t x = 0; x < width; x++) {
            uint32_t i = y * width + x;
            dst[rowOffset + x] = bz_quest_wc3_fog_pack_value(visible[i], explored[i]);
        }
    }
    return true;
}

bool bz_quest_wc3_fog_bytes_differ(const uint8_t *a, uint32_t aLen, const uint8_t *b, uint32_t bLen) {
    if (aLen != bLen) return true;
    if (aLen == 0) return false;
    if (!a || !b) return true;
    return memcmp(a, b, aLen) != 0;
}

bool bz_quest_wc3_selection_marker_from_translation(float tx, float ty, float tz, float radius, float tintR,
                                                    float tintG, float tintB, float tintA,
                                                    bzQuestWc3SelectionMarker_t *out) {
    if (!out || radius <= 0.0f) return false;
    memset(out, 0, sizeof(*out));
    out->world[0] = out->world[5] = out->world[10] = radius;
    out->world[15] = 1.0f;
    out->world[12] = tx;
    out->world[13] = ty;
    out->world[14] = tz;
    out->tint[0] = tintR;
    out->tint[1] = tintG;
    out->tint[2] = tintB;
    out->tint[3] = tintA;
    return true;
}

bool bz_quest_wc3_selection_marker_from_entity(float originX, float originY, float originZ, float radius,
                                               float tintR, float tintG, float tintB, float tintA,
                                               bzQuestWc3SelectionMarker_t *out) {
    /* Mirrors bz_quest_wc3_build_world_matrix()'s translation exactly:
     * target X = engine X, target Y = engine Z, target Z = engine Y. */
    return bz_quest_wc3_selection_marker_from_translation(originX, originZ, originY, radius,
                                                          tintR, tintG, tintB, tintA, out);
}
