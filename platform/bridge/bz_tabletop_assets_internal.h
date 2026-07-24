#ifndef BZ_TABLETOP_ASSETS_INTERNAL_H
#define BZ_TABLETOP_ASSETS_INTERNAL_H

#include "bz_tabletop_assets.h"

typedef struct {
    bzTTGeosetInfo_t info;
    uint32_t vertices_offset, normals_offset, uvs_offset, indices_offset;
} bzTTGeosetRecord_t;

typedef struct {
    uint32_t id;
    uint32_t corner_count;
} bzTTTerrainTypeRecord_t;

struct bzTTAsset {
    int refcount;
    bzTTAssetKind_t kind;
    bzTTAResult_t status;
    bool placeholder;
    char identity[BZ_TTA_MAX_IDENTITY];
    char cache_identity[BZ_TTA_MAX_IDENTITY];
    bzTTAssetMetadata_t metadata;
    union {
        struct {
            bzTTImageInfo_t info;
            uint32_t pixels_offset;
        } image;
        struct {
            bzTTModelInfo_t info;
            uint32_t geosets_offset;
            uint32_t materials_offset;
            uint32_t layers_offset;
            uint32_t textures_offset;
            uint32_t sequences_offset;
            uint32_t nodes_offset;
        } model;
    } u;
    size_t allocation_size;
    struct bzTTAsset *cache_next;
    unsigned char data[];
};

struct bzTTTerrain {
    int refcount;
    uintptr_t source_token;
    uint64_t generation;
    bzTTTerrainInfo_t info;
    uint32_t corners_offset, grounds_offset, cliffs_offset;
    uint32_t water_corner_count, water_tile_count;
    uint8_t tileset;
    size_t allocation_size;
    unsigned char data[];
};

typedef struct {
    bool (*path_is_confined)(const char *identity);
    bzTTAsset_t *(*load_asset)(const char *identity, bzTTAssetKind_t kind,
                               const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status);
    uintptr_t (*terrain_token)(void);
    bzTTTerrain_t *(*copy_terrain)(uintptr_t *source_token, bzTTAResult_t *status);
    bzTTAResult_t (*resolve_terrain_identity)(bzTTTerrainTextureKind_t kind, uint32_t type_id,
                                              uint8_t tileset, char *identity, size_t cap);
    uintptr_t (*metadata_token)(void);
    bzTTAResult_t (*resolve_entity_metadata)(uint32_t class_id, bzTTAssetMetadata_t *metadata);
} bzTTAssetSource_t;

bzTTAsset_t *BZ_TTA_AssetAlloc(size_t payload_bytes, bzTTAssetKind_t kind,
                                const char *identity, const bzTTAssetMetadata_t *metadata);
bzTTTerrain_t *BZ_TTA_TerrainAlloc(size_t payload_bytes);
void *BZ_TTA_AssetData(bzTTAsset_t *asset, uint32_t offset, size_t bytes);
void *BZ_TTA_TerrainData(bzTTTerrain_t *terrain, uint32_t offset, size_t bytes);

/* Installed by the selected game's static archive. */
void BZ_WC3_TTA_Source(bzTTAssetSource_t *source);

#endif
