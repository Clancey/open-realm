/*
 * bz_quest_wav.h - pure, host-testable RIFF/WAVE parser for the Quest
 * native audio sink (layer 7).
 *
 * `S_PlaySoundFile()` (platform/tabletop/client/s_tabletop_null.c) copies
 * an entire on-disk file's bytes verbatim into the bounded
 * BZ_TT_AUDIO_QUEUE_CAPACITY queue (platform/bridge/bz_tabletop_audio.h) -
 * it never inspects or decodes the payload. Every Warcraft III sound asset
 * this engine ever loads via FS_ReadFile() is a `.wav` file (see
 * games/warcraft-3/docs/sounds.md's SLK-driven sound catalog - every
 * `FileNames`/`{ModelName}Death.wav` entry is a WAVE file), so this parser
 * only ever needs to understand RIFF/WAVE, never any other container.
 *
 * This module is deliberately plain C11 with no Android/AAudio/heap-owning
 * dependency: it parses *in place* over a caller-owned byte buffer (no
 * copy, no allocation) and is covered by platform/android/quest/tests/
 * test_bz_quest_wav.c on the host with a plain C compiler - exactly the
 * project's existing "isolate a pure host-testable module, keep the
 * Android-specific consumer separate" convention (see bz_quest_data.h/
 * bz_quest_frame.h's own header comments).
 *
 * Explicitly supported (strict, not "best effort"):
 *   - "RIFF"...."WAVE" container, "fmt " chunk before "data" (the only
 *     order real WAVE writers ever produce - a "data" chunk with no prior
 *     "fmt " chunk is rejected as corrupt, not guessed at).
 *   - audioFormat == 1 (integer PCM) only. WAVE_FORMAT_EXTENSIBLE (0xFFFE),
 *     ADPCM, MP3-in-WAV, or any other tag is a hard, explicit rejection -
 *     never a silent "assume PCM and hope" fallback.
 *   - 1 or 2 channels (mono/stereo - every retail WC3 sound asset is one of
 *     these two), 8-bit unsigned or 16-bit signed PCM samples.
 *   - blockAlign/byteRate must be internally consistent with
 *     channels/bitsPerSample/sampleRate (a writer that got these wrong
 *     produced a corrupt file by this parser's contract, not a merely
 *     "unusual" one - see bz_quest_wav_parse()'s doc comment).
 *
 * Anything else (24/32-bit PCM, float PCM, >2 channels, a compressed
 * format, a truncated/misaligned/oversized chunk) is a hard parse failure
 * with a human-readable *outError - never a silent skip/substitution. The
 * one real caller (bz_quest_audio.c's drain step) logs that failure once
 * per queued item and drops it, matching this project's "missing asset"
 * once-per-unique-condition logging convention (see AGENTS.md's "Missing
 * Asset Placeholders" - the same principle applied to audio instead of
 * textures/models).
 */
#ifndef BZ_QUEST_WAV_H
#define BZ_QUEST_WAV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_WAV_ERROR_MAX = 160,
    BZ_QUEST_WAV_MAX_CHANNELS = 2,
    /* Generous sanity bound (not a real-world WAVE limit) so a corrupt
     * header's sampleRate field can never be treated as "plausible" -
     * every real WC3 asset is 8000-44100 Hz. */
    BZ_QUEST_WAV_MAX_SAMPLE_RATE = 192000,
};

/*
 * Result of a successful parse. `pcm` points *into* the caller's own
 * `bytes` buffer passed to bz_quest_wav_parse() - this struct never owns
 * memory and must not outlive that buffer. `frameCount` is
 * `pcmBytes / (channels * bitsPerSample/8)` (exact - see
 * bz_quest_wav_parse()'s data-chunk alignment check, which rejects any
 * `data` chunk whose size is not itself an exact multiple of the frame
 * size before this struct is ever populated).
 */
typedef struct {
    uint16_t channels;      /* 1 or 2 */
    uint32_t sampleRate;    /* Hz, 1..BZ_QUEST_WAV_MAX_SAMPLE_RATE */
    uint16_t bitsPerSample; /* 8 or 16 */
    const uint8_t *pcm;     /* into `bytes`, not owned */
    uint32_t pcmBytes;
    uint32_t frameCount;
} bzQuestWav_t;

/*
 * Parses `bytes[0..size)` as a RIFF/WAVE container per this header's top
 * comment. Walks every chunk (skipping any chunk that is not "fmt "/"data"
 * by its declared size, honoring RIFF's mandatory even-alignment pad byte
 * after an odd-sized chunk) with an explicit bounds check against `size`
 * before every read - a chunk size field that would read past the end of
 * `bytes` is a parse failure ("truncated/corrupt file"), never an
 * out-of-bounds read. Requires a "fmt " chunk (>=16 bytes) strictly before
 * any "data" chunk; validates audioFormat/channels/bitsPerSample per this
 * header's top comment, AND that blockAlign == channels*bitsPerSample/8
 * and byteRate == sampleRate*blockAlign (an inconsistent header is treated
 * as corrupt, matching this parser's "strict", not "lenient", contract).
 * Requires the eventual "data" chunk's size to be both fully in-bounds and
 * an exact multiple of blockAlign, and non-zero.
 *
 * Returns true with *out populated on success. Returns false (leaves *out
 * unspecified) with a NUL-terminated, human-readable reason copied into
 * outError (if outError/errorCap are non-NULL/non-zero) on ANY failure -
 * missing RIFF/WAVE magic, missing "fmt "/"data" chunk, unsupported
 * audioFormat/channels/bitsPerSample/sampleRate, inconsistent
 * blockAlign/byteRate, or a chunk that does not fit within `size`.
 */
bool bz_quest_wav_parse(const uint8_t *bytes, size_t size, bzQuestWav_t *out, char *outError,
                         size_t errorCap);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WAV_H */
