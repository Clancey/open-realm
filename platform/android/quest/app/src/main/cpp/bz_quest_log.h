/*
 * bz_quest_log.h - shared Android logcat macros for every bz_quest_*.c
 * translation unit (pulled out of bz_quest_host.c, which owned these alone
 * in layer 2, now that layer 3 splits the host into several files that all
 * need to log under the same "OpenRealmQuest" logcat tag).
 */
#ifndef BZ_QUEST_LOG_H
#define BZ_QUEST_LOG_H

#include <android/log.h>

#define BZ_QUEST_LOG_TAG "OpenRealmQuest"
#define BZ_QUEST_LOGI(...) __android_log_print(ANDROID_LOG_INFO, BZ_QUEST_LOG_TAG, __VA_ARGS__)
#define BZ_QUEST_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, BZ_QUEST_LOG_TAG, __VA_ARGS__)

#endif /* BZ_QUEST_LOG_H */
