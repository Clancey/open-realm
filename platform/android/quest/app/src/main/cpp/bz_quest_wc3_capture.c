/*
 * bz_quest_wc3_capture.c - see bz_quest_wc3_capture.h.
 */
#include "bz_quest_wc3_capture.h"

#include <stdio.h>
#include <string.h>

#include "platform/bridge/bz_tabletop_assets.h"
#include "platform/bridge/bz_tabletop_transport.h"

/* LiveTabletopTransport.swift:212 - "common/shared.h's frozen CS_MODELS base". */
static const uint32_t kModelConfigStringBase = 32;

/* Cross-checks bz_quest_vk_wc3.c's mirrored BZ_QUEST_TTA_BLEND_* constants
 * (that file deliberately does not #include this header - see its own
 * comment) against the real bzTTBlendMode_t values, at file scope so a
 * future renumbering fails the build here instead of silently
 * mis-blending on device. This is the one file in this slice that
 * legitimately includes bz_tabletop_assets.h, matching
 * bz_quest_renderer.c's own precedent of cross-checking a sibling file's
 * mirrored OpenXR bit values. */
_Static_assert(BZ_TTA_BLEND_OPAQUE == 0 && BZ_TTA_BLEND_TRANSPARENT == 1 &&
                   BZ_TTA_BLEND_ALPHA == 2 && BZ_TTA_BLEND_ADDITIVE == 3 &&
                   BZ_TTA_BLEND_ADD_ALPHA == 4 && BZ_TTA_BLEND_MODULATE == 5 &&
                   BZ_TTA_BLEND_MODULATE_2X == 6,
               "bz_quest_vk_wc3.c's mirrored BZ_QUEST_TTA_BLEND_* values have drifted from "
               "bz_tabletop_assets.h's bzTTBlendMode_t");

/* -- bounded static scratch storage (never stack-allocated - see
 * bz_quest_wc3_render.h's bzQuestWc3Model_t doc comment) -- */

/* The one model-geometry decode scratch buffer, reused sequentially for
 * each newly-touched model within a frame (capture is single-threaded). */
static bzQuestWc3Model_t s_scratchModel;
/* Per-geoset staging: the ABI's Copy* functions want struct-of-arrays
 * (bzTTVec3_t/bzTTVec2_t/uint16_t) input; these are re-interleaved with the
 * axis swap + winding fix into s_scratchModel's AoS bzQuestWc3Vertex_t/
 * uint32_t arrays below. Sized to one whole model's worth (a single geoset
 * can in principle hold every vertex/index of its model). */
static bzTTVec3_t s_stagePositions[BZ_QUEST_WC3_MAX_VERTS_PER_MODEL];
static bzTTVec3_t s_stageNormals[BZ_QUEST_WC3_MAX_VERTS_PER_MODEL];
static bzTTVec2_t s_stageUVs[BZ_QUEST_WC3_MAX_VERTS_PER_MODEL];
static uint16_t s_stageIndices[BZ_QUEST_WC3_MAX_INDICES_PER_MODEL]; /* per-geoset-local, per ABI */
/* The one texture-pixel decode scratch buffer, reused sequentially for each
 * newly-touched direct texture within a frame - see
 * BZ_QUEST_WC3_MAX_TEXTURE_BYTES's doc comment in bz_quest_wc3_render.h. */
static uint8_t s_scratchPixels[BZ_QUEST_WC3_MAX_TEXTURE_BYTES];

/* Per-frame "already decoded this model config-string index this frame"
 * dedup, matching LiveTabletopTransport.swift:236-238's baseModels dict -
 * reset at the top of every bz_quest_wc3_capture_frame() call. */
static uint32_t s_seenModelConfigIndex[BZ_QUEST_WC3_MAX_UNIQUE_MODELS_PER_FRAME];
static uint32_t s_seenModelCount;

/* Logs a given "unsupported/oversized for this slice" condition at most
 * once per unique (identity, detail) pair for the life of the process -
 * these are asset-shape diagnostics (a model either has a too-large
 * geoset or it doesn't), not per-frame snapshot state, so a simple
 * grows-forever dedup table (capped, oldest entries just stop being
 * deduped past the cap rather than crashing) is appropriate here, unlike
 * bz_quest_frame.c's generation-based per-frame dedup. */
enum { BZ_QUEST_WC3_MAX_LOGGED_KEYS = 256 };
static char s_loggedKeys[BZ_QUEST_WC3_MAX_LOGGED_KEYS][BZ_QUEST_WC3_MAX_IDENTITY + 32];
static uint32_t s_loggedKeyCount;

static bool log_once(const char *identity, const char *detail) {
    char key[BZ_QUEST_WC3_MAX_IDENTITY + 32];
    snprintf(key, sizeof(key), "%s|%s", identity, detail);
    for (uint32_t i = 0; i < s_loggedKeyCount; i++) {
        if (strcmp(s_loggedKeys[i], key) == 0) return false;
    }
    if (s_loggedKeyCount < BZ_QUEST_WC3_MAX_LOGGED_KEYS) {
        strncpy(s_loggedKeys[s_loggedKeyCount], key, sizeof(s_loggedKeys[0]) - 1);
        s_loggedKeyCount++;
    }
    return true;
}

#define LOG_ONCE(identity, detail, ...) \
    do { if (log_once((identity), (detail))) fprintf(stderr, __VA_ARGS__); } while (0)

/* Resolves one geoset's material's layers into bzQuestWc3Geoset_t.layers -
 * replaceable_id 0 (direct texture) is fully resolved and reported via
 * callbacks->onTextureReady; replaceable_id 1/2 (team color/glow) and any
 * other value are marked unsupported and logged once (see this file's
 * header comment's texture decode policy). */
static void decode_layers(const bzTTAsset_t *asset, const char *modelIdentity, uint32_t geosetIndex,
                          uint32_t materialIndex, bzQuestWc3Geoset_t *outGeoset,
                          const bzQuestWc3CaptureCallbacks_t *callbacks) {
    outGeoset->layerCount = 0;
    bzTTMaterialInfo_t mat;
    if (!BZ_TTAsset_MaterialInfo(asset, materialIndex, &mat)) {
        LOG_ONCE(modelIdentity, "material-missing",
                 "bz_quest_wc3_capture: model '%s' geoset %u material %u missing MaterialInfo\n",
                 modelIdentity, geosetIndex, materialIndex);
        return;
    }
    uint32_t layerCount = mat.layer_count;
    if (layerCount > BZ_QUEST_WC3_MAX_LAYERS_PER_GEOSET) layerCount = BZ_QUEST_WC3_MAX_LAYERS_PER_GEOSET;
    for (uint32_t li = 0; li < layerCount; li++) {
        bzTTMaterialLayerInfo_t layerInfo;
        if (!BZ_TTAsset_MaterialLayerInfo(asset, mat.first_layer + li, &layerInfo)) continue;

        bzQuestWc3LayerDesc_t *layer = &outGeoset->layers[outGeoset->layerCount];
        layer->blendMode = layerInfo.blend_mode;
        layer->flags = layerInfo.flags;
        layer->alpha = layerInfo.alpha;
        layer->unsupported = true;
        layer->textureIdentity[0] = '\0';

        bzTTModelTextureInfo_t texInfo;
        if (!BZ_TTAsset_ModelTextureInfo(asset, layerInfo.texture_index, &texInfo)) {
            LOG_ONCE(modelIdentity, "layer-texture-missing",
                     "bz_quest_wc3_capture: model '%s' geoset %u layer %u missing texture info\n",
                     modelIdentity, geosetIndex, li);
            outGeoset->layerCount++;
            continue;
        }

        if (texInfo.replaceable_id != 0) {
            /* replaceable_id 1/2 (team color/glow) or per-entity override -
             * explicitly deferred to a later Quest layer, see this file's
             * header comment. Not a bug: log once, skip this layer only. */
            LOG_ONCE(modelIdentity, "replaceable-texture-deferred",
                     "bz_quest_wc3_capture: model '%s' geoset %u layer %u uses replaceable_id %u "
                     "(team color/glow/override) - deferred out of scope for renderer slice 5A, "
                     "layer skipped\n",
                     modelIdentity, geosetIndex, li, texInfo.replaceable_id);
            outGeoset->layerCount++;
            continue;
        }

        const bzTTAsset_t *texAsset = BZ_TTA_RegisterModelTexture(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                                                   asset, layerInfo.texture_index);
        if (!texAsset) {
            LOG_ONCE(modelIdentity, "direct-texture-registration-failed",
                     "bz_quest_wc3_capture: model '%s' geoset %u layer %u direct texture "
                     "registration failed\n",
                     modelIdentity, geosetIndex, li);
            outGeoset->layerCount++;
            continue;
        }

        char texIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
        texIdentity[0] = '\0';
        bzTTImageInfo_t imageInfo;
        if (BZ_TTAsset_Identity(texAsset, texIdentity, sizeof(texIdentity)) &&
            BZ_TTAsset_ImageInfo(texAsset, &imageInfo) && imageInfo.format == BZ_TTA_PIXEL_RGBA8 &&
            imageInfo.origin == BZ_TTA_ORIGIN_TOP_LEFT) {
            if (imageInfo.data_bytes > BZ_QUEST_WC3_MAX_TEXTURE_BYTES ||
                imageInfo.width > BZ_QUEST_WC3_MAX_TEXTURE_DIM ||
                imageInfo.height > BZ_QUEST_WC3_MAX_TEXTURE_DIM) {
                LOG_ONCE(texIdentity, "texture-too-large",
                         "bz_quest_wc3_capture: texture '%s' is %ux%u (%u bytes), exceeds this "
                         "slice's %u-byte staging cap - layer skipped\n",
                         texIdentity, imageInfo.width, imageInfo.height, imageInfo.data_bytes,
                         (uint32_t)BZ_QUEST_WC3_MAX_TEXTURE_BYTES);
            } else {
                uint32_t copied =
                    BZ_TTAsset_CopyImagePixels(texAsset, s_scratchPixels, sizeof(s_scratchPixels));
                if (copied == imageInfo.data_bytes && callbacks && callbacks->onTextureReady) {
                    callbacks->onTextureReady(texIdentity, imageInfo.width, imageInfo.height,
                                              imageInfo.row_bytes, s_scratchPixels, copied,
                                              callbacks->textureUserdata);
                }
                strncpy(layer->textureIdentity, texIdentity, sizeof(layer->textureIdentity) - 1);
                layer->unsupported = false;
            }
        } else {
            LOG_ONCE(modelIdentity, "direct-texture-info-unavailable",
                     "bz_quest_wc3_capture: model '%s' geoset %u layer %u direct texture info "
                     "unavailable or in an unsupported pixel/origin format\n",
                     modelIdentity, geosetIndex, li);
        }
        BZ_TTAsset_Release(texAsset);
        outGeoset->layerCount++;
    }
}

/* Decodes one geoset's vertices/normals/UVs/indices into `model`'s combined
 * arrays at the given cursors, applying the Z-up -> Y-up axis swap and the
 * triangle winding fix (see bz_quest_wc3_render.h's coordinate evidence).
 * Returns false (geoset skipped, not the whole model) on overflow/malformed
 * data; `*vertexCursor`/`*indexCursor` are only advanced on success. */
static bool decode_geoset_geometry(const bzTTAsset_t *asset, const char *modelIdentity,
                                   uint32_t geosetIndex, const bzTTGeosetInfo_t *gi,
                                   bzQuestWc3Model_t *model, uint32_t *vertexCursor,
                                   uint32_t *indexCursor) {
    if (gi->vertex_count > BZ_QUEST_WC3_MAX_VERTS_PER_MODEL ||
        gi->index_count > BZ_QUEST_WC3_MAX_INDICES_PER_MODEL ||
        *vertexCursor + gi->vertex_count > BZ_QUEST_WC3_MAX_VERTS_PER_MODEL ||
        *indexCursor + gi->index_count > BZ_QUEST_WC3_MAX_INDICES_PER_MODEL) {
        LOG_ONCE(modelIdentity, "geoset-overflow",
                 "bz_quest_wc3_capture: model '%s' geoset %u (%u verts, %u indices) overflows "
                 "this slice's per-model scratch capacity - geoset skipped\n",
                 modelIdentity, geosetIndex, gi->vertex_count, gi->index_count);
        return false;
    }
    if (gi->index_count % 3 != 0) {
        LOG_ONCE(modelIdentity, "geoset-non-triangle",
                 "bz_quest_wc3_capture: model '%s' geoset %u has a non-multiple-of-3 index count "
                 "(%u) - geoset skipped\n",
                 modelIdentity, geosetIndex, gi->index_count);
        return false;
    }

    uint32_t vc = BZ_TTAsset_CopyGeosetVertices(asset, geosetIndex, s_stagePositions, gi->vertex_count);
    uint32_t nc = BZ_TTAsset_CopyGeosetNormals(asset, geosetIndex, s_stageNormals, gi->vertex_count);
    uint32_t uc = BZ_TTAsset_CopyGeosetUVs(asset, geosetIndex, s_stageUVs, gi->vertex_count);
    uint32_t ic = BZ_TTAsset_CopyGeosetIndices(asset, geosetIndex, s_stageIndices, gi->index_count);
    if (vc != gi->vertex_count || nc != gi->vertex_count || uc != gi->vertex_count ||
        ic != gi->index_count) {
        LOG_ONCE(modelIdentity, "geoset-copy-mismatch",
                 "bz_quest_wc3_capture: model '%s' geoset %u copy returned fewer elements than "
                 "advertised (verts %u/%u normals %u/%u uvs %u/%u indices %u/%u) - geoset skipped\n",
                 modelIdentity, geosetIndex, vc, gi->vertex_count, nc, gi->vertex_count, uc,
                 gi->vertex_count, ic, gi->index_count);
        return false;
    }

    uint32_t vertexOffset = *vertexCursor;
    for (uint32_t v = 0; v < gi->vertex_count; v++) {
        bzQuestWc3Vertex_t *dst = &model->vertices[vertexOffset + v];
        /* Axis swap Y<->Z - see bz_quest_wc3_render.h's coordinate evidence. */
        dst->position[0] = s_stagePositions[v].x;
        dst->position[1] = s_stagePositions[v].z;
        dst->position[2] = s_stagePositions[v].y;
        dst->normal[0] = s_stageNormals[v].x;
        dst->normal[1] = s_stageNormals[v].z;
        dst->normal[2] = s_stageNormals[v].y;
        dst->uv[0] = s_stageUVs[v].x;
        dst->uv[1] = s_stageUVs[v].y;
    }

    uint32_t triangleCount = gi->index_count / 3;
    uint32_t indexOffset = *indexCursor;
    for (uint32_t t = 0; t < triangleCount; t++) {
        uint16_t i0 = s_stageIndices[3 * t + 0];
        uint16_t i1 = s_stageIndices[3 * t + 1];
        uint16_t i2 = s_stageIndices[3 * t + 2];
        /* Winding fix [i0,i1,i2] -> [i0,i2,i1] - see bz_quest_wc3_render.h's
         * coordinate evidence (the axis swap alone flips handedness). */
        model->indices[indexOffset + 3 * t + 0] = vertexOffset + i0;
        model->indices[indexOffset + 3 * t + 1] = vertexOffset + i2;
        model->indices[indexOffset + 3 * t + 2] = vertexOffset + i1;
    }

    *vertexCursor += gi->vertex_count;
    *indexCursor += gi->index_count;
    return true;
}

/* Fully decodes `asset` (a registered, retained model asset) into the one
 * static s_scratchModel scratch buffer and invokes
 * callbacks->onModelReady/onTextureReady - see this file's header comment's
 * model/texture decode policy. `identity` must already have been resolved
 * via BZ_TTAsset_Identity(asset, ...). */
static void decode_model(const bzTTAsset_t *asset, const char *identity,
                         const bzQuestWc3CaptureCallbacks_t *callbacks) {
    memset(&s_scratchModel, 0, sizeof(s_scratchModel));
    strncpy(s_scratchModel.meta.identity, identity, sizeof(s_scratchModel.meta.identity) - 1);

    bzTTModelInfo_t modelInfo;
    if (!BZ_TTAsset_ModelInfo(asset, &modelInfo)) {
        LOG_ONCE(identity, "model-info-missing",
                 "bz_quest_wc3_capture: model '%s' has no ModelInfo - not rendered\n", identity);
        return;
    }
    uint32_t geosetCount = modelInfo.geoset_count;
    if (geosetCount > BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL) {
        LOG_ONCE(identity, "model-too-many-geosets",
                 "bz_quest_wc3_capture: model '%s' has %u geosets (max %u supported this slice) - "
                 "truncating to the first %u\n",
                 identity, geosetCount, BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL,
                 BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL);
        geosetCount = BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL;
    }

    uint32_t vertexCursor = 0, indexCursor = 0, outGeosetCount = 0;
    for (uint32_t g = 0; g < geosetCount; g++) {
        bzTTGeosetInfo_t gi;
        if (!BZ_TTAsset_GeosetInfo(asset, g, &gi)) {
            LOG_ONCE(identity, "geoset-info-missing",
                     "bz_quest_wc3_capture: model '%s' geoset %u missing GeosetInfo - skipped\n",
                     identity, g);
            continue;
        }
        bzQuestWc3Geoset_t *outGeoset = &s_scratchModel.meta.geosets[outGeosetCount];
        memset(outGeoset, 0, sizeof(*outGeoset));
        outGeoset->vertexOffset = vertexCursor;
        outGeoset->indexOffset = indexCursor;
        if (!decode_geoset_geometry(asset, identity, g, &gi, &s_scratchModel, &vertexCursor,
                                    &indexCursor)) {
            continue; /* geoset skipped, model continues with the rest */
        }
        outGeoset->vertexCount = vertexCursor - outGeoset->vertexOffset;
        outGeoset->indexCount = indexCursor - outGeoset->indexOffset;
        decode_layers(asset, identity, g, gi.material_index, outGeoset, callbacks);
        outGeosetCount++;
    }

    s_scratchModel.meta.vertexCount = vertexCursor;
    s_scratchModel.meta.indexCount = indexCursor;
    s_scratchModel.meta.geosetCount = outGeosetCount;

    if (callbacks && callbacks->onModelReady) {
        callbacks->onModelReady(&s_scratchModel, callbacks->modelUserdata);
    }
}

static bool already_seen_model(uint32_t modelConfigIndex) {
    for (uint32_t i = 0; i < s_seenModelCount; i++) {
        if (s_seenModelConfigIndex[i] == modelConfigIndex) return true;
    }
    return false;
}

void bz_quest_wc3_capture_frame(const bzQuestWc3CaptureCallbacks_t *callbacks,
                                bzQuestWc3RenderList_t *outRenderList) {
    memset(outRenderList, 0, sizeof(*outRenderList));
    s_seenModelCount = 0;

    static bzQuestWc3EntityInput_t entityInputs[BZ_QUEST_WC3_MAX_RENDER_ITEMS];
    uint32_t entityInputCount = 0;

    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    if (!snap) return;
    if (BZ_TTSnapshot_AbiVersion(snap) != BZ_TABLETOP_ABI_VERSION) {
        BZ_TTSnapshot_Release(snap);
        return;
    }
    if (BZ_TTA_AbiVersion() != BZ_TABLETOP_ASSETS_ABI_VERSION) {
        LOG_ONCE("<process>", "assets-abi-mismatch",
                 "bz_quest_wc3_capture: assets ABI version mismatch (built for %u, provider is "
                 "%u) - no Warcraft entities decoded this frame\n",
                 BZ_TABLETOP_ASSETS_ABI_VERSION, BZ_TTA_AbiVersion());
        BZ_TTSnapshot_Release(snap);
        return;
    }

    uint32_t entityCount = BZ_TTSnapshot_EntityCount(snap);
    for (uint32_t i = 0; i < entityCount; i++) {
        bzTTEntity_t entity;
        if (!BZ_TTSnapshot_EntityAt(snap, i, &entity)) continue;
        if (entity.model == 0) continue;

        bzTTEntityMetadataInput_t metaInput;
        memset(&metaInput, 0, sizeof(metaInput));
        bzTTAssetMetadata_t metadata;
        memset(&metadata, 0, sizeof(metadata));
        if (entity.class_id != 0) {
            metaInput.class_id = entity.class_id;
            metaInput.override_mask = BZ_TTA_METADATA_OVERRIDE_TEAM_COLOR;
            metaInput.team_color = entity.player;
            BZ_TTA_ResolveEntityMetadata(BZ_TABLETOP_ASSETS_ABI_VERSION, &metaInput, &metadata);
        } else {
            /* No class identity (e.g. a model-only effect) - team_color/tint
             * only, category/footprint stay at their zero (UNKNOWN/0) default,
             * matching LiveTabletopTransport.swift:377-386's fallback path. */
            metadata.team_color = entity.player;
        }

        const bzTTAsset_t *modelAsset = BZ_TTA_RegisterConfigString(
            BZ_TABLETOP_ASSETS_ABI_VERSION, snap, kModelConfigStringBase + entity.model,
            BZ_TTA_ASSET_MODEL, &metadata);
        if (!modelAsset) {
            LOG_ONCE("<process>", "model-registration-failed",
                     "bz_quest_wc3_capture: model registration failed for config-string index "
                     "%u - some entities will not be rendered\n",
                     entity.model);
            continue;
        }

        char identity[BZ_QUEST_WC3_MAX_IDENTITY];
        identity[0] = '\0';
        if (!BZ_TTAsset_IsPlaceholder(modelAsset) &&
            BZ_TTAsset_Identity(modelAsset, identity, sizeof(identity))) {
            if (entityInputCount < BZ_QUEST_WC3_MAX_RENDER_ITEMS) {
                bzQuestWc3EntityInput_t *in = &entityInputs[entityInputCount++];
                in->originX = entity.origin_x;
                in->originY = entity.origin_y;
                in->originZ = entity.origin_z;
                in->angle = entity.angle;
                in->footprintX = metadata.footprint_x;
                in->footprintY = metadata.footprint_y;
                in->category = metadata.category;
                strncpy(in->modelIdentity, identity, sizeof(in->modelIdentity) - 1);
            }

            if (!already_seen_model(entity.model)) {
                if (s_seenModelCount < BZ_QUEST_WC3_MAX_UNIQUE_MODELS_PER_FRAME) {
                    s_seenModelConfigIndex[s_seenModelCount++] = entity.model;
                    decode_model(modelAsset, identity, callbacks);
                } else {
                    LOG_ONCE("<process>", "unique-model-overflow",
                             "bz_quest_wc3_capture: more than %u unique models touched in one "
                             "frame - some models will not be (re-)decoded this frame\n",
                             (uint32_t)BZ_QUEST_WC3_MAX_UNIQUE_MODELS_PER_FRAME);
                }
            }
        }
        BZ_TTAsset_Release(modelAsset);
    }

    bz_quest_wc3_build_render_list(entityInputs, entityInputCount, outRenderList);
    BZ_TTSnapshot_Release(snap);
}
