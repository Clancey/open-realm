/*
 * cl_null.c - headless client stack for the visionOS tabletop engine archive.
 *
 * The xrsimulator/xros static engine libraries (see Makefile's
 * BZ_XR_ rules) link the real common/ and server/ sources, but must NOT
 * pull in client/cl_input.c (SDL_PollEvent every frame), renderer/r_main.c
 * (SDL/OpenGL window+context), or sound/s_sound.c: this build target must
 * not create a window, poll SDL input, or open an audio device (see
 * docs/visionos-tabletop.md). This file supplies the small CL_/Key_
 * symbol surface that common/bz_runtime.c and common/common.c call
 * unconditionally, mirroring the Quake 2 cl_null.c convention referenced
 * in bz_runtime.c's own comments.
 *
 * Every hook here is an explicit, named null backend: CL_Init logs once
 * that it is active, and any hook that would otherwise silently drop a
 * caller-visible request (connect, map load, forwarded command) logs that
 * event. CL_Frame/SCR_UpdateScreen intentionally do NOT log per call -
 * they run every frame, and a per-frame log would violate the "never log
 * per-frame" convention while adding no diagnostic value beyond CL_Init's
 * one-time notice. A later layer (the snapshot bridge / RealityKit tabletop
 * UI) replaces this file with a real client that renders the game state;
 * until then, this is the whole client-side story for the tabletop host.
 */
#include <stdio.h>
#include "common/common.h"
#include "client/client.h"

/* Real cl_main.c normally owns this global; since it is not linked here,
 * the null client supplies it instead (server_static/server are already
 * provided for real by server/sv_main.c, which IS linked). */
struct client_static cls;

static BOOL cl_null_active = false;

void CL_Init(void) {
    if (!cl_null_active) {
        fprintf(stderr, "CL_Init: visionOS tabletop null client active "
                         "(no window, no SDL input polling, no audio)\n");
        cl_null_active = true;
    }
}

void CL_Frame(DWORD msec) {
    (void)msec; /* nothing to poll or render: no input device, no renderer */
}

void CL_Shutdown(void) {
    cl_null_active = false;
}

void CL_Connect(LPCSTR host, unsigned short port) {
    fprintf(stderr, "CL_Connect: null client dropping connect request to %s:%u "
                     "(remote play arrives in a later layer)\n", host, port);
}

void CL_BeginLoadingMap(LPCSTR mapName) {
    fprintf(stderr, "CL_BeginLoadingMap: null client acknowledges map '%s' "
                     "but performs no load (no renderer/asset bridge yet)\n", mapName);
}

void SCR_UpdateScreen(DWORD msec) {
    (void)msec; /* no screen to update: no renderer is linked in this layer */
}

void CL_SetGameplayBindings(void) {
    /* no input device to bind: gameplay controls are out of scope for this layer */
}

void Key_Init(void) {
    /* no keyboard: nothing to initialize */
}

void Key_WriteBindings(FILE *file) {
    (void)file; /* no bindings exist to persist */
}

void Cmd_ForwardToServer(LPCSTR text) {
    fprintf(stderr, "Cmd_ForwardToServer: null client dropping unrecognized command '%s'\n", text);
}
