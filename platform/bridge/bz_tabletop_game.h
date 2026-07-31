#ifndef BZ_TABLETOP_GAME_H
#define BZ_TABLETOP_GAME_H

/*
 * Selected-game policy behind the title-neutral transport. Each statically
 * linked game supplies exactly one implementation.
 */
void BZ_GameTabletopInit(void);
void BZ_GameTabletopShutdown(void);
void BZ_GameTabletopPublish(void);

#endif
