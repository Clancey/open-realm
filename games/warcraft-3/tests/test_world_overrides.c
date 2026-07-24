#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_framework.h"
#include "g_local.h"

struct game_export globals;
struct game_import gi;
struct game_locals game;
struct level_locals level;
struct edict_s *g_edicts;
static size_t fail_allocation_size;

static HANDLE fixture_alloc(long size) {
    if ((size_t)size == fail_allocation_size) {
        fail_allocation_size = 0;
        return NULL;
    }
    return calloc(1, (size_t)size);
}
static void fixture_free(HANDLE data) { free(data); }

void CM_ReadUnits(HANDLE archive);
void CM_ReleaseModel(void);
void CM_ReadPathMap(HANDLE archive) { (void)archive; }

typedef struct { BYTE data[2048]; DWORD size; } overrideFixture_t;

static void fixture_dword(overrideFixture_t *fixture, DWORD value) {
    memcpy(fixture->data + fixture->size, &value, sizeof(value));
    fixture->size += sizeof(value);
}

static void fixture_string(overrideFixture_t *fixture, LPCSTR value) {
    size_t size = strlen(value) + 1;
    memcpy(fixture->data + fixture->size, value, size);
    fixture->size += (DWORD)size;
}

static HANDLE fixture_archive(LPCSTR path, LPCVOID data, DWORD size) {
    HANDLE archive = NULL;
    remove(path);
    ASSERT(SFileCreateArchive(path, 0, 4, &archive));
    ASSERT(SFileAddFileFromBuffer(archive, "war3map.w3b", data, size));
    ASSERT(SFileCloseArchive(archive));
    archive = NULL;
    ASSERT(SFileOpenArchive(path, 0, 0, &archive));
    return archive;
}

static void test_w3b_custom_destructable_overrides_and_malformed_bounds(void) {
    const char *path = "/tmp/open-realm-test-overrides.mpq";
    overrideFixture_t fixture = { 0 };
    HANDLE archive;
    gi.MemAlloc = fixture_alloc; gi.MemFree = fixture_free;

    fixture_dword(&fixture, 2);
    fixture_dword(&fixture, UINT32_MAX);
    archive = fixture_archive(path, fixture.data, fixture.size);
    CM_ReadUnits(archive);
    ASSERT_EQ_INT(world.info.num_originalDestructables, 0);
    ASSERT_EQ_INT(world.info.num_userCreatedDestructables, 0);
    ASSERT(world.info.originalDestructables == NULL);
    ASSERT(world.info.userCreatedDestructables == NULL);
    ASSERT(SFileCloseArchive(archive));

    memset(&fixture, 0, sizeof(fixture));
    fixture_dword(&fixture, 2);
    fixture.data[fixture.size++] = 0;
    archive = fixture_archive(path, fixture.data, fixture.size);
    CM_ReadUnits(archive);
    ASSERT_EQ_INT(world.info.num_originalDestructables, 0);
    ASSERT_EQ_INT(world.info.num_userCreatedDestructables, 0);
    ASSERT(SFileCloseArchive(archive));

    memset(&fixture, 0, sizeof(fixture));
    fixture_dword(&fixture, 2);
    fixture_dword(&fixture, 0);
    fixture_dword(&fixture, 1);
    fixture_dword(&fixture, MAKEFOURCC('L','T','l','t'));
    fixture_dword(&fixture, MAKEFOURCC('L','0','0','0'));
    fixture_dword(&fixture, 1);
    fixture_dword(&fixture, MAKEFOURCC('b','t','x','f'));
    fixture_dword(&fixture, mod_string);
    memset(fixture.data + fixture.size, 'x', MAX_TRIGSTR_LENGTH);
    fixture.size += MAX_TRIGSTR_LENGTH;
    archive = fixture_archive(path, fixture.data, fixture.size);
    CM_ReadUnits(archive);
    ASSERT_EQ_INT(world.info.num_originalDestructables, 0);
    ASSERT_EQ_INT(world.info.num_userCreatedDestructables, 0);
    ASSERT(SFileCloseArchive(archive));

    memset(&fixture, 0, sizeof(fixture));
    fixture_dword(&fixture, 2);
    fixture_dword(&fixture, 0);
    fixture_dword(&fixture, 1);
    fixture_dword(&fixture, MAKEFOURCC('L','T','l','t'));
    fixture_dword(&fixture, MAKEFOURCC('L','0','0','0'));
    fixture_dword(&fixture, 1);
    fixture_dword(&fixture, MAKEFOURCC('b','t','x','f'));
    fixture_dword(&fixture, mod_string);
    fixture_string(&fixture, "ReplaceableTextures\\LordaeronTree\\LordaeronSummerTree");
    fixture_dword(&fixture, MAKEFOURCC('L','0','0','0'));
    archive = fixture_archive(path, fixture.data, fixture.size);
    fail_allocation_size = sizeof(unitModification_t);
    CM_ReadUnits(archive);
    ASSERT_EQ_INT(world.info.num_originalDestructables, 0);
    ASSERT_EQ_INT(world.info.num_userCreatedDestructables, 0);
    ASSERT(SFileCloseArchive(archive));
    archive = NULL;
    ASSERT(SFileOpenArchive(path, 0, 0, &archive));
    CM_ReadUnits(archive);
    ASSERT_EQ_INT(world.info.num_originalDestructables, 0);
    ASSERT_EQ_INT(world.info.num_userCreatedDestructables, 1);
    ASSERT_EQ_INT(world.info.userCreatedDestructables[0].originalUnitID, MAKEFOURCC('L','T','l','t'));
    ASSERT_EQ_INT(world.info.userCreatedDestructables[0].newUnitID, MAKEFOURCC('L','0','0','0'));
    ASSERT_EQ_INT(world.info.userCreatedDestructables[0].numbeOfModifications, 1);
    ASSERT_STR_EQ(world.info.userCreatedDestructables[0].modifications[0].data,
                  "ReplaceableTextures\\LordaeronTree\\LordaeronSummerTree");
    ASSERT(SFileCloseArchive(archive));
    CM_ReleaseModel();
    memset(&world.info, 0, sizeof(world.info));
    remove(path);
}

void run_world_override_tests(void) {
    RUN_TEST(test_w3b_custom_destructable_overrides_and_malformed_bounds);
}
