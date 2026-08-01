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
    RUN_TEST(test_is_terminal_matches_failed_and_stopped_only);
    RUN_TEST(test_is_terminal_true_for_failed_state);
}
