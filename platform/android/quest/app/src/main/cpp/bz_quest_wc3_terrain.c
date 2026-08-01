/*
 * bz_quest_wc3_terrain.c - see bz_quest_wc3_terrain.h.
 */
#include "bz_quest_wc3_terrain.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t terrain_bucket_count(const bzQuestWc3TerrainInput_t *terrain) {
    return terrain->referencedGroundCount + terrain->referencedCliffCount + (terrain->hasWater ? 1u : 0u);
}

static uint32_t ground_bucket_index(uint32_t referencedIndex) { return referencedIndex; }
static uint32_t cliff_bucket_index(const bzQuestWc3TerrainInput_t *terrain, uint32_t referencedIndex) {
    return terrain->referencedGroundCount + referencedIndex;
}
static uint32_t water_bucket_index(const bzQuestWc3TerrainInput_t *terrain) {
    return terrain->referencedGroundCount + terrain->referencedCliffCount;
}

/* Keeps corner-grid lookup one place so every helper shares the same row-major shape. */
static const bzQuestWc3TerrainCorner_t *terrain_corner(const bzQuestWc3TerrainInput_t *terrain,
                                                       uint32_t x, uint32_t z) {
    return &terrain->corners[z * terrain->cornerWidth + x];
}

/* Negative sentinel means "capture could not resolve the raw type ID"; layer math uses base 0 then falls back. */
static uint32_t safe_ground_index(const bzQuestWc3TerrainCorner_t *corner) {
    return corner->groundTypeIndex >= 0 ? (uint32_t)corner->groundTypeIndex : 0u;
}

/* Referenced textures are keyed by ABI type_index, not raw type_id; rank == draw/material bucket. */
static int32_t find_ground_rank(const bzQuestWc3TerrainInput_t *terrain, uint32_t typeIndex) {
    for (uint32_t i = 0; i < terrain->referencedGroundCount; i++)
        if (terrain->grounds[i].typeIndex == typeIndex) return (int32_t)i;
    return -1;
}

static int32_t find_cliff_rank(const bzQuestWc3TerrainInput_t *terrain, uint32_t typeIndex) {
    for (uint32_t i = 0; i < terrain->referencedCliffCount; i++)
        if (terrain->cliffs[i].typeIndex == typeIndex) return (int32_t)i;
    return -1;
}

/* Matches WarcraftRenderMath.swift:445-449's normalize({h00-h10,max(cellSize,0.0001),h00-h01}). */
static void normalized_normal(float h00, float h10, float h01, float cellSize, float out[3]) {
    float x = h00 - h10, y = fmaxf(cellSize, 0.0001f), z = h00 - h01;
    float len = sqrtf(x * x + y * y + z * z);
    if (len <= 0.0f) len = 1.0f;
    out[0] = x / len;
    out[1] = y / len;
    out[2] = z / len;
}

/* Terrain uses the global tile-grid center, never a per-chunk local origin. */
static void terrain_point(const bzQuestWc3TerrainInput_t *terrain, uint32_t x, uint32_t z, float height,
                          float out[3]) {
    float centerX = (float)terrain->tileWidth * terrain->cellSize * 0.5f;
    float centerZ = (float)terrain->tileHeight * terrain->cellSize * 0.5f;
    out[0] = (float)x * terrain->cellSize - centerX;
    out[1] = height;
    out[2] = (float)z * terrain->cellSize - centerZ;
}

bool bz_quest_wc3_terrain_quad_uses_forward_winding(const float a[3], const float b[3], const float c[3],
                                                    const float normal[3]) {
    float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    float ac[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float face[3] = {
        ab[1] * ac[2] - ab[2] * ac[1],
        ab[2] * ac[0] - ab[0] * ac[2],
        ab[0] * ac[1] - ab[1] * ac[0],
    };
    return face[0] * normal[0] + face[1] * normal[1] + face[2] * normal[2] >= 0.0f;
}

/* One bucket == one contiguous draw range; this appends vertices/indices inside that bucket's reserved slice. */
static void emit_quad(bzQuestWc3TerrainChunk_t *chunk, uint32_t bucket, uint32_t bucketVertexStart,
                      uint32_t bucketIndexStart, uint32_t *vertexCursor, uint32_t *indexCursor,
                      const float a[3], const float b[3], const float c[3], const float d[3],
                      const float normal[3], const float uv[4][2], const float color[4][4]) {
    uint32_t base = bucketVertexStart + *vertexCursor;
    bzQuestWc3TerrainVertex_t *verts = &chunk->vertices[base];
    const float *pos[4] = {a, b, c, d};
    for (uint32_t i = 0; i < 4; i++) {
        memcpy(verts[i].position, pos[i], sizeof(verts[i].position));
        verts[i].uv[0] = uv[i][0];
        verts[i].uv[1] = uv[i][1];
        memcpy(verts[i].color, color[i], sizeof(verts[i].color));
    }
    uint32_t *dst = &chunk->indices[bucketIndexStart + *indexCursor];
    if (bz_quest_wc3_terrain_quad_uses_forward_winding(a, b, c, normal)) {
        dst[0] = base + 0; dst[1] = base + 1; dst[2] = base + 2;
        dst[3] = base + 0; dst[4] = base + 2; dst[5] = base + 3;
    } else {
        dst[0] = base + 0; dst[1] = base + 2; dst[2] = base + 1;
        dst[3] = base + 0; dst[4] = base + 3; dst[5] = base + 2;
    }
    *vertexCursor += 4;
    *indexCursor += 6;
    (void)bucket;
}

static void full_uv(float uv[4][2]) {
    uv[0][0] = 0.0f; uv[0][1] = 0.0f;
    uv[1][0] = 1.0f; uv[1][1] = 0.0f;
    uv[2][0] = 1.0f; uv[2][1] = 1.0f;
    uv[3][0] = 0.0f; uv[3][1] = 1.0f;
}

static void opaque_white(float color[4][4]) {
    for (uint32_t i = 0; i < 4; i++) color[i][0] = color[i][1] = color[i][2] = color[i][3] = 1.0f;
}

static bool terrain_dimensions_valid(const bzQuestWc3TerrainInput_t *terrain) {
    return terrain && terrain->cornerWidth > 1 && terrain->cornerHeight > 1 &&
           terrain->tileWidth == terrain->cornerWidth - 1 && terrain->tileHeight == terrain->cornerHeight - 1 &&
           terrain->tileWidth <= BZ_QUEST_WC3_TERRAIN_MAX_TILES &&
           terrain->tileHeight <= BZ_QUEST_WC3_TERRAIN_MAX_TILES &&
           terrain->chunkCountX == (terrain->tileWidth + BZ_QUEST_WC3_TERRAIN_CHUNK_TILES - 1) /
                                  BZ_QUEST_WC3_TERRAIN_CHUNK_TILES &&
           terrain->chunkCountZ == (terrain->tileHeight + BZ_QUEST_WC3_TERRAIN_CHUNK_TILES - 1) /
                                  BZ_QUEST_WC3_TERRAIN_CHUNK_TILES &&
           terrain->cellSize > 0.0f && isfinite(terrain->cellSize);
}

bzQuestWc3TerrainStatus_t bz_quest_wc3_terrain_measure(const bzQuestWc3TerrainBounds_t *bounds,
                                                       uint32_t tileWidth, uint32_t tileHeight,
                                                       bzQuestWc3TerrainMetrics_t *out) {
    if (!bounds || !out || tileWidth == 0 || tileHeight == 0) return BZ_QUEST_WC3_TERRAIN_ERR_INVALID_ARGUMENT;
    float spanX = bounds->maxX - bounds->minX, spanZ = bounds->maxZ - bounds->minZ;
    if (!isfinite(spanX) || !isfinite(spanZ) || spanX <= 0.0f || spanZ <= 0.0f)
        return BZ_QUEST_WC3_TERRAIN_ERR_INVALID_BOUNDS;
    float tileSizeX = spanX / (float)tileWidth, tileSizeZ = spanZ / (float)tileHeight;
    if (fabsf(tileSizeX - tileSizeZ) > fmaxf(tileSizeX, tileSizeZ) * 0.001f)
        return BZ_QUEST_WC3_TERRAIN_ERR_NON_SQUARE_TILES;
    out->scale = 1.08f / fmaxf(spanX, spanZ);
    out->cellSize = tileSizeX * out->scale;
    return BZ_QUEST_WC3_TERRAIN_OK;
}

void bz_quest_wc3_terrain_chunk_grid(uint32_t tileWidth, uint32_t tileHeight,
                                     uint32_t *outChunkCountX, uint32_t *outChunkCountZ) {
    if (outChunkCountX)
        *outChunkCountX = (tileWidth + BZ_QUEST_WC3_TERRAIN_CHUNK_TILES - 1) / BZ_QUEST_WC3_TERRAIN_CHUNK_TILES;
    if (outChunkCountZ)
        *outChunkCountZ = (tileHeight + BZ_QUEST_WC3_TERRAIN_CHUNK_TILES - 1) / BZ_QUEST_WC3_TERRAIN_CHUNK_TILES;
}

bool bz_quest_wc3_terrain_chunk_bounds(uint32_t tileWidth, uint32_t tileHeight, uint32_t chunkX,
                                       uint32_t chunkZ, bzQuestWc3TerrainChunkBounds_t *out) {
    uint32_t countX = 0, countZ = 0;
    if (!out) return false;
    bz_quest_wc3_terrain_chunk_grid(tileWidth, tileHeight, &countX, &countZ);
    if (chunkX >= countX || chunkZ >= countZ) return false;
    out->minX = chunkX * BZ_QUEST_WC3_TERRAIN_CHUNK_TILES;
    out->minZ = chunkZ * BZ_QUEST_WC3_TERRAIN_CHUNK_TILES;
    out->maxX = out->minX + BZ_QUEST_WC3_TERRAIN_CHUNK_TILES;
    out->maxZ = out->minZ + BZ_QUEST_WC3_TERRAIN_CHUNK_TILES;
    if (out->maxX > tileWidth) out->maxX = tileWidth;
    if (out->maxZ > tileHeight) out->maxZ = tileHeight;
    return true;
}

float bz_quest_wc3_terrain_water_opacity(float rawWaterHeight, float rawHeight) {
    float alpha = (rawWaterHeight - rawHeight) / 50.0f;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 0.5f) alpha = 0.5f;
    return alpha;
}

bool bz_quest_wc3_terrain_tile_is_water(const bzQuestWc3TerrainCorner_t corners[4]) {
    bool wet = false;
    for (uint32_t i = 0; i < 4; i++) wet = wet || (corners[i].flags & BZ_QUEST_WC3_TERRAIN_WATER) != 0;
    for (uint32_t i = 0; i < 4; i++) wet = wet && (corners[i].flags & BZ_QUEST_WC3_TERRAIN_MAP_EDGE) == 0;
    return wet;
}

bool bz_quest_wc3_terrain_tile_is_cliff(const bzQuestWc3TerrainCorner_t corners[4]) {
    bool ramp = false, anyCliffFace = false;
    uint8_t firstLevel = corners[0].cliffLevel;
    bool distinctLevel = false;
    for (uint32_t i = 0; i < 4; i++) {
        ramp = ramp || (corners[i].flags & BZ_QUEST_WC3_TERRAIN_RAMP) != 0;
        anyCliffFace = anyCliffFace || (corners[i].flags & BZ_QUEST_WC3_TERRAIN_NO_CLIFF) == 0;
        distinctLevel = distinctLevel || corners[i].cliffLevel != firstLevel;
    }
    return !ramp && anyCliffFace && distinctLevel;
}

float bz_quest_wc3_terrain_cliff_bottom(float h00, float h10, float h11, float h01, float cellSize) {
    float bottom = h00;
    if (h10 < bottom) bottom = h10;
    if (h11 < bottom) bottom = h11;
    if (h01 < bottom) bottom = h01;
    return bottom - cellSize;
}

bzQuestWc3TerrainMaterialRef_t bz_quest_wc3_terrain_resolve_cliff_material(
    const bzQuestWc3TerrainInput_t *terrain, const bzQuestWc3TerrainCorner_t corners[4]) {
    bzQuestWc3TerrainMaterialRef_t out = {BZ_QUEST_WC3_TERRAIN_MATERIAL_GROUND, 0};
    for (uint32_t i = 0; i < 4; i++) {
        if (corners[i].flags & BZ_QUEST_WC3_TERRAIN_NO_CLIFF) continue;
        if (corners[i].cliffTypeIndex >= 0) {
            int32_t rank = find_cliff_rank(terrain, (uint32_t)corners[i].cliffTypeIndex);
            if (rank >= 0) {
                out.kind = BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF;
                out.referencedIndex = (uint32_t)rank;
                return out;
            }
        }
        break;
    }
    if (terrain->referencedCliffCount) {
        out.kind = BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF;
        out.referencedIndex = 0;
        return out;
    }
    out.kind = BZ_QUEST_WC3_TERRAIN_MATERIAL_GROUND;
    out.referencedIndex = 0;
    return out;
}

bzQuestWc3TerrainStatus_t bz_quest_wc3_terrain_surface_layers(
    const bzQuestWc3TerrainInput_t *terrain, uint32_t tileX, uint32_t tileZ,
    const bzQuestWc3TerrainCorner_t corners[4], bzQuestWc3TerrainSurfaceLayer_t *outLayers,
    uint32_t *outCount) {
    if (!terrain || !outLayers || !outCount) return BZ_QUEST_WC3_TERRAIN_ERR_INVALID_ARGUMENT;
    (void)tileX; (void)tileZ;
    *outCount = 0;
    if (terrain->referencedGroundCount == 0) return BZ_QUEST_WC3_TERRAIN_ERR_NO_GROUND_TEXTURES;
    uint32_t indices[4] = {
        safe_ground_index(&corners[0]), safe_ground_index(&corners[1]),
        safe_ground_index(&corners[2]), safe_ground_index(&corners[3]),
    };
    for (uint32_t position = 0; position < terrain->referencedGroundCount; position++) {
        const bzQuestWc3TerrainTextureRef_t *texture = &terrain->grounds[position];
        if (texture->width < 64 || texture->height < 64 || texture->width % 64 || texture->height % 64)
            continue;
        int tile = position == 0 ? 15 :
            (indices[2] >= texture->typeIndex ? 4 : 0) +
            (indices[3] >= texture->typeIndex ? 8 : 0) +
            (indices[1] >= texture->typeIndex ? 1 : 0) +
            (indices[0] >= texture->typeIndex ? 2 : 0);
        if (tile == 0) continue;
        float offsetX = 0.0f;
        if (tile == 15 && texture->width > texture->height) {
            if (corners[0].groundVariation <= 15) {
                tile = corners[0].groundVariation;
                offsetX = 0.5f;
            } else if (corners[0].groundVariation == 16) {
                tile = 15;
            } else {
                tile = 0;
            }
        }
        float u = 1.0f / (float)(texture->width / 64), v = 1.0f / (float)(texture->height / 64);
        float minU = u * (float)(tile % 4) + offsetX, maxU = minU + u;
        float minV = v * (float)(tile / 4), maxV = minV + v;
        float centerU = minU + u * 0.5f, centerV = minV + v * 0.5f;
        #define INSET(x, c) ((x) + ((c) - (x)) * 0.05f)
        bzQuestWc3TerrainSurfaceLayer_t *layer = &outLayers[*outCount];
        layer->referencedIndex = position;
        layer->uv[0][0] = INSET(minU, centerU); layer->uv[0][1] = INSET(maxV, centerV);
        layer->uv[1][0] = INSET(maxU, centerU); layer->uv[1][1] = INSET(maxV, centerV);
        layer->uv[2][0] = INSET(maxU, centerU); layer->uv[2][1] = INSET(minV, centerV);
        layer->uv[3][0] = INSET(minU, centerU); layer->uv[3][1] = INSET(minV, centerV);
        #undef INSET
        (*outCount)++;
        if (*outCount >= BZ_QUEST_WC3_TERRAIN_MAX_SURFACE_LAYERS_PER_CELL) break;
    }
    if (*outCount == 0) {
        int32_t rank = find_ground_rank(terrain, indices[0]);
        if (rank >= 0) {
            outLayers[0].referencedIndex = (uint32_t)rank;
            full_uv(outLayers[0].uv);
            *outCount = 1;
        }
    }
    return BZ_QUEST_WC3_TERRAIN_OK;
}

static void count_cell_quads(const bzQuestWc3TerrainInput_t *terrain, uint32_t x, uint32_t z,
                             uint32_t bucketQuadCounts[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES]) {
    const bzQuestWc3TerrainCorner_t *c00 = terrain_corner(terrain, x + 0, z + 0);
    const bzQuestWc3TerrainCorner_t *c10 = terrain_corner(terrain, x + 1, z + 0);
    const bzQuestWc3TerrainCorner_t *c11 = terrain_corner(terrain, x + 1, z + 1);
    const bzQuestWc3TerrainCorner_t *c01 = terrain_corner(terrain, x + 0, z + 1);
    bzQuestWc3TerrainCorner_t corners[4] = {*c00, *c10, *c11, *c01};
    bzQuestWc3TerrainSurfaceLayer_t layers[BZ_QUEST_WC3_TERRAIN_MAX_SURFACE_LAYERS_PER_CELL];
    uint32_t layerCount = 0;
    if (bz_quest_wc3_terrain_surface_layers(terrain, x, z, corners, layers, &layerCount) == BZ_QUEST_WC3_TERRAIN_OK)
        for (uint32_t i = 0; i < layerCount; i++) bucketQuadCounts[ground_bucket_index(layers[i].referencedIndex)]++;
    if (terrain->hasWater && bz_quest_wc3_terrain_tile_is_water(corners))
        bucketQuadCounts[water_bucket_index(terrain)]++;
    if (bz_quest_wc3_terrain_tile_is_cliff(corners)) {
        bzQuestWc3TerrainMaterialRef_t cliff = bz_quest_wc3_terrain_resolve_cliff_material(terrain, corners);
        uint32_t bucket = cliff.kind == BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF ?
            cliff_bucket_index(terrain, cliff.referencedIndex) : ground_bucket_index(cliff.referencedIndex);
        bucketQuadCounts[bucket] += 4;
    }
}

/* Replays the authoritative per-cell builder into one bucketed VB/IB. */
static void emit_cell_quads(const bzQuestWc3TerrainInput_t *terrain, uint32_t x, uint32_t z,
                            bzQuestWc3TerrainChunk_t *chunk,
                            const uint32_t bucketVertexStart[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES],
                            const uint32_t bucketIndexStart[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES],
                            uint32_t bucketVertexCursor[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES],
                            uint32_t bucketIndexCursor[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES]) {
    const bzQuestWc3TerrainCorner_t *c00 = terrain_corner(terrain, x + 0, z + 0);
    const bzQuestWc3TerrainCorner_t *c10 = terrain_corner(terrain, x + 1, z + 0);
    const bzQuestWc3TerrainCorner_t *c11 = terrain_corner(terrain, x + 1, z + 1);
    const bzQuestWc3TerrainCorner_t *c01 = terrain_corner(terrain, x + 0, z + 1);
    bzQuestWc3TerrainCorner_t corners[4] = {*c00, *c10, *c11, *c01};
    float h00 = c00->height, h10 = c10->height, h11 = c11->height, h01 = c01->height;
    float a[3], b[3], c[3], d[3], normal[3], color[4][4], uv[4][2];
    terrain_point(terrain, x + 0, z + 0, h00, a);
    terrain_point(terrain, x + 1, z + 0, h10, b);
    terrain_point(terrain, x + 1, z + 1, h11, c);
    terrain_point(terrain, x + 0, z + 1, h01, d);
    normalized_normal(h00, h10, h01, terrain->cellSize, normal);
    opaque_white(color);

    bzQuestWc3TerrainSurfaceLayer_t layers[BZ_QUEST_WC3_TERRAIN_MAX_SURFACE_LAYERS_PER_CELL];
    uint32_t layerCount = 0;
    if (bz_quest_wc3_terrain_surface_layers(terrain, x, z, corners, layers, &layerCount) == BZ_QUEST_WC3_TERRAIN_OK) {
        for (uint32_t i = 0; i < layerCount; i++) {
            uint32_t bucket = ground_bucket_index(layers[i].referencedIndex);
            emit_quad(chunk, bucket, bucketVertexStart[bucket], bucketIndexStart[bucket],
                      &bucketVertexCursor[bucket], &bucketIndexCursor[bucket], a, b, c, d, normal,
                      layers[i].uv, color);
        }
    }

    if (terrain->hasWater && bz_quest_wc3_terrain_tile_is_water(corners)) {
        float wa[3], wb[3], wc[3], wd[3], wuv[4][2], wcolor[4][4], up[3] = {0.0f, 1.0f, 0.0f};
        terrain_point(terrain, x + 0, z + 0, c00->waterHeight, wa);
        terrain_point(terrain, x + 1, z + 0, c10->waterHeight, wb);
        terrain_point(terrain, x + 1, z + 1, c11->waterHeight, wc);
        terrain_point(terrain, x + 0, z + 1, c01->waterHeight, wd);
        float u0 = (float)(x % 3) / 3.0f, u1 = (float)(x % 3 + 1) / 3.0f;
        float v0 = (float)(z % 3) / 3.0f, v1 = (float)(z % 3 + 1) / 3.0f;
        wuv[0][0] = u0; wuv[0][1] = v0;
        wuv[1][0] = u1; wuv[1][1] = v0;
        wuv[2][0] = u1; wuv[2][1] = v1;
        wuv[3][0] = u0; wuv[3][1] = v1;
        const bzQuestWc3TerrainCorner_t *src[4] = {c00, c10, c11, c01};
        for (uint32_t i = 0; i < 4; i++) {
            wcolor[i][0] = wcolor[i][1] = wcolor[i][2] = 1.0f;
            wcolor[i][3] = bz_quest_wc3_terrain_water_opacity(src[i]->rawWaterHeight, src[i]->rawHeight);
        }
        uint32_t bucket = water_bucket_index(terrain);
        emit_quad(chunk, bucket, bucketVertexStart[bucket], bucketIndexStart[bucket],
                  &bucketVertexCursor[bucket], &bucketIndexCursor[bucket], wa, wb, wc, wd, up, wuv,
                  wcolor);
    }

    if (!bz_quest_wc3_terrain_tile_is_cliff(corners)) return;
    bzQuestWc3TerrainMaterialRef_t cliff = bz_quest_wc3_terrain_resolve_cliff_material(terrain, corners);
    uint32_t bucket = cliff.kind == BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF ?
        cliff_bucket_index(terrain, cliff.referencedIndex) : ground_bucket_index(cliff.referencedIndex);
    float bottom = bz_quest_wc3_terrain_cliff_bottom(h00, h10, h11, h01, terrain->cellSize);
    float westA[3], westB[3], westC[3], westD[3], eastA[3], eastB[3], eastC[3], eastD[3];
    float southA[3], southB[3], southC[3], southD[3], northA[3], northB[3], northC[3], northD[3];
    float westN[3] = {-1.0f, 0.0f, 0.0f}, eastN[3] = {1.0f, 0.0f, 0.0f};
    float southN[3] = {0.0f, 0.0f, -1.0f}, northN[3] = {0.0f, 0.0f, 1.0f};
    full_uv(uv);
    terrain_point(terrain, x + 0, z + 0, bottom, westA);
    terrain_point(terrain, x + 0, z + 1, bottom, westB);
    terrain_point(terrain, x + 0, z + 1, h01, westC);
    terrain_point(terrain, x + 0, z + 0, h00, westD);
    emit_quad(chunk, bucket, bucketVertexStart[bucket], bucketIndexStart[bucket],
              &bucketVertexCursor[bucket], &bucketIndexCursor[bucket], westA, westB, westC, westD,
              westN, uv, color);
    terrain_point(terrain, x + 1, z + 1, bottom, eastA);
    terrain_point(terrain, x + 1, z + 0, bottom, eastB);
    terrain_point(terrain, x + 1, z + 0, h10, eastC);
    terrain_point(terrain, x + 1, z + 1, h11, eastD);
    emit_quad(chunk, bucket, bucketVertexStart[bucket], bucketIndexStart[bucket],
              &bucketVertexCursor[bucket], &bucketIndexCursor[bucket], eastA, eastB, eastC, eastD,
              eastN, uv, color);
    terrain_point(terrain, x + 1, z + 0, bottom, southA);
    terrain_point(terrain, x + 0, z + 0, bottom, southB);
    terrain_point(terrain, x + 0, z + 0, h00, southC);
    terrain_point(terrain, x + 1, z + 0, h10, southD);
    emit_quad(chunk, bucket, bucketVertexStart[bucket], bucketIndexStart[bucket],
              &bucketVertexCursor[bucket], &bucketIndexCursor[bucket], southA, southB, southC, southD,
              southN, uv, color);
    terrain_point(terrain, x + 0, z + 1, bottom, northA);
    terrain_point(terrain, x + 1, z + 1, bottom, northB);
    terrain_point(terrain, x + 1, z + 1, h11, northC);
    terrain_point(terrain, x + 0, z + 1, h01, northD);
    emit_quad(chunk, bucket, bucketVertexStart[bucket], bucketIndexStart[bucket],
              &bucketVertexCursor[bucket], &bucketIndexCursor[bucket], northA, northB, northC, northD,
              northN, uv, color);
}

bzQuestWc3TerrainStatus_t bz_quest_wc3_terrain_build_chunk(const bzQuestWc3TerrainInput_t *terrain,
                                                           uint32_t chunkX, uint32_t chunkZ,
                                                           bzQuestWc3TerrainChunk_t *outChunk) {
    if (!terrain || !outChunk) return BZ_QUEST_WC3_TERRAIN_ERR_INVALID_ARGUMENT;
    if (!terrain_dimensions_valid(terrain)) return BZ_QUEST_WC3_TERRAIN_ERR_INVALID_DIMENSIONS;
    if (terrain->referencedGroundCount == 0) return BZ_QUEST_WC3_TERRAIN_ERR_NO_GROUND_TEXTURES;
    bzQuestWc3TerrainChunkBounds_t bounds;
    if (!bz_quest_wc3_terrain_chunk_bounds(terrain->tileWidth, terrain->tileHeight, chunkX, chunkZ, &bounds))
        return BZ_QUEST_WC3_TERRAIN_ERR_CHUNK_OUT_OF_RANGE;
    memset(outChunk, 0, sizeof(*outChunk));
    strncpy(outChunk->meta.terrainIdentity, terrain->identity, sizeof(outChunk->meta.terrainIdentity) - 1);
    outChunk->meta.chunkX = chunkX;
    outChunk->meta.chunkZ = chunkZ;
    outChunk->meta.minTileX = bounds.minX;
    outChunk->meta.minTileZ = bounds.minZ;
    outChunk->meta.maxTileX = bounds.maxX;
    outChunk->meta.maxTileZ = bounds.maxZ;

    uint32_t bucketCount = terrain_bucket_count(terrain);
    uint32_t bucketQuadCounts[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES] = {0};
    uint32_t bucketVertexStart[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES] = {0};
    uint32_t bucketIndexStart[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES] = {0};
    uint32_t bucketVertexCursor[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES] = {0};
    uint32_t bucketIndexCursor[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES] = {0};

    for (uint32_t z = bounds.minZ; z < bounds.maxZ; z++)
        for (uint32_t x = bounds.minX; x < bounds.maxX; x++)
            count_cell_quads(terrain, x, z, bucketQuadCounts);

    uint32_t vertexTotal = 0, indexTotal = 0;
    for (uint32_t b = 0; b < bucketCount; b++) {
        uint32_t quadCount = bucketQuadCounts[b];
        bucketVertexStart[b] = vertexTotal;
        bucketIndexStart[b] = indexTotal;
        vertexTotal += quadCount * 4;
        indexTotal += quadCount * 6;
        if (quadCount == 0) continue;
        if (outChunk->meta.drawRangeCount >= BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES)
            return BZ_QUEST_WC3_TERRAIN_ERR_CAPACITY;
        bzQuestWc3TerrainDrawRange_t *range = &outChunk->meta.drawRanges[outChunk->meta.drawRangeCount++];
        range->materialKind = b < terrain->referencedGroundCount ? BZ_QUEST_WC3_TERRAIN_MATERIAL_GROUND :
            b < terrain->referencedGroundCount + terrain->referencedCliffCount ? BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF :
            BZ_QUEST_WC3_TERRAIN_MATERIAL_WATER;
        range->referencedIndex = range->materialKind == BZ_QUEST_WC3_TERRAIN_MATERIAL_GROUND ? b :
            range->materialKind == BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF ? b - terrain->referencedGroundCount : 0u;
        range->indexOffset = bucketIndexStart[b];
        range->indexCount = quadCount * 6;
    }
    if (vertexTotal > BZ_QUEST_WC3_TERRAIN_MAX_VERTS_PER_CHUNK ||
        indexTotal > BZ_QUEST_WC3_TERRAIN_MAX_INDICES_PER_CHUNK)
        return BZ_QUEST_WC3_TERRAIN_ERR_CAPACITY;

    for (uint32_t z = bounds.minZ; z < bounds.maxZ; z++)
        for (uint32_t x = bounds.minX; x < bounds.maxX; x++)
            emit_cell_quads(terrain, x, z, outChunk, bucketVertexStart, bucketIndexStart,
                            bucketVertexCursor, bucketIndexCursor);

    outChunk->meta.vertexCount = outChunk->meta.indexCount = 0;
    for (uint32_t b = 0; b < bucketCount; b++) {
        outChunk->meta.vertexCount += bucketVertexCursor[b];
        outChunk->meta.indexCount += bucketIndexCursor[b];
    }
    return BZ_QUEST_WC3_TERRAIN_OK;
}

const char *bz_quest_wc3_terrain_status_string(bzQuestWc3TerrainStatus_t status) {
    switch (status) {
        case BZ_QUEST_WC3_TERRAIN_OK: return "ok";
        case BZ_QUEST_WC3_TERRAIN_ERR_INVALID_ARGUMENT: return "invalid argument";
        case BZ_QUEST_WC3_TERRAIN_ERR_INVALID_DIMENSIONS: return "invalid dimensions";
        case BZ_QUEST_WC3_TERRAIN_ERR_INVALID_BOUNDS: return "invalid bounds";
        case BZ_QUEST_WC3_TERRAIN_ERR_NON_SQUARE_TILES: return "non-square tiles";
        case BZ_QUEST_WC3_TERRAIN_ERR_NO_GROUND_TEXTURES: return "no ground textures";
        case BZ_QUEST_WC3_TERRAIN_ERR_CHUNK_OUT_OF_RANGE: return "chunk out of range";
        case BZ_QUEST_WC3_TERRAIN_ERR_CAPACITY: return "capacity overflow";
        default: return "unknown";
    }
}

void bz_quest_wc3_terrain_chunk_key(const char *terrainIdentity, uint32_t chunkX, uint32_t chunkZ,
                                    char outKey[BZ_QUEST_WC3_TERRAIN_MAX_KEY]) {
    snprintf(outKey, BZ_QUEST_WC3_TERRAIN_MAX_KEY, "%s|%u|%u", terrainIdentity ? terrainIdentity : "", chunkX,
             chunkZ);
}

bool bz_quest_wc3_terrain_key_equal(const char *a, const char *b) {
    return strncmp(a ? a : "", b ? b : "", BZ_QUEST_WC3_TERRAIN_MAX_KEY) == 0;
}
