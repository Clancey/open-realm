/*
 * test_bz_quest_wc3_hud_font.c - coverage for layer 5E's pure bitmap-font
 * atlas packing and monospace glyph-quad text layout.
 */
#include <string.h>

#include "bz_quest_wc3_hud_font.h"
#include "test_framework.h"

static void test_glyph_bit_out_of_range_col_row_is_blank(void) {
    ASSERT(!bz_quest_wc3_hud_font_bit(0, 100, 0));
    ASSERT(!bz_quest_wc3_hud_font_bit(0, 0, 100));
}

static void test_glyph_bit_nul_glyph_is_fully_blank(void) {
    for (uint32_t row = 0; row < BZ_QUEST_HUD_FONT_GLYPH_HEIGHT; row++)
        for (uint32_t col = 0; col < BZ_QUEST_HUD_FONT_GLYPH_WIDTH; col++)
            ASSERT(!bz_quest_wc3_hud_font_bit(1, col, row)); /* glyph 1 (0x01) is blank in fixed_8x13.h */
}

/* 'F' (glyph 70): verifies the LSB-left bit convention documented in the
 * header - the vertical stroke must sit at column 1 on every row, and the
 * shorter middle bar must share that same left edge as the full top bar. */
static void test_glyph_bit_f_stroke_on_left_matches_lsb_left_convention(void) {
    uint32_t glyphF = 70;
    ASSERT(bz_quest_wc3_hud_font_bit(glyphF, 1, 2));  /* top bar (0x7e) spans columns 1..6 */
    ASSERT(bz_quest_wc3_hud_font_bit(glyphF, 6, 2));
    ASSERT(!bz_quest_wc3_hud_font_bit(glyphF, 0, 2)); /* left margin column stays blank */
    ASSERT(!bz_quest_wc3_hud_font_bit(glyphF, 7, 2));
    ASSERT(bz_quest_wc3_hud_font_bit(glyphF, 1, 3));  /* vertical stroke row (0x02) only column 1 */
    ASSERT(!bz_quest_wc3_hud_font_bit(glyphF, 2, 3));
    ASSERT(bz_quest_wc3_hud_font_bit(glyphF, 1, 6));  /* middle bar (0x1e) spans columns 1..4 */
    ASSERT(bz_quest_wc3_hud_font_bit(glyphF, 4, 6));
    ASSERT(!bz_quest_wc3_hud_font_bit(glyphF, 5, 6)); /* shorter than the top bar */
}

static void test_build_atlas_rejects_undersized_buffer(void) {
    uint8_t small[4] = {0};
    ASSERT(!bz_quest_wc3_hud_font_build_atlas(small, sizeof(small)));
}

static void test_build_atlas_places_f_glyph_at_expected_cell(void) {
    static uint8_t atlas[BZ_QUEST_HUD_FONT_ATLAS_BYTES];
    ASSERT(bz_quest_wc3_hud_font_build_atlas(atlas, sizeof(atlas)));
    uint32_t glyphF = 70;
    uint32_t cellX = (glyphF % BZ_QUEST_HUD_FONT_ATLAS_COLUMNS) * BZ_QUEST_HUD_FONT_GLYPH_WIDTH;
    uint32_t cellY = (glyphF / BZ_QUEST_HUD_FONT_ATLAS_COLUMNS) * BZ_QUEST_HUD_FONT_GLYPH_HEIGHT;
    /* Row 2 (0x7e top bar): column 1 lit, column 0 blank, matching the bit-level assertions above. */
    ASSERT_EQ_INT(atlas[(cellY + 2) * BZ_QUEST_HUD_FONT_ATLAS_WIDTH + (cellX + 1)], 255);
    ASSERT_EQ_INT(atlas[(cellY + 2) * BZ_QUEST_HUD_FONT_ATLAS_WIDTH + (cellX + 0)], 0);
}

static void test_glyph_uv_supported_char_reports_supported(void) {
    float u0 = -1, v0 = -1, u1 = -1, v1 = -1;
    ASSERT(bz_quest_wc3_hud_font_glyph_uv((unsigned char)'F', &u0, &v0, &u1, &v1));
    ASSERT(u0 >= 0.0f && u0 < u1 && u1 <= 1.0f);
    ASSERT(v0 >= 0.0f && v0 < v1 && v1 <= 1.0f);
}

static void test_glyph_uv_unsupported_byte_falls_back_to_question_mark(void) {
    float aU0 = 0, aV0 = 0, aU1 = 0, aV1 = 0;
    float qU0 = 0, qV0 = 0, qU1 = 0, qV1 = 0;
    ASSERT(!bz_quest_wc3_hud_font_glyph_uv((unsigned char)200, &aU0, &aV0, &aU1, &aV1));
    ASSERT(bz_quest_wc3_hud_font_glyph_uv((unsigned char)'?', &qU0, &qV0, &qU1, &qV1));
    ASSERT(aU0 == qU0 && aV0 == qV0 && aU1 == qU1 && aV1 == qV1);
}

static void test_layout_text_null_args_are_rejected(void) {
    bzQuestHudGlyphQuad_t quads[4];
    uint32_t count = 99;
    ASSERT(!bz_quest_wc3_hud_font_layout_text(NULL, 0, 0, 1, quads, 4, &count));
    ASSERT_EQ_INT(count, 0);
    count = 99;
    ASSERT(!bz_quest_wc3_hud_font_layout_text("A", 0, 0, 0.0f, quads, 4, &count));
    ASSERT_EQ_INT(count, 0);
}

static void test_layout_text_skips_spaces_but_still_advances(void) {
    bzQuestHudGlyphQuad_t quads[4];
    uint32_t count = 0;
    ASSERT(bz_quest_wc3_hud_font_layout_text("A B", 10.0f, 20.0f, 2.0f, quads, 4, &count));
    ASSERT_EQ_INT(count, 2); /* 'A' and 'B', no quad written for the space */
    float advance = (float)BZ_QUEST_HUD_FONT_GLYPH_WIDTH * 2.0f;
    ASSERT(quads[0].x == 10.0f);
    ASSERT(quads[1].x == 10.0f + advance * 2.0f); /* space still consumed one advance slot */
    ASSERT(quads[0].y == 20.0f && quads[1].y == 20.0f);
    ASSERT(quads[0].w == advance);
    ASSERT(quads[0].h == (float)BZ_QUEST_HUD_FONT_GLYPH_HEIGHT * 2.0f);
}

static void test_layout_text_truncates_at_cap_without_overflow(void) {
    bzQuestHudGlyphQuad_t quads[2];
    uint32_t count = 99;
    ASSERT(!bz_quest_wc3_hud_font_layout_text("ABCD", 0, 0, 1.0f, quads, 2, &count));
    ASSERT_EQ_INT(count, 2);
}

void run_bz_quest_wc3_hud_font_tests(void) {
    RUN_TEST(test_glyph_bit_out_of_range_col_row_is_blank);
    RUN_TEST(test_glyph_bit_nul_glyph_is_fully_blank);
    RUN_TEST(test_glyph_bit_f_stroke_on_left_matches_lsb_left_convention);
    RUN_TEST(test_build_atlas_rejects_undersized_buffer);
    RUN_TEST(test_build_atlas_places_f_glyph_at_expected_cell);
    RUN_TEST(test_glyph_uv_supported_char_reports_supported);
    RUN_TEST(test_glyph_uv_unsupported_byte_falls_back_to_question_mark);
    RUN_TEST(test_layout_text_null_args_are_rejected);
    RUN_TEST(test_layout_text_skips_spaces_but_still_advances);
    RUN_TEST(test_layout_text_truncates_at_cap_without_overflow);
}
