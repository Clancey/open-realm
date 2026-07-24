#include "bz_tabletop_assets_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_tabletop_transport.h"

typedef struct bzTTMetadataCache {
    uintptr_t source_token;
    uint32_t class_id;
    bzTTAResult_t status;
    bzTTAssetMetadata_t metadata;
    struct bzTTMetadataCache *next;
} bzTTMetadataCache_t;

static pthread_mutex_t g_assets_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_assets_source_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_assets_initialized, g_assets_terminal = true;
static bzTTAssetSource_t g_assets_source;
static bzTTAsset_t *g_assets_cache;
static bzTTMetadataCache_t *g_metadata_cache;
static bzTTTerrain_t *g_latest_terrain;
static uint64_t g_cache_hits, g_cache_misses, g_placeholder_logs, g_metadata_logs;
static uint64_t g_assets_generation;
static uintptr_t g_failed_terrain_token, g_metadata_token;

static bool terrain_type_info(const bzTTTerrain_t *terrain, uint32_t index, uint32_t offset,
                              uint32_t count, bzTTTerrainTextureInfo_t *out);
static bool terrain_type_table(const bzTTTerrain_t *terrain, bzTTTerrainTextureKind_t kind,
                               uint32_t *offset, uint32_t *count);

static bool metadata_equal(const bzTTAssetMetadata_t *a, const bzTTAssetMetadata_t *b) {
    return a->category == b->category && a->class_id == b->class_id &&
           a->team_color == b->team_color &&
           !memcmp(&a->tint_r, &b->tint_r, sizeof(float)) &&
           !memcmp(&a->tint_g, &b->tint_g, sizeof(float)) &&
           !memcmp(&a->tint_b, &b->tint_b, sizeof(float)) &&
           !memcmp(&a->tint_a, &b->tint_a, sizeof(float)) &&
           !memcmp(&a->footprint_x, &b->footprint_x, sizeof(float)) &&
           !memcmp(&a->footprint_y, &b->footprint_y, sizeof(float));
}

static void asset_free(bzTTAsset_t *asset) { free(asset); }
static void terrain_free(bzTTTerrain_t *terrain) { free(terrain); }

static void asset_retain_locked(const bzTTAsset_t *asset_const) {
    ((bzTTAsset_t *)asset_const)->refcount++;
}

static void asset_release_locked(const bzTTAsset_t *asset_const) {
    bzTTAsset_t *asset = (bzTTAsset_t *)asset_const;
    if (--asset->refcount == 0)
        asset_free(asset);
}

static void terrain_retain_locked(const bzTTTerrain_t *terrain_const) {
    ((bzTTTerrain_t *)terrain_const)->refcount++;
}

static void terrain_release_locked(const bzTTTerrain_t *terrain_const) {
    bzTTTerrain_t *terrain = (bzTTTerrain_t *)terrain_const;
    if (--terrain->refcount == 0)
        terrain_free(terrain);
}

static void clear_metadata_locked(void) {
    bzTTMetadataCache_t *metadata, *next;
    for (metadata = g_metadata_cache; metadata; metadata = next) {
        next = metadata->next;
        free(metadata);
    }
    g_metadata_cache = NULL;
}

/* Drop cache publication references while retained readers keep immutable payloads alive. */
static void clear_published_locked(void) {
    for (bzTTAsset_t *asset = g_assets_cache, *next; asset; asset = next) {
        next = asset->cache_next;
        asset->cache_next = NULL;
        asset_release_locked(asset);
    }
    g_assets_cache = NULL;
    clear_metadata_locked();
    g_metadata_token = 0;
    if (g_latest_terrain) {
        terrain_release_locked(g_latest_terrain);
        g_latest_terrain = NULL;
    }
}

/* Cache owns one reference; callers receive one additional retained reference. */
static bzTTAsset_t *find_cached_locked(const char *identity, bzTTAssetKind_t kind,
                                       const bzTTAssetMetadata_t *metadata) {
    for (bzTTAsset_t *asset = g_assets_cache; asset; asset = asset->cache_next)
        if (asset->kind == kind && !strcmp(asset->cache_identity, identity) &&
            metadata_equal(&asset->metadata, metadata))
            return asset;
    return NULL;
}

static bzTTAsset_t *placeholder_asset(const char *identity, bzTTAssetKind_t kind,
                                      const bzTTAssetMetadata_t *metadata, bzTTAResult_t status) {
    bzTTAsset_t *asset;
    size_t bytes = kind == BZ_TTA_ASSET_IMAGE ? 4 : 0;
    asset = BZ_TTA_AssetAlloc(bytes, kind, identity, metadata);
    if (!asset)
        return NULL;
    asset->placeholder = true;
    asset->status = status;
    if (kind == BZ_TTA_ASSET_IMAGE) {
        uint8_t *pixel;
        asset->u.image.info = (bzTTImageInfo_t){
            .width = 1, .height = 1, .row_bytes = 4, .data_bytes = 4,
            .format = BZ_TTA_PIXEL_RGBA8, .origin = BZ_TTA_ORIGIN_TOP_LEFT,
        };
        asset->u.image.pixels_offset = 0;
        pixel = asset->data;
        pixel[0] = 255; pixel[1] = 0; pixel[2] = 255; pixel[3] = 255;
    } else {
        asset->u.model.info.version = 800;
    }
    return asset;
}

bzTTAsset_t *BZ_TTA_AssetAlloc(size_t payload_bytes, bzTTAssetKind_t kind,
                                const char *identity, const bzTTAssetMetadata_t *metadata) {
    bzTTAsset_t *asset;
    if (payload_bytes > SIZE_MAX - sizeof(*asset))
        return NULL;
    asset = calloc(1, sizeof(*asset) + payload_bytes);
    if (!asset)
        return NULL;
    asset->refcount = 1;
    asset->kind = kind;
    asset->status = BZ_TTA_OK;
    asset->allocation_size = sizeof(*asset) + payload_bytes;
    snprintf(asset->identity, sizeof(asset->identity), "%s", identity ? identity : "");
    snprintf(asset->cache_identity, sizeof(asset->cache_identity), "%s", identity ? identity : "");
    if (metadata)
        asset->metadata = *metadata;
    else {
        asset->metadata.team_color = BZ_TTA_TEAM_COLOR_NONE;
        asset->metadata.tint_r = asset->metadata.tint_g = asset->metadata.tint_b = 1.0f;
        asset->metadata.tint_a = 1.0f;
    }
    return asset;
}

bzTTTerrain_t *BZ_TTA_TerrainAlloc(size_t payload_bytes) {
    bzTTTerrain_t *terrain;
    if (payload_bytes > SIZE_MAX - sizeof(*terrain))
        return NULL;
    terrain = calloc(1, sizeof(*terrain) + payload_bytes);
    if (!terrain)
        return NULL;
    terrain->refcount = 1;
    terrain->allocation_size = sizeof(*terrain) + payload_bytes;
    return terrain;
}

void *BZ_TTA_AssetData(bzTTAsset_t *asset, uint32_t offset, size_t bytes) {
    size_t payload;
    if (!asset)
        return NULL;
    payload = asset->allocation_size - sizeof(*asset);
    if ((size_t)offset > payload || bytes > payload - offset)
        return NULL;
    return asset->data + offset;
}

void *BZ_TTA_TerrainData(bzTTTerrain_t *terrain, uint32_t offset, size_t bytes) {
    size_t payload;
    if (!terrain)
        return NULL;
    payload = terrain->allocation_size - sizeof(*terrain);
    if ((size_t)offset > payload || bytes > payload - offset)
        return NULL;
    return terrain->data + offset;
}

void BZ_TTA_Init(void) {
    bzTTAssetSource_t source = { 0 };
    BZ_WC3_TTA_Source(&source);
    pthread_mutex_lock(&g_assets_source_lock);
    pthread_mutex_lock(&g_assets_lock);
    clear_published_locked();
    g_assets_source = source;
    g_cache_hits = g_cache_misses = g_placeholder_logs = g_metadata_logs = 0;
    g_failed_terrain_token = 0;
    if (!++g_assets_generation) g_assets_generation = 1;
    g_assets_initialized = true;
    g_assets_terminal = false;
    pthread_mutex_unlock(&g_assets_lock);
    pthread_mutex_unlock(&g_assets_source_lock);
    fprintf(stderr, "BZTabletopAssets: initialized, abi_version=%u\n", BZ_TABLETOP_ASSETS_ABI_VERSION);
}

void BZ_TTA_Shutdown(void) {
    pthread_mutex_lock(&g_assets_lock);
    g_assets_terminal = true;
    clear_published_locked();
    pthread_mutex_unlock(&g_assets_lock);
    /* Mark terminal first, then drain without holding the cache lock before FS/game teardown. */
    pthread_mutex_lock(&g_assets_source_lock);
    pthread_mutex_unlock(&g_assets_source_lock);
    fprintf(stderr, "BZTabletopAssets: shutdown (terminal)\n");
}

uint32_t BZ_TTA_AbiVersion(void) { return BZ_TABLETOP_ASSETS_ABI_VERSION; }

static bool registration_context(uint32_t abi_version, bzTTAssetKind_t kind,
                                 bzTTAssetSource_t *source, uint64_t *generation) {
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || abi_version != BZ_TABLETOP_ASSETS_ABI_VERSION ||
        (kind != BZ_TTA_ASSET_IMAGE && kind != BZ_TTA_ASSET_MODEL)) {
        pthread_mutex_unlock(&g_assets_lock);
        return false;
    }

    *source = g_assets_source;
    *generation = g_assets_generation;
    pthread_mutex_unlock(&g_assets_lock);
    return true;
}

static bzTTAResult_t source_context(uint32_t abi_version, bzTTAssetSource_t *source,
                                    uint64_t *generation) {
    bzTTAResult_t status = BZ_TTA_OK;
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized) status = BZ_TTA_ERR_NOT_INITIALIZED;
    else if (g_assets_terminal) status = BZ_TTA_ERR_TERMINAL;
    else if (abi_version != BZ_TABLETOP_ASSETS_ABI_VERSION) status = BZ_TTA_ERR_ABI_VERSION;
    if (status == BZ_TTA_OK) {
        *source = g_assets_source;
        *generation = g_assets_generation;
    }
    pthread_mutex_unlock(&g_assets_lock);
    return status;
}

/* All authoritative identity sources converge here for one cache and lifecycle contract. */
static const bzTTAsset_t *register_identity(const char *identity, bzTTAssetKind_t kind,
                                            const bzTTAssetMetadata_t *metadata_arg,
                                            bzTTAResult_t status, const bzTTAssetSource_t *source,
                                            uint64_t generation) {
    bzTTAssetMetadata_t metadata = {
        .team_color = BZ_TTA_TEAM_COLOR_NONE, .tint_r = 1, .tint_g = 1, .tint_b = 1, .tint_a = 1,
    };
    bzTTAsset_t *asset;
    if (metadata_arg)
        metadata = *metadata_arg;
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || generation != g_assets_generation) {
        pthread_mutex_unlock(&g_assets_lock);
        return NULL;
    }
    asset = find_cached_locked(identity, kind, &metadata);
    if (asset) {
        g_cache_hits++;
        asset_retain_locked(asset);
        pthread_mutex_unlock(&g_assets_lock);
        return asset;
    }
    pthread_mutex_unlock(&g_assets_lock);

    /* Source archive readers may be process-global; serialize misses through immutable publication. */
    pthread_mutex_lock(&g_assets_source_lock);
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || generation != g_assets_generation) {
        pthread_mutex_unlock(&g_assets_lock);
        pthread_mutex_unlock(&g_assets_source_lock);
        return NULL;
    }
    asset = find_cached_locked(identity, kind, &metadata);
    if (asset) {
        g_cache_hits++;
        asset_retain_locked(asset);
        pthread_mutex_unlock(&g_assets_lock);
        pthread_mutex_unlock(&g_assets_source_lock);
        return asset;
    }
    g_cache_misses++;
    pthread_mutex_unlock(&g_assets_lock);
    if (status == BZ_TTA_OK) {
        if (!source->path_is_confined || !source->path_is_confined(identity))
            status = BZ_TTA_ERR_PATH_CONFINEMENT;
        else if (source->load_asset)
            asset = source->load_asset(identity, kind, &metadata, &status);
        else
            status = BZ_TTA_ERR_NOT_INITIALIZED;
    }
    if (!asset)
        asset = placeholder_asset(identity, kind, &metadata, status);
    if (!asset) {
        pthread_mutex_unlock(&g_assets_source_lock);
        return NULL;
    }
    snprintf(asset->cache_identity, sizeof(asset->cache_identity), "%s", identity);
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || generation != g_assets_generation) {
        pthread_mutex_unlock(&g_assets_lock);
        asset_release_locked(asset);
        pthread_mutex_unlock(&g_assets_source_lock);
        return NULL;
    }
    {
        bzTTAsset_t *raced = find_cached_locked(identity, kind, &metadata);
        if (raced) {
            asset_retain_locked(raced);
            pthread_mutex_unlock(&g_assets_lock);
            asset_release_locked(asset);
            pthread_mutex_unlock(&g_assets_source_lock);
            return raced;
        }

    }
    asset->cache_next = g_assets_cache;
    g_assets_cache = asset;
    asset_retain_locked(asset);
    if (asset->placeholder) g_placeholder_logs++;
    pthread_mutex_unlock(&g_assets_lock);
    pthread_mutex_unlock(&g_assets_source_lock);
    if (asset->placeholder)
        fprintf(stderr, "BZTabletopAssets: %s asset '%s' unavailable (%d); cached placeholder\n",
                kind == BZ_TTA_ASSET_MODEL ? "model" : "image", identity, status);
    return asset;
}

const bzTTAsset_t *BZ_TTA_RegisterConfigString(uint32_t abi_version,
                                               const bzTTSnapshot_t *snapshot,
                                               uint32_t cs_index,
                                               bzTTAssetKind_t kind,
                                               const bzTTAssetMetadata_t *metadata) {
    char identity[BZ_TTA_MAX_IDENTITY];
    bzTTAssetSource_t source;
    bzTTAResult_t status = BZ_TTA_OK;
    uint64_t generation;
    if (!snapshot || !registration_context(abi_version, kind, &source, &generation))
        return NULL;
    if (!BZ_TTSnapshot_ConfigString(snapshot, cs_index, identity, sizeof(identity))) {
        snprintf(identity, sizeof(identity), "<configstring:%u>", cs_index);
        status = BZ_TTA_ERR_NOT_FOUND;
    }
    return register_identity(identity, kind, metadata, status, &source, generation);
}

const bzTTAsset_t *BZ_TTA_RegisterModelTexture(uint32_t abi_version,
                                              const bzTTAsset_t *model,
                                              uint32_t texture_index) {
    bzTTModelTextureInfo_t texture;
    bzTTAssetSource_t source;
    char identity[BZ_TTA_MAX_IDENTITY];
    bzTTAResult_t status = BZ_TTA_OK;
    uint64_t generation;
    if (!BZ_TTAsset_ModelTextureInfo(model, texture_index, &texture) ||
        !registration_context(abi_version, BZ_TTA_ASSET_IMAGE, &source, &generation))
        return NULL;
    if (!texture.identity[0]) {
        snprintf(identity, sizeof(identity), "<replaceable:%u>", texture.replaceable_id);
        status = BZ_TTA_ERR_UNSUPPORTED;
    } else
        memcpy(identity, texture.identity, sizeof(identity));
    return register_identity(identity, BZ_TTA_ASSET_IMAGE, &model->metadata, status, &source, generation);
}

const bzTTAsset_t *BZ_TTA_RegisterTerrainTexture(uint32_t abi_version,
                                                 const bzTTTerrain_t *terrain,
                                                 bzTTTerrainTextureKind_t kind,
                                                 uint32_t type_index) {
    bzTTAssetSource_t source;
    bzTTAResult_t status;
    bzTTTerrainTextureInfo_t texture;
    uint32_t offset, count;
    uint64_t generation;
    char identity[BZ_TTA_MAX_IDENTITY];
    if (!terrain || (kind != BZ_TTA_TERRAIN_TEXTURE_GROUND && kind != BZ_TTA_TERRAIN_TEXTURE_CLIFF &&
                     kind != BZ_TTA_TERRAIN_TEXTURE_WATER))
        return NULL;
    if (source_context(abi_version, &source, &generation) != BZ_TTA_OK ||
        terrain->generation != generation || !source.resolve_terrain_identity)
        return NULL;
    if (kind == BZ_TTA_TERRAIN_TEXTURE_WATER) {
        if (type_index || !terrain->water_corner_count) return NULL;
        texture = (bzTTTerrainTextureInfo_t){ .corner_count = terrain->water_corner_count };
    } else if (!terrain_type_table(terrain, kind, &offset, &count) ||
               !terrain_type_info(terrain, type_index, offset, count, &texture) || !texture.corner_count)
        return NULL;
    snprintf(identity, sizeof(identity), "<terrain:%08x>", texture.type_id);
    pthread_mutex_lock(&g_assets_source_lock);
    status = source.resolve_terrain_identity(kind, texture.type_id, terrain->tileset, identity, sizeof(identity));
    pthread_mutex_unlock(&g_assets_source_lock);
    return register_identity(identity, BZ_TTA_ASSET_IMAGE, NULL, status, &source, generation);
}

static bzTTMetadataCache_t *find_metadata_locked(uintptr_t source_token, uint32_t class_id) {
    for (bzTTMetadataCache_t *entry = g_metadata_cache; entry; entry = entry->next)
        if (entry->source_token == source_token && entry->class_id == class_id)
            return entry;
    return NULL;
}

/* Cache immutable table results by class ID; runtime overrides remain caller-owned values. */
bzTTAResult_t BZ_TTA_ResolveEntityMetadata(uint32_t abi_version,
                                           const bzTTEntityMetadataInput_t *input,
                                           bzTTAssetMetadata_t *out) {
    bzTTAssetSource_t source;
    bzTTMetadataCache_t *entry, *raced;
    bzTTAssetMetadata_t metadata;
    bzTTAResult_t status;
    uint64_t generation;
    uintptr_t source_token;
    bool log_failure = false;
    if (!input || !out || input->override_mask &
        ~(BZ_TTA_METADATA_OVERRIDE_TEAM_COLOR | BZ_TTA_METADATA_OVERRIDE_TINT))
        return BZ_TTA_ERR_INVALID_ARGUMENT;
    status = source_context(abi_version, &source, &generation);
    if (status != BZ_TTA_OK) return status;
    if (!source.metadata_token || !source.resolve_entity_metadata) return BZ_TTA_ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&g_assets_source_lock);
    source_token = source.metadata_token();
    if (!source_token) {
        pthread_mutex_unlock(&g_assets_source_lock);
        return BZ_TTA_ERR_NOT_INITIALIZED;
    }
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || generation != g_assets_generation) {
        pthread_mutex_unlock(&g_assets_lock);
        pthread_mutex_unlock(&g_assets_source_lock);
        return BZ_TTA_ERR_TERMINAL;
    }
    if (g_metadata_token != source_token) {
        clear_metadata_locked();
        g_metadata_token = source_token;
    }
    entry = find_metadata_locked(source_token, input->class_id);
    if (entry) {
        g_cache_hits++;
        metadata = entry->metadata;
        status = entry->status;
        pthread_mutex_unlock(&g_assets_lock);
        pthread_mutex_unlock(&g_assets_source_lock);
        goto overrides;
    }
    g_cache_misses++;
    pthread_mutex_unlock(&g_assets_lock);
    metadata = (bzTTAssetMetadata_t){
        .class_id = input->class_id, .team_color = BZ_TTA_TEAM_COLOR_NONE,
        .tint_r = 1, .tint_g = 1, .tint_b = 1, .tint_a = 1,
    };
    status = source.resolve_entity_metadata(input->class_id, &metadata);
    if (source.metadata_token() != source_token) {
        pthread_mutex_unlock(&g_assets_source_lock);
        return BZ_TTA_ERR_NOT_INITIALIZED;
    }
    entry = malloc(sizeof(*entry));
    if (!entry) {
        pthread_mutex_unlock(&g_assets_source_lock);
        return BZ_TTA_ERR_OUT_OF_MEMORY;
    }
    *entry = (bzTTMetadataCache_t){
        .source_token = source_token, .class_id = input->class_id,
        .status = status, .metadata = metadata,
    };
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || generation != g_assets_generation) {
        pthread_mutex_unlock(&g_assets_lock);
        free(entry);
        pthread_mutex_unlock(&g_assets_source_lock);
        return BZ_TTA_ERR_TERMINAL;
    }
    raced = find_metadata_locked(source_token, input->class_id);
    if (raced) {
        metadata = raced->metadata;
        status = raced->status;
        pthread_mutex_unlock(&g_assets_lock);
        free(entry);
    } else {
        entry->next = g_metadata_cache;
        g_metadata_cache = entry;
        if (status != BZ_TTA_OK) {
            g_metadata_logs++;
            log_failure = true;
        }
        pthread_mutex_unlock(&g_assets_lock);
    }
    pthread_mutex_unlock(&g_assets_source_lock);
    if (log_failure)
        fprintf(stderr, "BZTabletopAssets: class_id 0x%08x metadata unavailable (%d); cached error\n",
                input->class_id, status);
overrides:
    if (input->override_mask & BZ_TTA_METADATA_OVERRIDE_TEAM_COLOR)
        metadata.team_color = input->team_color;
    if (input->override_mask & BZ_TTA_METADATA_OVERRIDE_TINT) {
        metadata.tint_r = input->tint_r; metadata.tint_g = input->tint_g;
        metadata.tint_b = input->tint_b; metadata.tint_a = input->tint_a;
    }
    *out = metadata;
    return status;
}

void BZ_TTAsset_Retain(const bzTTAsset_t *asset) {
    if (!asset) return;
    pthread_mutex_lock(&g_assets_lock);
    asset_retain_locked(asset);
    pthread_mutex_unlock(&g_assets_lock);
}

void BZ_TTAsset_Release(const bzTTAsset_t *asset) {
    if (!asset) return;
    pthread_mutex_lock(&g_assets_lock);
    asset_release_locked(asset);
    pthread_mutex_unlock(&g_assets_lock);
}

bool BZ_TTAsset_IsPlaceholder(const bzTTAsset_t *asset) { return asset ? asset->placeholder : true; }
bzTTAssetKind_t BZ_TTAsset_Kind(const bzTTAsset_t *asset) { return asset ? asset->kind : 0; }
bzTTAResult_t BZ_TTAsset_Status(const bzTTAsset_t *asset) {
    return asset ? asset->status : BZ_TTA_ERR_INVALID_ARGUMENT;
}

bool BZ_TTAsset_Identity(const bzTTAsset_t *asset, char *out, size_t cap) {
    if (!asset || !out || !cap) return false;
    snprintf(out, cap, "%s", asset->identity);
    return true;
}

bool BZ_TTAsset_Metadata(const bzTTAsset_t *asset, bzTTAssetMetadata_t *out) {
    if (!asset || !out) return false;
    *out = asset->metadata;
    return true;
}

bool BZ_TTAsset_ImageInfo(const bzTTAsset_t *asset, bzTTImageInfo_t *out) {
    if (!asset || asset->kind != BZ_TTA_ASSET_IMAGE || !out) return false;
    *out = asset->u.image.info;
    return true;
}

uint32_t BZ_TTAsset_CopyImagePixels(const bzTTAsset_t *asset, void *dst, uint32_t cap) {
    uint32_t bytes;
    void *src;
    if (!asset || asset->kind != BZ_TTA_ASSET_IMAGE || !dst || !cap) return 0;
    bytes = asset->u.image.info.data_bytes < cap ? asset->u.image.info.data_bytes : cap;
    src = BZ_TTA_AssetData((bzTTAsset_t *)asset, asset->u.image.pixels_offset, bytes);
    if (!src) return 0;
    memcpy(dst, src, bytes);
    return bytes;
}

bool BZ_TTAsset_ModelInfo(const bzTTAsset_t *asset, bzTTModelInfo_t *out) {
    if (!asset || asset->kind != BZ_TTA_ASSET_MODEL || !out) return false;
    *out = asset->u.model.info;
    return true;
}

static const bzTTGeosetRecord_t *geoset_record(const bzTTAsset_t *asset, uint32_t index) {
    if (!asset || asset->kind != BZ_TTA_ASSET_MODEL || index >= asset->u.model.info.geoset_count)
        return NULL;
    return BZ_TTA_AssetData((bzTTAsset_t *)asset, asset->u.model.geosets_offset +
                            index * sizeof(bzTTGeosetRecord_t), sizeof(bzTTGeosetRecord_t));
}

bool BZ_TTAsset_GeosetInfo(const bzTTAsset_t *asset, uint32_t index, bzTTGeosetInfo_t *out) {
    const bzTTGeosetRecord_t *record = geoset_record(asset, index);
    if (!record || !out) return false;
    *out = record->info;
    return true;
}

#define COPY_GEO_ARRAY(NAME, TYPE, FIELD, COUNT) \
uint32_t NAME(const bzTTAsset_t *asset, uint32_t index, TYPE *dst, uint32_t cap) { \
    const bzTTGeosetRecord_t *record = geoset_record(asset, index); \
    uint32_t n; const void *src; \
    if (!record || !dst || !cap) return 0; \
    n = record->info.COUNT < cap ? record->info.COUNT : cap; \
    src = BZ_TTA_AssetData((bzTTAsset_t *)asset, record->FIELD, (size_t)n * sizeof(TYPE)); \
    if (!src) return 0; \
    memcpy(dst, src, (size_t)n * sizeof(TYPE)); \
    return n; \
}
COPY_GEO_ARRAY(BZ_TTAsset_CopyGeosetVertices, bzTTVec3_t, vertices_offset, vertex_count)
COPY_GEO_ARRAY(BZ_TTAsset_CopyGeosetNormals, bzTTVec3_t, normals_offset, normal_count)
COPY_GEO_ARRAY(BZ_TTAsset_CopyGeosetUVs, bzTTVec2_t, uvs_offset, uv_count)
COPY_GEO_ARRAY(BZ_TTAsset_CopyGeosetIndices, uint16_t, indices_offset, index_count)
#undef COPY_GEO_ARRAY

#define MODEL_INFO_AT(NAME, TYPE, COUNT, OFFSET) \
bool NAME(const bzTTAsset_t *asset, uint32_t index, TYPE *out) { \
    const TYPE *src; \
    if (!asset || asset->kind != BZ_TTA_ASSET_MODEL || !out || index >= asset->u.model.info.COUNT) return false; \
    src = BZ_TTA_AssetData((bzTTAsset_t *)asset, asset->u.model.OFFSET + index * sizeof(TYPE), sizeof(TYPE)); \
    if (!src) return false; *out = *src; return true; \
}
MODEL_INFO_AT(BZ_TTAsset_MaterialInfo, bzTTMaterialInfo_t, material_count, materials_offset)
MODEL_INFO_AT(BZ_TTAsset_MaterialLayerInfo, bzTTMaterialLayerInfo_t, layer_count, layers_offset)
MODEL_INFO_AT(BZ_TTAsset_ModelTextureInfo, bzTTModelTextureInfo_t, texture_count, textures_offset)
MODEL_INFO_AT(BZ_TTAsset_SequenceInfo, bzTTSequenceInfo_t, sequence_count, sequences_offset)
MODEL_INFO_AT(BZ_TTAsset_NodeInfo, bzTTNodeInfo_t, node_count, nodes_offset)
#undef MODEL_INFO_AT

void BZ_TTA_PublishTerrainFromGame(void) {
    bzTTTerrain_t *terrain = NULL, *old;
    bzTTAssetSource_t source;
    uintptr_t token = 0;
    bzTTAResult_t status = BZ_TTA_OK;
    uint64_t generation;
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || !g_assets_source.terrain_token ||
        !g_assets_source.copy_terrain) {
        pthread_mutex_unlock(&g_assets_lock);
        return;
    }
    source = g_assets_source;
    generation = g_assets_generation;
    pthread_mutex_unlock(&g_assets_lock);
    pthread_mutex_lock(&g_assets_source_lock);
    token = source.terrain_token();
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || generation != g_assets_generation) {
        pthread_mutex_unlock(&g_assets_lock);
        pthread_mutex_unlock(&g_assets_source_lock);
        return;
    }
    if (!token || (g_latest_terrain && g_latest_terrain->source_token == token)) {
        pthread_mutex_unlock(&g_assets_lock);
        pthread_mutex_unlock(&g_assets_source_lock);
        return;
    }
    pthread_mutex_unlock(&g_assets_lock);
    terrain = source.copy_terrain(&token, &status);
    if (!terrain) {
        bool log_failure = false;
        pthread_mutex_lock(&g_assets_lock);
        if (g_assets_initialized && !g_assets_terminal && generation == g_assets_generation &&
            token != g_failed_terrain_token) {
            g_failed_terrain_token = token;
            log_failure = true;
        }
        pthread_mutex_unlock(&g_assets_lock);
        if (log_failure)
            fprintf(stderr, "BZTabletopAssets: terrain token 0x%llx unavailable (%d)\n",
                    (unsigned long long)token, status);
        pthread_mutex_unlock(&g_assets_source_lock);
        return;
    }
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || generation != g_assets_generation) {
        pthread_mutex_unlock(&g_assets_lock);
        terrain_release_locked(terrain);
        pthread_mutex_unlock(&g_assets_source_lock);
        return;
    }
    if (g_latest_terrain && g_latest_terrain->source_token == token) {
        pthread_mutex_unlock(&g_assets_lock);
        terrain_release_locked(terrain);
        pthread_mutex_unlock(&g_assets_source_lock);
        return;
    }
    terrain->source_token = token;
    terrain->generation = generation;
    old = g_latest_terrain;
    g_latest_terrain = terrain;
    if (old) terrain_release_locked(old);
    pthread_mutex_unlock(&g_assets_lock);
    pthread_mutex_unlock(&g_assets_source_lock);
}

const bzTTTerrain_t *BZ_TTA_LatestTerrain(void) {
    bzTTTerrain_t *terrain;
    pthread_mutex_lock(&g_assets_lock);
    if (!g_assets_initialized || g_assets_terminal || !g_latest_terrain) {
        pthread_mutex_unlock(&g_assets_lock);
        return NULL;
    }
    terrain = g_latest_terrain;
    terrain_retain_locked(terrain);
    pthread_mutex_unlock(&g_assets_lock);
    return terrain;
}

void BZ_TTTerrain_Retain(const bzTTTerrain_t *terrain) {
    if (!terrain) return;
    pthread_mutex_lock(&g_assets_lock);
    terrain_retain_locked(terrain);
    pthread_mutex_unlock(&g_assets_lock);
}

void BZ_TTTerrain_Release(const bzTTTerrain_t *terrain) {
    if (!terrain) return;
    pthread_mutex_lock(&g_assets_lock);
    terrain_release_locked(terrain);
    pthread_mutex_unlock(&g_assets_lock);
}

bool BZ_TTTerrain_Info(const bzTTTerrain_t *terrain, bzTTTerrainInfo_t *out) {
    if (!terrain || !out) return false;
    *out = terrain->info;
    return true;
}

bool BZ_TTTerrain_Corner(const bzTTTerrain_t *terrain, uint32_t x, uint32_t y,
                         bzTTTerrainCorner_t *out) {
    const bzTTTerrainCorner_t *corner;
    uint32_t index;
    if (!terrain || !out || x >= terrain->info.width || y >= terrain->info.height) return false;
    index = y * terrain->info.width + x;
    corner = BZ_TTA_TerrainData((bzTTTerrain_t *)terrain, terrain->corners_offset +
                                index * sizeof(*corner), sizeof(*corner));
    if (!corner) return false;
    *out = *corner;
    return true;
}

static bool terrain_type_info(const bzTTTerrain_t *terrain, uint32_t index, uint32_t offset,
                              uint32_t count, bzTTTerrainTextureInfo_t *out) {
    const bzTTTerrainTypeRecord_t *record;
    if (!terrain || !out || index >= count) return false;
    record = BZ_TTA_TerrainData((bzTTTerrain_t *)terrain,
                                offset + index * sizeof(*record), sizeof(*record));
    if (!record) return false;
    *out = (bzTTTerrainTextureInfo_t){
        .type_index = index, .type_id = record->id, .corner_count = record->corner_count,
    };
    return true;
}

static bool terrain_type_table(const bzTTTerrain_t *terrain, bzTTTerrainTextureKind_t kind,
                               uint32_t *offset, uint32_t *count) {
    if (!terrain || !offset || !count) return false;
    if (kind == BZ_TTA_TERRAIN_TEXTURE_GROUND) {
        *offset = terrain->grounds_offset; *count = terrain->info.ground_type_count;
        return true;
    }
    if (kind == BZ_TTA_TERRAIN_TEXTURE_CLIFF) {
        *offset = terrain->cliffs_offset; *count = terrain->info.cliff_type_count;
        return true;
    }
    return false;
}

bool BZ_TTTerrain_GroundType(const bzTTTerrain_t *terrain, uint32_t index, uint32_t *out) {
    bzTTTerrainTextureInfo_t info;
    if (!out || !terrain_type_info(terrain, index, terrain ? terrain->grounds_offset : 0,
                                   terrain ? terrain->info.ground_type_count : 0, &info))
        return false;
    *out = info.type_id;
    return true;
}

bool BZ_TTTerrain_CliffType(const bzTTTerrain_t *terrain, uint32_t index, uint32_t *out) {
    bzTTTerrainTextureInfo_t info;
    if (!out || !terrain_type_info(terrain, index, terrain ? terrain->cliffs_offset : 0,
                                   terrain ? terrain->info.cliff_type_count : 0, &info))
        return false;
    *out = info.type_id;
    return true;
}

uint32_t BZ_TTTerrain_ReferencedTextureCount(const bzTTTerrain_t *terrain,
                                             bzTTTerrainTextureKind_t kind) {
    bzTTTerrainTextureInfo_t info;
    uint32_t offset, count, references = 0;
    if (kind == BZ_TTA_TERRAIN_TEXTURE_WATER) return terrain && terrain->water_corner_count ? 1 : 0;
    if (!terrain_type_table(terrain, kind, &offset, &count)) return 0;
    for (uint32_t i = 0; i < count; i++)
        if (terrain_type_info(terrain, i, offset, count, &info) && info.corner_count) references++;
    return references;
}

bool BZ_TTTerrain_ReferencedTexture(const bzTTTerrain_t *terrain,
                                    bzTTTerrainTextureKind_t kind,
                                    uint32_t reference_index,
                                    bzTTTerrainTextureInfo_t *out) {
    bzTTTerrainTextureInfo_t info;
    uint32_t offset, count;
    if (!out) return false;
    if (kind == BZ_TTA_TERRAIN_TEXTURE_WATER) {
        if (!terrain || reference_index || !terrain->water_corner_count) return false;
        *out = (bzTTTerrainTextureInfo_t){ .corner_count = terrain->water_corner_count };
        return true;
    }
    if (!terrain_type_table(terrain, kind, &offset, &count)) return false;
    for (uint32_t i = 0, reference = 0; i < count; i++) {
        if (!terrain_type_info(terrain, i, offset, count, &info) || !info.corner_count) continue;
        if (reference++ == reference_index) {
            *out = info;
            return true;
        }
    }
    return false;
}

uint64_t BZ_TTA_CacheHits(void) {
    uint64_t value;
    pthread_mutex_lock(&g_assets_lock); value = g_cache_hits; pthread_mutex_unlock(&g_assets_lock);
    return value;
}

uint64_t BZ_TTA_CacheMisses(void) {
    uint64_t value;
    pthread_mutex_lock(&g_assets_lock); value = g_cache_misses; pthread_mutex_unlock(&g_assets_lock);
    return value;
}

uint64_t BZ_TTA_PlaceholderLogs(void) {
    uint64_t value;
    pthread_mutex_lock(&g_assets_lock); value = g_placeholder_logs; pthread_mutex_unlock(&g_assets_lock);
    return value;
}

uint64_t BZ_TTA_MetadataLogs(void) {
    uint64_t value;
    pthread_mutex_lock(&g_assets_lock); value = g_metadata_logs; pthread_mutex_unlock(&g_assets_lock);
    return value;
}
