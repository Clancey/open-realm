#ifndef SC2_DDS_H
#define SC2_DDS_H

/*
 * sc2_dds.h - pure DDS (DirectDraw Surface) header/mip validator.
 *
 * File-shaped, engine-agnostic: no OpenGL types, no allocation, no I/O. Callers hand this a
 * memory-mapped/loaded file buffer and get back format + per-mip offset/size metadata, or a
 * rejection with a logged reason. Renderer upload code (renderer/r_dds.c) consumes this so the
 * bounds/format checks live in one place instead of being re-derived at every upload site.
 */

#include "common/shared.h"

#define SC2_DDS_MAX_DIMENSION      16384u
#define SC2_DDS_MAX_PAYLOAD_BYTES  (256u * 1024u * 1024u)
#define SC2_DDS_MAX_MIP_LEVELS     16   /* log2(SC2_DDS_MAX_DIMENSION)+1 == 15; +1 safety margin */
#define SC2_DDS_MAGIC_SIZE         4
#define SC2_DDS_HEADER_SIZE        124  /* DDS_HEADER.dwSize; fixed per DDS spec */
#define SC2_DDS_PIXELFORMAT_SIZE   32   /* DDS_PIXELFORMAT.dwSize; fixed per DDS spec */
#define SC2_DDS_DATA_OFFSET        (SC2_DDS_MAGIC_SIZE + SC2_DDS_HEADER_SIZE) /* 128 */

typedef enum {
    SC2_DDS_FORMAT_NONE = 0,
    SC2_DDS_FORMAT_DXT1,
    SC2_DDS_FORMAT_DXT3,
    SC2_DDS_FORMAT_DXT5,
    SC2_DDS_FORMAT_RGB,   /* uncompressed 24bpp; byte order in `channels` */
    SC2_DDS_FORMAT_RGBA,  /* uncompressed 32bpp; byte order in `channels` */
} sc2DdsFormat_t;

typedef enum {
    SC2_DDS_CHANNELS_NONE = 0,  /* compressed formats carry no channel order */
    SC2_DDS_CHANNELS_RGB,
    SC2_DDS_CHANNELS_BGR,
    SC2_DDS_CHANNELS_RGBA,
    SC2_DDS_CHANNELS_BGRA,
} sc2DdsChannels_t;

typedef enum {
    SC2_DDS_OK = 0,
    SC2_DDS_ERR_INVALID_ARGUMENT,
    SC2_DDS_ERR_MALFORMED,
    SC2_DDS_ERR_UNSUPPORTED,
    SC2_DDS_ERR_TOO_LARGE,
} sc2DdsResult_t;

typedef struct {
    DWORD width;
    DWORD height;
    DWORD offset;  /* byte offset from the start of the pixel payload (buf + SC2_DDS_DATA_OFFSET) */
    DWORD size;    /* byte size of this level's data */
    DWORD rowBytes; /* compressed block-row bytes or uncompressed pixel-row bytes */
} sc2DdsMipLevel_t;

typedef struct {
    sc2DdsFormat_t   format;
    sc2DdsChannels_t channels;
    DWORD            width;
    DWORD            height;
    DWORD            bitsPerPixel;      /* uncompressed only; 0 for compressed formats */
    DWORD            blockBytes;        /* compressed only (8=DXT1, 16=DXT3/DXT5); 0 otherwise */
    DWORD            dataOffset;        /* == SC2_DDS_DATA_OFFSET; kept for caller convenience */
    DWORD            mipLevelCount;
    sc2DdsMipLevel_t mipLevels[SC2_DDS_MAX_MIP_LEVELS];
} sc2DdsImage_t;
typedef sc2DdsImage_t       *LPSC2DDSIMAGE;
typedef sc2DdsImage_t const *LPCSC2DDSIMAGE;

sc2DdsResult_t SC2_DdsParseResult(BYTE const *buf, DWORD filesize, sc2DdsImage_t *out);
LPCSTR SC2_DdsResultString(sc2DdsResult_t result);
static inline BOOL SC2_DdsParse(BYTE const *buf, DWORD filesize, sc2DdsImage_t *out) {
    return SC2_DdsParseResult(buf, filesize, out) == SC2_DDS_OK;
}

#endif
