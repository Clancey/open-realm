/*
 * test_bz_quest_scene.c - coverage for bz_quest_scene.c's tabletop test-scene
 * generator. Verifies the geometry stays within the declared table/cube
 * bounds (normal path) and that invalid input is rejected rather than
 * silently clamped (inverse/error path), per AGENTS.md's test discipline.
 */
#include "bz_quest_scene.h"
#include "test_framework.h"

static void test_scene_generate_normal(void) {
    static bzQuestVertex_t verts[BZ_QUEST_SCENE_VERTEX_COUNT];
    const float tableSize = 1.6f;
    const float tableY = -0.5f;
    const float tableDistance = 1.0f;

    bool ok = bz_quest_scene_generate(verts, tableSize, tableY, tableDistance);
    ASSERT(ok);

    /* Every emitted triangle belongs to either the table (fits the XZ
     * footprint at exactly tableY) or a proxy cube sitting on top of it
     * (bounded above by one tile's worth of cube height, including the
     * cubes' own bottom faces which sit flush at tableY). */
    const float half = tableSize * 0.5f;
    const float tileSize = tableSize / (float)BZ_QUEST_SCENE_TABLE_TILES;
    const float cubeTop = tableY + tileSize * 0.35f * 2.0f + 0.001f;
    /* Distinct colors seen (linear scan, small N so O(n*k) is fine): the
     * checkerboard contributes 2 shades and each proxy cube contributes 1,
     * so a correctly generated scene has exactly 2 + BZ_QUEST_SCENE_CUBE_COUNT
     * distinct colors - this indirectly proves both the checker alternation
     * and all four cubes were actually emitted, without needing to bucket
     * vertices by height (cube bottom faces sit at the same Y as the table).
     */
    float distinctColors[16][3];
    int distinctColorCount = 0;
    for (uint32_t i = 0; i < BZ_QUEST_SCENE_VERTEX_COUNT; i++) {
        const float x = verts[i].position[0];
        const float y = verts[i].position[1];
        const float z = verts[i].position[2];
        ASSERT(x >= -half - 0.0001f && x <= half + 0.0001f);
        ASSERT(z <= -tableDistance + half + 0.0001f && z >= -tableDistance - half - 0.0001f);
        ASSERT(y >= tableY - 0.0001f && y <= cubeTop);
        /* Colors must be finite, normalized [0,1] values, not garbage. */
        for (int c = 0; c < 3; c++) {
            ASSERT(verts[i].color[c] >= 0.0f && verts[i].color[c] <= 1.0f);
        }
        bool seen = false;
        for (int d = 0; d < distinctColorCount; d++) {
            if (verts[i].color[0] == distinctColors[d][0] &&
                verts[i].color[1] == distinctColors[d][1] &&
                verts[i].color[2] == distinctColors[d][2]) {
                seen = true;
                break;
            }
        }
        if (!seen && distinctColorCount < 16) {
            distinctColors[distinctColorCount][0] = verts[i].color[0];
            distinctColors[distinctColorCount][1] = verts[i].color[1];
            distinctColors[distinctColorCount][2] = verts[i].color[2];
            distinctColorCount++;
        }
    }
    ASSERT_EQ_INT(distinctColorCount, 2 + (int)BZ_QUEST_SCENE_CUBE_COUNT);
}

static void test_scene_generate_rejects_invalid_size(void) {
    static bzQuestVertex_t verts[BZ_QUEST_SCENE_VERTEX_COUNT];
    /* Poison the buffer so we can prove a rejected call left it untouched
     * rather than silently clamping/zeroing the invalid size. */
    verts[0].position[0] = 12345.0f;

    ASSERT(!bz_quest_scene_generate(verts, 0.0f, -0.5f, 1.0f));
    ASSERT(!bz_quest_scene_generate(verts, -1.0f, -0.5f, 1.0f));
    ASSERT(!bz_quest_scene_generate(NULL, 1.6f, -0.5f, 1.0f));
    ASSERT_EQ_FLOAT(verts[0].position[0], 12345.0f, 0.0001f);
}

void run_bz_quest_scene_tests(void) {
    RUN_TEST(test_scene_generate_normal);
    RUN_TEST(test_scene_generate_rejects_invalid_size);
}
