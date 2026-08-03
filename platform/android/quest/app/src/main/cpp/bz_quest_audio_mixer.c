/*
 * bz_quest_audio_mixer.c - see bz_quest_audio_mixer.h.
 */
#include "bz_quest_audio_mixer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void bz_quest_audio_mixer_init(bzQuestAudioMixer_t *mixer) { memset(mixer, 0, sizeof(*mixer)); }

/* Reads one signed sample from `wav`'s PCM data at (frame, channel),
 * widening an 8-bit unsigned sample to the same symmetric signed 16-bit
 * range bz_quest_wav.h's top comment documents (128 == silence per the
 * WAVE spec) - control-thread-only (called only from
 * bz_quest_audio_mixer_convert() below), never the RT callback. */
static int32_t wav_sample(const bzQuestWav_t *wav, uint32_t frame, uint16_t channel) {
    uint32_t bytesPerSample = wav->bitsPerSample / 8;
    uint32_t blockAlign = (uint32_t)wav->channels * bytesPerSample;
    const uint8_t *p = wav->pcm + (size_t)frame * blockAlign + (size_t)channel * bytesPerSample;
    if (wav->bitsPerSample == 8) return ((int32_t)p[0] - 128) * 256;
    return (int32_t)(int16_t)(p[0] | (p[1] << 8));
}

static int16_t clamp_i16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

bool bz_quest_audio_mixer_convert(const bzQuestWav_t *wav, uint32_t targetSampleRate, int16_t **outPcm,
                                   uint32_t *outFrames) {
    if (!wav || !outPcm || !outFrames || targetSampleRate == 0 || wav->frameCount == 0) return false;

    double ratio = (double)wav->sampleRate / (double)targetSampleRate;
    uint32_t frames = (uint32_t)llround((double)wav->frameCount / ratio);
    if (frames == 0) frames = 1; /* never emit an empty voice - see header comment */
    if (frames > BZ_QUEST_AUDIO_MAX_OUTPUT_FRAMES) return false;

    int16_t *pcm = malloc((size_t)frames * BZ_QUEST_AUDIO_TARGET_CHANNELS * sizeof(int16_t));
    if (!pcm) return false;

    for (uint32_t i = 0; i < frames; i++) {
        double srcPosF = (double)i * ratio;
        uint32_t idx0 = (uint32_t)srcPosF;
        if (idx0 > wav->frameCount - 1) idx0 = wav->frameCount - 1;
        uint32_t idx1 = idx0 + 1 < wav->frameCount ? idx0 + 1 : idx0;
        double frac = srcPosF - (double)idx0;

        for (int outCh = 0; outCh < BZ_QUEST_AUDIO_TARGET_CHANNELS; outCh++) {
            /* Mono source duplicates channel 0 into both output channels;
             * stereo source maps each output channel to its own source
             * channel directly (BZ_QUEST_WAV_MAX_CHANNELS caps channels at
             * 2, matching BZ_QUEST_AUDIO_TARGET_CHANNELS). */
            uint16_t srcCh = (wav->channels == 1) ? 0 : (uint16_t)outCh;
            int32_t s0 = wav_sample(wav, idx0, srcCh);
            int32_t s1 = wav_sample(wav, idx1, srcCh);
            double interpolated = (double)s0 + ((double)s1 - (double)s0) * frac;
            pcm[(size_t)i * BZ_QUEST_AUDIO_TARGET_CHANNELS + outCh] = clamp_i16((int32_t)llround(interpolated));
        }
    }

    *outPcm = pcm;
    *outFrames = frames;
    return true;
}

bool bz_quest_audio_mixer_submit(bzQuestAudioMixer_t *mixer, int16_t *pcm, uint32_t frameCount) {
    if (!mixer || !pcm || frameCount == 0) return false;

    for (int i = 0; i < BZ_QUEST_AUDIO_MAX_VOICES; i++) {
        bzQuestAudioVoice_t *voice = &mixer->voices[i];
        if (atomic_load_explicit(&voice->active, memory_order_acquire)) continue;

        /* A finished-but-not-yet-reaped voice may still own a previous
         * pcm buffer (see bz_quest_audio_mixer_reap()'s doc comment) -
         * this function is control-thread-only, same as reap(), so
         * freeing it here before reuse is race-free and avoids ever
         * silently leaking it if a drain step submits faster than it
         * reaps. */
        free(voice->pcm);
        voice->pcm = pcm;
        voice->frameCount = frameCount;
        voice->cursor = 0;
        atomic_store_explicit(&voice->active, true, memory_order_release);
        return true;
    }
    return false; /* every voice active - caller must free(pcm) and count a drop */
}

void bz_quest_audio_mixer_reap(bzQuestAudioMixer_t *mixer) {
    if (!mixer) return;
    for (int i = 0; i < BZ_QUEST_AUDIO_MAX_VOICES; i++) {
        bzQuestAudioVoice_t *voice = &mixer->voices[i];
        if (atomic_load_explicit(&voice->active, memory_order_acquire)) continue;
        if (voice->pcm) {
            free(voice->pcm);
            voice->pcm = NULL;
        }
    }
}

void bz_quest_audio_mixer_render(bzQuestAudioMixer_t *mixer, int16_t *out, uint32_t frameCount) {
    memset(out, 0, (size_t)frameCount * BZ_QUEST_AUDIO_TARGET_CHANNELS * sizeof(int16_t));
    if (!mixer) return;

    for (int i = 0; i < BZ_QUEST_AUDIO_MAX_VOICES; i++) {
        bzQuestAudioVoice_t *voice = &mixer->voices[i];
        if (!atomic_load_explicit(&voice->active, memory_order_acquire)) continue;

        uint32_t remaining = voice->frameCount - voice->cursor;
        uint32_t n = remaining < frameCount ? remaining : frameCount;
        for (uint32_t j = 0; j < n; j++) {
            uint32_t srcFrame = voice->cursor + j;
            for (int ch = 0; ch < BZ_QUEST_AUDIO_TARGET_CHANNELS; ch++) {
                size_t idx = (size_t)j * BZ_QUEST_AUDIO_TARGET_CHANNELS + ch;
                int32_t mixed = (int32_t)out[idx] +
                                 (int32_t)voice->pcm[(size_t)srcFrame * BZ_QUEST_AUDIO_TARGET_CHANNELS + ch];
                out[idx] = clamp_i16(mixed);
            }
        }
        voice->cursor += n;
        if (voice->cursor >= voice->frameCount)
            atomic_store_explicit(&voice->active, false, memory_order_release);
    }
}
