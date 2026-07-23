#ifndef __bz_tabletop_lifecycle_h__
#define __bz_tabletop_lifecycle_h__

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
 * Threading contract: BZ_TabletopStart() spawns one dedicated engine
 * thread per instance and never runs the engine on the calling thread.
 * Every BZ_RuntimeInit/Frame/Shutdown call happens on that one thread,
 * satisfying bz_runtime.h's "serialize onto one thread" requirement.
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

// Allocates a lifecycle host bound to a deep copy of the given argv-shaped
// arguments (the caller's array/strings do not need to outlive this call).
// Does not start the engine thread — call BZ_TabletopStart for that.
bzTabletopLifecycle_t *BZ_TabletopCreate(int argc, const char **argv);

// Starts the dedicated engine thread and blocks the calling thread until
// BZ_RuntimeInit() has run to completion on it, i.e. until the state has
// left STARTING (either RUNNING or FAILED). No-op if already
// STARTING/RUNNING/SUSPENDED. Safe to call again after FAILED/STOPPED to
// restart: any previous engine thread is joined first.
void BZ_TabletopStart(bzTabletopLifecycle_t *lc);

// Pauses/resumes per-frame ticking without tearing the engine down.
// No-op outside the RUNNING/SUSPENDED states respectively.
void BZ_TabletopSuspend(bzTabletopLifecycle_t *lc);
void BZ_TabletopResume(bzTabletopLifecycle_t *lc);

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

#ifdef __cplusplus
}
#endif

#endif
