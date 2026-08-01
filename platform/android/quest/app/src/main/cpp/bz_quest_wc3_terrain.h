/*
 * bz_quest_wc3_terrain.h - layer 5B: platform-independent Warcraft III terrain
 * scale/chunk math, per-cell layer selection, and chunk mesh construction.
 *
 * Like bz_quest_wc3_render.h, every type/function here is plain C POD/math only:
 * no bzTTTerrain_t, bzTTAsset_t, VkBuffer, VkImage, or Android/OpenXR/Vulkan
 * type appears in this file. The one impure terrain capture unit copies the
 * terrain ABI into bzQuestWc3TerrainInput_t, then the Quest terrain Vulkan
 * module builds/upload chunks from that POD copy. This keeps the highest-risk
 * logic (scale/chunk/layer/UV/winding/cliff/water rules) host-testable with a
 * normal C compiler and no NDK/ABI provider.
 *
 * Evidence trail (authoritative, do not change without re-deriving):
 *
 * - Terrain scale validation and per-corner scaled heights:
 *   WarcraftAssetAdapter.swift:560-719.
 *   spanX = max_x - min_x, spanZ = max_y - min_y, tile sizes must match within
 *   0.1%, scale = 1.08 / max(spanX, spanZ), cellSize = tileSizeX * scale,
 *   scaled heights = raw corner.height * scale. The ABI's min_y/max_y describe
 *   the terrain plane's second axis (world Z here), not vertical height.
 * - Terrain corners are NOT axis-swapped like entities/models:
 *   LiveTabletopTransport.swift:561-611 copies (x,z,height) directly from the
 *   terrain grid; only entity/model geometry does the Z-up -> Y-up swap.
 * - Chunking and point() centering:
 *   WarcraftRenderMath.swift:297-346. chunkSide = 32; each chunk covers
 *   [x, min(x+32,width)) x [z, min(z+32,height)) in TILE space; point(x,z,h) =
 *   (x*cellSize - width*cellSize*0.5, h, z*cellSize - height*cellSize*0.5).
 * - Ground/water/cliff quad topology, winding fix, cliff bottom, and normals:
 *   WarcraftRenderMath.swift:347-420. Every quad appends vertices in a,b,c,d
 *   order, then chooses indices [0,1,2,0,2,3] vs [0,2,1,0,3,2] from
 *   dot(cross(b-a,c-a), normal) >= 0. Ground normal = normalize({h00-h10,
 *   max(cellSize,0.0001), h00-h01}); cliff bottom = min(h00,h10,h11,h01) -
 *   cellSize; water uses per-corner alpha in vertex color.
 * - Cell feature derivation, IsTileWater map-edge rejection, cliff material
 *   resolution, and surface-layer UV selection:
 *   WarcraftAssetAdapter.swift:656-792. Ground layers are the referenced ground
 *   type_index values sorted ascending; position 0 is always the opaque base
 *   layer (tile 15), higher layers use the exact bit mask
 *   (idx11>=layer?4:0)+(idx01>=layer?8:0)+(idx10>=layer?1:0)+(idx00>=layer?2:0).
 *   Wide atlases use corner[0].groundVariation (0..15 => tile+offsetX 0.5,
 *   16 => tile 15, else tile 0). UVs are inset 5% toward the tile center.
 *
 * Bounds/cap reasoning:
 * - Warcraft III's practical W3E map ceiling is the editor's "Huge" 256x256
 *   tile grid, so the fixed caps below size corner/chunk arrays for that real
 *   format ceiling, not an unbounded theoretical future map size. A larger
 *   terrain is rejected, never silently truncated.
 * - The W3E terrain type tables cap ground/cliff IDs at 16 each; a cell's
 *   emitted surface-layer count is therefore bounded by the referenced ground
 *   count. Worst case, a corner reaches the highest referenced type_index and
 *   the cell emits one base layer plus every higher splat rank up to it, so
 *   BZ_QUEST_WC3_TERRAIN_MAX_SURFACE_LAYERS_PER_CELL must equal the full
 *   referenced-ground ceiling (16), not a smaller aesthetic guess like 4.
 */
#ifndef BZ_QUEST_WC3_TERRAIN_H
#define BZ_QUEST_WC3_TERRAIN_H

#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_wc3_render.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_WC3_TERRAIN_CHUNK_TILES = 32,
    BZ_QUEST_WC3_TERRAIN_MAX_TILES = 256,
    BZ_QUEST_WC3_TERRAIN_MAX_CORNERS_PER_AXIS = BZ_QUEST_WC3_TERRAIN_MAX_TILES + 1,
    BZ_QUEST_WC3_TERRAIN_MAX_CORNERS =
        BZ_QUEST_WC3_TERRAIN_MAX_CORNERS_PER_AXIS * BZ_QUEST_WC3_TERRAIN_MAX_CORNERS_PER_AXIS,
    BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS_PER_AXIS =
        (BZ_QUEST_WC3_TERRAIN_MAX_TILES + BZ_QUEST_WC3_TERRAIN_CHUNK_TILES - 1) /
        BZ_QUEST_WC3_TERRAIN_CHUNK_TILES,
    BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS =
        BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS_PER_AXIS * BZ_QUEST_WC3_TERRAIN_MAX_CHUNKS_PER_AXIS,
    BZ_QUEST_WC3_TERRAIN_MAX_GROUND_TYPES = 16,
    BZ_QUEST_WC3_TERRAIN_MAX_CLIFF_TYPES = 16,
    BZ_QUEST_WC3_TERRAIN_MAX_SURFACE_LAYERS_PER_CELL = BZ_QUEST_WC3_TERRAIN_MAX_GROUND_TYPES,
    BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES =
        BZ_QUEST_WC3_TERRAIN_MAX_GROUND_TYPES + BZ_QUEST_WC3_TERRAIN_MAX_CLIFF_TYPES + 1,
    BZ_QUEST_WC3_TERRAIN_MAX_QUADS_PER_CELL =
        BZ_QUEST_WC3_TERRAIN_MAX_SURFACE_LAYERS_PER_CELL + 4 + 1,
    BZ_QUEST_WC3_TERRAIN_MAX_CHUNK_CELLS =
        BZ_QUEST_WC3_TERRAIN_CHUNK_TILES * BZ_QUEST_WC3_TERRAIN_CHUNK_TILES,
    BZ_QUEST_WC3_TERRAIN_MAX_QUADS_PER_CHUNK =
        BZ_QUEST_WC3_TERRAIN_MAX_CHUNK_CELLS * BZ_QUEST_WC3_TERRAIN_MAX_QUADS_PER_CELL,
    BZ_QUEST_WC3_TERRAIN_MAX_VERTS_PER_CHUNK = BZ_QUEST_WC3_TERRAIN_MAX_QUADS_PER_CHUNK * 4,
    BZ_QUEST_WC3_TERRAIN_MAX_INDICES_PER_CHUNK = BZ_QUEST_WC3_TERRAIN_MAX_QUADS_PER_CHUNK * 6,
    BZ_QUEST_WC3_TERRAIN_MAX_KEY = BZ_QUEST_WC3_MAX_IDENTITY,
    BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_DIM = 2048,
    BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_BYTES =
        BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_DIM * BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_DIM * 4,
};

enum {
    BZ_QUEST_WC3_TERRAIN_MAP_EDGE = 1u << 0,
    BZ_QUEST_WC3_TERRAIN_RAMP = 1u << 1,
    BZ_QUEST_WC3_TERRAIN_BLIGHT = 1u << 2,
    BZ_QUEST_WC3_TERRAIN_WATER = 1u << 3,
    BZ_QUEST_WC3_TERRAIN_BOUNDARY = 1u << 4,
    BZ_QUEST_WC3_TERRAIN_NO_CLIFF = 1u << 5,
};

typedef enum {
    BZ_QUEST_WC3_TERRAIN_MATERIAL_GROUND = 1,
    BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF = 2,
    BZ_QUEST_WC3_TERRAIN_MATERIAL_WATER = 3,
} bzQuestWc3TerrainMaterialKind_t;

typedef enum {
    BZ_QUEST_WC3_TERRAIN_OK = 0,
    BZ_QUEST_WC3_TERRAIN_ERR_INVALID_ARGUMENT,
    BZ_QUEST_WC3_TERRAIN_ERR_INVALID_DIMENSIONS,
    BZ_QUEST_WC3_TERRAIN_ERR_INVALID_BOUNDS,
    BZ_QUEST_WC3_TERRAIN_ERR_NON_SQUARE_TILES,
    BZ_QUEST_WC3_TERRAIN_ERR_NO_GROUND_TEXTURES,
    BZ_QUEST_WC3_TERRAIN_ERR_CHUNK_OUT_OF_RANGE,
    BZ_QUEST_WC3_TERRAIN_ERR_CAPACITY,
} bzQuestWc3TerrainStatus_t;

typedef struct {
    float minX, minZ, maxX, maxZ;
} bzQuestWc3TerrainBounds_t;

typedef struct {
    float scale;
    float cellSize;
} bzQuestWc3TerrainMetrics_t;

typedef struct {
    float rawHeight;
    float rawWaterHeight;
    float height;
    float waterHeight;
    int32_t groundTypeIndex;
    int32_t cliffTypeIndex;
    uint8_t groundVariation;
    uint8_t cliffVariation;
    uint8_t cliffLevel;
    uint8_t flags;
} bzQuestWc3TerrainCorner_t;

typedef struct {
    char identity[BZ_QUEST_WC3_MAX_IDENTITY];
    uint32_t typeIndex;
    uint32_t typeId;
    uint32_t cornerCount;
    uint32_t width, height;
} bzQuestWc3TerrainTextureRef_t;

typedef struct {
    uint32_t referencedIndex;
    float uv[4][2];
} bzQuestWc3TerrainSurfaceLayer_t;

typedef struct {
    bzQuestWc3TerrainMaterialKind_t kind;
    uint32_t referencedIndex;
} bzQuestWc3TerrainMaterialRef_t;

typedef struct {
    char identity[BZ_QUEST_WC3_MAX_IDENTITY];
    uint32_t cornerWidth, cornerHeight;
    uint32_t tileWidth, tileHeight;
    uint32_t chunkCountX, chunkCountZ;
    float cellSize;
    bzQuestWc3TerrainBounds_t bounds;
    uint32_t groundTypeCount, cliffTypeCount;
    uint32_t referencedGroundCount, referencedCliffCount;
    bool hasWater;
    bzQuestWc3TerrainTextureRef_t grounds[BZ_QUEST_WC3_TERRAIN_MAX_GROUND_TYPES];
    bzQuestWc3TerrainTextureRef_t cliffs[BZ_QUEST_WC3_TERRAIN_MAX_CLIFF_TYPES];
    bzQuestWc3TerrainTextureRef_t water;
    /* ~2MB at the documented 256x256-tile ceiling; never stack-allocate this. */
    bzQuestWc3TerrainCorner_t corners[BZ_QUEST_WC3_TERRAIN_MAX_CORNERS];
} bzQuestWc3TerrainInput_t;

typedef struct {
    float position[3];
    float uv[2];
    float color[4];
} bzQuestWc3TerrainVertex_t;

typedef struct {
    bzQuestWc3TerrainMaterialKind_t materialKind;
    uint32_t referencedIndex;
    uint32_t indexOffset, indexCount;
} bzQuestWc3TerrainDrawRange_t;

typedef struct {
    char terrainIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
    uint32_t chunkX, chunkZ;
    uint32_t minTileX, minTileZ, maxTileX, maxTileZ;
    uint32_t vertexCount, indexCount;
    uint32_t drawRangeCount;
    bzQuestWc3TerrainDrawRange_t drawRanges[BZ_QUEST_WC3_TERRAIN_MAX_DRAW_RANGES];
} bzQuestWc3TerrainChunkMeta_t;

typedef struct {
    bzQuestWc3TerrainChunkMeta_t meta;
    /* ~3.6MB at the fixed chunk ceiling; never stack-allocate this. */
    bzQuestWc3TerrainVertex_t vertices[BZ_QUEST_WC3_TERRAIN_MAX_VERTS_PER_CHUNK];
    uint32_t indices[BZ_QUEST_WC3_TERRAIN_MAX_INDICES_PER_CHUNK];
} bzQuestWc3TerrainChunk_t;

typedef struct {
    uint32_t minX, minZ, maxX, maxZ;
} bzQuestWc3TerrainChunkBounds_t;

bzQuestWc3TerrainStatus_t bz_quest_wc3_terrain_measure(const bzQuestWc3TerrainBounds_t *bounds,
                                                       uint32_t tileWidth, uint32_t tileHeight,
                                                       bzQuestWc3TerrainMetrics_t *out);
void bz_quest_wc3_terrain_chunk_grid(uint32_t tileWidth, uint32_t tileHeight,
                                     uint32_t *outChunkCountX, uint32_t *outChunkCountZ);
bool bz_quest_wc3_terrain_chunk_bounds(uint32_t tileWidth, uint32_t tileHeight, uint32_t chunkX,
                                       uint32_t chunkZ, bzQuestWc3TerrainChunkBounds_t *out);
float bz_quest_wc3_terrain_water_opacity(float rawWaterHeight, float rawHeight);
bool bz_quest_wc3_terrain_quad_uses_forward_winding(const float a[3], const float b[3], const float c[3],
                                                    const float normal[3]);
bool bz_quest_wc3_terrain_tile_is_water(const bzQuestWc3TerrainCorner_t corners[4]);
bool bz_quest_wc3_terrain_tile_is_cliff(const bzQuestWc3TerrainCorner_t corners[4]);
float bz_quest_wc3_terrain_cliff_bottom(float h00, float h10, float h11, float h01, float cellSize);
bzQuestWc3TerrainMaterialRef_t bz_quest_wc3_terrain_resolve_cliff_material(
    const bzQuestWc3TerrainInput_t *terrain, const bzQuestWc3TerrainCorner_t corners[4]);
bzQuestWc3TerrainStatus_t bz_quest_wc3_terrain_surface_layers(
    const bzQuestWc3TerrainInput_t *terrain, uint32_t tileX, uint32_t tileZ,
    const bzQuestWc3TerrainCorner_t corners[4], bzQuestWc3TerrainSurfaceLayer_t *outLayers,
    uint32_t *outCount);
bzQuestWc3TerrainStatus_t bz_quest_wc3_terrain_build_chunk(const bzQuestWc3TerrainInput_t *terrain,
                                                           uint32_t chunkX, uint32_t chunkZ,
                                                           bzQuestWc3TerrainChunk_t *outChunk);
const char *bz_quest_wc3_terrain_status_string(bzQuestWc3TerrainStatus_t status);
void bz_quest_wc3_terrain_chunk_key(const char *terrainIdentity, uint32_t chunkX, uint32_t chunkZ,
                                    char outKey[BZ_QUEST_WC3_TERRAIN_MAX_KEY]);
bool bz_quest_wc3_terrain_key_equal(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_TERRAIN_H */
