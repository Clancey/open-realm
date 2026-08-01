/*
 * bz_quest_frame.c - see bz_quest_frame.h.
 */
#include "bz_quest_frame.h"

#include <string.h>

void bz_quest_frame_reset(bzQuestFrame_t *out) {
    memset(out, 0, sizeof(*out));
    out->status = BZ_QUEST_FRAME_NO_SNAPSHOT;
    out->lifecycleState = BZ_TABLETOP_STATE_IDLE;
}

void bz_quest_frame_from_values(const bzQuestFrameValues_t *values, bzQuestFrame_t *out) {
    memset(out, 0, sizeof(*out));

    if (!values->haveSnapshot) {
        out->status = BZ_QUEST_FRAME_NO_SNAPSHOT;
    } else if (values->abiVersion != BZ_TABLETOP_ABI_VERSION) {
        out->status = BZ_QUEST_FRAME_ABI_MISMATCH;
    } else {
        out->status = BZ_QUEST_FRAME_OK;
    }

    out->abiVersion = values->abiVersion;
    out->generation = values->generation;
    out->connState = values->connState;
    out->mapLoaded = values->mapLoaded;
    if (values->mapName) strncpy(out->mapName, values->mapName, sizeof(out->mapName) - 1);
    out->mapBoundsValid = values->mapBoundsValid;
    out->playerValid = values->playerValid;
    out->playerNumber = values->playerNumber;
    out->playerTeam = values->playerTeam;
    out->selectedEntityCount = values->selectedEntityCount;
    out->entityCount = values->entityCount;
    out->entitiesOverflowCount = values->entitiesOverflowCount;
    out->fogPresent = values->fogPresent;
    out->fogWidth = values->fogWidth;
    out->fogHeight = values->fogHeight;
    out->configStringCount = values->configStringCount;
    out->actionLayoutPresent = values->actionLayoutPresent;
    out->lifecycleState = values->lifecycleState;
    if (values->lifecycleError) strncpy(out->lifecycleError, values->lifecycleError, sizeof(out->lifecycleError) - 1);
}

bool bz_quest_frame_should_log(const bzQuestFrame_t *previous, const bzQuestFrame_t *current) {
    if (!previous || !current) return false;
    if (previous->status != current->status) return true;
    if (previous->lifecycleState != current->lifecycleState) return true;
    if (strcmp(previous->lifecycleError, current->lifecycleError) != 0) return true;
    return false;
}
