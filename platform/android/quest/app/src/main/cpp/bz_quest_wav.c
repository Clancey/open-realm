/*
 * bz_quest_wav.c - see bz_quest_wav.h.
 */
#include "bz_quest_wav.h"

#include <stdio.h>
#include <string.h>

/* Little-endian reads - RIFF/WAVE is always little-endian regardless of
 * host byte order (matches common/mpq.c's own explicit little-endian
 * reads for the same reason: never rely on struct-overlay + host
 * endianness). Each takes an already-bounds-checked offset. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool fail(char *outError, size_t errorCap, const char *msg) {
    if (outError && errorCap) snprintf(outError, errorCap, "%s", msg);
    return false;
}

bool bz_quest_wav_parse(const uint8_t *bytes, size_t size, bzQuestWav_t *out, char *outError,
                         size_t errorCap) {
    if (!bytes || !out) return fail(outError, errorCap, "null buffer");
    /* "RIFF" + size(4) + "WAVE" = 12 bytes minimum before any chunk. */
    if (size < 12) return fail(outError, errorCap, "buffer too small for a RIFF header");
    if (memcmp(bytes, "RIFF", 4) != 0) return fail(outError, errorCap, "missing 'RIFF' magic");
    if (memcmp(bytes + 8, "WAVE", 4) != 0) return fail(outError, errorCap, "missing 'WAVE' form type");

    bool haveFmt = false;
    uint16_t audioFormat = 0, channels = 0, bitsPerSample = 0, blockAlign = 0;
    uint32_t sampleRate = 0, byteRate = 0;
    const uint8_t *dataPtr = NULL;
    uint32_t dataSize = 0;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t *chunkId = bytes + pos;
        uint32_t chunkSize = rd32(bytes + pos + 4);
        size_t chunkDataOffset = pos + 8;
        /* Bounds check BEFORE trusting chunkSize for anything else - a
         * corrupt/truncated chunk size must fail here, never read past
         * `size`. */
        if (chunkSize > size - chunkDataOffset)
            return fail(outError, errorCap, "chunk size runs past end of buffer");

        if (memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) return fail(outError, errorCap, "'fmt ' chunk smaller than 16 bytes");
            audioFormat = rd16(bytes + chunkDataOffset + 0);
            channels = rd16(bytes + chunkDataOffset + 2);
            sampleRate = rd32(bytes + chunkDataOffset + 4);
            byteRate = rd32(bytes + chunkDataOffset + 8);
            blockAlign = rd16(bytes + chunkDataOffset + 12);
            bitsPerSample = rd16(bytes + chunkDataOffset + 14);

            if (audioFormat != 1)
                return fail(outError, errorCap,
                             "unsupported WAVE audioFormat (only integer PCM 1 is supported)");
            if (channels < 1 || channels > BZ_QUEST_WAV_MAX_CHANNELS)
                return fail(outError, errorCap, "unsupported channel count (only mono/stereo supported)");
            if (bitsPerSample != 8 && bitsPerSample != 16)
                return fail(outError, errorCap, "unsupported bit depth (only 8/16-bit PCM supported)");
            if (sampleRate == 0 || sampleRate > BZ_QUEST_WAV_MAX_SAMPLE_RATE)
                return fail(outError, errorCap, "sampleRate is zero or implausibly large");
            uint16_t expectedBlockAlign = (uint16_t)(channels * (bitsPerSample / 8));
            if (blockAlign != expectedBlockAlign)
                return fail(outError, errorCap, "blockAlign is inconsistent with channels/bitsPerSample");
            uint32_t expectedByteRate = sampleRate * (uint32_t)expectedBlockAlign;
            if (byteRate != expectedByteRate)
                return fail(outError, errorCap, "byteRate is inconsistent with sampleRate/blockAlign");
            haveFmt = true;
        } else if (memcmp(chunkId, "data", 4) == 0) {
            if (!haveFmt)
                return fail(outError, errorCap, "'data' chunk appeared before 'fmt ' chunk");
            dataPtr = bytes + chunkDataOffset;
            dataSize = chunkSize;
            /* The one chunk this parser actually needs has been found -
             * stop walking (a WAVE file may have trailing chunks like a
             * cue/LIST metadata block after "data" that this parser never
             * needs to understand). */
            break;
        }
        /* Skip any other chunk (or "fmt " itself, already consumed above)
         * by its declared size, honoring RIFF's mandatory pad byte after
         * an odd-sized chunk so the next chunk header is correctly
         * aligned. */
        pos = chunkDataOffset + chunkSize + (chunkSize & 1);
    }

    if (!haveFmt) return fail(outError, errorCap, "no 'fmt ' chunk found");
    if (!dataPtr) return fail(outError, errorCap, "no 'data' chunk found");
    if (dataSize == 0) return fail(outError, errorCap, "'data' chunk is empty");
    if (dataSize % blockAlign != 0)
        return fail(outError, errorCap, "'data' chunk size is not a multiple of the frame size");

    out->channels = channels;
    out->sampleRate = sampleRate;
    out->bitsPerSample = bitsPerSample;
    out->pcm = dataPtr;
    out->pcmBytes = dataSize;
    out->frameCount = dataSize / blockAlign;
    return true;
}
