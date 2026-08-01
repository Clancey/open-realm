/*
 * bz_quest_scene.c - see bz_quest_scene.h for the "why". Kept free of any
 * Vulkan/OpenXR/Android includes so it host-compiles for
 * tests/test_bz_quest_scene.c.
 */
#include "bz_quest_scene.h"

#include <string.h>

static uint32_t bz_quest_scene_emit_tri(bzQuestVertex_t *out, uint32_t index, const float a[3],
                                        const float b[3], const float c[3], const float color[3]) {
    const float *verts[3] = {a, b, c};
    for (uint32_t i = 0; i < 3; i++) {
        memcpy(out[index].position, verts[i], sizeof(out[index].position));
        memcpy(out[index].color, color, sizeof(out[index].color));
        index++;
    }
    return index;
}

/* Emits one quad (two triangles, corners given in either winding order -
 * the scene pipeline runs with VK_CULL_MODE_NONE precisely so this
 * generator doesn't have to reason about winding per-tile/per-face). */
static uint32_t bz_quest_scene_emit_quad(bzQuestVertex_t *out, uint32_t index, const float a[3],
                                         const float b[3], const float c[3], const float d[3],
                                         const float color[3]) {
    index = bz_quest_scene_emit_tri(out, index, a, b, c, color);
    index = bz_quest_scene_emit_tri(out, index, a, c, d, color);
    return index;
}

static uint32_t bz_quest_scene_emit_cube(bzQuestVertex_t *out, uint32_t index, float cx, float cy,
                                         float cz, float half, const float color[3]) {
    const float x0 = cx - half, x1 = cx + half;
    const float y0 = cy - half, y1 = cy + half;
    const float z0 = cz - half, z1 = cz + half;
    const float v[8][3] = {
        {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
        {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1},
    };
    /* -Z, +Z, -X, +X, -Y, +Y faces. */
    index = bz_quest_scene_emit_quad(out, index, v[0], v[1], v[2], v[3], color);
    index = bz_quest_scene_emit_quad(out, index, v[5], v[4], v[7], v[6], color);
    index = bz_quest_scene_emit_quad(out, index, v[4], v[0], v[3], v[7], color);
    index = bz_quest_scene_emit_quad(out, index, v[1], v[5], v[6], v[2], color);
    index = bz_quest_scene_emit_quad(out, index, v[4], v[5], v[1], v[0], color);
    index = bz_quest_scene_emit_quad(out, index, v[3], v[2], v[6], v[7], color);
    return index;
}

bool bz_quest_scene_generate(bzQuestVertex_t *outVertices, float tableSize, float tableY,
                             float tableDistance) {
    if (!outVertices || tableSize <= 0.0f) return false;

    static const float kCheckerLight[3] = {0.55f, 0.55f, 0.58f};
    static const float kCheckerDark[3] = {0.12f, 0.12f, 0.14f};
    static const float kCubeColors[BZ_QUEST_SCENE_CUBE_COUNT][3] = {
        {0.85f, 0.10f, 0.10f}, {0.10f, 0.80f, 0.15f}, {0.10f, 0.20f, 0.90f}, {0.90f, 0.80f, 0.10f}};

    const float tileSize = tableSize / (float)BZ_QUEST_SCENE_TABLE_TILES;
    const float originX = -tableSize * 0.5f;
    const float originZ = -tableDistance - tableSize * 0.5f;

    uint32_t index = 0;
    for (uint32_t iz = 0; iz < BZ_QUEST_SCENE_TABLE_TILES; iz++) {
        for (uint32_t ix = 0; ix < BZ_QUEST_SCENE_TABLE_TILES; ix++) {
            const float x0 = originX + (float)ix * tileSize;
            const float x1 = x0 + tileSize;
            const float z0 = originZ + (float)iz * tileSize;
            const float z1 = z0 + tileSize;
            const float a[3] = {x0, tableY, z0};
            const float b[3] = {x1, tableY, z0};
            const float c[3] = {x1, tableY, z1};
            const float d[3] = {x0, tableY, z1};
            const bool light = ((ix + iz) & 1u) == 0u;
            index = bz_quest_scene_emit_quad(outVertices, index, a, b, c, d,
                                              light ? kCheckerLight : kCheckerDark);
        }
    }

    /* Four proxy cubes near the table's corners, inset by one tile so they
     * sit fully on the surface instead of overhanging the edge. */
    const float inset = tileSize * 1.5f;
    const float half = tileSize * 0.35f;
    const float cubeY = tableY + half;
    const float corners[BZ_QUEST_SCENE_CUBE_COUNT][2] = {
        {originX + inset, originZ + inset},
        {originX + tableSize - inset, originZ + inset},
        {originX + inset, originZ + tableSize - inset},
        {originX + tableSize - inset, originZ + tableSize - inset},
    };
    for (uint32_t i = 0; i < BZ_QUEST_SCENE_CUBE_COUNT; i++) {
        index = bz_quest_scene_emit_cube(outVertices, index, corners[i][0], cubeY, corners[i][1],
                                          half, kCubeColors[i]);
    }

    return true;
}
