#include "../client/client.h"
#include "server/server.h"
#include "bz_runtime.h"

#include <SDL2/SDL.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/select.h>
#include <unistd.h>

#if defined(__APPLE__)
#define BZ_PLATFORM "Darwin"
#elif defined(_WIN32)
#define BZ_PLATFORM "Windows"
#elif defined(__linux__)
#define BZ_PLATFORM "Linux"
#elif defined(__OpenBSD__)
#define BZ_PLATFORM "OpenBSD"
#else
#define BZ_PLATFORM "Unknown"
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define BZ_ARCH "arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#define BZ_ARCH "x86_64"
#elif defined(__i386__) || defined(_M_IX86)
#define BZ_ARCH "x86"
#else
#define BZ_ARCH "unknown"
#endif

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BZ_BYTE_ORDER "big endian"
#else
#define BZ_BYTE_ORDER "little endian"
#endif

#define USAGE \
"Usage:\n" \
"  openwarcraft3 -data <folder> +map <map>       (listen server + local client)\n" \
"  openwarcraft3 -data <folder> +dedicated 1 +map <map>  (dedicated server, no client)\n" \
"  openwarcraft3 -data <folder>                 (client menu)\n" \
"  openwarcraft3 -data <folder> -connect <host>  (remote client, default port " \
                                                    PORT_SERVER_STRING ")\n" \
"  openwarcraft3 -data <folder> -connect <host:port>\n" \
"  openwarcraft3 -data <folder> -tft             (mount expansion MPQs)\n" \
"\n" \
"Examples:\n" \
"  openwarcraft3 -data /home/user/Warcraft3 +map Maps\\\\Campaign\\\\Human02.w3m\n" \
"  openwarcraft3 -data /home/user/Warcraft3 +dedicated 1 +map Maps\\\\Campaign\\\\Human02.w3m\n" \
"  openwarcraft3 -data /home/user/Warcraft3 -tft +menu_single_player_campaign\n" \
"  openwarcraft3 -data /home/user/Warcraft3 -connect 192.168.1.10\n" \
"\n" \
"Notes:\n" \
"  - The data folder should contain Warcraft III MPQs and optionally Maps/.\n" \
"  - Expansion MPQs are skipped by default; use -tft or +fs_expansion 1 to mount them.\n" \
"  - The data folder may also be saved as data in the generated per-build config.\n" \
"  - The map path uses the internal MPQ path format; use +map to launch one.\n" \
"  - Remote clients still need the game data for asset loading.\n" \
"  - Dedicated mode runs the server headless without renderer, sound, or UI.\n"

extern LPTEXTURE Texture;

void Sys_Quit(void) {
    exit(0);
}

/* Read a line from stdin for dedicated server console input.
 * Returns NULL if no input is available (non-blocking check). */
static LPSTR Sys_ConsoleInput(void) {
    static char line[256];
    static char *pos = line;
    fd_set fds;
    struct timeval tv;
    int ch;

    if (!Cvar_Integer("dedicated", 0)) {
        return NULL;
    }
    /* Non-blocking check: is there data on stdin? */
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) {
        return NULL;
    }
    while (1) {
        ch = fgetc(stdin);
        if (ch == EOF) {
            pos = line;
            return NULL;
        }
        if (ch == '\n') {
            *pos = '\0';
            pos = line;
            return line;
        }
        if (pos < line + sizeof(line) - 1) {
            *pos++ = (char)ch;
        }
    }
}

/*
 * main() is a thin desktop wrapper: everything reusable across hosts
 * (cvar/filesystem/client/server bring-up, the per-frame SV_Frame/CL_Frame
 * dance, and orderly shutdown) lives in bz_runtime.c. This file keeps only
 * what is genuinely desktop-specific: the startup banner and -data usage
 * text, SDL_GetTicks() as the frame time source, and dedicated-mode stdin
 * console input (select()/fgetc(), which has no equivalent on an
 * embeddable host with no terminal). */
int main(int argc, LPSTR argv[]) {
    fprintf(stderr,
            "\nOpenWarcraft3\n"
            "Platform: %s\n"
            "Architecture: %s\n"
            "Byte ordering: %s\n\n",
            BZ_PLATFORM,
            BZ_ARCH,
            BZ_BYTE_ORDER);

    bzRuntimeArgs_t runtimeArgs = { argc, (LPCSTR *)argv };
    bzRuntimeInitResult_t initResult = BZ_RuntimeInit(&runtimeArgs);
    if (initResult != BZ_RUNTIME_INIT_OK) {
        if (initResult == BZ_RUNTIME_INIT_ERR_NO_DATA_DIR) {
            printf(USAGE);
        }
        /* All other failure reasons are already reported to stderr by
         * BZ_RuntimeInit() itself, matching the previous inline messages. */
        return 1;
    }

    fprintf(stderr, "OpenWarcraft3 initialized.\n\n");

    bool dedicated = Cvar_Integer("dedicated", 0) != 0;
    DWORD startTime = SDL_GetTicks();
    bool running = true;
    while (running) {
        DWORD currentTime = SDL_GetTicks();
        DWORD msec = currentTime - startTime;
        startTime = currentTime;

        running = BZ_RuntimeFrame(msec);

        if (dedicated) {
            /* Dedicated server: read console commands from stdin. */
            LPSTR cmd = Sys_ConsoleInput();
            if (cmd && *cmd) {
                Cbuf_AddText(cmd);
                Cbuf_AddText("\n");
                Cbuf_Execute();
            }
        }
    }

    return 0;
}
