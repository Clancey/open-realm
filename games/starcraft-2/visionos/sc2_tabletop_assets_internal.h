#ifndef SC2_TABLETOP_ASSETS_INTERNAL_H
#define SC2_TABLETOP_ASSETS_INTERNAL_H

#include <string.h>

#include "sc2_tabletop_assets.h"

/* Archive identities must remain relative, backslash-separated (SC2's MPQ convention; see
 * common/sc2_utils.h sc2_normalize_slashes), and free of empty/traversal components. Mirrors
 * wc3_tta_path_is_confined()'s algorithm (games/warcraft-3/visionos/wc3_tabletop_assets_internal.h)
 * but is a distinct, self-contained copy: SC2 must never include or depend on WC3 headers. */
static inline bool sc2_tta_path_is_confined(const char *identity) {
    const char *segment, *end;
    if (!identity || !*identity || strlen(identity) >= BZ_SC2A_MAX_IDENTITY ||
        identity[0] == '/' || identity[0] == '\\')
        return false;
    for (const unsigned char *p = (const unsigned char *)identity; *p; p++)
        if (*p < 0x20 || *p == ':' || *p == 0x7f)
            return false;
    for (segment = identity;; segment = end + 1) {
        for (end = segment; *end && *end != '/' && *end != '\\'; end++);
        if (end == segment || (end - segment == 1 && segment[0] == '.') ||
            (end - segment == 2 && segment[0] == '.' && segment[1] == '.'))
            return false;
        if (!*end) return true;
        if (!end[1]) return false;
    }
}

static inline bool sc2_tta_normalize_identity(const char *identity, char *out, size_t cap) {
    size_t len;
    if (!out || !cap || !sc2_tta_path_is_confined(identity))
        return false;
    len = strlen(identity);
    if (len >= cap)
        return false;
    for (size_t i = 0; i <= len; i++)
        out[i] = identity[i] == '/' ? '\\' : identity[i];
    return true;
}

struct bzSC2Terrain {
    int refcount;
    uintptr_t source_token;
    uint64_t session_generation; /* BZ_SC2A_Init() session counter; NOT info.generation (map's own) */
    bzSC2AResult_t status;
    bool placeholder;
    bzSC2ATerrainInfo_t info;
    uint32_t textures_offset;       /* bzSC2ATerrainTextureInfo_t[info.texture_count] */
    uint32_t cliff_sets_offset;     /* bzSC2ACliffSetInfo_t[info.cliff_set_count] */
    uint32_t cliff_cells_offset;    /* bzSC2ACliffCellInfo_t[info.cliff_cell_count] */
    uint32_t height_samples_offset; /* bzSC2AHeightSample_t[hmap_width * hmap_height] */
    uint32_t cells_offset;          /* bzSC2ACellInfo_t[cell_width * cell_height] */
    uint32_t mask_offset;           /* uint8_t[mask_width * mask_height * mask_layer_count] */
    size_t allocation_size;
    unsigned char data[];
};

struct bzSC2Image {
    int refcount;
    bzSC2AResult_t status;
    bool placeholder;
    char identity[BZ_SC2A_MAX_IDENTITY];
    char cache_identity[BZ_SC2A_MAX_IDENTITY];
    bzSC2AImageInfo_t info;
    uint32_t mips_offset;   /* bzSC2AImageMipInfo_t[info.mip_count] */
    uint32_t pixels_offset; /* uint8_t[info.data_bytes] */
    size_t allocation_size;
    struct bzSC2Image *cache_next;
    unsigned char data[];
};

typedef struct {
    bool (*path_is_confined)(const char *identity);
    /* Returns a malloc'd buffer the caller frees with free_file(), or NULL on failure. */
    void *(*read_file)(const char *identity, uint32_t *size);
    void (*free_file)(void *data);
    uintptr_t (*terrain_token)(void);
    /* Builds one fully-populated, immutable bzSC2Terrain_t (via BZ_SC2A_TerrainAlloc/Data below)
     * from the current authoritative map state, or returns NULL with *status set. */
    bzSC2Terrain_t *(*copy_terrain)(uintptr_t *source_token, bzSC2AResult_t *status);
} bzSC2ASource_t;

bzSC2Terrain_t *BZ_SC2A_TerrainAlloc(size_t payload_bytes);
bzSC2Image_t *BZ_SC2A_ImageAlloc(size_t payload_bytes);
void *BZ_SC2A_TerrainData(bzSC2Terrain_t *terrain, uint32_t offset, size_t bytes);
void *BZ_SC2A_ImageData(bzSC2Image_t *image, uint32_t offset, size_t bytes);

/* Installed by the selected game's static archive (games/starcraft-2/visionos/sc2_tabletop_game.c
 * in production; test_sc2_tabletop_assets.c installs its own fixture-backed source instead). */
void BZ_SC2_TTA_Source(bzSC2ASource_t *source);
void BZ_SC2A_ProviderLock(void);
void BZ_SC2A_ProviderUnlock(void);

#ifdef BZ_SC2A_TEST_HOOKS
void BZ_SC2A_TestTerrainImageValidated(void);
#endif

#endif
