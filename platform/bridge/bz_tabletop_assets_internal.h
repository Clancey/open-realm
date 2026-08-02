#ifndef BZ_TABLETOP_ASSETS_INTERNAL_H
#define BZ_TABLETOP_ASSETS_INTERNAL_H

#include "bz_tabletop_assets.h"

/* A track's keys live at keys_offset as an array of bzTTVec3Key_t/bzTTQuatKey_t/bzTTFloatKey_t
 * (element type is implied by which channel/accessor reads it); key_count 0 means "no track". */
typedef struct {
    uint32_t key_count;
    uint32_t interp;
    uint32_t global_sequence;
    uint32_t keys_offset;
} bzTTTrackRecord_t;

typedef struct {
    bzTTGeosetInfo_t info;
    uint32_t vertices_offset, normals_offset, uvs_offset, indices_offset;
    uint32_t skin_offset; /* array of bzTTVertexSkin_t, count == info.vertex_count (always present) */
    uint32_t palette_offset; /* array of uint32_t node indices, count == info.matrix_palette_count */
    bzTTGeosetAnimInfo_t anim;
    uint32_t alpha_keys_offset; /* array of bzTTFloatKey_t, count == anim.alpha_track.key_count */
} bzTTGeosetRecord_t;

typedef struct {
    bzTTNodeInfo_t info;
    bzTTTrackRecord_t translation, rotation, scale;
} bzTTNodeRecord_t;

/* PRE2 particle emitter (ABI v4). Each of the 8 tracks is a plain float track (see
 * bzTTEmitterChannel_t) - one bzTTTrackRecord_t per channel, in bzTTEmitterChannel_t's own
 * declared order, mirrored by node_emitter_track_record()'s switch in bz_tabletop_assets.c. */
typedef struct {
    bzTTParticleEmitterInfo_t info;
    bzTTTrackRecord_t visibility, emission_rate, width, length, speed, latitude, gravity, variation;
} bzTTEmitterRecord_t;

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
            uint32_t nodes_offset; /* array of bzTTNodeRecord_t (not bzTTNodeInfo_t directly) */
            uint32_t global_sequences_offset; /* array of uint32_t durations, count = info.global_sequence_count */
            uint32_t emitters_offset; /* array of bzTTEmitterRecord_t, count = info.emitter_count (ABI v4) */
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
    bzTTTeamTextureKind_t kind;
    uint32_t team_color;
    char *identity;
    size_t cap;
} bzTTTeamTextureResolve_t;

typedef struct {
    bool (*path_is_confined)(const char *identity);
    bzTTAsset_t *(*load_asset)(const char *identity, bzTTAssetKind_t kind,
                               const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status);
    uintptr_t (*terrain_token)(void);
    bzTTTerrain_t *(*copy_terrain)(uintptr_t *source_token, bzTTAResult_t *status);
    bzTTAResult_t (*resolve_terrain_identity)(bzTTTerrainTextureKind_t kind, uint32_t type_id,
                                              uint8_t tileset, char *identity, size_t cap);
    uint32_t (*team_texture_count)(bzTTTeamTextureKind_t kind);
    bzTTAResult_t (*resolve_team_texture_identity)(const bzTTTeamTextureResolve_t *resolve);
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
