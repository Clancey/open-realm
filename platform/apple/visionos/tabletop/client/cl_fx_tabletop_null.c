/*
 * cl_fx_tabletop_null.c - headless entity-event glue for the visionOS
 * tabletop client (Layer 2).
 *
 * Real client/cl_fx.c's CL_EntityEvent() plays sounds/particle effects for
 * entity events (footsteps, spell casts, etc.) via S_PlaySoundFile(),
 * declared only in the SDL-audio-dependent sound/s_local.h. Audio and
 * particle effects are explicitly out of scope for this layer (see
 * AGENTS.md); client/cl_parse.c calls this unconditionally while parsing
 * entity state, so it must exist, but does nothing here - the entity event
 * itself is still visible to the transport via entityState_t.event, which
 * BZ_TT_PublishSnapshotFromClient() reads directly from cl.ents[].current.
 */
#include "client/client.h"

void CL_EntityEvent(entityState_t const *ent) { (void)ent; }
