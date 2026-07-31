#include <stdio.h>

#include "platform/bridge/bz_tabletop_game.h"

/* Layer 1 deliberately publishes shared snapshots but no false Warcraft-shaped asset data. */
void BZ_GameTabletopInit(void) {
    fprintf(stderr, "BZTabletopSC2: snapshot transport active; asset publication is not linked in Layer 1\n");
}

void BZ_GameTabletopShutdown(void) {}
void BZ_GameTabletopPublish(void) {}
