#include "client/client.h"
#include "common/shared.h"

void S_PlaySoundFile(LPCSTR path);

/* Entity events retain server authority; the native sink only consumes the resolved WAV payload. */
void CL_EntityEvent(entityState_t const *ent) {
    if (!ent->event || !ent->sound || ent->sound >= MAX_SOUNDS)
        return;
    LPCSTR path = cl.configstrings[CS_SOUNDS + ent->sound];
    if (path && *path)
        S_PlaySoundFile(path);
}
