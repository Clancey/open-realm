/*
 * test_bz_quest_audio_mixer.c - coverage for bz_quest_audio_mixer.c's PCM
 * format conversion and bounded lock-free voice pool (layer 7). Exercises
 * bz_quest_audio_mixer_render() (the only function ever called from the
 * real AAudio RT callback thread - see bz_quest_audio_mixer.h's top
 * comment) purely on this single test thread; no separate thread is
 * needed to prove the *data* contract (mixing/clamping/cursor advance/
 * active-flag handoff), since the atomic operations themselves are
 * exercised identically regardless of which thread calls them.
 */
#include <stdlib.h>
#include <string.h>

#include "bz_quest_audio_mixer.h"
#include "test_framework.h"

/* ------------------------------------------------------------------ */
/* bz_quest_audio_mixer_convert                                        */
/* ------------------------------------------------------------------ */

static void test_convert_mono_upsamples_and_duplicates_channels(void) {
    int16_t src[4] = { 1000, 2000, 3000, 4000 };
    bzQuestWav_t wav = { .channels = 1, .sampleRate = 8000, .bitsPerSample = 16,
                          .pcm = (const uint8_t *)src, .pcmBytes = sizeof(src), .frameCount = 4 };
    int16_t *out = NULL;
    uint32_t frames = 0;
    ASSERT(bz_quest_audio_mixer_convert(&wav, 16000, &out, &frames));
    ASSERT_NOT_NULL(out);
    ASSERT_EQ_INT(frames, 8); /* exact 2x upsample */
    /* Every output frame's left/right channel must match (mono source
     * duplicated to both output channels - see header comment). */
    for (uint32_t i = 0; i < frames; i++)
        ASSERT_EQ_INT(out[i * 2 + 0], out[i * 2 + 1]);
    /* First and last samples of a linear resample must match the source
     * endpoints exactly (no extrapolation past the source's own range). */
    ASSERT_EQ_INT(out[0], 1000);
    free(out);
}

static void test_convert_downsamples(void) {
    int16_t src[8];
    for (int i = 0; i < 8; i++) src[i] = (int16_t)(i * 100);
    bzQuestWav_t wav = { .channels = 1, .sampleRate = 16000, .bitsPerSample = 16,
                          .pcm = (const uint8_t *)src, .pcmBytes = sizeof(src), .frameCount = 8 };
    int16_t *out = NULL;
    uint32_t frames = 0;
    ASSERT(bz_quest_audio_mixer_convert(&wav, 8000, &out, &frames));
    ASSERT_EQ_INT(frames, 4); /* exact 2x downsample */
    free(out);
}

static void test_convert_stereo_preserves_channels(void) {
    /* 2 stereo frames: (L=100,R=-100), (L=200,R=-200) */
    int16_t src[4] = { 100, -100, 200, -200 };
    bzQuestWav_t wav = { .channels = 2, .sampleRate = 8000, .bitsPerSample = 16,
                          .pcm = (const uint8_t *)src, .pcmBytes = sizeof(src), .frameCount = 2 };
    int16_t *out = NULL;
    uint32_t frames = 0;
    ASSERT(bz_quest_audio_mixer_convert(&wav, 8000, &out, &frames)); /* same rate: identity */
    ASSERT_EQ_INT(frames, 2);
    ASSERT_EQ_INT(out[0], 100);
    ASSERT_EQ_INT(out[1], -100);
    ASSERT_EQ_INT(out[2], 200);
    ASSERT_EQ_INT(out[3], -200);
    free(out);
}

static void test_convert_widens_8bit_unsigned_symmetrically(void) {
    uint8_t src[2] = { 0, 255 }; /* min/max unsigned 8-bit */
    bzQuestWav_t wav = { .channels = 1, .sampleRate = 8000, .bitsPerSample = 8,
                          .pcm = src, .pcmBytes = sizeof(src), .frameCount = 2 };
    int16_t *out = NULL;
    uint32_t frames = 0;
    ASSERT(bz_quest_audio_mixer_convert(&wav, 8000, &out, &frames));
    ASSERT_EQ_INT(frames, 2);
    ASSERT_EQ_INT(out[0], -32768); /* (0-128)*256 */
    ASSERT_EQ_INT(out[2], 32512);  /* (255-128)*256 */
    free(out);
}

static void test_convert_rejects_zero_target_rate(void) {
    int16_t src[2] = { 0, 0 };
    bzQuestWav_t wav = { .channels = 1, .sampleRate = 8000, .bitsPerSample = 16,
                          .pcm = (const uint8_t *)src, .pcmBytes = sizeof(src), .frameCount = 2 };
    int16_t *out = NULL;
    uint32_t frames = 0;
    ASSERT(!bz_quest_audio_mixer_convert(&wav, 0, &out, &frames));
}

static void test_convert_rejects_oversized_output(void) {
    /* A pathological sampleRate ratio (source rate far below target) must
     * be rejected before ever allocating - see BZ_QUEST_AUDIO_MAX_OUTPUT_FRAMES's
     * doc comment. Only needs 1 real source frame of backing memory since
     * the bound check runs before any sample is read. */
    int16_t src[1] = { 0 };
    bzQuestWav_t wav = { .channels = 1, .sampleRate = 1, .bitsPerSample = 16,
                          .pcm = (const uint8_t *)src, .pcmBytes = sizeof(src), .frameCount = 1 };
    int16_t *out = NULL;
    uint32_t frames = 0;
    ASSERT(!bz_quest_audio_mixer_convert(&wav, 100000000u, &out, &frames));
}

static void test_convert_rejects_null_args(void) {
    int16_t *out = NULL;
    uint32_t frames = 0;
    ASSERT(!bz_quest_audio_mixer_convert(NULL, 8000, &out, &frames));
    bzQuestWav_t wav = { .channels = 1, .sampleRate = 8000, .bitsPerSample = 16, .frameCount = 1 };
    ASSERT(!bz_quest_audio_mixer_convert(&wav, 8000, NULL, &frames));
    ASSERT(!bz_quest_audio_mixer_convert(&wav, 8000, &out, NULL));
}

/* ------------------------------------------------------------------ */
/* bz_quest_audio_mixer_submit / _render / _reap                       */
/* ------------------------------------------------------------------ */

static int16_t *make_pcm(uint32_t frames, int16_t l, int16_t r) {
    int16_t *pcm = malloc((size_t)frames * BZ_QUEST_AUDIO_TARGET_CHANNELS * sizeof(int16_t));
    for (uint32_t i = 0; i < frames; i++) {
        pcm[i * 2 + 0] = l;
        pcm[i * 2 + 1] = r;
    }
    return pcm;
}

static void test_submit_rejects_null_or_zero_frames(void) {
    bzQuestAudioMixer_t mixer;
    bz_quest_audio_mixer_init(&mixer);
    ASSERT(!bz_quest_audio_mixer_submit(&mixer, NULL, 4));
    int16_t *pcm = make_pcm(4, 1, 1);
    ASSERT(!bz_quest_audio_mixer_submit(&mixer, pcm, 0));
    free(pcm);
}

static void test_render_mixes_single_voice_across_two_calls(void) {
    bzQuestAudioMixer_t mixer;
    bz_quest_audio_mixer_init(&mixer);
    int16_t *pcm = make_pcm(4, 500, -500);
    ASSERT(bz_quest_audio_mixer_submit(&mixer, pcm, 4));

    int16_t out[2 * BZ_QUEST_AUDIO_TARGET_CHANNELS];
    bz_quest_audio_mixer_render(&mixer, out, 2); /* first half */
    ASSERT_EQ_INT(out[0], 500);
    ASSERT_EQ_INT(out[1], -500);
    ASSERT(atomic_load_explicit(&mixer.voices[0].active, memory_order_relaxed)); /* not finished yet */

    bz_quest_audio_mixer_render(&mixer, out, 2); /* second half - exhausts the voice */
    ASSERT_EQ_INT(out[2], 500);
    ASSERT(!atomic_load_explicit(&mixer.voices[0].active, memory_order_relaxed));
}

static void test_render_zeroes_output_with_no_active_voices(void) {
    bzQuestAudioMixer_t mixer;
    bz_quest_audio_mixer_init(&mixer);
    int16_t out[4 * BZ_QUEST_AUDIO_TARGET_CHANNELS];
    memset(out, 0xAA, sizeof(out));
    bz_quest_audio_mixer_render(&mixer, out, 4);
    for (size_t i = 0; i < 4 * BZ_QUEST_AUDIO_TARGET_CHANNELS; i++)
        ASSERT_EQ_INT(out[i], 0);
}

static void test_render_clamps_overlapping_voices(void) {
    bzQuestAudioMixer_t mixer;
    bz_quest_audio_mixer_init(&mixer);
    ASSERT(bz_quest_audio_mixer_submit(&mixer, make_pcm(2, 30000, 30000), 2));
    ASSERT(bz_quest_audio_mixer_submit(&mixer, make_pcm(2, 30000, 30000), 2));

    int16_t out[2 * BZ_QUEST_AUDIO_TARGET_CHANNELS];
    bz_quest_audio_mixer_render(&mixer, out, 2);
    /* 30000+30000 = 60000, must saturate to INT16_MAX, never wrap negative. */
    ASSERT_EQ_INT(out[0], 32767);
    ASSERT_EQ_INT(out[1], 32767);
}

static void test_submit_fails_when_every_voice_active(void) {
    bzQuestAudioMixer_t mixer;
    bz_quest_audio_mixer_init(&mixer);
    for (int i = 0; i < BZ_QUEST_AUDIO_MAX_VOICES; i++)
        ASSERT(bz_quest_audio_mixer_submit(&mixer, make_pcm(4, 1, 1), 4));

    int16_t *pcm = make_pcm(4, 1, 1);
    ASSERT(!bz_quest_audio_mixer_submit(&mixer, pcm, 4));
    free(pcm); /* caller retains ownership on a rejected submit - see header comment */
}

static void test_reap_frees_finished_voice(void) {
    bzQuestAudioMixer_t mixer;
    bz_quest_audio_mixer_init(&mixer);
    ASSERT(bz_quest_audio_mixer_submit(&mixer, make_pcm(1, 1, 1), 1));
    int16_t out[1 * BZ_QUEST_AUDIO_TARGET_CHANNELS];
    bz_quest_audio_mixer_render(&mixer, out, 1); /* exhausts the 1-frame voice */
    ASSERT(!atomic_load_explicit(&mixer.voices[0].active, memory_order_relaxed));
    ASSERT_NOT_NULL(mixer.voices[0].pcm);
    bz_quest_audio_mixer_reap(&mixer);
    ASSERT_NULL(mixer.voices[0].pcm);
}

static void test_submit_reuses_slot_after_reap(void) {
    bzQuestAudioMixer_t mixer;
    bz_quest_audio_mixer_init(&mixer);
    for (int i = 0; i < BZ_QUEST_AUDIO_MAX_VOICES; i++)
        ASSERT(bz_quest_audio_mixer_submit(&mixer, make_pcm(1, 1, 1), 1));

    int16_t out[1 * BZ_QUEST_AUDIO_TARGET_CHANNELS];
    bz_quest_audio_mixer_render(&mixer, out, 1); /* exhausts every 1-frame voice at once */
    bz_quest_audio_mixer_reap(&mixer);

    int16_t *pcm = make_pcm(2, 7, 7);
    ASSERT(bz_quest_audio_mixer_submit(&mixer, pcm, 2)); /* a slot is free again */
}

void run_bz_quest_audio_mixer_tests(void) {
    RUN_TEST(test_convert_mono_upsamples_and_duplicates_channels);
    RUN_TEST(test_convert_downsamples);
    RUN_TEST(test_convert_stereo_preserves_channels);
    RUN_TEST(test_convert_widens_8bit_unsigned_symmetrically);
    RUN_TEST(test_convert_rejects_zero_target_rate);
    RUN_TEST(test_convert_rejects_oversized_output);
    RUN_TEST(test_convert_rejects_null_args);
    RUN_TEST(test_submit_rejects_null_or_zero_frames);
    RUN_TEST(test_render_mixes_single_voice_across_two_calls);
    RUN_TEST(test_render_zeroes_output_with_no_active_voices);
    RUN_TEST(test_render_clamps_overlapping_voices);
    RUN_TEST(test_submit_fails_when_every_voice_active);
    RUN_TEST(test_reap_frees_finished_voice);
    RUN_TEST(test_submit_reuses_slot_after_reap);
}
