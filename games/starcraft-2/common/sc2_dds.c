#include "sc2_dds.h"

#include <string.h>

/* DDS_PIXELFORMAT.dwFlags bits (DDPF_*), per the Microsoft DDS reference. */
#define SC2_DDPF_FOURCC      0x00000004u
#define SC2_DDPF_RGB         0x00000040u
#define SC2_DDPF_YUV         0x00000200u
#define SC2_DDPF_LUMINANCE   0x00020000u
#define SC2_DDSD_PITCH       0x00000008u
#define SC2_DDSCAPS2_CUBEMAP 0x00000200u
#define SC2_DDSCAPS2_VOLUME  0x00200000u

static DWORD sc2_dds_read_u32(BYTE const *p) {
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

/* Mip chains bottom out at 1x1; this counts exactly how many levels that takes so a claimed
 * dwMipMapCount can be bounds-checked against the file's own width/height instead of trusting it. */
static DWORD sc2_dds_max_mip_levels(DWORD width, DWORD height) {
    DWORD levels = 1;
    DWORD w = width, h = height;

    while (w > 1 || h > 1) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        levels++;
    }
    return levels;
}

/* Classifies the FourCC of a compressed pixel format, explicitly naming the unsupported
 * families (BC4/BC5/ATI2, DX10 extended header, packed YUV) instead of falling through a
 * single generic rejection, so diagnostics point at the real cause. */
static sc2DdsResult_t sc2_dds_parse_fourcc_format(DWORD fourcc, sc2DdsFormat_t *format, DWORD *blockBytes) {
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '1')) { *format = SC2_DDS_FORMAT_DXT1; *blockBytes = 8; return SC2_DDS_OK; }
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '3')) { *format = SC2_DDS_FORMAT_DXT3; *blockBytes = 16; return SC2_DDS_OK; }
    if (fourcc == MAKEFOURCC('D', 'X', 'T', '5')) { *format = SC2_DDS_FORMAT_DXT5; *blockBytes = 16; return SC2_DDS_OK; }
    if (fourcc == MAKEFOURCC('A', 'T', 'I', '1') || fourcc == MAKEFOURCC('A', 'T', 'I', '2') ||
        fourcc == MAKEFOURCC('B', 'C', '4', 'U') || fourcc == MAKEFOURCC('B', 'C', '4', 'S') ||
        fourcc == MAKEFOURCC('B', 'C', '5', 'U') || fourcc == MAKEFOURCC('B', 'C', '5', 'S')) {
        return SC2_DDS_ERR_UNSUPPORTED;
    }
    if (fourcc == MAKEFOURCC('D', 'X', '1', '0')) {
        return SC2_DDS_ERR_UNSUPPORTED;
    }
    if (fourcc == MAKEFOURCC('U', 'Y', 'V', 'Y') || fourcc == MAKEFOURCC('Y', 'U', 'Y', '2') ||
        fourcc == MAKEFOURCC('R', 'G', 'B', 'G') || fourcc == MAKEFOURCC('G', 'R', 'G', 'B')) {
        return SC2_DDS_ERR_UNSUPPORTED;
    }
    return SC2_DDS_ERR_UNSUPPORTED;
}

/* Matches the small set of uncompressed bit-mask layouts the desktop renderer can upload
 * directly (BGRA/RGBA/BGR/RGB). Any other mask combination is explicitly unsupported rather
 * than guessed at, since silently reinterpreting channel order would corrupt colors. */
static sc2DdsResult_t sc2_dds_parse_rgb_format(DWORD rgbBitCount, DWORD rMask, DWORD gMask, DWORD bMask, DWORD aMask,
                                               sc2DdsFormat_t *format, sc2DdsChannels_t *channels,
                                               DWORD *bitsPerPixel) {
    if (rgbBitCount == 32 && rMask == 0x00FF0000 && gMask == 0x0000FF00 && bMask == 0x000000FF && aMask == 0xFF000000) {
        *format = SC2_DDS_FORMAT_RGBA; *channels = SC2_DDS_CHANNELS_BGRA; *bitsPerPixel = 32;
        return SC2_DDS_OK;
    }
    if (rgbBitCount == 32 && rMask == 0x000000FF && gMask == 0x0000FF00 && bMask == 0x00FF0000 && aMask == 0xFF000000) {
        *format = SC2_DDS_FORMAT_RGBA; *channels = SC2_DDS_CHANNELS_RGBA; *bitsPerPixel = 32;
        return SC2_DDS_OK;
    }
    if (rgbBitCount == 24 && rMask == 0xFF0000 && gMask == 0x00FF00 && bMask == 0x0000FF && !aMask) {
        *format = SC2_DDS_FORMAT_RGB; *channels = SC2_DDS_CHANNELS_BGR; *bitsPerPixel = 24;
        return SC2_DDS_OK;
    }
    if (rgbBitCount == 24 && rMask == 0x000000FF && gMask == 0x0000FF00 && bMask == 0x00FF0000 && !aMask) {
        *format = SC2_DDS_FORMAT_RGB; *channels = SC2_DDS_CHANNELS_RGB; *bitsPerPixel = 24;
        return SC2_DDS_OK;
    }
    return SC2_DDS_ERR_UNSUPPORTED;
}

sc2DdsResult_t SC2_DdsParseResult(BYTE const *buf, DWORD filesize, sc2DdsImage_t *out) {
    DWORD headerSize, headerFlags, width, height, pitchOrLinearSize, depth, mipMapCount, caps2, maxMipLevels, levels;
    DWORD pfSize, pfFlags, fourcc, rgbBitCount, rMask, gMask, bMask, aMask;
    BYTE const *pf;
    ULONGLONG payload;
    DWORD w, h, i;

    sc2DdsResult_t result;
    if (!buf || !out)
        return SC2_DDS_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (filesize < SC2_DDS_DATA_OFFSET)
        return SC2_DDS_ERR_MALFORMED;
    if (buf[0] != 'D' || buf[1] != 'D' || buf[2] != 'S' || buf[3] != ' ')
        return SC2_DDS_ERR_MALFORMED;

    headerSize = sc2_dds_read_u32(buf + 4);
    if (headerSize != SC2_DDS_HEADER_SIZE)
        return SC2_DDS_ERR_MALFORMED;
    headerFlags  = sc2_dds_read_u32(buf + 8);
    height       = sc2_dds_read_u32(buf + 12);
    width        = sc2_dds_read_u32(buf + 16);
    pitchOrLinearSize = sc2_dds_read_u32(buf + 20);
    depth        = sc2_dds_read_u32(buf + 24);
    mipMapCount  = sc2_dds_read_u32(buf + 28);
    caps2        = sc2_dds_read_u32(buf + 112);

    if (!width || !height)
        return SC2_DDS_ERR_MALFORMED;
    if (width > SC2_DDS_MAX_DIMENSION || height > SC2_DDS_MAX_DIMENSION)
        return SC2_DDS_ERR_TOO_LARGE;
    if (depth > 1)
        return SC2_DDS_ERR_UNSUPPORTED;
    if (caps2 & (SC2_DDSCAPS2_CUBEMAP | SC2_DDSCAPS2_VOLUME))
        return SC2_DDS_ERR_UNSUPPORTED;

    pf = buf + 76;
    pfSize       = sc2_dds_read_u32(pf);
    pfFlags      = sc2_dds_read_u32(pf + 4);
    fourcc       = sc2_dds_read_u32(pf + 8);
    rgbBitCount  = sc2_dds_read_u32(pf + 12);
    rMask        = sc2_dds_read_u32(pf + 16);
    gMask        = sc2_dds_read_u32(pf + 20);
    bMask        = sc2_dds_read_u32(pf + 24);
    aMask        = sc2_dds_read_u32(pf + 28);

    if (pfSize != SC2_DDS_PIXELFORMAT_SIZE)
        return SC2_DDS_ERR_MALFORMED;

    if (pfFlags & SC2_DDPF_YUV)
        return SC2_DDS_ERR_UNSUPPORTED;
    if (pfFlags & SC2_DDPF_LUMINANCE)
        return SC2_DDS_ERR_UNSUPPORTED;
    if (pfFlags & SC2_DDPF_FOURCC) {
        result = sc2_dds_parse_fourcc_format(fourcc, &out->format, &out->blockBytes);
        if (result != SC2_DDS_OK) return result;
        out->channels = SC2_DDS_CHANNELS_NONE;
    } else if (pfFlags & SC2_DDPF_RGB) {
        result = sc2_dds_parse_rgb_format(rgbBitCount, rMask, gMask, bMask, aMask,
                                          &out->format, &out->channels, &out->bitsPerPixel);
        if (result != SC2_DDS_OK) return result;
    } else {
        return SC2_DDS_ERR_UNSUPPORTED;
    }
    /* DDS file rows are byte-tight by the Microsoft file-layout formula. A larger declared
     * runtime pitch does not encode lower-mip strides, so reject it rather than misaddressing
     * retained mip bytes; the desktop uploader pairs this contract with unpack alignment 1. */
    if (!out->blockBytes && headerFlags & SC2_DDSD_PITCH) {
        DWORD tightPitch = width * (out->bitsPerPixel / 8);
        if (!pitchOrLinearSize)
            return SC2_DDS_ERR_MALFORMED;
        if (pitchOrLinearSize != tightPitch)
            return SC2_DDS_ERR_UNSUPPORTED;
    }

    maxMipLevels = sc2_dds_max_mip_levels(width, height);
    levels = mipMapCount == 0 ? 1 : mipMapCount;
    if (levels > maxMipLevels || levels > SC2_DDS_MAX_MIP_LEVELS)
        return SC2_DDS_ERR_MALFORMED;

    payload = 0;
    w = width; h = height;
    for (i = 0; i < levels; i++) {
        ULONGLONG levelSize, rowBytes;

        if (out->blockBytes) {
            ULONGLONG blocksWide = ((ULONGLONG)w + 3) / 4;
            ULONGLONG blocksHigh = ((ULONGLONG)h + 3) / 4;
            rowBytes = blocksWide * out->blockBytes;
            levelSize = rowBytes * blocksHigh;
        } else {
            rowBytes = (ULONGLONG)w * (out->bitsPerPixel / 8);
            levelSize = rowBytes * h;
        }
        if (levelSize > SC2_DDS_MAX_PAYLOAD_BYTES || payload + levelSize > SC2_DDS_MAX_PAYLOAD_BYTES)
            return SC2_DDS_ERR_TOO_LARGE;
        out->mipLevels[i].width  = w;
        out->mipLevels[i].height = h;
        out->mipLevels[i].offset = (DWORD)payload;
        out->mipLevels[i].size   = (DWORD)levelSize;
        out->mipLevels[i].rowBytes = (DWORD)rowBytes;
        payload += levelSize;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if ((ULONGLONG)SC2_DDS_DATA_OFFSET + payload > (ULONGLONG)filesize)
        return SC2_DDS_ERR_MALFORMED;

    out->width = width;
    out->height = height;
    out->dataOffset = SC2_DDS_DATA_OFFSET;
    out->mipLevelCount = levels;
    return SC2_DDS_OK;
}

LPCSTR SC2_DdsResultString(sc2DdsResult_t result) {
    switch (result) {
    case SC2_DDS_OK: return "ok";
    case SC2_DDS_ERR_INVALID_ARGUMENT: return "invalid argument";
    case SC2_DDS_ERR_MALFORMED: return "malformed or truncated DDS";
    case SC2_DDS_ERR_UNSUPPORTED: return "unsupported DDS format";
    case SC2_DDS_ERR_TOO_LARGE: return "DDS exceeds bounded dimensions or payload";
    default: return "unknown DDS result";
    }
}
