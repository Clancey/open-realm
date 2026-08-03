/*
 * test_bz_quest_audio_lifecycle.c - exhaustive legal/illegal transition
 * coverage for bz_quest_audio_lifecycle.c's pure AAudio stream state
 * machine and one-shot-per-change drop counter helper (layer 7).
 */
#include "bz_quest_audio_lifecycle.h"
#include "test_framework.h"

static void test_init_starts_stopped(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_STOPPED);
}

static void test_start_from_stopped_succeeds(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    ASSERT(bz_quest_audio_lifecycle_can_start(&lc));
    bz_quest_audio_lifecycle_mark_started(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_RUNNING);
}

static void test_start_from_failed_succeeds(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    bz_quest_audio_lifecycle_mark_start_failed(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_FAILED);
    ASSERT(bz_quest_audio_lifecycle_can_start(&lc)); /* a fresh attempt may recover - see header */
    bz_quest_audio_lifecycle_mark_started(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_RUNNING);
}

static void test_cannot_start_while_running_or_paused(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    bz_quest_audio_lifecycle_mark_started(&lc);
    ASSERT(!bz_quest_audio_lifecycle_can_start(&lc));
    bz_quest_audio_lifecycle_mark_paused(&lc);
    ASSERT(!bz_quest_audio_lifecycle_can_start(&lc));
}

static void test_pause_resume_round_trip(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    bz_quest_audio_lifecycle_mark_started(&lc);
    ASSERT(bz_quest_audio_lifecycle_can_pause(&lc));
    bz_quest_audio_lifecycle_mark_paused(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_PAUSED);
    ASSERT(!bz_quest_audio_lifecycle_can_pause(&lc)); /* already paused - no double-pause */
    ASSERT(bz_quest_audio_lifecycle_can_resume(&lc));
    bz_quest_audio_lifecycle_mark_resumed(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_RUNNING);
    ASSERT(!bz_quest_audio_lifecycle_can_resume(&lc)); /* already running - no double-resume */
}

static void test_cannot_pause_when_stopped_or_failed(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    ASSERT(!bz_quest_audio_lifecycle_can_pause(&lc));
    bz_quest_audio_lifecycle_mark_start_failed(&lc);
    ASSERT(!bz_quest_audio_lifecycle_can_pause(&lc));
}

static void test_cannot_resume_when_stopped_running_or_failed(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    ASSERT(!bz_quest_audio_lifecycle_can_resume(&lc));
    bz_quest_audio_lifecycle_mark_started(&lc);
    ASSERT(!bz_quest_audio_lifecycle_can_resume(&lc));
    bz_quest_audio_lifecycle_mark_start_failed(&lc);
    ASSERT(!bz_quest_audio_lifecycle_can_resume(&lc));
}

static void test_stop_from_running_and_paused(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    bz_quest_audio_lifecycle_mark_started(&lc);
    ASSERT(bz_quest_audio_lifecycle_can_stop(&lc));
    bz_quest_audio_lifecycle_mark_stopped(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_STOPPED);

    bz_quest_audio_lifecycle_mark_started(&lc);
    bz_quest_audio_lifecycle_mark_paused(&lc);
    ASSERT(bz_quest_audio_lifecycle_can_stop(&lc));
    bz_quest_audio_lifecycle_mark_stopped(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_STOPPED);
}

static void test_cannot_stop_when_stopped_or_failed(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    ASSERT(!bz_quest_audio_lifecycle_can_stop(&lc));
    bz_quest_audio_lifecycle_mark_start_failed(&lc);
    ASSERT(!bz_quest_audio_lifecycle_can_stop(&lc));
}

static void test_restart_only_from_running_or_paused(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    ASSERT(!bz_quest_audio_lifecycle_should_restart(&lc)); /* STOPPED: nothing to restart */
    bz_quest_audio_lifecycle_mark_start_failed(&lc);
    ASSERT(!bz_quest_audio_lifecycle_should_restart(&lc)); /* FAILED: already torn down */

    bz_quest_audio_lifecycle_init(&lc);
    bz_quest_audio_lifecycle_mark_started(&lc);
    ASSERT(bz_quest_audio_lifecycle_should_restart(&lc));
    bz_quest_audio_lifecycle_mark_paused(&lc);
    ASSERT(bz_quest_audio_lifecycle_should_restart(&lc));
}

static void test_restart_success_always_lands_running(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    bz_quest_audio_lifecycle_mark_started(&lc);
    bz_quest_audio_lifecycle_mark_paused(&lc);
    bz_quest_audio_lifecycle_mark_restarted(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_RUNNING);
}

static void test_restart_failure_lands_failed(void) {
    bzQuestAudioLifecycle_t lc;
    bz_quest_audio_lifecycle_init(&lc);
    bz_quest_audio_lifecycle_mark_started(&lc);
    bz_quest_audio_lifecycle_mark_restart_failed(&lc);
    ASSERT_EQ_INT(lc.state, BZ_QUEST_AUDIO_LC_FAILED);
    ASSERT(!bz_quest_audio_lifecycle_should_restart(&lc)); /* FAILED: do not spin restarting */
}

static void test_counter_changed_logs_once_per_value(void) {
    uint32_t lastLogged = 0;
    ASSERT(!bz_quest_audio_lifecycle_counter_changed(&lastLogged, 0)); /* no change from initial 0 */
    ASSERT(bz_quest_audio_lifecycle_counter_changed(&lastLogged, 3));  /* first real drop */
    ASSERT_EQ_INT(lastLogged, 3);
    ASSERT(!bz_quest_audio_lifecycle_counter_changed(&lastLogged, 3)); /* same value again - no log */
    ASSERT(!bz_quest_audio_lifecycle_counter_changed(&lastLogged, 3)); /* still no log, repeatedly */
    ASSERT(bz_quest_audio_lifecycle_counter_changed(&lastLogged, 7));  /* increased again - log once */
    ASSERT_EQ_INT(lastLogged, 7);
}

static void test_counter_changed_logs_on_decrease_too(void) {
    /* A decreasing counter should never happen in practice (see header),
     * but must still be surfaced once rather than silently ignored. */
    uint32_t lastLogged = 10;
    ASSERT(bz_quest_audio_lifecycle_counter_changed(&lastLogged, 2));
    ASSERT_EQ_INT(lastLogged, 2);
}

void run_bz_quest_audio_lifecycle_tests(void) {
    RUN_TEST(test_init_starts_stopped);
    RUN_TEST(test_start_from_stopped_succeeds);
    RUN_TEST(test_start_from_failed_succeeds);
    RUN_TEST(test_cannot_start_while_running_or_paused);
    RUN_TEST(test_pause_resume_round_trip);
    RUN_TEST(test_cannot_pause_when_stopped_or_failed);
    RUN_TEST(test_cannot_resume_when_stopped_running_or_failed);
    RUN_TEST(test_stop_from_running_and_paused);
    RUN_TEST(test_cannot_stop_when_stopped_or_failed);
    RUN_TEST(test_restart_only_from_running_or_paused);
    RUN_TEST(test_restart_success_always_lands_running);
    RUN_TEST(test_restart_failure_lands_failed);
    RUN_TEST(test_counter_changed_logs_once_per_value);
    RUN_TEST(test_counter_changed_logs_on_decrease_too);
}
