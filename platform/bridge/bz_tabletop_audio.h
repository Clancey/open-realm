#ifndef BZ_TABLETOP_AUDIO_H
#define BZ_TABLETOP_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BZ_TABLETOP_AUDIO_ABI_VERSION 1u

enum {
    BZ_TT_AUDIO_QUEUE_CAPACITY = 32,
    BZ_TT_AUDIO_MAX_BYTES = 1024 * 1024,
};

typedef enum {
    BZ_TT_AUDIO_DEVICE = 0,
    BZ_TT_AUDIO_DUMMY,
} bzTTAudioMode_t;

bool BZ_TTAudio_Configure(uint32_t abi_version, bzTTAudioMode_t mode);
bool BZ_TTAudio_Dequeue(uint32_t abi_version, void *bytes, size_t capacity, size_t *size);
uint32_t BZ_TTAudio_DroppedCount(uint32_t abi_version);

#ifdef __cplusplus
}
#endif

#endif
