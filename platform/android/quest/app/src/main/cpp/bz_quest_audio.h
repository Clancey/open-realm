/*
 * bz_quest_audio.h - Quest-owned AAudio output sink (layer 7): the ONLY
 * translation unit in this project that includes <aaudio/AAudio.h> and
 * owns a real AAudioStream. Bridges the shared bounded tabletop audio
 * queue (platform/bridge/bz_tabletop_audio.h - unchanged, no new ABI) to
 * an actual Android audio output device via bz_quest_wav.h (parsing) and
 * bz_quest_audio_mixer.h (RT-safe mixing), driven by
 * bz_quest_audio_lifecycle.h's pure state machine.
 *
 * Threading model (mirrors bz_quest_bridge.h's "one thread owns the
 * lifecycle" convention, extended with AAudio's own RT callback thread):
 *
 *   - Control thread: Android's main/UI thread, the same one
 *     bz_quest_host.c's android_main() already runs everything else on.
 *     Every function below except the two AAudio callbacks themselves
 *     must only ever be called from this thread. bz_quest_audio_drain()
 *     is the one that actually touches BZ_TTAudio_Dequeue(),
 *     bz_quest_wav_parse(), bz_quest_audio_mixer_convert()/submit()/
 *     reap() - i.e. every allocating/decoding/logging step - called once
 *     per android_main() loop iteration, exactly like
 *     bz_quest_snapshot_capture() already is (see bz_quest_host.c).
 *   - AAudio RT callback thread: a high-priority thread AAudio itself
 *     creates and owns (see https://developer.android.com/ndk/guides/
 *     audio/aaudio/aaudio#using-a-high-priority-callback). Only ever
 *     calls bz_quest_audio_mixer_render() (see that header's real-time-
 *     safety contract) - never allocates, locks, logs, or touches the
 *     bridge/engine.
 *   - AAudio error-callback thread: a separate thread AAudio may invoke
 *     the registered error callback from - explicitly NOT safe to stop/
 *     close the stream from (see https://developer.android.com/ndk/
 *     guides/audio/aaudio/aaudio#disconnected "If you are notified of the
 *     disconnect in an error callback thread then the stopping and
 *     closing of the stream must be done from another thread"). This
 *     callback does nothing but publish a single atomic flag;
 *     bz_quest_audio_drain() (control thread) performs the actual close/
 *     reopen/restart - see that function's own comment.
 *
 * Format: requests AAUDIO_FORMAT_PCM_I16 and a stereo
 * (BZ_QUEST_AUDIO_TARGET_CHANNELS) channel count explicitly - matching
 * this project's "request a compatible stream and reject mismatches
 * explicitly" requirement (see bz_quest_audio_mixer.h) - and verifies both
 * after opening rather than trusting the request silently held (AAudio's
 * own docs promise format/channel count are honored exactly when
 * specified, but this is defensive, not redundant: a future edit that
 * drops one of the explicit builder calls would otherwise silently start
 * converting to the wrong channel layout). Sample rate is deliberately
 * NOT requested - AAudio picks the device's native rate, and
 * bz_quest_audio_mixer_convert() resamples every voice to whatever that
 * turns out to be (queried via AAudioStream_getSampleRate() once, after
 * open - see https://developer.android.com/ndk/guides/audio/aaudio/aaudio
 * "After the stream is opened you must query the sample data format").
 *
 * Explicitly out of scope for this prototype (see docs/quest-tabletop.md's
 * Layer 7 section): spatial/Meta XR Audio, buffer-size/latency tuning
 * (AAUDIO_PERFORMANCE_MODE_NONE - the balanced default - is used, not
 * LOW_LATENCY), exclusive sharing mode, and any decoded-audio format
 * beyond bz_quest_wav.h's strict PCM WAVE subset.
 */
#ifndef BZ_QUEST_AUDIO_H
#define BZ_QUEST_AUDIO_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <aaudio/AAudio.h>

#include "bz_quest_audio_lifecycle.h"
#include "bz_quest_audio_mixer.h"
#include "platform/bridge/bz_tabletop_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    AAudioStream *stream; /* NULL whenever lc.state is STOPPED or FAILED */
    bzQuestAudioLifecycle_t lc;
    bzQuestAudioMixer_t mixer;
    uint32_t sampleRate; /* the stream's actual native rate; 0 until a successful open */

    /* Written ONLY by bz_quest_audio_error_callback() (release store, any
     * AAudio-owned thread); read/cleared ONLY by bz_quest_audio_drain() on
     * the control thread (acquire load then relaxed clear once observed -
     * see that function's comment for why a plain bool would race). */
    atomic_bool disconnected;

    /* Control-thread-only bookkeeping - see bz_quest_audio_drain(). */
    uint32_t voiceDropCount;        /* cumulative bz_quest_audio_mixer_submit() failures */
    uint32_t lastLoggedQueueDrops;  /* last BZ_TTAudio_DroppedCount() value logged */
    uint32_t lastLoggedVoiceDrops;  /* last voiceDropCount value logged */
    char lastParseError[BZ_QUEST_WAV_ERROR_MAX]; /* dedupes repeated identical WAV rejections */
    uint8_t dequeueBuf[BZ_TT_AUDIO_MAX_BYTES];    /* reused scratch buffer for BZ_TTAudio_Dequeue() */
} bzQuestAudio_t;

/*
 * Configures the shared queue for the real device sink
 * (BZ_TTAudio_Configure(..., BZ_TT_AUDIO_DEVICE) - see
 * platform/tabletop/client/s_tabletop_null.c), opens and starts an AAudio
 * stream, and marks the lifecycle RUNNING. Caller must zero-initialize
 * `audio` before the very first call (mirrors bzQuestBridge_t's own
 * contract). Only legal from STOPPED or a previously FAILED state (see
 * bz_quest_audio_lifecycle_can_start()) - returns false without side
 * effects (already logged) if called from RUNNING/PAUSED.
 *
 * On any failure (BZ_TTAudio_Configure(DEVICE) itself failing, or the
 * AAudio stream failing to open/verify/start), reconfigures the queue
 * back to BZ_TT_AUDIO_DUMMY (matches LiveTabletopAudio.swift's own
 * "leave the shared queue harmlessly dummy on failure" convention) and
 * leaves the lifecycle FAILED.
 */
bool bz_quest_audio_start(bzQuestAudio_t *audio);

/*
 * Drains every currently-queued sound (BZ_TTAudio_Dequeue() in a loop
 * until empty), parses each as WAVE (bz_quest_wav_parse()), converts it
 * to the stream's native format (bz_quest_audio_mixer_convert()), and
 * submits it into the RT-safe voice pool (bz_quest_audio_mixer_submit())
 * - then reaps any voice the RT callback has already finished
 * (bz_quest_audio_mixer_reap()). Call once per bz_quest_host.c
 * android_main() loop iteration, exactly like bz_quest_snapshot_capture()
 * - control-thread-only; this is the one function that allocates/logs/
 * touches the bridge.
 *
 * A parse failure is logged once per *distinct* rejection reason (not
 * once per occurrence - see bz_quest_wav.h's own "once per unique
 * condition" convention) and the offending item is dropped; a conversion
 * failure (only possible from a pathological/oversized frame count - see
 * bz_quest_audio_mixer_convert()'s own bound) is logged and dropped the
 * same way. A voice-pool-full submit failure increments this sink's own
 * drop counter and logs only when that counter's value actually changes
 * (see bz_quest_audio_lifecycle_counter_changed()) - never once per
 * drain() call. The shared queue's own BZ_TTAudio_DroppedCount() is
 * logged with the same one-shot-per-change rule.
 *
 * If the AAudio error callback has flagged a disconnect
 * (bz_quest_audio_error_callback()) since the last drain, this function
 * performs the actual close+reopen+restart here on the control thread
 * (never from the error-callback thread itself - see this header's top
 * comment) before doing anything else. A failed restart attempt leaves
 * the lifecycle FAILED and every subsequent drain() call becomes a no-op
 * until bz_quest_audio_stop()+bz_quest_audio_start() are called again
 * (mirrors bz_quest_renderer_init()'s own "never silently retry from
 * inside the command loop" convention in bz_quest_host.c).
 *
 * A no-op (returns immediately) when the lifecycle is STOPPED or FAILED.
 * Still drains/converts/submits while PAUSED (mirrors
 * LiveTabletopTransport.swift's own poll()->audio.drain() call, which is
 * likewise unconditional on suspend state) - newly submitted voices
 * simply wait silently until the next resume, since a paused AAudio
 * stream's RT callback never fires.
 */
void bz_quest_audio_drain(bzQuestAudio_t *audio);

/* AAudioStream_requestPause(). Control-thread-only; only legal from
 * RUNNING (bz_quest_audio_lifecycle_can_pause()) - a caller bug to invoke
 * this from any other state, not silently ignored beyond a logged error. */
void bz_quest_audio_suspend(bzQuestAudio_t *audio);

/* AAudioStream_requestStart() again after a prior suspend. Control-
 * thread-only; only legal from PAUSED. */
void bz_quest_audio_resume(bzQuestAudio_t *audio);

/*
 * AAudioStream_requestStop()+AAudioStream_close(), frees every voice
 * slot's owned PCM buffer regardless of its active flag (safe: no
 * callback can be running once close() has returned - see
 * https://developer.android.com/ndk/guides/audio/aaudio/aaudio "Closing
 * an audio stream"), and reconfigures the shared queue back to
 * BZ_TT_AUDIO_DUMMY. Control-thread-only; only legal from RUNNING or
 * PAUSED (bz_quest_audio_lifecycle_can_stop()) - idempotent no-op with a
 * logged error if already STOPPED/FAILED, mirroring
 * bz_quest_bridge_destroy()'s own "safe to call even on a never-started
 * or already-torn-down instance" convention.
 */
void bz_quest_audio_stop(bzQuestAudio_t *audio);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_AUDIO_H */
