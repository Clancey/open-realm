/*
 * ui_tabletop_null.c - headless UI backend for the shared tabletop client
 * (see platform/tabletop/client), linked by every native host (visionOS
 * today; Android/Meta Quest later).
 *
 * Real menu/glue UI (the per-game ui/ directories implementing client/ui.h's
 * uiExport_t) is out of scope for this layer (no native UI host renders it
 * yet). This file supplies a no-op UI_GetAPI() so client/cl_main.c
 * links and runs unmodified, plus a small cache that CL_ParseUnitUI()
 * (cl_scrn_tabletop_null.c) feeds via ui.UpdateUnitUI() - the one UI
 * callback that carries data the tabletop transport's snapshot needs (see
 * bzTTUnitLayout_t in platform/bridge/bz_tabletop_transport.h).
 *
 * This cache is only ever written and read from the engine/client thread
 * (UpdateUnitUI is called synchronously from CL_ReadPackets() while
 * decoding a packet; BZTT_CopyCachedUnitUI is called synchronously from
 * BZ_TT_PublishSnapshotFromClient() later the same frame, from
 * SCR_UpdateScreen()) - no lock is needed here, unlike the transport's own
 * cross-thread snapshot/command state.
 */
#include <string.h>

#include "client/ui.h"
#include "platform/tabletop/client/bz_tabletop_client_glue.h"

static bzTTUnitLayout_t cached_layouts[BZ_TT_MAX_UNIT_LAYOUTS];
static uint32_t cached_count;

static void UINull_Init(void) {}
static void UINull_Shutdown(void) {}
static void UINull_Refresh(DWORD time) { (void)time; }
static void UINull_KeyEvent(int key, BOOL down, DWORD time) { (void)key; (void)down; (void)time; }
static void UINull_TextInput(LPCSTR text) { (void)text; }
static BOOL UINull_MouseEvent(uiMouseEvent_t event, int x, int y, int32_t param) {
    (void)event; (void)x; (void)y; (void)param;
    return false; /* never consumed: there is no menu/HUD surface to hit-test */
}

static void ConvertButton(bzTTCommandButton_t *out, uiCommandButton_t const *in) {
    strncpy(out->art, in->art, sizeof(out->art) - 1);
    strncpy(out->tooltip, in->tooltip, sizeof(out->tooltip) - 1);
    strncpy(out->ubertip, in->ubertip, sizeof(out->ubertip) - 1);
    strncpy(out->command, in->command, sizeof(out->command) - 1);
    out->hotkey = in->hotkey;
    out->grid_x = in->x;
    out->grid_y = in->y;
    out->research = in->research != 0;
    out->active = in->active != 0;
}

static void ConvertInventory(bzTTInventoryItem_t *out, uiInventoryItem_t const *in) {
    strncpy(out->art, in->art, sizeof(out->art) - 1);
    strncpy(out->tooltip, in->tooltip, sizeof(out->tooltip) - 1);
    strncpy(out->ubertip, in->ubertip, sizeof(out->ubertip) - 1);
    out->slot = in->slot;
}

static void ConvertQueue(bzTTQueueItem_t *out, uiQueueItem_t const *in) {
    strncpy(out->art, in->art, sizeof(out->art) - 1);
    out->entity = in->entity;
}

/* Caches the server's most recent svc_unit_ui data for BZTT_CopyCachedUnitUI
 * to read. num_units == 0 (or units == NULL) means the server explicitly
 * cleared unit UI data (see CL_ParseUnitUI) - stored honestly as an empty
 * cache, not left stale. */
static void UINull_UpdateUnitUI(DWORD num_units, uiUnitData_t *units) {
    uint32_t n = num_units;
    if (n > BZ_TT_MAX_UNIT_LAYOUTS) n = BZ_TT_MAX_UNIT_LAYOUTS;
    cached_count = 0;
    if (!units) return;

    for (uint32_t i = 0; i < n; i++) {
        uiUnitData_t const *src = &units[i];
        bzTTUnitLayout_t *dst = &cached_layouts[i];

        memset(dst, 0, sizeof(*dst));
        dst->entity_num = src->entity_num;

        dst->num_buttons = src->num_buttons > BZ_TT_MAX_COMMAND_BUTTONS
            ? BZ_TT_MAX_COMMAND_BUTTONS : src->num_buttons;
        for (uint32_t j = 0; j < dst->num_buttons; j++) {
            ConvertButton(&dst->buttons[j], &src->buttons[j]);
        }

        dst->num_inventory = src->num_inventory > BZ_TT_MAX_INVENTORY_SLOTS
            ? BZ_TT_MAX_INVENTORY_SLOTS : src->num_inventory;
        for (uint32_t j = 0; j < dst->num_inventory; j++) {
            ConvertInventory(&dst->inventory[j], &src->inventory[j]);
        }

        dst->num_queue = src->num_queue > BZ_TT_MAX_BUILD_QUEUE_ITEMS
            ? BZ_TT_MAX_BUILD_QUEUE_ITEMS : src->num_queue;
        for (uint32_t j = 0; j < dst->num_queue; j++) {
            ConvertQueue(&dst->queue[j], &src->queue[j]);
        }
    }
    cached_count = n;
}

/* Lobby/matchmaking setup is out of scope for this layer (multiplayer UI is
 * explicitly excluded) - intentionally ignored. */
static void UINull_UpdateLobbySetup(lobbyState_t const *state) { (void)state; }

uiExport_t UI_GetAPI(uiImport_t uiimport) {
    (void)uiimport;
    return (uiExport_t) {
        .Init = UINull_Init,
        .Shutdown = UINull_Shutdown,
        .Refresh = UINull_Refresh,
        .KeyEvent = UINull_KeyEvent,
        .TextInput = UINull_TextInput,
        .MouseEvent = UINull_MouseEvent,
        .UpdateUnitUI = UINull_UpdateUnitUI,
        .UpdateLobbySetup = UINull_UpdateLobbySetup,
    };
}

uint32_t BZTT_CopyCachedUnitUI(bzTTUnitLayout_t *out, uint32_t cap) {
    uint32_t n = cached_count < cap ? cached_count : cap;
    if (out && n) memcpy(out, cached_layouts, n * sizeof(*out));
    return n;
}
