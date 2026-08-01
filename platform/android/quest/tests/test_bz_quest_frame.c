/*
 * test_bz_quest_frame.c - coverage for bz_quest_frame.c's pure diagnostic
 * frame-descriptor assembly and throttled-log decision (layer 4). Each case
 * covers a normal path and its inverse/error path, per AGENTS.md's test
 * discipline, including cache/readiness hit/miss style coverage for
 * bz_quest_frame_should_log() (this layer's one piece of retained "cache"
 * state - the previous frame it compares against).
 */
#include <string.h>

#include "bz_quest_frame.h"
#include "test_framework.h"

static void values_defaults(bzQuestFrameValues_t *v) {
    memset(v, 0, sizeof(*v));
}

/* ------------------------------------------------------------------ */
/* bz_quest_frame_reset                                                */
/* ------------------------------------------------------------------ */

static void test_reset_yields_no_snapshot_idle(void) {
    bzQuestFrame_t frame;
    memset(&frame, 0xAA, sizeof(frame)); /* poison first, to prove reset actually clears it */
    bz_quest_frame_reset(&frame);
    ASSERT_EQ_INT(frame.status, BZ_QUEST_FRAME_NO_SNAPSHOT);
    ASSERT_EQ_INT(frame.lifecycleState, BZ_TABLETOP_STATE_IDLE);
    ASSERT_EQ_INT(frame.generation, 0);
    ASSERT_STR_EQ(frame.lifecycleError, "");
}

/* ------------------------------------------------------------------ */
/* bz_quest_frame_from_values                                          */
/* ------------------------------------------------------------------ */

static void test_from_values_no_snapshot(void) {
    bzQuestFrameValues_t values;
    values_defaults(&values);
    values.haveSnapshot = false;
    values.lifecycleState = BZ_TABLETOP_STATE_STARTING;

    bzQuestFrame_t frame;
    bz_quest_frame_from_values(&values, &frame);
    ASSERT_EQ_INT(frame.status, BZ_QUEST_FRAME_NO_SNAPSHOT);
    ASSERT_EQ_INT(frame.lifecycleState, BZ_TABLETOP_STATE_STARTING);
}

static void test_from_values_abi_mismatch(void) {
    bzQuestFrameValues_t values;
    values_defaults(&values);
    values.haveSnapshot = true;
    values.abiVersion = BZ_TABLETOP_ABI_VERSION + 1;
    values.generation = 5;

    bzQuestFrame_t frame;
    bz_quest_frame_from_values(&values, &frame);
    ASSERT_EQ_INT(frame.status, BZ_QUEST_FRAME_ABI_MISMATCH);
    ASSERT_EQ_INT(frame.generation, 5);
}

static void test_from_values_ok_copies_fields(void) {
    bzQuestFrameValues_t values;
    values_defaults(&values);
    values.haveSnapshot = true;
    values.abiVersion = BZ_TABLETOP_ABI_VERSION;
    values.generation = 42;
    values.mapLoaded = true;
    values.mapName = "(2)IceCrown";
    values.playerValid = true;
    values.playerNumber = 1;
    values.playerTeam = 2;
    values.entityCount = 17;
    values.entitiesOverflowCount = 3;
    values.selectedEntityCount = 2;
    values.fogPresent = true;
    values.fogWidth = 64;
    values.fogHeight = 64;
    values.configStringCount = 8;
    values.actionLayoutPresent = true;
    values.lifecycleState = BZ_TABLETOP_STATE_RUNNING;
    values.lifecycleError = NULL;

    bzQuestFrame_t frame;
    bz_quest_frame_from_values(&values, &frame);
    ASSERT_EQ_INT(frame.status, BZ_QUEST_FRAME_OK);
    ASSERT_EQ_INT(frame.generation, 42);
    ASSERT(frame.mapLoaded);
    ASSERT_STR_EQ(frame.mapName, "(2)IceCrown");
    ASSERT(frame.playerValid);
    ASSERT_EQ_INT(frame.playerNumber, 1);
    ASSERT_EQ_INT(frame.playerTeam, 2);
    ASSERT_EQ_INT(frame.entityCount, 17);
    ASSERT_EQ_INT(frame.entitiesOverflowCount, 3);
    ASSERT_EQ_INT(frame.selectedEntityCount, 2);
    ASSERT(frame.fogPresent);
    ASSERT_EQ_INT(frame.fogWidth, 64);
    ASSERT_EQ_INT(frame.fogHeight, 64);
    ASSERT_EQ_INT(frame.configStringCount, 8);
    ASSERT(frame.actionLayoutPresent);
    ASSERT_EQ_INT(frame.lifecycleState, BZ_TABLETOP_STATE_RUNNING);
    ASSERT_STR_EQ(frame.lifecycleError, "");
}

static void test_from_values_truncates_oversized_map_name(void) {
    char longName[BZ_QUEST_FRAME_MAP_NAME_MAX + 64];
    memset(longName, 'x', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';

    bzQuestFrameValues_t values;
    values_defaults(&values);
    values.haveSnapshot = true;
    values.abiVersion = BZ_TABLETOP_ABI_VERSION;
    values.mapLoaded = true;
    values.mapName = longName;

    bzQuestFrame_t frame;
    bz_quest_frame_from_values(&values, &frame);
    ASSERT_EQ_INT(strlen(frame.mapName), BZ_QUEST_FRAME_MAP_NAME_MAX - 1); /* NUL-terminated, never overflowed */
}

static void test_from_values_null_map_name_and_error_leave_empty_strings(void) {
    bzQuestFrameValues_t values;
    values_defaults(&values);
    values.haveSnapshot = true;
    values.abiVersion = BZ_TABLETOP_ABI_VERSION;
    values.mapName = NULL;
    values.lifecycleError = NULL;

    bzQuestFrame_t frame;
    bz_quest_frame_from_values(&values, &frame);
    ASSERT_STR_EQ(frame.mapName, "");
    ASSERT_STR_EQ(frame.lifecycleError, "");
}

/* ------------------------------------------------------------------ */
/* bz_quest_frame_should_log - the retained "cache" state hit/miss cases */
/* ------------------------------------------------------------------ */

static void test_should_log_null_pointers_never_log(void) {
    bzQuestFrame_t frame;
    bz_quest_frame_reset(&frame);
    ASSERT(!bz_quest_frame_should_log(NULL, &frame));
    ASSERT(!bz_quest_frame_should_log(&frame, NULL));
    ASSERT(!bz_quest_frame_should_log(NULL, NULL));
}

static void test_should_log_cache_hit_identical_frame_never_logs(void) {
    /* "Cache hit": nothing changed since the previous frame - must never
     * log, satisfying "never log per frame". */
    bzQuestFrame_t previous, current;
    bz_quest_frame_reset(&previous);
    bz_quest_frame_reset(&current);
    ASSERT(!bz_quest_frame_should_log(&previous, &current));
}

static void test_should_log_cache_miss_status_transition_logs(void) {
    bzQuestFrame_t previous, current;
    bz_quest_frame_reset(&previous); /* NO_SNAPSHOT */
    current = previous;
    current.status = BZ_QUEST_FRAME_OK;
    ASSERT(bz_quest_frame_should_log(&previous, &current));
}

static void test_should_log_cache_miss_generation_advance_logs_only_when_ok(void) {
    bzQuestFrame_t previous, current;
    bz_quest_frame_reset(&previous);
    previous.status = BZ_QUEST_FRAME_OK;
    previous.generation = 10;
    current = previous;
    current.generation = 11;
    ASSERT(bz_quest_frame_should_log(&previous, &current));

    /* Generation advancing while NOT OK (e.g. still NO_SNAPSHOT/mismatch)
     * must not itself trigger a log - only a status change would (covered
     * above); this guards against over-eager logging on meaningless
     * generation churn in a non-OK snapshot state. */
    previous.status = BZ_QUEST_FRAME_ABI_MISMATCH;
    previous.generation = 10;
    current = previous;
    current.generation = 11;
    ASSERT(!bz_quest_frame_should_log(&previous, &current));
}

static void test_should_log_cache_hit_same_generation_ok_never_logs(void) {
    bzQuestFrame_t previous, current;
    bz_quest_frame_reset(&previous);
    previous.status = BZ_QUEST_FRAME_OK;
    previous.generation = 10;
    current = previous;
    ASSERT(!bz_quest_frame_should_log(&previous, &current));
}

static void test_should_log_lifecycle_state_change_logs(void) {
    bzQuestFrame_t previous, current;
    bz_quest_frame_reset(&previous);
    current = previous;
    current.lifecycleState = BZ_TABLETOP_STATE_RUNNING;
    ASSERT(bz_quest_frame_should_log(&previous, &current));
}

static void test_should_log_lifecycle_error_appear_and_clear_logs(void) {
    bzQuestFrame_t previous, current;
    bz_quest_frame_reset(&previous);
    current = previous;
    strcpy(current.lifecycleError, "engine failed to init data directory");
    ASSERT(bz_quest_frame_should_log(&previous, &current));

    /* Error clearing (current -> empty) must also log - a developer
     * watching logcat needs to see recovery, not just onset. */
    previous = current;
    current.lifecycleError[0] = '\0';
    ASSERT(bz_quest_frame_should_log(&previous, &current));
}

void run_bz_quest_frame_tests(void) {
    RUN_TEST(test_reset_yields_no_snapshot_idle);
    RUN_TEST(test_from_values_no_snapshot);
    RUN_TEST(test_from_values_abi_mismatch);
    RUN_TEST(test_from_values_ok_copies_fields);
    RUN_TEST(test_from_values_truncates_oversized_map_name);
    RUN_TEST(test_from_values_null_map_name_and_error_leave_empty_strings);
    RUN_TEST(test_should_log_null_pointers_never_log);
    RUN_TEST(test_should_log_cache_hit_identical_frame_never_logs);
    RUN_TEST(test_should_log_cache_miss_status_transition_logs);
    RUN_TEST(test_should_log_cache_miss_generation_advance_logs_only_when_ok);
    RUN_TEST(test_should_log_cache_hit_same_generation_ok_never_logs);
    RUN_TEST(test_should_log_lifecycle_state_change_logs);
    RUN_TEST(test_should_log_lifecycle_error_appear_and_clear_logs);
}
