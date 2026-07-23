/*
 * test_bz_tabletop_lifecycle.c — coverage for
 * platform/apple/visionos/tabletop/bridge/bz_tabletop_lifecycle.c, the
 * portable pthreads-based state machine and dedicated-thread driver the
 * Objective-C++ bridge (bz_tabletop_bridge.mm) forwards to. Runs on the
 * normal desktop toolchain (no visionOS SDK needed) using the same
 * stubbed CL_/SV_ boundary as test_bz_runtime.c, since this module's
 * engine thread calls the real BZ_RuntimeInit/Frame/Shutdown from
 * common/bz_runtime.c.
 *
 * Unlike test_bz_runtime.c, this file must NOT define Sys_Quit() itself:
 * bz_tabletop_lifecycle.c owns that definition (thread-local
 * self-identification so a frame-limit/"quit" command can signal the
 * right lifecycle instance — see that file's header comment).
 */

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "../client/client.h"
#include "server/server.h"
#include "bz_tabletop_lifecycle.h"
#include "test_framework.h"

struct client_static cls;
struct server_static svs;
struct server sv;

static int cl_init_calls;
static int cl_frame_calls;
static int cl_shutdown_calls;
static int sv_init_calls;
static int sv_shutdown_calls;

void Key_Init(void) { }
void Key_WriteBindings(FILE *file) { (void)file; }
void Cmd_ForwardToServer(LPCSTR text) { (void)text; }
void CL_SetGameplayBindings(void) { }
void PF_Sleep(DWORD msec) { (void)msec; }

void CL_Init(void) { cl_init_calls++; }
void CL_Frame(DWORD msec) { (void)msec; cl_frame_calls++; }
void CL_Shutdown(void) { cl_shutdown_calls++; }
void CL_Connect(LPCSTR host, unsigned short port) { (void)host; (void)port; }
void CL_BeginLoadingMap(LPCSTR mapName) { (void)mapName; }
void SCR_UpdateScreen(DWORD msec) { (void)msec; }

void SV_Init(void) { sv_init_calls++; svs.initialized = true; }
void SV_Frame(DWORD msec) { (void)msec; }
void SV_Map(LPCSTR pFilename) { (void)pFilename; }
void SV_Shutdown(void) { sv_shutdown_calls++; svs.initialized = false; }

static void reset_counters(void) {
    cl_init_calls = cl_frame_calls = cl_shutdown_calls = 0;
    sv_init_calls = sv_shutdown_calls = 0;
    svs.initialized = false;
}

/* Polls BZ_TabletopGetState() until it matches `want` or `timeout_ms`
 * elapses. Used only for the frame-limit self-stop scenario, where the
 * engine thread transitions state on its own without this test ever
 * calling BZ_TabletopStop() — there is no other safe way to await an
 * asynchronous, thread-driven transition without an internal condvar
 * exposed to callers, and this module's public API deliberately does not
 * expose one (only the blocking BZ_TabletopStart/Stop calls do). */
static bool wait_for_state(bzTabletopLifecycle_t *lc, bzTabletopState_t want, int timeout_ms) {
    struct timespec step = { 0, 5L * 1000L * 1000L }; /* 5ms */
    int waited_ms = 0;
    while (BZ_TabletopGetState(lc) != want && waited_ms < timeout_ms) {
        nanosleep(&step, NULL);
        waited_ms += 5;
    }
    return BZ_TabletopGetState(lc) == want;
}

static void test_valid_init_reaches_running(void) {
    reset_counters();
    const char *argv[] = { "test_bz_tabletop_lifecycle", "-data", "build/tests", "+com_frame_limit", "0" };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(5, argv);
    ASSERT_NOT_NULL(lc);

    BZ_TabletopStart(lc);

    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_RUNNING);
    ASSERT_NULL(BZ_TabletopLastError(lc));
    ASSERT_EQ_INT(cl_init_calls, 1);

    BZ_TabletopStop(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_STOPPED);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);

    BZ_TabletopDestroy(lc);
}

static void test_bad_init_reaches_failed_with_error(void) {
    reset_counters();
    const char *argv[] = { "test_bz_tabletop_lifecycle", "-data", "build/tests/does-not-exist" };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(3, argv);
    ASSERT_NOT_NULL(lc);

    BZ_TabletopStart(lc);

    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_FAILED);
    ASSERT_NOT_NULL(BZ_TabletopLastError(lc));
    ASSERT_EQ_INT(cl_init_calls, 0);
    /* The engine thread already exited on its own (init failed before the
     * frame loop ever started) - Destroy must still join it cleanly. */
    BZ_TabletopDestroy(lc);
}

static void test_suspend_resume_transitions(void) {
    reset_counters();
    const char *argv[] = { "test_bz_tabletop_lifecycle", "-data", "build/tests", "+com_frame_limit", "0" };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(5, argv);
    BZ_TabletopStart(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_RUNNING);

    BZ_TabletopSuspend(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_SUSPENDED);
    /* No frames should tick while suspended: give the engine thread a
     * moment to observe the state and confirm it stays put. */
    struct timespec pause = { 0, 30L * 1000L * 1000L };
    nanosleep(&pause, NULL);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_SUSPENDED);

    BZ_TabletopResume(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_RUNNING);

    BZ_TabletopStop(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_STOPPED);
    BZ_TabletopDestroy(lc);
}

static void test_stop_blocks_until_stopped_and_is_idempotent(void) {
    reset_counters();
    const char *argv[] = { "test_bz_tabletop_lifecycle", "-data", "build/tests", "+com_frame_limit", "0" };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(5, argv);
    BZ_TabletopStart(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_RUNNING);

    BZ_TabletopStop(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_STOPPED);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);

    /* Idempotent: a second external stop from an already-STOPPED state
     * must be a safe no-op, not a crash/hang/double-shutdown. */
    BZ_TabletopStop(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_STOPPED);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);

    BZ_TabletopDestroy(lc);
}

static void *stopper_thread_main(void *arg) {
    BZ_TabletopStop((bzTabletopLifecycle_t *)arg);
    return NULL;
}

static void test_concurrent_stop_calls_do_not_crash_or_hang(void) {
    reset_counters();
    const char *argv[] = { "test_bz_tabletop_lifecycle", "-data", "build/tests", "+com_frame_limit", "0" };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(5, argv);
    BZ_TabletopStart(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_RUNNING);

    /* Two threads racing BZ_TabletopStop() against each other (and against
     * the engine thread) must both return, and the engine must end up
     * cleanly STOPPED exactly once (cl_shutdown_calls stays 1). */
    pthread_t stoppers[2];
    pthread_create(&stoppers[0], NULL, stopper_thread_main, lc);
    pthread_create(&stoppers[1], NULL, stopper_thread_main, lc);
    pthread_join(stoppers[0], NULL);
    pthread_join(stoppers[1], NULL);

    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_STOPPED);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);

    BZ_TabletopDestroy(lc);
}

static void test_start_after_terminal_state_is_rejected(void) {
    reset_counters();
    const char *argv[] = { "test_bz_tabletop_lifecycle", "-data", "build/tests", "+com_frame_limit", "0" };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(5, argv);

    BZ_TabletopStart(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_RUNNING);
    ASSERT_EQ_INT(BZ_TabletopEngineThreadSpawnCount(lc), 1);
    BZ_TabletopStop(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_STOPPED);
    ASSERT_EQ_INT(cl_init_calls, 1);

    /* The engine thread is single-shot per instance: STOPPED is terminal
     * and must never regress back into STARTING/RUNNING. A further
     * BZ_TabletopStart() call must be rejected as a no-op — no second
     * engine thread, no second CL_Init(), state stays STOPPED. Callers
     * that need to run again must BZ_TabletopCreate() a fresh instance. */
    BZ_TabletopStart(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_STOPPED);
    ASSERT_EQ_INT(cl_init_calls, 1);
    ASSERT_EQ_INT(BZ_TabletopEngineThreadSpawnCount(lc), 1);

    BZ_TabletopDestroy(lc);
}

static void test_start_after_failed_is_rejected(void) {
    reset_counters();
    const char *argv[] = { "test_bz_tabletop_lifecycle", "-data", "build/tests/does-not-exist" };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(3, argv);

    BZ_TabletopStart(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_FAILED);
    ASSERT_NOT_NULL(BZ_TabletopLastError(lc));
    /* None of BZ_RuntimeInit()'s failure paths reach CL_Init()/SV_Init()
     * (they all return before either is called), so cl_init_calls/
     * sv_init_calls can't distinguish "rejected" from "silently re-ran and
     * failed again" here. BZ_TabletopEngineThreadSpawnCount() can: it only
     * increments when BZ_TabletopStart() actually pthread_create()s a new
     * engine thread, which happened exactly once so far. */
    ASSERT_EQ_INT(BZ_TabletopEngineThreadSpawnCount(lc), 1);

    /* FAILED is equally terminal: a bad first init must not be retried by
     * calling BZ_TabletopStart() again on the same instance — verified by
     * the spawn count staying at 1 rather than becoming 2. */
    BZ_TabletopStart(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_FAILED);
    ASSERT_EQ_INT(BZ_TabletopEngineThreadSpawnCount(lc), 1);

    BZ_TabletopDestroy(lc);
}

static void test_destroy_implies_stop(void) {
    reset_counters();
    const char *argv[] = { "test_bz_tabletop_lifecycle", "-data", "build/tests", "+com_frame_limit", "0" };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(5, argv);
    BZ_TabletopStart(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_RUNNING);

    /* No explicit BZ_TabletopStop() call: Destroy must stop and join the
     * engine thread itself. If it did not, this call would hang forever
     * (pthread_join on a still-running thread that nobody ever signaled)
     * and the whole test binary would time out instead of completing. */
    BZ_TabletopDestroy(lc);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);
}

static void test_frame_limit_triggers_self_stop_without_external_stop(void) {
    reset_counters();
    const char *argv[] = {
        "test_bz_tabletop_lifecycle", "-data", "build/tests",
        "+map", "Human02", "+com_frame_limit", "3",
    };
    bzTabletopLifecycle_t *lc = BZ_TabletopCreate(7, argv);

    BZ_TabletopStart(lc);
    ASSERT_EQ_INT(BZ_TabletopGetState(lc), BZ_TABLETOP_STATE_RUNNING);
    ASSERT_EQ_INT(sv_init_calls, 1);

    /* Nobody calls BZ_TabletopStop() here: reaching com_frame_limit=3
     * inside BZ_RuntimeFrame() drives Com_Quit() -> Sys_Quit() (defined in
     * bz_tabletop_lifecycle.c) -> BZ_TabletopStop()'s re-entrant,
     * self-thread branch, which only flags the request; the engine
     * thread's own loop then observes BZ_RuntimeFrame() returning false,
     * breaks, and transitions itself to STOPPED. */
    ASSERT(wait_for_state(lc, BZ_TABLETOP_STATE_STOPPED, 2000));
    ASSERT_EQ_INT(sv_shutdown_calls, 1);

    /* thread_started is still true here (nobody external joined it yet) -
     * Destroy must join the already-exited thread without hanging. */
    BZ_TabletopDestroy(lc);
}

void run_bz_tabletop_lifecycle_tests(void) {
    RUN_TEST(test_valid_init_reaches_running);
    RUN_TEST(test_bad_init_reaches_failed_with_error);
    RUN_TEST(test_suspend_resume_transitions);
    RUN_TEST(test_stop_blocks_until_stopped_and_is_idempotent);
    RUN_TEST(test_concurrent_stop_calls_do_not_crash_or_hang);
    RUN_TEST(test_start_after_terminal_state_is_rejected);
    RUN_TEST(test_start_after_failed_is_rejected);
    RUN_TEST(test_destroy_implies_stop);
    RUN_TEST(test_frame_limit_triggers_self_stop_without_external_stop);
}
