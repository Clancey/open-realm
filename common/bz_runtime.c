#include "../client/client.h"
#include "server/server.h"
#include "bz_runtime.h"

/*
 * bz_runtime.c — engine bring-up/frame/teardown shared by every host.
 *
 * This is the same logic previously inlined in common/main.c's main(); it is
 * factored out so a visionOS Objective-C++ lifecycle bridge (or any other
 * embedder) can drive the engine without owning an SDL run loop. Desktop
 * targets (main.c) remain a thin wrapper: read platform time, call
 * BZ_RuntimeFrame(), handle desktop-only concerns (console stdin, the
 * -data usage banner) themselves.
 */

typedef enum {
    BZ_RUNTIME_STATE_IDLE = 0,
    BZ_RUNTIME_STATE_RUNNING,
    BZ_RUNTIME_STATE_SHUTDOWN,
} bzRuntimeInternalState_t;

static bzRuntimeInternalState_t bz_runtime_state = BZ_RUNTIME_STATE_IDLE;
static DWORD bz_runtime_frame_count = 0;

/* game_port validation lived in main.c's Sys_GamePort(); moved here because
 * it is generic cvar bring-up, not platform-specific despite the old name. */
static unsigned short BZ_RuntimeGamePort(void) {
    int port = Cvar_Integer("game_port", PORT_SERVER);

    if (port <= 0 || port > 65535) {
        fprintf(stderr,
                "Invalid game_port %d, using default %u\n",
                port,
                (unsigned)PORT_SERVER);
        Cvar_Set("game_port", PORT_SERVER_STRING);
        port = PORT_SERVER;
    }
    return (unsigned short)port;
}

LPCSTR BZ_RuntimeInitResultString(bzRuntimeInitResult_t result) {
    switch (result) {
        case BZ_RUNTIME_INIT_OK: return "ok";
        case BZ_RUNTIME_INIT_ERR_NO_DATA_DIR: return "no data directory supplied (-data <folder>)";
        case BZ_RUNTIME_INIT_ERR_DATA_DIR_ADD_FAILED: return "failed to add data directory";
        case BZ_RUNTIME_INIT_ERR_MAP_RESOLVE_FAILED: return "failed to resolve +map argument";
        case BZ_RUNTIME_INIT_ERR_DEDICATED_REQUIRES_MAP: return "dedicated server requires +map <map>";
    }
    return "unknown runtime init error";
}

/*
 * BZ_RuntimeInit — one-time engine bring-up: cvars/filesystem, then either
 * the dedicated-server path or the client (+menu/+listen-server/+connect)
 * path, exactly mirroring the desktop main() flow this replaces.
 */
bzRuntimeInitResult_t BZ_RuntimeInit(const bzRuntimeArgs_t *args) {
    bz_runtime_frame_count = 0;

    Com_Init(args->argc, args->argv);

    LPCSTR data_dir = Cvar_String("data", "");
    if (!data_dir || !*data_dir) {
        return BZ_RUNTIME_INIT_ERR_NO_DATA_DIR;
    }
    if (!FS_AddDataDirectory(data_dir)) {
        fprintf(stderr, "Failed to add data directory: %s\n", data_dir);
        return BZ_RUNTIME_INIT_ERR_DATA_DIR_ADD_FAILED;
    }

    LPCSTR extra_data_dir = Cvar_String("extra_data", "");
    if (extra_data_dir && *extra_data_dir) {
        FS_AddDataDirectory(extra_data_dir);
    }

    PATHSTR resolved_map;
    LPCSTR map = Cvar_String("map", "");
    LPCSTR connect_addr = Cvar_String("connect", "");
    bool has_map = map && *map;
    bool has_connect_addr = connect_addr && *connect_addr;
    bool menu_mode = !has_map && !has_connect_addr;
    bool listen_server_mode = has_map && !has_connect_addr;
    unsigned short game_port = BZ_RuntimeGamePort();

    if (has_map) {
        if (!Com_ResolveMapArgument(map, resolved_map, sizeof(resolved_map))) {
            return BZ_RUNTIME_INIT_ERR_MAP_RESOLVE_FAILED;
        }
        map = resolved_map;
    }
    bool dedicated = Cvar_Integer("dedicated", 0) != 0;

    /* Dedicated server mode: follow the Quake 2 convention of running the
     * server without the client stack.  Unlike Quake 2 which uses a
     * compile-time cl_null.c stub, we use runtime checks here because the
     * codebase is not yet structured for a separate dedicated target and
     * runtime branching keeps a single binary for both modes. */
    cls.key_dest = dedicated ? key_game : (menu_mode ? key_menu : key_game);
    cls.state = ca_disconnected;

    NET_Init();
    /* From here on resources exist (at minimum the network layer), so
     * shutdown is meaningful; mark running before any remaining checks can
     * fail so an early-return below still tears NET_Init() back down
     * instead of leaking it. */
    bz_runtime_state = BZ_RUNTIME_STATE_RUNNING;

    if (dedicated) {
        // Dedicated server mode: no client stack, no SDL window.
        if (!has_map) {
            fprintf(stderr, "Dedicated server requires +map <map>\n");
            BZ_RuntimeShutdown();
            return BZ_RUNTIME_INIT_ERR_DEDICATED_REQUIRES_MAP;
        }
        SV_Init();
        fprintf(stderr, "Dedicated server starting on map: %s\n", map);
        /* Call SV_Map directly instead of routing through the 'map' command,
         * because Com_Map_f -> MenuAction -> CL_BeginLoadingMap requires the
         * client stack which is not initialized in dedicated mode. */
        SV_Map(map);
    } else {
        if (!menu_mode) {
            SV_Init();
        }
        CL_Init();
        Cbuf_AddLateCommands();
        Cbuf_Execute();

        if (has_connect_addr) {
            // Remote-client mode: skip the local server, connect over UDP.
            CL_Connect(connect_addr, game_port);
        } else if (listen_server_mode) {
            // Listen-server mode: show the client loading screen before the
            // synchronous server map load, mirroring Quake's loading plaque flow.
            if (!svs.initialized) {
                SV_Init();
            }
            CL_BeginLoadingMap(map);
            SCR_UpdateScreen(0);
            SV_Map(map);
        }
        // Menu mode: UI runs client-side, no server connection needed (Quake 3 pattern)
    }

    return BZ_RUNTIME_INIT_OK;
}

/*
 * BZ_RuntimeFrame — advance server and/or client state by the caller-supplied
 * elapsed milliseconds. The caller owns the time source (SDL_GetTicks() on
 * desktop, a host display link elsewhere) so this stays testable without a
 * real clock and portable to hosts without SDL.
 *
 * Returns false once the engine has shut down (com_frame_limit reached, or
 * a "quit" requested from anywhere — menu, console, key binding — via
 * Com_Quit()), so callers know to stop calling this and exit their loop.
 */
bool BZ_RuntimeFrame(DWORD elapsed_msec) {
    if (bz_runtime_state != BZ_RUNTIME_STATE_RUNNING) {
        return false;
    }

    bool dedicated = Cvar_Integer("dedicated", 0) != 0;
    if (svs.initialized && (sv.state == ss_lobby || sv.state == ss_game)) {
        SV_Frame(elapsed_msec);
    }
    if (!dedicated) {
        CL_Frame(elapsed_msec);
    }

    bz_runtime_frame_count++;
    int frame_limit = Cvar_Integer("com_frame_limit", 0);
    if (frame_limit > 0 && bz_runtime_frame_count >= (DWORD)frame_limit) {
        Com_Quit();
    }

    return bz_runtime_state == BZ_RUNTIME_STATE_RUNNING;
}

/*
 * BZ_RuntimeShutdown — orderly, idempotent teardown. Com_Quit() calls this
 * before Sys_Quit(), and BZ_RuntimeFrame() calls Com_Quit() at the frame
 * limit, so this can run more than once per process (menu "quit", console
 * "quit", frame limit, and an explicit host teardown call may all race to
 * request it) — only the first call does anything.
 */
void BZ_RuntimeShutdown(void) {
    if (bz_runtime_state != BZ_RUNTIME_STATE_RUNNING) {
        return;
    }
    bz_runtime_state = BZ_RUNTIME_STATE_SHUTDOWN;

    if (!Cvar_Integer("dedicated", 0)) {
        CL_Shutdown();
    }
    SV_Shutdown();
    NET_Shutdown();
    FS_Shutdown();
}
