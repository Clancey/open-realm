/*
 * Native-host sound handoff for the shared tabletop client (see
 * platform/tabletop/client), linked by every native host (visionOS
 * today; Android/Meta Quest later). The engine remains the owner of
 * event/path resolution; copied WAV payloads cross a bounded C queue to
 * whichever platform audio sink (AVFoundation, AAudio, ...) the host
 * drains BZ_TTAudio_Dequeue() from.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "platform/bridge/bz_tabletop_audio.h"

typedef struct {
    LPBYTE bytes;
    size_t size;
} bzTTAudioItem_t;

static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static bzTTAudioItem_t audio_queue[BZ_TT_AUDIO_QUEUE_CAPACITY];
static unsigned audio_read, audio_count;
static uint32_t audio_dropped;
static bzTTAudioMode_t audio_mode = BZ_TT_AUDIO_DUMMY;

/* Clears retained payloads whenever a host changes or shuts down its sink. */
static void S_ClearQueue(void) {
    pthread_mutex_lock(&audio_lock);
    while (audio_count) {
        free(audio_queue[audio_read].bytes);
        audio_queue[audio_read] = (bzTTAudioItem_t){ 0 };
        audio_read = (audio_read + 1) % BZ_TT_AUDIO_QUEUE_CAPACITY;
        audio_count--;
    }
    audio_read = 0;
    pthread_mutex_unlock(&audio_lock);
}

bool BZ_TTAudio_Configure(uint32_t abi_version, bzTTAudioMode_t mode) {
    if (abi_version != BZ_TABLETOP_AUDIO_ABI_VERSION ||
        (mode != BZ_TT_AUDIO_DEVICE && mode != BZ_TT_AUDIO_DUMMY))
        return false;
    S_ClearQueue();
    pthread_mutex_lock(&audio_lock);
    audio_mode = mode;
    audio_dropped = 0;
    pthread_mutex_unlock(&audio_lock);
    fprintf(stderr, "S_Init: native tabletop audio sink is %s\n",
            mode == BZ_TT_AUDIO_DEVICE ? "the platform device sink" : "explicit simulator dummy");
    return true;
}

bool BZ_TTAudio_Dequeue(uint32_t abi_version, void *bytes, size_t capacity, size_t *size) {
    if (size)
        *size = 0;
    if (abi_version != BZ_TABLETOP_AUDIO_ABI_VERSION || !bytes || !size)
        return false;
    pthread_mutex_lock(&audio_lock);
    if (!audio_count || capacity < audio_queue[audio_read].size) {
        pthread_mutex_unlock(&audio_lock);
        return false;
    }
    *size = audio_queue[audio_read].size;
    memcpy(bytes, audio_queue[audio_read].bytes, *size);
    free(audio_queue[audio_read].bytes);
    audio_queue[audio_read] = (bzTTAudioItem_t){ 0 };
    audio_read = (audio_read + 1) % BZ_TT_AUDIO_QUEUE_CAPACITY;
    audio_count--;
    pthread_mutex_unlock(&audio_lock);
    return true;
}

uint32_t BZ_TTAudio_DroppedCount(uint32_t abi_version) {
    if (abi_version != BZ_TABLETOP_AUDIO_ABI_VERSION)
        return 0;
    pthread_mutex_lock(&audio_lock);
    uint32_t count = audio_dropped;
    pthread_mutex_unlock(&audio_lock);
    return count;
}

BOOL S_Init(void) { return true; }
void S_Shutdown(void) { S_ClearQueue(); }

void S_PlaySoundFile(LPCSTR path) {
    DWORD size = 0;
    LPBYTE source;
    static LPCSTR last_missing;
    static BOOL warned_full, warned_large;

    if (!path || !*path)
        return;
    pthread_mutex_lock(&audio_lock);
    if (audio_mode == BZ_TT_AUDIO_DUMMY) {
        audio_dropped++;
        pthread_mutex_unlock(&audio_lock);
        return;
    }
    pthread_mutex_unlock(&audio_lock);
    source = FS_ReadFile(path, &size);
    if (!source) {
        if (path != last_missing)
            fprintf(stderr, "S_PlaySoundFile: missing '%s'\n", path);
        last_missing = path;
        return;
    }
    if (!size || size > BZ_TT_AUDIO_MAX_BYTES) {
        if (!warned_large)
            fprintf(stderr, "S_PlaySoundFile: payload exceeds native audio bound (%u bytes)\n", (unsigned)size);
        warned_large = true;
        FS_FreeFile(source);
        return;
    }
    LPBYTE copy = malloc(size);
    if (!copy) {
        fprintf(stderr, "S_PlaySoundFile: out of memory copying '%s'\n", path);
        FS_FreeFile(source);
        return;
    }
    memcpy(copy, source, size);
    FS_FreeFile(source);
    pthread_mutex_lock(&audio_lock);
    if (audio_count == BZ_TT_AUDIO_QUEUE_CAPACITY) {
        audio_dropped++;
        pthread_mutex_unlock(&audio_lock);
        free(copy);
        if (!warned_full)
            fprintf(stderr, "S_PlaySoundFile: native audio queue full; dropping events\n");
        warned_full = true;
        return;
    }
    unsigned write = (audio_read + audio_count) % BZ_TT_AUDIO_QUEUE_CAPACITY;
    audio_queue[write] = (bzTTAudioItem_t){ copy, size };
    audio_count++;
    pthread_mutex_unlock(&audio_lock);
}

void S_PlaySound(DWORD kit_id) {
    static BOOL warned;
    if (!warned)
        fprintf(stderr, "S_PlaySound: sound-kit playback is not supported by the native sink (kit %u)\n",
                (unsigned)kit_id);
    warned = true;
}

void S_PlaySoundByName(LPCSTR name) {
    static BOOL warned;
    if (!warned)
        fprintf(stderr, "S_PlaySoundByName: named sound-kit playback is unsupported ('%s')\n", name ? name : "");
    warned = true;
}
