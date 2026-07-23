/*
 * s_tabletop_null.c - headless sound backend for the visionOS tabletop
 * client (Layer 2). Audio is explicitly out of scope for this layer (see
 * AGENTS.md); these are named, logged-once no-ops rather than silent
 * stubs, matching the "no hacks, no silent fallbacks" convention.
 */
#include <stdio.h>

#include "common/common.h"

BOOL S_Init(void) {
    fprintf(stderr, "S_Init: visionOS tabletop build has no audio backend (no-op)\n");
    return true;
}

void S_Shutdown(void) {}

/* Called frequently during gameplay (per-event) - must stay silent. */
void S_PlaySound(DWORD kit_id) { (void)kit_id; }
void S_PlaySoundByName(LPCSTR name) { (void)name; }
