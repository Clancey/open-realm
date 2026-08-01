/*
 * bz_quest_bridge.h - Quest-owned lifecycle/session adapter wrapping the
 * portable platform/tabletop/bridge/bz_tabletop_lifecycle.h engine-thread
 * state machine (layer 4).
 *
 * Deliberately plain C with no Android/OpenXR/Vulkan dependency (uses only
 * fprintf(stderr, ...) for its own diagnostics, mirroring
 * bz_tabletop_lifecycle.c's own portable-diagnostic convention) so this
 * whole adapter - not just isolated helper functions - builds and runs on
 * the host with a plain C compiler against the REAL
 * platform/tabletop/bridge/bz_tabletop_lifecycle.c and the lightweight
 * common/bz_runtime.c stub set, exactly like games/warcraft-3/tests/
 * test_bz_tabletop_lifecycle.c already does (see platform/android/quest/
 * tests/test_bz_quest_bridge.c, which links this file the same way). Only
 * bz_quest_host.c (the real Android translation unit) calls BZ_QUEST_LOGI/E
 * around these functions to route diagnostics to logcat.
 *
 * Ownership: a bzQuestBridge_t owns at most one bzTabletopLifecycle_t at a
 * time. bz_quest_bridge_start() is single-shot per bzQuestBridge_t instance
 * (mirrors BZ_TabletopStart()'s own single-shot-per-lifecycle contract) -
 * call bz_quest_bridge_destroy() and re-run bz_quest_bridge_start() on the
 * same (now zeroed) struct for a fresh attempt (e.g. after a FAILED/STOPPED
 * terminal state), matching this layer's "map reload"/"repeated start-stop"
 * requirement at the bridge level even though the underlying lc itself
 * remains single-shot.
 */
#ifndef BZ_QUEST_BRIDGE_H
#define BZ_QUEST_BRIDGE_H

#include <stdbool.h>

#include "bz_quest_data.h"
#include "platform/tabletop/bridge/bz_tabletop_lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BZ_QUEST_BRIDGE_IDLE = 0,   /* bz_quest_bridge_start() never called on this instance */
    BZ_QUEST_BRIDGE_RUNNING,
    BZ_QUEST_BRIDGE_SUSPENDED,
    BZ_QUEST_BRIDGE_FAILED,     /* data-dir resolution failed, or the underlying lifecycle reached FAILED */
    BZ_QUEST_BRIDGE_STOPPED,
} bzQuestBridgeState_t;

/* Caller must zero-initialize (e.g. via a static/global or memset) before
 * the first bz_quest_bridge_start() call - single-shot, like the
 * bzTabletopLifecycle_t it wraps. */
typedef struct {
    bzTabletopLifecycle_t *lc; /* NULL until BZ_TabletopCreate() succeeds */
    bool startAttempted;       /* bz_quest_bridge_start() has been called once on this instance */
    char dataDir[BZ_QUEST_DATA_DIR_MAX];
    char preLcError[BZ_QUEST_DATA_ERROR_MAX]; /* set only for failures before lc exists; empty otherwise */
} bzQuestBridge_t;

/*
 * Resolves the data directory (bz_quest_data_resolve()), builds argv
 * (bz_quest_data_build_argv()), then BZ_TabletopCreate()+BZ_TabletopStart()
 * - which blocks the calling thread until BZ_RuntimeInit() completes (state
 * leaves STARTING), exactly mirroring bz_quest_ensure_renderer_init()'s
 * existing synchronous-on-APP_CMD_START convention (see bz_quest_host.c).
 * The dedicated engine thread bz_tabletop_lifecycle.c spawns is the ONLY
 * thread that ever calls BZ_RuntimeFrame(); this function's caller (the
 * Android main/UI thread) never touches engine frame functions directly.
 *
 * internalDataPath/externalDataPath should be ANativeActivity's own fields
 * of the same name; mapName may be NULL/empty (no map auto-load - this
 * layer does not implement gameplay/asset rendering, see bz_quest_frame.h).
 *
 * Returns true iff the underlying lifecycle reached RUNNING. Returns false
 * on ANY failure (data-dir resolution failure, BZ_TabletopCreate() OOM, or
 * the lifecycle reaching FAILED) - see bz_quest_bridge_last_error() for the
 * exact reason in every case. Rejects (returns false, logs via
 * fprintf(stderr, ...), does nothing else) a second call on an instance
 * that already attempted a start.
 */
bool bz_quest_bridge_start(bzQuestBridge_t *bridge, const char *internalDataPath,
                            const char *externalDataPath, const char *mapName);

/* No-ops if the bridge never reached RUNNING/SUSPENDED - forwards directly
 * to BZ_TabletopSuspend()/BZ_TabletopResume(). */
void bz_quest_bridge_suspend(bzQuestBridge_t *bridge);
void bz_quest_bridge_resume(bzQuestBridge_t *bridge);

/* Forwards to BZ_TabletopStop() (blocks until the engine thread is joined;
 * safe to call multiple times, from any state, including before a start
 * was ever attempted - matches BZ_TabletopStop(NULL)'s own no-op safety). */
void bz_quest_bridge_stop(bzQuestBridge_t *bridge);

/*
 * Stops (if needed) and destroys the underlying lifecycle, then fully
 * zeroes *bridge so the same storage can be reused for a fresh
 * bz_quest_bridge_start() call later (a "map reload" or "restart" spins up
 * an entirely new bzTabletopLifecycle_t rather than attempting to resume a
 * terminal one - see this header's own comment above). Safe to call on a
 * never-started or already-destroyed bridge.
 */
void bz_quest_bridge_destroy(bzQuestBridge_t *bridge);

/* Derives the bridge-level state from bridge->startAttempted/lc's live
 * BZ_TabletopGetState() - never cached, so this always reflects the engine
 * thread's latest self-transition (e.g. a frame-limit/console "quit"). */
bzQuestBridgeState_t bz_quest_bridge_state(const bzQuestBridge_t *bridge);

/* True iff bz_quest_bridge_state() is FAILED or STOPPED - i.e. no further
 * progress is possible on this instance; the caller (bz_quest_host.c's main
 * loop) must treat this the same as any other exit trigger and fall
 * through to the one teardown code path (see bz_quest_bridge_destroy()). */
bool bz_quest_bridge_is_terminal(const bzQuestBridge_t *bridge);

/* Returns the human-readable reason for the current FAILED state (data-dir
 * resolution failure surfaced via bridge->preLcError, or the underlying
 * lifecycle's own BZ_TabletopLastError()), or NULL if there is none. */
const char *bz_quest_bridge_last_error(const bzQuestBridge_t *bridge);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_BRIDGE_H */
