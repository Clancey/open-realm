/*
 * cl_scrn_tabletop_null.c - headless screen/UI-wire glue for the shared
 * tabletop client (see platform/tabletop/client), linked by every native
 * host (visionOS today; Android/Meta Quest later).
 *
 * Real client/cl_scrn.c pulls in <SDL2/SDL.h> and drives the actual pixel
 * draw calls (re.BeginFrame/DrawLayout/EndFrame, the loading plaque, the
 * FDF layout hit-test system) - all out of scope for a windowless build.
 * This file supplies the small subset of its symbols that client/cl_main.c
 * and client/cl_parse.c call unconditionally, replacing "draw the frame"
 * with "publish a tabletop transport snapshot" and "decode svc_unit_ui"
 * with the same wire format, verbatim, forwarded to the null UI glue.
 */
#include <string.h>

#include "client/client.h"
#include "platform/bridge/bz_tabletop_transport.h"

/* Real client/cl_scrn.c defines this (client.h declares it extern); with
 * that file excluded, this is the one global this build still needs from
 * it. client/cl_main.c sets it true once CL_Init() finishes, and
 * SCR_UpdateScreen() below gates on it exactly like the real one did. */
BOOL scr_initialized;

/* No window exists to freeze, so unlike the real disable_screen/
 * disable_servercount mechanism (cl_scrn.c), there is nothing to gate here:
 * headless mode never draws regardless of loading state. Both callers
 * (CL_Init's initial connect and CL_ParseServerData's map-change path) only
 * need these to exist and be harmless. */
void SCR_BeginLoadingPlaque(void) {}
void SCR_EndLoadingPlaque(void) {}

/* Publishes one tabletop transport snapshot per frame, after CL_ReadPackets()
 * has parsed any incoming server state and (via CL_Frame()'s call order)
 * CL_PrepRefresh() has run - see BZ_TT_PublishSnapshotFromClient()'s header
 * comment for why this is the correct point to publish. There is no
 * "recursively called" hazard here (unlike the real SCR_UpdateScreen) since
 * nothing here re-enters the client. */
void SCR_UpdateScreen(DWORD msec) {
    (void)msec;
    if (!scr_initialized) return;
    BZ_TT_PublishSnapshotFromClient();
}

/* The FDF layout layer cache (cl_scrn.c's static layout_layers[]) exists
 * only to let the excluded draw code re-render the last frame's layout
 * without recomputing it. cl.layout[layer] (client/client.h) already holds
 * the authoritative data independently of that cache, so with no draw code
 * to feed, these are true no-ops rather than a stand-in cache. */
void SCR_SetLayoutLayer(DWORD layer, HANDLE data) { (void)layer; (void)data; }
void SCR_ClearLayoutLayer(DWORD layer) { (void)layer; }

/* client/keys.c's Key_Event() calls this unconditionally when an unbound key
 * is pressed, to let a server-authored FDF layout claim the keystroke as a
 * hotkey. Key_Event() itself is only ever invoked from the excluded SDL
 * keyboard-polling code (client/cl_input.c) and is therefore dead code in
 * this headless build - but keys.c still references the symbol, so it must
 * resolve. Returning false (never consumed) is exactly what the real
 * implementation would do with no layout layers active, which is always
 * true here since nothing populates them. */
BOOL SCR_LayoutKeyEvent(int key) { (void)key; return false; }

/* Verbatim port of client/cl_scrn.c's CL_ParseUnitUI() wire decode (see the
 * "Legacy unit UI response parser" comment there) - unchanged byte-for-byte
 * protocol handling, just without the SDL-tainted file it used to live in.
 * ui.UpdateUnitUI() (ui_tabletop_null.c) is what actually caches this for
 * the transport to read. */
void CL_ParseUnitUI(LPSIZEBUF msg) {
    BYTE num_units = MSG_ReadByte(msg);

    if (num_units == 0 || num_units > 12) {
        if (num_units == 0 && ui.UpdateUnitUI) {
            ui.UpdateUnitUI(0, NULL);
        }
        return;
    }

    uiUnitData_t *units = (uiUnitData_t *)MemAlloc(sizeof(uiUnitData_t) * num_units);
    memset(units, 0, sizeof(uiUnitData_t) * num_units);

    for (BYTE i = 0; i < num_units; i++) {
        uiUnitData_t *unit = &units[i];
        unit->entity_num = MSG_ReadShort(msg);

        unit->num_buttons = MSG_ReadByte(msg);
        if (unit->num_buttons > MAX_COMMAND_BUTTONS) {
            unit->num_buttons = MAX_COMMAND_BUTTONS;
        }
        for (BYTE j = 0; j < unit->num_buttons; j++) {
            uiCommandButton_t *btn = &unit->buttons[j];

            strncpy(btn->art, MSG_ReadString2(msg), sizeof(btn->art) - 1);
            strncpy(btn->tooltip, MSG_ReadString2(msg), sizeof(btn->tooltip) - 1);
            strncpy(btn->ubertip, MSG_ReadString2(msg), sizeof(btn->ubertip) - 1);
            strncpy(btn->command, MSG_ReadString2(msg), sizeof(btn->command) - 1);
            btn->hotkey = MSG_ReadByte(msg);
        }

        unit->num_inventory = MSG_ReadByte(msg);
        if (unit->num_inventory > MAX_INVENTORY_SLOTS) {
            unit->num_inventory = MAX_INVENTORY_SLOTS;
        }
        for (BYTE j = 0; j < unit->num_inventory; j++) {
            uiInventoryItem_t *item = &unit->inventory[j];

            strncpy(item->art, MSG_ReadString2(msg), sizeof(item->art) - 1);
            strncpy(item->tooltip, MSG_ReadString2(msg), sizeof(item->tooltip) - 1);
            strncpy(item->ubertip, MSG_ReadString2(msg), sizeof(item->ubertip) - 1);
            item->slot = MSG_ReadByte(msg);
        }

        unit->num_queue = MSG_ReadByte(msg);
        if (unit->num_queue > MAX_BUILD_QUEUE_ITEMS) {
            unit->num_queue = MAX_BUILD_QUEUE_ITEMS;
        }
        for (BYTE j = 0; j < unit->num_queue; j++) {
            uiQueueItem_t *queue_item = &unit->queue[j];
            LPCSTR art = MSG_ReadString2(msg);

            strncpy(queue_item->art, art, sizeof(queue_item->art) - 1);
            queue_item->entity = MSG_ReadShort(msg);
        }
    }

    ui.UpdateUnitUI((DWORD)num_units, units);
    MemFree(units);
}
