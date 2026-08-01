/*
 * test_bz_quest_data.c - coverage for bz_quest_data.c's data-directory
 * resolution, override validation, and argv construction (layer 4). Each
 * case covers a normal path and its inverse/error path, per AGENTS.md's
 * test discipline. Uses real temporary files/directories (via mkdtemp) to
 * exercise bz_quest_data_read_override_file()'s actual POSIX file access -
 * this module deliberately does no Android-specific I/O (see its header
 * comment), so a plain host compiler and libc are all that's needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bz_quest_data.h"
#include "test_framework.h"

/* ------------------------------------------------------------------ */
/* bz_quest_data_default_dir                                          */
/* ------------------------------------------------------------------ */

static void test_default_dir_prefers_external(void) {
    char out[BZ_QUEST_DATA_DIR_MAX];
    bool ok = bz_quest_data_default_dir("/data/user/0/pkg/files", "/sdcard/Android/data/pkg/files", out,
                                         sizeof(out));
    ASSERT(ok);
    ASSERT_STR_EQ(out, "/sdcard/Android/data/pkg/files/Warcraft III");
}

static void test_default_dir_falls_back_to_internal(void) {
    /* externalDataPath is NULL per the NDK's own documented possibility
     * (e.g. no shared storage volume mounted) - see bz_quest_data.h. */
    char out[BZ_QUEST_DATA_DIR_MAX];
    bool ok = bz_quest_data_default_dir("/data/user/0/pkg/files", NULL, out, sizeof(out));
    ASSERT(ok);
    ASSERT_STR_EQ(out, "/data/user/0/pkg/files/Warcraft III");
}

static void test_default_dir_rejects_no_usable_base(void) {
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(!bz_quest_data_default_dir(NULL, NULL, out, sizeof(out)));
    ASSERT(!bz_quest_data_default_dir("", "", out, sizeof(out)));
}

static void test_default_dir_rejects_overflow(void) {
    char longBase[BZ_QUEST_DATA_DIR_MAX];
    memset(longBase, 'a', sizeof(longBase) - 1);
    longBase[sizeof(longBase) - 1] = '\0';
    char out[8]; /* deliberately too small */
    ASSERT(!bz_quest_data_default_dir(longBase, NULL, out, sizeof(out)));
}

/* ------------------------------------------------------------------ */
/* bz_quest_data_validate_override                                    */
/* ------------------------------------------------------------------ */

static void test_validate_override_accepts_absolute_path(void) {
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(bz_quest_data_validate_override("/sdcard/wc3data", out, sizeof(out)));
    ASSERT_STR_EQ(out, "/sdcard/wc3data");
}

static void test_validate_override_trims_crlf(void) {
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(bz_quest_data_validate_override("/sdcard/wc3data\r\n", out, sizeof(out)));
    ASSERT_STR_EQ(out, "/sdcard/wc3data");

    ASSERT(bz_quest_data_validate_override("/sdcard/wc3data\n", out, sizeof(out)));
    ASSERT_STR_EQ(out, "/sdcard/wc3data");
}

static void test_validate_override_rejects_null(void) {
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(!bz_quest_data_validate_override(NULL, out, sizeof(out)));
}

static void test_validate_override_rejects_empty(void) {
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(!bz_quest_data_validate_override("", out, sizeof(out)));
    ASSERT(!bz_quest_data_validate_override("\n", out, sizeof(out)));
}

static void test_validate_override_rejects_relative_path(void) {
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(!bz_quest_data_validate_override("sdcard/wc3data", out, sizeof(out)));
}

static void test_validate_override_rejects_disallowed_chars(void) {
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(!bz_quest_data_validate_override("/sdcard/\"wc3data\"", out, sizeof(out)));
    ASSERT(!bz_quest_data_validate_override("/sdcard/wc3data;rm -rf", out, sizeof(out)));
    ASSERT(!bz_quest_data_validate_override("/sdcard/wc3\rdata", out, sizeof(out)));
}

static void test_validate_override_rejects_oversized(void) {
    char raw[BZ_QUEST_DATA_DIR_MAX + 32];
    raw[0] = '/';
    memset(raw + 1, 'a', sizeof(raw) - 2);
    raw[sizeof(raw) - 1] = '\0';
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(!bz_quest_data_validate_override(raw, out, sizeof(out)));
}

/* ------------------------------------------------------------------ */
/* bz_quest_data_read_override_file                                    */
/* ------------------------------------------------------------------ */

static bool make_temp_dir(char *out, size_t cap) {
    if (snprintf(out, cap, "/tmp/bz_quest_data_test_XXXXXX") >= (int)cap) return false;
    return mkdtemp(out) != NULL;
}

static void test_read_override_file_absent_is_not_an_error(void) {
    char dir[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(make_temp_dir(dir, sizeof(dir)));
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(!bz_quest_data_read_override_file(dir, out, sizeof(out)));
    remove(dir);
}

static void test_read_override_file_present_returns_raw_line(void) {
    char dir[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(make_temp_dir(dir, sizeof(dir)));
    char path[BZ_QUEST_DATA_DIR_MAX];
    snprintf(path, sizeof(path), "%s/" BZ_QUEST_DATA_OVERRIDE_FILENAME, dir);
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fputs("/sdcard/staged_wc3\n", f);
    fclose(f);

    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(bz_quest_data_read_override_file(dir, out, sizeof(out)));
    ASSERT_STR_EQ(out, "/sdcard/staged_wc3\n");

    remove(path);
    remove(dir);
}

static void test_read_override_file_rejects_missing_base(void) {
    char out[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(!bz_quest_data_read_override_file(NULL, out, sizeof(out)));
    ASSERT(!bz_quest_data_read_override_file("", out, sizeof(out)));
}

/* ------------------------------------------------------------------ */
/* bz_quest_data_resolve                                               */
/* ------------------------------------------------------------------ */

static void test_resolve_uses_default_when_no_override_staged(void) {
    char dir[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(make_temp_dir(dir, sizeof(dir)));

    char outDir[BZ_QUEST_DATA_DIR_MAX], outError[BZ_QUEST_DATA_ERROR_MAX];
    ASSERT(bz_quest_data_resolve(dir, NULL, outDir, sizeof(outDir), outError, sizeof(outError)));
    char expected[BZ_QUEST_DATA_DIR_MAX];
    snprintf(expected, sizeof(expected), "%s/" BZ_QUEST_DATA_SUBDIR, dir);
    ASSERT_STR_EQ(outDir, expected);

    remove(dir);
}

static void test_resolve_uses_valid_staged_override(void) {
    char dir[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(make_temp_dir(dir, sizeof(dir)));
    char path[BZ_QUEST_DATA_DIR_MAX];
    snprintf(path, sizeof(path), "%s/" BZ_QUEST_DATA_OVERRIDE_FILENAME, dir);
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fputs("/sdcard/staged_wc3\n", f);
    fclose(f);

    char outDir[BZ_QUEST_DATA_DIR_MAX], outError[BZ_QUEST_DATA_ERROR_MAX];
    ASSERT(bz_quest_data_resolve(dir, NULL, outDir, sizeof(outDir), outError, sizeof(outError)));
    ASSERT_STR_EQ(outDir, "/sdcard/staged_wc3");

    remove(path);
    remove(dir);
}

static void test_resolve_hard_fails_on_invalid_staged_override(void) {
    /* An invalid override must never silently fall back to the default -
     * see bz_quest_data.h's data-path contract comment. */
    char dir[BZ_QUEST_DATA_DIR_MAX];
    ASSERT(make_temp_dir(dir, sizeof(dir)));
    char path[BZ_QUEST_DATA_DIR_MAX];
    snprintf(path, sizeof(path), "%s/" BZ_QUEST_DATA_OVERRIDE_FILENAME, dir);
    FILE *f = fopen(path, "w");
    ASSERT_NOT_NULL(f);
    fputs("relative/not/absolute\n", f);
    fclose(f);

    char outDir[BZ_QUEST_DATA_DIR_MAX], outError[BZ_QUEST_DATA_ERROR_MAX];
    ASSERT(!bz_quest_data_resolve(dir, NULL, outDir, sizeof(outDir), outError, sizeof(outError)));
    ASSERT(outError[0] != '\0');

    remove(path);
    remove(dir);
}

static void test_resolve_fails_with_no_usable_base_and_no_override(void) {
    char outDir[BZ_QUEST_DATA_DIR_MAX], outError[BZ_QUEST_DATA_ERROR_MAX];
    ASSERT(!bz_quest_data_resolve(NULL, NULL, outDir, sizeof(outDir), outError, sizeof(outError)));
    ASSERT(outError[0] != '\0');
}

/* ------------------------------------------------------------------ */
/* bz_quest_data_build_argv                                            */
/* ------------------------------------------------------------------ */

static void test_build_argv_without_map(void) {
    char storage[BZ_QUEST_DATA_ARGV_MAX][BZ_QUEST_DATA_DIR_MAX];
    const char *argv[BZ_QUEST_DATA_ARGV_MAX];
    int argc = bz_quest_data_build_argv("/sdcard/Warcraft III", NULL, storage, argv, BZ_QUEST_DATA_ARGV_MAX);
    ASSERT_EQ_INT(argc, 3);
    ASSERT_STR_EQ(argv[1], "-data");
    ASSERT_STR_EQ(argv[2], "/sdcard/Warcraft III");
}

static void test_build_argv_with_map(void) {
    char storage[BZ_QUEST_DATA_ARGV_MAX][BZ_QUEST_DATA_DIR_MAX];
    const char *argv[BZ_QUEST_DATA_ARGV_MAX];
    int argc = bz_quest_data_build_argv("/sdcard/Warcraft III", "(2)IceCrown", storage, argv,
                                         BZ_QUEST_DATA_ARGV_MAX);
    ASSERT_EQ_INT(argc, 5);
    ASSERT_STR_EQ(argv[3], "+map");
    ASSERT_STR_EQ(argv[4], "(2)IceCrown");
}

static void test_build_argv_rejects_undersized_max_argv(void) {
    char storage[BZ_QUEST_DATA_ARGV_MAX][BZ_QUEST_DATA_DIR_MAX];
    const char *argv[BZ_QUEST_DATA_ARGV_MAX];
    /* mapName is non-NULL (needs argc 5), but caller only allows 3. */
    int argc = bz_quest_data_build_argv("/sdcard/Warcraft III", "SomeMap", storage, argv, 3);
    ASSERT_EQ_INT(argc, 0);
}

static void test_build_argv_rejects_null_data_dir(void) {
    char storage[BZ_QUEST_DATA_ARGV_MAX][BZ_QUEST_DATA_DIR_MAX];
    const char *argv[BZ_QUEST_DATA_ARGV_MAX];
    int argc = bz_quest_data_build_argv(NULL, NULL, storage, argv, BZ_QUEST_DATA_ARGV_MAX);
    ASSERT_EQ_INT(argc, 0);
}

void run_bz_quest_data_tests(void) {
    RUN_TEST(test_default_dir_prefers_external);
    RUN_TEST(test_default_dir_falls_back_to_internal);
    RUN_TEST(test_default_dir_rejects_no_usable_base);
    RUN_TEST(test_default_dir_rejects_overflow);
    RUN_TEST(test_validate_override_accepts_absolute_path);
    RUN_TEST(test_validate_override_trims_crlf);
    RUN_TEST(test_validate_override_rejects_null);
    RUN_TEST(test_validate_override_rejects_empty);
    RUN_TEST(test_validate_override_rejects_relative_path);
    RUN_TEST(test_validate_override_rejects_disallowed_chars);
    RUN_TEST(test_validate_override_rejects_oversized);
    RUN_TEST(test_read_override_file_absent_is_not_an_error);
    RUN_TEST(test_read_override_file_present_returns_raw_line);
    RUN_TEST(test_read_override_file_rejects_missing_base);
    RUN_TEST(test_resolve_uses_default_when_no_override_staged);
    RUN_TEST(test_resolve_uses_valid_staged_override);
    RUN_TEST(test_resolve_hard_fails_on_invalid_staged_override);
    RUN_TEST(test_resolve_fails_with_no_usable_base_and_no_override);
    RUN_TEST(test_build_argv_without_map);
    RUN_TEST(test_build_argv_with_map);
    RUN_TEST(test_build_argv_rejects_undersized_max_argv);
    RUN_TEST(test_build_argv_rejects_null_data_dir);
}
