/*
 * sc2_tabletop_models.c - retained M3 geometry/material snapshots and cache.
 *
 * Parsing remains in common/sc2_m3.c; this file copies only the static Layer 2B1 contract into
 * renderer-independent immutable records. Unknown rendering semantics stay raw and unsupported.
 */
#include "sc2_tabletop_models_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/sc2_m3.h"

enum {
    SC2M_MAX_VERTICES = 1u << 20, SC2M_MAX_INDICES = 1u << 22, SC2M_MAX_RECORDS = 1u << 20,
};

static pthread_mutex_t g_model_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_model_initialized, g_model_terminal = true;
static uint64_t g_model_generation, g_model_map_generation;
static uint64_t g_model_hits, g_model_misses, g_model_placeholder_logs;
static bzSC2ASource_t g_model_source;
static bzSC2Model_t *g_model_cache;

static void model_retain_locked(const bzSC2Model_t *model) { ((bzSC2Model_t *)model)->refcount++; }
static void model_release_locked(const bzSC2Model_t *model) {
    if (--((bzSC2Model_t *)model)->refcount == 0) free((void *)model);
}

static void model_clear_cache_locked(void) {
    for (bzSC2Model_t *model = g_model_cache, *next; model; model = next) {
        next = model->cache_next;
        model->cache_next = NULL;
        model_release_locked(model);
    }
    g_model_cache = NULL;
}

static bzSC2Model_t *model_find_locked(const char *identity) {
    for (bzSC2Model_t *model = g_model_cache; model; model = model->cache_next)
        if (!strcmp(model->info.identity, identity)) return model;
    return NULL;
}

static void *model_data(bzSC2Model_t *model, uint32_t offset, size_t bytes) {
    size_t payload;
    if (!model || model->allocation_size < sizeof(*model)) return NULL;
    payload = model->allocation_size - sizeof(*model);
    if ((size_t)offset > payload || bytes > payload - offset) return NULL;
    return model->data + offset;
}

static bzSC2Model_t *model_alloc(size_t payload) {
    bzSC2Model_t *model;
    if (payload > SIZE_MAX - sizeof(*model)) return NULL;
    model = calloc(1, sizeof(*model) + payload);
    if (!model) return NULL;
    model->refcount = 1;
    model->allocation_size = sizeof(*model) + payload;
    return model;
}

static bzSC2Model_t *model_placeholder(bzSC2MResult_t status, const char *identity) {
    bzSC2Model_t *model = model_alloc(0);
    if (!model) return NULL;
    model->placeholder = true;
    model->status = status;
    if (identity) snprintf(model->info.identity, sizeof(model->info.identity), "%s", identity);
    return model;
}

typedef struct {
    size_t bytes;
    uint32_t vertices, indices, divisions, regions, batches, bone_lookup;
    uint32_t material_references, materials, composite_sections, layers;
} sc2ModelLayout_t;

static bool add_segment(size_t *total, uint32_t count, size_t stride, size_t alignment, uint32_t *offset) {
    size_t start, bytes;
    if (!alignment || alignment & (alignment - 1) || *total > SIZE_MAX - (alignment - 1)) return false;
    start = (*total + alignment - 1) & ~(alignment - 1);
    if (count && stride > SIZE_MAX / count) return false;
    bytes = (size_t)count * stride;
    if (bytes > SIZE_MAX - start || start + bytes > UINT32_MAX) return false;
    *offset = (uint32_t)start; *total = start + bytes;
    return true;
}

static bool model_layout(const bzSC2MModelInfo_t *info, sc2ModelLayout_t *layout) {
#define SC2M_SEGMENT(NAME, COUNT, TYPE) \
    if (!add_segment(&layout->bytes, COUNT, sizeof(TYPE), _Alignof(TYPE), &layout->NAME)) return false
    *layout = (sc2ModelLayout_t){ 0 };
    SC2M_SEGMENT(vertices, info->vertex_count, bzSC2MVertex_t);
    SC2M_SEGMENT(indices, info->index_count, uint16_t);
    SC2M_SEGMENT(divisions, info->division_count, bzSC2MDivisionInfo_t);
    SC2M_SEGMENT(regions, info->region_count, bzSC2MRegionInfo_t);
    SC2M_SEGMENT(batches, info->batch_count, bzSC2MBatchInfo_t);
    SC2M_SEGMENT(bone_lookup, info->bone_lookup_count, uint16_t);
    SC2M_SEGMENT(material_references, info->material_reference_count, bzSC2MMaterialReferenceInfo_t);
    SC2M_SEGMENT(materials, info->standard_material_count, bzSC2MStandardMaterialInfo_t);
    SC2M_SEGMENT(composite_sections, info->composite_section_count, bzSC2MCompositeSectionInfo_t);
    SC2M_SEGMENT(layers, info->layer_count, bzSC2MLayerInfo_t);
#undef SC2M_SEGMENT
    return true;
}

static uint32_t layer_count(const m3Material_t *material) {
    return material->diffuseLayerNum + material->decalLayerNum + material->specularLayerNum +
           material->glossLayerNum + material->emissiveLayerNum + material->emissive2LayerNum +
           material->evioLayerNum + material->evioMaskLayerNum + material->alphaMaskLayerNum +
           material->alphaMask2LayerNum + material->normalLayerNum + material->heightLayerNum +
           material->lightMapLayerNum + material->ambientOcclusionLayerNum;
}

static bool model_counts(const m3Model_t *src, bzSC2MModelInfo_t *info) {
    uint64_t indices = 0, regions = 0, batches = 0, layers = 0, sections = 0;
    if (src->verticesNum > SC2M_MAX_VERTICES || src->divisionsNum > SC2M_MAX_RECORDS ||
        src->boneLookupNum > SC2M_MAX_RECORDS || src->materialReferencesNum > SC2M_MAX_RECORDS ||
        src->materialStandardNum > SC2M_MAX_RECORDS)
        return false;
    FOR_LOOP(i, src->divisionsNum) {
        indices += src->divisions[i].facesNum;
        regions += src->divisions[i].regionsNum;
        batches += src->divisions[i].batchesNum;
    }
    FOR_LOOP(i, src->materialStandardNum) layers += layer_count(&src->materialStandard[i]);
    FOR_LOOP(i, src->materialCompositeNum) sections += src->materialComposite[i].sectionsNum;
    if (indices > SC2M_MAX_INDICES || regions > SC2M_MAX_RECORDS || batches > SC2M_MAX_RECORDS ||
        layers > SC2M_MAX_RECORDS || sections > SC2M_MAX_RECORDS)
        return false;
    info->vertex_count = src->verticesNum; info->index_count = (uint32_t)indices;
    info->division_count = src->divisionsNum; info->region_count = (uint32_t)regions;
    info->batch_count = (uint32_t)batches; info->bone_lookup_count = src->boneLookupNum;
    info->material_reference_count = src->materialReferencesNum;
    info->standard_material_count = src->materialStandardNum;
    info->composite_section_count = (uint32_t)sections; info->layer_count = (uint32_t)layers;
    return true;
}

static bool validate_model(const m3Model_t *src) {
    if (!src->head || src->type != 23 || SC2_M3VertexUVCount(src->vertexFlags) > 4 ||
        !SC2_M3ValidateGeometry(src))
        return false;
    FOR_LOOP(i, src->materialReferencesNum) {
        const m3MaterialReference_t *ref = &src->materialReferences[i];
        if ((ref->materialType == BZ_SC2M_MATERIAL_STANDARD && ref->materialIndex >= src->materialStandardNum) ||
            (ref->materialType == BZ_SC2M_MATERIAL_COMPOSITE && ref->materialIndex >= src->materialCompositeNum))
            return false;
    }
    return true;
}

static uint32_t anim_flags_u32(const m3Uint32AnimRef_t *ref) { return ref->animFlags ? 1u : 0u; }

static void copy_layer(bzSC2MLayerInfo_t *dst, const m3Layer_t *src, uint32_t material, uint32_t semantic) {
    *dst = (bzSC2MLayerInfo_t){
        .material = material, .semantic = semantic, .flags = src->flags, .uv_source = src->uvSource1,
        .color_channel = src->colorChannelSetting, .rtt_channel = src->rttChannel,
        .brightness = src->brightness.initValue, .bright_multiplier = src->brightMult.initValue,
        .midtone_offset = src->midtoneOffset.initValue, .noise_amplitude = src->noise.Amp,
        .noise_frequency = src->noise.Freq, .fresnel_type = src->fresnel.Type,
        .fresnel_exponent = src->fresnel.Exponent, .fresnel_min = src->fresnel.Min,
        .fresnel_max_offset = src->fresnel.MaxOffset, .video_frame_rate = src->video.FrameRate,
        .video_start_frame = src->video.StartFrame, .video_end_frame = src->video.EndFrame,
        .video_mode = src->video.Mode, .video_sync_timing = src->video.SyncTiming,
        .video_play = src->video.Play.initValue, .video_restart = src->video.Restart.initValue,
        .flipbook_rows = src->flipBook.Rows, .flipbook_columns = src->flipBook.Columns,
        .flipbook_frame = src->flipBook.Frame.initValue,
    };
    memcpy(dst->color, &src->color.initValue, 4);
    memcpy(dst->uv_offset, &src->uv.Offset.initValue, sizeof(dst->uv_offset));
    memcpy(dst->uv_angle, &src->uv.Angle.initValue, sizeof(dst->uv_angle));
    memcpy(dst->uv_tiling, &src->uv.Tiling.initValue, sizeof(dst->uv_tiling));
    memcpy(dst->tri_planar_offset, &src->triPlanarOffset.initValue, sizeof(dst->tri_planar_offset));
    memcpy(dst->tri_planar_scale, &src->triPlanarScale.initValue, sizeof(dst->tri_planar_scale));
    dst->fresnel2_inverted_mask[0] = src->fresnel2.InvertedMaskX;
    dst->fresnel2_inverted_mask[1] = src->fresnel2.InvertedMaskY;
    dst->fresnel2_inverted_mask[2] = src->fresnel2.InvertedMaskZ;
    dst->fresnel2_rotation[0] = src->fresnel2.RotationYaw;
    dst->fresnel2_rotation[1] = src->fresnel2.RotationPitch;
    dst->animated_flags = src->color.animFlags || src->brightMult.animFlags || src->midtoneOffset.animFlags ||
                          src->brightness.animFlags || src->uv.Offset.animFlags || src->uv.Angle.animFlags ||
                          src->uv.Tiling.animFlags || anim_flags_u32(&src->video.Play) ||
                          anim_flags_u32(&src->video.Restart) || src->flipBook.Frame.animFlags;
    if (dst->animated_flags) dst->unsupported_flags |= BZ_SC2M_UNSUPPORTED_ANIMATED_LAYER;
    if (src->imagePath) snprintf(dst->texture_identity, sizeof(dst->texture_identity), "%s", src->imagePath);
}

static bzSC2Model_t *export_model(const char *identity, const m3Model_t *src, uint64_t generation,
                                  bzSC2MResult_t *status) {
    bzSC2MModelInfo_t info = { .registration_generation = generation, .modl_version = src->type,
        .vertex_flags = src->vertexFlags, .vertex_stride = SC2_M3VertexDiskSize(src->vertexFlags),
        .uv_count = SC2_M3VertexUVCount(src->vertexFlags) };
    sc2ModelLayout_t layout;
    bzSC2Model_t *model;
    uint32_t index_cursor = 0, region_cursor = 0, batch_cursor = 0, section_cursor = 0, layer_cursor = 0;
    if (!model_counts(src, &info)) { *status = BZ_SC2M_ERR_TOO_LARGE; return NULL; }
    if (!validate_model(src)) {
        *status = src->type == 23 ? BZ_SC2M_ERR_MALFORMED : BZ_SC2M_ERR_UNSUPPORTED;
        return NULL;
    }
    if (!model_layout(&info, &layout)) {
        *status = BZ_SC2M_ERR_TOO_LARGE; return NULL;
    }
    model = model_alloc(layout.bytes);
    if (!model) { *status = BZ_SC2M_ERR_OUT_OF_MEMORY; return NULL; }
    model->status = BZ_SC2M_OK; model->info = info;
    snprintf(model->info.identity, sizeof(model->info.identity), "%s", identity);
    if (src->modelName) snprintf(model->info.name, sizeof(model->info.name), "%s", src->modelName);
    memcpy(model->info.bounds_min, &src->boundings.min, sizeof(model->info.bounds_min));
    memcpy(model->info.bounds_max, &src->boundings.max, sizeof(model->info.bounds_max));
    model->info.bounds_radius = src->boundings.radius;
    model->vertices_offset = layout.vertices; model->indices_offset = layout.indices;
    model->divisions_offset = layout.divisions; model->regions_offset = layout.regions;
    model->batches_offset = layout.batches; model->bone_lookup_offset = layout.bone_lookup;
    model->material_references_offset = layout.material_references; model->materials_offset = layout.materials;
    model->composite_sections_offset = layout.composite_sections; model->layers_offset = layout.layers;
    FOR_LOOP(i, info.vertex_count) {
        bzSC2MVertex_t *dst = model_data(model, model->vertices_offset + i * sizeof(*dst), sizeof(*dst));
        memcpy(dst->position, &src->vertices[i].pos, sizeof(dst->position));
        memcpy(dst->bone_weights, src->vertices[i].boneWeight, 4);
        memcpy(dst->bone_indices, src->vertices[i].boneIndex, 4);
        memcpy(dst->normal, src->vertices[i].normal, 4); memcpy(dst->tangent, src->vertices[i].tangent, 4);
        memcpy(dst->color, &src->vertices[i].color, 4); memcpy(dst->uv, src->vertices[i].uv, sizeof(dst->uv));
    }
    index_cursor = region_cursor = batch_cursor = 0;
    FOR_LOOP(d, src->divisionsNum) {
        const m3Divisions_t *division = &src->divisions[d];
        bzSC2MDivisionInfo_t *di = model_data(model, model->divisions_offset + d * sizeof(*di), sizeof(*di));
        *di = (bzSC2MDivisionInfo_t){ .first_index = index_cursor, .index_count = division->facesNum,
            .first_region = region_cursor, .region_count = division->regionsNum,
            .first_batch = batch_cursor, .batch_count = division->batchesNum };
        if (division->facesNum)
            memcpy(model_data(model, model->indices_offset + index_cursor * sizeof(uint16_t),
                              division->facesNum * sizeof(uint16_t)), division->faces,
                   division->facesNum * sizeof(uint16_t));
        FOR_LOOP(r, division->regionsNum) {
            const m3Region_t *src_region = &division->regions[r];
            bzSC2MRegionInfo_t *dst = model_data(model, model->regions_offset + region_cursor * sizeof(*dst),
                                                 sizeof(*dst));
            *dst = (bzSC2MRegionInfo_t){ .division = d, .first_vertex = src_region->firstVertexIndex,
                .vertex_count = src_region->verticesCount,
                .first_index = index_cursor + src_region->firstTriangleIndex,
                .index_count = src_region->triangleIndicesCount, .bone_count = src_region->bonesCount,
                .first_bone_lookup = src_region->firstBoneLookupIndex,
                .bone_lookup_count = src_region->boneLookupIndicesCount,
                .root_bone = src_region->rootBoneIndex, .bone_weight_pairs = src_region->boneWeightPairsCount };
            region_cursor++;
        }
        FOR_LOOP(b, division->batchesNum) {
            bzSC2MBatchInfo_t *dst = model_data(model, model->batches_offset + batch_cursor * sizeof(*dst),
                                                sizeof(*dst));
            *dst = (bzSC2MBatchInfo_t){ .division = d,
                .region = di->first_region + division->batches[b].regionIndex,
                .material_reference = division->batches[b].materialReferenceIndex };
            batch_cursor++;
        }
        index_cursor += division->facesNum;
    }
    if (info.bone_lookup_count)
        memcpy(model_data(model, model->bone_lookup_offset, info.bone_lookup_count * sizeof(uint16_t)),
               src->boneLookup, info.bone_lookup_count * sizeof(uint16_t));
    FOR_LOOP(i, src->materialReferencesNum) {
        const m3MaterialReference_t *ref = &src->materialReferences[i];
        bzSC2MMaterialReferenceInfo_t *dst = model_data(model,
            model->material_references_offset + i * sizeof(*dst), sizeof(*dst));
        dst->kind = ref->materialType; dst->index = ref->materialIndex;
        dst->status = ref->materialType == BZ_SC2M_MATERIAL_STANDARD ||
                      ref->materialType == BZ_SC2M_MATERIAL_COMPOSITE ? BZ_SC2M_OK : BZ_SC2M_ERR_UNSUPPORTED;
        if (dst->status == BZ_SC2M_ERR_UNSUPPORTED)
            model->info.unsupported_flags |= BZ_SC2M_UNSUPPORTED_NONSTANDARD_MATERIAL;
        if (ref->materialType == BZ_SC2M_MATERIAL_COMPOSITE)
            model->info.unsupported_flags |= BZ_SC2M_UNSUPPORTED_COMPOSITE_EVALUATION;
    }
    FOR_LOOP(i, src->materialStandardNum) {
        const m3Material_t *material = &src->materialStandard[i];
        bzSC2MStandardMaterialInfo_t *dst = model_data(model, model->materials_offset + i * sizeof(*dst),
                                                       sizeof(*dst));
        *dst = (bzSC2MStandardMaterialInfo_t){ .flags = material->flags,
            .additional_flags = material->additionalFlags, .blend_mode = material->blendMode,
            .priority = material->priority, .used_rtt_channels = material->usedRTTChannels,
            .cutout_threshold = material->cutoutThreshold, .layer_first = layer_cursor,
            .layer_count = layer_count(material), .unsupported_flags = BZ_SC2M_UNSUPPORTED_MATERIAL_FLAGS,
            .specularity = material->specularity, .depth_blend_falloff = material->depthBlendFalloff,
            .specular_multiplier = material->specMult, .emissive_multiplier = material->emisMult,
            .layer_blend_type = material->layerBlendType, .emissive_blend_type = material->emisBlendType,
            .emissive_mode = material->emisMode, .specular_type = material->specType };
        model->info.unsupported_flags |= BZ_SC2M_UNSUPPORTED_MATERIAL_FLAGS;
        if (material->name) snprintf(dst->name, sizeof(dst->name), "%s", material->name);
#define SC2M_LAYERS(NAME, SEMANTIC) FOR_LOOP(n, material->NAME##Num) { \
    bzSC2MLayerInfo_t *layer = model_data(model, model->layers_offset + layer_cursor++ * sizeof(*layer), \
                                          sizeof(*layer)); \
    copy_layer(layer, &material->NAME[n], i, SEMANTIC); \
    if (layer->unsupported_flags) model->info.unsupported_flags |= layer->unsupported_flags; \
}
        SC2M_LAYERS(diffuseLayer, BZ_SC2M_LAYER_DIFFUSE)
        SC2M_LAYERS(decalLayer, BZ_SC2M_LAYER_DECAL)
        SC2M_LAYERS(specularLayer, BZ_SC2M_LAYER_SPECULAR)
        SC2M_LAYERS(glossLayer, BZ_SC2M_LAYER_GLOSS)
        SC2M_LAYERS(emissiveLayer, BZ_SC2M_LAYER_EMISSIVE)
        SC2M_LAYERS(emissive2Layer, BZ_SC2M_LAYER_EMISSIVE2)
        SC2M_LAYERS(evioLayer, BZ_SC2M_LAYER_ENVIRONMENT)
        SC2M_LAYERS(evioMaskLayer, BZ_SC2M_LAYER_ENVIRONMENT_MASK)
        SC2M_LAYERS(alphaMaskLayer, BZ_SC2M_LAYER_ALPHA_MASK)
        SC2M_LAYERS(alphaMask2Layer, BZ_SC2M_LAYER_ALPHA_MASK2)
        SC2M_LAYERS(normalLayer, BZ_SC2M_LAYER_NORMAL)
        SC2M_LAYERS(heightLayer, BZ_SC2M_LAYER_HEIGHT)
        SC2M_LAYERS(lightMapLayer, BZ_SC2M_LAYER_LIGHT_MAP)
        SC2M_LAYERS(ambientOcclusionLayer, BZ_SC2M_LAYER_AMBIENT_OCCLUSION)
#undef SC2M_LAYERS
    }
    FOR_LOOP(i, src->materialCompositeNum) FOR_LOOP(n, src->materialComposite[i].sectionsNum) {
        const m3CompositeMaterialSection_t *section = &src->materialComposite[i].sections[n];
        bzSC2MCompositeSectionInfo_t *dst = model_data(model,
            model->composite_sections_offset + section_cursor++ * sizeof(*dst), sizeof(*dst));
        *dst = (bzSC2MCompositeSectionInfo_t){ .composite_material = i,
            .material_reference = section->materialReferenceIndex, .alpha = section->alphaFactor.initValue,
            .animated = section->alphaFactor.animFlags != 0 };
    }
    *status = BZ_SC2M_OK;
    return model;
}

static bzSC2Model_t *load_model(const char *identity, bzSC2MResult_t *status, uint64_t generation) {
    uint32_t size = 0;
    void *raw;
    m3Model_t *parsed;
    bzSC2Model_t *model;
    if (!g_model_source.read_file) { *status = BZ_SC2M_ERR_NOT_INITIALIZED; return NULL; }
    raw = g_model_source.read_file(identity, &size);
    if (!raw || !size) {
        if (raw && g_model_source.free_file) g_model_source.free_file(raw);
        *status = BZ_SC2M_ERR_NOT_FOUND; return NULL;
    }
    parsed = SC2_M3Parse(raw, size);
    if (g_model_source.free_file) g_model_source.free_file(raw);
    if (!parsed || !parsed->head) {
        *status = parsed && parsed->unsupported ? BZ_SC2M_ERR_UNSUPPORTED : BZ_SC2M_ERR_MALFORMED;
        SC2_M3Free(parsed); return NULL;
    }
    model = export_model(identity, parsed, generation, status);
    SC2_M3Free(parsed);
    return model;
}

void BZ_SC2M_Init(void) {
    bzSC2ASource_t source = { 0 };
    BZ_SC2_TTA_Source(&source);
    BZ_SC2A_ProviderLock();
    pthread_mutex_lock(&g_model_lock);
    model_clear_cache_locked();
    g_model_source = source; g_model_hits = g_model_misses = g_model_placeholder_logs = 0;
    if (!++g_model_generation) g_model_generation = 1;
    g_model_map_generation = 0; g_model_initialized = true; g_model_terminal = false;
    pthread_mutex_unlock(&g_model_lock);
    BZ_SC2A_ProviderUnlock();
}

void BZ_SC2M_Shutdown(void) {
    pthread_mutex_lock(&g_model_lock);
    g_model_terminal = true; model_clear_cache_locked();
    pthread_mutex_unlock(&g_model_lock);
    BZ_SC2A_ProviderLock();
    BZ_SC2A_ProviderUnlock();
}

uint32_t BZ_SC2M_AbiVersion(void) { return BZ_SC2M_ABI_VERSION; }

uint64_t BZ_SC2M_BeginRegistration(uint64_t map_generation) {
    pthread_mutex_lock(&g_model_lock);
    if (g_model_initialized && !g_model_terminal && map_generation != g_model_map_generation) {
        model_clear_cache_locked();
        g_model_map_generation = map_generation;
        if (!++g_model_generation) g_model_generation = 1;
    }
    uint64_t generation = g_model_generation;
    pthread_mutex_unlock(&g_model_lock);
    return generation;
}

void BZ_SC2M_EndRegistration(uint64_t registration_generation) {
    /* BeginRegistration already evicts the preceding generation; current entries remain cached. */
    (void)registration_generation;
}

const bzSC2Model_t *BZ_SC2M_RegisterModel(uint32_t abi_version, const char *identity) {
    char normalized[BZ_SC2M_MAX_IDENTITY];
    bzSC2Model_t *model;
    bzSC2MResult_t status;
    uint64_t generation;
    if (abi_version != BZ_SC2M_ABI_VERSION) return model_placeholder(BZ_SC2M_ERR_ABI_VERSION, identity);
    if (!identity || !sc2_tta_normalize_identity(identity, normalized, sizeof(normalized)))
        return model_placeholder(BZ_SC2M_ERR_PATH_CONFINEMENT, identity);
    pthread_mutex_lock(&g_model_lock);
    status = !g_model_initialized ? BZ_SC2M_ERR_NOT_INITIALIZED :
             g_model_terminal ? BZ_SC2M_ERR_TERMINAL : BZ_SC2M_OK;
    generation = g_model_generation;
    model = status == BZ_SC2M_OK ? model_find_locked(normalized) : NULL;
    if (model) { g_model_hits++; model_retain_locked(model); }
    pthread_mutex_unlock(&g_model_lock);
    if (model) return model;
    if (status != BZ_SC2M_OK) return model_placeholder(status, normalized);
    BZ_SC2A_ProviderLock();
    pthread_mutex_lock(&g_model_lock);
    model = g_model_generation == generation && !g_model_terminal ? model_find_locked(normalized) : NULL;
    if (model) { g_model_hits++; model_retain_locked(model); }
    else if (g_model_generation == generation && !g_model_terminal) g_model_misses++;
    status = g_model_terminal || g_model_generation != generation ? BZ_SC2M_ERR_TERMINAL : BZ_SC2M_OK;
    pthread_mutex_unlock(&g_model_lock);
    if (model || status != BZ_SC2M_OK) {
        BZ_SC2A_ProviderUnlock();
        return model ? model : model_placeholder(status, normalized);
    }
    model = load_model(normalized, &status, generation);
    if (!model) model = model_placeholder(status, normalized);
    if (!model) { BZ_SC2A_ProviderUnlock(); return NULL; }
    pthread_mutex_lock(&g_model_lock);
    if (g_model_terminal || generation != g_model_generation) {
        pthread_mutex_unlock(&g_model_lock);
        model_release_locked(model);
        BZ_SC2A_ProviderUnlock();
        return model_placeholder(BZ_SC2M_ERR_TERMINAL, normalized);
    }
    model->cache_next = g_model_cache; g_model_cache = model; model_retain_locked(model);
    if (model->placeholder) g_model_placeholder_logs++;
    pthread_mutex_unlock(&g_model_lock);
    BZ_SC2A_ProviderUnlock();
    if (model->placeholder)
        fprintf(stderr, "SC2TabletopModels: model '%s' unavailable (%d); cached placeholder\n", normalized, status);
    return model;
}

void BZ_SC2Model_Retain(const bzSC2Model_t *model) {
    if (!model) return;
    pthread_mutex_lock(&g_model_lock); model_retain_locked(model); pthread_mutex_unlock(&g_model_lock);
}
void BZ_SC2Model_Release(const bzSC2Model_t *model) {
    if (!model) return;
    pthread_mutex_lock(&g_model_lock); model_release_locked(model); pthread_mutex_unlock(&g_model_lock);
}
bool BZ_SC2Model_IsPlaceholder(const bzSC2Model_t *model) { return !model || model->placeholder; }
bzSC2MResult_t BZ_SC2Model_Status(const bzSC2Model_t *model) {
    return model ? model->status : BZ_SC2M_ERR_INVALID_ARGUMENT;
}
bool BZ_SC2Model_Info(const bzSC2Model_t *model, bzSC2MModelInfo_t *out) {
    if (!model || !out) return false; *out = model->info; return true;
}

#define SC2M_ACCESSOR(NAME, TYPE, COUNT, OFFSET) \
bool NAME(const bzSC2Model_t *model, uint32_t index, TYPE *out) { \
    const TYPE *src; \
    if (!model || !out || index >= model->info.COUNT) return false; \
    src = model_data((bzSC2Model_t *)model, model->OFFSET + index * (uint32_t)sizeof(TYPE), sizeof(TYPE)); \
    if (!src) return false; *out = *src; return true; \
}
SC2M_ACCESSOR(BZ_SC2Model_Vertex, bzSC2MVertex_t, vertex_count, vertices_offset)
SC2M_ACCESSOR(BZ_SC2Model_Index, uint16_t, index_count, indices_offset)
SC2M_ACCESSOR(BZ_SC2Model_DivisionInfo, bzSC2MDivisionInfo_t, division_count, divisions_offset)
SC2M_ACCESSOR(BZ_SC2Model_RegionInfo, bzSC2MRegionInfo_t, region_count, regions_offset)
SC2M_ACCESSOR(BZ_SC2Model_BatchInfo, bzSC2MBatchInfo_t, batch_count, batches_offset)
SC2M_ACCESSOR(BZ_SC2Model_BoneLookup, uint16_t, bone_lookup_count, bone_lookup_offset)
SC2M_ACCESSOR(BZ_SC2Model_MaterialReferenceInfo, bzSC2MMaterialReferenceInfo_t, material_reference_count,
              material_references_offset)
SC2M_ACCESSOR(BZ_SC2Model_StandardMaterialInfo, bzSC2MStandardMaterialInfo_t, standard_material_count,
              materials_offset)
SC2M_ACCESSOR(BZ_SC2Model_CompositeSectionInfo, bzSC2MCompositeSectionInfo_t, composite_section_count,
              composite_sections_offset)
SC2M_ACCESSOR(BZ_SC2Model_LayerInfo, bzSC2MLayerInfo_t, layer_count, layers_offset)
#undef SC2M_ACCESSOR

const bzSC2Image_t *BZ_SC2M_RegisterLayerImage(uint32_t abi_version, const bzSC2Model_t *model,
                                              uint32_t layer_index) {
    bzSC2MLayerInfo_t layer;
    if (abi_version != BZ_SC2M_ABI_VERSION || !model || model->placeholder ||
        !BZ_SC2Model_LayerInfo(model, layer_index, &layer))
        return BZ_SC2A_RegisterImage(BZ_SC2A_ABI_VERSION, "");
    pthread_mutex_lock(&g_model_lock);
    bool current = !g_model_terminal && model->info.registration_generation == g_model_generation;
    pthread_mutex_unlock(&g_model_lock);
    return BZ_SC2A_RegisterImage(BZ_SC2A_ABI_VERSION, current ? layer.texture_identity : "");
}

uint64_t BZ_SC2M_CacheHits(void) {
    pthread_mutex_lock(&g_model_lock);
    uint64_t n = g_model_hits;
    pthread_mutex_unlock(&g_model_lock);
    return n;
}
uint64_t BZ_SC2M_CacheMisses(void) {
    pthread_mutex_lock(&g_model_lock);
    uint64_t n = g_model_misses;
    pthread_mutex_unlock(&g_model_lock);
    return n;
}
uint64_t BZ_SC2M_PlaceholderLogs(void) {
    pthread_mutex_lock(&g_model_lock);
    uint64_t n = g_model_placeholder_logs;
    pthread_mutex_unlock(&g_model_lock);
    return n;
}
