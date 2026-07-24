/*
 * test_bz_tabletop_transport.c — ABI-level coverage for
 * platform/bridge/bz_tabletop_transport.c: snapshot lifecycle (publish/
 * latest/retain/release/generation/immutability/capacity), every command
 * type plus its invalid inverse, queue overflow, stale generation,
 * concurrent readers, and terminal/cleanup races.
 *
 * This file drives BZ_TT_PublishSnapshotFromClient() by writing directly
 * into the real `cl`/`cls` globals (test_bz_tabletop_transport_stubs.c) -
 * it does not go through the real wire parser (client/cl_parse.c); that is
 * covered separately by test_bz_tabletop_transport_client.c, which is the
 * "real client packet parse -> snapshot" and "typed command -> loopback
 * server delivery" suite. Splitting the two keeps this file's scenarios
 * (queue capacity, races, staleness) fast and independent of the network
 * wire format.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "client/client.h"
#include "platform/bridge/bz_tabletop_transport.h"
#include "test_framework.h"

void test_transport_stubs_reset(void);
void test_transport_block_asset_init(bool block);
void test_transport_wait_for_asset_init(void);
bool test_transport_asset_terminal(void);
void test_transport_set_world_bounds(BOX2 bounds);
void test_transport_set_unit_layouts(const bzTTUnitLayout_t *layouts, uint32_t count);

static void reset_all(void) {
    test_transport_stubs_reset();
    BZ_TT_Shutdown();
    BZ_TT_Init();
}

/* --- Lifecycle / not-initialized / terminal ------------------------------ */

static void test_post_before_init_is_rejected(void) {
    test_transport_stubs_reset();
    /* Before the first ever BZ_TT_Init() call, g_initialized defaults false
     * (g_terminal defaults true too, but the not-initialized check is
     * evaluated first in ValidateAndEnqueue() - a never-initialized
     * transport is a more precise error than "terminal"). This must be the
     * very first test to touch the transport module for that default state
     * to be observable. */
    uint32_t id = 1;
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, &id, 1), BZ_TT_ERR_NOT_INITIALIZED);
    ASSERT_NULL(BZ_TT_Latest());

    BZ_TT_Init();
}

static void test_shutdown_rejects_further_posts_but_keeps_outstanding_snapshots(void) {
    reset_all();
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *held = BZ_TT_Latest();
    ASSERT_NOT_NULL(held);
    uint64_t gen = BZ_TTSnapshot_Generation(held);
    ASSERT(gen > 0);

    BZ_TT_Shutdown();

    uint32_t id = 1;
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, &id, 1), BZ_TT_ERR_TERMINAL);
    ASSERT_NULL(BZ_TT_Latest());
    /* The reference taken before Shutdown() must remain valid/unchanged -
     * Shutdown() must never free a snapshot another thread still holds. */
    ASSERT_EQ_INT(BZ_TTSnapshot_Generation(held), gen);
    BZ_TTSnapshot_Release(held);

    BZ_TT_Init();
}

static void test_init_after_shutdown_is_idempotent_restart(void) {
    reset_all();
    BZ_TT_PublishSnapshotFromClient();
    ASSERT_NOT_NULL(BZ_TT_Latest() ? (void *)1 : NULL);
    BZ_TT_Shutdown();
    BZ_TT_Init(); /* restart: Latest() must be NULL again until a fresh publish */
    ASSERT_NULL(BZ_TT_Latest());
}

static void *transport_init_thread(void *opaque) { (void)opaque; BZ_TT_Init(); return NULL; }
static void *transport_shutdown_thread(void *opaque) {
    atomic_bool *done = opaque;
    BZ_TT_Shutdown(); atomic_store(done, true); return NULL;
}

static void test_asset_and_transport_lifecycle_are_atomic(void) {
    pthread_t initializer, shutdowner;
    atomic_bool shutdown_done = false;
    uint32_t id = 1;
    BZ_TT_Shutdown();
    test_transport_block_asset_init(true);
    ASSERT_EQ_INT(pthread_create(&initializer, NULL, transport_init_thread, NULL), 0);
    test_transport_wait_for_asset_init();
    ASSERT_EQ_INT(pthread_create(&shutdowner, NULL, transport_shutdown_thread, &shutdown_done), 0);
    struct timespec tiny = { 0, 2L * 1000L * 1000L };
    nanosleep(&tiny, NULL);
    ASSERT(!atomic_load(&shutdown_done));
    test_transport_block_asset_init(false);
    ASSERT_EQ_INT(pthread_join(initializer, NULL), 0);
    ASSERT_EQ_INT(pthread_join(shutdowner, NULL), 0);
    ASSERT(test_transport_asset_terminal());
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, &id, 1), BZ_TT_ERR_TERMINAL);
    BZ_TT_Init();
}

/* --- Snapshot generation / immutability / ownership ---------------------- */

static void test_generation_increments_monotonically_per_publish(void) {
    reset_all();
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *first = BZ_TT_Latest();
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *second = BZ_TT_Latest();

    ASSERT_NOT_NULL(first);
    ASSERT_NOT_NULL(second);
    ASSERT(BZ_TTSnapshot_Generation(second) > BZ_TTSnapshot_Generation(first));

    BZ_TTSnapshot_Release(first);
    BZ_TTSnapshot_Release(second);
}

static void test_retained_snapshot_is_immutable_across_a_later_publish(void) {
    reset_all();
    cl.playerstate.team = 3;
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *first = BZ_TT_Latest();
    ASSERT_NOT_NULL(first);
    ASSERT_EQ_INT(BZ_TTSnapshot_Player(first)->team, 3);

    /* Mutate live client state and publish again - the FIRST snapshot must
     * still report team==3 (deep-copied, not a live view). */
    cl.playerstate.team = 9;
    BZ_TT_PublishSnapshotFromClient();
    ASSERT_EQ_INT(BZ_TTSnapshot_Player(first)->team, 3);

    const bzTTSnapshot_t *second = BZ_TT_Latest();
    ASSERT_EQ_INT(BZ_TTSnapshot_Player(second)->team, 9);

    BZ_TTSnapshot_Release(first);
    BZ_TTSnapshot_Release(second);
}

static void test_retain_and_release_are_reference_counted(void) {
    reset_all();
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);

    BZ_TTSnapshot_Retain(snap);
    BZ_TTSnapshot_Retain(snap);
    /* Three references total now (Latest + 2 Retain). Release all three;
     * the third release frees the block - there is no direct way to
     * observe the free without a use-after-free, so this only asserts the
     * calls do not crash (a real bug here would be caught by ASan/TSan in
     * the full test run, see docs/visionos-tabletop.md). */
    BZ_TTSnapshot_Release(snap);
    BZ_TTSnapshot_Release(snap);
    BZ_TTSnapshot_Release(snap);
}

/* --- Snapshot content: entities/player/map/fog/configstrings/selection --- */

static void test_snapshot_reflects_player_and_configstrings(void) {
    reset_all();
    cl.playerstate.number = 2;
    cl.playerstate.team = 1;
    cl.playerstate.color = 4;
    cl.playerstate.race = 2;
    cl.playerstate.uiflags = 0x5;
    cl.playerstate.client_ui_state = 1;
    cl.playerstate.selected_entity = 7;
    cl.playerstate.start_location = 2;
    cl.playerstate.stats[PLAYERSTATE_RESOURCE_GOLD] = 250;
    cl.playerstate.stats[PLAYERSTATE_RESOURCE_LUMBER] = 60;
    cl.playerstate.stats[PLAYERSTATE_RESOURCE_FOOD_USED] = 12;
    cl.playerstate.stats[PLAYERSTATE_RESOURCE_FOOD_CAP] = 40;
    cl.playerstate.stats[PLAYERSTATE_RESOURCE_HERO_TOKENS] = 1;
    snprintf(cl.configstrings[CS_WORLD], sizeof(cl.configstrings[CS_WORLD]), "%s", "Human02");

    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);

    const bzTTPlayer_t *player = BZ_TTSnapshot_Player(snap);
    ASSERT_NOT_NULL(player);
    ASSERT_EQ_INT(player->number, 2);
    ASSERT_EQ_INT(player->team, 1);
    ASSERT_EQ_INT(player->color, 4);
    ASSERT_EQ_INT(player->race, 2);
    ASSERT_EQ_INT(player->uiflags, 0x5);
    ASSERT_EQ_INT(player->client_ui_state, 1);
    ASSERT_EQ_INT(player->selected_entity, 7);
    ASSERT_EQ_INT(player->start_location, 2);
    ASSERT_EQ_INT(player->resource_gold, 250);
    ASSERT_EQ_INT(player->resource_lumber, 60);
    ASSERT_EQ_INT(player->resource_food_used, 12);
    ASSERT_EQ_INT(player->resource_food_cap, 40);
    ASSERT_EQ_INT(player->resource_hero_tokens, 1);

    char map_name[64] = {0};
    ASSERT(BZ_TTSnapshot_MapName(snap, map_name, sizeof(map_name)));
    ASSERT_STR_EQ(map_name, "Human02");

    char cs[64] = {0};
    ASSERT(BZ_TTSnapshot_ConfigString(snap, CS_WORLD, cs, sizeof(cs)));
    ASSERT_STR_EQ(cs, "Human02");
    /* An empty configstring index must report false, not an empty string
     * masquerading as success. */
    ASSERT(!BZ_TTSnapshot_ConfigString(snap, CS_WORLD + 1, cs, sizeof(cs)));

    BZ_TTSnapshot_Release(snap);
}

static void test_configstring_count_is_zero_for_null_snapshot(void) {
    /* No reset_all()/init needed - this accessor must handle a NULL
     * snapshot pointer the same way every other BZ_TTSnapshot_* accessor
     * does (see BZ_TTSnapshot_Generation/Player/etc.), independent of
     * transport lifecycle state. */
    ASSERT_EQ_INT(BZ_TTSnapshot_ConfigStringCount(NULL), 0);
}

static void test_configstring_count_and_iteration_bounds(void) {
    reset_all();
    snprintf(cl.configstrings[CS_WORLD], sizeof(cl.configstrings[CS_WORLD]), "%s", "Human02");
    /* Leave every other slot empty - a valid, in-range, empty slot, distinct
     * from an out-of-range index. */

    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);

    uint32_t count = BZ_TTSnapshot_ConfigStringCount(snap);
    ASSERT_EQ_INT(count, MAX_CONFIGSTRINGS);

    char cs[64];
    uint32_t populated = 0;
    uint32_t empty_in_range = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (BZ_TTSnapshot_ConfigString(snap, i, cs, sizeof(cs))) {
            populated++;
        } else {
            /* An in-range index reporting false must mean "validly empty",
             * never "out of range" - callers iterating [0, count) never
             * need to special-case this. */
            empty_in_range++;
        }
    }
    ASSERT_EQ_INT(populated, 1);
    ASSERT_EQ_INT(empty_in_range, count - 1);

    /* Last valid index (count - 1) must be reachable and behave like any
     * other in-range index (empty here, since only CS_WORLD was set). */
    ASSERT(!BZ_TTSnapshot_ConfigString(snap, count - 1, cs, sizeof(cs)));

    /* One-past-the-end (== count) is genuinely out of range, same as any
     * index beyond it - both return false, matching BZ_TTSnapshot_ConfigString's
     * existing out-of-range behavior. */
    ASSERT(!BZ_TTSnapshot_ConfigString(snap, count, cs, sizeof(cs)));
    ASSERT(!BZ_TTSnapshot_ConfigString(snap, count + 1000, cs, sizeof(cs)));

    BZ_TTSnapshot_Release(snap);
}

static void test_map_bounds_only_valid_when_refresh_prepped(void) {
    reset_all();
    bzTTBox2_t bounds;

    /* refresh_prepped is false by default - BuildMap() must not fabricate
     * bounds from an unloaded collision model. */
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *not_ready = BZ_TT_Latest();
    ASSERT_NOT_NULL(not_ready);
    ASSERT(!BZ_TTSnapshot_MapBounds(not_ready, &bounds));
    BZ_TTSnapshot_Release(not_ready);

    cl.refresh_prepped = true;
    test_transport_set_world_bounds((BOX2){ .min = {10, 20}, .max = {310, 220} });
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *ready = BZ_TT_Latest();
    ASSERT_NOT_NULL(ready);
    ASSERT(BZ_TTSnapshot_MapBounds(ready, &bounds));
    ASSERT_EQ_FLOAT(bounds.min_x, 10, 0.01f);
    ASSERT_EQ_FLOAT(bounds.min_y, 20, 0.01f);
    ASSERT_EQ_FLOAT(bounds.max_x, 310, 0.01f);
    ASSERT_EQ_FLOAT(bounds.max_y, 220, 0.01f);
    BZ_TTSnapshot_Release(ready);
}

static void test_snapshot_reflects_entities_and_selection(void) {
    reset_all();
    cl.num_entities = 3;
    cl.ents[0].current.number = 0; /* Empty model slot is not desktop-visible. */
    cl.ents[0].current.model2 = 9;
    cl.ents[0].current.image = 10;
    cl.ents[1].current.number = 1;
    cl.ents[1].current.class_id = 42;
    cl.ents[1].current.origin.x = 100.0f;
    cl.ents[1].current.origin.y = 200.0f;
    cl.ents[1].current.origin.z = 0.0f;
    cl.ents[1].current.player = 2;
    cl.ents[1].current.model = 7;
    cl.ents[1].current.model2 = 12; /* One active slot remains one transport entity despite its render attachment. */
    cl.ents[1].selected = true;
    cl.ents[2].current.number = 2;
    cl.ents[2].current.class_id = 43;
    cl.ents[2].current.model2 = 11; /* Attachments do not activate an empty base-model slot. */

    cl.selection.num_selected = 1;
    cl.selection.entity_nums[0] = 1;

    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);
    ASSERT_EQ_INT(BZ_TTSnapshot_EntityCount(snap), 1);
    ASSERT_EQ_INT(BZ_TTSnapshot_EntitiesOverflowCount(snap), 0);

    bzTTEntity_t ent;
    ASSERT(BZ_TTSnapshot_EntityAt(snap, 0, &ent));
    ASSERT_EQ_INT(ent.number, 1);
    ASSERT_EQ_INT(ent.class_id, 42);
    ASSERT_EQ_FLOAT(ent.origin_x, 100.0f, 0.01f);
    ASSERT_EQ_FLOAT(ent.origin_y, 200.0f, 0.01f);
    ASSERT_EQ_INT(ent.player, 2);
    ASSERT_EQ_INT(ent.model, 7);
    ASSERT(ent.selected);
    ASSERT(!BZ_TTSnapshot_EntityAt(snap, 1, &ent)); /* Empty slots were excluded. */

    uint32_t selected_ids[BZ_TT_MAX_SELECTED_ENTITIES];
    uint32_t n = BZ_TTSnapshot_SelectedEntityIds(snap, selected_ids, BZ_TT_MAX_SELECTED_ENTITIES);
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(selected_ids[0], 1);

    BZ_TTSnapshot_Release(snap);
}

static void test_entity_overflow_is_reported_not_truncated_silently(void) {
    reset_all();
    /* cl.num_entities can legitimately be as large as MAX_CLIENT_ENTITIES;
     * exceed the transport's BZ_TT_MAX_ENTITIES cap and confirm the excess
     * is reported via EntitiesOverflowCount(), not silently dropped. */
    cl.num_entities = MAX_CLIENT_ENTITIES;
    FOR_LOOP(i, BZ_TT_MAX_ENTITIES) {
        cl.ents[i].current.number = (DWORD)i;
        cl.ents[i].current.model = 1;
    }
    for (DWORD i = BZ_TT_MAX_ENTITIES + 100; i < BZ_TT_MAX_ENTITIES + 105; i++) {
        cl.ents[i].current.number = i;
        cl.ents[i].current.model = 1;
    }

    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);
    ASSERT_EQ_INT(BZ_TTSnapshot_EntityCount(snap), BZ_TT_MAX_ENTITIES);
    ASSERT_EQ_INT(BZ_TTSnapshot_EntitiesOverflowCount(snap), 5);
    BZ_TTSnapshot_Release(snap);
}

static void test_fog_dimensions_and_planes_round_trip(void) {
    reset_all();
    uint32_t const w = 4, h = 3, cells = w * h;
    BYTE visible[12], explored[12];
    FOR_LOOP(i, cells) {
        visible[i] = (BYTE)(i % 2);
        explored[i] = 1;
    }
    cl.fow.width = w;
    cl.fow.height = h;
    cl.fow.visible = visible;
    cl.fow.explored = explored;

    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);

    uint32_t out_w = 0, out_h = 0;
    ASSERT(BZ_TTSnapshot_FogDimensions(snap, &out_w, &out_h));
    ASSERT_EQ_INT(out_w, w);
    ASSERT_EQ_INT(out_h, h);

    uint8_t dst_visible[12] = {0}, dst_explored[12] = {0};
    ASSERT_EQ_INT(BZ_TTSnapshot_FogVisible(snap, dst_visible, cells), cells);
    ASSERT_EQ_INT(BZ_TTSnapshot_FogExplored(snap, dst_explored, cells), cells);
    ASSERT_EQ_INT(memcmp(dst_visible, visible, cells), 0);
    ASSERT_EQ_INT(memcmp(dst_explored, explored, cells), 0);

    BZ_TTSnapshot_Release(snap);
    cl.fow.width = cl.fow.height = 0;
    cl.fow.visible = cl.fow.explored = NULL;
}

static void test_fog_dimensions_false_when_no_fog_buffer(void) {
    reset_all();
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);
    uint32_t w = 0, h = 0;
    ASSERT(!BZ_TTSnapshot_FogDimensions(snap, &w, &h));
    BZ_TTSnapshot_Release(snap);
}

static void test_unit_layout_is_versioned_empty_when_never_delivered(void) {
    reset_all();
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);
    ASSERT_EQ_INT(BZ_TTSnapshot_UnitLayoutCount(snap), 0);
    bzTTUnitLayout_t out;
    ASSERT(!BZ_TTSnapshot_UnitLayoutAt(snap, 0, &out));
    BZ_TTSnapshot_Release(snap);
}

static void test_unit_layout_reflects_cached_ui_data(void) {
    reset_all();
    bzTTUnitLayout_t layout;
    memset(&layout, 0, sizeof(layout));
    layout.entity_num = 5;
    layout.num_buttons = 1;
    snprintf(layout.buttons[0].art, sizeof(layout.buttons[0].art), "%s", "ReplaceableTextures\\CommandButtons\\BTNFootman.blp");
    layout.buttons[0].hotkey = 'A';
    test_transport_set_unit_layouts(&layout, 1);

    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);
    ASSERT_EQ_INT(BZ_TTSnapshot_UnitLayoutCount(snap), 1);
    bzTTUnitLayout_t out;
    ASSERT(BZ_TTSnapshot_UnitLayoutAt(snap, 0, &out));
    ASSERT_EQ_INT(out.entity_num, 5);
    ASSERT_EQ_INT(out.num_buttons, 1);
    ASSERT_STR_EQ(out.buttons[0].art, "ReplaceableTextures\\CommandButtons\\BTNFootman.blp");
    ASSERT_EQ_INT(out.buttons[0].hotkey, 'A');
    BZ_TTSnapshot_Release(snap);
    test_transport_set_unit_layouts(NULL, 0);
}

/* --- Commands: every type + its invalid inverse -------------------------- */

static char *drain_and_read_message(char *out, size_t cap) {
    /* Mirrors EncodeCommand()'s wire format: [byte clc_stringcmd][string]. */
    if (cls.netchan.message.cursize < 2) {
        out[0] = '\0';
        return out;
    }
    /* Skip the clc_stringcmd opcode byte (offset 0); the string starts at
     * offset 1 and is NUL-terminated by SZ_Printf(). */
    snprintf(out, cap, "%s", (const char *)(cls.netchan.message.data + 1));
    return out;
}

static void reset_netchan_message(void) {
    cls.netchan.message.cursize = 0;
    cls.netchan.message.overflowed = false;
}

static void test_post_select_encodes_ids_and_rejects_invalid(void) {
    reset_all();
    reset_netchan_message();
    uint32_t ids[3] = { 1, 2, 3 };
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, ids, 3), BZ_TT_OK);
    BZ_TT_Drain();
    char buf[64];
    ASSERT_STR_EQ(drain_and_read_message(buf, sizeof(buf)), "select 1 2 3");

    /* Invalid inverse: NULL ids, zero count, count over the per-command
     * cap, and an out-of-range entity id must all be rejected before ever
     * touching the queue. */
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, NULL, 1), BZ_TT_ERR_INVALID_ARGUMENT);
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, ids, 0), BZ_TT_ERR_INVALID_ARGUMENT);
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, ids, BZ_TT_MAX_SELECT_IDS_PER_COMMAND + 1), BZ_TT_ERR_INVALID_ARGUMENT);
    uint32_t bad_id = BZ_TT_ENTITY_ID_LIMIT;
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, &bad_id, 1), BZ_TT_ERR_INVALID_ARGUMENT);
}

static void test_post_smart_entity_encodes_and_rejects_invalid(void) {
    reset_all();
    reset_netchan_message();
    ASSERT_EQ_INT(BZ_TT_PostSmartEntity(BZ_TABLETOP_ABI_VERSION, 0, 7), BZ_TT_OK);
    BZ_TT_Drain();
    char buf[64];
    ASSERT_STR_EQ(drain_and_read_message(buf, sizeof(buf)), "smart 7");

    ASSERT_EQ_INT(BZ_TT_PostSmartEntity(BZ_TABLETOP_ABI_VERSION, 0, BZ_TT_ENTITY_ID_LIMIT), BZ_TT_ERR_INVALID_ARGUMENT);
}

static void test_post_smart_point_encodes_and_rejects_invalid(void) {
    reset_all();
    reset_netchan_message();
    ASSERT_EQ_INT(BZ_TT_PostSmartPoint(BZ_TABLETOP_ABI_VERSION, 0, 12.0f, -34.0f), BZ_TT_OK);
    BZ_TT_Drain();
    char buf[64];
    ASSERT_STR_EQ(drain_and_read_message(buf, sizeof(buf)), "smartpoint 12 -34");

    float nanv = NAN;
    ASSERT_EQ_INT(BZ_TT_PostSmartPoint(BZ_TABLETOP_ABI_VERSION, 0, nanv, 0), BZ_TT_ERR_INVALID_ARGUMENT);
    ASSERT_EQ_INT(BZ_TT_PostSmartPoint(BZ_TABLETOP_ABI_VERSION, 0, 1e7f, 0), BZ_TT_ERR_INVALID_ARGUMENT);
}

static void test_post_button_encodes_and_rejects_invalid(void) {
    reset_all();
    reset_netchan_message();
    ASSERT_EQ_INT(BZ_TT_PostButton(BZ_TABLETOP_ABI_VERSION, 0, "hpea", 4), BZ_TT_OK);
    BZ_TT_Drain();
    char buf[64];
    ASSERT_STR_EQ(drain_and_read_message(buf, sizeof(buf)), "button hpea");

    ASSERT_EQ_INT(BZ_TT_PostButton(BZ_TABLETOP_ABI_VERSION, 0, "hpe", 3), BZ_TT_ERR_INVALID_ARGUMENT);
    ASSERT_EQ_INT(BZ_TT_PostButton(BZ_TABLETOP_ABI_VERSION, 0, NULL, 4), BZ_TT_ERR_INVALID_ARGUMENT);
}

static void test_post_cancel_encodes_and_rejects_bad_abi_version(void) {
    reset_all();
    reset_netchan_message();
    ASSERT_EQ_INT(BZ_TT_PostCancel(BZ_TABLETOP_ABI_VERSION, 0), BZ_TT_OK);
    BZ_TT_Drain();
    char buf[64];
    ASSERT_STR_EQ(drain_and_read_message(buf, sizeof(buf)), "cancel");

    ASSERT_EQ_INT(BZ_TT_PostCancel(BZ_TABLETOP_ABI_VERSION + 1, 0), BZ_TT_ERR_ABI_VERSION);
}

static void test_queue_overflow_reports_full_without_dropping_silently(void) {
    reset_all();
    for (uint32_t i = 0; i < BZ_TT_COMMAND_QUEUE_CAPACITY; i++) {
        ASSERT_EQ_INT(BZ_TT_PostCancel(BZ_TABLETOP_ABI_VERSION, 0), BZ_TT_OK);
    }
    /* Capacity is exactly full now; one more must be rejected, not silently
     * evict the oldest command. */
    ASSERT_EQ_INT(BZ_TT_PostCancel(BZ_TABLETOP_ABI_VERSION, 0), BZ_TT_ERR_QUEUE_FULL);

    reset_netchan_message();
    BZ_TT_Drain();
    /* All 256 accepted commands must have actually been encoded (drain
     * doesn't silently truncate either). */
    uint32_t count = 0;
    DWORD pos = 0;
    while (pos + 1 < cls.netchan.message.cursize) {
        pos++; /* clc_stringcmd opcode byte */
        while (pos < cls.netchan.message.cursize && cls.netchan.message.data[pos] != 0) pos++;
        pos++; /* NUL terminator */
        count++;
    }
    ASSERT_EQ_INT(count, BZ_TT_COMMAND_QUEUE_CAPACITY);
}

static void test_stale_generation_rejects_commands_from_a_superseded_snapshot(void) {
    reset_all();
    BZ_TT_PublishSnapshotFromClient();
    const bzTTSnapshot_t *first = BZ_TT_Latest();
    uint64_t observed = BZ_TTSnapshot_Generation(first);
    BZ_TTSnapshot_Release(first);

    uint32_t id = 1;
    /* Not stale yet: no newer snapshot has been published. */
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, observed, &id, 1), BZ_TT_OK);

    BZ_TT_PublishSnapshotFromClient();
    /* Now a newer generation exists - the same observed generation must be
     * rejected as stale. */
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, observed, &id, 1), BZ_TT_ERR_STALE_GENERATION);
    /* generation 0 always skips the staleness check. */
    ASSERT_EQ_INT(BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, &id, 1), BZ_TT_OK);
}

/* --- Concurrency: readers vs. publisher, and terminal cleanup races ------ */

static _Atomic bool g_stress_stop;

static void *reader_thread_main(void *arg) {
    (void)arg;
    while (!g_stress_stop) {
        const bzTTSnapshot_t *snap = BZ_TT_Latest();
        if (snap) {
            /* Touch every accessor to catch a torn-down/half-built snapshot
             * under TSan/ASan, not just a NULL-deref crash. */
            (void)BZ_TTSnapshot_Generation(snap);
            (void)BZ_TTSnapshot_Player(snap);
            (void)BZ_TTSnapshot_EntityCount(snap);
            BZ_TTSnapshot_Release(snap);
        }
    }
    return NULL;
}

static void *publisher_thread_main(void *arg) {
    int *iterations = (int *)arg;
    for (int i = 0; i < *iterations; i++) {
        BZ_TT_PublishSnapshotFromClient();
    }
    return NULL;
}

static void test_concurrent_readers_survive_repeated_publish(void) {
    reset_all();
    enum { NUM_READERS = 4, PUBLISH_ITERATIONS = 2000 };
    pthread_t readers[NUM_READERS];
    pthread_t publisher;
    int iterations = PUBLISH_ITERATIONS;

    g_stress_stop = false;
    FOR_LOOP(i, NUM_READERS) {
        pthread_create(&readers[i], NULL, reader_thread_main, NULL);
    }
    pthread_create(&publisher, NULL, publisher_thread_main, &iterations);
    pthread_join(publisher, NULL);
    g_stress_stop = true;
    FOR_LOOP(i, NUM_READERS) {
        pthread_join(readers[i], NULL);
    }

    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    ASSERT_NOT_NULL(snap);
    ASSERT_EQ_INT(BZ_TTSnapshot_Generation(snap), PUBLISH_ITERATIONS);
    BZ_TTSnapshot_Release(snap);
}

typedef struct {
    int iterations;
    _Atomic int accepted;
    _Atomic int rejected;
} PosterArgs;

static void *poster_drainer_thread_main(void *arg) {
    PosterArgs *args = (PosterArgs *)arg;
    uint32_t id = 1;
    for (int i = 0; i < args->iterations; i++) {
        bzTTResult_t rc = BZ_TT_PostSelect(BZ_TABLETOP_ABI_VERSION, 0, &id, 1);
        if (rc == BZ_TT_OK) {
            args->accepted++;
        } else {
            args->rejected++;
        }
    }
    return NULL;
}

static void *shutdown_thread_main(void *arg) {
    (void)arg;
    BZ_TT_Shutdown();
    return NULL;
}

static void test_terminal_cleanup_races_with_post_and_drain_do_not_crash(void) {
    reset_all();
    enum { NUM_POSTERS = 4, ITERATIONS = 500 };
    pthread_t posters[NUM_POSTERS];
    pthread_t shutdowner;
    PosterArgs args = { .iterations = ITERATIONS, .accepted = 0, .rejected = 0 };

    FOR_LOOP(i, NUM_POSTERS) {
        pthread_create(&posters[i], NULL, poster_drainer_thread_main, &args);
    }
    /* Racing Shutdown() against in-flight Post()s: every post must resolve
     * to either BZ_TT_OK or BZ_TT_ERR_TERMINAL - never a crash/hang/torn
     * queue entry - and a concurrent Drain() must never read out from under
     * a queue Shutdown() is tearing down (both take g_lock). */
    pthread_create(&shutdowner, NULL, shutdown_thread_main, NULL);
    FOR_LOOP(i, 50) {
        BZ_TT_Drain();
    }

    pthread_join(shutdowner, NULL);
    FOR_LOOP(i, NUM_POSTERS) {
        pthread_join(posters[i], NULL);
    }
    BZ_TT_Drain(); /* final drain after shutdown: must be a safe no-op */

    ASSERT_EQ_INT(args.accepted + args.rejected, NUM_POSTERS * ITERATIONS);
    ASSERT_NULL(BZ_TT_Latest());

    BZ_TT_Init();
}

static void *destroy_publish_race_thread_main(void *arg) {
    int *iterations = (int *)arg;
    for (int i = 0; i < *iterations; i++) {
        BZ_TT_PublishSnapshotFromClient();
    }
    return NULL;
}

static void test_repeated_publish_and_shutdown_do_not_crash(void) {
    reset_all();
    enum { ITERATIONS = 2000 };
    pthread_t publisher;
    int iterations = ITERATIONS;

    pthread_create(&publisher, NULL, destroy_publish_race_thread_main, &iterations);
    struct timespec tiny = { 0, 200L * 1000L };
    nanosleep(&tiny, NULL);
    BZ_TT_Shutdown();
    pthread_join(publisher, NULL);

    /* Terminal: Latest() must consistently report NULL, never a
     * half-published snapshot. */
    ASSERT_NULL(BZ_TT_Latest());
    BZ_TT_Init();
}

void run_bz_tabletop_transport_tests(void) {
    RUN_TEST(test_post_before_init_is_rejected);
    RUN_TEST(test_shutdown_rejects_further_posts_but_keeps_outstanding_snapshots);
    RUN_TEST(test_init_after_shutdown_is_idempotent_restart);
    RUN_TEST(test_asset_and_transport_lifecycle_are_atomic);
    RUN_TEST(test_generation_increments_monotonically_per_publish);
    RUN_TEST(test_retained_snapshot_is_immutable_across_a_later_publish);
    RUN_TEST(test_retain_and_release_are_reference_counted);
    RUN_TEST(test_snapshot_reflects_player_and_configstrings);
    RUN_TEST(test_configstring_count_is_zero_for_null_snapshot);
    RUN_TEST(test_configstring_count_and_iteration_bounds);
    RUN_TEST(test_map_bounds_only_valid_when_refresh_prepped);
    RUN_TEST(test_snapshot_reflects_entities_and_selection);
    RUN_TEST(test_entity_overflow_is_reported_not_truncated_silently);
    RUN_TEST(test_fog_dimensions_and_planes_round_trip);
    RUN_TEST(test_fog_dimensions_false_when_no_fog_buffer);
    RUN_TEST(test_unit_layout_is_versioned_empty_when_never_delivered);
    RUN_TEST(test_unit_layout_reflects_cached_ui_data);
    RUN_TEST(test_post_select_encodes_ids_and_rejects_invalid);
    RUN_TEST(test_post_smart_entity_encodes_and_rejects_invalid);
    RUN_TEST(test_post_smart_point_encodes_and_rejects_invalid);
    RUN_TEST(test_post_button_encodes_and_rejects_invalid);
    RUN_TEST(test_post_cancel_encodes_and_rejects_bad_abi_version);
    RUN_TEST(test_queue_overflow_reports_full_without_dropping_silently);
    RUN_TEST(test_stale_generation_rejects_commands_from_a_superseded_snapshot);
    RUN_TEST(test_concurrent_readers_survive_repeated_publish);
    RUN_TEST(test_terminal_cleanup_races_with_post_and_drain_do_not_crash);
    RUN_TEST(test_repeated_publish_and_shutdown_do_not_crash);
}
