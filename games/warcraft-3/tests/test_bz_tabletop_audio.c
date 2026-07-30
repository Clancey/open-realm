#include "test_framework.h"
#include "platform/bridge/bz_tabletop_audio.h"
#include "common/common.h"

#include <stdlib.h>
#include <string.h>

void S_PlaySoundFile(LPCSTR path);
void S_Shutdown(void);

HANDLE FS_ReadFile(LPCSTR path, LPDWORD size) {
    if (!path || strcmp(path, "Sound\\test.wav")) {
        if (size)
            *size = 0;
        return NULL;
    }
    LPBYTE bytes = malloc(4);
    memcpy(bytes, "RIFF", 4);
    *size = 4;
    return bytes;
}

void FS_FreeFile(HANDLE data) { free(data); }

static void test_device_queue_copies_payload(void) {
    BYTE bytes[16] = { 0 };
    size_t size = 0;
    ASSERT(BZ_TTAudio_Configure(BZ_TABLETOP_AUDIO_ABI_VERSION, BZ_TT_AUDIO_DEVICE));
    S_PlaySoundFile("Sound\\test.wav");
    ASSERT(BZ_TTAudio_Dequeue(BZ_TABLETOP_AUDIO_ABI_VERSION, bytes, sizeof(bytes), &size));
    ASSERT_EQ_INT(size, 4);
    ASSERT(memcmp(bytes, "RIFF", 4) == 0);
    ASSERT(!BZ_TTAudio_Dequeue(BZ_TABLETOP_AUDIO_ABI_VERSION, bytes, sizeof(bytes), &size));
    S_Shutdown();
}

static void test_dummy_sink_is_explicit_and_bounded(void) {
    BYTE bytes[16];
    size_t size;
    ASSERT(BZ_TTAudio_Configure(BZ_TABLETOP_AUDIO_ABI_VERSION, BZ_TT_AUDIO_DUMMY));
    S_PlaySoundFile("Sound\\test.wav");
    ASSERT_EQ_INT(BZ_TTAudio_DroppedCount(BZ_TABLETOP_AUDIO_ABI_VERSION), 1);
    ASSERT(!BZ_TTAudio_Dequeue(BZ_TABLETOP_AUDIO_ABI_VERSION, bytes, sizeof(bytes), &size));
    ASSERT(!BZ_TTAudio_Configure(BZ_TABLETOP_AUDIO_ABI_VERSION + 1, BZ_TT_AUDIO_DEVICE));
}

static void test_full_queue_reports_drops(void) {
    ASSERT(BZ_TTAudio_Configure(BZ_TABLETOP_AUDIO_ABI_VERSION, BZ_TT_AUDIO_DEVICE));
    for (int i = 0; i < BZ_TT_AUDIO_QUEUE_CAPACITY + 1; i++)
        S_PlaySoundFile("Sound\\test.wav");
    ASSERT_EQ_INT(BZ_TTAudio_DroppedCount(BZ_TABLETOP_AUDIO_ABI_VERSION), 1);
    S_Shutdown();
}

void run_bz_tabletop_audio_tests(void) {
    RUN_TEST(test_device_queue_copies_payload);
    RUN_TEST(test_dummy_sink_is_explicit_and_bounded);
    RUN_TEST(test_full_queue_reports_drops);
}
