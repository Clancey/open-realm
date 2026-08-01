/*
 * bz_quest_audio.c - see bz_quest_audio.h for the full contract, threading
 * model, and format-negotiation rationale.
 */
#include "bz_quest_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bz_quest_log.h"

/* AAudio's data callback - the ONLY function in this whole file (or any
 * file this one calls into) ever invoked on AAudio's real-time-priority
 * callback thread. Touches nothing but bz_quest_audio_mixer_render() - see
 * bz_quest_audio_mixer.h's real-time-safety contract. `stream`/`userData`
 * beyond `audio` are unused: this sink has exactly one stream and no
 * per-callback bookkeeping beyond what the mixer itself already owns. */
static aaudio_data_callback_result_t bz_quest_audio_data_callback(AAudioStream *stream, void *userData,
                                                                   void *audioData, int32_t numFrames) {
    (void)stream;
    bzQuestAudio_t *audio = (bzQuestAudio_t *)userData;
    bz_quest_audio_mixer_render(&audio->mixer, (int16_t *)audioData, (uint32_t)numFrames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/* AAudio's error callback - may run on yet another AAudio-owned thread,
 * and the AAudio docs explicitly forbid stopping/closing the stream from
 * here (see bz_quest_audio.h's header comment). The ONE safe action is
 * this single atomic release-store; bz_quest_audio_drain() (control
 * thread) does the actual recovery work on its own next call. */
static void bz_quest_audio_error_callback(AAudioStream *stream, void *userData, aaudio_result_t error) {
    (void)stream;
    bzQuestAudio_t *audio = (bzQuestAudio_t *)userData;
    BZ_QUEST_LOGE("AAudio error callback fired: %s (deferring recovery to the control thread)",
                  AAudio_convertResultToText(error));
    atomic_store_explicit(&audio->disconnected, true, memory_order_release);
}

/*
 * Builds, opens, verifies, and starts a fresh AAudioStream into
 * audio->stream/sampleRate. Shared by bz_quest_audio_start() (first open)
 * and bz_quest_audio_drain()'s disconnect-recovery path (reopen) - see
 * bz_quest_audio.h's header comment for why both must run on the control
 * thread. Leaves audio->stream/sampleRate untouched on failure (the
 * caller's own prior state, if any, is already closed by the time this is
 * called in both real call sites).
 */
static bool bz_quest_audio_open_and_start(bzQuestAudio_t *audio) {
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK || builder == NULL) {
        BZ_QUEST_LOGE("AAudio_createStreamBuilder failed: %s", AAudio_convertResultToText(result));
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, BZ_QUEST_AUDIO_TARGET_CHANNELS);
    /* Balanced default, not AAUDIO_PERFORMANCE_MODE_LOW_LATENCY - see
     * bz_quest_audio.h's header comment on scope. */
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setDataCallback(builder, bz_quest_audio_data_callback, audio);
    AAudioStreamBuilder_setErrorCallback(builder, bz_quest_audio_error_callback, audio);

    AAudioStream *stream = NULL;
    result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK || stream == NULL) {
        BZ_QUEST_LOGE("AAudioStreamBuilder_openStream failed: %s", AAudio_convertResultToText(result));
        return false;
    }

    /* Defensive re-check, not redundant with the builder calls above - see
     * bz_quest_audio.h's header comment ("reject mismatches explicitly"). */
    if (AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_I16 ||
        AAudioStream_getChannelCount(stream) != BZ_QUEST_AUDIO_TARGET_CHANNELS) {
        BZ_QUEST_LOGE(
            "AAudio granted a stream with an unexpected format/channel count - rejecting rather "
            "than mixing into a mismatched buffer");
        AAudioStream_close(stream);
        return false;
    }

    int32_t sampleRate = AAudioStream_getSampleRate(stream);
    if (sampleRate <= 0) {
        BZ_QUEST_LOGE("AAudio reported an invalid stream sample rate (%d)", (int)sampleRate);
        AAudioStream_close(stream);
        return false;
    }

    result = AAudioStream_requestStart(stream);
    if (result != AAUDIO_OK) {
        BZ_QUEST_LOGE("AAudioStream_requestStart failed: %s", AAudio_convertResultToText(result));
        AAudioStream_close(stream);
        return false;
    }

    audio->stream = stream;
    audio->sampleRate = (uint32_t)sampleRate;
    atomic_store_explicit(&audio->disconnected, false, memory_order_relaxed);
    return true;
}

bool bz_quest_audio_start(bzQuestAudio_t *audio) {
    if (!bz_quest_audio_lifecycle_can_start(&audio->lc)) {
        BZ_QUEST_LOGE("bz_quest_audio_start called from an illegal lifecycle state - ignoring");
        return false;
    }

    bz_quest_audio_mixer_init(&audio->mixer);
    audio->voiceDropCount = 0;
    audio->lastLoggedQueueDrops = 0;
    audio->lastLoggedVoiceDrops = 0;
    audio->lastParseError[0] = '\0';

    if (!BZ_TTAudio_Configure(BZ_TABLETOP_AUDIO_ABI_VERSION, BZ_TT_AUDIO_DEVICE)) {
        BZ_QUEST_LOGE("BZ_TTAudio_Configure(DEVICE) failed - shared audio queue rejected this ABI version");
        bz_quest_audio_lifecycle_mark_start_failed(&audio->lc);
        return false;
    }

    if (!bz_quest_audio_open_and_start(audio)) {
        BZ_TTAudio_Configure(BZ_TABLETOP_AUDIO_ABI_VERSION, BZ_TT_AUDIO_DUMMY);
        bz_quest_audio_lifecycle_mark_start_failed(&audio->lc);
        return false;
    }

    bz_quest_audio_lifecycle_mark_started(&audio->lc);
    BZ_QUEST_LOGI("bz_quest_audio_start succeeded (nativeSampleRate=%u)", audio->sampleRate);
    return true;
}

void bz_quest_audio_drain(bzQuestAudio_t *audio) {
    if (audio->lc.state == BZ_QUEST_AUDIO_LC_STOPPED || audio->lc.state == BZ_QUEST_AUDIO_LC_FAILED) return;

    if (atomic_load_explicit(&audio->disconnected, memory_order_acquire) &&
        bz_quest_audio_lifecycle_should_restart(&audio->lc)) {
        BZ_QUEST_LOGE("closing the disconnected AAudio stream and attempting a fresh one");
        AAudioStream_requestStop(audio->stream);
        AAudioStream_close(audio->stream);
        audio->stream = NULL;
        atomic_store_explicit(&audio->disconnected, false, memory_order_relaxed);

        if (bz_quest_audio_open_and_start(audio)) {
            bz_quest_audio_lifecycle_mark_restarted(&audio->lc);
            BZ_QUEST_LOGI("AAudio stream restarted after disconnect (nativeSampleRate=%u)", audio->sampleRate);
        } else {
            bz_quest_audio_lifecycle_mark_restart_failed(&audio->lc);
            BZ_QUEST_LOGE(
                "AAudio stream restart failed - audio sink stopped for this session; "
                "bz_quest_audio_stop()+bz_quest_audio_start() are required to try again");
            return;
        }
    }

    uint32_t queueDrops = BZ_TTAudio_DroppedCount(BZ_TABLETOP_AUDIO_ABI_VERSION);
    if (bz_quest_audio_lifecycle_counter_changed(&audio->lastLoggedQueueDrops, queueDrops)) {
        BZ_QUEST_LOGE("tabletop audio queue has dropped %u sound(s) total (BZ_TT_AUDIO_QUEUE_CAPACITY exceeded)",
                      queueDrops);
    }

    for (;;) {
        size_t size = 0;
        if (!BZ_TTAudio_Dequeue(BZ_TABLETOP_AUDIO_ABI_VERSION, audio->dequeueBuf, sizeof(audio->dequeueBuf), &size))
            break;

        bzQuestWav_t wav;
        char parseError[BZ_QUEST_WAV_ERROR_MAX];
        if (!bz_quest_wav_parse(audio->dequeueBuf, size, &wav, parseError, sizeof(parseError))) {
            /* Once per *distinct* reason, not once per occurrence - see
             * bz_quest_audio.h's drain() comment and AGENTS.md's "Missing
             * Asset Placeholders" one-shot-per-condition convention. */
            if (strncmp(audio->lastParseError, parseError, sizeof(audio->lastParseError)) != 0) {
                snprintf(audio->lastParseError, sizeof(audio->lastParseError), "%s", parseError);
                BZ_QUEST_LOGE("rejected an unsupported/corrupt Warcraft sound: %s", parseError);
            }
            continue;
        }

        int16_t *pcm = NULL;
        uint32_t frames = 0;
        if (!bz_quest_audio_mixer_convert(&wav, audio->sampleRate, &pcm, &frames)) {
            BZ_QUEST_LOGE("failed to convert a parsed Warcraft sound to the AAudio stream's native format");
            continue;
        }

        if (!bz_quest_audio_mixer_submit(&audio->mixer, pcm, frames)) {
            free(pcm);
            audio->voiceDropCount++;
            if (bz_quest_audio_lifecycle_counter_changed(&audio->lastLoggedVoiceDrops, audio->voiceDropCount)) {
                BZ_QUEST_LOGE("voice pool full - dropped %u sound(s) total (BZ_QUEST_AUDIO_MAX_VOICES exceeded)",
                              audio->voiceDropCount);
            }
        }
    }

    bz_quest_audio_mixer_reap(&audio->mixer);
}

void bz_quest_audio_suspend(bzQuestAudio_t *audio) {
    if (!bz_quest_audio_lifecycle_can_pause(&audio->lc)) {
        BZ_QUEST_LOGE("bz_quest_audio_suspend called from an illegal lifecycle state - ignoring");
        return;
    }
    aaudio_result_t result = AAudioStream_requestPause(audio->stream);
    if (result != AAUDIO_OK) {
        BZ_QUEST_LOGE("AAudioStream_requestPause failed: %s", AAudio_convertResultToText(result));
        /* Still record PAUSED: the request is asynchronous and best-effort
         * per AAudio's own docs (no synchronous failure recovery exists
         * for a pause request) - the next suspend/resume/stop call must
         * still see a consistent state rather than get stuck unable to
         * ever request resume/stop again. */
    }
    bz_quest_audio_lifecycle_mark_paused(&audio->lc);
}

void bz_quest_audio_resume(bzQuestAudio_t *audio) {
    if (!bz_quest_audio_lifecycle_can_resume(&audio->lc)) {
        BZ_QUEST_LOGE("bz_quest_audio_resume called from an illegal lifecycle state - ignoring");
        return;
    }
    aaudio_result_t result = AAudioStream_requestStart(audio->stream);
    if (result != AAUDIO_OK) {
        BZ_QUEST_LOGE("AAudioStream_requestStart (resume) failed: %s", AAudio_convertResultToText(result));
    }
    bz_quest_audio_lifecycle_mark_resumed(&audio->lc);
}

void bz_quest_audio_stop(bzQuestAudio_t *audio) {
    if (!bz_quest_audio_lifecycle_can_stop(&audio->lc)) {
        BZ_QUEST_LOGE("bz_quest_audio_stop called from an illegal lifecycle state - ignoring");
        return;
    }

    AAudioStream_requestStop(audio->stream);
    AAudioStream_close(audio->stream);
    audio->stream = NULL;

    /* No callback can be running once close() has returned (see
     * bz_quest_audio.h's header comment) - safe to free every slot's
     * owned PCM regardless of its active flag, unlike
     * bz_quest_audio_mixer_reap()'s own "only ever touch a slot the
     * callback has already marked inactive" rule during normal operation. */
    for (int i = 0; i < BZ_QUEST_AUDIO_MAX_VOICES; i++) {
        free(audio->mixer.voices[i].pcm);
        audio->mixer.voices[i].pcm = NULL;
    }

    BZ_TTAudio_Configure(BZ_TABLETOP_AUDIO_ABI_VERSION, BZ_TT_AUDIO_DUMMY);
    bz_quest_audio_lifecycle_mark_stopped(&audio->lc);
    BZ_QUEST_LOGI("bz_quest_audio_stop complete");
}
