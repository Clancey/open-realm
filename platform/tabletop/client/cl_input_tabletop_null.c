/*
 * cl_input_tabletop_null.c - headless input glue for the shared tabletop
 * client (see platform/tabletop/client), linked by every native host
 * (visionOS today; Android/Meta Quest later).
 *
 * Real client/cl_input.c polls SDL for mouse/keyboard events (via
 * cl_input_local.h, which pulls SDL2) and drives real-time drag-select,
 * minimap clicks, and key bindings - none of which exist in a windowless
 * build with no platform input source. This file supplies only the subset
 * of client/client.h's input-facing symbols that other real, linked client
 * files (cl_main.c, cl_parse.c, cl_view.c, common/common.c) call
 * unconditionally. Functions used exclusively by the excluded
 * cl_input*.c/cl_input_w3.c/cl_input_wow.c files (CL_GameplayInputReady,
 * IN_SelectDown/Up, CL_HandleGameKey, CL_TryMinimapClick, ...) are
 * intentionally not reimplemented here: nothing in this build calls them.
 *
 * Tabletop selection/commands instead arrive over the bridge's typed
 * command queue (platform/bridge/bz_tabletop_transport.h) and are encoded
 * onto the wire from BZ_TT_Drain(), called below from CL_Input() - the
 * same point in CL_Frame() (client/cl_main.c) where real mouse-driven
 * commands would otherwise be queued, and always before CL_SendCommand().
 */
#include <stdio.h>

#include "client/client.h"
#include "platform/bridge/bz_tabletop_transport.h"

mouseEvent_t mouse; /* never updated: no platform pointer exists headless */

BOOL CL_AltModifierDown(void) { return false; /* no keyboard/modifier source headless */ }

/* cl_view.c gates a screen-space raycast on this; with mouse never moving,
 * "the pointer is over gameplay UI" can never be true. */
BOOL CL_MouseOverGameplayUI(void) { return false; }

void CL_SetMenuBindings(void) { cls.key_dest = key_menu; }

void CL_SetGameplayInput(void) { cls.key_dest = key_game; }

/* Preserves the one line every caller of this function actually depends on:
 * common/common.c's MenuAction("map") flips the netchan to loopback so the
 * listen-server path works exactly as it does on desktop (see
 * client/cl_input.c's CL_SetGameplayBindings(), which this mirrors minus
 * the SDL text-input toggle). */
void CL_SetGameplayBindings(void) {
    CL_SetGameplayInput();
    cls.netchan.remote_address.type = NA_LOOPBACK;
}

void CL_InitInput(void) {
    fprintf(stderr, "CL_InitInput: headless tabletop build has no platform input source; "
                     "commands arrive only via the bz_tabletop_transport command queue\n");
}

/* No SDL event pump exists to poll. Drains the tabletop transport's typed
 * command queue - encoding each into the same clc_stringcmd wire commands
 * (select/smart/smartpoint/button/cancel) real mouse/keyboard input would
 * produce - before CL_Frame() calls CL_SendCommand(), so drained commands
 * ship in the same frame they were posted. */
void CL_Input(void) {
    BZ_TT_Drain();
}
