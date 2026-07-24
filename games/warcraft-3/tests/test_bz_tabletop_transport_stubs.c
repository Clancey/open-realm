/*
 * test_bz_tabletop_transport_stubs.c — shared globals and minimal stub
 * definitions for the bz_tabletop_transport.c test suite
 * (test_bz_tabletop_transport.c + test_bz_tabletop_transport_client.c).
 *
 * This binary links the REAL platform/bridge/bz_tabletop_transport.c, the
 * REAL client/cl_parse.c (server->client packet parser), and the REAL
 * platform/apple/visionos/tabletop/client/cl_scrn_tabletop_null.c (the same
 * headless glue that ships in the xrsimulator/xros archive) - not a second,
 * parallel reimplementation of any of them. This file supplies only the
 * handful of symbols those real files still need at link time that would
 * otherwise come from excluded/SDL-tainted engine files (mirrors
 * games/warcraft-3/tests/test_client_stubs.c's role for client/cl_scrn.c in
 * the desktop test-ui target).
 */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "client/client.h"
#include "common/cmodel.h"
#include "platform/apple/visionos/tabletop/client/bz_tabletop_client_glue.h"
#include "platform/bridge/bz_tabletop_transport.h"

struct client_state cl;
struct client_static cls;
refExport_t re;
uiExport_t ui;
mouseEvent_t mouse;

typedef struct {
    char name[64];
    char value[128];
} mockCvar_t;

static mockCvar_t mock_cvars[32];
#define MOCK_CVAR_COUNT (sizeof(mock_cvars) / sizeof(mock_cvars[0]))
static pthread_mutex_t asset_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t asset_cond = PTHREAD_COND_INITIALIZER;
static bool asset_block_init, asset_init_waiting, asset_terminal = true;

void test_transport_stubs_set_cvar(LPCSTR name, LPCSTR value) {
    FOR_LOOP(i, MOCK_CVAR_COUNT) {
        if (!mock_cvars[i].name[0] || !strcmp(mock_cvars[i].name, name)) {
            snprintf(mock_cvars[i].name, sizeof(mock_cvars[i].name), "%s", name ? name : "");
            snprintf(mock_cvars[i].value, sizeof(mock_cvars[i].value), "%s", value ? value : "");
            return;
        }
    }
}

int Cvar_Integer(LPCSTR name, int fallback) {
    FOR_LOOP(i, MOCK_CVAR_COUNT) {
        if (mock_cvars[i].name[0] && !strcmp(mock_cvars[i].name, name)) {
            return atoi(mock_cvars[i].value);
        }
    }
    return fallback;
}

LPCSTR Cvar_String(LPCSTR name, LPCSTR fallback) {
    FOR_LOOP(i, MOCK_CVAR_COUNT) {
        if (mock_cvars[i].name[0] && !strcmp(mock_cvars[i].name, name)) {
            return mock_cvars[i].value;
        }
    }
    return fallback;
}

/* client/cl_parse.c calls these directly (cl.layout/cl.cursorEntity/cl.fow
 * allocation) - real common/common.c's versions pull in the full cvar/cmd
 * subsystem, so a thin libc-backed pair is supplied here instead, same as
 * test_server_net.c's test_mem_alloc/test_mem_free wrap the real ones for a
 * lighter link. NFT_DUPTEXT's MemFree(strdup(...)) pairing (common/msg.c)
 * is also satisfied by this plain free(). */
HANDLE MemAlloc(long size) { return malloc((size_t)size); }
void MemFree(HANDLE mem) { free(mem); }

void CL_BeginLoadingMap(LPCSTR mapName) { (void)mapName; }
void CL_SetGameplayInput(void) { cls.key_dest = key_game; }
void CL_EntityEvent(entityState_t const *ent) { (void)ent; }
void CL_Disconnect(LPCSTR reason, BOOL notify) { (void)reason; (void)notify; cls.state = ca_disconnected; }
void CL_ParseTEnt(LPSIZEBUF msg) { (void)msg; }
void Cbuf_AddText(LPCSTR text) { (void)text; }
void Com_Error(errorCode_t code, LPCSTR fmt, ...) { (void)code; (void)fmt; }

/* No collision model is loaded in these tests unless a scenario explicitly
 * wants BZ_TT_PublishSnapshotFromClient()'s BuildMap() to see valid bounds
 * (gated on cl.refresh_prepped, which callers only set true for that
 * scenario) - see test_transport_set_world_bounds(). */
static BOX2 g_test_world_bounds;

void test_transport_set_world_bounds(BOX2 bounds) { g_test_world_bounds = bounds; }
BOX2 CM_GetWorldBounds(void) { return g_test_world_bounds; }

/* Controllable stand-in for platform/apple/visionos/tabletop/client/
 * ui_tabletop_null.c's real cache (same contract: BZ_TT_PublishSnapshotFromClient()
 * calls this once per publish). Kept separate from the real UI glue file so
 * these tests can deterministically control what "the last CL_ParseUnitUI()
 * decode" looked like without needing the real same-thread cache's timing. */
static bzTTUnitLayout_t g_test_unit_layouts[BZ_TT_MAX_UNIT_LAYOUTS];
static uint32_t g_test_unit_layout_count;

void test_transport_set_unit_layouts(const bzTTUnitLayout_t *layouts, uint32_t count) {
    g_test_unit_layout_count = count < BZ_TT_MAX_UNIT_LAYOUTS ? count : BZ_TT_MAX_UNIT_LAYOUTS;
    if (g_test_unit_layout_count) {
        memcpy(g_test_unit_layouts, layouts, g_test_unit_layout_count * sizeof(*layouts));
    }
}

uint32_t BZTT_CopyCachedUnitUI(bzTTUnitLayout_t *out, uint32_t cap) {
    uint32_t n = g_test_unit_layout_count < cap ? g_test_unit_layout_count : cap;
    if (n) {
        memcpy(out, g_test_unit_layouts, n * sizeof(*out));
    }
    return n;
}

void test_transport_stubs_reset(void) {
    memset(&cl, 0, sizeof(cl));
    memset(&cls, 0, sizeof(cls));
    memset(&re, 0, sizeof(re));
    memset(&ui, 0, sizeof(ui));
    memset(&mouse, 0, sizeof(mouse));
    memset(mock_cvars, 0, sizeof(mock_cvars));
    memset(&g_test_world_bounds, 0, sizeof(g_test_world_bounds));
    g_test_unit_layout_count = 0;
    pthread_mutex_lock(&asset_lock);
    asset_block_init = asset_init_waiting = false; asset_terminal = true;
    pthread_mutex_unlock(&asset_lock);
    cls.netchan.message.data = cls.netchan.message_buf;
    cls.netchan.message.maxsize = sizeof(cls.netchan.message_buf);
}

void test_transport_block_asset_init(bool block) {
    pthread_mutex_lock(&asset_lock);
    asset_block_init = block;
    if (!block) pthread_cond_broadcast(&asset_cond);
    pthread_mutex_unlock(&asset_lock);
}
void test_transport_wait_for_asset_init(void) {
    pthread_mutex_lock(&asset_lock);
    while (!asset_init_waiting) pthread_cond_wait(&asset_cond, &asset_lock);
    pthread_mutex_unlock(&asset_lock);
}
bool test_transport_asset_terminal(void) {
    bool terminal;
    pthread_mutex_lock(&asset_lock); terminal = asset_terminal; pthread_mutex_unlock(&asset_lock);
    return terminal;
}

/* Asset export has its own focused suite; these synchronized lifecycle stubs
 * prove transport and asset transitions share one serialization boundary. */
void BZ_TTA_Init(void) {
    pthread_mutex_lock(&asset_lock);
    asset_init_waiting = true;
    pthread_cond_broadcast(&asset_cond);
    while (asset_block_init) pthread_cond_wait(&asset_cond, &asset_lock);
    asset_init_waiting = false; asset_terminal = false;
    pthread_mutex_unlock(&asset_lock);
}
void BZ_TTA_Shutdown(void) {
    pthread_mutex_lock(&asset_lock); asset_terminal = true; pthread_mutex_unlock(&asset_lock);
}
void BZ_TTA_PublishTerrainFromGame(void) {}
