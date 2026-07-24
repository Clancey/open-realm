#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/common.h"
#include "platform/bridge/bz_tabletop_transport.h"

struct bzTTSnapshot {
    char configstrings[16][BZ_TT_MAX_CONFIGSTRING_LEN];
};

struct world_state world;
static bool test_tft;
static pthread_mutex_t read_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t read_cond = PTHREAD_COND_INITIALIZER;
static bool read_blocked;
static unsigned read_waiters;

void test_assets_set_tft(bool enabled) { test_tft = enabled; }
void test_assets_block_reads(bool blocked) {
    pthread_mutex_lock(&read_lock);
    read_blocked = blocked;
    if (!blocked) pthread_cond_broadcast(&read_cond);
    pthread_mutex_unlock(&read_lock);
}
void test_assets_wait_for_blocked_reads(unsigned count) {
    pthread_mutex_lock(&read_lock);
    while (read_waiters < count) pthread_cond_wait(&read_cond, &read_lock);
    pthread_mutex_unlock(&read_lock);
}

void test_assets_set_configstring(struct bzTTSnapshot *snapshot, uint32_t index, const char *value) {
    if (snapshot && index < 16)
        snprintf(snapshot->configstrings[index], sizeof(snapshot->configstrings[index]), "%s", value);
}

bool BZ_TTSnapshot_ConfigString(const bzTTSnapshot_t *snapshot, uint32_t index, char *out, size_t cap) {
    if (!snapshot || !out || !cap || index >= 16 || !snapshot->configstrings[index][0])
        return false;
    snprintf(out, cap, "%s", snapshot->configstrings[index]);
    return true;
}

HANDLE FS_ReadFile(LPCSTR identity, LPDWORD size) {
    char path[512];
    FILE *file;
    long length;
    void *data;
    if (!identity || !size) return NULL;
    pthread_mutex_lock(&read_lock);
    if (read_blocked) {
        read_waiters++;
        pthread_cond_broadcast(&read_cond);
        while (read_blocked) pthread_cond_wait(&read_cond, &read_lock);
        read_waiters--;
    }
    pthread_mutex_unlock(&read_lock);
    if (!strcmp(identity, "TestUI/Textures/variant.blp"))
        identity = test_tft ? "TestUI/Textures/orientation_2x2.blp" : "TestUI/Textures/solid_white.blp";
    snprintf(path, sizeof(path), "build/tests/resources/%s", identity);
    file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || fseek(file, 0, SEEK_SET)) {
        fclose(file); return NULL;
    }
    data = malloc((size_t)length);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
        free(data); fclose(file); return NULL;
    }
    fclose(file);
    *size = (DWORD)length;
    return data;
}

void FS_FreeFile(void *data) { free(data); }
