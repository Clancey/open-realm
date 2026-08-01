/*
 * test_bz_quest_wc3_fog.c - coverage for layer 5D's pure fog/selection math.
 */
#include <string.h>

#include "bz_quest_wc3_fog.h"
#include "test_framework.h"

static bzQuestWc3FogBounds_t make_bounds(float minX, float minY, float maxX, float maxY) {
    bzQuestWc3FogBounds_t bounds = {minX, minY, maxX, maxY};
    return bounds;
}

static void test_classify_visible_cell(void) {
    ASSERT_EQ_INT(bz_quest_wc3_fog_classify_cell(1, 0), BZ_QUEST_WC3_FOG_VISIBLE);
    ASSERT_EQ_INT(bz_quest_wc3_fog_pack_value(1, 0), 255);
}

static void test_classify_explored_not_visible_cell(void) {
    ASSERT_EQ_INT(bz_quest_wc3_fog_classify_cell(0, 1), BZ_QUEST_WC3_FOG_EXPLORED_NOT_VISIBLE);
    ASSERT_EQ_INT(bz_quest_wc3_fog_pack_value(0, 1), 128);
}

static void test_classify_unseen_cell(void) {
    ASSERT_EQ_INT(bz_quest_wc3_fog_classify_cell(0, 0), BZ_QUEST_WC3_FOG_UNSEEN);
    ASSERT_EQ_INT(bz_quest_wc3_fog_pack_value(0, 0), 0);
}

static void test_cell_index_row_major_rectangular_grid(void) {
    uint32_t index = 0;
    ASSERT(bz_quest_wc3_fog_cell_index(3, 5, 2, 4, &index));
    ASSERT_EQ_INT(index, 14);
}

static void test_cell_index_rejects_out_of_range(void) {
    uint32_t index = 0;
    ASSERT(!bz_quest_wc3_fog_cell_index(3, 5, 3, 0, &index));
    ASSERT(!bz_quest_wc3_fog_cell_index(3, 5, 0, 5, &index));
}

static void test_world_to_cell_round_trips_first_cell_center(void) {
    bzQuestWc3FogBounds_t bounds = make_bounds(10.0f, 20.0f, 202.0f, 340.0f);
    float wx = 0.0f, wy = 0.0f;
    uint32_t x = 99, y = 99;
    ASSERT(bz_quest_wc3_fog_cell_center(&bounds, 3, 5, 0, 0, &wx, &wy));
    ASSERT(bz_quest_wc3_fog_world_to_cell(&bounds, 3, 5, wx, wy, &x, &y));
    ASSERT_EQ_INT(x, 0);
    ASSERT_EQ_INT(y, 0);
}

static void test_world_to_cell_round_trips_last_cell_center(void) {
    bzQuestWc3FogBounds_t bounds = make_bounds(-32.0f, 96.0f, 160.0f, 416.0f);
    float wx = 0.0f, wy = 0.0f;
    uint32_t x = 0, y = 0;
    ASSERT(bz_quest_wc3_fog_cell_center(&bounds, 3, 5, 2, 4, &wx, &wy));
    ASSERT(bz_quest_wc3_fog_world_to_cell(&bounds, 3, 5, wx, wy, &x, &y));
    ASSERT_EQ_INT(x, 2);
    ASSERT_EQ_INT(y, 4);
}

static void test_world_to_cell_clamps_outside_bounds(void) {
    bzQuestWc3FogBounds_t bounds = make_bounds(0.0f, 0.0f, 128.0f, 320.0f);
    uint32_t x = 0, y = 0;
    ASSERT(bz_quest_wc3_fog_world_to_cell(&bounds, 2, 5, -999.0f, 9999.0f, &x, &y));
    ASSERT_EQ_INT(x, 0);
    ASSERT_EQ_INT(y, 4);
}

static void test_world_to_cell_handles_rectangular_non_chunk_multiple_grid(void) {
    bzQuestWc3FogBounds_t bounds = make_bounds(0.0f, 0.0f, 448.0f, 320.0f);
    uint32_t x = 0, y = 0;
    ASSERT(bz_quest_wc3_fog_world_to_cell(&bounds, 7, 5, 6.9f * BZ_QUEST_WC3_FOG_CELL_SIZE,
                                          4.1f * BZ_QUEST_WC3_FOG_CELL_SIZE, &x, &y));
    ASSERT_EQ_INT(x, 6);
    ASSERT_EQ_INT(y, 4);
}

static void test_pack_texture_without_padding(void) {
    const uint8_t visible[6] = {1, 0, 0, 0, 1, 0};
    const uint8_t explored[6] = {1, 1, 0, 1, 1, 0};
    uint8_t packed[6];
    ASSERT(bz_quest_wc3_fog_pack_texture(visible, explored, 3, 2, 3, packed, sizeof(packed)));
    ASSERT_EQ_INT(packed[0], 255);
    ASSERT_EQ_INT(packed[1], 128);
    ASSERT_EQ_INT(packed[2], 0);
    ASSERT_EQ_INT(packed[3], 128);
    ASSERT_EQ_INT(packed[4], 255);
    ASSERT_EQ_INT(packed[5], 0);
}

static void test_pack_texture_with_padding_zeroes_pad_bytes(void) {
    const uint8_t visible[6] = {1, 0, 0, 0, 1, 0};
    const uint8_t explored[6] = {1, 1, 0, 1, 1, 0};
    uint8_t packed[8];
    memset(packed, 0xCD, sizeof(packed));
    ASSERT(bz_quest_wc3_fog_pack_texture(visible, explored, 3, 2, 4, packed, sizeof(packed)));
    ASSERT_EQ_INT(packed[0], 255);
    ASSERT_EQ_INT(packed[1], 128);
    ASSERT_EQ_INT(packed[2], 0);
    ASSERT_EQ_INT(packed[3], 0);
    ASSERT_EQ_INT(packed[4], 128);
    ASSERT_EQ_INT(packed[5], 255);
    ASSERT_EQ_INT(packed[6], 0);
    ASSERT_EQ_INT(packed[7], 0);
}

static void test_pack_texture_rejects_short_row_stride(void) {
    const uint8_t visible[1] = {1}, explored[1] = {1};
    uint8_t packed[1] = {0};
    ASSERT(!bz_quest_wc3_fog_pack_texture(visible, explored, 2, 1, 1, packed, sizeof(packed)));
}

static void test_dirty_check_false_for_identical_buffers(void) {
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 3, 4};
    ASSERT(!bz_quest_wc3_fog_bytes_differ(a, sizeof(a), b, sizeof(b)));
}

static void test_dirty_check_true_for_different_byte(void) {
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[4] = {1, 2, 9, 4};
    ASSERT(bz_quest_wc3_fog_bytes_differ(a, sizeof(a), b, sizeof(b)));
}

static void test_dirty_check_true_for_different_lengths(void) {
    const uint8_t a[4] = {1, 2, 3, 4};
    const uint8_t b[3] = {1, 2, 3};
    ASSERT(bz_quest_wc3_fog_bytes_differ(a, sizeof(a), b, sizeof(b)));
}

static void test_selection_marker_from_entity_builds_swapped_translation_and_radius_scale(void) {
    bzQuestWc3SelectionMarker_t marker;
    ASSERT(bz_quest_wc3_selection_marker_from_entity(100.0f, 200.0f, 12.0f, 50.0f,
                                                     0.1f, 0.2f, 0.3f, 0.4f, &marker));
    ASSERT_EQ_FLOAT(marker.world[0], 50.0f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[5], 50.0f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[10], 50.0f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[12], 100.0f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[13], 12.0f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[14], 200.0f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.tint[0], 0.1f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.tint[3], 0.4f, 0.0001f);
}

static void test_selection_marker_from_translation_preserves_target_axes(void) {
    bzQuestWc3SelectionMarker_t marker;
    ASSERT(bz_quest_wc3_selection_marker_from_translation(-10.0f, 4.0f, 77.0f, 12.5f,
                                                          1.0f, 0.5f, 0.25f, 1.0f, &marker));
    ASSERT_EQ_FLOAT(marker.world[0], 12.5f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[12], -10.0f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[13], 4.0f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.world[14], 77.0f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.tint[1], 0.5f, 0.0001f);
    ASSERT_EQ_FLOAT(marker.tint[2], 0.25f, 0.0001f);
}

static void test_selection_marker_rejects_nonpositive_radius(void) {
    bzQuestWc3SelectionMarker_t marker;
    ASSERT(!bz_quest_wc3_selection_marker_from_entity(0.0f, 0.0f, 0.0f, 0.0f,
                                                      1.0f, 1.0f, 1.0f, 1.0f, &marker));
}

static void test_zero_dimensions_are_reported_safe_and_empty(void) {
    const uint8_t plane[1] = {0};
    uint8_t packed[4] = {0};
    uint32_t cellX = 0, cellY = 0;
    bzQuestWc3FogBounds_t bounds = make_bounds(0.0f, 0.0f, 64.0f, 64.0f);
    ASSERT(!bz_quest_wc3_fog_grid_supported(0, 4));
    ASSERT_EQ_INT(bz_quest_wc3_fog_cell_count(0, 4), 0);
    ASSERT(!bz_quest_wc3_fog_pack_texture(plane, plane, 0, 4, 1, packed, sizeof(packed)));
    ASSERT(!bz_quest_wc3_fog_world_to_cell(&bounds, 0, 4, 0.0f, 0.0f, &cellX, &cellY));
}

void run_bz_quest_wc3_fog_tests(void) {
    RUN_TEST(test_classify_visible_cell);
    RUN_TEST(test_classify_explored_not_visible_cell);
    RUN_TEST(test_classify_unseen_cell);
    RUN_TEST(test_cell_index_row_major_rectangular_grid);
    RUN_TEST(test_cell_index_rejects_out_of_range);
    RUN_TEST(test_world_to_cell_round_trips_first_cell_center);
    RUN_TEST(test_world_to_cell_round_trips_last_cell_center);
    RUN_TEST(test_world_to_cell_clamps_outside_bounds);
    RUN_TEST(test_world_to_cell_handles_rectangular_non_chunk_multiple_grid);
    RUN_TEST(test_pack_texture_without_padding);
    RUN_TEST(test_pack_texture_with_padding_zeroes_pad_bytes);
    RUN_TEST(test_pack_texture_rejects_short_row_stride);
    RUN_TEST(test_dirty_check_false_for_identical_buffers);
    RUN_TEST(test_dirty_check_true_for_different_byte);
    RUN_TEST(test_dirty_check_true_for_different_lengths);
    RUN_TEST(test_selection_marker_from_entity_builds_swapped_translation_and_radius_scale);
    RUN_TEST(test_selection_marker_from_translation_preserves_target_axes);
    RUN_TEST(test_selection_marker_rejects_nonpositive_radius);
    RUN_TEST(test_zero_dimensions_are_reported_safe_and_empty);
}
