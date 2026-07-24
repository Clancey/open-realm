#include "wc3_tabletop_assets_internal.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLP_MAX_DIMENSION 8192u
#define STBI_MAX_DIMENSIONS BLP_MAX_DIMENSION
#define STB_IMAGE_IMPLEMENTATION
#define STBI_WARCRAFT3_BLP_JPEG_RGBA_BANDS
#include "renderer/stb/stb_image.h"
#undef STBI_WARCRAFT3_BLP_JPEG_RGBA_BANDS
#undef STB_IMAGE_IMPLEMENTATION
#undef STBI_MAX_DIMENSIONS

#define BLP1_MAGIC 0x31504c42u
#define BLP2_MAGIC 0x32504c42u
#define BLP_MIPS 16

#pragma pack(push, 1)
typedef struct {
    uint32_t magic, type, alpha_bits, width, height, extra, has_mips;
    uint32_t offsets[BLP_MIPS], lengths[BLP_MIPS];
} blp1Header_t;

typedef struct {
    uint32_t magic, type;
    uint8_t encoding, alpha_depth, alpha_encoding, has_mips;
    uint32_t width, height;
    uint32_t offsets[BLP_MIPS], lengths[BLP_MIPS];
    uint8_t palette[256][4];
} blp2Header_t;
#pragma pack(pop)

static bool span_ok(size_t size, uint32_t offset, size_t bytes) {
    return (size_t)offset <= size && bytes <= size - offset;
}

static bool image_bytes(uint32_t width, uint32_t height, size_t *bytes) {
    if (!width || !height || width > BLP_MAX_DIMENSION || height > BLP_MAX_DIMENSION ||
        width > SIZE_MAX / height || (size_t)width * height > SIZE_MAX / 4)
        return false;
    *bytes = (size_t)width * height * 4;
    return true;
}

static bzTTAsset_t *image_asset(const char *identity, const bzTTAssetMetadata_t *metadata,
                                uint32_t width, uint32_t height, const uint8_t *pixels,
                                bzTTAResult_t *status) {
    size_t bytes;
    bzTTAsset_t *asset;
    if (!image_bytes(width, height, &bytes)) {
        *status = BZ_TTA_ERR_MALFORMED;
        return NULL;
    }
    asset = BZ_TTA_AssetAlloc(bytes, BZ_TTA_ASSET_IMAGE, identity, metadata);
    if (!asset) {
        *status = BZ_TTA_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    asset->u.image.info = (bzTTImageInfo_t){
        .width = width, .height = height, .row_bytes = width * 4, .data_bytes = (uint32_t)bytes,
        .format = BZ_TTA_PIXEL_RGBA8, .origin = BZ_TTA_ORIGIN_TOP_LEFT,
    };
    asset->u.image.pixels_offset = 0;
    memcpy(asset->data, pixels, bytes);
    *status = BZ_TTA_OK;
    return asset;
}

static void palette_pixel(uint8_t *dst, const uint8_t palette[256][4], uint8_t index, uint8_t alpha) {
    const uint8_t *src = palette[index]; /* file order is BGRA */
    dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = alpha;
}

static uint8_t packed_alpha(const uint8_t *alpha, size_t pixel, uint8_t depth) {
    if (depth == 0) return 255;
    if (depth == 1) return alpha[pixel >> 3] & (1u << (pixel & 7)) ? 255 : 0;
    if (depth == 4) {
        uint8_t value = (alpha[pixel >> 1] >> ((pixel & 1) * 4)) & 15;
        return (uint8_t)((value << 4) | value);
    }
    return alpha[pixel];
}

static bzTTAsset_t *decode_paletted(const uint8_t *src, size_t src_size,
                                    const uint8_t palette[256][4], uint8_t alpha_depth,
                                    uint32_t width, uint32_t height, const char *identity,
                                    const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status) {
    size_t pixels = (size_t)width * height, alpha_bytes, rgba_bytes;
    uint8_t *rgba;
    if (!image_bytes(width, height, &rgba_bytes) || (alpha_depth != 0 && alpha_depth != 1 &&
        alpha_depth != 4 && alpha_depth != 8)) {
        *status = BZ_TTA_ERR_MALFORMED;
        return NULL;
    }
    alpha_bytes = alpha_depth == 0 ? 0 : (pixels * alpha_depth + 7) / 8;
    if (pixels > src_size || alpha_bytes > src_size - pixels) {
        *status = BZ_TTA_ERR_MALFORMED;
        return NULL;
    }
    rgba = malloc(rgba_bytes);
    if (!rgba) {
        *status = BZ_TTA_ERR_OUT_OF_MEMORY;
        return NULL;
    }
    for (size_t i = 0; i < pixels; i++)
        palette_pixel(rgba + i * 4, palette, src[i], packed_alpha(src + pixels, i, alpha_depth));
    bzTTAsset_t *asset = image_asset(identity, metadata, width, height, rgba, status);
    free(rgba);
    return asset;
}

static void dxt_color(uint16_t value, uint8_t alpha, uint8_t out[4]) {
    out[0] = (uint8_t)(((value >> 11) & 31) * 255 / 31);
    out[1] = (uint8_t)(((value >> 5) & 63) * 255 / 63);
    out[2] = (uint8_t)((value & 31) * 255 / 31);
    out[3] = alpha;
}

static void dxt_colors(const uint8_t *block, uint8_t colors[4][4], bool force_four) {
    uint16_t c0 = (uint16_t)(block[0] | block[1] << 8);
    uint16_t c1 = (uint16_t)(block[2] | block[3] << 8);
    dxt_color(c0, 255, colors[0]); dxt_color(c1, 255, colors[1]);
    if (force_four || c0 > c1) {
        for (int c = 0; c < 3; c++) {
            colors[2][c] = (uint8_t)((2 * colors[0][c] + colors[1][c]) / 3);
            colors[3][c] = (uint8_t)((colors[0][c] + 2 * colors[1][c]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (int c = 0; c < 3; c++) colors[2][c] = (uint8_t)((colors[0][c] + colors[1][c]) / 2);
        colors[2][3] = 255; memset(colors[3], 0, 4);
    }
}

static void dxt5_alpha(const uint8_t *block, uint8_t alpha[16]) {
    uint8_t table[8] = { block[0], block[1] };
    uint64_t bits = 0;
    if (table[0] > table[1])
        for (int i = 2; i < 8; i++) table[i] = (uint8_t)(((8 - i) * table[0] + (i - 1) * table[1]) / 7);
    else {
        for (int i = 2; i < 6; i++) table[i] = (uint8_t)(((6 - i) * table[0] + (i - 1) * table[1]) / 5);
        table[6] = 0; table[7] = 255;
    }
    for (int i = 0; i < 6; i++) bits |= (uint64_t)block[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) alpha[i] = table[(bits >> (3 * i)) & 7];
}

static bzTTAsset_t *decode_dxt(const uint8_t *src, size_t src_size, uint8_t format,
                               uint32_t width, uint32_t height, const char *identity,
                               const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status) {
    size_t rgba_bytes, blocks_x = (width + 3) / 4, blocks_y = (height + 3) / 4;
    size_t block_bytes = format == 1 ? 8 : 16, required;
    uint8_t *rgba;
    if (!image_bytes(width, height, &rgba_bytes) || blocks_x > SIZE_MAX / blocks_y ||
        blocks_x * blocks_y > SIZE_MAX / block_bytes ||
        (required = blocks_x * blocks_y * block_bytes) > src_size) {
        *status = BZ_TTA_ERR_MALFORMED;
        return NULL;
    }
    rgba = calloc(1, rgba_bytes);
    if (!rgba) { *status = BZ_TTA_ERR_OUT_OF_MEMORY; return NULL; }
    for (size_t by = 0; by < blocks_y; by++) for (size_t bx = 0; bx < blocks_x; bx++) {
        uint8_t alpha[16], colors[4][4];
        const uint8_t *color_block;
        uint32_t indices;
        memset(alpha, 255, sizeof(alpha));
        if (format == 3) {
            for (int i = 0; i < 16; i++) {
                uint8_t value = (src[i / 2] >> ((i & 1) * 4)) & 15;
                alpha[i] = (uint8_t)((value << 4) | value);
            }
            color_block = src + 8;
        } else if (format == 5) {
            dxt5_alpha(src, alpha); color_block = src + 8;
        } else color_block = src;
        dxt_colors(color_block, colors, format != 1);
        indices = (uint32_t)color_block[4] | (uint32_t)color_block[5] << 8 |
                  (uint32_t)color_block[6] << 16 | (uint32_t)color_block[7] << 24;
        for (uint32_t py = 0; py < 4; py++) for (uint32_t px = 0; px < 4; px++) {
            uint32_t x = (uint32_t)bx * 4 + px, y = (uint32_t)by * 4 + py, i = py * 4 + px;
            if (x >= width || y >= height) continue;
            memcpy(rgba + ((size_t)y * width + x) * 4, colors[(indices >> (2 * i)) & 3], 4);
            if (format != 1) rgba[((size_t)y * width + x) * 4 + 3] = alpha[i];
        }
        src += block_bytes;
    }
    bzTTAsset_t *asset = image_asset(identity, metadata, width, height, rgba, status);
    free(rgba);
    return asset;
}

static bzTTAsset_t *decode_blp2(const uint8_t *data, size_t size, const char *identity,
                                const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status) {
    blp2Header_t header;
    size_t rgba_bytes;
    const uint8_t *src;
    if (size < sizeof(header)) { *status = BZ_TTA_ERR_MALFORMED; return NULL; }
    memcpy(&header, data, sizeof(header));
    if (!image_bytes(header.width, header.height, &rgba_bytes) ||
        !span_ok(size, header.offsets[0], header.lengths[0])) {
        *status = BZ_TTA_ERR_MALFORMED; return NULL;
    }
    src = data + header.offsets[0];
    if (header.encoding == 1)
        return decode_paletted(src, header.lengths[0], header.palette, header.alpha_depth,
                               header.width, header.height, identity, metadata, status);
    if (header.encoding == 3) {
        uint8_t *rgba;
        if (header.lengths[0] < rgba_bytes) { *status = BZ_TTA_ERR_MALFORMED; return NULL; }
        rgba = malloc(rgba_bytes);
        if (!rgba) { *status = BZ_TTA_ERR_OUT_OF_MEMORY; return NULL; }
        for (size_t i = 0; i < rgba_bytes / 4; i++) {
            rgba[i * 4] = src[i * 4 + 2]; rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4]; rgba[i * 4 + 3] = src[i * 4 + 3];
        }
        bzTTAsset_t *asset = image_asset(identity, metadata, header.width, header.height, rgba, status);
        free(rgba);
        return asset;
    }
    if (header.encoding == 2) {
        uint8_t format = header.alpha_encoding == 7 ? 5 : header.alpha_encoding == 1 ? 3 : 1;
        return decode_dxt(src, header.lengths[0], format, header.width, header.height,
                          identity, metadata, status);
    }
    *status = BZ_TTA_ERR_UNSUPPORTED;
    return NULL;
}

static bzTTAsset_t *decode_blp1(const uint8_t *data, size_t size, const char *identity,
                                const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status) {
    blp1Header_t header;
    if (size < sizeof(header)) { *status = BZ_TTA_ERR_MALFORMED; return NULL; }
    memcpy(&header, data, sizeof(header));
    if (!span_ok(size, header.offsets[0], header.lengths[0])) {
        *status = BZ_TTA_ERR_MALFORMED; return NULL;
    }
    if (header.type == 1) {
        const uint8_t (*palette)[4];
        if (size < sizeof(header) + 1024) { *status = BZ_TTA_ERR_MALFORMED; return NULL; }
        palette = (const uint8_t (*)[4])(data + sizeof(header));
        return decode_paletted(data + header.offsets[0], header.lengths[0], palette,
                               (uint8_t)header.alpha_bits, header.width, header.height,
                               identity, metadata, status);
    }
    if (header.type == 0) {
        uint32_t jpeg_header_size;
        size_t combined_size, rgba_bytes;
        uint8_t *combined, *decoded;
        int width, height, components, info_width, info_height, info_components;
        bzTTAsset_t *asset;
        if (!span_ok(size, sizeof(header), sizeof(jpeg_header_size))) {
            *status = BZ_TTA_ERR_MALFORMED; return NULL;
        }
        memcpy(&jpeg_header_size, data + sizeof(header), sizeof(jpeg_header_size));
        if (!image_bytes(header.width, header.height, &rgba_bytes) ||
            !span_ok(size, sizeof(header) + sizeof(jpeg_header_size), jpeg_header_size) ||
            jpeg_header_size > SIZE_MAX - header.lengths[0] ||
            jpeg_header_size + (size_t)header.lengths[0] > INT_MAX) {
            *status = BZ_TTA_ERR_MALFORMED; return NULL;
        }
        combined_size = jpeg_header_size + header.lengths[0];
        combined = malloc(combined_size);
        if (!combined) { *status = BZ_TTA_ERR_OUT_OF_MEMORY; return NULL; }
        memcpy(combined, data + sizeof(header) + sizeof(jpeg_header_size), jpeg_header_size);
        memcpy(combined + jpeg_header_size, data + header.offsets[0], header.lengths[0]);
        if (!stbi_info_from_memory(combined, (int)combined_size, &info_width, &info_height, &info_components) ||
            info_width != (int)header.width || info_height != (int)header.height) {
            free(combined); *status = BZ_TTA_ERR_MALFORMED; return NULL;
        }
        decoded = stbi_load_from_memory(combined, (int)combined_size, &width, &height, &components, STBI_rgb_alpha);
        free(combined);
        if (!decoded || width != info_width || height != info_height) {
            stbi_image_free(decoded); *status = BZ_TTA_ERR_MALFORMED; return NULL;
        }
        asset = image_asset(identity, metadata, (uint32_t)width, (uint32_t)height, decoded, status);
        stbi_image_free(decoded);
        return asset;
    }
    *status = BZ_TTA_ERR_UNSUPPORTED;
    return NULL;
}

bzTTAsset_t *BZ_WC3_TTA_DecodeBLP(const void *data, size_t size, const char *identity,
                                   const bzTTAssetMetadata_t *metadata, bzTTAResult_t *status) {
    uint32_t magic;
    if (!data || size < sizeof(magic) || !status) return NULL;
    memcpy(&magic, data, sizeof(magic));
    if (magic == BLP2_MAGIC) return decode_blp2(data, size, identity, metadata, status);
    if (magic == BLP1_MAGIC) return decode_blp1(data, size, identity, metadata, status);
    *status = BZ_TTA_ERR_UNSUPPORTED;
    return NULL;
}
