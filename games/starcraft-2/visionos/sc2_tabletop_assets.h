/*
 * sc2_tabletop_assets.h - immutable StarCraft II terrain and terrain-texture export ABI.
 *
 * Layer 2A: a distinct, independently versioned ABI. It is plain C99/POD, exposes no engine,
 * renderer, SDL, OpenGL, Objective-C, Swift, or RealityKit types, and must never be confused
 * with platform/bridge/bz_tabletop_assets.h (BZ_TABLETOP_ASSETS_ABI_VERSION 2, Warcraft-shaped).
 * That header is untouched by this one; this ABI owns terrain plus generic encoded-image handles
 * consumed by the separate SC2 model ABI.
 */
#ifndef SC2_TABLETOP_ASSETS_H
#define SC2_TABLETOP_ASSETS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BZ_SC2_TABLETOP_ASSETS_ABI_VERSION 1u
#define BZ_SC2A_ABI_VERSION BZ_SC2_TABLETOP_ASSETS_ABI_VERSION

enum {
    BZ_SC2A_MAX_IDENTITY = 256,          /* matches sc2TerrainTexture_t.diffuse/normal size */
    BZ_SC2A_MAX_CATALOG_NAME = 64,       /* matches sc2CliffSet_t.name/mesh size */
    BZ_SC2A_TERRAIN_MAX_DIMENSION = 1024, /* cap on cell grid; HMAP may add one corner sample */
    BZ_SC2A_MAX_TERRAIN_TEXTURES = 16,
    BZ_SC2A_MAX_CLIFF_SETS = 8,
    BZ_SC2A_MAX_CLIFF_CELLS = 16384,
    BZ_SC2A_MASK_MAX_DIMENSION = 8192, /* t3TextureMasks use an 8x terrain-cell grid */
    BZ_SC2A_IMAGE_MAX_DIMENSION = 16384,
};
#define BZ_SC2A_MASK_MAX_BYTES  (256u * 1024u * 1024u)
#define BZ_SC2A_IMAGE_MAX_BYTES (256u * 1024u * 1024u)

typedef struct bzSC2Terrain bzSC2Terrain_t;
typedef struct bzSC2Image bzSC2Image_t;

/* Append-only: add new members after OUT_OF_MEMORY, never renumber existing ones. */
typedef enum {
    BZ_SC2A_OK = 0,
    BZ_SC2A_ERR_NOT_INITIALIZED,
    BZ_SC2A_ERR_TERMINAL,
    BZ_SC2A_ERR_ABI_VERSION,
    BZ_SC2A_ERR_INVALID_ARGUMENT,
    BZ_SC2A_ERR_PATH_CONFINEMENT,
    BZ_SC2A_ERR_NOT_FOUND,
    BZ_SC2A_ERR_MALFORMED,
    BZ_SC2A_ERR_UNSUPPORTED,
    BZ_SC2A_ERR_TOO_LARGE,
    BZ_SC2A_ERR_OUT_OF_MEMORY,
} bzSC2AResult_t;

/* t3Terrain.xml <texture>/<texture ... normal> resolve to one identity each; DDS only. */
typedef enum {
    BZ_SC2A_PIXEL_DXT1 = 1,
    BZ_SC2A_PIXEL_DXT3,
    BZ_SC2A_PIXEL_DXT5,
    BZ_SC2A_PIXEL_RGB8,
    BZ_SC2A_PIXEL_BGR8,
    BZ_SC2A_PIXEL_RGBA8,
    BZ_SC2A_PIXEL_BGRA8,
} bzSC2APixelFormat_t;

typedef enum {
    BZ_SC2A_ORIGIN_TOP_LEFT = 1,
} bzSC2AImageOrigin_t;

typedef enum {
    BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE = 1,
    BZ_SC2A_TERRAIN_CHANNEL_NORMAL = 2,
} bzSC2ATerrainChannel_t;

/* One bit per embedded terrain file this parser decodes; see games/starcraft-2/docs/
 * embedded-map-files.md "File Index" for the source-of-truth per-file parse status. */
enum {
    BZ_SC2A_TERRAIN_HAS_HMAP = 1u << 0,     /* t3HeightMap present and bounds-valid */
    BZ_SC2A_TERRAIN_HAS_SMAP = 1u << 1,     /* t3SyncHeightMap present and bounds-valid */
    BZ_SC2A_TERRAIN_HAS_LFCT = 1u << 2,     /* t3CellFlags present and bounds-valid */
    BZ_SC2A_TERRAIN_HAS_CLIF = 1u << 3,     /* t3SyncCliffLevel present and bounds-valid */
    BZ_SC2A_TERRAIN_HAS_MASK = 1u << 4,     /* t3TextureMasks present and bounds-valid */
    BZ_SC2A_TERRAIN_HAS_FOG = 1u << 5,      /* t3Terrain.xml fog block enabled */
    BZ_SC2A_TERRAIN_HAS_LIGHTING = 1u << 6, /* Objects/LightData lighting block enabled */
};

/* Files the parser knows about but does not (yet, or ever) decode into this snapshot. */
enum {
    BZ_SC2A_TERRAIN_UNSUPPORTED_WATER = 1u << 0,           /* t3Water */
    BZ_SC2A_TERRAIN_UNSUPPORTED_PATHING = 1u << 1,         /* t3SyncPathingInfo */
    BZ_SC2A_TERRAIN_UNSUPPORTED_FLUFF_DOODAD = 1u << 2,    /* t3FluffDoodad */
    BZ_SC2A_TERRAIN_UNSUPPORTED_HARD_TILE = 1u << 3,       /* t3HardTile */
    BZ_SC2A_TERRAIN_UNSUPPORTED_VERTEX_COLOR = 1u << 4,    /* t3VertCol; layout unknown, never decoded */
    BZ_SC2A_TERRAIN_UNSUPPORTED_PAINTED_PATHING = 1u << 5, /* PaintedPathingLayer; partially understood only */
};

typedef struct {
    uint64_t generation;          /* sc2Map_t.generation at publication time */
    uint32_t availability_flags;  /* BZ_SC2A_TERRAIN_HAS_* */
    uint32_t malformed_flags;     /* matching HAS_* bit identifies a present malformed layer */
    uint32_t unsupported_flags;   /* BZ_SC2A_TERRAIN_UNSUPPORTED_* */
    uint32_t cell_width, cell_height;               /* MapInfo authoritative cell grid */
    uint32_t hmap_width, hmap_height;                /* t3HeightMap corner grid, 0 if absent */
    uint32_t mask_width, mask_height, mask_layer_count; /* t3TextureMasks, 0 if absent */
    uint32_t texture_count;
    uint32_t cliff_set_count;
    uint32_t cliff_cell_count;
    float origin_x, origin_y;
    float cell_size;
    float height_quantize_bias, height_quantize_scale, standard_height;
    uint32_t fog_enabled;
    float fog_density, fog_falloff, fog_start_height;
    uint32_t fog_color; /* packed 0xAABBGGRR, byte order matches COLOR32{r,g,b,a} */
} bzSC2ATerrainInfo_t;

typedef struct {
    uint32_t index;
    uint32_t flags;
    char diffuse_identity[BZ_SC2A_MAX_IDENTITY];
    char normal_identity[BZ_SC2A_MAX_IDENTITY];
} bzSC2ATerrainTextureInfo_t;

typedef struct {
    uint32_t index;
    uint32_t flags;
    char name[BZ_SC2A_MAX_CATALOG_NAME];
    char mesh[BZ_SC2A_MAX_CATALOG_NAME];
} bzSC2ACliffSetInfo_t;

typedef struct {
    uint32_t flat_index; /* row-major index into the (cell_width+1)/2-wide half-res cliff grid */
    uint32_t flags;
    uint32_t cliff_set;  /* index into cliff_sets[] */
    uint32_t variant;
} bzSC2ACliffCellInfo_t;

typedef struct {
    float height;       /* decoded: (raw_height + raw_adjustment) * scale - bias - standardHeight - 1 */
    float adjustment;    /* decoded: raw_adjustment * scale */
    uint16_t raw_mask;   /* t3HeightMap mask field, 0x00-0x03 */
} bzSC2AHeightSample_t;

typedef struct {
    uint16_t cliff_level; /* decoded t3SyncCliffLevel value; 0 if CLIF layer absent */
    uint8_t cell_flags;   /* raw t3CellFlags byte; test (value & 0x0f) for cliff holes */
    uint8_t reserved;
} bzSC2ACellInfo_t;

typedef struct {
    uint32_t format; /* bzSC2APixelFormat_t */
    uint32_t width, height;
    uint32_t mip_count;
    uint32_t data_bytes; /* total payload bytes across every mip level */
    uint32_t flags;
    uint32_t origin;
    uint32_t reserved;
} bzSC2AImageInfo_t;

typedef struct {
    uint32_t width, height;
    uint32_t offset; /* byte offset from the start of the pixel payload */
    uint32_t size;
    uint32_t row_bytes;
} bzSC2AImageMipInfo_t;

void BZ_SC2A_Init(void);
void BZ_SC2A_Shutdown(void);
uint32_t BZ_SC2A_AbiVersion(void);

/* Called on the engine thread after SC2_MapLoad() has produced a consistent sc2Map_t. */
void BZ_SC2A_PublishTerrainFromGame(void);
/* Always returns a retained handle once initialized: either the latest published terrain, or a
 * placeholder whose BZ_SC2ATerrain_Status() reports why nothing has published successfully yet. */
const bzSC2Terrain_t *BZ_SC2A_LatestTerrain(uint32_t abi_version);
void BZ_SC2ATerrain_Retain(const bzSC2Terrain_t *terrain);
void BZ_SC2ATerrain_Release(const bzSC2Terrain_t *terrain);
bool BZ_SC2ATerrain_IsPlaceholder(const bzSC2Terrain_t *terrain);
bzSC2AResult_t BZ_SC2ATerrain_Status(const bzSC2Terrain_t *terrain);
bool BZ_SC2ATerrain_Info(const bzSC2Terrain_t *terrain, bzSC2ATerrainInfo_t *out);
bool BZ_SC2ATerrain_TextureInfo(const bzSC2Terrain_t *terrain, uint32_t index, bzSC2ATerrainTextureInfo_t *out);
bool BZ_SC2ATerrain_CliffSetInfo(const bzSC2Terrain_t *terrain, uint32_t index, bzSC2ACliffSetInfo_t *out);
bool BZ_SC2ATerrain_CliffCellInfo(const bzSC2Terrain_t *terrain, uint32_t index, bzSC2ACliffCellInfo_t *out);
bool BZ_SC2ATerrain_HeightSample(const bzSC2Terrain_t *terrain, uint32_t x, uint32_t y, bzSC2AHeightSample_t *out);
bool BZ_SC2ATerrain_CellInfo(const bzSC2Terrain_t *terrain, uint32_t x, uint32_t y, bzSC2ACellInfo_t *out);
/* Copies one decoded (nibble-expanded, one byte per texel, values 0-15) t3TextureMasks layer. */
uint32_t BZ_SC2ATerrain_CopyTextureMaskLayer(const bzSC2Terrain_t *terrain, uint32_t layer,
                                             uint8_t *dst, uint32_t cap);

/* Resolves terrain_textures[texture_index]'s diffuse or normal identity through the confined
 * filesystem path and returns a retained, cross-reload-cached image (or cached placeholder). */
const bzSC2Image_t *BZ_SC2A_RegisterTerrainImage(uint32_t abi_version, const bzSC2Terrain_t *terrain,
                                                 uint32_t texture_index, bzSC2ATerrainChannel_t channel);
/* Resolves any confined SC2 DDS identity through the same cache used by terrain and model layers. */
const bzSC2Image_t *BZ_SC2A_RegisterImage(uint32_t abi_version, const char *identity);
void BZ_SC2AImage_Retain(const bzSC2Image_t *image);
void BZ_SC2AImage_Release(const bzSC2Image_t *image);
bool BZ_SC2AImage_IsPlaceholder(const bzSC2Image_t *image);
bzSC2AResult_t BZ_SC2AImage_Status(const bzSC2Image_t *image);
bool BZ_SC2AImage_Identity(const bzSC2Image_t *image, char *out, size_t cap);
bool BZ_SC2AImage_Info(const bzSC2Image_t *image, bzSC2AImageInfo_t *out);
bool BZ_SC2AImage_MipInfo(const bzSC2Image_t *image, uint32_t index, bzSC2AImageMipInfo_t *out);
uint32_t BZ_SC2AImage_CopyMip(const bzSC2Image_t *image, uint32_t index, void *dst, uint32_t cap);

/* Stable counters for tests and host diagnostics. */
uint64_t BZ_SC2A_CacheHits(void);
uint64_t BZ_SC2A_CacheMisses(void);
uint64_t BZ_SC2A_PlaceholderLogs(void);

#ifdef __cplusplus
}
#endif

#endif
