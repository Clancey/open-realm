/*
 * sc2_tabletop_assets.c - Layer 2A ABI implementation: lifecycle, cache, and accessors.
 *
 * This file is the SC2-specific counterpart to platform/bridge/bz_tabletop_assets.c but is
 * entirely self-contained (SC2 has exactly one asset kind - images - and no team/entity
 * metadata), so there is no separate generic-engine/per-game-source file split: the pluggable
 * bzSC2ASource_t table (installed by BZ_SC2_TTA_Source, implemented in sc2_tabletop_game.c or a
 * test fixture) supplies only raw file I/O and terrain snapshotting; DDS decoding, path
 * confinement helpers, caching, and all public accessors live here.
 */
#include "sc2_tabletop_assets_internal.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/sc2_dds.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_source_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized, g_terminal = true;
static bzSC2ASource_t g_source;
static bzSC2Image_t *g_image_cache;
static bzSC2Terrain_t *g_latest_terrain;
static bzSC2AResult_t g_last_terrain_status = BZ_SC2A_ERR_NOT_FOUND;
static uint64_t g_cache_hits, g_cache_misses, g_placeholder_logs;
static uint64_t g_generation;
static uintptr_t g_failed_terrain_token;

static void terrain_free(bzSC2Terrain_t *terrain) { free(terrain); }
static void image_free(bzSC2Image_t *image) { free(image); }

static void terrain_retain_locked(const bzSC2Terrain_t *terrain_const) {
    ((bzSC2Terrain_t *)terrain_const)->refcount++;
}

static void terrain_release_locked(const bzSC2Terrain_t *terrain_const) {
    bzSC2Terrain_t *terrain = (bzSC2Terrain_t *)terrain_const;
    if (--terrain->refcount == 0)
        terrain_free(terrain);
}

static void image_retain_locked(const bzSC2Image_t *image_const) {
    ((bzSC2Image_t *)image_const)->refcount++;
}

static void image_release_locked(const bzSC2Image_t *image_const) {
    bzSC2Image_t *image = (bzSC2Image_t *)image_const;
    if (--image->refcount == 0)
        image_free(image);
}

/* Drop cache publication references while retained readers keep immutable payloads alive. */
static void clear_published_locked(void) {
    for (bzSC2Image_t *image = g_image_cache, *next; image; image = next) {
        next = image->cache_next;
        image->cache_next = NULL;
        image_release_locked(image);
    }
    g_image_cache = NULL;
    if (g_latest_terrain) {
        terrain_release_locked(g_latest_terrain);
        g_latest_terrain = NULL;
    }
}

static bzSC2Image_t *find_cached_image_locked(const char *identity) {
    for (bzSC2Image_t *image = g_image_cache; image; image = image->cache_next)
        if (!strcmp(image->cache_identity, identity))
            return image;
    return NULL;
}

static bzSC2AResult_t registration_generation_status_locked(uint64_t generation) {
    if (!g_initialized) return BZ_SC2A_ERR_NOT_INITIALIZED;
    if (g_terminal || generation != g_generation) return BZ_SC2A_ERR_TERMINAL;
    return BZ_SC2A_OK;
}

static bzSC2Terrain_t *placeholder_terrain(bzSC2AResult_t status) {
    bzSC2Terrain_t *terrain = BZ_SC2A_TerrainAlloc(0);
    if (!terrain)
        return NULL;
    terrain->placeholder = true;
    terrain->status = status;
    return terrain;
}

/* One magenta texel, matching the WC3 placeholder image convention so a missing/rejected SC2
 * terrain texture is visually obvious rather than silently blank or black. */
static bzSC2Image_t *placeholder_image(bzSC2AResult_t status) {
    bzSC2Image_t *image;
    bzSC2AImageMipInfo_t *mip;
    uint8_t *pixel;
    image = BZ_SC2A_ImageAlloc(sizeof(bzSC2AImageMipInfo_t) + 4);
    if (!image)
        return NULL;
    image->placeholder = true;
    image->status = status;
    image->mips_offset = 0;
    image->pixels_offset = (uint32_t)sizeof(bzSC2AImageMipInfo_t);
    mip = BZ_SC2A_ImageData(image, image->mips_offset, sizeof(*mip));
    pixel = BZ_SC2A_ImageData(image, image->pixels_offset, 4);
    *mip = (bzSC2AImageMipInfo_t){ .width = 1, .height = 1, .offset = 0, .size = 4, .row_bytes = 4 };
    pixel[0] = 255; pixel[1] = 0; pixel[2] = 255; pixel[3] = 255;
    image->info = (bzSC2AImageInfo_t){
        .format = BZ_SC2A_PIXEL_RGBA8, .width = 1, .height = 1, .mip_count = 1, .data_bytes = 4,
        .origin = BZ_SC2A_ORIGIN_TOP_LEFT,
    };
    return image;
}

bzSC2Terrain_t *BZ_SC2A_TerrainAlloc(size_t payload_bytes) {
    bzSC2Terrain_t *terrain;
    if (payload_bytes > SIZE_MAX - sizeof(*terrain))
        return NULL;
    terrain = calloc(1, sizeof(*terrain) + payload_bytes);
    if (!terrain)
        return NULL;
    terrain->refcount = 1;
    terrain->allocation_size = sizeof(*terrain) + payload_bytes;
    return terrain;
}

bzSC2Image_t *BZ_SC2A_ImageAlloc(size_t payload_bytes) {
    bzSC2Image_t *image;
    if (payload_bytes > SIZE_MAX - sizeof(*image))
        return NULL;
    image = calloc(1, sizeof(*image) + payload_bytes);
    if (!image)
        return NULL;
    image->refcount = 1;
    image->status = BZ_SC2A_OK;
    image->allocation_size = sizeof(*image) + payload_bytes;
    return image;
}

void *BZ_SC2A_TerrainData(bzSC2Terrain_t *terrain, uint32_t offset, size_t bytes) {
    size_t payload;
    if (!terrain)
        return NULL;
    payload = terrain->allocation_size - sizeof(*terrain);
    if ((size_t)offset > payload || bytes > payload - offset)
        return NULL;
    return terrain->data + offset;
}

void *BZ_SC2A_ImageData(bzSC2Image_t *image, uint32_t offset, size_t bytes) {
    size_t payload;
    if (!image)
        return NULL;
    payload = image->allocation_size - sizeof(*image);
    if ((size_t)offset > payload || bytes > payload - offset)
        return NULL;
    return image->data + offset;
}

void BZ_SC2A_Init(void) {
    bzSC2ASource_t source = { 0 };
    BZ_SC2_TTA_Source(&source);
    pthread_mutex_lock(&g_source_lock);
    pthread_mutex_lock(&g_lock);
    clear_published_locked();
    g_source = source;
    g_cache_hits = g_cache_misses = g_placeholder_logs = 0;
    g_failed_terrain_token = 0;
    g_last_terrain_status = BZ_SC2A_ERR_NOT_FOUND;
    if (!++g_generation) g_generation = 1;
    g_initialized = true;
    g_terminal = false;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_source_lock);
    fprintf(stderr, "SC2TabletopAssets: initialized, abi_version=%u\n", BZ_SC2A_ABI_VERSION);
}

void BZ_SC2A_Shutdown(void) {
    pthread_mutex_lock(&g_lock);
    g_terminal = true;
    clear_published_locked();
    pthread_mutex_unlock(&g_lock);
    /* Mark terminal first, then drain without holding the cache lock before FS/game teardown. */
    pthread_mutex_lock(&g_source_lock);
    pthread_mutex_unlock(&g_source_lock);
    fprintf(stderr, "SC2TabletopAssets: shutdown (terminal)\n");
}

uint32_t BZ_SC2A_AbiVersion(void) { return BZ_SC2A_ABI_VERSION; }

void BZ_SC2A_PublishTerrainFromGame(void) {
    bzSC2Terrain_t *terrain, *old;
    bzSC2ASource_t source;
    uintptr_t token = 0;
    bzSC2AResult_t status = BZ_SC2A_OK;
    uint64_t generation;
    pthread_mutex_lock(&g_lock);
    if (!g_initialized || g_terminal || !g_source.terrain_token || !g_source.copy_terrain) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    source = g_source;
    generation = g_generation;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_lock(&g_source_lock);
    token = source.terrain_token();
    pthread_mutex_lock(&g_lock);
    if (!g_initialized || g_terminal || generation != g_generation) {
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_unlock(&g_source_lock);
        return;
    }
    if (!token || token == g_failed_terrain_token ||
        (g_latest_terrain && g_latest_terrain->source_token == token)) {
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_unlock(&g_source_lock);
        return;
    }
    pthread_mutex_unlock(&g_lock);
    terrain = source.copy_terrain(&token, &status);
    if (!terrain) {
        bool log_failure = false;
        pthread_mutex_lock(&g_lock);
        if (g_initialized && !g_terminal && generation == g_generation && token != g_failed_terrain_token) {
            g_failed_terrain_token = token;
            g_last_terrain_status = status;
            log_failure = true;
        }
        pthread_mutex_unlock(&g_lock);
        if (log_failure)
            fprintf(stderr, "SC2TabletopAssets: terrain token 0x%llx unavailable (%d)\n",
                    (unsigned long long)token, status);
        pthread_mutex_unlock(&g_source_lock);
        return;
    }
    pthread_mutex_lock(&g_lock);
    if (!g_initialized || g_terminal || generation != g_generation) {
        pthread_mutex_unlock(&g_lock);
        terrain_release_locked(terrain);
        pthread_mutex_unlock(&g_source_lock);
        return;
    }
    if (g_latest_terrain && g_latest_terrain->source_token == token) {
        pthread_mutex_unlock(&g_lock);
        terrain_release_locked(terrain);
        pthread_mutex_unlock(&g_source_lock);
        return;
    }
    terrain->source_token = token;
    terrain->session_generation = generation;
    terrain->status = BZ_SC2A_OK;
    old = g_latest_terrain;
    g_latest_terrain = terrain;
    g_failed_terrain_token = 0;
    g_last_terrain_status = BZ_SC2A_OK;
    if (old) terrain_release_locked(old);
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_source_lock);
}

/* Always returns a retained handle: the latest published terrain, or a placeholder carrying
 * whatever status explains why nothing is available (never-published, shutdown, or bad ABI). */
const bzSC2Terrain_t *BZ_SC2A_LatestTerrain(uint32_t abi_version) {
    bzSC2Terrain_t *terrain;
    bzSC2AResult_t status;
    if (abi_version != BZ_SC2A_ABI_VERSION)
        return placeholder_terrain(BZ_SC2A_ERR_ABI_VERSION);
    pthread_mutex_lock(&g_lock);
    status = !g_initialized ? BZ_SC2A_ERR_NOT_INITIALIZED : g_terminal ? BZ_SC2A_ERR_TERMINAL : BZ_SC2A_OK;
    if (status == BZ_SC2A_OK && g_latest_terrain) {
        terrain = g_latest_terrain;
        terrain_retain_locked(terrain);
        pthread_mutex_unlock(&g_lock);
        return terrain;
    }
    if (status == BZ_SC2A_OK) status = g_last_terrain_status;
    pthread_mutex_unlock(&g_lock);
    return placeholder_terrain(status);
}

void BZ_SC2ATerrain_Retain(const bzSC2Terrain_t *terrain) {
    if (!terrain) return;
    pthread_mutex_lock(&g_lock);
    terrain_retain_locked(terrain);
    pthread_mutex_unlock(&g_lock);
}

void BZ_SC2ATerrain_Release(const bzSC2Terrain_t *terrain) {
    if (!terrain) return;
    pthread_mutex_lock(&g_lock);
    terrain_release_locked(terrain);
    pthread_mutex_unlock(&g_lock);
}

bool BZ_SC2ATerrain_IsPlaceholder(const bzSC2Terrain_t *terrain) { return terrain ? terrain->placeholder : true; }
bzSC2AResult_t BZ_SC2ATerrain_Status(const bzSC2Terrain_t *terrain) {
    return terrain ? terrain->status : BZ_SC2A_ERR_INVALID_ARGUMENT;
}

bool BZ_SC2ATerrain_Info(const bzSC2Terrain_t *terrain, bzSC2ATerrainInfo_t *out) {
    if (!terrain || !out) return false;
    *out = terrain->info;
    return true;
}

/* Indexed catalog accessors (terrain textures, cliff sets, cliff cells) share one shape:
 * bounds-check against the matching info count, then bounds-checked read via TerrainData(). */
#define SC2A_TERRAIN_INDEXED(NAME, TYPE, COUNT_FIELD, OFFSET_FIELD)                                \
    bool NAME(const bzSC2Terrain_t *terrain, uint32_t index, TYPE *out) {                          \
        const TYPE *src;                                                                           \
        if (!terrain || !out || index >= terrain->info.COUNT_FIELD) return false;                  \
        src = BZ_SC2A_TerrainData((bzSC2Terrain_t *)terrain,                                        \
                                  terrain->OFFSET_FIELD + (uint32_t)index * sizeof(TYPE), sizeof(TYPE)); \
        if (!src) return false;                                                                    \
        *out = *src;                                                                               \
        return true;                                                                               \
    }
SC2A_TERRAIN_INDEXED(BZ_SC2ATerrain_TextureInfo, bzSC2ATerrainTextureInfo_t, texture_count, textures_offset)
SC2A_TERRAIN_INDEXED(BZ_SC2ATerrain_CliffSetInfo, bzSC2ACliffSetInfo_t, cliff_set_count, cliff_sets_offset)
SC2A_TERRAIN_INDEXED(BZ_SC2ATerrain_CliffCellInfo, bzSC2ACliffCellInfo_t, cliff_cell_count, cliff_cells_offset)
#undef SC2A_TERRAIN_INDEXED

bool BZ_SC2ATerrain_HeightSample(const bzSC2Terrain_t *terrain, uint32_t x, uint32_t y, bzSC2AHeightSample_t *out) {
    const bzSC2AHeightSample_t *src;
    uint32_t index;
    if (!terrain || !out || !terrain->info.hmap_width || !terrain->info.hmap_height ||
        x >= terrain->info.hmap_width || y >= terrain->info.hmap_height)
        return false;
    index = y * terrain->info.hmap_width + x;
    src = BZ_SC2A_TerrainData((bzSC2Terrain_t *)terrain,
                              terrain->height_samples_offset + index * (uint32_t)sizeof(*src), sizeof(*src));
    if (!src) return false;
    *out = *src;
    return true;
}

bool BZ_SC2ATerrain_CellInfo(const bzSC2Terrain_t *terrain, uint32_t x, uint32_t y, bzSC2ACellInfo_t *out) {
    const bzSC2ACellInfo_t *src;
    uint32_t index;
    if (!terrain || !out || x >= terrain->info.cell_width || y >= terrain->info.cell_height)
        return false;
    index = y * terrain->info.cell_width + x;
    src = BZ_SC2A_TerrainData((bzSC2Terrain_t *)terrain,
                              terrain->cells_offset + index * (uint32_t)sizeof(*src), sizeof(*src));
    if (!src) return false;
    *out = *src;
    return true;
}

uint32_t BZ_SC2ATerrain_CopyTextureMaskLayer(const bzSC2Terrain_t *terrain, uint32_t layer,
                                             uint8_t *dst, uint32_t cap) {
    uint32_t layer_bytes, offset;
    const void *src;
    if (!terrain || !dst || !cap || layer >= terrain->info.mask_layer_count ||
        !terrain->info.mask_width || !terrain->info.mask_height)
        return 0;
    layer_bytes = terrain->info.mask_width * terrain->info.mask_height;
    offset = terrain->mask_offset + layer * layer_bytes;
    layer_bytes = layer_bytes < cap ? layer_bytes : cap;
    src = BZ_SC2A_TerrainData((bzSC2Terrain_t *)terrain, offset, layer_bytes);
    if (!src) return 0;
    memcpy(dst, src, layer_bytes);
    return layer_bytes;
}

static bzSC2AResult_t registration_context(uint32_t abi_version, bzSC2ASource_t *source, uint64_t *generation) {
    bzSC2AResult_t status;
    pthread_mutex_lock(&g_lock);
    status = abi_version != BZ_SC2A_ABI_VERSION ? BZ_SC2A_ERR_ABI_VERSION :
             !g_initialized ? BZ_SC2A_ERR_NOT_INITIALIZED :
             g_terminal ? BZ_SC2A_ERR_TERMINAL : BZ_SC2A_OK;
    if (status == BZ_SC2A_OK) {
        *source = g_source;
        *generation = g_generation;
    }
    pthread_mutex_unlock(&g_lock);
    return status;
}

static bzSC2AResult_t map_dds_result(sc2DdsResult_t result) {
    switch (result) {
    case SC2_DDS_OK: return BZ_SC2A_OK;
    case SC2_DDS_ERR_INVALID_ARGUMENT: return BZ_SC2A_ERR_INVALID_ARGUMENT;
    case SC2_DDS_ERR_MALFORMED: return BZ_SC2A_ERR_MALFORMED;
    case SC2_DDS_ERR_UNSUPPORTED: return BZ_SC2A_ERR_UNSUPPORTED;
    case SC2_DDS_ERR_TOO_LARGE: return BZ_SC2A_ERR_TOO_LARGE;
    default: return BZ_SC2A_ERR_MALFORMED;
    }
}

static bool map_pixel_format(const sc2DdsImage_t *dds, uint32_t *out) {
    switch (dds->format) {
    case SC2_DDS_FORMAT_DXT1: *out = BZ_SC2A_PIXEL_DXT1; return true;
    case SC2_DDS_FORMAT_DXT3: *out = BZ_SC2A_PIXEL_DXT3; return true;
    case SC2_DDS_FORMAT_DXT5: *out = BZ_SC2A_PIXEL_DXT5; return true;
    case SC2_DDS_FORMAT_RGB:
        if (dds->channels == SC2_DDS_CHANNELS_RGB) { *out = BZ_SC2A_PIXEL_RGB8; return true; }
        if (dds->channels == SC2_DDS_CHANNELS_BGR) { *out = BZ_SC2A_PIXEL_BGR8; return true; }
        return false;
    case SC2_DDS_FORMAT_RGBA:
        if (dds->channels == SC2_DDS_CHANNELS_RGBA) { *out = BZ_SC2A_PIXEL_RGBA8; return true; }
        if (dds->channels == SC2_DDS_CHANNELS_BGRA) { *out = BZ_SC2A_PIXEL_BGRA8; return true; }
        return false;
    default: return false;
    }
}

static bzSC2Image_t *load_image(const char *identity, const bzSC2ASource_t *source, bzSC2AResult_t *status) {
    uint32_t size = 0, format, pixel_bytes, mips_bytes;
    void *raw;
    sc2DdsImage_t dds;
    bzSC2Image_t *image;
    bzSC2AImageMipInfo_t *mips;
    uint8_t *pixels;
    if (!source->read_file) { *status = BZ_SC2A_ERR_NOT_INITIALIZED; return NULL; }
    raw = source->read_file(identity, &size);
    if (!raw || !size) {
        if (raw && source->free_file) source->free_file(raw);
        *status = BZ_SC2A_ERR_NOT_FOUND;
        return NULL;
    }
    sc2DdsResult_t dds_result = SC2_DdsParseResult((BYTE const *)raw, size, &dds);
    if (dds_result != SC2_DDS_OK) {
        *status = map_dds_result(dds_result);
        if (source->free_file) source->free_file(raw);
        return NULL;
    }
    if (!map_pixel_format(&dds, &format)) {
        if (source->free_file) source->free_file(raw);
        *status = BZ_SC2A_ERR_UNSUPPORTED;
        return NULL;
    }
    pixel_bytes = dds.mipLevels[dds.mipLevelCount - 1].offset + dds.mipLevels[dds.mipLevelCount - 1].size;
    mips_bytes = dds.mipLevelCount * (uint32_t)sizeof(bzSC2AImageMipInfo_t);
    image = BZ_SC2A_ImageAlloc((size_t)mips_bytes + pixel_bytes);
    if (!image) {
        if (source->free_file) source->free_file(raw);
        *status = BZ_SC2A_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    image->mips_offset = 0;
    image->pixels_offset = mips_bytes;
    mips = BZ_SC2A_ImageData(image, image->mips_offset, mips_bytes);
    pixels = BZ_SC2A_ImageData(image, image->pixels_offset, pixel_bytes);
    for (uint32_t i = 0; i < dds.mipLevelCount; i++)
        mips[i] = (bzSC2AImageMipInfo_t){
            .width = dds.mipLevels[i].width, .height = dds.mipLevels[i].height,
            .offset = dds.mipLevels[i].offset, .size = dds.mipLevels[i].size,
            .row_bytes = dds.mipLevels[i].rowBytes,
        };
    memcpy(pixels, (uint8_t const *)raw + dds.dataOffset, pixel_bytes);
    if (source->free_file) source->free_file(raw);
    image->info = (bzSC2AImageInfo_t){
        .format = format, .width = dds.width, .height = dds.height,
        .mip_count = dds.mipLevelCount, .data_bytes = pixel_bytes, .origin = BZ_SC2A_ORIGIN_TOP_LEFT,
    };
    *status = BZ_SC2A_OK;
    return image;
}

/* All authoritative identity sources (currently only terrain textures) converge here for one
 * cache, log-once, and lifecycle contract; images persist across terrain reload within a session. */
static const bzSC2Image_t *register_identity(const char *identity, bzSC2AResult_t status,
                                             const bzSC2ASource_t *source, uint64_t generation) {
    bzSC2Image_t *image;
    bzSC2AResult_t lifecycle_status;
    pthread_mutex_lock(&g_lock);
    lifecycle_status = registration_generation_status_locked(generation);
    if (lifecycle_status != BZ_SC2A_OK) {
        pthread_mutex_unlock(&g_lock);
        /* A racing shutdown invalidates the read, but callers still receive the typed placeholder. */
        return placeholder_image(lifecycle_status);
    }
    image = find_cached_image_locked(identity);
    if (image) {
        g_cache_hits++;
        image_retain_locked(image);
        pthread_mutex_unlock(&g_lock);
        return image;
    }
    pthread_mutex_unlock(&g_lock);

    /* Source archive readers may be process-global; serialize misses through immutable publication. */
    pthread_mutex_lock(&g_source_lock);
    pthread_mutex_lock(&g_lock);
    lifecycle_status = registration_generation_status_locked(generation);
    if (lifecycle_status != BZ_SC2A_OK) {
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_unlock(&g_source_lock);
        return placeholder_image(lifecycle_status);
    }
    image = find_cached_image_locked(identity);
    if (image) {
        g_cache_hits++;
        image_retain_locked(image);
        pthread_mutex_unlock(&g_lock);
        pthread_mutex_unlock(&g_source_lock);
        return image;
    }
    g_cache_misses++;
    pthread_mutex_unlock(&g_lock);
    if (status == BZ_SC2A_OK) {
        if (!source->path_is_confined || !source->path_is_confined(identity))
            status = BZ_SC2A_ERR_PATH_CONFINEMENT;
        else
            image = load_image(identity, source, &status);
    }
    if (!image)
        image = placeholder_image(status);
    if (!image) {
        pthread_mutex_unlock(&g_source_lock);
        return NULL;
    }
    snprintf(image->identity, sizeof(image->identity), "%s", identity);
    snprintf(image->cache_identity, sizeof(image->cache_identity), "%s", identity);
    pthread_mutex_lock(&g_lock);
    lifecycle_status = registration_generation_status_locked(generation);
    if (lifecycle_status != BZ_SC2A_OK) {
        pthread_mutex_unlock(&g_lock);
        image_release_locked(image);
        pthread_mutex_unlock(&g_source_lock);
        return placeholder_image(lifecycle_status);
    }
    {
        bzSC2Image_t *raced = find_cached_image_locked(identity);
        if (raced) {
            image_retain_locked(raced);
            pthread_mutex_unlock(&g_lock);
            image_release_locked(image);
            pthread_mutex_unlock(&g_source_lock);
            return raced;
        }
    }
    image->cache_next = g_image_cache;
    g_image_cache = image;
    image_retain_locked(image);
    if (image->placeholder) g_placeholder_logs++;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&g_source_lock);
    if (image->placeholder)
        fprintf(stderr, "SC2TabletopAssets: image '%s' unavailable (%d); cached placeholder\n",
                identity, status);
    return image;
}

const bzSC2Image_t *BZ_SC2A_RegisterImage(uint32_t abi_version, const char *identity) {
    bzSC2ASource_t source;
    char normalized[BZ_SC2A_MAX_IDENTITY];
    bzSC2AResult_t status;
    uint64_t generation;
    status = registration_context(abi_version, &source, &generation);
    if (status != BZ_SC2A_OK)
        return placeholder_image(status);
    if (!identity || !identity[0])
        return register_identity("", BZ_SC2A_ERR_NOT_FOUND, &source, generation);
    if (!sc2_tta_normalize_identity(identity, normalized, sizeof(normalized)))
        return register_identity(identity, BZ_SC2A_ERR_PATH_CONFINEMENT, &source, generation);
    return register_identity(normalized, BZ_SC2A_OK, &source, generation);
}

const bzSC2Image_t *BZ_SC2A_RegisterTerrainImage(uint32_t abi_version, const bzSC2Terrain_t *terrain,
                                                 uint32_t texture_index, bzSC2ATerrainChannel_t channel) {
    bzSC2ATerrainTextureInfo_t texture;
    bzSC2ASource_t source;
    char identity[BZ_SC2A_MAX_IDENTITY];
    bzSC2AResult_t status;
    uint64_t generation;
    if (channel != BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE && channel != BZ_SC2A_TERRAIN_CHANNEL_NORMAL)
        return placeholder_image(BZ_SC2A_ERR_INVALID_ARGUMENT);
    status = registration_context(abi_version, &source, &generation);
    if (status != BZ_SC2A_OK)
        return placeholder_image(status);
    if (!terrain || terrain->session_generation != generation ||
        !BZ_SC2ATerrain_TextureInfo(terrain, texture_index, &texture))
        return placeholder_image(BZ_SC2A_ERR_INVALID_ARGUMENT);
    snprintf(identity, sizeof(identity), "%s",
             channel == BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE ? texture.diffuse_identity : texture.normal_identity);
    return BZ_SC2A_RegisterImage(abi_version, identity);
}

void BZ_SC2AImage_Retain(const bzSC2Image_t *image) {
    if (!image) return;
    pthread_mutex_lock(&g_lock);
    image_retain_locked(image);
    pthread_mutex_unlock(&g_lock);
}

void BZ_SC2AImage_Release(const bzSC2Image_t *image) {
    if (!image) return;
    pthread_mutex_lock(&g_lock);
    image_release_locked(image);
    pthread_mutex_unlock(&g_lock);
}

bool BZ_SC2AImage_IsPlaceholder(const bzSC2Image_t *image) { return image ? image->placeholder : true; }
bzSC2AResult_t BZ_SC2AImage_Status(const bzSC2Image_t *image) {
    return image ? image->status : BZ_SC2A_ERR_INVALID_ARGUMENT;
}

bool BZ_SC2AImage_Identity(const bzSC2Image_t *image, char *out, size_t cap) {
    if (!image || !out || !cap) return false;
    snprintf(out, cap, "%s", image->identity);
    return true;
}

bool BZ_SC2AImage_Info(const bzSC2Image_t *image, bzSC2AImageInfo_t *out) {
    if (!image || !out) return false;
    *out = image->info;
    return true;
}

bool BZ_SC2AImage_MipInfo(const bzSC2Image_t *image, uint32_t index, bzSC2AImageMipInfo_t *out) {
    const bzSC2AImageMipInfo_t *src;
    if (!image || !out || index >= image->info.mip_count) return false;
    src = BZ_SC2A_ImageData((bzSC2Image_t *)image,
                            image->mips_offset + index * (uint32_t)sizeof(*src), sizeof(*src));
    if (!src) return false;
    *out = *src;
    return true;
}

uint32_t BZ_SC2AImage_CopyMip(const bzSC2Image_t *image, uint32_t index, void *dst, uint32_t cap) {
    bzSC2AImageMipInfo_t mip;
    uint32_t bytes;
    const void *src;
    if (!BZ_SC2AImage_MipInfo(image, index, &mip) || !dst || !cap) return 0;
    bytes = mip.size < cap ? mip.size : cap;
    src = BZ_SC2A_ImageData((bzSC2Image_t *)image, image->pixels_offset + mip.offset, bytes);
    if (!src) return 0;
    memcpy(dst, src, bytes);
    return bytes;
}

uint64_t BZ_SC2A_CacheHits(void) {
    uint64_t value;
    pthread_mutex_lock(&g_lock);
    value = g_cache_hits;
    pthread_mutex_unlock(&g_lock);
    return value;
}

uint64_t BZ_SC2A_CacheMisses(void) {
    uint64_t value;
    pthread_mutex_lock(&g_lock);
    value = g_cache_misses;
    pthread_mutex_unlock(&g_lock);
    return value;
}

uint64_t BZ_SC2A_PlaceholderLogs(void) {
    uint64_t value;
    pthread_mutex_lock(&g_lock);
    value = g_placeholder_logs;
    pthread_mutex_unlock(&g_lock);
    return value;
}
