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

#define BZ_TABLETOP_ASSETS_ABI_VERSION 4u

enum {
    BZ_TTA_MAX_IDENTITY = 260, /* MDX texture records carry a fixed 260-byte path */
    BZ_TTA_MAX_SEQUENCE_NAME = 80,
    BZ_TTA_MAX_NODE_NAME = 80,
    BZ_TTA_TERRAIN_CHUNK_TILES = 32,
    BZ_TTA_TEAM_COLOR_NONE = UINT32_MAX,
    BZ_TTA_NO_GLOBAL_SEQUENCE = UINT32_MAX, /* bzTTTrackInfo_t.global_sequence: track is keyed by entity/sequence time */
    BZ_TTA_MAX_VERTEX_SKIN_BONES = 4, /* matches classic MDX MAX_SKIN_BONES */
    BZ_TTA_MAX_MATRIX_PALETTE = 128, /* matches classic MDX MDX_MATRIX_PALETTE (per-geoset bone cap) */
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
    BZ_TTA_CATEGORY_ITEM,
} bzTTAssetCategory_t;

typedef enum {
    BZ_TTA_TERRAIN_TEXTURE_GROUND = 1,
    BZ_TTA_TERRAIN_TEXTURE_CLIFF = 2,
    BZ_TTA_TERRAIN_TEXTURE_WATER = 3,
} bzTTTerrainTextureKind_t;

/* Values intentionally match classic MDX replaceable texture IDs. */
typedef enum {
    BZ_TTA_TEAM_TEXTURE_COLOR = 1,
    BZ_TTA_TEAM_TEXTURE_GLOW = 2,
} bzTTTeamTextureKind_t;

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
    uint32_t global_sequence_count; /* appended for ABI v3: GLBS wall-clock-modulo sequences */
    uint32_t emitter_count; /* appended for ABI v4: PRE2 particle emitters, see BZ_TTAsset_ParticleEmitterInfo */
} bzTTModelInfo_t;

typedef struct {
    uint32_t vertex_count;
    uint32_t normal_count;
    uint32_t uv_count;
    uint32_t index_count;
    uint32_t material_index;
    uint32_t vertex_group_count;
    bzTTBounds3_t bounds;
    /* Every geoset always has a resolved skin + matrix palette (>=1 entry), matching
     * classic MDX R_SetupGeosetVertexBuffer: geosets with no BONE hierarchy still get a
     * single-entry palette bound to node 0, so unanimated models render identically via
     * the same skinning path (identity bone matrix) rather than a separate static path. */
    uint32_t matrix_palette_count; /* appended v3: <= BZ_TTA_MAX_MATRIX_PALETTE */
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

/* Matches classic MDX MODELKEYTRACKTYPE exactly (renderer/r_local.h). Interpolation
 * runs between the keyframe's value and the following/preceding keyframe's in/out
 * tangents; NONE holds the left value, LINEAR lerps, HERMITE/BEZIER use the tangents. */
typedef enum {
    BZ_TTA_INTERP_NONE = 0,
    BZ_TTA_INTERP_LINEAR = 1,
    BZ_TTA_INTERP_HERMITE = 2,
    BZ_TTA_INTERP_BEZIER = 3,
} bzTTKeyInterp_t;

typedef enum {
    BZ_TTA_NODE_TRANSLATION = 0,
    BZ_TTA_NODE_ROTATION = 1,
    BZ_TTA_NODE_SCALE = 2,
} bzTTNodeChannel_t;

typedef struct {
    float x, y, z, w;
} bzTTQuat_t;

typedef struct {
    uint32_t time_msec;
    bzTTVec3_t value, in_tan, out_tan;
} bzTTVec3Key_t;

typedef struct {
    uint32_t time_msec;
    bzTTQuat_t value, in_tan, out_tan;
} bzTTQuatKey_t;

typedef struct {
    uint32_t time_msec;
    float value, in_tan, out_tan;
} bzTTFloatKey_t;

/* Describes one node channel's keyframe track without exposing its keys. global_sequence
 * is BZ_TTA_NO_GLOBAL_SEQUENCE when the track samples entity/sequence time directly, or a
 * global-sequence-table index whose duration (BZ_TTAsset_GlobalSequenceInfo) must instead be
 * used to compute a wrapped [0, duration) sample time, matching classic MDX GLBS semantics. */
typedef struct {
    uint32_t key_count;
    uint32_t interp; /* bzTTKeyInterp_t */
    uint32_t global_sequence;
} bzTTTrackInfo_t;

typedef struct {
    bool has_alpha_track;
    float static_alpha; /* used verbatim when has_alpha_track is false */
    bzTTTrackInfo_t alpha_track;
} bzTTGeosetAnimInfo_t;

/* Raw MDX PRE2 "head or tail" selector (mdx-m3-viewer's ParticleEmitter2 HeadOrTail enum,
 * cross-checked against games/warcraft-3/renderer/mdx/r_mdx_load.c's ReadParticleEmitter -
 * see games/warcraft-3/docs/file-formats/mdx.md's "Particle Emitters (PRE2)" section). Head
 * spawns a single billboard per particle; Tail spawns a trail/streak using tail_length; Both
 * draws one particle as both. This project's authoritative CPU particle simulation
 * (renderer/r_particles.c) does not yet consume tail rendering itself (see that doc section
 * for the traced, honest scope of what desktop actually draws) - this enum is carried through
 * so a consumer can at least select a supported vs. unsupported behavior deliberately instead
 * of guessing, and so a future tail-rendering slice has the data already plumbed. */
typedef enum {
    BZ_TTA_PARTICLE_HEAD = 0,
    BZ_TTA_PARTICLE_TAIL = 1,
    BZ_TTA_PARTICLE_BOTH = 2,
} bzTTParticleHeadTail_t;

/* One PRE2 particle emitter's immutable/static data (ABI v4). `node_index` is this emitter's
 * own entry in the SAME node array BZ_TTAsset_NodeInfo()/BZ_TTAsset_NodeTrackInfo() already
 * expose (translation/rotation/scale + parent-chain hierarchy + pivot) - an emitter is just
 * another node in the classic MDX node hierarchy (mdxParticleEmitter_t embeds mdxNode_t
 * exactly like a bone/helper/light/attachment does), so this ABI adds no separate
 * hierarchy/pivot/transform-track surface for emitters, only the particle-specific fields
 * below. `blend_mode` is already translated from PRE2's own distinct on-disk FilterMode
 * numbering (Blend/Additive/Modulate/Modulate2x/AlphaKey - NOT the same numeric convention as
 * bzTTMaterialLayerInfo_t.blend_mode) into this ABI's shared bzTTBlendMode_t vocabulary at
 * decode time - see games/warcraft-3/docs/file-formats/mdx.md for the exact mapping table and
 * citations. Every float below is a *static default*, used verbatim when the corresponding
 * BZ_TTAsset_EmitterTrackInfo() channel reports key_count 0 (no keyframe track) - matching
 * classic MDX's GET_PARTICLE_ANIM_PARAM fallback (r_mdx_geoset.c) exactly.
 *
 * IMPORTANT traced quirk (verified by re-reading + git blame on
 * games/warcraft-3/renderer/mdx/r_mdx_geoset.c's MDLX_RenderEmitter, commit 2bfbd4e0 - not
 * assumed): GET_PARTICLE_ANIM_PARAM computes a sampled-with-static-fallback LOCAL for all 7
 * physical channels (Width/Length/Speed/Latitude/Gravity/Variation/EmissionRate), but
 * MDLX_RenderEmitter's own call sites only ever READ the sampled locals for EmissionRate/
 * Speed/Gravity - FX_GenerateRandomOrigin(emitter->Length, emitter->Width) and
 * FX_GenerateRandomDirection(emitter->Latitude * M_PI/180) read the STATIC struct fields
 * directly (bypassing their own already-computed sampled locals), and the sampled Variation
 * local is never read by anything at all (confirmed by an exhaustive grep of every "Variation"
 * occurrence in this codebase - see games/warcraft-3/docs/file-formats/mdx.md's "Particle
 * Emitters (PRE2)" section). This ABI still exposes full track data for width/length/latitude/
 * variation (this struct's static fields plus BZ_TTAsset_EmitterTrackInfo()'s per-channel track)
 * because it is a complete, general, versioned mirror of the on-disk format, not scoped to any
 * one consumer's rendering behavior - but a consumer reproducing the authoritative renderer's
 * OBSERVABLE behavior (e.g. platform/android/quest/app/src/main/cpp/bz_quest_wc3_particles.h)
 * must reproduce this exact selective usage: sample EmissionRate/Speed/Gravity/Visibility,
 * use width/length/latitude's STATIC field only (never their track), and never use variation for
 * anything - not "fix" or "improve" on it, since that would diverge from traced authoritative
 * behavior. */
typedef struct {
    uint32_t node_index;
    uint32_t blend_mode;    /* bzTTBlendMode_t, translated from raw FilterMode - see above */
    uint32_t head_or_tail;  /* bzTTParticleHeadTail_t, translated from raw FrameFlags - see above */
    uint32_t texture_index; /* TEXS index, same convention as bzTTModelTextureInfo_t */
    uint32_t replaceable_id; /* TEXREPL_NONE/TEAMCOLOR/TEAMGLOW - same convention as material textures */
    uint32_t rows, columns;  /* atlas grid; both >= 1 (a raw 0 is normalized to 1 at decode time) */
    float speed, variation, latitude, gravity, life_span, emission_rate, length, width;
    float tail_length, time_middle; /* PRE2 TailLength/Time - segment-blend midpoint fraction [0,1] */
    float segment_color[9];  /* 3 RGB segments (start/mid/end), matches classic SegmentColor layout */
    uint8_t segment_alpha[3];
    float particle_scaling[3];
} bzTTParticleEmitterInfo_t;

/* PRE2's 8 animatable float channels (Visibility + the 7 physical parameters) - unlike node
 * translation/rotation/scale (3 distinct value types needing 3 distinct channel enums/copy
 * functions), every emitter channel is a plain float track, so one enum + one generic
 * Info/Copy accessor pair covers all 8 (DRY - see BZ_TTAsset_EmitterTrackInfo()'s doc
 * comment). */
typedef enum {
    BZ_TTA_EMITTER_VISIBILITY = 0,
    BZ_TTA_EMITTER_EMISSION_RATE = 1,
    BZ_TTA_EMITTER_WIDTH = 2,
    BZ_TTA_EMITTER_LENGTH = 3,
    BZ_TTA_EMITTER_SPEED = 4,
    BZ_TTA_EMITTER_LATITUDE = 5,
    BZ_TTA_EMITTER_GRAVITY = 6,
    BZ_TTA_EMITTER_VARIATION = 7,
} bzTTEmitterChannel_t;

/* Resolved top-4 vertex skin (mirrors classic mdxVertexSkin_t exactly). bone_index values are
 * slots into this geoset's own matrix palette (BZ_TTAsset_CopyGeosetMatrixPalette), not raw
 * MDX object_ids or global node indices — mirrors the desktop renderer's per-geoset palette
 * indirection exactly (bounded to BZ_TTA_MAX_MATRIX_PALETTE bones regardless of model size).
 * bone_weight values sum to 255 per vertex. */
typedef struct {
    uint8_t bone_index[BZ_TTA_MAX_VERTEX_SKIN_BONES];
    uint8_t bone_weight[BZ_TTA_MAX_VERTEX_SKIN_BONES];
} bzTTVertexSkin_t;

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
/* Returns the provider-authored index count for MDX team color/glow images.
 * Registration accepts [0, count), returns a retained image, and never accepts a path. */
uint32_t BZ_TTA_TeamTextureCount(uint32_t abi_version, bzTTTeamTextureKind_t kind);
const bzTTAsset_t *BZ_TTA_RegisterTeamTexture(uint32_t abi_version,
                                             bzTTTeamTextureKind_t kind,
                                             uint32_t team_color);
/* Resolves referenced W3E terrain imagery. Ground/cliff use their table type_index;
 * water is the singleton type_index 0. Unreferenced entries return NULL without lookup. */
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

/* Global sequence durations (msec). Nodes/geoset-anim tracks reference these by index via
 * bzTTTrackInfo_t.global_sequence; sampling wraps time modulo the returned duration. */
bool BZ_TTAsset_GlobalSequenceInfo(const bzTTAsset_t *asset, uint32_t index, uint32_t *out_duration_msec);

/* Node animation tracks (translation/scale = vec3 keys, rotation = quaternion keys). Absent
 * tracks (no KGxx chunk for that node) report key_count 0; callers must fall back to the
 * node's initial_translation/rotation/scale rest pose, matching classic MDX behavior. */
bool BZ_TTAsset_NodeTrackInfo(const bzTTAsset_t *asset, uint32_t node_index,
                              bzTTNodeChannel_t channel, bzTTTrackInfo_t *out);
uint32_t BZ_TTAsset_CopyNodeTranslationKeys(const bzTTAsset_t *asset, uint32_t node_index,
                                            bzTTVec3Key_t *dst, uint32_t cap);
uint32_t BZ_TTAsset_CopyNodeRotationKeys(const bzTTAsset_t *asset, uint32_t node_index,
                                         bzTTQuatKey_t *dst, uint32_t cap);
uint32_t BZ_TTAsset_CopyNodeScaleKeys(const bzTTAsset_t *asset, uint32_t node_index,
                                      bzTTVec3Key_t *dst, uint32_t cap);

/* Geoset dynamic material state: alpha animation (GEOA/KGAO) and resolved vertex skin
 * (GNDX/MTGC/MATS, already reduced to the top-4-weighted-bone form used for GPU skinning).
 * The matrix palette lists this geoset's referenced nodes (BZ_TTAsset_NodeInfo indices,
 * already remapped from raw MDX object_id at decode time); bzTTVertexSkin_t.bone_index
 * values are slots into this per-geoset array, matching classic MDX indirection. */
bool BZ_TTAsset_GeosetAnimInfo(const bzTTAsset_t *asset, uint32_t index, bzTTGeosetAnimInfo_t *out);
uint32_t BZ_TTAsset_CopyGeosetAlphaKeys(const bzTTAsset_t *asset, uint32_t index,
                                        bzTTFloatKey_t *dst, uint32_t cap);
uint32_t BZ_TTAsset_CopyGeosetVertexSkin(const bzTTAsset_t *asset, uint32_t index,
                                         bzTTVertexSkin_t *dst, uint32_t cap);
uint32_t BZ_TTAsset_CopyGeosetMatrixPalette(const bzTTAsset_t *asset, uint32_t index,
                                            uint32_t *dst, uint32_t cap);

/* PRE2 particle emitters (ABI v4) - traced end-to-end from games/warcraft-3/renderer/mdx's
 * real chunk parser/CPU simulation/draw path (r_mdx_load.c's ReadParticleEmitter,
 * r_mdx_geoset.c's MDLX_RenderEmitter, renderer/r_particles.c's spawn/age/draw), the only MDX
 * effect class this project's authoritative runtime actually parses AND simulates AND draws
 * today - see games/warcraft-3/docs/file-formats/mdx.md for the full audit (PREM/RIBB/EVTS
 * are each explicitly out of scope, with the evidence for why). `index` is in
 * [0, bzTTModelInfo_t.emitter_count). */
bool BZ_TTAsset_ParticleEmitterInfo(const bzTTAsset_t *asset, uint32_t index, bzTTParticleEmitterInfo_t *out);
/* One emitter's animatable-float-channel track (see bzTTEmitterChannel_t) - key_count 0 means
 * "no track for this channel", matching BZ_TTAsset_NodeTrackInfo()'s own "absent track"
 * contract exactly; callers fall back to bzTTParticleEmitterInfo_t's matching static field. */
bool BZ_TTAsset_EmitterTrackInfo(const bzTTAsset_t *asset, uint32_t emitter_index,
                                 bzTTEmitterChannel_t channel, bzTTTrackInfo_t *out);
uint32_t BZ_TTAsset_CopyEmitterFloatKeys(const bzTTAsset_t *asset, uint32_t emitter_index,
                                         bzTTEmitterChannel_t channel, bzTTFloatKey_t *dst, uint32_t cap);

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
/* Returns the dense registration list while preserving each W3E table index for corners.
 * Water has one reference iff a non-map-edge tile renders: type_index/type_id=0,
 * corner_count remains the authoritative number of water-flagged corners. */
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
