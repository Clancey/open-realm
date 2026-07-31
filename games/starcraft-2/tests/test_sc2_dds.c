/*
 * test_sc2_dds.c - DDS header/mip validator coverage (games/starcraft-2/common/sc2_dds.c).
 */

#include <string.h>

#include "games/starcraft-2/common/sc2_dds.h"
#include "test_framework.h"

/* DDS_PIXELFORMAT.dwFlags bits; mirrors the private constants in sc2_dds.c since the header
 * intentionally does not export raw DDS wire-format details to callers. */
#define TEST_DDPF_FOURCC    0x00000004u
#define TEST_DDPF_RGB       0x00000040u
#define TEST_DDPF_YUV       0x00000200u
#define TEST_DDPF_LUMINANCE 0x00020000u
#define TEST_DDSD_PITCH     0x00000008u
#define TEST_DDSCAPS2_CUBEMAP 0x00000200u

#define TEST_DDS_MAX_BUF (256 * 1024)

static void put_u32(BYTE *p, DWORD v) {
    p[0] = (BYTE)(v);
    p[1] = (BYTE)(v >> 8);
    p[2] = (BYTE)(v >> 16);
    p[3] = (BYTE)(v >> 24);
}

/* Fills a minimal-but-complete 128-byte DDS header (magic + DDS_HEADER + DDS_PIXELFORMAT) at
 * the front of `buf`. Callers append payload bytes starting at SC2_DDS_DATA_OFFSET. */
static void build_dds_header(BYTE *buf, DWORD width, DWORD height, DWORD depth, DWORD mipMapCount,
                              DWORD pfFlags, DWORD fourcc, DWORD rgbBitCount,
                              DWORD rMask, DWORD gMask, DWORD bMask, DWORD aMask) {
    memset(buf, 0, SC2_DDS_DATA_OFFSET);
    buf[0] = 'D'; buf[1] = 'D'; buf[2] = 'S'; buf[3] = ' ';
    put_u32(buf + 4, SC2_DDS_HEADER_SIZE);
    put_u32(buf + 12, height);
    put_u32(buf + 16, width);
    put_u32(buf + 24, depth);
    put_u32(buf + 28, mipMapCount);
    put_u32(buf + 76, SC2_DDS_PIXELFORMAT_SIZE);
    put_u32(buf + 80, pfFlags);
    put_u32(buf + 84, fourcc);
    put_u32(buf + 88, rgbBitCount);
    put_u32(buf + 92, rMask);
    put_u32(buf + 96, gMask);
    put_u32(buf + 100, bMask);
    put_u32(buf + 104, aMask);
}

static DWORD dxt_block_payload_size(DWORD width, DWORD height, DWORD mipMapCount, DWORD blockBytes) {
    DWORD total = 0, w = width, h = height, i;

    for (i = 0; i < mipMapCount; i++) {
        total += ((w + 3) / 4) * ((h + 3) / 4) * blockBytes;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

static void test_dds_valid_dxt1_single_level(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    DWORD payload = dxt_block_payload_size(128, 128, 1, 8);

    build_dds_header(buf, 128, 128, 0, 1, TEST_DDPF_FOURCC, MAKEFOURCC('D','X','T','1'), 0, 0, 0, 0, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0xAB, payload);
    ASSERT(SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + payload, &image));
    ASSERT_EQ_INT(image.format, SC2_DDS_FORMAT_DXT1);
    ASSERT_EQ_INT(image.blockBytes, 8);
    ASSERT_EQ_INT(image.width, 128);
    ASSERT_EQ_INT(image.height, 128);
    ASSERT_EQ_INT(image.mipLevelCount, 1);
    ASSERT_EQ_INT(image.mipLevels[0].size, payload);
}

static void test_dds_valid_dxt5_full_mip_chain(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    DWORD const mipMapCount = 7; /* 64 -> 32 -> 16 -> 8 -> 4 -> 2 -> 1 */
    DWORD payload = dxt_block_payload_size(64, 64, mipMapCount, 16);

    build_dds_header(buf, 64, 64, 0, mipMapCount, TEST_DDPF_FOURCC, MAKEFOURCC('D','X','T','5'), 0, 0, 0, 0, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0xCD, payload);
    ASSERT(SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + payload, &image));
    ASSERT_EQ_INT(image.format, SC2_DDS_FORMAT_DXT5);
    ASSERT_EQ_INT(image.mipLevelCount, mipMapCount);
    ASSERT_EQ_INT(image.mipLevels[0].width, 64);
    ASSERT_EQ_INT(image.mipLevels[mipMapCount - 1].width, 1);
    ASSERT_EQ_INT(image.mipLevels[mipMapCount - 1].height, 1);
}

static void test_dds_valid_dxt3(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    DWORD payload = dxt_block_payload_size(32, 32, 1, 16);

    build_dds_header(buf, 32, 32, 0, 1, TEST_DDPF_FOURCC, MAKEFOURCC('D','X','T','3'), 0, 0, 0, 0, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, payload);
    ASSERT(SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + payload, &image));
    ASSERT_EQ_INT(image.format, SC2_DDS_FORMAT_DXT3);
    ASSERT_EQ_INT(image.blockBytes, 16);
}

static void test_dds_valid_rgba_bgra_mask(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    DWORD payload = 4 * 4 * 4; /* 4x4 @ 32bpp */

    build_dds_header(buf, 4, 4, 0, 1, TEST_DDPF_RGB, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, payload);
    ASSERT(SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + payload, &image));
    ASSERT_EQ_INT(image.format, SC2_DDS_FORMAT_RGBA);
    ASSERT_EQ_INT(image.channels, SC2_DDS_CHANNELS_BGRA);
    ASSERT_EQ_INT(image.bitsPerPixel, 32);
    ASSERT_EQ_INT(image.mipLevelCount, 1);
}

static void test_dds_valid_rgb_24bpp(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    DWORD payload = 4 * 4 * 3;

    build_dds_header(buf, 4, 4, 0, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, payload);
    ASSERT(SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + payload, &image));
    ASSERT_EQ_INT(image.format, SC2_DDS_FORMAT_RGB);
    ASSERT_EQ_INT(image.channels, SC2_DDS_CHANNELS_RGB);
    ASSERT_EQ_INT(image.bitsPerPixel, 24);
}

static void test_dds_valid_odd_width_rgb_uses_tight_rows(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    DWORD payload = 1 * 3 * 3;

    build_dds_header(buf, 1, 3, 0, 1, TEST_DDPF_RGB, 0, 24,
                     0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, payload);
    ASSERT(SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + payload, &image));
    ASSERT_EQ_INT(image.mipLevels[0].rowBytes, 3);
    ASSERT_EQ_INT(image.mipLevels[0].size, 9);
}

static void test_dds_reject_non_tight_declared_pitch(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 1, 3, 0, 1, TEST_DDPF_RGB, 0, 24,
                     0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    put_u32(buf + 8, TEST_DDSD_PITCH);
    put_u32(buf + 20, 4);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, 12);
    ASSERT_EQ_INT(SC2_DdsParseResult(buf, SC2_DDS_DATA_OFFSET + 12, &image), SC2_DDS_ERR_UNSUPPORTED);
}

static void test_dds_valid_rgba_zero_mip_count_defaults_to_one(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    DWORD payload = 4 * 4 * 4;

    /* mipMapCount == 0 is spec-valid ("no mipmaps"); the renderer's original DXT/FourCC path
     * did not normalize this to 1 level (only the uncompressed path did), which would have set
     * GL_TEXTURE_MAX_LEVEL to (DWORD)-1 for a compressed texture with no declared mips. The
     * validator normalizes mipMapCount==0 to exactly 1 level for every format, uniformly. */
    build_dds_header(buf, 4, 4, 0, 0, TEST_DDPF_RGB, 0, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, payload);
    ASSERT(SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + payload, &image));
    ASSERT_EQ_INT(image.mipLevelCount, 1);
}

static void test_dds_reject_null_buffer_or_output(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 4, 4, 0, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    ASSERT(!SC2_DdsParse(NULL, SC2_DDS_DATA_OFFSET, &image));
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET, NULL));
}

static void test_dds_reject_truncated_header(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 4, 4, 0, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET - 1, &image));
}

static void test_dds_reject_bad_magic(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 4, 4, 0, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    buf[0] = 'X';
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 48, &image));
}

static void test_dds_reject_bad_header_sizes(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 4, 4, 0, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    put_u32(buf + 4, 123); /* wrong DDS_HEADER.dwSize */
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 48, &image));

    build_dds_header(buf, 4, 4, 0, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    put_u32(buf + 76, 31); /* wrong DDS_PIXELFORMAT.dwSize */
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 48, &image));
}

static void test_dds_reject_out_of_bounds_dimensions(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 0, 4, 0, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 48, &image));

    build_dds_header(buf, SC2_DDS_MAX_DIMENSION + 1, 4, 0, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 48, &image));
}

static void test_dds_reject_volume_texture(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 4, 4, 2, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 48, &image));
}

static void test_dds_reject_cubemap_texture(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 4, 4, 0, 1, TEST_DDPF_RGB, 0, 24, 0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    put_u32(buf + 112, TEST_DDSCAPS2_CUBEMAP);
    ASSERT_EQ_INT(SC2_DdsParseResult(buf, SC2_DDS_DATA_OFFSET + 48, &image), SC2_DDS_ERR_UNSUPPORTED);
}

static void test_dds_reject_bc4_bc5_ati2(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    DWORD const rejected_fourccs[] = {
        MAKEFOURCC('A','T','I','1'), MAKEFOURCC('A','T','I','2'),
        MAKEFOURCC('B','C','4','U'), MAKEFOURCC('B','C','4','S'),
        MAKEFOURCC('B','C','5','U'), MAKEFOURCC('B','C','5','S'),
    };
    DWORD i;

    for (i = 0; i < sizeof(rejected_fourccs) / sizeof(rejected_fourccs[0]); i++) {
        build_dds_header(buf, 8, 8, 0, 1, TEST_DDPF_FOURCC, rejected_fourccs[i], 0, 0, 0, 0, 0);
        memset(buf + SC2_DDS_DATA_OFFSET, 0, 128);
        ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 128, &image));
    }
}

static void test_dds_reject_dx10(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 8, 8, 0, 1, TEST_DDPF_FOURCC, MAKEFOURCC('D','X','1','0'), 0, 0, 0, 0, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, 128);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 128, &image));
}

static void test_dds_reject_yuv(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    /* Packed-YUV FourCC under DDPF_FOURCC. */
    build_dds_header(buf, 8, 8, 0, 1, TEST_DDPF_FOURCC, MAKEFOURCC('U','Y','V','Y'), 0, 0, 0, 0, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, 128);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 128, &image));

    /* Non-FourCC DDPF_YUV pixel format flag. */
    build_dds_header(buf, 8, 8, 0, 1, TEST_DDPF_YUV, 0, 16, 0, 0, 0, 0);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 128, &image));
}

static void test_dds_reject_unknown_fourcc(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 8, 8, 0, 1, TEST_DDPF_FOURCC, MAKEFOURCC('F','O','O','B'), 0, 0, 0, 0, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, 128);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 128, &image));
}

static void test_dds_reject_unknown_rgb_mask(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 8, 8, 0, 1, TEST_DDPF_RGB, 0, 16, 0xF800, 0x07E0, 0x001F, 0); /* RGB565 */
    memset(buf + SC2_DDS_DATA_OFFSET, 0, 128);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 128, &image));
}

static void test_dds_reject_unsupported_pixel_format_flags(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;

    build_dds_header(buf, 8, 8, 0, 1, 0 /* neither FOURCC nor RGB/LUMINANCE */, 0, 0, 0, 0, 0, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, 128);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 128, &image));

    build_dds_header(buf, 8, 8, 0, 1, TEST_DDPF_LUMINANCE, 0, 24,
                     0x000000FF, 0x0000FF00, 0x00FF0000, 0);
    ASSERT_EQ_INT(SC2_DdsParseResult(buf, SC2_DDS_DATA_OFFSET + 192, &image), SC2_DDS_ERR_UNSUPPORTED);

    build_dds_header(buf, 8, 8, 0, 1, TEST_DDPF_RGB, 0, 24,
                     0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    ASSERT_EQ_INT(SC2_DdsParseResult(buf, SC2_DDS_DATA_OFFSET + 192, &image), SC2_DDS_ERR_UNSUPPORTED);
}

static void test_dds_reject_mip_count_exceeds_dimensions(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    /* 4x4 can have at most 3 levels (4 -> 2 -> 1); claim far more than that. */
    DWORD payload = dxt_block_payload_size(4, 4, 3, 8) * 4;

    build_dds_header(buf, 4, 4, 0, 10, TEST_DDPF_FOURCC, MAKEFOURCC('D','X','T','1'), 0, 0, 0, 0, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, payload);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + payload, &image));
}

static void test_dds_reject_mip_count_exceeds_array_cap(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    /* SC2_DDS_MAX_DIMENSION (16384 = 2^14) needs at most 15 levels to reach 1x1, so this claims
     * one more than that theoretical maximum. The fixed SC2_DDS_MAX_MIP_LEVELS (16) array cap
     * is a secondary defensive bound that this dimension never independently exercises (16384
     * is the largest width sc2DdsImage_t.mipLevels[] can ever be asked to describe), but the
     * combined "out of bounds for these dimensions" rejection still fires here either way. */
    build_dds_header(buf, SC2_DDS_MAX_DIMENSION, SC2_DDS_MAX_DIMENSION, 0, SC2_DDS_MAX_MIP_LEVELS + 1,
                      TEST_DDPF_FOURCC, MAKEFOURCC('D','X','T','1'), 0, 0, 0, 0, 0);
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + 16, &image));
}

static void test_dds_reject_truncated_payload(void) {
    BYTE buf[TEST_DDS_MAX_BUF];
    sc2DdsImage_t image;
    DWORD payload = dxt_block_payload_size(128, 128, 1, 8);

    build_dds_header(buf, 128, 128, 0, 1, TEST_DDPF_FOURCC, MAKEFOURCC('D','X','T','1'), 0, 0, 0, 0, 0);
    memset(buf + SC2_DDS_DATA_OFFSET, 0, payload);
    /* File ends one byte before the declared mip payload finishes. */
    ASSERT(!SC2_DdsParse(buf, SC2_DDS_DATA_OFFSET + payload - 1, &image));
}

void run_sc2_dds_tests(void) {
    RUN_TEST(test_dds_valid_dxt1_single_level);
    RUN_TEST(test_dds_valid_dxt5_full_mip_chain);
    RUN_TEST(test_dds_valid_dxt3);
    RUN_TEST(test_dds_valid_rgba_bgra_mask);
    RUN_TEST(test_dds_valid_rgb_24bpp);
    RUN_TEST(test_dds_valid_odd_width_rgb_uses_tight_rows);
    RUN_TEST(test_dds_reject_non_tight_declared_pitch);
    RUN_TEST(test_dds_valid_rgba_zero_mip_count_defaults_to_one);
    RUN_TEST(test_dds_reject_null_buffer_or_output);
    RUN_TEST(test_dds_reject_truncated_header);
    RUN_TEST(test_dds_reject_bad_magic);
    RUN_TEST(test_dds_reject_bad_header_sizes);
    RUN_TEST(test_dds_reject_out_of_bounds_dimensions);
    RUN_TEST(test_dds_reject_volume_texture);
    RUN_TEST(test_dds_reject_cubemap_texture);
    RUN_TEST(test_dds_reject_bc4_bc5_ati2);
    RUN_TEST(test_dds_reject_dx10);
    RUN_TEST(test_dds_reject_yuv);
    RUN_TEST(test_dds_reject_unknown_fourcc);
    RUN_TEST(test_dds_reject_unknown_rgb_mask);
    RUN_TEST(test_dds_reject_unsupported_pixel_format_flags);
    RUN_TEST(test_dds_reject_mip_count_exceeds_dimensions);
    RUN_TEST(test_dds_reject_mip_count_exceeds_array_cap);
    RUN_TEST(test_dds_reject_truncated_payload);
}
