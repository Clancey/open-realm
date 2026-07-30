#ifndef __bz_tabletop_lifecycle_h__
#define __bz_tabletop_lifecycle_h__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * bz_tabletop_lifecycle — portable (no Objective-C, no Foundation, no
 * RealityKit) state machine and dedicated-thread driver for hosting the
 * static visionOS WC3 engine archive (see Makefile's xrsimulator/xros
 * targets) from an embedding app.
 *
 * This is the host-independent core that
 * platform/apple/visionos/tabletop/bridge/bz_tabletop_bridge.mm's thin
 * Objective-C++ wrapper calls into. Keeping the actual logic here — plain
 * C, POSIX threads — makes it unit-testable via the normal `make test` C
 * harness on any desktop toolchain, without needing Xcode, a booted
 * simulator, or the Objective-C runtime at all.
 *
 * This header deliberately uses only plain C types (not common/shared.h's
 * LPCSTR/BOOL) so it can be #included directly from Objective-C++ (the
 * .mm wrapper) without colliding with objc/objc.h's own `typedef bool
 * BOOL;` — common/shared.h's `typedef unsigned char BOOL;` is a hard
 * redefinition error when both are visible in the same translation unit.
 * bz_tabletop_lifecycle.c still uses common/bz_runtime.h's real types
 * internally; only this public surface stays ObjC-safe.
 *
 * Threading contract: BZ_TabletopStart() spawns exactly one dedicated,
 * single-shot engine thread per instance and never runs the engine on the
 * calling thread. Every BZ_RuntimeInit/Frame/Shutdown call happens on that
 * one thread, satisfying bz_runtime.h's "serialize onto one thread"
 * requirement. Once that thread exits — whether from an external
 * BZ_TabletopStop() or the engine quitting itself (frame limit / console
 * "quit") — the instance is terminal (FAILED or STOPPED) and cannot be
 * restarted; BZ_TabletopStart() rejects any further call on it. Callers
 * that need to run again must BZ_TabletopCreate() a fresh instance.
 * BZ_TabletopStop() is safe to call from any thread, including
 * re-entrantly from the engine thread itself (Sys_Quit(), defined in
 * bz_tabletop_lifecycle.c, does exactly this when a frame-limit or
 * console "quit" fires) — see BZ_TabletopStop's own comment for how that
 * self-join case is handled without deadlocking.
 */

typedef enum {
    BZ_TABLETOP_STATE_IDLE = 0,
    BZ_TABLETOP_STATE_STARTING,
    BZ_TABLETOP_STATE_RUNNING,
    BZ_TABLETOP_STATE_FAILED,
    BZ_TABLETOP_STATE_SUSPENDED,
    BZ_TABLETOP_STATE_STOPPED,
} bzTabletopState_t;

typedef struct bzTabletopLifecycle_s bzTabletopLifecycle_t;

enum {
    BZ_TABLETOP_MAP_PATH_MAX = 260,
    BZ_TABLETOP_COMMAND_QUEUE_CAPACITY = 8,
};

// Allocates a lifecycle host bound to a deep copy of the given argv-shaped
// arguments (the caller's array/strings do not need to outlive this call).
// Does not start the engine thread — call BZ_TabletopStart for that.
bzTabletopLifecycle_t *BZ_TabletopCreate(int argc, const char **argv);

// Starts the dedicated, single-shot engine thread and blocks the calling
// thread until BZ_RuntimeInit() has run to completion on it, i.e. until
// the state has left STARTING (either RUNNING or FAILED). No-op if
// already STARTING/RUNNING/SUSPENDED. FAILED and STOPPED are terminal:
// once reached, this instance's engine thread has exited for good and
// BZ_TabletopStart() rejects (logs and no-ops) any further call — create
// a new BZ_TabletopCreate() instance to run again.
void BZ_TabletopStart(bzTabletopLifecycle_t *lc);

// Pauses/resumes per-frame ticking without tearing the engine down.
// No-op outside the RUNNING/SUSPENDED states respectively.
void BZ_TabletopSuspend(bzTabletopLifecycle_t *lc);
void BZ_TabletopResume(bzTabletopLifecycle_t *lc);

// Queues a bare in-engine `map "<path>"` command for execution on the
// dedicated engine thread before its next frame. Returns false for malformed
// paths, a full queue, or a lifecycle outside RUNNING/SUSPENDED.
bool BZ_TabletopSubmitMap(bzTabletopLifecycle_t *lc, const char *map);

// Requests an orderly shutdown. If called from a thread other than the
// engine thread, blocks until the engine thread has called
// BZ_RuntimeShutdown() and fully exited, then joins it (state ends up
// STOPPED before this returns). If called FROM the engine thread itself
// (Sys_Quit()'s re-entrant path), only marks the stop request and returns
// immediately — a thread cannot join itself — leaving the actual join for
// a later call to BZ_TabletopStop()/BZ_TabletopDestroy() from another
// thread. Idempotent either way: safe to call multiple times, from any
// state, including after the engine has already stopped.
void BZ_TabletopStop(bzTabletopLifecycle_t *lc);

// Frees a lifecycle host. Calls BZ_TabletopStop() first if needed, so
// callers do not need to stop explicitly before destroying.
void BZ_TabletopDestroy(bzTabletopLifecycle_t *lc);

bzTabletopState_t BZ_TabletopGetState(bzTabletopLifecycle_t const *lc);
// NULL when no initialization error has occurred.
const char *BZ_TabletopLastError(bzTabletopLifecycle_t const *lc);

// Test/diagnostic accessor: how many times the dedicated engine thread has
// actually been spawned for this instance. Should never exceed 1, since
// BZ_TabletopStart() is single-shot per instance and rejects any call
// after the first thread is created (see BZ_TabletopStart's own comment).
// Used by test_bz_tabletop_lifecycle.c to verify a rejected restart truly
// did not spawn a second engine thread, since none of the BZ_RuntimeInit()
// failure paths run CL_Init()/SV_Init() (they all return before reaching
// either), so those stub counters alone can't distinguish "rejected" from
// "silently re-ran and failed again" for the FAILED-terminal case.
int BZ_TabletopEngineThreadSpawnCount(bzTabletopLifecycle_t const *lc);

// Test/diagnostic accessor: how many times this instance's state has
// actually been set to RUNNING. Used by test_bz_tabletop_lifecycle.c to
// prove that a BZ_TabletopStop() call arriving while still STARTING
// prevents RUNNING from ever being published at all (not merely that the
// final observable state is STOPPED, which by itself is also true of the
// bug this accessor exists to catch — the frame loop's first iteration
// would already observe stop_requested and break before running a frame
// either way, so the final state/shutdown-count alone cannot distinguish
// "RUNNING was skipped" from "RUNNING was briefly published, then the loop
// immediately exited without ticking").
int BZ_TabletopRunningPublishCount(bzTabletopLifecycle_t const *lc);

#ifdef __cplusplus
}
#endif

#endif
