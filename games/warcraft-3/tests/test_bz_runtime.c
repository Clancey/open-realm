/*
 * test_bz_runtime.c — coverage for the reusable engine runtime (bz_runtime.c)
 * factored out of common/main.c for embeddable hosts (e.g. the visionOS
 * lifecycle bridge). Exercises init/frame/shutdown across menu, listen
 * server, and dedicated modes, the com_frame_limit stop condition, explicit
 * startup-failure error codes, and shutdown idempotence — all without real
 * game assets, using the same client/server.h struct definitions and a
 * stubbed CL_/SV_ boundary as test_commands.c.
 */

#include <stdio.h>
#include <string.h>

#include "../client/client.h"
#include "server/server.h"
#include "bz_runtime.h"
#include "test_framework.h"

/* bz_runtime.c reads/writes these globals directly; cl_main.c/sv_main.c are
 * not linked into this binary, so this test provides the storage (same
 * pattern as test_client_stubs.c's `cls` and sv_main.c's `svs`/`sv`). */
struct client_static cls;
struct server_static svs;
struct server sv;

static int cl_init_calls;
static int cl_frame_calls;
static int cl_shutdown_calls;
static int cl_connect_calls;
static int cl_begin_loading_map_calls;
static int scr_update_screen_calls;
static int sv_init_calls;
static int sv_frame_calls;
static int sv_map_calls;
static int sv_shutdown_calls;
static int sys_quit_calls;
static PATHSTR last_connect_host;
static unsigned short last_connect_port;
static PATHSTR last_loading_map;
static PATHSTR last_sv_map;

void Key_Init(void) { }
void Key_WriteBindings(FILE *file) { (void)file; }
void Cmd_ForwardToServer(LPCSTR text) { (void)text; }
void CL_SetGameplayBindings(void) { }
void PF_Sleep(DWORD msec) { (void)msec; }

void CL_Init(void) { cl_init_calls++; }
void CL_Frame(DWORD msec) { (void)msec; cl_frame_calls++; }
void CL_Shutdown(void) { cl_shutdown_calls++; }
void CL_Connect(LPCSTR host, unsigned short port) {
    cl_connect_calls++;
    snprintf(last_connect_host, sizeof(last_connect_host), "%s", host ? host : "");
    last_connect_port = port;
}
void CL_BeginLoadingMap(LPCSTR mapName) {
    cl_begin_loading_map_calls++;
    snprintf(last_loading_map, sizeof(last_loading_map), "%s", mapName ? mapName : "");
}
void SCR_UpdateScreen(DWORD msec) { (void)msec; scr_update_screen_calls++; }

void SV_Init(void) { sv_init_calls++; svs.initialized = true; }
void SV_Frame(DWORD msec) { (void)msec; sv_frame_calls++; }
void SV_Map(LPCSTR pFilename) {
    sv_map_calls++;
    snprintf(last_sv_map, sizeof(last_sv_map), "%s", pFilename ? pFilename : "");
}
void SV_Shutdown(void) { sv_shutdown_calls++; svs.initialized = false; }

/* Embeddable-host-style Sys_Quit(): unlike desktop main.c's exit(0), this
 * must NOT terminate the process — bz_runtime.c's Com_Quit() calls it after
 * BZ_RuntimeShutdown(), and this test process keeps running further
 * scenarios afterward. Counting calls also verifies the frame-limit path
 * actually reaches Com_Quit(). */
void Sys_Quit(void) { sys_quit_calls++; }

static void reset_counters(void) {
    cl_init_calls = cl_frame_calls = cl_shutdown_calls = cl_connect_calls = 0;
    cl_begin_loading_map_calls = scr_update_screen_calls = 0;
    sv_init_calls = sv_frame_calls = sv_map_calls = sv_shutdown_calls = 0;
    sys_quit_calls = 0;
    last_connect_host[0] = '\0';
    last_connect_port = 0;
    last_loading_map[0] = '\0';
    last_sv_map[0] = '\0';
    svs.initialized = false;
}

/*
 * BZ_RuntimeInit() reloads share/default.cfg on every Com_Init() call, which
 * resets "dedicated"/"com_frame_limit"/"map"/"connect" to known defaults, so
 * tests do not need to reset those between scenarios. "data" is a
 * CVAR_ARCHIVE cvar with no default.cfg line, so it survives across
 * Com_Init() calls unless a new -data is supplied; reset it explicitly
 * wherever a scenario relies on it being empty.
 */
static void test_missing_data_dir_is_explicit_error(void) {
    LPCSTR argv[] = { "test_bz_runtime" };
    bzRuntimeArgs_t args = { 1, argv };

    reset_counters();
    Cvar_Set("data", "");

    bzRuntimeInitResult_t result = BZ_RuntimeInit(&args);

    ASSERT_EQ_INT(result, BZ_RUNTIME_INIT_ERR_NO_DATA_DIR);
    ASSERT_STR_EQ(BZ_RuntimeInitResultString(result), "no data directory supplied (-data <folder>)");
    ASSERT_EQ_INT(cl_init_calls, 0);
    ASSERT_EQ_INT(sv_init_calls, 0);
    /* No resources were ever acquired (NET_Init() is only reached after the
     * data-directory check), so a frame tick must be a safe no-op. */
    ASSERT(!BZ_RuntimeFrame(16));
    ASSERT_EQ_INT(cl_frame_calls, 0);
}

static void test_bad_data_dir_is_explicit_error(void) {
    LPCSTR argv[] = { "test_bz_runtime", "-data", "build/tests/does-not-exist" };
    bzRuntimeArgs_t args = { 3, argv };

    reset_counters();

    bzRuntimeInitResult_t result = BZ_RuntimeInit(&args);

    ASSERT_EQ_INT(result, BZ_RUNTIME_INIT_ERR_DATA_DIR_ADD_FAILED);
    ASSERT_EQ_INT(cl_init_calls, 0);
    ASSERT_EQ_INT(sv_init_calls, 0);
}

static void test_menu_mode_runs_client_only_until_shutdown(void) {
    LPCSTR argv[] = { "test_bz_runtime", "-data", "build/tests", "+com_frame_limit", "0" };
    bzRuntimeArgs_t args = { 5, argv };

    reset_counters();

    ASSERT_EQ_INT(BZ_RuntimeInit(&args), BZ_RUNTIME_INIT_OK);
    ASSERT_EQ_INT(cls.key_dest, key_menu);
    ASSERT_EQ_INT(cls.state, ca_disconnected);
    ASSERT_EQ_INT(cl_init_calls, 1);
    /* Menu mode: no map, no connect target, so no local server comes up. */
    ASSERT_EQ_INT(sv_init_calls, 0);

    ASSERT(BZ_RuntimeFrame(16));
    ASSERT(BZ_RuntimeFrame(16));
    ASSERT_EQ_INT(cl_frame_calls, 2);
    ASSERT_EQ_INT(sys_quit_calls, 0);

    BZ_RuntimeShutdown();
    ASSERT_EQ_INT(cl_shutdown_calls, 1);
    ASSERT_EQ_INT(sv_shutdown_calls, 1);

    /* Idempotent: a second explicit shutdown call must be a no-op. */
    BZ_RuntimeShutdown();
    ASSERT_EQ_INT(cl_shutdown_calls, 1);
    ASSERT_EQ_INT(sv_shutdown_calls, 1);

    /* Once shut down, frames must stay inert (no more CL_Frame calls, and
     * BZ_RuntimeFrame() reports "stop calling me"). */
    ASSERT(!BZ_RuntimeFrame(16));
    ASSERT_EQ_INT(cl_frame_calls, 2);
}

static void test_bare_map_command_starts_selected_map(void) {
    LPCSTR argv[] = { "test_bz_runtime", "-data", "build/tests", "+com_frame_limit", "0" };
    bzRuntimeArgs_t args = { 5, argv };

    reset_counters();
    ASSERT_EQ_INT(BZ_RuntimeInit(&args), BZ_RUNTIME_INIT_OK);
    ASSERT(BZ_RuntimeExecuteCommand("map \"Maps\\Campaign\\Human02.w3m\""));
    ASSERT_EQ_INT(cl_begin_loading_map_calls, 1);
    ASSERT_EQ_INT(sv_map_calls, 1);
    ASSERT_STR_EQ(last_loading_map, "Maps\\Campaign\\Human02.w3m");
    ASSERT_STR_EQ(last_sv_map, "Maps\\Campaign\\Human02.w3m");
    BZ_RuntimeShutdown();
    ASSERT(!BZ_RuntimeExecuteCommand("map Human02"));
}

static void test_listen_server_mode_resolves_map_and_stops_at_frame_limit(void) {
    LPCSTR argv[] = {
        "test_bz_runtime", "-data", "build/tests",
        "+map", "Human02", "+com_frame_limit", "3",
    };
    bzRuntimeArgs_t args = { 7, argv };

    reset_counters();

    ASSERT_EQ_INT(BZ_RuntimeInit(&args), BZ_RUNTIME_INIT_OK);
    /* +map resolves through the same short-name resolver as Com_Map_f, then
     * hands the resolved MPQ-internal path to both the client loading
     * screen and the server, mirroring the pre-refactor main() sequence. */
    ASSERT_STR_EQ(last_loading_map, "Maps\\Campaign\\Human02.w3m");
    ASSERT_STR_EQ(last_sv_map, "Maps\\Campaign\\Human02.w3m");
    ASSERT_EQ_INT(scr_update_screen_calls, 1);
    ASSERT_EQ_INT(sv_init_calls, 1);
    ASSERT_EQ_INT(cl_init_calls, 1);
    ASSERT_EQ_INT(cl_connect_calls, 0);

    ASSERT(BZ_RuntimeFrame(16));
    ASSERT(BZ_RuntimeFrame(16));
    ASSERT_EQ_INT(sys_quit_calls, 0);
    /* Third frame reaches com_frame_limit=3: BZ_RuntimeFrame() must call
     * Com_Quit() (which tears the runtime down and calls Sys_Quit()) and
     * report false so the caller's loop stops. */
    ASSERT(!BZ_RuntimeFrame(16));
    ASSERT_EQ_INT(cl_frame_calls, 3);
    ASSERT_EQ_INT(sys_quit_calls, 1);
    ASSERT_EQ_INT(cl_shutdown_calls, 1);
    ASSERT_EQ_INT(sv_shutdown_calls, 1);

    /* Frame limit shutdown already ran; further frames must not re-run
     * CL_Frame or call Com_Quit()/Sys_Quit() again. */
    ASSERT(!BZ_RuntimeFrame(16));
    ASSERT_EQ_INT(cl_frame_calls, 3);
    ASSERT_EQ_INT(sys_quit_calls, 1);
}

static void test_remote_connect_mode_skips_local_map_load(void) {
    LPCSTR argv[] = {
        "test_bz_runtime", "-data", "build/tests",
        "+connect", "203.0.113.5", "+com_frame_limit", "0",
    };
    bzRuntimeArgs_t args = { 7, argv };

    reset_counters();

    ASSERT_EQ_INT(BZ_RuntimeInit(&args), BZ_RUNTIME_INIT_OK);
    ASSERT_EQ_INT(cl_connect_calls, 1);
    ASSERT_STR_EQ(last_connect_host, "203.0.113.5");
    /* Remote-client mode still brings the local server module up (matching
     * the pre-refactor main() flow's unconditional "if (!menu_mode)
     * SV_Init()" — preserved as-is by this layer, not something to "fix"
     * here), but never loads a local map or connects a listen server. */
    ASSERT_EQ_INT(sv_init_calls, 1);
    ASSERT_EQ_INT(cl_begin_loading_map_calls, 0);
    ASSERT_EQ_INT(sv_map_calls, 0);

    BZ_RuntimeShutdown();
}

static void test_dedicated_mode_without_map_fails_and_tears_itself_down(void) {
    LPCSTR argv[] = { "test_bz_runtime", "-data", "build/tests", "+dedicated", "1" };
    bzRuntimeArgs_t args = { 5, argv };

    reset_counters();

    bzRuntimeInitResult_t result = BZ_RuntimeInit(&args);

    ASSERT_EQ_INT(result, BZ_RUNTIME_INIT_ERR_DEDICATED_REQUIRES_MAP);
    ASSERT_STR_EQ(BZ_RuntimeInitResultString(result), "dedicated server requires +map <map>");
    /* SV_Init() is only reached after the +map check, so it must not run. */
    ASSERT_EQ_INT(sv_init_calls, 0);
    ASSERT_EQ_INT(cl_init_calls, 0);
    /* NET_Init() already ran by this point (see bz_runtime.c comment on
     * ordering), so the error path must call BZ_RuntimeShutdown() itself to
     * avoid leaking it — verified indirectly: a frame tick afterward must
     * stay inert. */
    ASSERT(!BZ_RuntimeFrame(16));
    ASSERT_EQ_INT(cl_frame_calls, 0);
    ASSERT_EQ_INT(sv_frame_calls, 0);
}

static void test_dedicated_mode_with_map_runs_headless(void) {
    LPCSTR argv[] = {
        "test_bz_runtime", "-data", "build/tests",
        "+dedicated", "1", "+map", "Human02", "+com_frame_limit", "0",
    };
    bzRuntimeArgs_t args = { 9, argv };

    reset_counters();

    ASSERT_EQ_INT(BZ_RuntimeInit(&args), BZ_RUNTIME_INIT_OK);
    ASSERT_EQ_INT(cls.key_dest, key_game);
    ASSERT_EQ_INT(sv_init_calls, 1);
    ASSERT_STR_EQ(last_sv_map, "Maps\\Campaign\\Human02.w3m");
    /* Dedicated mode never brings up the client stack (no SDL window, no
     * renderer, no UI — see bz_runtime.c's dedicated branch). */
    ASSERT_EQ_INT(cl_init_calls, 0);
    ASSERT_EQ_INT(scr_update_screen_calls, 0);
    ASSERT_EQ_INT(cl_begin_loading_map_calls, 0);

    ASSERT(BZ_RuntimeFrame(16));
    /* CL_Frame() must never run in dedicated mode, even mid-frame-loop. */
    ASSERT_EQ_INT(cl_frame_calls, 0);

    BZ_RuntimeShutdown();
    ASSERT_EQ_INT(sv_shutdown_calls, 1);
    /* Dedicated mode must not call CL_Shutdown() (no client was started). */
    ASSERT_EQ_INT(cl_shutdown_calls, 0);
}

void run_bz_runtime_tests(void) {
    RUN_TEST(test_missing_data_dir_is_explicit_error);
    RUN_TEST(test_bad_data_dir_is_explicit_error);
    RUN_TEST(test_menu_mode_runs_client_only_until_shutdown);
    RUN_TEST(test_bare_map_command_starts_selected_map);
    RUN_TEST(test_listen_server_mode_resolves_map_and_stops_at_frame_limit);
    RUN_TEST(test_remote_connect_mode_skips_local_map_load);
    RUN_TEST(test_dedicated_mode_without_map_fails_and_tears_itself_down);
    RUN_TEST(test_dedicated_mode_with_map_runs_headless);
}
