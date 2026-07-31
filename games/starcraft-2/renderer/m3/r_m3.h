#ifndef R_M3_H
#define R_M3_H

#include "games/starcraft-2/common/sc2_m3.h"

m3Model_t *R_LoadModelM3(void *buffer, DWORD size);
void R_FreeModelM3(m3Model_t *model);

#endif
