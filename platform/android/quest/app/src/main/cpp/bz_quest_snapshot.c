/*
 * bz_quest_snapshot.c - see bz_quest_snapshot.h.
 */
#include "bz_quest_snapshot.h"

#include <string.h>

#include "platform/bridge/bz_tabletop_transport.h"

void bz_quest_snapshot_capture(const bzTabletopLifecycle_t *lc, bzQuestFrame_t *out) {
    bzQuestFrameValues_t values;
    memset(&values, 0, sizeof(values));
    values.lifecycleState = BZ_TabletopGetState(lc);
    values.lifecycleError = BZ_TabletopLastError(lc);

    char mapNameBuf[BZ_QUEST_FRAME_MAP_NAME_MAX];
    mapNameBuf[0] = '\0';

    const bzTTSnapshot_t *snap = BZ_TT_Latest();
    if (snap) {
        values.haveSnapshot = true;
        values.abiVersion = BZ_TTSnapshot_AbiVersion(snap);
        values.generation = BZ_TTSnapshot_Generation(snap);
        values.connState = BZ_TTSnapshot_ConnState(snap);
        values.mapLoaded = BZ_TTSnapshot_MapName(snap, mapNameBuf, sizeof(mapNameBuf));
        values.mapName = mapNameBuf;

        bzTTBox2_t bounds;
        values.mapBoundsValid = BZ_TTSnapshot_MapBounds(snap, &bounds);

        const bzTTPlayer_t *player = BZ_TTSnapshot_Player(snap);
        values.playerValid = player != NULL;
        if (player) {
            values.playerNumber = player->number;
            values.playerTeam = player->team;
        }

        uint32_t selectedIds[BZ_TT_MAX_SELECTED_ENTITIES];
        values.selectedEntityCount = BZ_TTSnapshot_SelectedEntityIds(snap, selectedIds, BZ_TT_MAX_SELECTED_ENTITIES);
        values.entityCount = BZ_TTSnapshot_EntityCount(snap);
        values.entitiesOverflowCount = BZ_TTSnapshot_EntitiesOverflowCount(snap);

        uint32_t fogWidth = 0, fogHeight = 0;
        values.fogPresent = BZ_TTSnapshot_FogDimensions(snap, &fogWidth, &fogHeight);
        values.fogWidth = fogWidth;
        values.fogHeight = fogHeight;

        values.configStringCount = BZ_TTSnapshot_ConfigStringCount(snap);
        values.actionLayoutPresent = BZ_TTSnapshot_ActionLayout(snap) != NULL;
    }

    bz_quest_frame_from_values(&values, out);

    /* Released on every branch - haveSnapshot false means snap is NULL and
     * there is nothing to release, matching BZ_TT_Latest()'s own contract. */
    if (snap) BZ_TTSnapshot_Release(snap);
}
