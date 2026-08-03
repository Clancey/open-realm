/*
 * bz_quest_audio_lifecycle.h - pure, host-testable AAudio stream lifecycle
 * state machine + one-shot-per-change drop-counter tracking for the Quest
 * native audio sink (layer 7).
 *
 * Split out from bz_quest_audio.c (the real AAudioStream-owning
 * translation unit) for the same reason bz_quest_pure.h exists for the
 * OpenXR/Vulkan renderer (layer 3) and bz_quest_audio_mixer.h exists for
 * the RT mixer: plain C11, no <aaudio/AAudio.h>/Android dependency, so
 * platform/android/quest/tests/test_bz_quest_audio_lifecycle.c builds and
 * runs every transition/rejection path on the host with a plain C
 * compiler. bz_quest_audio.c calls these functions immediately before/
 * after each real AAudioStream_request*()/close() call and never invents
 * its own parallel state.
 *
 * States mirror the AAudio stream's own stable-state subset relevant to
 * this sink (see https://developer.android.com/ndk/guides/audio/aaudio/
 * aaudio#state-transitions "Open, Started, Paused, Flushed, Stopped" plus
 * the documented "Disconnected" error state) collapsed to exactly what
 * bz_quest_host.c's APP_CMD_* driven calls need: STOPPED (no stream open,
 * or a previously-open stream fully closed), RUNNING (stream open and
 * started), PAUSED (stream open, requestPause'd - mirrors
 * APP_CMD_PAUSE), and FAILED (stream open or (re)open attempt itself
 * failed - terminal until an explicit bz_quest_audio_lifecycle_mark_stopped()
 * + a fresh start attempt; never silently retried by this module itself).
 */
#ifndef BZ_QUEST_AUDIO_LIFECYCLE_H
#define BZ_QUEST_AUDIO_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BZ_QUEST_AUDIO_LC_STOPPED = 0,
    BZ_QUEST_AUDIO_LC_RUNNING,
    BZ_QUEST_AUDIO_LC_PAUSED,
    BZ_QUEST_AUDIO_LC_FAILED,
} bzQuestAudioLcState_t;

typedef struct {
    bzQuestAudioLcState_t state;
} bzQuestAudioLifecycle_t;

/* Sets state to STOPPED. Control-thread-only, like every function below. */
void bz_quest_audio_lifecycle_init(bzQuestAudioLifecycle_t *lc);

/* True only from STOPPED or FAILED (a fresh attempt is always allowed to
 * try to recover from a previously failed attempt - see this header's top
 * comment; nothing auto-retries, bz_quest_audio.c must call this itself). */
bool bz_quest_audio_lifecycle_can_start(const bzQuestAudioLifecycle_t *lc);
void bz_quest_audio_lifecycle_mark_started(bzQuestAudioLifecycle_t *lc);
void bz_quest_audio_lifecycle_mark_start_failed(bzQuestAudioLifecycle_t *lc);

/* True only from RUNNING (pausing an already-paused/stopped/failed sink is
 * a caller bug, not silently swallowed here - matches this project's "no
 * silent fallback" rule). */
bool bz_quest_audio_lifecycle_can_pause(const bzQuestAudioLifecycle_t *lc);
void bz_quest_audio_lifecycle_mark_paused(bzQuestAudioLifecycle_t *lc);

/* True only from PAUSED. */
bool bz_quest_audio_lifecycle_can_resume(const bzQuestAudioLifecycle_t *lc);
void bz_quest_audio_lifecycle_mark_resumed(bzQuestAudioLifecycle_t *lc);

/* True from RUNNING or PAUSED only (stopping an already-stopped/failed
 * sink is a caller bug - same "no silent fallback" rule as pause above). */
bool bz_quest_audio_lifecycle_can_stop(const bzQuestAudioLifecycle_t *lc);
void bz_quest_audio_lifecycle_mark_stopped(bzQuestAudioLifecycle_t *lc);

/*
 * True only when the AAudio error callback (see bz_quest_audio.c) has
 * flagged a disconnect AND this sink was actually open (RUNNING or
 * PAUSED) when the control thread next observed it - a disconnect
 * observed while STOPPED/FAILED means there is nothing left to restart
 * (the stream was already being/already torn down) and must not spuriously
 * re-open a stream nobody asked for.
 */
bool bz_quest_audio_lifecycle_should_restart(const bzQuestAudioLifecycle_t *lc);
/* A restart attempt succeeded - always lands in RUNNING regardless of
 * whether the stream was RUNNING or PAUSED before the disconnect (matches
 * bz_quest_audio.c's drain-driven restart, which always leaves the newly
 * reopened stream started - see that file's header comment for why
 * resuming into PAUSED again would need a second Android lifecycle event
 * that may never arrive if the app is already foregrounded). */
void bz_quest_audio_lifecycle_mark_restarted(bzQuestAudioLifecycle_t *lc);
void bz_quest_audio_lifecycle_mark_restart_failed(bzQuestAudioLifecycle_t *lc);

/*
 * Pure "did this monotonic counter change since I last logged it" check,
 * shared by both the shared bounded-queue drop counter (BZ_TTAudio_
 * DroppedCount()) and this sink's own voice-pool-full drop counter (see
 * bz_quest_audio_mixer_submit()'s own doc comment) - both must be logged
 * once per value change, never once per drain()/frame/callback (this
 * project's "no per-frame log spam" rule - see AGENTS.md's "Missing Asset
 * Placeholders" section for the same one-shot-per-change convention
 * applied elsewhere in this codebase).
 *
 * `lastLogged` is updated to `current` and true is returned (log now)
 * exactly when `current != *lastLogged`; otherwise `*lastLogged` is left
 * untouched and false is returned (nothing changed, do not log).
 * `current` decreasing (should never happen - both source counters are
 * strictly non-decreasing for the lifetime of the process) still counts
 * as "changed" rather than being silently ignored, since a decreasing
 * counter would itself be a bug worth surfacing once.
 */
bool bz_quest_audio_lifecycle_counter_changed(uint32_t *lastLogged, uint32_t current);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_AUDIO_LIFECYCLE_H */
