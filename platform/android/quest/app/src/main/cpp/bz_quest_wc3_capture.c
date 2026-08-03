/*
 * bz_quest_wc3_capture.c - see bz_quest_wc3_capture.h.
 */
#include "bz_quest_wc3_capture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/bridge/bz_tabletop_assets.h"
#include "platform/bridge/bz_tabletop_transport.h"

/* LiveTabletopTransport.swift:212 - "common/shared.h's frozen CS_MODELS base". */
static const uint32_t kModelConfigStringBase = 32;

/* r_mdx.h:49 (MDLXNODE_Billboarded = 8) - see this file's header comment on
 * why this file mirrors small MDX constant values instead of #including
 * games/warcraft-3/renderer/mdx/r_mdx.h (that header pulls in the desktop
 * engine's own internal renderer/r_local.h dependency surface, which this
 * Quest-host module must stay free of). Raw MDX node flags are passed
 * through bzTTNodeInfo_t.flags unfiltered by the ABI (see that struct's
 * comment), so this bit is checked directly against the ABI's own value -
 * no ABI-side "is billboarded" accessor exists to cross-check against. */
static const uint32_t kMdxNodeBillboarded = 0x8;

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

/* bz_quest_wc3_hud.h mirrors bzTTActionTarget_t/bzTTActionSemantic_t/
 * bzTTGameResult_t values by raw cast (see bz_quest_wc3_capture_hud() below)
 * rather than #including the bridge ABI header from that pure module - this
 * cross-check makes a future transport renumbering fail the build here
 * instead of silently mis-mapping a HUD state on device, matching this
 * file's BZ_TTA_BLEND_* precedent immediately above. */
_Static_assert((int)BZ_QUEST_HUD_TARGET_NONE == (int)BZ_TT_ACTION_TARGET_NONE &&
                   (int)BZ_QUEST_HUD_TARGET_POINT == (int)BZ_TT_ACTION_TARGET_POINT &&
                   (int)BZ_QUEST_HUD_TARGET_ENTITY == (int)BZ_TT_ACTION_TARGET_ENTITY &&
                   (int)BZ_QUEST_HUD_TARGET_ENTITY_OR_POINT == (int)BZ_TT_ACTION_TARGET_ENTITY_OR_POINT,
               "bz_quest_wc3_hud.h's bzQuestHudActionTarget_t has drifted from bzTTActionTarget_t");
_Static_assert((int)BZ_QUEST_HUD_SEMANTIC_UNSUPPORTED == (int)BZ_TT_ACTION_UNSUPPORTED &&
                   (int)BZ_QUEST_HUD_SEMANTIC_BUTTON == (int)BZ_TT_ACTION_BUTTON &&
                   (int)BZ_QUEST_HUD_SEMANTIC_CANCEL == (int)BZ_TT_ACTION_CANCEL,
               "bz_quest_wc3_hud.h's bzQuestHudActionSemantic_t has drifted from bzTTActionSemantic_t");
_Static_assert((int)BZ_QUEST_HUD_GAME_RESULT_NONE == (int)BZ_TT_GAME_RESULT_NONE &&
                   (int)BZ_QUEST_HUD_GAME_RESULT_VICTORY == (int)BZ_TT_GAME_RESULT_VICTORY &&
                   (int)BZ_QUEST_HUD_GAME_RESULT_DEFEAT == (int)BZ_TT_GAME_RESULT_DEFEAT &&
                   (int)BZ_QUEST_HUD_GAME_RESULT_DRAW == (int)BZ_TT_GAME_RESULT_DRAW,
               "bz_quest_wc3_hud.h's bzQuestHudGameResult_t has drifted from bzTTGameResult_t");
_Static_assert(BZ_QUEST_HUD_MAX_BUTTONS == BZ_TT_MAX_COMMAND_BUTTONS,
               "bz_quest_wc3_hud.h's BZ_QUEST_HUD_MAX_BUTTONS has drifted from BZ_TT_MAX_COMMAND_BUTTONS");

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
/* Per-geoset resolved top-4 vertex skin (BZ_TTAsset_CopyGeosetVertexSkin) -
 * always present per bzTTGeosetInfo_t's "always has a resolved skin"
 * guarantee (see bz_quest_wc3_render.h's bzQuestWc3Vertex_t comment), so
 * every geoset's vertices get real boneIndex/boneWeight, animated or not. */
static bzTTVertexSkin_t s_stageVertexSkin[BZ_QUEST_WC3_MAX_VERTS_PER_MODEL];
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

/* Resolves `texAsset`'s identity, validates+copies its pixels through the
 * shared scratch-pixel buffer, and offers them via
 * callbacks->onTextureReady - shared by direct model textures (decode_layers)
 * and per-entity team-color/glow textures (bz_quest_wc3_capture_frame).
 * Writes the resolved identity into `outIdentity` (outIdentityCap bytes) and
 * returns true on success; on any failure `outIdentity[0]` is left '\0' and
 * this function has already logged once against `logIdentity`. Does not
 * retain/release `texAsset` - the caller owns its lifetime. */
static bool decode_and_offer_texture(const bzTTAsset_t *texAsset, const char *logIdentity,
                                     char *outIdentity, size_t outIdentityCap,
                                     const bzQuestWc3CaptureCallbacks_t *callbacks) {
    outIdentity[0] = '\0';
    char texIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
    texIdentity[0] = '\0';
    bzTTImageInfo_t imageInfo;
    if (!BZ_TTAsset_Identity(texAsset, texIdentity, sizeof(texIdentity)) ||
        !BZ_TTAsset_ImageInfo(texAsset, &imageInfo) || imageInfo.format != BZ_TTA_PIXEL_RGBA8 ||
        imageInfo.origin != BZ_TTA_ORIGIN_TOP_LEFT) {
        LOG_ONCE(logIdentity, "texture-info-unavailable",
                 "bz_quest_wc3_capture: texture info for '%s' unavailable or in an unsupported "
                 "pixel/origin format\n",
                 logIdentity);
        return false;
    }
    if (imageInfo.data_bytes > BZ_QUEST_WC3_MAX_TEXTURE_BYTES ||
        imageInfo.width > BZ_QUEST_WC3_MAX_TEXTURE_DIM || imageInfo.height > BZ_QUEST_WC3_MAX_TEXTURE_DIM) {
        LOG_ONCE(texIdentity, "texture-too-large",
                 "bz_quest_wc3_capture: texture '%s' is %ux%u (%u bytes), exceeds this slice's "
                 "%u-byte staging cap - skipped\n",
                 texIdentity, imageInfo.width, imageInfo.height, imageInfo.data_bytes,
                 (uint32_t)BZ_QUEST_WC3_MAX_TEXTURE_BYTES);
        return false;
    }
    uint32_t copied = BZ_TTAsset_CopyImagePixels(texAsset, s_scratchPixels, sizeof(s_scratchPixels));
    if (copied != imageInfo.data_bytes) {
        LOG_ONCE(texIdentity, "texture-copy-mismatch",
                 "bz_quest_wc3_capture: texture '%s' pixel copy returned %u/%u bytes - skipped\n",
                 texIdentity, copied, imageInfo.data_bytes);
        return false;
    }
    if (callbacks && callbacks->onTextureReady) {
        callbacks->onTextureReady(texIdentity, imageInfo.width, imageInfo.height,
                                  imageInfo.row_bytes, s_scratchPixels, copied,
                                  callbacks->textureUserdata);
    }
    strncpy(outIdentity, texIdentity, outIdentityCap - 1);
    outIdentity[outIdentityCap - 1] = '\0';
    return true;
}

/* Resolves one geoset's material's layers into bzQuestWc3Geoset_t.layers -
 * replaceable_id 0 (direct texture) is fully resolved and reported via
 * callbacks->onTextureReady; replaceable_id 1/2 (team color/glow) are marked
 * via bzQuestWc3LayerDesc_t.teamColor/teamGlow with textureIdentity left
 * empty, since the concrete texture depends on the entity's team and is
 * resolved per-entity by bz_quest_wc3_capture_frame(), not here; any other
 * replaceable_id (per-entity image override) is marked unsupported and
 * logged once (see this file's header comment's texture decode policy). */
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
        layer->teamColor = false;
        layer->teamGlow = false;
        layer->textureIdentity[0] = '\0';

        bzTTModelTextureInfo_t texInfo;
        if (!BZ_TTAsset_ModelTextureInfo(asset, layerInfo.texture_index, &texInfo)) {
            LOG_ONCE(modelIdentity, "layer-texture-missing",
                     "bz_quest_wc3_capture: model '%s' geoset %u layer %u missing texture info\n",
                     modelIdentity, geosetIndex, li);
            outGeoset->layerCount++;
            continue;
        }

        if (texInfo.replaceable_id == 1 || texInfo.replaceable_id == 2) {
            /* Team color (1) / team glow (2) - the concrete texture is
             * per-ENTITY (each entity's own team_color), not per-model, so
             * this file only marks the ROLE here; bz_quest_wc3_capture_frame
             * resolves+decodes the actual per-entity texture via
             * BZ_TTA_RegisterTeamTexture() and the renderer binds it from
             * the draw's own bzQuestWc3RenderItem_t::teamColorTexture
             * Identity/teamGlowTextureIdentity - see this file's header
             * comment and bzQuestWc3LayerDesc_t's doc comment. */
            layer->unsupported = false;
            layer->teamColor = (texInfo.replaceable_id == 1);
            layer->teamGlow = (texInfo.replaceable_id == 2);
            outGeoset->layerCount++;
            continue;
        }
        if (texInfo.replaceable_id != 0) {
            /* Any other replaceable_id: a per-entity image override
             * (entity.metadata.image + BZ_TTA_ASSET_IMAGE) - explicitly
             * deferred to a later Quest layer, see this file's header
             * comment. Not a bug: log once, skip this layer only. */
            LOG_ONCE(modelIdentity, "replaceable-texture-deferred",
                     "bz_quest_wc3_capture: model '%s' geoset %u layer %u uses replaceable_id %u "
                     "(per-entity image override) - deferred out of scope for renderer slice 5C, "
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
        layer->unsupported = !decode_and_offer_texture(texAsset, modelIdentity, layer->textureIdentity,
                                                       sizeof(layer->textureIdentity), callbacks);
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
    uint32_t sc = BZ_TTAsset_CopyGeosetVertexSkin(asset, geosetIndex, s_stageVertexSkin, gi->vertex_count);
    if (vc != gi->vertex_count || nc != gi->vertex_count || uc != gi->vertex_count ||
        ic != gi->index_count || sc != gi->vertex_count) {
        LOG_ONCE(modelIdentity, "geoset-copy-mismatch",
                 "bz_quest_wc3_capture: model '%s' geoset %u copy returned fewer elements than "
                 "advertised (verts %u/%u normals %u/%u uvs %u/%u indices %u/%u skin %u/%u) - "
                 "geoset skipped\n",
                 modelIdentity, geosetIndex, vc, gi->vertex_count, nc, gi->vertex_count, uc,
                 gi->vertex_count, ic, gi->index_count, sc, gi->vertex_count);
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
        memcpy(dst->boneIndex, s_stageVertexSkin[v].bone_index, sizeof(dst->boneIndex));
        memcpy(dst->boneWeight, s_stageVertexSkin[v].bone_weight, sizeof(dst->boneWeight));
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

/* A bump allocator over a single heap block. Used in two modes: with
 * `base == NULL` (sizing-only - `anim_take()` just advances `offset` and
 * returns NULL, no memory is touched) and with `base` pointing at a real
 * malloc()'d block of exactly the size the sizing pass computed (fill mode -
 * `anim_take()` returns a real writable pointer). build_model_anim() below
 * runs the exact same code path in both modes, so the two passes can never
 * disagree about how many bytes are needed - see that function's comment. */
typedef struct { uint8_t *base; size_t offset; } bzQuestAnimArena_t;

static size_t anim_arena_align(size_t v) { return (v + (size_t)7u) & ~(size_t)7u; }

static void *anim_arena_take(bzQuestAnimArena_t *arena, size_t bytes) {
    if (bytes == 0) return NULL;
    size_t start = anim_arena_align(arena->offset);
    arena->offset = start + bytes;
    return arena->base ? (void *)(arena->base + start) : NULL;
}

/* Builds an owned bzQuestWc3ModelAnim_t arena for `asset`, covering nodes
 * (with parent_id already resolved to array indices - see
 * bz_quest_wc3_render.h's bzQuestWc3StoredNode_t comment), sequence
 * [start,end) ranges, global sequence durations, and per-geoset alpha/
 * bone-palette animation data. `rawToOutGeoset[g]` maps a raw ABI geoset
 * index (0..rawGeosetCount) to its compacted, failure-skipping
 * bzQuestWc3ModelMeta_t::geosets[] index, or UINT32_MAX if that raw geoset
 * was skipped by decode_model()'s geometry pass - `geosetAnims` is built
 * index-aligned with that same compacted array (outGeosetCount entries).
 *
 * Runs a first Info()-only sizing pass (no ABI Copy*() calls - see
 * bzQuestAnimArena_t's comment) to compute the exact arena size, then a
 * second pass that repeats the identical control flow with a real
 * malloc()'d base, this time issuing the ABI Copy*() calls straight into
 * arena-relative addresses. One malloc(), one bz_quest_wc3_model_anim_free()
 * releases it. Returns NULL (never a partially-filled arena) when the model
 * genuinely has no animation data worth retaining, or when the allocation
 * fails (logged once; the model still renders via its static bind pose). */
static bzQuestWc3ModelAnim_t *build_model_anim(const bzTTAsset_t *asset, const char *identity,
                                               const bzTTModelInfo_t *modelInfo,
                                               const uint32_t *rawToOutGeoset, uint32_t rawGeosetCount,
                                               uint32_t outGeosetCount,
                                               const bzQuestWc3CaptureCallbacks_t *callbacks) {
    uint32_t nodeCount = modelInfo->node_count;
    if (nodeCount > BZ_QUEST_WC3_MAX_NODES_PER_MODEL) {
        LOG_ONCE(identity, "model-too-many-nodes",
                 "bz_quest_wc3_capture: model '%s' has %u nodes (max %u supported this slice) - "
                 "truncating to the first %u\n",
                 identity, nodeCount, (uint32_t)BZ_QUEST_WC3_MAX_NODES_PER_MODEL,
                 (uint32_t)BZ_QUEST_WC3_MAX_NODES_PER_MODEL);
        nodeCount = BZ_QUEST_WC3_MAX_NODES_PER_MODEL;
    }
    uint32_t sequenceCount = modelInfo->sequence_count;
    if (sequenceCount > BZ_QUEST_WC3_MAX_SEQUENCES_PER_MODEL) {
        LOG_ONCE(identity, "model-too-many-sequences",
                 "bz_quest_wc3_capture: model '%s' has %u sequences (max %u supported this "
                 "slice) - truncating to the first %u\n",
                 identity, sequenceCount, (uint32_t)BZ_QUEST_WC3_MAX_SEQUENCES_PER_MODEL,
                 (uint32_t)BZ_QUEST_WC3_MAX_SEQUENCES_PER_MODEL);
        sequenceCount = BZ_QUEST_WC3_MAX_SEQUENCES_PER_MODEL;
    }
    uint32_t globalSeqCount = modelInfo->global_sequence_count;
    if (globalSeqCount > BZ_QUEST_WC3_MAX_GLOBAL_SEQUENCES_PER_MODEL) {
        LOG_ONCE(identity, "model-too-many-global-sequences",
                 "bz_quest_wc3_capture: model '%s' has %u global sequences (max %u supported "
                 "this slice) - truncating to the first %u\n",
                 identity, globalSeqCount, (uint32_t)BZ_QUEST_WC3_MAX_GLOBAL_SEQUENCES_PER_MODEL,
                 (uint32_t)BZ_QUEST_WC3_MAX_GLOBAL_SEQUENCES_PER_MODEL);
        globalSeqCount = BZ_QUEST_WC3_MAX_GLOBAL_SEQUENCES_PER_MODEL;
    }
    uint32_t emitterCount = modelInfo->emitter_count;
    if (emitterCount > BZ_QUEST_WC3_MAX_EMITTERS_PER_MODEL) {
        LOG_ONCE(identity, "model-too-many-emitters",
                 "bz_quest_wc3_capture: model '%s' has %u PRE2 particle emitters (max %u "
                 "supported this slice) - truncating to the first %u\n",
                 identity, emitterCount, (uint32_t)BZ_QUEST_WC3_MAX_EMITTERS_PER_MODEL,
                 (uint32_t)BZ_QUEST_WC3_MAX_EMITTERS_PER_MODEL);
        emitterCount = BZ_QUEST_WC3_MAX_EMITTERS_PER_MODEL;
    }

    /* Bounded Info()-only scratch, queried exactly once here and reused by
     * both passes below so they can never diverge in what they decide to
     * copy (see this function's two-pass arena comment). */
    static bzTTNodeInfo_t s_animNodeInfo[BZ_QUEST_WC3_MAX_NODES_PER_MODEL];
    static bzTTTrackInfo_t s_animTransInfo[BZ_QUEST_WC3_MAX_NODES_PER_MODEL];
    static bzTTTrackInfo_t s_animRotInfo[BZ_QUEST_WC3_MAX_NODES_PER_MODEL];
    static bzTTTrackInfo_t s_animScaleInfo[BZ_QUEST_WC3_MAX_NODES_PER_MODEL];
    for (uint32_t n = 0; n < nodeCount; n++) {
        if (!BZ_TTAsset_NodeInfo(asset, n, &s_animNodeInfo[n])) {
            LOG_ONCE(identity, "node-info-missing",
                     "bz_quest_wc3_capture: model '%s' node %u missing NodeInfo - treated as an "
                     "identity root node\n",
                     identity, n);
            memset(&s_animNodeInfo[n], 0, sizeof(s_animNodeInfo[n]));
            s_animNodeInfo[n].parent_id = UINT32_MAX;
        }
        /* Camera-facing billboard nodes (MDLXNODE_Billboarded) are out of
         * scope for this slice - only the parent-chain hierarchy transform
         * is applied (bz_quest_wc3_build_pose()), never a per-frame
         * camera-facing override (that would need this frame's view
         * orientation threaded into pose building, which no authoritative
         * snapshot/ABI field currently exposes - see this file's header
         * comment on scoped exclusions). A billboarded node still poses
         * correctly for everything this slice DOES claim (hierarchy/
         * keyframe/sequence correctness); it just does not additionally
         * face the viewer, a real but bounded, diagnosable gap - logged
         * once per model, never silently dropped/frozen. */
        if (s_animNodeInfo[n].flags & kMdxNodeBillboarded) {
            LOG_ONCE(identity, "node-billboard-unsupported",
                     "bz_quest_wc3_capture: model '%s' node %u is billboarded (MDLXNODE_"
                     "Billboarded) - camera-facing override not implemented this slice, node "
                     "still poses via its normal parent-chain hierarchy transform\n",
                     identity, n);
        }
        if (!BZ_TTAsset_NodeTrackInfo(asset, n, BZ_TTA_NODE_TRANSLATION, &s_animTransInfo[n]))
            memset(&s_animTransInfo[n], 0, sizeof(s_animTransInfo[n]));
        if (!BZ_TTAsset_NodeTrackInfo(asset, n, BZ_TTA_NODE_ROTATION, &s_animRotInfo[n]))
            memset(&s_animRotInfo[n], 0, sizeof(s_animRotInfo[n]));
        if (!BZ_TTAsset_NodeTrackInfo(asset, n, BZ_TTA_NODE_SCALE, &s_animScaleInfo[n]))
            memset(&s_animScaleInfo[n], 0, sizeof(s_animScaleInfo[n]));
        if (s_animTransInfo[n].key_count > BZ_QUEST_WC3_MAX_KEYS_PER_TRACK) {
            LOG_ONCE(identity, "node-track-too-many-keys",
                     "bz_quest_wc3_capture: model '%s' node %u translation track has %u keys "
                     "(max %u supported this slice) - truncating\n",
                     identity, n, s_animTransInfo[n].key_count,
                     (uint32_t)BZ_QUEST_WC3_MAX_KEYS_PER_TRACK);
            s_animTransInfo[n].key_count = BZ_QUEST_WC3_MAX_KEYS_PER_TRACK;
        }
        if (s_animRotInfo[n].key_count > BZ_QUEST_WC3_MAX_KEYS_PER_TRACK) {
            LOG_ONCE(identity, "node-track-too-many-keys",
                     "bz_quest_wc3_capture: model '%s' node %u rotation track has %u keys "
                     "(max %u supported this slice) - truncating\n",
                     identity, n, s_animRotInfo[n].key_count,
                     (uint32_t)BZ_QUEST_WC3_MAX_KEYS_PER_TRACK);
            s_animRotInfo[n].key_count = BZ_QUEST_WC3_MAX_KEYS_PER_TRACK;
        }
        if (s_animScaleInfo[n].key_count > BZ_QUEST_WC3_MAX_KEYS_PER_TRACK) {
            LOG_ONCE(identity, "node-track-too-many-keys",
                     "bz_quest_wc3_capture: model '%s' node %u scale track has %u keys (max %u "
                     "supported this slice) - truncating\n",
                     identity, n, s_animScaleInfo[n].key_count,
                     (uint32_t)BZ_QUEST_WC3_MAX_KEYS_PER_TRACK);
            s_animScaleInfo[n].key_count = BZ_QUEST_WC3_MAX_KEYS_PER_TRACK;
        }
    }

    static bzTTSequenceInfo_t s_animSeqInfo[BZ_QUEST_WC3_MAX_SEQUENCES_PER_MODEL];
    for (uint32_t s = 0; s < sequenceCount; s++) {
        if (!BZ_TTAsset_SequenceInfo(asset, s, &s_animSeqInfo[s])) {
            LOG_ONCE(identity, "sequence-info-missing",
                     "bz_quest_wc3_capture: model '%s' sequence %u missing SequenceInfo - treated "
                     "as a zero-length [0,0) interval\n",
                     identity, s);
            memset(&s_animSeqInfo[s], 0, sizeof(s_animSeqInfo[s]));
        }
    }

    static uint32_t s_animGlobalSeqDuration[BZ_QUEST_WC3_MAX_GLOBAL_SEQUENCES_PER_MODEL];
    for (uint32_t g = 0; g < globalSeqCount; g++) {
        if (!BZ_TTAsset_GlobalSequenceInfo(asset, g, &s_animGlobalSeqDuration[g])) {
            LOG_ONCE(identity, "global-sequence-info-missing",
                     "bz_quest_wc3_capture: model '%s' global sequence %u missing duration - "
                     "treated as a zero-length loop\n",
                     identity, g);
            s_animGlobalSeqDuration[g] = 0;
        }
    }

    /* PRE2 particle emitters (ABI v4) - the only MDX effect class this project's authoritative
     * runtime actually parses+simulates+draws end to end, see bz_quest_wc3_particles.h's header
     * comment for the full audit. Each emitter's own node hierarchy/pivot/KGTR-KGRT-KGSC track
     * already lives in s_animNodeInfo/s_animTransInfo/etc above (an emitter IS just another
     * node - platform/bridge/bz_tabletop_assets.h's bzTTParticleEmitterInfo_t doc comment).
     *
     * Only 4 of PRE2's 8 animatable float channels ever need a track sampled here (visibility/
     * emissionRate/speed/gravity) - width/length/latitude/variation are deliberately excluded:
     * the authoritative renderer's own MDLX_RenderEmitter samples all 7 physical channels but
     * only ever reads back the sampled EmissionRate/Speed/Gravity locals, reading
     * emitter->Width/Length/Latitude's STATIC field directly at their call sites instead (and
     * never reading the sampled Variation local at all) - see
     * bzQuestWc3StoredEmitter_t's doc comment (bz_quest_wc3_render.h) for the full git-blame-
     * verified citation. Requesting track info the Quest renderer will never sample would just
     * be wasted ABI calls/arena space for data that can never affect a frame. */
    static const bzTTEmitterChannel_t kEmitterTrackChannels[4] = {
        BZ_TTA_EMITTER_VISIBILITY, BZ_TTA_EMITTER_EMISSION_RATE, BZ_TTA_EMITTER_SPEED, BZ_TTA_EMITTER_GRAVITY,
    };
    static bzTTParticleEmitterInfo_t s_animEmitterInfo[BZ_QUEST_WC3_MAX_EMITTERS_PER_MODEL];
    static bzTTTrackInfo_t s_animEmitterTrackInfo[BZ_QUEST_WC3_MAX_EMITTERS_PER_MODEL][4];
    for (uint32_t e = 0; e < emitterCount; e++) {
        if (!BZ_TTAsset_ParticleEmitterInfo(asset, e, &s_animEmitterInfo[e])) {
            LOG_ONCE(identity, "emitter-info-missing",
                     "bz_quest_wc3_capture: model '%s' emitter %u missing ParticleEmitterInfo - "
                     "this emitter will not spawn particles\n",
                     identity, e);
            memset(&s_animEmitterInfo[e], 0, sizeof(s_animEmitterInfo[e]));
            s_animEmitterInfo[e].node_index = UINT32_MAX; /* sentinel: resolved-away below */
        } else if (s_animEmitterInfo[e].node_index >= nodeCount) {
            /* Only reachable if the emitter's own node fell past
             * BZ_QUEST_WC3_MAX_NODES_PER_MODEL's truncation point above - the emitter itself is
             * still decoded (so a future larger node cap could revive it), but it cannot spawn
             * without a resolvable node this frame. */
            LOG_ONCE(identity, "emitter-node-truncated",
                     "bz_quest_wc3_capture: model '%s' emitter %u references node %u, past this "
                     "slice's %u-node cap - this emitter will not spawn particles\n",
                     identity, e, s_animEmitterInfo[e].node_index, nodeCount);
        }
        for (uint32_t ci = 0; ci < 4; ci++) {
            bzTTEmitterChannel_t ch = kEmitterTrackChannels[ci];
            if (!BZ_TTAsset_EmitterTrackInfo(asset, e, ch, &s_animEmitterTrackInfo[e][ci]))
                memset(&s_animEmitterTrackInfo[e][ci], 0, sizeof(s_animEmitterTrackInfo[e][ci]));
            if (s_animEmitterTrackInfo[e][ci].key_count > BZ_QUEST_WC3_MAX_KEYS_PER_TRACK) {
                LOG_ONCE(identity, "emitter-track-too-many-keys",
                         "bz_quest_wc3_capture: model '%s' emitter %u channel %u track has %u "
                         "keys (max %u supported this slice) - truncating\n",
                         identity, e, (uint32_t)ch, s_animEmitterTrackInfo[e][ci].key_count,
                         (uint32_t)BZ_QUEST_WC3_MAX_KEYS_PER_TRACK);
                s_animEmitterTrackInfo[e][ci].key_count = BZ_QUEST_WC3_MAX_KEYS_PER_TRACK;
            }
        }
    }

    /* geoset-anim Info, keyed by the COMPACTED output geoset index (so
     * geosetAnims[] ends up index-aligned with meta.geosets[] even when
     * earlier raw geosets were skipped) - s_animOutToRawGeoset lets the fill
     * pass call the ABI's raw-index Copy*() functions back. */
    static bzTTGeosetAnimInfo_t s_animGeosetAnimInfo[BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL];
    static uint32_t s_animPaletteCount[BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL];
    static uint32_t s_animOutToRawGeoset[BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL];
    for (uint32_t g = 0; g < rawGeosetCount; g++) {
        uint32_t outIndex = rawToOutGeoset[g];
        if (outIndex == UINT32_MAX) continue;
        s_animOutToRawGeoset[outIndex] = g;
        bzTTGeosetInfo_t gi;
        s_animPaletteCount[outIndex] = 0;
        if (BZ_TTAsset_GeosetInfo(asset, g, &gi)) {
            s_animPaletteCount[outIndex] = gi.matrix_palette_count;
            if (s_animPaletteCount[outIndex] > BZ_QUEST_WC3_MAX_MATRIX_PALETTE)
                s_animPaletteCount[outIndex] = BZ_QUEST_WC3_MAX_MATRIX_PALETTE;
        }
        if (!BZ_TTAsset_GeosetAnimInfo(asset, g, &s_animGeosetAnimInfo[outIndex])) {
            memset(&s_animGeosetAnimInfo[outIndex], 0, sizeof(s_animGeosetAnimInfo[outIndex]));
            s_animGeosetAnimInfo[outIndex].static_alpha = 1.0f;
        }
        if (s_animGeosetAnimInfo[outIndex].alpha_track.key_count > BZ_QUEST_WC3_MAX_KEYS_PER_TRACK) {
            LOG_ONCE(identity, "geoset-alpha-too-many-keys",
                     "bz_quest_wc3_capture: model '%s' geoset %u alpha track has %u keys (max %u "
                     "supported this slice) - truncating\n",
                     identity, outIndex, s_animGeosetAnimInfo[outIndex].alpha_track.key_count,
                     (uint32_t)BZ_QUEST_WC3_MAX_KEYS_PER_TRACK);
            s_animGeosetAnimInfo[outIndex].alpha_track.key_count = BZ_QUEST_WC3_MAX_KEYS_PER_TRACK;
        }
    }

    /* Raw ABI-shaped scratch for Copy*Keys()/CopyGeosetMatrixPalette()
     * output, reused across every node/geoset before being reshaped (field
     * renames only - same layout) into this project's own Quest key/index
     * types written into the arena below. */
    static bzTTVec3Key_t s_animRawVec3Keys[BZ_QUEST_WC3_MAX_KEYS_PER_TRACK];
    static bzTTQuatKey_t s_animRawQuatKeys[BZ_QUEST_WC3_MAX_KEYS_PER_TRACK];
    static bzTTFloatKey_t s_animRawFloatKeys[BZ_QUEST_WC3_MAX_KEYS_PER_TRACK];
    static uint32_t s_animRawPalette[BZ_QUEST_WC3_MAX_MATRIX_PALETTE];

    bzQuestAnimArena_t arena;
    memset(&arena, 0, sizeof(arena));
    bool ok = true;
    for (int pass = 0; pass < 2 && ok; pass++) {
        arena.offset = 0;
        bzQuestWc3StoredNode_t *nodes =
            (bzQuestWc3StoredNode_t *)anim_arena_take(&arena, (size_t)nodeCount * sizeof(*nodes));
        bzQuestWc3StoredSeqRange_t *sequences = (bzQuestWc3StoredSeqRange_t *)anim_arena_take(
            &arena, (size_t)sequenceCount * sizeof(*sequences));
        uint32_t *globalSeqDurations =
            (uint32_t *)anim_arena_take(&arena, (size_t)globalSeqCount * sizeof(uint32_t));
        bzQuestWc3StoredGeosetAnim_t *geosetAnims = (bzQuestWc3StoredGeosetAnim_t *)anim_arena_take(
            &arena, (size_t)outGeosetCount * sizeof(*geosetAnims));
        bzQuestWc3StoredEmitter_t *emitters =
            (bzQuestWc3StoredEmitter_t *)anim_arena_take(&arena, (size_t)emitterCount * sizeof(*emitters));

        for (uint32_t n = 0; n < nodeCount; n++) {
            uint32_t parentIndex = BZ_QUEST_WC3_NO_PARENT;
            if (s_animNodeInfo[n].parent_id != UINT32_MAX) {
                for (uint32_t p = 0; p < nodeCount; p++) {
                    if (s_animNodeInfo[p].object_id == s_animNodeInfo[n].parent_id) {
                        parentIndex = p;
                        break;
                    }
                }
                if (parentIndex == BZ_QUEST_WC3_NO_PARENT) {
                    LOG_ONCE(identity, "node-parent-unresolved",
                             "bz_quest_wc3_capture: model '%s' node %u parent_id %u does not "
                             "match any decoded node - treated as a root node\n",
                             identity, n, s_animNodeInfo[n].parent_id);
                }
            }

            bzQuestWc3StoredTrack_t translation, rotation, scale;
            memset(&translation, 0, sizeof(translation));
            memset(&rotation, 0, sizeof(rotation));
            memset(&scale, 0, sizeof(scale));

            translation.keyCount = s_animTransInfo[n].key_count;
            translation.interp = (bzQuestWc3Interp_t)s_animTransInfo[n].interp;
            translation.globalSequence = (s_animTransInfo[n].global_sequence == BZ_TTA_NO_GLOBAL_SEQUENCE)
                                              ? BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE
                                              : s_animTransInfo[n].global_sequence;
            if (translation.keyCount > 0) {
                bzQuestWc3Vec3Key_t *dst = (bzQuestWc3Vec3Key_t *)anim_arena_take(
                    &arena, (size_t)translation.keyCount * sizeof(*dst));
                translation.vec3Keys = dst;
                if (dst) {
                    uint32_t got = BZ_TTAsset_CopyNodeTranslationKeys(asset, n, s_animRawVec3Keys,
                                                                       translation.keyCount);
                    for (uint32_t k = 0; k < got && k < translation.keyCount; k++) {
                        dst[k].timeMsec = s_animRawVec3Keys[k].time_msec;
                        dst[k].value.x = s_animRawVec3Keys[k].value.x;
                        dst[k].value.y = s_animRawVec3Keys[k].value.y;
                        dst[k].value.z = s_animRawVec3Keys[k].value.z;
                        dst[k].inTan.x = s_animRawVec3Keys[k].in_tan.x;
                        dst[k].inTan.y = s_animRawVec3Keys[k].in_tan.y;
                        dst[k].inTan.z = s_animRawVec3Keys[k].in_tan.z;
                        dst[k].outTan.x = s_animRawVec3Keys[k].out_tan.x;
                        dst[k].outTan.y = s_animRawVec3Keys[k].out_tan.y;
                        dst[k].outTan.z = s_animRawVec3Keys[k].out_tan.z;
                    }
                }
            }

            rotation.keyCount = s_animRotInfo[n].key_count;
            rotation.interp = (bzQuestWc3Interp_t)s_animRotInfo[n].interp;
            rotation.globalSequence = (s_animRotInfo[n].global_sequence == BZ_TTA_NO_GLOBAL_SEQUENCE)
                                           ? BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE
                                           : s_animRotInfo[n].global_sequence;
            if (rotation.keyCount > 0) {
                bzQuestWc3QuatKey_t *dst = (bzQuestWc3QuatKey_t *)anim_arena_take(
                    &arena, (size_t)rotation.keyCount * sizeof(*dst));
                rotation.quatKeys = dst;
                if (dst) {
                    uint32_t got =
                        BZ_TTAsset_CopyNodeRotationKeys(asset, n, s_animRawQuatKeys, rotation.keyCount);
                    for (uint32_t k = 0; k < got && k < rotation.keyCount; k++) {
                        dst[k].timeMsec = s_animRawQuatKeys[k].time_msec;
                        dst[k].value.x = s_animRawQuatKeys[k].value.x;
                        dst[k].value.y = s_animRawQuatKeys[k].value.y;
                        dst[k].value.z = s_animRawQuatKeys[k].value.z;
                        dst[k].value.w = s_animRawQuatKeys[k].value.w;
                        dst[k].inTan.x = s_animRawQuatKeys[k].in_tan.x;
                        dst[k].inTan.y = s_animRawQuatKeys[k].in_tan.y;
                        dst[k].inTan.z = s_animRawQuatKeys[k].in_tan.z;
                        dst[k].inTan.w = s_animRawQuatKeys[k].in_tan.w;
                        dst[k].outTan.x = s_animRawQuatKeys[k].out_tan.x;
                        dst[k].outTan.y = s_animRawQuatKeys[k].out_tan.y;
                        dst[k].outTan.z = s_animRawQuatKeys[k].out_tan.z;
                        dst[k].outTan.w = s_animRawQuatKeys[k].out_tan.w;
                    }
                }
            }

            scale.keyCount = s_animScaleInfo[n].key_count;
            scale.interp = (bzQuestWc3Interp_t)s_animScaleInfo[n].interp;
            scale.globalSequence = (s_animScaleInfo[n].global_sequence == BZ_TTA_NO_GLOBAL_SEQUENCE)
                                        ? BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE
                                        : s_animScaleInfo[n].global_sequence;
            if (scale.keyCount > 0) {
                bzQuestWc3Vec3Key_t *dst =
                    (bzQuestWc3Vec3Key_t *)anim_arena_take(&arena, (size_t)scale.keyCount * sizeof(*dst));
                scale.vec3Keys = dst;
                if (dst) {
                    uint32_t got = BZ_TTAsset_CopyNodeScaleKeys(asset, n, s_animRawVec3Keys, scale.keyCount);
                    for (uint32_t k = 0; k < got && k < scale.keyCount; k++) {
                        dst[k].timeMsec = s_animRawVec3Keys[k].time_msec;
                        dst[k].value.x = s_animRawVec3Keys[k].value.x;
                        dst[k].value.y = s_animRawVec3Keys[k].value.y;
                        dst[k].value.z = s_animRawVec3Keys[k].value.z;
                        dst[k].inTan.x = s_animRawVec3Keys[k].in_tan.x;
                        dst[k].inTan.y = s_animRawVec3Keys[k].in_tan.y;
                        dst[k].inTan.z = s_animRawVec3Keys[k].in_tan.z;
                        dst[k].outTan.x = s_animRawVec3Keys[k].out_tan.x;
                        dst[k].outTan.y = s_animRawVec3Keys[k].out_tan.y;
                        dst[k].outTan.z = s_animRawVec3Keys[k].out_tan.z;
                    }
                }
            }

            if (nodes) {
                nodes[n].parentIndex = parentIndex;
                nodes[n].pivot.x = s_animNodeInfo[n].pivot.x;
                nodes[n].pivot.y = s_animNodeInfo[n].pivot.y;
                nodes[n].pivot.z = s_animNodeInfo[n].pivot.z;
                nodes[n].translation = translation;
                nodes[n].rotation = rotation;
                nodes[n].scale = scale;
            }
        }

        for (uint32_t s = 0; s < sequenceCount; s++) {
            if (sequences) {
                sequences[s].startMsec = s_animSeqInfo[s].start_msec;
                sequences[s].endMsec = s_animSeqInfo[s].end_msec;
            }
        }

        for (uint32_t g = 0; g < globalSeqCount; g++) {
            if (globalSeqDurations) globalSeqDurations[g] = s_animGlobalSeqDuration[g];
        }

        for (uint32_t outIndex = 0; outIndex < outGeosetCount; outIndex++) {
            bzQuestWc3StoredGeosetAnim_t ga;
            memset(&ga, 0, sizeof(ga));
            ga.hasAlphaTrack = s_animGeosetAnimInfo[outIndex].has_alpha_track;
            ga.staticAlpha = s_animGeosetAnimInfo[outIndex].static_alpha;
            ga.alphaTrack.keyCount = s_animGeosetAnimInfo[outIndex].alpha_track.key_count;
            ga.alphaTrack.interp = (bzQuestWc3Interp_t)s_animGeosetAnimInfo[outIndex].alpha_track.interp;
            ga.alphaTrack.globalSequence =
                (s_animGeosetAnimInfo[outIndex].alpha_track.global_sequence == BZ_TTA_NO_GLOBAL_SEQUENCE)
                    ? BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE
                    : s_animGeosetAnimInfo[outIndex].alpha_track.global_sequence;
            if (ga.alphaTrack.keyCount > 0) {
                bzQuestWc3FloatKey_t *dst = (bzQuestWc3FloatKey_t *)anim_arena_take(
                    &arena, (size_t)ga.alphaTrack.keyCount * sizeof(*dst));
                ga.alphaTrack.floatKeys = dst;
                if (dst) {
                    uint32_t got = BZ_TTAsset_CopyGeosetAlphaKeys(
                        asset, s_animOutToRawGeoset[outIndex], s_animRawFloatKeys, ga.alphaTrack.keyCount);
                    for (uint32_t k = 0; k < got && k < ga.alphaTrack.keyCount; k++) {
                        dst[k].timeMsec = s_animRawFloatKeys[k].time_msec;
                        dst[k].value = s_animRawFloatKeys[k].value;
                        dst[k].inTan = s_animRawFloatKeys[k].in_tan;
                        dst[k].outTan = s_animRawFloatKeys[k].out_tan;
                    }
                }
            }
            ga.paletteNodeIndexCount = s_animPaletteCount[outIndex];
            if (ga.paletteNodeIndexCount > 0) {
                uint32_t *dst =
                    (uint32_t *)anim_arena_take(&arena, (size_t)ga.paletteNodeIndexCount * sizeof(uint32_t));
                ga.paletteNodeIndices = dst;
                if (dst) {
                    uint32_t got = BZ_TTAsset_CopyGeosetMatrixPalette(
                        asset, s_animOutToRawGeoset[outIndex], s_animRawPalette, ga.paletteNodeIndexCount);
                    for (uint32_t k = 0; k < got && k < ga.paletteNodeIndexCount; k++) dst[k] = s_animRawPalette[k];
                }
            }
            if (geosetAnims) geosetAnims[outIndex] = ga;
        }

        for (uint32_t e = 0; e < emitterCount; e++) {
            bzQuestWc3StoredEmitter_t em;
            memset(&em, 0, sizeof(em));
            const bzTTParticleEmitterInfo_t *ei = &s_animEmitterInfo[e];
            em.nodeIndex = ei->node_index < nodeCount ? ei->node_index : BZ_QUEST_WC3_NO_PARENT;
            em.blendMode = ei->blend_mode;
            em.headOrTail = ei->head_or_tail;
            em.rows = ei->rows;
            em.columns = ei->columns;
            em.speed = ei->speed; em.latitude = ei->latitude;
            em.gravity = ei->gravity; em.lifeSpan = ei->life_span; em.emissionRate = ei->emission_rate;
            em.length = ei->length; em.width = ei->width;
            em.tailLength = ei->tail_length; em.timeMiddle = ei->time_middle;
            memcpy(em.segmentColor, ei->segment_color, sizeof(em.segmentColor));
            memcpy(em.segmentAlpha, ei->segment_alpha, sizeof(em.segmentAlpha));
            memcpy(em.particleScaling, ei->particle_scaling, sizeof(em.particleScaling));

            /* Texture resolution: exactly mirrors decode_layers()'s own three-way
             * replaceable_id contract (see bzQuestWc3StoredEmitter_t's doc comment) - reuses
             * decode_and_offer_texture() rather than a second copy of that decode/validate/
             * offer control flow (DRY). */
            em.unsupported = true;
            bzTTModelTextureInfo_t texInfo;
            if (!BZ_TTAsset_ModelTextureInfo(asset, ei->texture_index, &texInfo)) {
                LOG_ONCE(identity, "emitter-texture-missing",
                         "bz_quest_wc3_capture: model '%s' emitter %u missing texture info\n",
                         identity, e);
            } else if (texInfo.replaceable_id == 1 || texInfo.replaceable_id == 2) {
                em.unsupported = false;
                em.teamColor = (texInfo.replaceable_id == 1);
                em.teamGlow = (texInfo.replaceable_id == 2);
            } else if (texInfo.replaceable_id != 0) {
                LOG_ONCE(identity, "emitter-replaceable-texture-deferred",
                         "bz_quest_wc3_capture: model '%s' emitter %u uses replaceable_id %u "
                         "(per-entity image override) - deferred out of scope, emitter spawns "
                         "no particles\n",
                         identity, e, texInfo.replaceable_id);
            } else {
                const bzTTAsset_t *texAsset =
                    BZ_TTA_RegisterModelTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, asset, ei->texture_index);
                if (!texAsset) {
                    LOG_ONCE(identity, "emitter-texture-registration-failed",
                             "bz_quest_wc3_capture: model '%s' emitter %u direct texture "
                             "registration failed\n",
                             identity, e);
                } else {
                    em.unsupported = !decode_and_offer_texture(texAsset, identity, em.textureIdentity,
                                                               sizeof(em.textureIdentity), callbacks);
                    BZ_TTAsset_Release(texAsset);
                }
            }

            /* Table-driven: every one of these 4 channels is a plain float track (unlike node
             * translation/rotation/scale's 3 distinct value types), so one small loop over
             * {channel, destination field} pairs covers all 4 - see bzTTEmitterChannel_t's own
             * doc comment on why this is DRY versus 4 near-identical hand-written blocks. Only
             * visibility/emissionRate/speed/gravity get a track at all here - see
             * bzQuestWc3StoredEmitter_t's doc comment (bz_quest_wc3_render.h) for why
             * width/length/latitude/variation are deliberately excluded. */
            bzQuestWc3StoredTrack_t *trackFields[4] = {
                &em.visibilityTrack, &em.emissionRateTrack, &em.speedTrack, &em.gravityTrack,
            };
            for (uint32_t ci = 0; ci < 4; ci++) {
                const bzTTTrackInfo_t *ti = &s_animEmitterTrackInfo[e][ci];
                bzQuestWc3StoredTrack_t *track = trackFields[ci];
                track->keyCount = ti->key_count;
                track->interp = (bzQuestWc3Interp_t)ti->interp;
                track->globalSequence = (ti->global_sequence == BZ_TTA_NO_GLOBAL_SEQUENCE)
                                            ? BZ_QUEST_WC3_NO_GLOBAL_SEQUENCE
                                            : ti->global_sequence;
                if (track->keyCount > 0) {
                    bzQuestWc3FloatKey_t *dst =
                        (bzQuestWc3FloatKey_t *)anim_arena_take(&arena, (size_t)track->keyCount * sizeof(*dst));
                    track->floatKeys = dst;
                    if (dst) {
                        uint32_t got = BZ_TTAsset_CopyEmitterFloatKeys(asset, e, kEmitterTrackChannels[ci],
                                                                       s_animRawFloatKeys, track->keyCount);
                        for (uint32_t k = 0; k < got && k < track->keyCount; k++) {
                            dst[k].timeMsec = s_animRawFloatKeys[k].time_msec;
                            dst[k].value = s_animRawFloatKeys[k].value;
                            dst[k].inTan = s_animRawFloatKeys[k].in_tan;
                            dst[k].outTan = s_animRawFloatKeys[k].out_tan;
                        }
                    }
                }
            }
            if (emitters) emitters[e] = em;
        }

        if (pass == 0) {
            if (arena.offset == 0) {
                ok = false; /* legitimately no animation data worth retaining */
                break;
            }
            uint8_t *base = (uint8_t *)malloc(arena.offset);
            if (!base) {
                LOG_ONCE(identity, "anim-arena-oom",
                         "bz_quest_wc3_capture: model '%s' animation arena allocation failed (%zu "
                         "bytes) - model renders via its static bind pose only\n",
                         identity, arena.offset);
                ok = false;
                break;
            }
            arena.base = base;
        } else {
            bzQuestWc3ModelAnim_t *anim = (bzQuestWc3ModelAnim_t *)malloc(sizeof(*anim));
            if (!anim) {
                LOG_ONCE(identity, "anim-struct-oom",
                         "bz_quest_wc3_capture: model '%s' animation struct allocation failed - "
                         "model renders via its static bind pose only\n",
                         identity);
                free(arena.base);
                return NULL;
            }
            anim->arena = arena.base;
            anim->nodeCount = nodeCount;
            anim->nodes = nodes;
            anim->sequenceCount = sequenceCount;
            anim->sequences = sequences;
            anim->globalSeqDurationCount = globalSeqCount;
            anim->globalSeqDurations = globalSeqDurations;
            anim->geosetAnimCount = outGeosetCount;
            anim->geosetAnims = geosetAnims;
            anim->emitterCount = emitterCount;
            anim->emitters = emitters;
            return anim;
        }
    }
    return NULL;
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
    static uint32_t s_rawToOutGeoset[BZ_QUEST_WC3_MAX_GEOSETS_PER_MODEL];
    for (uint32_t g = 0; g < geosetCount; g++) {
        s_rawToOutGeoset[g] = UINT32_MAX;
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
        s_rawToOutGeoset[g] = outGeosetCount;
        outGeosetCount++;
    }

    s_scratchModel.meta.vertexCount = vertexCursor;
    s_scratchModel.meta.indexCount = indexCursor;
    s_scratchModel.meta.geosetCount = outGeosetCount;

    /* build_model_anim() malloc()s an owned bzQuestWc3ModelAnim_t arena
     * whose ONLY free/ownership-transfer path is onModelReady (see
     * model_ready_cb()'s doc comment in bz_quest_vk_wc3.c - every one of
     * its branches either frees the arena or transfers it into the
     * persistent model cache). With no onModelReady consumer, nothing
     * would ever do either: s_scratchModel is a static scratch buffer
     * memset() at the top of the next decode_model() call, so the arena
     * pointer would simply be overwritten and lost - a leak. Building the
     * arena at all is also pointless work when nobody will read it. So,
     * matching this file's own header comment's documented "NULL callback
     * is safe/no-op, geometry/texture decode is simply skipped" contract,
     * skip the allocation entirely when there is no consumer. */
    s_scratchModel.meta.anim = (callbacks && callbacks->onModelReady)
        ? build_model_anim(asset, identity, &modelInfo, s_rawToOutGeoset, geosetCount, outGeosetCount, callbacks)
        : NULL;

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

    /* Shared world/tabletop transform (bz_quest_wc3_render.h) derived from
     * this snapshot's own map bounds - the SAME transform terrain uses
     * (bz_quest_wc3_terrain_measure()) and fog uses
     * (bz_quest_wc3_capture_fog() below), applied exactly once per entity
     * inside bz_quest_wc3_build_render_list()/build_world_matrix(). `haveTransform
     * = false` (bounds missing/degenerate) means entity translations pass
     * through RAW/unscaled this frame - a defensive fallback, logged once,
     * not a silent misplacement: if bounds are genuinely absent there is no
     * terrain/fog to align with either, so raw passthrough is no worse than
     * the pre-fix behavior it replaces. */
    bzTTBox2_t mapBounds;
    bzQuestWc3WorldTransform_t transform;
    bool haveTransform = BZ_TTSnapshot_MapBounds(snap, &mapBounds) &&
                         bz_quest_wc3_world_transform_measure(mapBounds.min_x, mapBounds.min_y, mapBounds.max_x,
                                                              mapBounds.max_y, &transform);
    if (!haveTransform && entityCount > 0) {
        LOG_ONCE("<process>", "entity-transform-bounds-missing",
                 "bz_quest_wc3_capture: %u entities arrived with no valid map bounds - entity "
                 "positions render in raw/unscaled space this frame (will not align with terrain)\n",
                 entityCount);
    }

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
                in->tintR = metadata.tint_r;
                in->tintG = metadata.tint_g;
                in->tintB = metadata.tint_b;
                in->tintA = metadata.tint_a;
                in->category = metadata.category;
                in->frame = entity.frame;
                in->selected = entity.selected;
                in->teamColorTextureIdentity[0] = '\0';
                in->teamGlowTextureIdentity[0] = '\0';
                strncpy(in->modelIdentity, identity, sizeof(in->modelIdentity) - 1);

                /* Team color/glow are resolved per-ENTITY (not per-model,
                 * see decode_layers()'s comment) - only attempted when the
                 * provider actually authored that many team images, per
                 * BZ_TTA_TeamTextureCount()'s doc comment ("[0, count)").
                 * A model with no teamColor/teamGlow layer simply never
                 * uses these identities (bzQuestWc3LayerDesc_t gates the
                 * bind), so an unconditional best-effort attempt here is
                 * safe and keeps this file decoupled from per-model layer
                 * introspection. */
                if (metadata.team_color < BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                                                   BZ_TTA_TEAM_TEXTURE_COLOR)) {
                    const bzTTAsset_t *teamColorAsset = BZ_TTA_RegisterTeamTexture(
                        BZ_TABLETOP_ASSETS_ABI_VERSION, BZ_TTA_TEAM_TEXTURE_COLOR, metadata.team_color);
                    if (teamColorAsset) {
                        decode_and_offer_texture(teamColorAsset, identity, in->teamColorTextureIdentity,
                                                 sizeof(in->teamColorTextureIdentity), callbacks);
                        BZ_TTAsset_Release(teamColorAsset);
                    }
                }
                if (metadata.team_color < BZ_TTA_TeamTextureCount(BZ_TABLETOP_ASSETS_ABI_VERSION,
                                                                   BZ_TTA_TEAM_TEXTURE_GLOW)) {
                    const bzTTAsset_t *teamGlowAsset = BZ_TTA_RegisterTeamTexture(
                        BZ_TABLETOP_ASSETS_ABI_VERSION, BZ_TTA_TEAM_TEXTURE_GLOW, metadata.team_color);
                    if (teamGlowAsset) {
                        decode_and_offer_texture(teamGlowAsset, identity, in->teamGlowTextureIdentity,
                                                 sizeof(in->teamGlowTextureIdentity), callbacks);
                        BZ_TTAsset_Release(teamGlowAsset);
                    }
                }
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

    bz_quest_wc3_build_render_list(entityInputs, entityInputCount, haveTransform ? &transform : NULL,
                                   outRenderList);
    BZ_TTSnapshot_Release(snap);
}

bool bz_quest_wc3_capture_fog(bzQuestWc3FogCapture_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    if (!snap) return false;
    if (BZ_TTSnapshot_AbiVersion(snap) != BZ_TABLETOP_ABI_VERSION) {
        BZ_TTSnapshot_Release(snap);
        return false;
    }

    const bzTTPlayer_t *player = BZ_TTSnapshot_Player(snap);
    out->targetMode = player ? (uint32_t)player->target : (uint32_t)BZ_TT_ACTION_TARGET_NONE;
    if (out->targetMode != (uint32_t)BZ_TT_ACTION_TARGET_NONE) {
        char detail[48];
        snprintf(detail, sizeof(detail), "target-mode-%u-no-location", out->targetMode);
        LOG_ONCE("<process>", detail,
                 "bz_quest_wc3_capture: target mode %u is authoritative but has no transported point/entity payload; "
                 "layer 5D draws no Quest target marker from it\n",
                 out->targetMode);
    }

    if (!BZ_TTSnapshot_FogDimensions(snap, &out->width, &out->height)) {
        BZ_TTSnapshot_Release(snap);
        return false;
    }
    if (!bz_quest_wc3_fog_grid_supported(out->width, out->height)) {
        LOG_ONCE("<process>", "fog-grid-too-large",
                 "bz_quest_wc3_capture: fog grid %ux%u exceeds Quest layer 5D's real Warcraft III cap (%ux%u) - fog unavailable this frame\n",
                 out->width, out->height, (uint32_t)BZ_QUEST_WC3_FOG_MAX_CELLS_PER_AXIS,
                 (uint32_t)BZ_QUEST_WC3_FOG_MAX_CELLS_PER_AXIS);
        BZ_TTSnapshot_Release(snap);
        return false;
    }

    bzTTBox2_t bounds;
    if (!BZ_TTSnapshot_MapBounds(snap, &bounds)) {
        LOG_ONCE("<process>", "fog-bounds-missing",
                 "bz_quest_wc3_capture: fog grid %ux%u arrived with no map bounds - fog unavailable this frame\n",
                 out->width, out->height);
        BZ_TTSnapshot_Release(snap);
        return false;
    }
    out->bounds.minX = bounds.min_x;
    out->bounds.minY = bounds.min_y;
    out->bounds.maxX = bounds.max_x;
    out->bounds.maxY = bounds.max_y;

    /* Same shared world/tabletop transform bz_quest_wc3_capture_frame()
     * derives for entities, from the SAME map bounds - see
     * bz_quest_wc3_render.h's header comment. A fog grid cannot be placed
     * on screen without a valid transform (there would be nothing correct
     * to align it to either), so - unlike entities' defensive raw-
     * passthrough fallback above - degenerate bounds here fail the whole
     * capture, matching bz_quest_wc3_terrain_measure()'s own
     * BZ_QUEST_WC3_TERRAIN_ERR_INVALID_BOUNDS treatment of the same bounds
     * value for terrain. */
    if (!bz_quest_wc3_world_transform_measure(out->bounds.minX, out->bounds.minY, out->bounds.maxX,
                                              out->bounds.maxY, &out->transform)) {
        LOG_ONCE("<process>", "fog-bounds-degenerate",
                 "bz_quest_wc3_capture: fog grid %ux%u arrived with degenerate map bounds "
                 "(min_x=%f min_y=%f max_x=%f max_y=%f) - fog unavailable this frame\n",
                 out->width, out->height, bounds.min_x, bounds.min_y, bounds.max_x, bounds.max_y);
        memset(out, 0, sizeof(*out));
        BZ_TTSnapshot_Release(snap);
        return false;
    }

    uint32_t cells = bz_quest_wc3_fog_cell_count(out->width, out->height);
    uint32_t visibleBytes = BZ_TTSnapshot_FogVisible(snap, out->visible, cells);
    uint32_t exploredBytes = BZ_TTSnapshot_FogExplored(snap, out->explored, cells);
    if (visibleBytes != cells || exploredBytes != cells) {
        LOG_ONCE("<process>", "fog-copy-mismatch",
                 "bz_quest_wc3_capture: fog copy returned %u/%u visible bytes and %u/%u explored bytes - fog unavailable this frame\n",
                 visibleBytes, cells, exploredBytes, cells);
        memset(out, 0, sizeof(*out));
        BZ_TTSnapshot_Release(snap);
        return false;
    }

    out->available = true;
    BZ_TTSnapshot_Release(snap);
    return true;
}

bool bz_quest_wc3_capture_hud(bzQuestHudInput_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    if (!snap) return false;
    if (BZ_TTSnapshot_AbiVersion(snap) != BZ_TABLETOP_ABI_VERSION) {
        BZ_TTSnapshot_Release(snap);
        return false;
    }

    out->frameId = BZ_TTSnapshot_Generation(snap);

    const bzTTPlayer_t *player = BZ_TTSnapshot_Player(snap);
    if (player) {
        out->player.present = true;
        strncpy(out->player.name, player->name, sizeof(out->player.name) - 1);
        out->player.gold = player->resource_gold;
        out->player.lumber = player->resource_lumber;
        out->player.foodUsed = player->resource_food_used;
        out->player.foodCap = player->resource_food_cap;
        out->player.heroTokens = player->resource_hero_tokens;
        out->player.gameResult = (bzQuestHudGameResult_t)player->game_result;
    }

    uint32_t selectedIds[BZ_TT_MAX_SELECTED_ENTITIES];
    out->selectedCount = BZ_TTSnapshot_SelectedEntityIds(snap, selectedIds, BZ_TT_MAX_SELECTED_ENTITIES);

    const bzTTActionLayout_t *layout = BZ_TTSnapshot_ActionLayout(snap);
    if (layout) {
        out->actionLayout.present = layout->present;
        out->actionLayout.visible = layout->visible;
        out->actionLayout.valid = layout->valid;
        out->actionLayout.currentTarget = (bzQuestHudActionTarget_t)layout->current_target;
        uint32_t numButtons = layout->num_buttons;
        if (numButtons > BZ_QUEST_HUD_MAX_BUTTONS) {
            LOG_ONCE("<process>", "hud-button-count-clamped",
                     "bz_quest_wc3_capture: action layout reported %u buttons, clamping to %u\n", numButtons,
                     (uint32_t)BZ_QUEST_HUD_MAX_BUTTONS);
            numButtons = BZ_QUEST_HUD_MAX_BUTTONS;
        }
        out->actionLayout.numButtons = (uint8_t)numButtons;
        for (uint32_t i = 0; i < numButtons; i++) {
            const bzTTActionButton_t *src = &layout->buttons[i];
            bzQuestHudButtonInput_t *dst = &out->actionLayout.buttons[i];
            strncpy(dst->actionCode, src->action_code, sizeof(dst->actionCode) - 1);
            strncpy(dst->tooltip, src->tooltip, sizeof(dst->tooltip) - 1);
            dst->hotkey = src->hotkey;
            dst->gridX = src->grid_x;
            dst->gridY = src->grid_y;
            dst->hidden = src->hidden;
            dst->disabled = src->disabled;
            dst->cooldown = src->cooldown;
            dst->target = (bzQuestHudActionTarget_t)src->target;
            dst->semantic = (bzQuestHudActionSemantic_t)src->semantic;
            /* image_index is deliberately never copied/resolved - see this
             * function's header comment and bz_quest_wc3_hud.h's full
             * evidence trail for why no ABI path can turn it into pixels. */
            if (src->image_index != 0) {
                char detail[48];
                snprintf(detail, sizeof(detail), "hud-icon-unresolvable-%u", src->image_index);
                LOG_ONCE("<process>", detail,
                         "bz_quest_wc3_capture: command-card button '%s' references image_index %u, which this "
                         "ABI cannot resolve to pixel data - rendering a flat placeholder quad instead of an icon\n",
                         src->action_code, src->image_index);
            }
        }
    }

    BZ_TTSnapshot_Release(snap);
    return true;
}

/* FNV-1a over the map name so a real map reload (name change) yields a new
 * epoch, while a mere generation bump (same map, new snapshot) does not. 0 is
 * reserved for "no name" so the very first real map still differs from init. */
static uint64_t bz_quest_map_epoch(const char *name) {
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = name; p && *p; ++p) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    return h;
}

void bz_quest_wc3_capture_interaction(bzQuestWc3InteractionCapture_t *out) {
    memset(out, 0, sizeof(*out));

    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    if (!snap) return;
    if (BZ_TTSnapshot_AbiVersion(snap) != BZ_TABLETOP_ABI_VERSION) {
        BZ_TTSnapshot_Release(snap);
        return;
    }

    out->generation = BZ_TTSnapshot_Generation(snap);

    char mapName[128];
    mapName[0] = '\0';
    if (BZ_TTSnapshot_MapName(snap, mapName, sizeof(mapName))) out->mapEpoch = bz_quest_map_epoch(mapName);

    const bzTTPlayer_t *player = BZ_TTSnapshot_Player(snap);
    out->targetMode = player ? (bzQuestHudActionTarget_t)player->target : BZ_QUEST_HUD_TARGET_NONE;

    out->selectedCount = BZ_TTSnapshot_SelectedEntityIds(snap, out->selectedIds, BZ_QUEST_INPUT_MAX_SELECT_IDS);

    bzTTBox2_t mapBounds;
    out->haveTransform = BZ_TTSnapshot_MapBounds(snap, &mapBounds) &&
                         bz_quest_wc3_world_transform_measure(mapBounds.min_x, mapBounds.min_y, mapBounds.max_x,
                                                              mapBounds.max_y, &out->transform);

    const bzQuestWc3WorldTransform_t *xf = out->haveTransform ? &out->transform : NULL;
    uint32_t entityCount = BZ_TTSnapshot_EntityCount(snap);
    for (uint32_t i = 0; i < entityCount && out->entityCount < BZ_QUEST_WC3_MAX_RENDER_ITEMS; i++) {
        bzTTEntity_t entity;
        if (!BZ_TTSnapshot_EntityAt(snap, i, &entity)) continue;
        if (entity.model == 0) continue; /* only entities that actually render are hittable */

        /* Category/footprint via the same metadata resolution the render/HUD
         * paths use (team_color override), so the hit-sphere radius matches
         * the rendered selection marker's footprint scale exactly. */
        bzTTEntityMetadataInput_t metaInput;
        memset(&metaInput, 0, sizeof(metaInput));
        bzTTAssetMetadata_t metadata;
        memset(&metadata, 0, sizeof(metadata));
        if (entity.class_id != 0) {
            metaInput.class_id = entity.class_id;
            metaInput.override_mask = BZ_TTA_METADATA_OVERRIDE_TEAM_COLOR;
            metaInput.team_color = entity.player;
            BZ_TTA_ResolveEntityMetadata(BZ_TABLETOP_ASSETS_ABI_VERSION, &metaInput, &metadata);
        }

        bzQuestInputEntity_t *e = &out->entities[out->entityCount++];
        e->number = entity.number;
        /* Z-up engine origin -> Y-up composed center, exactly as
         * bz_quest_wc3_build_world_matrix() places the rendered mesh
         * (world_transform_point(xf, originX, originZ, originY)). */
        float center[3];
        bz_quest_wc3_world_transform_point(xf, entity.origin_x, entity.origin_z, entity.origin_y, center);
        e->centerX = center[0];
        e->centerY = center[1];
        e->centerZ = center[2];
        /* Engine ground origin (x,y) for the documented target-entity-as-point
         * ABI-gap workaround - the authoritative coords BZ_TT_PostTargetPoint
         * expects, never a fabricated value. */
        e->engineX = entity.origin_x;
        e->engineNorth = entity.origin_y;
        /* Hit-sphere radius = the larger horizontal half-extent of the SAME
         * per-axis footprint/category scale the rendered selection marker uses
         * (bz_quest_wc3_entity_footprint_scale()), NOT bzTTEntity_t.radius
         * (raw WC3 units). A conservative enclosing sphere over the marker
         * footprint - see bz_quest_wc3_render.h's selection-marker comment. */
        float sx, sy, sz;
        bz_quest_wc3_entity_footprint_scale(metadata.category, metadata.footprint_x, metadata.footprint_y, &sx,
                                            &sy, &sz);
        e->radius = fmaxf(sx, sz);
        if (e->radius <= 0.0f) e->radius = 0.02f; /* floor: never a zero-radius (un-hittable) sphere */
    }

    out->available = true;
    BZ_TTSnapshot_Release(snap);
}

uint32_t bz_quest_wc3_render_clock_msec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    /* uint32_t wraps every ~49.7 days of continuous runtime - immaterial
     * here since this value only ever feeds a `% (globalSeqDuration + 1)`
     * wraparound (bz_quest_wc3_anim.h's header comment), the same class of
     * wraparound desktop's own SDL_GetTicks()-backed uint32_t tick counter
     * already tolerates. */
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
