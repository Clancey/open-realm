/*
 * bz_quest_audio_lifecycle.c - see bz_quest_audio_lifecycle.h for the full
 * contract. Every function here is a plain state-table lookup/update -
 * intentionally trivial so the *rules* (which transitions are legal) are
 * easy to audit and to exercise exhaustively in
 * platform/android/quest/tests/test_bz_quest_audio_lifecycle.c, including
 * every illegal/inverse transition.
 */
#include "bz_quest_audio_lifecycle.h"

void bz_quest_audio_lifecycle_init(bzQuestAudioLifecycle_t *lc) { lc->state = BZ_QUEST_AUDIO_LC_STOPPED; }

bool bz_quest_audio_lifecycle_can_start(const bzQuestAudioLifecycle_t *lc) {
    return lc->state == BZ_QUEST_AUDIO_LC_STOPPED || lc->state == BZ_QUEST_AUDIO_LC_FAILED;
}
void bz_quest_audio_lifecycle_mark_started(bzQuestAudioLifecycle_t *lc) { lc->state = BZ_QUEST_AUDIO_LC_RUNNING; }
void bz_quest_audio_lifecycle_mark_start_failed(bzQuestAudioLifecycle_t *lc) { lc->state = BZ_QUEST_AUDIO_LC_FAILED; }

bool bz_quest_audio_lifecycle_can_pause(const bzQuestAudioLifecycle_t *lc) {
    return lc->state == BZ_QUEST_AUDIO_LC_RUNNING;
}
void bz_quest_audio_lifecycle_mark_paused(bzQuestAudioLifecycle_t *lc) { lc->state = BZ_QUEST_AUDIO_LC_PAUSED; }

bool bz_quest_audio_lifecycle_can_resume(const bzQuestAudioLifecycle_t *lc) {
    return lc->state == BZ_QUEST_AUDIO_LC_PAUSED;
}
void bz_quest_audio_lifecycle_mark_resumed(bzQuestAudioLifecycle_t *lc) { lc->state = BZ_QUEST_AUDIO_LC_RUNNING; }

bool bz_quest_audio_lifecycle_can_stop(const bzQuestAudioLifecycle_t *lc) {
    return lc->state == BZ_QUEST_AUDIO_LC_RUNNING || lc->state == BZ_QUEST_AUDIO_LC_PAUSED;
}
void bz_quest_audio_lifecycle_mark_stopped(bzQuestAudioLifecycle_t *lc) { lc->state = BZ_QUEST_AUDIO_LC_STOPPED; }

bool bz_quest_audio_lifecycle_should_restart(const bzQuestAudioLifecycle_t *lc) {
    return lc->state == BZ_QUEST_AUDIO_LC_RUNNING || lc->state == BZ_QUEST_AUDIO_LC_PAUSED;
}
void bz_quest_audio_lifecycle_mark_restarted(bzQuestAudioLifecycle_t *lc) { lc->state = BZ_QUEST_AUDIO_LC_RUNNING; }
void bz_quest_audio_lifecycle_mark_restart_failed(bzQuestAudioLifecycle_t *lc) {
    lc->state = BZ_QUEST_AUDIO_LC_FAILED;
}

bool bz_quest_audio_lifecycle_counter_changed(uint32_t *lastLogged, uint32_t current) {
    if (current == *lastLogged) return false;
    *lastLogged = current;
    return true;
}
