/*
 * bz_quest_scene.h - procedurally generates the layer-3 tabletop test scene:
 * a checkerboard "table" quad plus a few colored unit-proxy cubes at
 * different positions/heights. This exists to prove per-eye stereo
 * transforms, depth testing, clipping, and scale on real Quest hardware -
 * it is explicitly NOT Warcraft III asset rendering (see the task scope in
 * docs/quest-tabletop.md).
 *
 * Deliberately depends on nothing but plain floats/ints (no Vulkan/OpenXR
 * headers) so platform/android/quest/tests/test_bz_quest_scene_main.c can
 * build and check the generated geometry with a host compiler - mirrors
 * bz_quest_pure.h's rationale.
 */
#ifndef BZ_QUEST_SCENE_H
#define BZ_QUEST_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bzQuestVertex_s {
    float position[3];
    float color[3];
} bzQuestVertex_t;

/* Checkerboard table: BZ_QUEST_SCENE_TABLE_TILES x BZ_QUEST_SCENE_TABLE_TILES
 * quads (2 triangles each), plus BZ_QUEST_SCENE_CUBE_COUNT small cubes
 * (6 faces x 2 triangles each) sitting on top of it. */
#define BZ_QUEST_SCENE_TABLE_TILES 8u
#define BZ_QUEST_SCENE_CUBE_COUNT 4u
#define BZ_QUEST_SCENE_TABLE_VERTEX_COUNT (BZ_QUEST_SCENE_TABLE_TILES * BZ_QUEST_SCENE_TABLE_TILES * 6u)
#define BZ_QUEST_SCENE_CUBE_VERTEX_COUNT (6u * 6u)
#define BZ_QUEST_SCENE_VERTEX_COUNT \
    (BZ_QUEST_SCENE_TABLE_VERTEX_COUNT + BZ_QUEST_SCENE_CUBE_COUNT * BZ_QUEST_SCENE_CUBE_VERTEX_COUNT)

/*
 * Fills `outVertices` (which must have room for BZ_QUEST_SCENE_VERTEX_COUNT
 * entries) with the test scene's triangle list, in local/reference-space
 * meters: the table top is centered at (0, tableY, -tableDistance) spanning
 * `tableSize` meters square in the XZ plane, so a caller standing at the
 * XR_REFERENCE_SPACE_TYPE_LOCAL origin sees it roughly waist-height in front
 * of them (see docs/quest-tabletop.md for the exact placement rationale).
 * Always writes exactly BZ_QUEST_SCENE_VERTEX_COUNT vertices and returns
 * true; returns false (writing nothing) if tableSize is non-positive - a
 * zero/negative-size table is a caller bug, not something to clamp
 * silently.
 */
bool bz_quest_scene_generate(bzQuestVertex_t *outVertices, float tableSize, float tableY,
                             float tableDistance);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_SCENE_H */
