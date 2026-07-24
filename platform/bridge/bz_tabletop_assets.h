/*
 * bz_tabletop_assets.h - immutable Warcraft asset and terrain export ABI.
 *
 * This is intentionally separate from bz_tabletop_transport.h. It is plain C,
 * versioned independently, and exposes no engine, renderer, SDL, OpenGL,
 * Objective-C, Swift, or RealityKit types.
 */
#ifndef BZ_TABLETOP_ASSETS_H
#define BZ_TABLETOP_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BZ_TABLETOP_ASSETS_ABI_VERSION 1u

enum {
    BZ_TTA_MAX_IDENTITY = 260, /* MDX texture records carry a fixed 260-byte path */
    BZ_TTA_MAX_SEQUENCE_NAME = 80,
    BZ_TTA_MAX_NODE_NAME = 80,
    BZ_TTA_TERRAIN_CHUNK_TILES = 32,
    BZ_TTA_TEAM_COLOR_NONE = UINT32_MAX,
};

typedef struct bzTTSnapshot bzTTSnapshot_t;
typedef struct bzTTAsset bzTTAsset_t;
typedef struct bzTTTerrain bzTTTerrain_t;

typedef enum {
    BZ_TTA_OK = 0,
    BZ_TTA_ERR_NOT_INITIALIZED,
    BZ_TTA_ERR_TERMINAL,
    BZ_TTA_ERR_ABI_VERSION,
    BZ_TTA_ERR_INVALID_ARGUMENT,
    BZ_TTA_ERR_PATH_CONFINEMENT,
    BZ_TTA_ERR_NOT_FOUND,
    BZ_TTA_ERR_MALFORMED,
    BZ_TTA_ERR_UNSUPPORTED,
    BZ_TTA_ERR_OUT_OF_MEMORY,
} bzTTAResult_t;

typedef enum {
    BZ_TTA_ASSET_IMAGE = 1,
    BZ_TTA_ASSET_MODEL = 2,
} bzTTAssetKind_t;

typedef enum {
    BZ_TTA_CATEGORY_UNKNOWN = 0,
    BZ_TTA_CATEGORY_MOBILE,
    BZ_TTA_CATEGORY_BUILDING,
    BZ_TTA_CATEGORY_RESOURCE,
    BZ_TTA_CATEGORY_DOODAD,
    BZ_TTA_CATEGORY_DESTRUCTABLE,
} bzTTAssetCategory_t;

typedef enum {
    BZ_TTA_TERRAIN_TEXTURE_GROUND = 1,
    BZ_TTA_TERRAIN_TEXTURE_CLIFF = 2,
} bzTTTerrainTextureKind_t;

enum {
    BZ_TTA_METADATA_OVERRIDE_TEAM_COLOR = 1u << 0,
    BZ_TTA_METADATA_OVERRIDE_TINT = 1u << 1,
};

typedef enum {
    BZ_TTA_PIXEL_RGBA8 = 1,
} bzTTPixelFormat_t;

typedef enum {
    BZ_TTA_ORIGIN_TOP_LEFT = 1,
    BZ_TTA_ORIGIN_BOTTOM_LEFT = 2,
} bzTTImageOrigin_t;

typedef enum {
    BZ_TTA_BLEND_OPAQUE = 0,
    BZ_TTA_BLEND_TRANSPARENT = 1,
    BZ_TTA_BLEND_ALPHA = 2,
    BZ_TTA_BLEND_ADDITIVE = 3,
    BZ_TTA_BLEND_ADD_ALPHA = 4,
    BZ_TTA_BLEND_MODULATE = 5,
    BZ_TTA_BLEND_MODULATE_2X = 6,
} bzTTBlendMode_t;

typedef struct {
    float x, y;
} bzTTVec2_t;

typedef struct {
    float x, y, z;
} bzTTVec3_t;

typedef struct {
    bzTTVec3_t min;
    bzTTVec3_t max;
    float radius;
} bzTTBounds3_t;

typedef struct {
    uint32_t category; /* bzTTAssetCategory_t */
    uint32_t class_id;
    uint32_t team_color;
    float tint_r, tint_g, tint_b, tint_a;
    float footprint_x, footprint_y;
} bzTTAssetMetadata_t;

typedef struct {
    uint32_t class_id;
    uint32_t override_mask;
    uint32_t team_color;
    float tint_r, tint_g, tint_b, tint_a;
} bzTTEntityMetadataInput_t;

typedef struct {
    uint32_t width, height;
    uint32_t row_bytes;
    uint32_t data_bytes;
    uint32_t format; /* bzTTPixelFormat_t */
    uint32_t origin; /* bzTTImageOrigin_t */
} bzTTImageInfo_t;

typedef struct {
    uint32_t version;
    uint32_t geoset_count;
    uint32_t material_count;
    uint32_t layer_count;
    uint32_t texture_count;
    uint32_t sequence_count;
    uint32_t node_count;
    bzTTBounds3_t bounds;
} bzTTModelInfo_t;

typedef struct {
    uint32_t vertex_count;
    uint32_t normal_count;
    uint32_t uv_count;
    uint32_t index_count;
    uint32_t material_index;
    uint32_t vertex_group_count;
    bzTTBounds3_t bounds;
} bzTTGeosetInfo_t;

typedef struct {
    int32_t priority;
    uint32_t flags;
    uint32_t first_layer;
    uint32_t layer_count;
} bzTTMaterialInfo_t;

typedef struct {
    uint32_t blend_mode; /* bzTTBlendMode_t */
    uint32_t flags;
    uint32_t texture_index;
    int32_t transform_index;
    int32_t uv_channel;
    float alpha;
} bzTTMaterialLayerInfo_t;

typedef struct {
    uint32_t replaceable_id;
    uint32_t wrapping_flags;
    char identity[BZ_TTA_MAX_IDENTITY];
} bzTTModelTextureInfo_t;

typedef struct {
    char name[BZ_TTA_MAX_SEQUENCE_NAME];
    uint32_t start_msec, end_msec;
    float move_speed;
    uint32_t flags;
    float rarity;
    int32_t sync_point;
    bzTTBounds3_t bounds;
} bzTTSequenceInfo_t;

typedef struct {
    char name[BZ_TTA_MAX_NODE_NAME];
    uint32_t object_id;
    uint32_t parent_id;
    uint32_t flags;
    bzTTVec3_t pivot;
    bzTTVec3_t initial_translation;
    float initial_rotation_x, initial_rotation_y, initial_rotation_z, initial_rotation_w;
    bzTTVec3_t initial_scale;
} bzTTNodeInfo_t;

typedef struct {
    uint32_t width, height;               /* corner grid dimensions */
    uint32_t tile_width, tile_height;     /* width-1, height-1 */
    uint32_t chunk_tiles;
    uint32_t chunk_count_x, chunk_count_y;
    float min_x, min_y, max_x, max_y;
    uint32_t ground_type_count, cliff_type_count;
} bzTTTerrainInfo_t;

typedef struct {
    float height;
    float water_height;
    uint32_t ground_id;
    uint32_t cliff_id;
    uint8_t ground_variation;
    uint8_t cliff_variation;
    uint8_t cliff_level;
    uint8_t flags;
} bzTTTerrainCorner_t;

typedef struct {
    uint32_t type_index;
    uint32_t type_id;
    uint32_t corner_count;
} bzTTTerrainTextureInfo_t;

enum {
    BZ_TTA_TERRAIN_MAP_EDGE = 1u << 0,
    BZ_TTA_TERRAIN_RAMP = 1u << 1,
    BZ_TTA_TERRAIN_BLIGHT = 1u << 2,
    BZ_TTA_TERRAIN_WATER = 1u << 3,
    BZ_TTA_TERRAIN_BOUNDARY = 1u << 4,
    BZ_TTA_TERRAIN_NO_CLIFF = 1u << 5,
};

void BZ_TTA_Init(void);
void BZ_TTA_Shutdown(void);
uint32_t BZ_TTA_AbiVersion(void);

/* Registers the configstring identity at cs_index through the existing
 * filesystem/MPQ search order. A valid lifecycle call returns a retained typed
 * descriptor; missing/malformed assets return a cached placeholder. */
const bzTTAsset_t *BZ_TTA_RegisterConfigString(uint32_t abi_version,
                                               const bzTTSnapshot_t *snapshot,
                                               uint32_t cs_index,
                                               bzTTAssetKind_t kind,
                                               const bzTTAssetMetadata_t *metadata);
/* Resolves an MDX TEXS identity through the same confined filesystem/MPQ path. */
const bzTTAsset_t *BZ_TTA_RegisterModelTexture(uint32_t abi_version,
                                              const bzTTAsset_t *model,
                                              uint32_t texture_index);
/* Resolves a referenced W3E terrain type through Terrain.slk or CliffTypes.slk. Cliff
 * resolution includes the authoritative map-tileset candidate and generic fallback.
 * Zero-reference table entries are not registrations and return NULL without lookup. */
const bzTTAsset_t *BZ_TTA_RegisterTerrainTexture(uint32_t abi_version,
                                                const bzTTTerrain_t *terrain,
                                                bzTTTerrainTextureKind_t kind,
                                                uint32_t type_index);
/* Resolves immutable class data without server-edict access. Runtime values replace
 * table defaults only when their corresponding override bit is present. */
bzTTAResult_t BZ_TTA_ResolveEntityMetadata(uint32_t abi_version,
                                           const bzTTEntityMetadataInput_t *input,
                                           bzTTAssetMetadata_t *out);
void BZ_TTAsset_Retain(const bzTTAsset_t *asset);
void BZ_TTAsset_Release(const bzTTAsset_t *asset);
bool BZ_TTAsset_IsPlaceholder(const bzTTAsset_t *asset);
bzTTAssetKind_t BZ_TTAsset_Kind(const bzTTAsset_t *asset);
bzTTAResult_t BZ_TTAsset_Status(const bzTTAsset_t *asset);
bool BZ_TTAsset_Identity(const bzTTAsset_t *asset, char *out, size_t cap);
bool BZ_TTAsset_Metadata(const bzTTAsset_t *asset, bzTTAssetMetadata_t *out);

bool BZ_TTAsset_ImageInfo(const bzTTAsset_t *asset, bzTTImageInfo_t *out);
uint32_t BZ_TTAsset_CopyImagePixels(const bzTTAsset_t *asset, void *dst, uint32_t cap);

bool BZ_TTAsset_ModelInfo(const bzTTAsset_t *asset, bzTTModelInfo_t *out);
bool BZ_TTAsset_GeosetInfo(const bzTTAsset_t *asset, uint32_t index, bzTTGeosetInfo_t *out);
uint32_t BZ_TTAsset_CopyGeosetVertices(const bzTTAsset_t *asset, uint32_t index,
                                       bzTTVec3_t *dst, uint32_t cap);
uint32_t BZ_TTAsset_CopyGeosetNormals(const bzTTAsset_t *asset, uint32_t index,
                                      bzTTVec3_t *dst, uint32_t cap);
uint32_t BZ_TTAsset_CopyGeosetUVs(const bzTTAsset_t *asset, uint32_t index,
                                  bzTTVec2_t *dst, uint32_t cap);
uint32_t BZ_TTAsset_CopyGeosetIndices(const bzTTAsset_t *asset, uint32_t index,
                                      uint16_t *dst, uint32_t cap);
bool BZ_TTAsset_MaterialInfo(const bzTTAsset_t *asset, uint32_t index, bzTTMaterialInfo_t *out);
bool BZ_TTAsset_MaterialLayerInfo(const bzTTAsset_t *asset, uint32_t index,
                                  bzTTMaterialLayerInfo_t *out);
bool BZ_TTAsset_ModelTextureInfo(const bzTTAsset_t *asset, uint32_t index,
                                 bzTTModelTextureInfo_t *out);
bool BZ_TTAsset_SequenceInfo(const bzTTAsset_t *asset, uint32_t index, bzTTSequenceInfo_t *out);
bool BZ_TTAsset_NodeInfo(const bzTTAsset_t *asset, uint32_t index, bzTTNodeInfo_t *out);

/* Called on the engine thread after authoritative map state is consistent. */
void BZ_TTA_PublishTerrainFromGame(void);
const bzTTTerrain_t *BZ_TTA_LatestTerrain(void);
void BZ_TTTerrain_Retain(const bzTTTerrain_t *terrain);
void BZ_TTTerrain_Release(const bzTTTerrain_t *terrain);
bool BZ_TTTerrain_Info(const bzTTTerrain_t *terrain, bzTTTerrainInfo_t *out);
bool BZ_TTTerrain_Corner(const bzTTTerrain_t *terrain, uint32_t x, uint32_t y,
                         bzTTTerrainCorner_t *out);
bool BZ_TTTerrain_GroundType(const bzTTTerrain_t *terrain, uint32_t index, uint32_t *out);
bool BZ_TTTerrain_CliffType(const bzTTTerrain_t *terrain, uint32_t index, uint32_t *out);
/* Returns the dense registration list while preserving each W3E table index for corners. */
uint32_t BZ_TTTerrain_ReferencedTextureCount(const bzTTTerrain_t *terrain,
                                             bzTTTerrainTextureKind_t kind);
bool BZ_TTTerrain_ReferencedTexture(const bzTTTerrain_t *terrain,
                                    bzTTTerrainTextureKind_t kind,
                                    uint32_t reference_index,
                                    bzTTTerrainTextureInfo_t *out);

/* Stable counters for tests and host diagnostics. */
uint64_t BZ_TTA_CacheHits(void);
uint64_t BZ_TTA_CacheMisses(void);
uint64_t BZ_TTA_PlaceholderLogs(void);
uint64_t BZ_TTA_MetadataLogs(void);

#ifdef __cplusplus
}
#endif

#endif
