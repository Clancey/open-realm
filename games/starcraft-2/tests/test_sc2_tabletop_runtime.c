#include <stdio.h>
#include <string.h>
#include <time.h>

#include "platform/apple/visionos/tabletop/bridge/bz_tabletop_lifecycle.h"
#include "platform/bridge/bz_tabletop_transport.h"

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
            BZ_TTSnapshot_Release(first);
        }
    }
    BZ_TabletopStop(lc);
    CHECK(BZ_TabletopGetState(lc) == BZ_TABLETOP_STATE_STOPPED, "engine did not stop");
    CHECK(BZ_TT_Latest() == NULL, "terminal transport exposed a snapshot");
    CHECK(BZ_TT_PostCancel(BZ_TABLETOP_ABI_VERSION, 0) == BZ_TT_ERR_TERMINAL,
          "terminal transport accepted a command");
    BZ_TabletopDestroy(lc);
    if (failures) return 1;
    printf("test_sc2_tabletop_runtime: passed\n");
    return 0;
}
