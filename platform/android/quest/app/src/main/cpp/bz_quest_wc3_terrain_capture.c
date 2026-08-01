/*
 * bz_quest_wc3_terrain_capture.c - see bz_quest_wc3_terrain_capture.h.
 */
#include "bz_quest_wc3_terrain_capture.h"

#include <stdio.h>
#include <string.h>

#include "platform/bridge/bz_tabletop_assets.h"

_Static_assert(BZ_TTA_TERRAIN_CHUNK_TILES == BZ_QUEST_WC3_TERRAIN_CHUNK_TILES,
               "terrain chunk size drifted from bz_tabletop_assets.h");
_Static_assert(BZ_TTA_TERRAIN_MAP_EDGE == BZ_QUEST_WC3_TERRAIN_MAP_EDGE &&
                   BZ_TTA_TERRAIN_RAMP == BZ_QUEST_WC3_TERRAIN_RAMP &&
                   BZ_TTA_TERRAIN_BLIGHT == BZ_QUEST_WC3_TERRAIN_BLIGHT &&
                   BZ_TTA_TERRAIN_WATER == BZ_QUEST_WC3_TERRAIN_WATER &&
                   BZ_TTA_TERRAIN_BOUNDARY == BZ_QUEST_WC3_TERRAIN_BOUNDARY &&
                   BZ_TTA_TERRAIN_NO_CLIFF == BZ_QUEST_WC3_TERRAIN_NO_CLIFF,
               "terrain flag bits drifted from bz_tabletop_assets.h");
_Static_assert(BZ_TTA_TERRAIN_TEXTURE_GROUND == BZ_QUEST_WC3_TERRAIN_MATERIAL_GROUND &&
                   BZ_TTA_TERRAIN_TEXTURE_CLIFF == BZ_QUEST_WC3_TERRAIN_MATERIAL_CLIFF &&
                   BZ_TTA_TERRAIN_TEXTURE_WATER == BZ_QUEST_WC3_TERRAIN_MATERIAL_WATER,
               "terrain texture kind values drifted from bz_tabletop_assets.h");

static bzQuestWc3TerrainInput_t s_scratchTerrain;
static uint8_t s_scratchPixels[BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_BYTES];
static char s_lastTerrainKey[BZ_QUEST_WC3_MAX_IDENTITY];

enum { BZ_QUEST_WC3_TERRAIN_CAPTURE_MAX_LOGGED_KEYS = 256 };
static char s_loggedKeys[BZ_QUEST_WC3_TERRAIN_CAPTURE_MAX_LOGGED_KEYS][BZ_QUEST_WC3_TERRAIN_MAX_KEY];
static uint32_t s_loggedKeyCount;

static bool log_once(const char *identity, const char *detail) {
    char key[BZ_QUEST_WC3_TERRAIN_MAX_KEY];
    snprintf(key, sizeof(key), "%s|%s", identity, detail);
    for (uint32_t i = 0; i < s_loggedKeyCount; i++)
        if (strcmp(s_loggedKeys[i], key) == 0) return false;
    if (s_loggedKeyCount < BZ_QUEST_WC3_TERRAIN_CAPTURE_MAX_LOGGED_KEYS) {
        strncpy(s_loggedKeys[s_loggedKeyCount], key, sizeof(s_loggedKeys[0]) - 1);
        s_loggedKeyCount++;
    }
    return true;
}

#define LOG_ONCE(identity, detail, ...) \
    do { if (log_once((identity), (detail))) fprintf(stderr, __VA_ARGS__); } while (0)

/* Terrain generations are immutable handle snapshots; pointer+dims+bounds is a stable same-generation key. */
static void terrain_key(const bzTTTerrain_t *terrain, const bzTTTerrainInfo_t *info, char *out, size_t cap) {
    snprintf(out, cap, "<terrain:%p:%ux%u:%g,%g,%g,%g>", (const void *)terrain, info->width, info->height,
             info->min_x, info->min_y, info->max_x, info->max_y);
}

static int32_t find_type_index(const uint32_t *ids, uint32_t count, uint32_t id) {
    for (uint32_t i = 0; i < count; i++)
        if (ids[i] == id) return (int32_t)i;
    return -1;
}

static uint32_t copy_type_table(const bzTTTerrain_t *terrain, bool ground, uint32_t count, uint32_t maxCount,
                                uint32_t *out, const char *terrainIdentity, const char *label) {
    if (count > maxCount) {
        LOG_ONCE(terrainIdentity, label,
                 "bz_quest_wc3_terrain_capture: terrain '%s' has %u %s types (max %u) - terrain unavailable this generation\n",
                 terrainIdentity, count, ground ? "ground" : "cliff", maxCount);
        return 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        bool ok = ground ? BZ_TTTerrain_GroundType(terrain, i, &out[i]) : BZ_TTTerrain_CliffType(terrain, i, &out[i]);
        if (!ok) {
            LOG_ONCE(terrainIdentity, label,
                     "bz_quest_wc3_terrain_capture: terrain '%s' could not copy %s type %u\n",
                     terrainIdentity, ground ? "ground" : "cliff", i);
            return 0;
        }
    }
    return count;
}

static bool register_texture_identity(const bzTTTerrain_t *terrain, bzTTTerrainTextureKind_t kind, uint32_t typeIndex,
                                      char *identity, bzTTImageInfo_t *imageInfo, const bzTTAsset_t **outAsset) {
    const bzTTAsset_t *asset = BZ_TTA_RegisterTerrainTexture(BZ_TABLETOP_ASSETS_ABI_VERSION, terrain, kind, typeIndex);
    if (!asset) return false;
    if (!BZ_TTAsset_Identity(asset, identity, BZ_QUEST_WC3_MAX_IDENTITY) || !BZ_TTAsset_ImageInfo(asset, imageInfo)) {
        BZ_TTAsset_Release(asset);
        return false;
    }
    *outAsset = asset;
    return true;
}

static bool valid_texture_shape(const char *identity, bzTTTerrainTextureKind_t kind, const bzTTImageInfo_t *imageInfo,
                                bool requireAtlasGrid) {
    if (imageInfo->format != BZ_TTA_PIXEL_RGBA8 || imageInfo->origin != BZ_TTA_ORIGIN_TOP_LEFT) {
        LOG_ONCE(identity, "image-format",
                 "bz_quest_wc3_terrain_capture: terrain texture '%s' kind %u has unsupported pixel/origin format\n",
                 identity, (unsigned)kind);
        return false;
    }
    if (imageInfo->width > BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_DIM ||
        imageInfo->height > BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_DIM ||
        imageInfo->data_bytes > BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_BYTES) {
        LOG_ONCE(identity, "image-too-large",
                 "bz_quest_wc3_terrain_capture: terrain texture '%s' kind %u exceeds %u/%u-byte slice cap\n",
                 identity, (unsigned)kind, (unsigned)BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_DIM,
                 (unsigned)BZ_QUEST_WC3_TERRAIN_MAX_TEXTURE_BYTES);
        return false;
    }
    if (requireAtlasGrid && (imageInfo->width < 64 || imageInfo->height < 64 || imageInfo->width % 64 ||
                             imageInfo->height % 64)) {
        LOG_ONCE(identity, "ground-atlas-grid",
                 "bz_quest_wc3_terrain_capture: ground texture '%s' is not a 64-pixel tiled atlas - layer excluded\n",
                 identity);
        return false;
    }
    return true;
}

static void copy_reference_tables(const bzTTTerrain_t *terrain, const char *terrainIdentity,
                                  bzQuestWc3TerrainInput_t *out, uint32_t wetCornerCount) {
    uint32_t referenceCount = BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND);
    for (uint32_t i = 0; i < referenceCount && out->referencedGroundCount < BZ_QUEST_WC3_TERRAIN_MAX_GROUND_TYPES; i++) {
        bzTTTerrainTextureInfo_t info;
        char identity[BZ_QUEST_WC3_MAX_IDENTITY] = {0};
        bzTTImageInfo_t imageInfo;
        const bzTTAsset_t *asset = NULL;
        if (!BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND, i, &info) || !info.corner_count) {
            LOG_ONCE(terrainIdentity, "ground-reference-invalid",
                     "bz_quest_wc3_terrain_capture: terrain '%s' ground reference %u is invalid\n",
                     terrainIdentity, i);
            continue;
        }
        if (!register_texture_identity(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND, info.type_index, identity, &imageInfo,
                                       &asset)) {
            LOG_ONCE(terrainIdentity, "ground-registration-failed",
                     "bz_quest_wc3_terrain_capture: terrain '%s' ground type_index %u registration failed\n",
                     terrainIdentity, info.type_index);
            continue;
        }
        if (valid_texture_shape(identity, BZ_TTA_TERRAIN_TEXTURE_GROUND, &imageInfo, true)) {
            bzQuestWc3TerrainTextureRef_t *ref = &out->grounds[out->referencedGroundCount++];
            memset(ref, 0, sizeof(*ref));
            strncpy(ref->identity, identity, sizeof(ref->identity) - 1);
            ref->typeIndex = info.type_index;
            ref->typeId = info.type_id;
            ref->cornerCount = info.corner_count;
            ref->width = imageInfo.width;
            ref->height = imageInfo.height;
        }
        BZ_TTAsset_Release(asset);
    }

    referenceCount = BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF);
    for (uint32_t i = 0; i < referenceCount && out->referencedCliffCount < BZ_QUEST_WC3_TERRAIN_MAX_CLIFF_TYPES; i++) {
        bzTTTerrainTextureInfo_t info;
        char identity[BZ_QUEST_WC3_MAX_IDENTITY] = {0};
        bzTTImageInfo_t imageInfo;
        const bzTTAsset_t *asset = NULL;
        if (!BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF, i, &info) || !info.corner_count) {
            LOG_ONCE(terrainIdentity, "cliff-reference-invalid",
                     "bz_quest_wc3_terrain_capture: terrain '%s' cliff reference %u is invalid\n",
                     terrainIdentity, i);
            continue;
        }
        if (!register_texture_identity(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF, info.type_index, identity, &imageInfo,
                                       &asset)) {
            LOG_ONCE(terrainIdentity, "cliff-registration-failed",
                     "bz_quest_wc3_terrain_capture: terrain '%s' cliff type_index %u registration failed\n",
                     terrainIdentity, info.type_index);
            continue;
        }
        if (valid_texture_shape(identity, BZ_TTA_TERRAIN_TEXTURE_CLIFF, &imageInfo, false)) {
            bzQuestWc3TerrainTextureRef_t *ref = &out->cliffs[out->referencedCliffCount++];
            memset(ref, 0, sizeof(*ref));
            strncpy(ref->identity, identity, sizeof(ref->identity) - 1);
            ref->typeIndex = info.type_index;
            ref->typeId = info.type_id;
            ref->cornerCount = info.corner_count;
            ref->width = imageInfo.width;
            ref->height = imageInfo.height;
        }
        BZ_TTAsset_Release(asset);
    }

    if (BZ_TTTerrain_ReferencedTextureCount(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER) != 1) return;
    bzTTTerrainTextureInfo_t info;
    char identity[BZ_QUEST_WC3_MAX_IDENTITY] = {0};
    bzTTImageInfo_t imageInfo;
    const bzTTAsset_t *asset = NULL;
    if (!BZ_TTTerrain_ReferencedTexture(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER, 0, &info) || info.type_index != 0 ||
        info.type_id != 0 || info.corner_count != wetCornerCount) {
        LOG_ONCE(terrainIdentity, "water-reference-invalid",
                 "bz_quest_wc3_terrain_capture: terrain '%s' water reference is inconsistent (corner_count=%u wet=%u)\n",
                 terrainIdentity, info.corner_count, wetCornerCount);
        return;
    }
    if (!register_texture_identity(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER, 0, identity, &imageInfo, &asset)) {
        LOG_ONCE(terrainIdentity, "water-registration-failed",
                 "bz_quest_wc3_terrain_capture: terrain '%s' water registration failed\n", terrainIdentity);
        return;
    }
    if (valid_texture_shape(identity, BZ_TTA_TERRAIN_TEXTURE_WATER, &imageInfo, false)) {
        memset(&out->water, 0, sizeof(out->water));
        strncpy(out->water.identity, identity, sizeof(out->water.identity) - 1);
        out->water.typeIndex = 0;
        out->water.typeId = 0;
        out->water.cornerCount = info.corner_count;
        out->water.width = imageInfo.width;
        out->water.height = imageInfo.height;
        out->hasWater = true;
    }
    BZ_TTAsset_Release(asset);
}

static void emit_texture(const bzTTTerrain_t *terrain, bzTTTerrainTextureKind_t kind, uint32_t typeIndex,
                         const bzQuestWc3TerrainTextureRef_t *ref,
                         const bzQuestWc3TerrainCaptureCallbacks_t *callbacks) {
    if (!callbacks || !callbacks->onTextureReady || !ref->identity[0]) return;
    const bzTTAsset_t *asset = NULL;
    bzTTImageInfo_t imageInfo;
    char identity[BZ_QUEST_WC3_MAX_IDENTITY] = {0};
    if (!register_texture_identity(terrain, kind, typeIndex, identity, &imageInfo, &asset)) return;
    uint32_t copied = BZ_TTAsset_CopyImagePixels(asset, s_scratchPixels, sizeof(s_scratchPixels));
    if (copied == imageInfo.data_bytes)
        callbacks->onTextureReady(identity, imageInfo.width, imageInfo.height, imageInfo.row_bytes,
                                  s_scratchPixels, copied, callbacks->textureUserdata);
    BZ_TTAsset_Release(asset);
}

static void emit_textures(const bzTTTerrain_t *terrain, const bzQuestWc3TerrainInput_t *input,
                          const bzQuestWc3TerrainCaptureCallbacks_t *callbacks) {
    for (uint32_t i = 0; i < input->referencedGroundCount; i++)
        emit_texture(terrain, BZ_TTA_TERRAIN_TEXTURE_GROUND, input->grounds[i].typeIndex, &input->grounds[i], callbacks);
    for (uint32_t i = 0; i < input->referencedCliffCount; i++)
        emit_texture(terrain, BZ_TTA_TERRAIN_TEXTURE_CLIFF, input->cliffs[i].typeIndex, &input->cliffs[i], callbacks);
    if (input->hasWater) emit_texture(terrain, BZ_TTA_TERRAIN_TEXTURE_WATER, 0, &input->water, callbacks);
}

bool bz_quest_wc3_terrain_capture(const bzQuestWc3TerrainCaptureCallbacks_t *callbacks) {
    const bzTTTerrain_t *terrain = BZ_TTA_LatestTerrain();
    if (!terrain) return false;

    bzTTTerrainInfo_t info;
    if (!BZ_TTTerrain_Info(terrain, &info)) {
        BZ_TTTerrain_Release(terrain);
        return false;
    }
    char terrainIdentity[BZ_QUEST_WC3_MAX_IDENTITY];
    terrain_key(terrain, &info, terrainIdentity, sizeof(terrainIdentity));
    if (bz_quest_wc3_identity_equal(terrainIdentity, s_lastTerrainKey)) {
        /* Same generation: skip the expensive corner walk/onTerrainReady rebuild,
         * but still re-offer this generation's already-known referenced textures
         * every call. s_scratchTerrain's referencedGround/Cliff/water tables were
         * populated the frame this generation first appeared and are untouched
         * here, so this is just re-registering/copying+re-callbacking them - the
         * Vulkan cache dedups anything already uploaded via cache_find() before
         * spending a budget slot. Without this, any texture rejected by the first
         * frame's bounded upload budget would never be retried (see
         * bz_quest_wc3_terrain_capture.h's header comment). */
        emit_textures(terrain, &s_scratchTerrain, callbacks);
        BZ_TTTerrain_Release(terrain);
        return true;
    }
    strncpy(s_lastTerrainKey, terrainIdentity, sizeof(s_lastTerrainKey) - 1);

    memset(&s_scratchTerrain, 0, sizeof(s_scratchTerrain));
    strncpy(s_scratchTerrain.identity, terrainIdentity, sizeof(s_scratchTerrain.identity) - 1);
    s_scratchTerrain.cornerWidth = info.width;
    s_scratchTerrain.cornerHeight = info.height;
    s_scratchTerrain.tileWidth = info.tile_width;
    s_scratchTerrain.tileHeight = info.tile_height;
    s_scratchTerrain.chunkCountX = info.chunk_count_x;
    s_scratchTerrain.chunkCountZ = info.chunk_count_y;
    s_scratchTerrain.bounds.minX = info.min_x;
    s_scratchTerrain.bounds.minZ = info.min_y;
    s_scratchTerrain.bounds.maxX = info.max_x;
    s_scratchTerrain.bounds.maxZ = info.max_y;
    s_scratchTerrain.groundTypeCount = info.ground_type_count;
    s_scratchTerrain.cliffTypeCount = info.cliff_type_count;

    bzQuestWc3TerrainMetrics_t metrics;
    bool geometryValid = info.chunk_tiles == BZ_QUEST_WC3_TERRAIN_CHUNK_TILES &&
                         info.width <= BZ_QUEST_WC3_TERRAIN_MAX_CORNERS_PER_AXIS &&
                         info.height <= BZ_QUEST_WC3_TERRAIN_MAX_CORNERS_PER_AXIS &&
                         bz_quest_wc3_terrain_measure(&s_scratchTerrain.bounds, info.tile_width, info.tile_height,
                                                      &metrics) == BZ_QUEST_WC3_TERRAIN_OK;
    if (!geometryValid)
        LOG_ONCE(terrainIdentity, "terrain-metadata-invalid",
                 "bz_quest_wc3_terrain_capture: terrain '%s' metadata is inconsistent (chunkTiles=%u dims=%ux%u)\n",
                 terrainIdentity, info.chunk_tiles, info.width, info.height);
    else
        s_scratchTerrain.cellSize = metrics.cellSize;

    uint32_t groundTypes[BZ_QUEST_WC3_TERRAIN_MAX_GROUND_TYPES] = {0};
    uint32_t cliffTypes[BZ_QUEST_WC3_TERRAIN_MAX_CLIFF_TYPES] = {0};
    uint32_t groundTypeCount = copy_type_table(terrain, true, info.ground_type_count,
                                               BZ_QUEST_WC3_TERRAIN_MAX_GROUND_TYPES, groundTypes,
                                               terrainIdentity, "ground-types");
    uint32_t cliffTypeCount = copy_type_table(terrain, false, info.cliff_type_count,
                                              BZ_QUEST_WC3_TERRAIN_MAX_CLIFF_TYPES, cliffTypes,
                                              terrainIdentity, "cliff-types");
    uint32_t wetCornerCount = 0;
    if (geometryValid) {
        for (uint32_t z = 0; z < info.height; z++) for (uint32_t x = 0; x < info.width; x++) {
            bzTTTerrainCorner_t raw;
            bzQuestWc3TerrainCorner_t *dst = &s_scratchTerrain.corners[z * info.width + x];
            if (!BZ_TTTerrain_Corner(terrain, x, z, &raw)) {
                geometryValid = false;
                LOG_ONCE(terrainIdentity, "corner-copy-failed",
                         "bz_quest_wc3_terrain_capture: terrain '%s' corner %u,%u copy failed\n",
                         terrainIdentity, x, z);
                break;
            }
            dst->rawHeight = raw.height;
            dst->rawWaterHeight = raw.water_height;
            dst->height = raw.height * metrics.scale;
            dst->waterHeight = raw.water_height * metrics.scale;
            dst->groundTypeIndex = find_type_index(groundTypes, groundTypeCount, raw.ground_id);
            dst->cliffTypeIndex = (raw.flags & BZ_TTA_TERRAIN_NO_CLIFF) ? -1 :
                                  find_type_index(cliffTypes, cliffTypeCount, raw.cliff_id);
            dst->groundVariation = raw.ground_variation;
            dst->cliffVariation = raw.cliff_variation;
            dst->cliffLevel = raw.cliff_level;
            dst->flags = raw.flags;
            if (dst->groundTypeIndex < 0)
                LOG_ONCE(terrainIdentity, "ground-id-unresolved",
                         "bz_quest_wc3_terrain_capture: terrain '%s' ground id 0x%08x missing from type table\n",
                         terrainIdentity, raw.ground_id);
            if (dst->cliffTypeIndex < 0 && !(raw.flags & BZ_TTA_TERRAIN_NO_CLIFF))
                LOG_ONCE(terrainIdentity, "cliff-id-unresolved",
                         "bz_quest_wc3_terrain_capture: terrain '%s' cliff id 0x%08x missing from type table\n",
                         terrainIdentity, raw.cliff_id);
            if (raw.flags & BZ_TTA_TERRAIN_WATER) wetCornerCount++;
        }
    }
    copy_reference_tables(terrain, terrainIdentity, &s_scratchTerrain, wetCornerCount);

    if (callbacks && callbacks->onTerrainReady) callbacks->onTerrainReady(&s_scratchTerrain, callbacks->terrainUserdata);
    emit_textures(terrain, &s_scratchTerrain, callbacks);
    BZ_TTTerrain_Release(terrain);
    return true;
}
