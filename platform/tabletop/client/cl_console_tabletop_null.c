/*
 * cl_console_tabletop_null.c - headless developer console glue for the
 * shared tabletop client (see platform/tabletop/client), linked by every
 * native host (visionOS today; Android/Meta Quest later).
 *
 * Real client/console.c pulls in <SDL2/SDL.h> (for text-input handling)
 * and stores CON_printf() output into an in-memory ring buffer that is
 * only ever drawn by the excluded cl_scrn.c/CON_DrawConsole() - i.e. on
 * desktop, console output is never visible unless the in-game console is
 * opened and drawn. A headless build has no such screen, so instead of
 * reimplementing that (pointless) ring buffer, this sends CON_printf()
 * straight to stderr - genuinely more useful here than the original
 * behavior, not a stand-in for it. client/cl_main.c calls CON_Init()
 * unconditionally at startup; nothing here needs to happen since there is
 * no console input/history/toggle state to set up.
 */
#include <stdarg.h>
#include <stdio.h>

#include "client/client.h"

void CON_printf(LPCSTR fmt, ...) {
    va_list argptr;
    va_start(argptr, fmt);
    vfprintf(stderr, fmt, argptr);
    va_end(argptr);
    fputc('\n', stderr);
}

void CON_Init(void) {}
