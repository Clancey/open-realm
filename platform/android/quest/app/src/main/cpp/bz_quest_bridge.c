/*
 * bz_quest_bridge.c - see bz_quest_bridge.h.
 */
#include "bz_quest_bridge.h"

#include <stdio.h>
#include <string.h>

bool bz_quest_bridge_start(bzQuestBridge_t *bridge, const char *internalDataPath,
                            const char *externalDataPath, const char *mapName) {
    if (bridge->startAttempted) {
        fprintf(stderr, "bz_quest_bridge_start: already attempted on this instance; "
                        "destroy and start a fresh bzQuestBridge_t to run again\n");
        return false;
    }
    bridge->startAttempted = true;

    if (!bz_quest_data_resolve(internalDataPath, externalDataPath, bridge->dataDir, sizeof(bridge->dataDir),
                                bridge->preLcError, sizeof(bridge->preLcError))) {
        fprintf(stderr, "bz_quest_bridge_start: %s\n", bridge->preLcError);
        return false;
    }

    /* Detection failure (directory not yet present/creatable) is not itself
     * a start failure here - edition simply stays at its safe ROC default
     * and FS_AddDataDirectory() inside BZ_TabletopStart() below remains the
     * authoritative existence/archive check, surfacing its own actionable
     * "Failed to add data directory" error if the directory is genuinely
     * unusable (see bz_quest_data_detect_edition()'s doc comment). */
    bzQuestDataEdition_t edition = BZ_QUEST_DATA_EDITION_ROC;
    bz_quest_data_detect_edition(bridge->dataDir, &edition);
    fprintf(stderr, "bz_quest_bridge_start: resolved data dir '%s', edition=%s\n", bridge->dataDir,
            edition == BZ_QUEST_DATA_EDITION_TFT ? "tft" : "roc");

    char storage[BZ_QUEST_DATA_ARGV_MAX][BZ_QUEST_DATA_DIR_MAX];
    const char *argv[BZ_QUEST_DATA_ARGV_MAX];
    int argc = bz_quest_data_build_argv(bridge->dataDir, edition, mapName, storage, argv, BZ_QUEST_DATA_ARGV_MAX);
    if (!argc) {
        snprintf(bridge->preLcError, sizeof(bridge->preLcError),
                 "bz_quest_data_build_argv failed for data dir '%s'", bridge->dataDir);
        fprintf(stderr, "bz_quest_bridge_start: %s\n", bridge->preLcError);
        return false;
    }

    bridge->lc = BZ_TabletopCreate(argc, argv);
    if (!bridge->lc) {
        snprintf(bridge->preLcError, sizeof(bridge->preLcError), "BZ_TabletopCreate failed (out of memory)");
        fprintf(stderr, "bz_quest_bridge_start: %s\n", bridge->preLcError);
        return false;
    }

    /* Blocks the calling thread until BZ_RuntimeInit() completes on the one
     * dedicated engine thread bz_tabletop_lifecycle.c spawns - this thread
     * (the Android main/UI thread, see bz_quest_host.c) never calls an
     * engine frame function itself. */
    BZ_TabletopStart(bridge->lc);
    return BZ_TabletopGetState(bridge->lc) == BZ_TABLETOP_STATE_RUNNING;
}

void bz_quest_bridge_suspend(bzQuestBridge_t *bridge) {
    if (bridge->lc) BZ_TabletopSuspend(bridge->lc);
}

void bz_quest_bridge_resume(bzQuestBridge_t *bridge) {
    if (bridge->lc) BZ_TabletopResume(bridge->lc);
}

void bz_quest_bridge_stop(bzQuestBridge_t *bridge) {
    if (bridge->lc) BZ_TabletopStop(bridge->lc);
}

void bz_quest_bridge_destroy(bzQuestBridge_t *bridge) {
    if (bridge->lc) BZ_TabletopDestroy(bridge->lc); /* BZ_TabletopDestroy() calls Stop() first if needed */
    memset(bridge, 0, sizeof(*bridge));
}

bzQuestBridgeState_t bz_quest_bridge_state(const bzQuestBridge_t *bridge) {
    if (!bridge->startAttempted) return BZ_QUEST_BRIDGE_IDLE;
    if (!bridge->lc) return BZ_QUEST_BRIDGE_FAILED; /* data-dir/argv resolution failed before a lifecycle existed */

    switch (BZ_TabletopGetState(bridge->lc)) {
        case BZ_TABLETOP_STATE_RUNNING:   return BZ_QUEST_BRIDGE_RUNNING;
        case BZ_TABLETOP_STATE_SUSPENDED: return BZ_QUEST_BRIDGE_SUSPENDED;
        case BZ_TABLETOP_STATE_FAILED:    return BZ_QUEST_BRIDGE_FAILED;
        case BZ_TABLETOP_STATE_STOPPED:   return BZ_QUEST_BRIDGE_STOPPED;
        /* IDLE/STARTING are never observed here: bz_quest_bridge_start()
         * always calls BZ_TabletopStart() synchronously right after
         * BZ_TabletopCreate(), and that call blocks until STARTING is left. */
        default: return BZ_QUEST_BRIDGE_FAILED;
    }
}

bool bz_quest_bridge_is_terminal(const bzQuestBridge_t *bridge) {
    bzQuestBridgeState_t state = bz_quest_bridge_state(bridge);
    return state == BZ_QUEST_BRIDGE_FAILED || state == BZ_QUEST_BRIDGE_STOPPED;
}

const char *bz_quest_bridge_last_error(const bzQuestBridge_t *bridge) {
    if (!bridge->lc) return bridge->preLcError[0] ? bridge->preLcError : NULL;
    return BZ_TabletopLastError(bridge->lc);
}
