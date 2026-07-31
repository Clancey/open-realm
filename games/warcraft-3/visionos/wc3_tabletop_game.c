#include "platform/bridge/bz_tabletop_game.h"
#include "platform/bridge/bz_tabletop_assets.h"

/* Warcraft owns the current asset and W3E terrain ABI. */
void BZ_GameTabletopInit(void) { BZ_TTA_Init(); }
void BZ_GameTabletopShutdown(void) { BZ_TTA_Shutdown(); }
void BZ_GameTabletopPublish(void) { BZ_TTA_PublishTerrainFromGame(); }
