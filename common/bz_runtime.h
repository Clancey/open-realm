#ifndef __bz_runtime_h__
#define __bz_runtime_h__

#include "shared.h"

/*
 * bz_runtime — reusable engine lifecycle (init / frame / shutdown), factored
 * out of the desktop main() loop in main.c so any host (desktop main(), a
 * visionOS Objective-C++ lifecycle bridge, future embedders) can drive the
 * same client/server/network bring-up without duplicating it.
 *
 * Threading contract: like the rest of the Quake-style engine, this module
 * is single-threaded — callers must serialize BZ_RuntimeInit/Frame/Shutdown
 * calls onto one thread (a host that owns a dedicated engine thread must not
 * call these concurrently from its main thread).
 */

// Grouped argv/argc so BZ_RuntimeInit keeps a single parameter regardless of
// how many command-line-shaped inputs a future host wants to pass through.
typedef struct {
    int argc;
    LPCSTR *argv;
} bzRuntimeArgs_t;

typedef enum {
    BZ_RUNTIME_INIT_OK = 0,
    BZ_RUNTIME_INIT_ERR_NO_DATA_DIR,          // -data <folder> was not supplied
    BZ_RUNTIME_INIT_ERR_DATA_DIR_ADD_FAILED,  // FS_AddDataDirectory() rejected -data
    BZ_RUNTIME_INIT_ERR_MAP_RESOLVE_FAILED,   // +map argument did not resolve to an archive path
    BZ_RUNTIME_INIT_ERR_DEDICATED_REQUIRES_MAP, // +dedicated 1 without +map
} bzRuntimeInitResult_t;

// common/bz_runtime.c
bzRuntimeInitResult_t BZ_RuntimeInit(const bzRuntimeArgs_t *args);
LPCSTR BZ_RuntimeInitResultString(bzRuntimeInitResult_t result);
bool BZ_RuntimeFrame(DWORD elapsed_msec);
bool BZ_RuntimeExecuteCommand(LPCSTR command);
void BZ_RuntimeShutdown(void);

#endif
