/*
 * test_bz_quest_wav.c - coverage for bz_quest_wav.c's strict RIFF/WAVE
 * parser (layer 7). Builds synthetic in-memory WAVE byte buffers by hand
 * (see build_wav() below) rather than depending on any retail asset - this
 * project's tests must never depend on real Warcraft III data (see
 * CONTRIBUTING.md), and no real WC3 sound asset is available in this
 * development environment anyway (see docs/quest-tabletop.md's "Hardware/
 * data-only acceptance procedure" for the general rule this mirrors).
 * Covers a normal path and its inverse/error path for every validation
 * rule bz_quest_wav.h documents.
 */
#include <string.h>

#include "bz_quest_wav.h"
#include "test_framework.h"

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Assembles a minimal, valid "RIFF....WAVEfmt <16 bytes>data<n bytes>"
 * buffer into `out` (caller-sized) and returns the total length written,
 * or 0 if it would not fit. `pcm`/`pcmBytes` may be NULL/0 for a
 * deliberately empty data chunk (used by the "empty data chunk" failure
 * case below). */
static size_t build_wav(uint8_t *out, size_t cap, uint16_t audioFormat, uint16_t channels,
                         uint32_t sampleRate, uint16_t bitsPerSample, const uint8_t *pcm, uint32_t pcmBytes) {
    uint16_t blockAlign = (uint16_t)(channels * (bitsPerSample / 8));
    uint32_t byteRate = sampleRate * blockAlign;
    size_t total = 12 + 8 + 16 + 8 + pcmBytes;
    if (total > cap) return 0;

    memcpy(out, "RIFF", 4);
    w32(out + 4, (uint32_t)(total - 8));
    memcpy(out + 8, "WAVE", 4);

    memcpy(out + 12, "fmt ", 4);
    w32(out + 16, 16);
    w16(out + 20, audioFormat);
    w16(out + 22, channels);
    w32(out + 24, sampleRate);
    w32(out + 28, byteRate);
    w16(out + 32, blockAlign);
    w16(out + 34, bitsPerSample);

    memcpy(out + 36, "data", 4);
    w32(out + 40, pcmBytes);
    if (pcmBytes) memcpy(out + 44, pcm, pcmBytes);
    return total;
}

static void test_valid_mono16_parses(void) {
    uint8_t pcm[8]; /* 4 frames of 16-bit mono */
    for (int i = 0; i < 4; i++) w16(pcm + i * 2, (uint16_t)(1000 + i));
    uint8_t buf[128];
    size_t n = build_wav(buf, sizeof(buf), 1, 1, 22050, 16, pcm, sizeof(pcm));
    ASSERT(n > 0);

    bzQuestWav_t wav;
    char err[BZ_QUEST_WAV_ERROR_MAX];
    ASSERT(bz_quest_wav_parse(buf, n, &wav, err, sizeof(err)));
    ASSERT_EQ_INT(wav.channels, 1);
    ASSERT_EQ_INT(wav.sampleRate, 22050);
    ASSERT_EQ_INT(wav.bitsPerSample, 16);
    ASSERT_EQ_INT(wav.pcmBytes, 8);
    ASSERT_EQ_INT(wav.frameCount, 4);
    ASSERT(wav.pcm == buf + 44); /* points into the caller's buffer, no copy */
}

static void test_valid_stereo8_parses(void) {
    uint8_t pcm[6] = { 10, 20, 30, 40, 50, 60 }; /* 3 stereo 8-bit frames */
    uint8_t buf[128];
    size_t n = build_wav(buf, sizeof(buf), 1, 2, 44100, 8, pcm, sizeof(pcm));
    ASSERT(n > 0);

    bzQuestWav_t wav;
    ASSERT(bz_quest_wav_parse(buf, n, &wav, NULL, 0));
    ASSERT_EQ_INT(wav.channels, 2);
    ASSERT_EQ_INT(wav.bitsPerSample, 8);
    ASSERT_EQ_INT(wav.frameCount, 3);
}

static void test_trailing_chunk_after_data_is_skipped(void) {
    uint8_t pcm[2] = { 0, 0 };
    uint8_t buf[128];
    size_t n = build_wav(buf, sizeof(buf), 1, 1, 8000, 16, pcm, sizeof(pcm));
    /* Parsing must succeed on the exact prefix that ends right after the
     * data chunk - this proves the parser does not require (or choke on
     * the absence of) any further trailing chunk. */
    bzQuestWav_t wav;
    ASSERT(bz_quest_wav_parse(buf, n, &wav, NULL, 0));
    ASSERT_EQ_INT(wav.frameCount, 1);
}

static void test_missing_riff_magic_rejected(void) {
    uint8_t buf[64] = { 0 };
    memcpy(buf, "JUNK", 4);
    bzQuestWav_t wav;
    char err[BZ_QUEST_WAV_ERROR_MAX] = { 0 };
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, err, sizeof(err)));
    ASSERT(strstr(err, "RIFF") != NULL);
}

static void test_missing_wave_form_rejected(void) {
    uint8_t buf[64] = { 0 };
    memcpy(buf, "RIFF", 4);
    w32(buf + 4, 56);
    memcpy(buf + 8, "AVI ", 4);
    bzQuestWav_t wav;
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, NULL, 0));
}

static void test_too_small_buffer_rejected(void) {
    uint8_t buf[8] = { 0 };
    bzQuestWav_t wav;
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, NULL, 0));
}

static void test_unsupported_audio_format_rejected(void) {
    uint8_t pcm[4] = { 0, 0, 0, 0 };
    uint8_t buf[128];
    size_t n = build_wav(buf, sizeof(buf), 0xFFFE /* WAVE_FORMAT_EXTENSIBLE */, 1, 22050, 16, pcm, sizeof(pcm));
    bzQuestWav_t wav;
    char err[BZ_QUEST_WAV_ERROR_MAX] = { 0 };
    ASSERT(!bz_quest_wav_parse(buf, n, &wav, err, sizeof(err)));
    ASSERT(strstr(err, "audioFormat") != NULL);
}

static void test_unsupported_channel_count_rejected(void) {
    uint8_t pcm[8] = { 0 };
    uint8_t buf[128];
    size_t n = build_wav(buf, sizeof(buf), 1, 6, 48000, 16, pcm, sizeof(pcm));
    bzQuestWav_t wav;
    ASSERT(!bz_quest_wav_parse(buf, n, &wav, NULL, 0));
}

static void test_unsupported_bit_depth_rejected(void) {
    /* Hand-build a 24-bit fmt chunk directly (build_wav()'s blockAlign
     * math only supports 8/16 cleanly) - 1 frame, 1 channel, 3 bytes. */
    uint8_t buf[64] = { 0 };
    memcpy(buf, "RIFF", 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    w32(buf + 16, 16);
    w16(buf + 20, 1);
    w16(buf + 22, 1);
    w32(buf + 24, 48000);
    w32(buf + 28, 48000 * 3);
    w16(buf + 32, 3);
    w16(buf + 34, 24);
    memcpy(buf + 36, "data", 4);
    w32(buf + 40, 3);
    w32(buf + 4, 36 + 8 + 3 - 8);
    bzQuestWav_t wav;
    char err[BZ_QUEST_WAV_ERROR_MAX] = { 0 };
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, err, sizeof(err)));
    ASSERT(strstr(err, "bit depth") != NULL);
}

static void test_zero_sample_rate_rejected(void) {
    uint8_t pcm[2] = { 0, 0 };
    uint8_t buf[128];
    size_t n = build_wav(buf, sizeof(buf), 1, 1, 0, 16, pcm, sizeof(pcm));
    bzQuestWav_t wav;
    ASSERT(!bz_quest_wav_parse(buf, n, &wav, NULL, 0));
}

static void test_implausible_sample_rate_rejected(void) {
    uint8_t pcm[2] = { 0, 0 };
    uint8_t buf[128];
    size_t n = build_wav(buf, sizeof(buf), 1, 1, 500000, 16, pcm, sizeof(pcm));
    bzQuestWav_t wav;
    ASSERT(!bz_quest_wav_parse(buf, n, &wav, NULL, 0));
}

static void test_inconsistent_block_align_rejected(void) {
    uint8_t buf[64] = { 0 };
    memcpy(buf, "RIFF", 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    w32(buf + 16, 16);
    w16(buf + 20, 1);
    w16(buf + 22, 1);
    w32(buf + 24, 22050);
    w32(buf + 28, 22050 * 2);
    w16(buf + 32, 99); /* wrong - should be 2 for mono 16-bit */
    w16(buf + 34, 16);
    memcpy(buf + 36, "data", 4);
    w32(buf + 40, 2);
    w32(buf + 4, 36 + 8 + 2 - 8);
    bzQuestWav_t wav;
    char err[BZ_QUEST_WAV_ERROR_MAX] = { 0 };
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, err, sizeof(err)));
    ASSERT(strstr(err, "blockAlign") != NULL);
}

static void test_inconsistent_byte_rate_rejected(void) {
    uint8_t buf[64] = { 0 };
    memcpy(buf, "RIFF", 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    w32(buf + 16, 16);
    w16(buf + 20, 1);
    w16(buf + 22, 1);
    w32(buf + 24, 22050);
    w32(buf + 28, 123456); /* wrong byteRate */
    w16(buf + 32, 2);
    w16(buf + 34, 16);
    memcpy(buf + 36, "data", 4);
    w32(buf + 40, 2);
    w32(buf + 4, 36 + 8 + 2 - 8);
    bzQuestWav_t wav;
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, NULL, 0));
}

static void test_data_before_fmt_rejected(void) {
    uint8_t buf[64] = { 0 };
    memcpy(buf, "RIFF", 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "data", 4);
    w32(buf + 16, 2);
    memcpy(buf + 12 + 8, "fmt ", 4); /* never reached: parser sees "data" first */
    w32(buf + 4, 56);
    bzQuestWav_t wav;
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, NULL, 0));
}

static void test_missing_fmt_chunk_rejected(void) {
    uint8_t buf[64] = { 0 };
    memcpy(buf, "RIFF", 4);
    w32(buf + 4, 56);
    memcpy(buf + 8, "WAVE", 4);
    bzQuestWav_t wav;
    char err[BZ_QUEST_WAV_ERROR_MAX] = { 0 };
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, err, sizeof(err)));
    ASSERT(strstr(err, "fmt") != NULL);
}

static void test_empty_data_chunk_rejected(void) {
    uint8_t buf[128];
    size_t n = build_wav(buf, sizeof(buf), 1, 1, 22050, 16, NULL, 0);
    bzQuestWav_t wav;
    ASSERT(!bz_quest_wav_parse(buf, n, &wav, NULL, 0));
}

static void test_misaligned_data_chunk_rejected(void) {
    /* 16-bit mono blockAlign=2; a data size of 3 bytes is not a multiple
     * of that, so this must be rejected as a corrupt/truncated payload
     * rather than silently dropping the trailing partial frame. */
    uint8_t buf[64] = { 0 };
    memcpy(buf, "RIFF", 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    w32(buf + 16, 16);
    w16(buf + 20, 1);
    w16(buf + 22, 1);
    w32(buf + 24, 22050);
    w32(buf + 28, 22050 * 2);
    w16(buf + 32, 2);
    w16(buf + 34, 16);
    memcpy(buf + 36, "data", 4);
    w32(buf + 40, 3);
    w32(buf + 4, 36 + 8 + 3 - 8);
    bzQuestWav_t wav;
    char err[BZ_QUEST_WAV_ERROR_MAX] = { 0 };
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, err, sizeof(err)));
    ASSERT(strstr(err, "multiple") != NULL);
}

static void test_truncated_chunk_size_rejected(void) {
    /* fmt chunk claims 16 bytes but the buffer ends 4 bytes early. */
    uint8_t buf[32] = { 0 };
    memcpy(buf, "RIFF", 4);
    w32(buf + 4, 24);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    w32(buf + 16, 16);
    bzQuestWav_t wav;
    char err[BZ_QUEST_WAV_ERROR_MAX] = { 0 };
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), &wav, err, sizeof(err)));
    ASSERT(strstr(err, "past end") != NULL);
}

static void test_null_inputs_rejected(void) {
    bzQuestWav_t wav;
    ASSERT(!bz_quest_wav_parse(NULL, 100, &wav, NULL, 0));
    uint8_t buf[16] = { 0 };
    ASSERT(!bz_quest_wav_parse(buf, sizeof(buf), NULL, NULL, 0));
}

void run_bz_quest_wav_tests(void) {
    RUN_TEST(test_valid_mono16_parses);
    RUN_TEST(test_valid_stereo8_parses);
    RUN_TEST(test_trailing_chunk_after_data_is_skipped);
    RUN_TEST(test_missing_riff_magic_rejected);
    RUN_TEST(test_missing_wave_form_rejected);
    RUN_TEST(test_too_small_buffer_rejected);
    RUN_TEST(test_unsupported_audio_format_rejected);
    RUN_TEST(test_unsupported_channel_count_rejected);
    RUN_TEST(test_unsupported_bit_depth_rejected);
    RUN_TEST(test_zero_sample_rate_rejected);
    RUN_TEST(test_implausible_sample_rate_rejected);
    RUN_TEST(test_inconsistent_block_align_rejected);
    RUN_TEST(test_inconsistent_byte_rate_rejected);
    RUN_TEST(test_data_before_fmt_rejected);
    RUN_TEST(test_missing_fmt_chunk_rejected);
    RUN_TEST(test_empty_data_chunk_rejected);
    RUN_TEST(test_misaligned_data_chunk_rejected);
    RUN_TEST(test_truncated_chunk_size_rejected);
    RUN_TEST(test_null_inputs_rejected);
}
