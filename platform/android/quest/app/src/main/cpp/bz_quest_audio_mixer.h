/*
 * bz_quest_audio_mixer.h - pure, host-testable bounded voice pool + PCM
 * format conversion for the Quest native audio sink (layer 7).
 *
 * Split out from bz_quest_audio.c (the real AAudio-owning translation
 * unit) for exactly this project's existing convention (see
 * bz_quest_wav.h's/bz_quest_data.h's own header comments): everything here
 * is plain C11 with no Android/AAudio dependency, so
 * platform/android/quest/tests/test_bz_quest_audio_mixer.c builds and runs
 * these exact decision paths on the host with a plain C compiler.
 *
 * Real-time-safety contract (the reason this module exists as its own
 * seam, not folded into bz_quest_audio.c's AAudio callback registration):
 * AAudio's data callback runs on a dedicated real-time-priority thread and
 * must never allocate, free, lock a mutex, log, or call back into
 * arbitrary engine/bridge code - see
 * https://developer.android.com/ndk/guides/audio/aaudio/aaudio#audio_callback
 * ("Note: Since callback() is called from a special high priority thread,
 * you should not perform any operation in that thread that might trigger
 * an unbounded delay, such as: [...] allocate memory, [...] use mutexes,
 * [...] read/write files"). bz_quest_audio_mixer_render() (the ONLY
 * function this module allows to be called from that thread) touches
 * nothing but plain arithmetic over already-prepared, already-owned
 * buffers.
 *
 * Producer/consumer handoff: each bzQuestAudioVoice_t's `active` field is a
 * single atomic_bool used as the SOLE synchronization point between the
 * control thread (bz_quest_audio.c's drain step, running on Android's
 * main/UI thread) and the AAudio callback thread - a textbook single-flag
 * lock-free handoff, not a queue:
 *
 *   - Control thread (bz_quest_audio_mixer_submit): finds a slot with
 *     atomic_load(active, acquire) == false, writes pcm/frameCount/cursor
 *     (plain, non-atomic - safe because nothing else touches them while
 *     active is false), THEN atomic_store(active, true, release). The
 *     release publishes those plain writes to whichever thread later
 *     performs the matching acquire.
 *   - Callback thread (bz_quest_audio_mixer_render): atomic_load(active,
 *     acquire); if true, the acquire guarantees it sees the control
 *     thread's pcm/frameCount/cursor writes. It is the ONLY thread that
 *     ever mutates `cursor` once a voice is active (plain read-modify-
 *     write - no other thread touches cursor concurrently). When a voice
 *     finishes (cursor == frameCount), it does atomic_store(active, false,
 *     release) - the one and only write callback ever makes to `active`,
 *     and it never revisits pcm/frameCount/cursor again after that store.
 *   - Control thread (bz_quest_audio_mixer_reap): later, on its own turn,
 *     re-checks atomic_load(active, acquire); once it observes false it
 *     may safely free() `pcm` (nothing else can be concurrently reading
 *     it - the callback thread's own release store above happens-before
 *     this acquire load) and clear the pointer so a later submit() can
 *     reuse the slot.
 *
 * No slot's `pcm`/`frameCount` is ever written by the control thread while
 * `active` is true, and never read by the callback thread once it has
 * itself set `active` to false - so there is no data race despite zero
 * locks.
 */
#ifndef BZ_QUEST_AUDIO_MIXER_H
#define BZ_QUEST_AUDIO_MIXER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_wav.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* Bounds concurrent voices (and therefore worst-case memory: see
     * bz_quest_audio_mixer_convert()'s own output-frame bound) - a
     * tabletop scene never plausibly needs more simultaneous one-shot
     * sound effects than this; the (control-thread-only, never per-frame/
     * per-callback - see bz_quest_audio.c) drop counter below tracks any
     * excess exactly like BZ_TT_AUDIO_QUEUE_CAPACITY's own drop counter. */
    BZ_QUEST_AUDIO_MAX_VOICES = 8,
    /* All voices are converted (see bz_quest_audio_mixer_convert()) to
     * this fixed interleaved format before ever reaching a voice slot -
     * the RT callback then need never branch on source channel count/
     * sample rate/bit depth per-voice. Stereo (not mono) so mixing never
     * needs to widen a mono voice at render time either. */
    BZ_QUEST_AUDIO_TARGET_CHANNELS = 2,
    /* 30 seconds at a plausible 48 kHz target is already far beyond any
     * real WC3 sound asset's length (BZ_TT_AUDIO_MAX_BYTES bounds the
     * *source* file to 1 MiB - a few seconds at most of 22.05 kHz mono
     * 16-bit PCM) - this is a hard backstop against a corrupt/oversized
     * header producing an unbounded conversion allocation, not a value
     * ever expected to bind in practice. */
    BZ_QUEST_AUDIO_MAX_OUTPUT_FRAMES = 48000u * 30u,
};

typedef struct {
    int16_t *pcm;              /* interleaved BZ_QUEST_AUDIO_TARGET_CHANNELS int16 frames; owned, heap */
    uint32_t frameCount;       /* set by control thread before publishing `active` */
    uint32_t cursor;           /* owned exclusively by the callback thread while active */
    _Atomic bool active;       /* the sole control/callback synchronization point - see header comment */
} bzQuestAudioVoice_t;

typedef struct {
    bzQuestAudioVoice_t voices[BZ_QUEST_AUDIO_MAX_VOICES];
} bzQuestAudioMixer_t;

/* Zeroes every voice slot (all inactive, no owned pcm). Control-thread-only. */
void bz_quest_audio_mixer_init(bzQuestAudioMixer_t *mixer);

/*
 * Converts a parsed WAV (see bz_quest_wav.h) to interleaved
 * BZ_QUEST_AUDIO_TARGET_CHANNELS-channel int16 PCM at `targetSampleRate` -
 * deterministic, bounded, and control-thread-only (this is the one
 * function in this module that allocates, exactly because it never runs
 * on the RT callback thread - see bz_quest_audio.c's drain step, the only
 * real caller).
 *
 * 8-bit source samples (unsigned, 128 = silence per the WAVE spec) are
 * widened to signed 16-bit; mono source is duplicated to both output
 * channels (this module's target is always stereo - see
 * BZ_QUEST_AUDIO_TARGET_CHANNELS); a source sample rate that differs from
 * `targetSampleRate` is resampled via deterministic linear interpolation
 * (matches this project's "deterministic bounded conversion... with
 * tests" requirement - never a black-box resampling library). The rounded
 * output frame count is `roundf(wav->frameCount * targetSampleRate /
 * (double)wav->sampleRate)` (0 input frames is already rejected by
 * bz_quest_wav_parse()'s own "'data' chunk is empty" check, so this is
 * always >= 1 for a wav that parsed successfully, given a sane
 * targetSampleRate).
 *
 * Returns true with a freshly malloc()'d *outPcm (caller becomes the
 * owner - either bz_quest_audio_mixer_submit() takes ownership, or the
 * caller must free() it directly on a submit failure) and *outFrames set.
 * Returns false (does not allocate; outPcm/outFrames left unspecified) if
 * `targetSampleRate` is 0, the computed output frame count would exceed
 * BZ_QUEST_AUDIO_MAX_OUTPUT_FRAMES (a corrupt/oversized-looking asset -
 * see this header's enum comment), or malloc() itself fails.
 */
bool bz_quest_audio_mixer_convert(const bzQuestWav_t *wav, uint32_t targetSampleRate, int16_t **outPcm,
                                   uint32_t *outFrames);

/*
 * Publishes a converted voice (see bz_quest_audio_mixer_convert()) into the
 * first free slot. Control-thread-only. `pcm` must be a malloc()'d buffer
 * of `frameCount * BZ_QUEST_AUDIO_TARGET_CHANNELS` int16 samples; ownership
 * transfers to the mixer on success (the caller must NOT free it) and
 * stays with the caller on failure (no free slot - every voice is active;
 * the caller must free(pcm) itself and count this as a voice-pool drop,
 * exactly like BZ_TT_AUDIO_QUEUE_CAPACITY's own drop counter - see
 * bz_quest_audio.c's drain step).
 *
 * Returns false without touching `pcm`'s ownership if `frameCount` is 0,
 * `pcm` is NULL, or every voice is currently active.
 */
bool bz_quest_audio_mixer_submit(bzQuestAudioMixer_t *mixer, int16_t *pcm, uint32_t frameCount);

/*
 * Frees the owned `pcm` buffer of any voice the callback thread has
 * already marked inactive (cursor reached frameCount - see this header's
 * top comment), and clears the slot back to "never used". Control-thread-
 * only; safe to call as often as convenient (e.g. once per drain step) -
 * a slot with no owned pcm left to free is simply skipped.
 */
void bz_quest_audio_mixer_reap(bzQuestAudioMixer_t *mixer);

/*
 * THE ONLY function in this module ever called from the AAudio RT
 * callback thread (see this header's top comment for the full real-time-
 * safety contract). Mixes every currently-active voice's next
 * `frameCount` interleaved stereo frames into `out` (zeroed first, then
 * each voice's samples are added and clamped to the int16 range in turn -
 * a simple, deterministic, allocation-free saturating mixer), advances
 * each mixed voice's cursor, and marks any voice that reaches the end of
 * its buffer inactive (see this header's top comment for why that
 * "active = false" store is itself still real-time-safe: a single atomic
 * store, no allocation, no lock, no I/O).
 *
 * `out` must have room for `frameCount * BZ_QUEST_AUDIO_TARGET_CHANNELS`
 * int16 samples; this function never reads or writes outside that range
 * regardless of any voice's own frameCount/cursor (a voice's remaining
 * frames are always clamped to `frameCount` before mixing).
 */
void bz_quest_audio_mixer_render(bzQuestAudioMixer_t *mixer, int16_t *out, uint32_t frameCount);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_AUDIO_MIXER_H */
