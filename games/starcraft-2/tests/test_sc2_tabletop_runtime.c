#include <stdio.h>
#include <string.h>
#include <time.h>

#include "platform/apple/visionos/tabletop/bridge/bz_tabletop_lifecycle.h"
#include "platform/bridge/bz_tabletop_transport.h"
#include "games/starcraft-2/visionos/sc2_tabletop_assets.h"

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "test_sc2_tabletop_runtime: %s\n", message); failures++; } \
} while (0)

static void wait_tick(void) {
    struct timespec tick = { 0, 20L * 1000L * 1000L };
    nanosleep(&tick, NULL);
}

static const bzTTSnapshot_t *wait_for_snapshot(uint64_t after, int active, uint32_t min_entities) {
    for (int i = 0; i < 500; i++) {
        const bzTTSnapshot_t *snap = BZ_TT_Latest();
        if (snap && BZ_TTSnapshot_Generation(snap) > after &&
            (!active || BZ_TTSnapshot_ConnState(snap) == BZ_TT_CONN_ACTIVE) &&
            BZ_TTSnapshot_EntityCount(snap) >= min_entities)
            return snap;
        if (snap) BZ_TTSnapshot_Release(snap);
        wait_tick();
    }
    return NULL;
}

static uint32_t first_player_entity(const bzTTSnapshot_t *snap, bzTTEntity_t *out) {
    for (uint32_t i = 0; i < BZ_TTSnapshot_EntityCount(snap); i++)
        if (BZ_TTSnapshot_EntityAt(snap, i, out) && out->player == 1 && out->model)
            return out->number;
    return 0;
}

static const bzTTSnapshot_t *wait_for_selection(uint64_t after, uint32_t id) {
    for (int i = 0; i < 500; i++) {
        uint32_t selected[BZ_TT_MAX_SELECTED_ENTITIES];
        const bzTTSnapshot_t *snap = BZ_TT_Latest();
        if (snap && BZ_TTSnapshot_Generation(snap) > after &&
            BZ_TTSnapshot_SelectedEntityIds(snap, selected, BZ_TT_MAX_SELECTED_ENTITIES) == 1 &&
            selected[0] == id)
            return snap;
        if (snap) BZ_TTSnapshot_Release(snap);
        wait_tick();
    }
    return NULL;
}

/* Freeze the deterministic fixture and retail TRaynor01 terrain/DDS evidence at the public ABI. */
static const bzSC2Terrain_t *check_assets(int live) {
    const bzSC2Terrain_t *terrain = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
    bzSC2ATerrainInfo_t terrain_info;
    uint32_t textures = live ? 8 : 2;
    CHECK(terrain != NULL && !BZ_SC2ATerrain_IsPlaceholder(terrain), "terrain publication is unavailable");
    if (!terrain || BZ_SC2ATerrain_IsPlaceholder(terrain)) return terrain;
    CHECK(BZ_SC2ATerrain_Info(terrain, &terrain_info), "terrain metadata is unavailable");
    CHECK(terrain_info.cell_width == (live ? 136u : 8u) &&
          terrain_info.cell_height == (live ? 160u : 6u), "terrain cell dimensions changed");
    CHECK(terrain_info.hmap_width == (live ? 137u : 9u) &&
          terrain_info.hmap_height == (live ? 161u : 7u), "terrain HMAP dimensions changed");
    CHECK(terrain_info.mask_width == (live ? 1088u : 4u) &&
          terrain_info.mask_height == (live ? 1280u : 4u) &&
          terrain_info.mask_layer_count == (live ? 8u : 2u), "terrain MASK dimensions changed");
    CHECK(terrain_info.texture_count == textures, "terrain texture count changed");
    CHECK(terrain_info.cliff_set_count == (live ? 2u : 1u), "terrain cliff-set count changed");
    CHECK(terrain_info.cliff_cell_count == (live ? 1576u : 2u), "terrain cliff-cell count changed");
    CHECK(terrain_info.availability_flags == (live ? 0x3fu : 0x7fu), "terrain availability flags changed");
    CHECK(terrain_info.malformed_flags == 0, "terrain contains malformed required layers");
    CHECK(terrain_info.unsupported_flags == (live ? 0x3du : 0x30u), "terrain unsupported flags changed");
    for (uint32_t i = 0; i < textures; i++)
        for (uint32_t channel = BZ_SC2A_TERRAIN_CHANNEL_DIFFUSE;
             channel <= BZ_SC2A_TERRAIN_CHANNEL_NORMAL; channel++) {
            const bzSC2Image_t *first = BZ_SC2A_RegisterTerrainImage(
                BZ_SC2A_ABI_VERSION, terrain, i, (bzSC2ATerrainChannel_t)channel);
            const bzSC2Image_t *second = BZ_SC2A_RegisterTerrainImage(
                BZ_SC2A_ABI_VERSION, terrain, i, (bzSC2ATerrainChannel_t)channel);
            bzSC2AImageInfo_t image_info;
            CHECK(first != NULL && second != NULL && first == second, "terrain image cache did not reuse its handle");
            CHECK(first && !BZ_SC2AImage_IsPlaceholder(first), "terrain image resolved to a placeholder");
            if (first && !BZ_SC2AImage_IsPlaceholder(first)) {
                CHECK(BZ_SC2AImage_Info(first, &image_info), "terrain image metadata is unavailable");
                CHECK(image_info.format == (live ? BZ_SC2A_PIXEL_DXT5 : BZ_SC2A_PIXEL_DXT1),
                      "terrain image DDS format changed");
                CHECK(image_info.width == (live ? 1024u : 4u) && image_info.height == (live ? 1024u : 4u),
                      "terrain image dimensions changed");
                CHECK(image_info.mip_count == (live ? 11u : 1u), "terrain image mip count changed");
                CHECK(image_info.data_bytes == (live ? 1398128u : 8u), "terrain image payload size changed");
            }
            BZ_SC2AImage_Release(first); BZ_SC2AImage_Release(second);
        }
    CHECK(BZ_SC2A_CacheMisses() == textures * 2, "terrain image cache miss count changed");
    CHECK(BZ_SC2A_CacheHits() == textures * 2, "terrain image cache hit count changed");
    return terrain;
}

int main(int argc, char **argv) {
    const char *data = argc > 1 ? argv[1] : "build/tests";
    const char *map_name = argc > 2 ? argv[2] : "Tiny";
    int require_commands = argc <= 3 || strcmp(argv[3], "snapshot-only");
    const char *engine_argv[] = {
        "test_sc2_tabletop_runtime", "-data", data,
        "+map", map_name,
    };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(
        (int)(sizeof(engine_argv) / sizeof(engine_argv[0])), engine_argv);
    const bzSC2Terrain_t *retained_terrain = NULL;
    CHECK(lc != NULL, "lifecycle allocation failed");
    if (!lc) return 1;
    BZ_TabletopStart(lc);
    CHECK(BZ_TabletopGetState(lc) == BZ_TABLETOP_STATE_RUNNING, "engine did not reach running");
    if (BZ_TabletopGetState(lc) == BZ_TABLETOP_STATE_RUNNING) {
        const bzTTSnapshot_t *first = wait_for_snapshot(0, 1, 3);
        CHECK(first != NULL, "timed out waiting for active snapshot");
        if (first) {
            char map[256] = { 0 };
            bzTTBox2_t bounds;
            bzTTEntity_t entity;
            uint64_t generation = BZ_TTSnapshot_Generation(first);
            uint32_t id = require_commands ? first_player_entity(first, &entity) : 0;
            CHECK(BZ_TTSnapshot_AbiVersion(first) == BZ_TABLETOP_ABI_VERSION, "snapshot ABI mismatch");
            CHECK(BZ_TTSnapshot_MapName(first, map, sizeof(map)) && strstr(map, map_name),
                  "snapshot map identity mismatch");
            CHECK(BZ_TTSnapshot_MapBounds(first, &bounds) && bounds.max_x > bounds.min_x &&
                  bounds.max_y > bounds.min_y, "snapshot map bounds are invalid");
            CHECK(BZ_TTSnapshot_EntityCount(first) >= 3, "snapshot did not publish fixture entities");
            if (require_commands) CHECK(id != 0, "map has no player-owned entity");
            if (id) {
                CHECK(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, generation, &id, 1) == BZ_TT_OK,
                      "typed select command was rejected");
                const bzTTSnapshot_t *selected = wait_for_selection(generation, id);
                CHECK(selected != NULL, "timed out waiting for select acknowledgement");
                if (selected) {
                    uint32_t selected_ids[BZ_TT_MAX_SELECTED_ENTITIES];
                    uint32_t count = BZ_TTSnapshot_SelectedEntityIds(
                        selected, selected_ids, BZ_TT_MAX_SELECTED_ENTITIES);
                    uint64_t selected_generation = BZ_TTSnapshot_Generation(selected);
                    CHECK(count == 1 && selected_ids[0] == id, "server did not acknowledge selection");
                    CHECK(BZ_TT_PostSmartPoint(BZ_TABLETOP_ABI_VERSION, selected_generation,
                                              entity.origin_x + 8.0f, entity.origin_y) == BZ_TT_OK,
                          "typed point command was rejected");
                    CHECK(BZ_TT_PostCancel(BZ_TABLETOP_ABI_VERSION, generation) == BZ_TT_ERR_STALE_GENERATION,
                          "stale generation was not rejected");
                    BZ_TTSnapshot_Release(selected);
                }
            }
            retained_terrain = check_assets(!require_commands);
            BZ_TTSnapshot_Release(first);
        }
    }
    BZ_TabletopStop(lc);
    CHECK(BZ_TabletopGetState(lc) == BZ_TABLETOP_STATE_STOPPED, "engine did not stop");
    CHECK(BZ_TT_Latest() == NULL, "terminal transport exposed a snapshot");
    CHECK(BZ_TT_PostCancel(BZ_TABLETOP_ABI_VERSION, 0) == BZ_TT_ERR_TERMINAL,
          "terminal transport accepted a command");
    if (retained_terrain) {
        bzSC2ATerrainInfo_t retained_info;
        CHECK(BZ_SC2ATerrain_Info(retained_terrain, &retained_info), "retained terrain died during shutdown");
        BZ_SC2ATerrain_Release(retained_terrain);
    }
    {
        const bzSC2Terrain_t *terminal = BZ_SC2A_LatestTerrain(BZ_SC2A_ABI_VERSION);
        CHECK(terminal && BZ_SC2ATerrain_Status(terminal) == BZ_SC2A_ERR_TERMINAL,
             "terminal asset source accepted a terrain read");
        BZ_SC2ATerrain_Release(terminal);
    }
    BZ_TabletopDestroy(lc);
    if (failures) return 1;
    printf("test_sc2_tabletop_runtime: passed\n");
    return 0;
}
