/*
 * sc2_tabletop_models.h - retained immutable SC2 M3 static geometry/material ABI.
 *
 * Layer 2B1 exports parser facts only. Authored records are preserved even when their rendering
 * semantics are unsupported; no renderer, animation, entity, Swift, or RealityKit type crosses it.
 */
#ifndef SC2_TABLETOP_MODELS_H
#define SC2_TABLETOP_MODELS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sc2_tabletop_assets.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BZ_SC2_TABLETOP_MODELS_ABI_VERSION 1u
#define BZ_SC2M_ABI_VERSION BZ_SC2_TABLETOP_MODELS_ABI_VERSION

enum { BZ_SC2M_MAX_NAME = 128, BZ_SC2M_MAX_IDENTITY = BZ_SC2A_MAX_IDENTITY };

typedef struct bzSC2Model bzSC2Model_t;

typedef enum {
    BZ_SC2M_OK = 0,
    BZ_SC2M_ERR_NOT_INITIALIZED,
    BZ_SC2M_ERR_TERMINAL,
    BZ_SC2M_ERR_ABI_VERSION,
    BZ_SC2M_ERR_INVALID_ARGUMENT,
    BZ_SC2M_ERR_PATH_CONFINEMENT,
    BZ_SC2M_ERR_NOT_FOUND,
    BZ_SC2M_ERR_MALFORMED,
    BZ_SC2M_ERR_UNSUPPORTED,
    BZ_SC2M_ERR_TOO_LARGE,
    BZ_SC2M_ERR_OUT_OF_MEMORY,
} bzSC2MResult_t;

typedef enum {
    BZ_SC2M_MATERIAL_STANDARD = 1,
    BZ_SC2M_MATERIAL_DISPLACEMENT,
    BZ_SC2M_MATERIAL_COMPOSITE,
    BZ_SC2M_MATERIAL_TERRAIN,
    BZ_SC2M_MATERIAL_VOLUME,
    BZ_SC2M_MATERIAL_UNKNOWN,
    BZ_SC2M_MATERIAL_CREEP,
    BZ_SC2M_MATERIAL_VOLUME_NOISE,
    BZ_SC2M_MATERIAL_SPLAT_TERRAIN_BAKE,
    BZ_SC2M_MATERIAL_LENS_FLARE = 11,
} bzSC2MMaterialKind_t;

typedef enum {
    BZ_SC2M_LAYER_DIFFUSE = 1, BZ_SC2M_LAYER_DECAL, BZ_SC2M_LAYER_SPECULAR, BZ_SC2M_LAYER_GLOSS,
    BZ_SC2M_LAYER_EMISSIVE, BZ_SC2M_LAYER_EMISSIVE2, BZ_SC2M_LAYER_ENVIRONMENT,
    BZ_SC2M_LAYER_ENVIRONMENT_MASK, BZ_SC2M_LAYER_ALPHA_MASK, BZ_SC2M_LAYER_ALPHA_MASK2,
    BZ_SC2M_LAYER_NORMAL, BZ_SC2M_LAYER_HEIGHT, BZ_SC2M_LAYER_LIGHT_MAP,
    BZ_SC2M_LAYER_AMBIENT_OCCLUSION,
} bzSC2MLayerSemantic_t;

enum {
    BZ_SC2M_UNSUPPORTED_NONSTANDARD_MATERIAL = 1u << 0,
    BZ_SC2M_UNSUPPORTED_COMPOSITE_EVALUATION = 1u << 1,
    BZ_SC2M_UNSUPPORTED_ANIMATED_LAYER = 1u << 2,
    BZ_SC2M_UNSUPPORTED_LAYER_SEMANTICS = 1u << 3,
    BZ_SC2M_UNSUPPORTED_MATERIAL_FLAGS = 1u << 4,
};

typedef struct {
    uint64_t registration_generation;
    uint32_t modl_version, vertex_flags, vertex_stride, uv_count;
    uint32_t vertex_count, index_count, division_count, region_count, batch_count, bone_lookup_count;
    uint32_t material_reference_count, standard_material_count, composite_section_count, layer_count;
    uint32_t unsupported_flags;
    float bounds_min[3], bounds_max[3], bounds_radius;
    char identity[BZ_SC2M_MAX_IDENTITY];
    char name[BZ_SC2M_MAX_NAME];
} bzSC2MModelInfo_t;

typedef struct {
    float position[3];
    uint8_t bone_weights[4], bone_indices[4], normal[4], tangent[4], color[4];
    int16_t uv[4][2];
} bzSC2MVertex_t;

typedef struct {
    uint32_t first_index, index_count, first_region, region_count, first_batch, batch_count;
} bzSC2MDivisionInfo_t;

typedef struct {
    uint32_t division, first_vertex, vertex_count, first_index, index_count;
    uint16_t bone_count, first_bone_lookup, bone_lookup_count, root_bone;
    uint8_t bone_weight_pairs, reserved[3];
} bzSC2MRegionInfo_t;

typedef struct {
    uint32_t division, region, material_reference;
} bzSC2MBatchInfo_t;

typedef struct {
    uint32_t kind, index;
    bzSC2MResult_t status;
} bzSC2MMaterialReferenceInfo_t;

typedef struct {
    uint32_t flags, additional_flags, blend_mode;
    int32_t priority;
    uint32_t used_rtt_channels, cutout_threshold, layer_first, layer_count, unsupported_flags;
    float specularity, depth_blend_falloff, specular_multiplier, emissive_multiplier;
    uint32_t layer_blend_type, emissive_blend_type, emissive_mode, specular_type;
    char name[BZ_SC2M_MAX_NAME];
} bzSC2MStandardMaterialInfo_t;

typedef struct {
    uint32_t composite_material, material_reference;
    float alpha;
    uint32_t animated;
} bzSC2MCompositeSectionInfo_t;

typedef struct {
    uint32_t material, semantic, flags, uv_source, color_channel, rtt_channel;
    uint32_t availability_flags, animated_flags, unsupported_flags;
    uint8_t color[4];
    float brightness, bright_multiplier, midtone_offset;
    float uv_offset[2], uv_angle[3], uv_tiling[2];
    float tri_planar_offset[3], tri_planar_scale[3];
    float noise_amplitude, noise_frequency;
    uint32_t fresnel_type;
    float fresnel_exponent, fresnel_min, fresnel_max_offset;
    uint32_t video_frame_rate, video_start_frame, video_end_frame, video_mode;
    uint32_t video_sync_timing, video_play, video_restart;
    uint32_t flipbook_rows, flipbook_columns, flipbook_frame;
    float fresnel2_inverted_mask[3], fresnel2_rotation[2];
    char texture_identity[BZ_SC2M_MAX_IDENTITY];
} bzSC2MLayerInfo_t;

void BZ_SC2M_Init(void);
void BZ_SC2M_Shutdown(void);
uint32_t BZ_SC2M_AbiVersion(void);
uint64_t BZ_SC2M_BeginRegistration(uint64_t map_generation);
void BZ_SC2M_EndRegistration(uint64_t registration_generation);
const bzSC2Model_t *BZ_SC2M_RegisterModel(uint32_t abi_version, const char *identity);
void BZ_SC2Model_Retain(const bzSC2Model_t *model);
void BZ_SC2Model_Release(const bzSC2Model_t *model);
bool BZ_SC2Model_IsPlaceholder(const bzSC2Model_t *model);
bzSC2MResult_t BZ_SC2Model_Status(const bzSC2Model_t *model);
bool BZ_SC2Model_Info(const bzSC2Model_t *model, bzSC2MModelInfo_t *out);
bool BZ_SC2Model_Vertex(const bzSC2Model_t *model, uint32_t index, bzSC2MVertex_t *out);
bool BZ_SC2Model_Index(const bzSC2Model_t *model, uint32_t index, uint16_t *out);
bool BZ_SC2Model_DivisionInfo(const bzSC2Model_t *model, uint32_t index, bzSC2MDivisionInfo_t *out);
bool BZ_SC2Model_RegionInfo(const bzSC2Model_t *model, uint32_t index, bzSC2MRegionInfo_t *out);
bool BZ_SC2Model_BatchInfo(const bzSC2Model_t *model, uint32_t index, bzSC2MBatchInfo_t *out);
bool BZ_SC2Model_BoneLookup(const bzSC2Model_t *model, uint32_t index, uint16_t *out);
bool BZ_SC2Model_MaterialReferenceInfo(const bzSC2Model_t *model, uint32_t index,
                                       bzSC2MMaterialReferenceInfo_t *out);
bool BZ_SC2Model_StandardMaterialInfo(const bzSC2Model_t *model, uint32_t index,
                                     bzSC2MStandardMaterialInfo_t *out);
bool BZ_SC2Model_CompositeSectionInfo(const bzSC2Model_t *model, uint32_t index,
                                     bzSC2MCompositeSectionInfo_t *out);
bool BZ_SC2Model_LayerInfo(const bzSC2Model_t *model, uint32_t index, bzSC2MLayerInfo_t *out);
const bzSC2Image_t *BZ_SC2M_RegisterLayerImage(uint32_t abi_version, const bzSC2Model_t *model,
                                              uint32_t layer_index);
uint64_t BZ_SC2M_CacheHits(void);
uint64_t BZ_SC2M_CacheMisses(void);
uint64_t BZ_SC2M_PlaceholderLogs(void);

#ifdef __cplusplus
}
#endif
#endif
