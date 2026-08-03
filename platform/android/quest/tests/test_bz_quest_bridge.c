/*
 * test_bz_quest_bridge.c - coverage for bz_quest_bridge.c, the Quest
 * lifecycle/session adapter (layer 4). Links directly against the REAL
 * platform/tabletop/bridge/bz_tabletop_lifecycle.c and the lightweight
 * common/bz_runtime.c stub set, exactly mirroring games/warcraft-3/tests/
 * test_bz_tabletop_lifecycle.c's own technique (see that file's header
 * comment and bz_quest_bridge.h's) - this file does NOT re-test
 * bz_tabletop_lifecycle.c's own state machine (already covered there); it
 * tests bz_quest_bridge.c's layer on top: data-dir resolution wired into
 * BZ_TabletopCreate()/Start(), single-shot start rejection, suspend/resume/
 * stop forwarding and no-op safety, destroy-then-fresh-start ("map reload"/
 * "repeated start/stop" at the bridge level), and is_terminal()/
 * last_error() correctness.
 *
 * Unlike test_bz_tabletop_lifecycle.c, this file must NOT define Sys_Quit()
 * itself: bz_tabletop_lifecycle.c owns that definition (see its header
 * comment).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "client/client.h"
#include "server/server.h"

#include "bz_quest_bridge.h"
#include "test_framework.h"

struct client_static cls;
struct server_static svs;
struct server sv;

static int cl_init_calls;
static int cl_shutdown_calls;

void Key_Init(void) { }
void Key_WriteBindings(FILE *file) { (void)file; }
void Cmd_ForwardToServer(LPCSTR text) { (void)text; }
void CL_SetGameplayBindings(void) { }
void PF_Sleep(DWORD msec) { (void)msec; }

void CL_Init(void) { cl_init_calls++; }
void CL_Frame(DWORD msec) { (void)msec; }
void CL_Shutdown(void) { cl_shutdown_calls++; }
void CL_Connect(LPCSTR host, unsigned short port) { (void)host; (void)port; }
void CL_BeginLoadingMap(LPCSTR mapName) { (void)mapName; }
void SCR_UpdateScreen(DWORD msec) { (void)msec; }

void SV_Init(void) { svs.initialized = true; }
void SV_Frame(DWORD msec) { (void)msec; }
void SV_Map(LPCSTR pFilename) { (void)pFilename; }
void SV_Shutdown(void) { svs.initialized = false; }

static void reset_counters(void) {
    cl_init_calls = cl_shutdown_calls = 0;
    svs.initialized = false;
}

/* Absolute path to the repo-root build/tests directory test-assets
 * populates with a real tests.mpq (see games/warcraft-3/game.mk) - reused
 * here via the override mechanism (an absolute path is required - see
 * bz_quest_data.h) rather than through argv directly, so these tests
 * exercise bz_quest_bridge_start()'s REAL bz_quest_data_resolve() call,
 * not a bypass of it. */
static bool valid_data_dir_path(char *out, size_t cap) {
    if (!getcwd(out, cap)) return false;
    size_t len = strlen(out);
    int n = snprintf(out + len, cap - len, "/build/tests");
    return n > 0 && (size_t)(len + (size_t)n) < cap;
}

static bool make_temp_internal_dir(char *out, size_t cap) {
    if (snprintf(out, cap, "/tmp/bz_quest_bridge_test_XXXXXX") >= (int)cap) return false;
    return mkdtemp(out) != NULL;
}

static bool stage_override(const char *internalDataPath, const char *value) {
    char path[512];
    snprintf(path, sizeof(path), "%s/" BZ_QUEST_DATA_OVERRIDE_FILENAME, internalDataPath);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(value, f);
    fclose(f);
    return true;
}

static bool make_temp_data_dir(char *out, size_t cap) {
    if (snprintf(out, cap, "/tmp/bz_quest_bridge_data_XXXXXX") >= (int)cap) return false;
    return mkdtemp(out) != NULL;
}

/* Packs a single-entry .mpq archive at archivePath containing one file
 * named entryName holding entryContent, via the real build/bin/mpqtool
 * (built by the same top-level `make` this test itself depends on - see
 * platform/android/quest/build.mk's test-quest-bridge target) - the exact
 * "mpqtool -mpq <archive> pack <src> <archive-file>" invocation
 * games/warcraft-3/game.mk's own test-assets target uses to build
 * tests.mpq. Real MPQ bytes are required here (not a bare renamed empty
 * file) because FS_AddArchive() -> SFileOpenArchive() must actually
 * succeed for the mounted-vs-skipped distinction below to mean anything. */
static bool pack_marker_archive(const char *archivePath, const char *entryName, const char *entryContent) {
    char cwd[512];
    if (!getcwd(cwd, sizeof(cwd))) return false;

    char srcPath[300];
    if (snprintf(srcPath, sizeof(srcPath), "/tmp/bz_quest_bridge_marker_XXXXXX") >= (int)sizeof(srcPath)) return false;
    int fd = mkstemp(srcPath);
    if (fd < 0) return false;
    ssize_t n = write(fd, entryContent, strlen(entryContent));
    close(fd);
    if (n < 0 || (size_t)n != strlen(entryContent)) {
        remove(srcPath);
        return false;
    }

    char cmd[1600];
    int written = snprintf(cmd, sizeof(cmd), "'%s/%s/mpqtool' -mpq '%s' pack '%s' '%s' >/dev/null 2>&1", cwd,
                            "build/bin", archivePath, srcPath, entryName);
    bool ok = written > 0 && written < (int)sizeof(cmd) && system(cmd) == 0;
    remove(srcPath);
    return ok;
}

/* ------------------------------------------------------------------ */
/* bz_quest_bridge_start                                               */
/* ------------------------------------------------------------------ */

static void test_start_with_valid_override_reaches_running(void) {
    reset_counters();
    char internalDir[256], dataDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(valid_data_dir_path(dataDir, sizeof(dataDir)));
    ASSERT(stage_override(internalDir, dataDir));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_RUNNING);
    ASSERT_STR_EQ(bridge.dataDir, dataDir);
    ASSERT_NULL(bz_quest_bridge_last_error(&bridge));
    ASSERT_EQ_INT(cl_init_calls, 1);

    bz_quest_bridge_destroy(&bridge);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);
}

static void test_start_with_missing_data_reaches_failed(void) {
    /* No override staged, and the temp internalDataPath's default
     * "<dir>/Warcraft III" subdirectory does not exist/contain no
     * archives - BZ_RuntimeInit() must fail and the bridge must surface
     * the real lifecycle FAILED state (not a silently-ignored error). */
    reset_counters();
    char internalDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(!bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_FAILED);
    ASSERT_NOT_NULL(bz_quest_bridge_last_error(&bridge));
    ASSERT_EQ_INT(cl_init_calls, 0); /* BZ_RuntimeInit() failed before CL_Init() */

    bz_quest_bridge_destroy(&bridge); /* must still join the already-exited engine thread cleanly */
}

static void test_start_with_invalid_override_fails_before_lc_exists(void) {
    /* An override that fails bz_quest_data_validate_override() (here: a
     * relative path) is a hard configuration error - never a silent
     * fall-back to the default (see bz_quest_data.h). No lifecycle should
     * ever be created for this failure. */
    reset_counters();
    char internalDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(stage_override(internalDir, "relative/not/absolute\n"));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(!bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT_NULL(bridge.lc);
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_FAILED);
    const char *error = bz_quest_bridge_last_error(&bridge);
    ASSERT_NOT_NULL(error);
    ASSERT(strstr(error, "invalid data directory override") != NULL);
    ASSERT_EQ_INT(cl_init_calls, 0);

    bz_quest_bridge_destroy(&bridge); /* safe even though lc was never created */
}

static void test_second_start_call_is_rejected(void) {
    reset_counters();
    char internalDir[256], dataDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(valid_data_dir_path(dataDir, sizeof(dataDir)));
    ASSERT(stage_override(internalDir, dataDir));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));

    /* A second bz_quest_bridge_start() call on the SAME (already-attempted)
     * instance must be rejected outright - single-shot per instance, per
     * bz_quest_bridge.h's ownership contract. */
    ASSERT(!bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT_EQ_INT(cl_init_calls, 1); /* did not re-run CL_Init() */

    bz_quest_bridge_destroy(&bridge);
}

/* ------------------------------------------------------------------ */
/* suspend/resume/stop forwarding and no-op safety                     */
/* ------------------------------------------------------------------ */

static void test_suspend_resume_forwarding(void) {
    reset_counters();
    char internalDir[256], dataDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(valid_data_dir_path(dataDir, sizeof(dataDir)));
    ASSERT(stage_override(internalDir, dataDir));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));

    bz_quest_bridge_suspend(&bridge);
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_SUSPENDED);
    bz_quest_bridge_resume(&bridge);
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_RUNNING);

    bz_quest_bridge_stop(&bridge);
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_STOPPED);

    bz_quest_bridge_destroy(&bridge);
}

static void test_suspend_resume_stop_are_safe_before_start(void) {
    /* Android can deliver APP_CMD_PAUSE/RESUME/STOP before APP_CMD_START
     * has ever successfully attempted a bridge start (e.g. renderer/bridge
     * init still pending) - none of these may crash on a never-started,
     * fully-zeroed bridge. */
    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    bz_quest_bridge_suspend(&bridge);
    bz_quest_bridge_resume(&bridge);
    bz_quest_bridge_stop(&bridge);
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_IDLE);
    bz_quest_bridge_destroy(&bridge); /* also safe on a never-started bridge */
}

static void test_stop_is_idempotent(void) {
    reset_counters();
    char internalDir[256], dataDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(valid_data_dir_path(dataDir, sizeof(dataDir)));
    ASSERT(stage_override(internalDir, dataDir));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));

    bz_quest_bridge_stop(&bridge);
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_STOPPED);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);

    /* A second stop from an already-STOPPED state must be a safe no-op. */
    bz_quest_bridge_stop(&bridge);
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_STOPPED);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);

    bz_quest_bridge_destroy(&bridge);
}

/* ------------------------------------------------------------------ */
/* destroy-then-fresh-start ("map reload" / "repeated start/stop")     */
/* ------------------------------------------------------------------ */

static void test_destroy_resets_instance_for_a_fresh_start(void) {
    reset_counters();
    char internalDir[256], dataDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(valid_data_dir_path(dataDir, sizeof(dataDir)));
    ASSERT(stage_override(internalDir, dataDir));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT_EQ_INT(cl_init_calls, 1);

    bz_quest_bridge_destroy(&bridge);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);
    /* destroy() must fully zero the struct so the SAME storage can be
     * reused for a fresh start (bz_quest_bridge.h's documented "map
     * reload"/"repeated start/stop" mechanism). */
    ASSERT(!bridge.startAttempted);
    ASSERT_NULL(bridge.lc);
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_IDLE);

    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_RUNNING);
    ASSERT_EQ_INT(cl_init_calls, 2); /* a brand new engine thread/lifecycle ran CL_Init() again */

    bz_quest_bridge_destroy(&bridge);
}

/* ------------------------------------------------------------------ */
/* TFT edition auto-detection reaching real runtime init                */
/* ------------------------------------------------------------------ */

static void test_roc_only_data_leaves_expansion_disabled(void) {
    /* A data directory with only a base archive (no War3x*) must:
     *  - auto-detect as ROC (bz_quest_bridge_start()'s bz_quest_data_detect_edition() call)
     *  - reach RUNNING via the real BZ_RuntimeInit()/FS_AddDataDirectory()
     *  - leave the real fs_expansion cvar at 0
     *  - mount the base archive's own content (sanity: FS_FileExists finds it)
     * fs_expansion is force-reset to its true fresh-process default (0)
     * before start: it is a real process-global cvar in this test binary,
     * so this removes any dependency on which test happened to run before
     * this one - a real Android launch only ever calls Com_Init() once. */
    reset_counters();
    Cvar_Set("fs_expansion", "0");
    char internalDir[256], dataDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(make_temp_data_dir(dataDir, sizeof(dataDir)));
    ASSERT(stage_override(internalDir, dataDir));

    char war3Path[400];
    snprintf(war3Path, sizeof(war3Path), "%s/War3.mpq", dataDir);
    ASSERT(pack_marker_archive(war3Path, "ROC_MARKER.txt", "roc-marker\n"));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_RUNNING);
    ASSERT_EQ_INT(Cvar_Integer("fs_expansion", -1), 0);
    ASSERT(FS_FileExists("ROC_MARKER.txt"));
    ASSERT(!FS_FileExists("TFT_MARKER.txt"));

    bz_quest_bridge_destroy(&bridge);
}

static void test_tft_over_roc_data_enables_and_mounts_expansion(void) {
    /* A data directory with a base archive AND a War3x*-prefixed archive
     * must: auto-detect as TFT, reach RUNNING, and the War3x archive's own
     * content must be visible via FS_FileExists() - proving "-tft" reached
     * Cvar_ApplyCommandLine() (see bz_quest_data.h's ordering doc comment)
     * in time for FS_AddDataDirectory()'s archive scan/mount pass, not
     * merely that the cvar got set after the fact. This is the exact
     * behavior gap the review flagged: previously the engine skipped every
     * War3x* archive because -tft was never in argv at all. fs_expansion
     * is force-reset to 0 first for the same test-order-independence
     * reason as the ROC test above. */
    reset_counters();
    Cvar_Set("fs_expansion", "0");
    char internalDir[256], dataDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(make_temp_data_dir(dataDir, sizeof(dataDir)));
    ASSERT(stage_override(internalDir, dataDir));

    char war3Path[400], war3xPath[400];
    snprintf(war3Path, sizeof(war3Path), "%s/War3.mpq", dataDir);
    snprintf(war3xPath, sizeof(war3xPath), "%s/War3x.mpq", dataDir);
    ASSERT(pack_marker_archive(war3Path, "ROC_MARKER.txt", "roc-marker\n"));
    ASSERT(pack_marker_archive(war3xPath, "TFT_MARKER.txt", "tft-marker\n"));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_RUNNING);
    ASSERT_EQ_INT(Cvar_Integer("fs_expansion", -1), 1);
    ASSERT(FS_FileExists("ROC_MARKER.txt"));
    ASSERT(FS_FileExists("TFT_MARKER.txt")); /* proves the War3x.mpq archive was actually mounted, not skipped */

    bz_quest_bridge_destroy(&bridge);
}

static void test_case_insensitive_war3x_name_still_mounts_via_bridge(void) {
    /* A differently-cased "war3X.mpq" (as a Windows-sourced install might
     * produce) must be detected identically to "War3x.mpq" end-to-end
     * through the real bridge, mirroring FS_AddArchiveScanEntry()'s own
     * case-insensitive strncasecmp() check. */
    reset_counters();
    Cvar_Set("fs_expansion", "0");
    char internalDir[256], dataDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(make_temp_data_dir(dataDir, sizeof(dataDir)));
    ASSERT(stage_override(internalDir, dataDir));

    char war3Path[400], war3xPath[400];
    snprintf(war3Path, sizeof(war3Path), "%s/war3.MPQ", dataDir);
    snprintf(war3xPath, sizeof(war3xPath), "%s/WAR3X.mpq", dataDir);
    ASSERT(pack_marker_archive(war3Path, "ROC_MARKER.txt", "roc-marker\n"));
    ASSERT(pack_marker_archive(war3xPath, "TFT_MARKER.txt", "tft-marker\n"));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT_EQ_INT(bz_quest_bridge_state(&bridge), BZ_QUEST_BRIDGE_RUNNING);
    ASSERT_EQ_INT(Cvar_Integer("fs_expansion", -1), 1);
    ASSERT(FS_FileExists("TFT_MARKER.txt"));

    bz_quest_bridge_destroy(&bridge);
}

/* ------------------------------------------------------------------ */
/* bz_quest_bridge_is_terminal                                         */
/* ------------------------------------------------------------------ */

static void test_is_terminal_matches_failed_and_stopped_only(void) {
    reset_counters();
    char internalDir[256], dataDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));
    ASSERT(valid_data_dir_path(dataDir, sizeof(dataDir)));
    ASSERT(stage_override(internalDir, dataDir));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(!bz_quest_bridge_is_terminal(&bridge)); /* IDLE: not terminal */

    ASSERT(bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT(!bz_quest_bridge_is_terminal(&bridge)); /* RUNNING: not terminal */

    bz_quest_bridge_suspend(&bridge);
    ASSERT(!bz_quest_bridge_is_terminal(&bridge)); /* SUSPENDED: not terminal */
    bz_quest_bridge_resume(&bridge);

    bz_quest_bridge_stop(&bridge);
    ASSERT(bz_quest_bridge_is_terminal(&bridge)); /* STOPPED: terminal */

    bz_quest_bridge_destroy(&bridge);
}

static void test_is_terminal_true_for_failed_state(void) {
    reset_counters();
    char internalDir[256];
    ASSERT(make_temp_internal_dir(internalDir, sizeof(internalDir)));

    bzQuestBridge_t bridge;
    memset(&bridge, 0, sizeof(bridge));
    ASSERT(!bz_quest_bridge_start(&bridge, internalDir, NULL, NULL));
    ASSERT(bz_quest_bridge_is_terminal(&bridge)); /* FAILED: terminal */

    bz_quest_bridge_destroy(&bridge);
}

void run_bz_quest_bridge_tests(void) {
    RUN_TEST(test_start_with_valid_override_reaches_running);
    RUN_TEST(test_start_with_missing_data_reaches_failed);
    RUN_TEST(test_start_with_invalid_override_fails_before_lc_exists);
    RUN_TEST(test_second_start_call_is_rejected);
    RUN_TEST(test_suspend_resume_forwarding);
    RUN_TEST(test_suspend_resume_stop_are_safe_before_start);
    RUN_TEST(test_stop_is_idempotent);
    RUN_TEST(test_destroy_resets_instance_for_a_fresh_start);
    RUN_TEST(test_roc_only_data_leaves_expansion_disabled);
    RUN_TEST(test_tft_over_roc_data_enables_and_mounts_expansion);
    RUN_TEST(test_case_insensitive_war3x_name_still_mounts_via_bridge);
    RUN_TEST(test_is_terminal_matches_failed_and_stopped_only);
    RUN_TEST(test_is_terminal_true_for_failed_state);
}
