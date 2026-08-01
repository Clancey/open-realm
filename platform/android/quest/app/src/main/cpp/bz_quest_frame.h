/*
 * bz_quest_frame.h - Quest-owned plain-C diagnostic frame descriptor for the
 * tabletop transport snapshot + lifecycle state (layer 4).
 *
 * This is NOT a renderable frame - no Warcraft asset/terrain translation or
 * drawing happens here (out of scope for this layer, see docs/
 * quest-tabletop.md). It exists solely to prove, in a host-testable and
 * diagnosable way, that snapshots really do advance once the engine thread
 * is running, without pretending full rendering exists.
 *
 * bz_quest_frame_from_values() and bz_quest_frame_should_log() are pure
 * (no I/O, no BZ_TT_ or BZ_Tabletop_ calls) and covered by platform/android/
 * quest/tests/test_bz_quest_frame.c on the host with a plain C compiler.
 * The one real snapshot-reading caller (bz_quest_snapshot_capture(), see
 * bz_quest_snapshot.h) lives in its own translation unit precisely so this
 * file can stay link-clean of platform/bridge/bz_tabletop_transport.c's
 * heavy engine dependencies (client/server/net/cmodel) while still sharing
 * bz_tabletop_transport.h's/bz_tabletop_lifecycle.h's plain, portable enum/
 * struct *declarations* (header-only - no link-time cost).
 */
#ifndef BZ_QUEST_FRAME_H
#define BZ_QUEST_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include "platform/bridge/bz_tabletop_transport.h"
#include "platform/tabletop/bridge/bz_tabletop_lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_FRAME_MAP_NAME_MAX = BZ_TT_MAX_CONFIGSTRING_LEN,
    BZ_QUEST_FRAME_ERROR_MAX = 256,
};

typedef enum {
    BZ_QUEST_FRAME_NO_SNAPSHOT = 0, /* BZ_TT_Latest() returned NULL - nothing published yet */
    BZ_QUEST_FRAME_ABI_MISMATCH,    /* snapshot's abiVersion != BZ_TABLETOP_ABI_VERSION this host built against */
    BZ_QUEST_FRAME_OK,
} bzQuestFrameStatus_t;

/* Input values for bz_quest_frame_from_values() - grouped per AGENTS.md's
 * ">3 parameters get an input struct" convention. Mirrors a small subset of
 * bzTTSnapshot_t's accessors (see bz_quest_snapshot.c, the one real
 * populator of this struct) plus the underlying lifecycle's own state/error,
 * which is not itself part of any snapshot. "Camera state" per this layer's
 * task scope means bzTTPlayer_t's fields below (WC3 tabletop has no
 * separate camera ABI - the XR head pose is tracked independently by
 * bz_quest_xr.c, out of scope for this descriptor) - no ABI widening
 * needed; v3 already exposes everything this layer requires. */
typedef struct {
    bool haveSnapshot;         /* false iff BZ_TT_Latest() returned NULL */
    uint32_t abiVersion;       /* only meaningful if haveSnapshot */
    uint64_t generation;
    bzTTConnState_t connState;
    bool mapLoaded;            /* true iff BZ_TTSnapshot_MapName() reported CS_WORLD is non-empty */
    const char *mapName;       /* may be NULL; only meaningful if mapLoaded */
    bool mapBoundsValid;
    bool playerValid;          /* true iff BZ_TTSnapshot_Player() returned non-NULL */
    uint32_t playerNumber, playerTeam;
    uint32_t selectedEntityCount;
    uint32_t entityCount;
    uint32_t entitiesOverflowCount;
    bool fogPresent;
    uint32_t fogWidth, fogHeight;
    uint32_t configStringCount;
    bool actionLayoutPresent;
    bzTabletopState_t lifecycleState;
    const char *lifecycleError; /* may be NULL (mirrors BZ_TabletopLastError()'s own NULL-means-none contract) */
} bzQuestFrameValues_t;

typedef struct {
    bzQuestFrameStatus_t status;
    uint32_t abiVersion;
    uint64_t generation;
    bzTTConnState_t connState;
    bool mapLoaded;
    char mapName[BZ_QUEST_FRAME_MAP_NAME_MAX];
    bool mapBoundsValid;
    bool playerValid;
    uint32_t playerNumber, playerTeam;
    uint32_t selectedEntityCount;
    uint32_t entityCount;
    uint32_t entitiesOverflowCount;
    bool fogPresent;
    uint32_t fogWidth, fogHeight;
    uint32_t configStringCount;
    bool actionLayoutPresent;
    bzTabletopState_t lifecycleState;
    char lifecycleError[BZ_QUEST_FRAME_ERROR_MAX]; /* empty iff no error */
} bzQuestFrame_t;

/* Resets *out to the "never captured a frame yet" value: status =
 * NO_SNAPSHOT, lifecycleState = IDLE, every other field zeroed/empty. Use
 * to initialize the "previous frame" state bz_quest_frame_should_log()
 * compares against before the first real capture. */
void bz_quest_frame_reset(bzQuestFrame_t *out);

/*
 * Assembles *out from already-extracted plain values: computes `status`
 * (NO_SNAPSHOT if !values->haveSnapshot; ABI_MISMATCH if haveSnapshot but
 * values->abiVersion != BZ_TABLETOP_ABI_VERSION; OK otherwise) and copies
 * every other field, bounding mapName/lifecycleError into out's fixed
 * buffers (truncating, never overflowing - see BZ_TTSnapshot_MapName()'s
 * own cap-bounded contract this mirrors).
 */
void bz_quest_frame_from_values(const bzQuestFrameValues_t *values, bzQuestFrame_t *out);

/*
 * Decides whether the host should emit a diagnostic log line this poll.
 * Never true merely because a frame ran, and NEVER true merely because
 * `generation` advanced: BZ_TT_PublishSnapshotFromClient() bumps generation
 * on every engine frame (see platform/bridge/bz_tabletop_transport.c and
 * platform/tabletop/client/cl_scrn_tabletop_null.c's SCR_UpdateScreen()),
 * so a bare generation-advance trigger would log at the engine's frame
 * rate (tens of times per second) - exactly the per-frame logging AGENTS.md
 * and docs/quest-tabletop.md forbid. Returns true iff any of: the snapshot
 * status changed (first-ever snapshot proves generation is already
 * advancing, or an ABI mismatch newly appearing/clearing), the lifecycle
 * state changed, or the lifecycle error text appeared/changed/cleared.
 * `generation` itself is still copied into bzQuestFrame_t for whoever reads
 * the descriptor directly - only the automatic log trigger excludes it.
 * Returns false if either pointer is NULL (nothing to compare against
 * yet).
 */
bool bz_quest_frame_should_log(const bzQuestFrame_t *previous, const bzQuestFrame_t *current);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_FRAME_H */
