/*
 * bz_tabletop_transport.c - implementation of the versioned tabletop
 * snapshot/command transport ABI declared in bz_tabletop_transport.h.
 *
 * Unlike the header, this file is free to depend on real engine internals
 * (client/client.h, common/common.h, common/net.h) - it is the bridge
 * between the authoritative client state (cl.*, cls.*) and the pure-C ABI
 * a later Swift/RealityKit host consumes. It must never be linked into a
 * desktop (SDL) target: it exists solely to be linked into the visionOS
 * tabletop static archives (see platform/apple/visionos/build.mk).
 *
 * Synchronization contract (see header comment for the design summary):
 * every public entry point takes the SAME g_lock. The lock is a static
 * POSIX mutex that is never destroyed, so no path can ever observe it
 * torn down mid-use. Initialize/publish/latest/retain/release/post/drain
 * all recheck g_initialized/g_terminal while holding g_lock immediately
 * before doing anything that would be unsafe post-shutdown.
 */
#include "bz_tabletop_transport.h"
#include "bz_tabletop_game.h"

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client/client.h"
#include "common/cmodel.h"
#include "common/common.h"
#include "common/net.h"
#include "platform/tabletop/client/bz_tabletop_client_glue.h"

/* --- ABI/engine constant parity -----------------------------------------
 * These structs are transport-owned copies, not aliases, of engine
 * internals - but their bounded sizes must track the real engine limits so
 * every entry actually fits. If any of these ever drift, this file (not a
 * silent truncation at runtime) is the place that should fail to build. */
_Static_assert(BZ_TT_MAX_SELECTED_ENTITIES == MAX_SELECTED_ENTITIES, "selected-entity cap must match engine");
_Static_assert(BZ_TT_MAX_SELECT_IDS_PER_COMMAND == MAX_SELECTED_ENTITIES, "select command cap must match engine selection cap");
_Static_assert(BZ_TT_MAX_COMMAND_BUTTONS == MAX_COMMAND_BUTTONS, "command-button cap must match engine");
_Static_assert(BZ_TT_MAX_INVENTORY_SLOTS == MAX_INVENTORY_SLOTS, "inventory-slot cap must match engine");
_Static_assert(BZ_TT_MAX_BUILD_QUEUE_ITEMS == MAX_BUILD_QUEUE_ITEMS, "build-queue cap must match engine");
_Static_assert(BZ_TT_ENTITY_ID_LIMIT == MAX_GAME_ENTITIES, "entity id bound must match engine");
_Static_assert(BZ_TT_MAX_NAME_LEN == sizeof(UINAME), "name buffer size must match engine UINAME");
_Static_assert(BZ_TT_MAX_CONFIGSTRING_LEN == MAX_PATHLEN, "configstring buffer size must match engine PATHSTR");
_Static_assert((int)BZ_TT_CONN_DISCONNECTED == (int)ca_disconnected, "conn state enum must track connstate_t");
_Static_assert((int)BZ_TT_CONN_CONNECTING == (int)ca_connecting, "conn state enum must track connstate_t");
_Static_assert((int)BZ_TT_CONN_CONNECTED == (int)ca_connected, "conn state enum must track connstate_t");
_Static_assert((int)BZ_TT_CONN_ACTIVE == (int)ca_active, "conn state enum must track connstate_t");
_Static_assert((int)BZ_TT_ACTION_TARGET_NONE == (int)UI_ACTION_TARGET_NONE, "action target enum must track engine");
_Static_assert((int)BZ_TT_ACTION_TARGET_POINT == (int)UI_ACTION_TARGET_POINT, "action target enum must track engine");
_Static_assert((int)BZ_TT_ACTION_TARGET_ENTITY == (int)UI_ACTION_TARGET_ENTITY, "action target enum must track engine");
_Static_assert((int)BZ_TT_ACTION_TARGET_ENTITY_OR_POINT == (int)UI_ACTION_TARGET_ENTITY_OR_POINT,
               "action target enum must track engine");

/* Sanity bound for smartpoint coordinates: generous enough for any real
 * map (WC3 maps are on the order of 10^2-10^4 units), tight enough to catch
 * obviously-corrupt input (e.g. a caller accidentally passing a raw
 * pixel/NDC coordinate instead of a world coordinate). Not part of the
 * public ABI so it can be retuned without a version bump. */
#define BZ_TT_MAX_WORLD_COORD 1000000.0f

/* --- Internal snapshot record -------------------------------------------
 * One malloc per publish: refcount starts at 1 (the transport's own
 * "latest" reference); BZ_TT_Latest() adds a caller reference, Release
 * removes one, and the block is freed exactly when the count reaches zero
 * under g_lock. fog_visible/fog_explored are separately malloc'd because
 * their size is map-dependent (unlike every other field here, which has a
 * compile-time bound). */
struct bzTTSnapshot {
    int refcount;
    uint32_t abi_version;
    uint64_t generation;
    bzTTConnState_t conn_state;
    bool map_loaded;
    char map_name[BZ_TT_MAX_CONFIGSTRING_LEN];
    bool map_bounds_valid;
    bzTTBox2_t map_bounds;
    bzTTPlayer_t player;
    uint32_t selected_ids[BZ_TT_MAX_SELECTED_ENTITIES];
    uint32_t num_selected;
    uint32_t entity_count;
    bzTTEntity_t entities[BZ_TT_MAX_ENTITIES];
    uint32_t entities_overflow_count;
    uint32_t fog_width, fog_height;
    uint8_t *fog_visible;   /* fog_width*fog_height bytes, or NULL */
    uint8_t *fog_explored;  /* fog_width*fog_height bytes, or NULL */
    char configstrings[MAX_CONFIGSTRINGS][BZ_TT_MAX_CONFIGSTRING_LEN];
    uint32_t num_configstrings; /* always MAX_CONFIGSTRINGS today; stored (not just
                                 * returned as a #define) so a future snapshot that
                                 * captures a subset can report its true shape without
                                 * an ABI break - see BZ_TTSnapshot_ConfigStringCount(). */
    uint32_t num_unit_layouts;
    bzTTUnitLayout_t unit_layouts[BZ_TT_MAX_UNIT_LAYOUTS];
    bzTTActionLayout_t action_layout;
};

typedef struct {
    bzTTCommandType_t type;
    uint32_t select_ids[BZ_TT_MAX_SELECT_IDS_PER_COMMAND];
    uint32_t select_count;
    uint32_t smart_entity_id;
    float point_x, point_y;
    char button_code[BZ_TT_MAX_BUTTON_CODE_LEN + 1];
} bzTTQueuedCommand_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_initialized = false;
static bool g_terminal = true;
static uint64_t g_generation = 0;
static bzTTSnapshot_t *g_latest = NULL;

static bzTTQueuedCommand_t g_queue[BZ_TT_COMMAND_QUEUE_CAPACITY];
static uint32_t g_queue_head = 0, g_queue_tail = 0, g_queue_count = 0;

/* --- Ref-counting (caller must hold g_lock) ----------------------------- */

static void SnapshotFree(bzTTSnapshot_t *snap) {
    free(snap->fog_visible);
    free(snap->fog_explored);
    free(snap);
}

static void SnapshotRetainLocked(const bzTTSnapshot_t *snap_const) {
    bzTTSnapshot_t *snap = (bzTTSnapshot_t *)snap_const;
    snap->refcount++;
}

static void SnapshotReleaseLocked(const bzTTSnapshot_t *snap_const) {
    bzTTSnapshot_t *snap = (bzTTSnapshot_t *)snap_const;
    if (--snap->refcount == 0) {
        SnapshotFree(snap);
    }
}

/* --- Lifecycle ----------------------------------------------------------- */

void BZ_TT_Init(void) {
    pthread_mutex_lock(&g_lock);
    if (g_latest) {
        SnapshotReleaseLocked(g_latest);
        g_latest = NULL;
    }
    g_queue_head = g_queue_tail = g_queue_count = 0;
    g_generation = 0;
    g_initialized = true;
    g_terminal = false;
    /* One lock makes transport and selected-game publication state externally atomic. */
    BZ_GameTabletopInit();
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "BZTabletopTransport: initialized, abi_version=%u\n", BZ_TABLETOP_ABI_VERSION);
}

void BZ_TT_Shutdown(void) {
    pthread_mutex_lock(&g_lock);
    g_terminal = true;
    BZ_GameTabletopShutdown();
    pthread_mutex_unlock(&g_lock);
    fprintf(stderr, "BZTabletopTransport: shutdown (terminal)\n");
}

/* --- Snapshot consumption ------------------------------------------------ */

const bzTTSnapshot_t *BZ_TT_Latest(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_initialized || g_terminal || !g_latest) {
        pthread_mutex_unlock(&g_lock);
        return NULL;
    }
    SnapshotRetainLocked(g_latest);
    bzTTSnapshot_t *result = g_latest;
    pthread_mutex_unlock(&g_lock);
    return result;
}

void BZ_TTSnapshot_Retain(const bzTTSnapshot_t *snap) {
    if (!snap) return;
    pthread_mutex_lock(&g_lock);
    SnapshotRetainLocked(snap);
    pthread_mutex_unlock(&g_lock);
}

void BZ_TTSnapshot_Release(const bzTTSnapshot_t *snap) {
    if (!snap) return;
    pthread_mutex_lock(&g_lock);
    SnapshotReleaseLocked(snap);
    pthread_mutex_unlock(&g_lock);
}

uint32_t BZ_TTSnapshot_AbiVersion(const bzTTSnapshot_t *snap) { return snap ? snap->abi_version : 0; }
uint64_t BZ_TTSnapshot_Generation(const bzTTSnapshot_t *snap) { return snap ? snap->generation : 0; }
bzTTConnState_t BZ_TTSnapshot_ConnState(const bzTTSnapshot_t *snap) {
    return snap ? snap->conn_state : BZ_TT_CONN_DISCONNECTED;
}

bool BZ_TTSnapshot_MapName(const bzTTSnapshot_t *snap, char *out_name, size_t cap) {
    if (!snap || !snap->map_loaded || !out_name || cap == 0) return false;
    snprintf(out_name, cap, "%s", snap->map_name);
    return true;
}

bool BZ_TTSnapshot_MapBounds(const bzTTSnapshot_t *snap, bzTTBox2_t *out) {
    if (!snap || !snap->map_bounds_valid || !out) return false;
    *out = snap->map_bounds;
    return true;
}

const bzTTPlayer_t *BZ_TTSnapshot_Player(const bzTTSnapshot_t *snap) {
    return snap ? &snap->player : NULL;
}

uint32_t BZ_TTSnapshot_SelectedEntityIds(const bzTTSnapshot_t *snap, uint32_t *out_ids, uint32_t cap) {
    uint32_t n;
    if (!snap || !out_ids || cap == 0) return 0;
    n = snap->num_selected < cap ? snap->num_selected : cap;
    memcpy(out_ids, snap->selected_ids, n * sizeof(uint32_t));
    return n;
}

uint32_t BZ_TTSnapshot_EntityCount(const bzTTSnapshot_t *snap) { return snap ? snap->entity_count : 0; }

bool BZ_TTSnapshot_EntityAt(const bzTTSnapshot_t *snap, uint32_t index, bzTTEntity_t *out) {
    if (!snap || !out || index >= snap->entity_count) return false;
    *out = snap->entities[index];
    return true;
}

uint32_t BZ_TTSnapshot_EntitiesOverflowCount(const bzTTSnapshot_t *snap) {
    return snap ? snap->entities_overflow_count : 0;
}

bool BZ_TTSnapshot_FogDimensions(const bzTTSnapshot_t *snap, uint32_t *out_width, uint32_t *out_height) {
    if (!snap || !snap->fog_visible || !snap->fog_explored || snap->fog_width == 0 || snap->fog_height == 0) {
        return false;
    }
    if (out_width) *out_width = snap->fog_width;
    if (out_height) *out_height = snap->fog_height;
    return true;
}

uint32_t BZ_TTSnapshot_FogVisible(const bzTTSnapshot_t *snap, uint8_t *dst, uint32_t dst_cap) {
    uint32_t cells, n;
    if (!snap || !snap->fog_visible || !dst || dst_cap == 0) return 0;
    cells = snap->fog_width * snap->fog_height;
    n = cells < dst_cap ? cells : dst_cap;
    memcpy(dst, snap->fog_visible, n);
    return n;
}

uint32_t BZ_TTSnapshot_FogExplored(const bzTTSnapshot_t *snap, uint8_t *dst, uint32_t dst_cap) {
    uint32_t cells, n;
    if (!snap || !snap->fog_explored || !dst || dst_cap == 0) return 0;
    cells = snap->fog_width * snap->fog_height;
    n = cells < dst_cap ? cells : dst_cap;
    memcpy(dst, snap->fog_explored, n);
    return n;
}

uint32_t BZ_TTSnapshot_ConfigStringCount(const bzTTSnapshot_t *snap) {
    if (!snap) return 0;
    return snap->num_configstrings;
}

bool BZ_TTSnapshot_ConfigString(const bzTTSnapshot_t *snap, uint32_t cs_index, char *out, size_t cap) {
    if (!snap || cs_index >= snap->num_configstrings || !out || cap == 0) return false;
    if (!snap->configstrings[cs_index][0]) return false;
    snprintf(out, cap, "%s", snap->configstrings[cs_index]);
    return true;
}

uint32_t BZ_TTSnapshot_UnitLayoutCount(const bzTTSnapshot_t *snap) { return snap ? snap->num_unit_layouts : 0; }

bool BZ_TTSnapshot_UnitLayoutAt(const bzTTSnapshot_t *snap, uint32_t index, bzTTUnitLayout_t *out) {
    if (!snap || !out || index >= snap->num_unit_layouts) return false;
    *out = snap->unit_layouts[index];
    return true;
}

const bzTTActionLayout_t *BZ_TTSnapshot_ActionLayout(const bzTTSnapshot_t *snap) {
    return snap ? &snap->action_layout : NULL;
}

/* --- Command posting ----------------------------------------------------
 *
 * Pure argument validation (ids/counts/coordinates/payload length) happens
 * lock-free, since it depends on nothing but the arguments. Only the
 * ABI-version/terminal/stale-generation checks and the actual enqueue touch
 * shared state, so those alone happen under g_lock. */

static bzTTResult_t EnqueueLocked(const bzTTQueuedCommand_t *cmd) {
    if (g_queue_count >= BZ_TT_COMMAND_QUEUE_CAPACITY) {
        return BZ_TT_ERR_QUEUE_FULL;
    }
    g_queue[g_queue_tail] = *cmd;
    g_queue_tail = (g_queue_tail + 1) % BZ_TT_COMMAND_QUEUE_CAPACITY;
    g_queue_count++;
    return BZ_TT_OK;
}

/* Checks abi_version (pure), then locks to check initialized/terminal/stale
 * generation, enqueues cmd on success, and unlocks. observed_generation==0
 * means "skip the staleness check". */
static bzTTResult_t ValidateAndEnqueue(uint32_t abi_version, uint64_t observed_generation,
                                       const bzTTQueuedCommand_t *cmd) {
    bzTTResult_t rc;

    if (abi_version != BZ_TABLETOP_ABI_VERSION) {
        return BZ_TT_ERR_ABI_VERSION;
    }

    pthread_mutex_lock(&g_lock);
    if (!g_initialized) {
        rc = BZ_TT_ERR_NOT_INITIALIZED;
    } else if (g_terminal) {
        rc = BZ_TT_ERR_TERMINAL;
    } else if (observed_generation != 0 && observed_generation < g_generation) {
        rc = BZ_TT_ERR_STALE_GENERATION;
    } else {
        rc = EnqueueLocked(cmd);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

bzTTResult_t BZ_TT_PostSelect(uint32_t abi_version, uint64_t observed_generation,
                               const uint32_t *entity_ids, uint32_t count) {
    bzTTQueuedCommand_t cmd = { .type = BZ_TT_CMD_SELECT };

    if (!entity_ids || count == 0 || count > BZ_TT_MAX_SELECT_IDS_PER_COMMAND) {
        return BZ_TT_ERR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (entity_ids[i] >= BZ_TT_ENTITY_ID_LIMIT) {
            return BZ_TT_ERR_INVALID_ARGUMENT;
        }
    }
    cmd.select_count = count;
    memcpy(cmd.select_ids, entity_ids, count * sizeof(uint32_t));
    return ValidateAndEnqueue(abi_version, observed_generation, &cmd);
}

bzTTResult_t BZ_TT_PostSmartEntity(uint32_t abi_version, uint64_t observed_generation, uint32_t target_entity_id) {
    bzTTQueuedCommand_t cmd = { .type = BZ_TT_CMD_SMART_ENTITY };

    if (target_entity_id >= BZ_TT_ENTITY_ID_LIMIT) {
        return BZ_TT_ERR_INVALID_ARGUMENT;
    }
    cmd.smart_entity_id = target_entity_id;
    return ValidateAndEnqueue(abi_version, observed_generation, &cmd);
}

static bool IsValidPoint(float x, float y) {
    return !isnan(x) && !isnan(y) && !isinf(x) && !isinf(y) &&
           fabsf(x) <= BZ_TT_MAX_WORLD_COORD && fabsf(y) <= BZ_TT_MAX_WORLD_COORD;
}

bzTTResult_t BZ_TT_PostSmartPoint(uint32_t abi_version, uint64_t observed_generation, float x, float y) {
    bzTTQueuedCommand_t cmd = { .type = BZ_TT_CMD_SMART_POINT };

    if (!IsValidPoint(x, y)) {
        return BZ_TT_ERR_INVALID_ARGUMENT;
    }
    cmd.point_x = x;
    cmd.point_y = y;
    return ValidateAndEnqueue(abi_version, observed_generation, &cmd);
}

static bool IsSafeButtonCode(const char *code, size_t code_len) {
    if (!code || code_len == 0 || code_len > BZ_TT_MAX_BUTTON_CODE_LEN)
        return false;
    for (size_t i = 0; i < code_len; i++)
        if (!isalnum((unsigned char)code[i]) && code[i] != '_')
            return false;
    return true;
}

bzTTResult_t BZ_TT_PostButton(uint32_t abi_version, uint64_t observed_generation, const char *code, size_t code_len) {
    bzTTQueuedCommand_t cmd = { .type = BZ_TT_CMD_BUTTON };

    if (!IsSafeButtonCode(code, code_len)) {
        return BZ_TT_ERR_INVALID_ARGUMENT;
    }
    memcpy(cmd.button_code, code, code_len);
    return ValidateAndEnqueue(abi_version, observed_generation, &cmd);
}

bzTTResult_t BZ_TT_PostCancel(uint32_t abi_version, uint64_t observed_generation) {
    bzTTQueuedCommand_t cmd = { .type = BZ_TT_CMD_CANCEL };
    return ValidateAndEnqueue(abi_version, observed_generation, &cmd);
}

bzTTResult_t BZ_TT_PostTargetPoint(uint32_t abi_version, uint64_t observed_generation, float x, float y) {
    bzTTQueuedCommand_t cmd = { .type = BZ_TT_CMD_TARGET_POINT };
    if (!IsValidPoint(x, y))
        return BZ_TT_ERR_INVALID_ARGUMENT;
    cmd.point_x = x; cmd.point_y = y;
    return ValidateAndEnqueue(abi_version, observed_generation, &cmd);
}

/* --- Engine-thread-only integration --------------------------------------
 *
 * Neither of these may run concurrently with itself; both may run
 * concurrently with Post/Latest/Retain/Release from other threads. */

/* Encodes exactly one queued command through the same clc_stringcmd +
 * SZ_Printf append pattern used by Cmd_ForwardToServer()/CL_UIServerCommand()
 * (cl_main.c) - never CL_ClientCommand(), which memsets the outgoing buffer
 * first and would clobber any command already appended this frame. */
static void EncodeCommand(const bzTTQueuedCommand_t *cmd) {
    char buf[512];
    int len;

    switch (cmd->type) {
        case BZ_TT_CMD_SELECT:
            len = snprintf(buf, sizeof(buf), "select");
            for (uint32_t i = 0; i < cmd->select_count && len > 0 && (size_t)len < sizeof(buf); i++) {
                len += snprintf(buf + len, sizeof(buf) - (size_t)len, " %u", (unsigned)cmd->select_ids[i]);
            }
            break;
        case BZ_TT_CMD_SMART_ENTITY:
            snprintf(buf, sizeof(buf), "smart %u", (unsigned)cmd->smart_entity_id);
            break;
        case BZ_TT_CMD_SMART_POINT:
            snprintf(buf, sizeof(buf), "smartpoint %d %d", (int)cmd->point_x, (int)cmd->point_y);
            break;
        case BZ_TT_CMD_BUTTON:
            snprintf(buf, sizeof(buf), "button %s", cmd->button_code);
            break;
        case BZ_TT_CMD_CANCEL:
            snprintf(buf, sizeof(buf), "button CmdCancel");
            break;
        case BZ_TT_CMD_TARGET_POINT:
            snprintf(buf, sizeof(buf), "point %d %d", (int)cmd->point_x, (int)cmd->point_y);
            break;
        default:
            fprintf(stderr, "BZTabletopTransport: dropping unknown queued command type %d\n", (int)cmd->type);
            return;
    }
    MSG_WriteByte(&cls.netchan.message, clc_stringcmd);
    SZ_Printf(&cls.netchan.message, "%s", buf);
}

void BZ_TT_Drain(void) {
    static bzTTQueuedCommand_t local[BZ_TT_COMMAND_QUEUE_CAPACITY];
    uint32_t n = 0;

    pthread_mutex_lock(&g_lock);
    if (g_initialized && !g_terminal) {
        while (g_queue_count > 0) {
            local[n++] = g_queue[g_queue_head];
            g_queue_head = (g_queue_head + 1) % BZ_TT_COMMAND_QUEUE_CAPACITY;
            g_queue_count--;
        }
    }
    pthread_mutex_unlock(&g_lock);

    for (uint32_t i = 0; i < n; i++) {
        EncodeCommand(&local[i]);
    }
}

/* --- Snapshot construction from live client state ------------------------ */

static void BuildPlayer(bzTTPlayer_t *out) {
    PLAYER const *ps = &cl.playerstate;
    memset(out, 0, sizeof(*out));
    out->number = ps->number;
    out->team = ps->team;
    out->color = ps->color;
    out->race = ps->race;
    out->uiflags = ps->uiflags;
    out->client_ui_state = ps->client_ui_state;
    out->selected_entity = ps->selected_entity;
    out->start_location = ps->start_location;
    out->resource_gold = ps->stats[PLAYERSTATE_RESOURCE_GOLD];
    out->resource_lumber = ps->stats[PLAYERSTATE_RESOURCE_LUMBER];
    out->resource_food_used = ps->stats[PLAYERSTATE_RESOURCE_FOOD_USED];
    out->resource_food_cap = ps->stats[PLAYERSTATE_RESOURCE_FOOD_CAP];
    out->resource_hero_tokens = ps->stats[PLAYERSTATE_RESOURCE_HERO_TOKENS];
    if (ps->name) {
        snprintf(out->name, sizeof(out->name), "%s", ps->name);
    }
    out->target = (bzTTActionTarget_t)ps->client_ui_target;
    if (cls.state == ca_active && cl.playerstate_valid) {
        switch (ps->stats[PLAYERSTATE_GAME_RESULT]) {
            case 0: out->game_result = BZ_TT_GAME_RESULT_VICTORY; break;
            case 1: out->game_result = BZ_TT_GAME_RESULT_DEFEAT; break;
            case 2: out->game_result = BZ_TT_GAME_RESULT_DRAW; break;
            default: out->game_result = BZ_TT_GAME_RESULT_NONE; break;
        }
    }
}

static void BuildEntity(bzTTEntity_t *out, centity_t const *ce) {
    entityState_t const *es = &ce->current;
    memset(out, 0, sizeof(*out));
    out->number = es->number;
    out->class_id = es->class_id;
    out->origin_x = es->origin.x;
    out->origin_y = es->origin.y;
    out->origin_z = es->origin.z;
    out->angle = es->angle;
    out->rotation_x = es->rotation.x;
    out->rotation_y = es->rotation.y;
    out->rotation_z = es->rotation.z;
    out->scale = es->scale;
    out->radius = es->radius;
    out->player = es->player;
    out->model = es->model;
    out->model2 = es->model2;
    out->image = es->image;
    out->sound = es->sound;
    out->frame = es->frame;
    out->event = es->event;
    out->flags = es->flags;
    out->renderfx = es->renderfx;
    out->ability = es->ability;
    out->splat = es->splat;
    out->shadow = es->shadow;
    out->shadow_rect = es->shadow_rect;
    /* The server stamps per-client RF_SELECTED; ce->selected is only speculative desktop input state. */
    out->selected = es->renderfx & RF_SELECTED;
}

static void BuildEntities(bzTTSnapshot_t *snap) {
    uint32_t written = 0, overflow = 0;
    for (DWORD i = 0; i < cl.num_entities && i < MAX_CLIENT_ENTITIES; i++) {
        /* Match CL_AddEntities: zero-model slots are inactive, not dropped entities. */
        if (!cl.ents[i].current.model)
            continue;
        if (written < BZ_TT_MAX_ENTITIES) {
            BuildEntity(&snap->entities[written], &cl.ents[i]);
            if (snap->entities[written].selected && snap->num_selected < BZ_TT_MAX_SELECTED_ENTITIES)
                snap->selected_ids[snap->num_selected++] = snap->entities[written].number;
            written++;
        } else {
            overflow++;
        }
    }
    snap->entity_count = written;
    snap->entities_overflow_count = overflow;
    if (overflow > 0) {
        static bool warned = false;
        if (!warned) {
            fprintf(stderr,
                    "BZTabletopTransport: entity snapshot cap (%u) exceeded, "
                    "%u entities dropped this publish (reported via "
                    "BZ_TTSnapshot_EntitiesOverflowCount, logged once)\n",
                    (unsigned)BZ_TT_MAX_ENTITIES, (unsigned)overflow);
            warned = true;
        }
    }
}

static void BuildFog(bzTTSnapshot_t *snap) {
    DWORD cells = cl.fow.width * cl.fow.height;
    snap->fog_width = cl.fow.width;
    snap->fog_height = cl.fow.height;
    if (cells == 0 || !cl.fow.visible || !cl.fow.explored) {
        return;
    }
    snap->fog_visible = malloc(cells);
    snap->fog_explored = malloc(cells);
    if (!snap->fog_visible || !snap->fog_explored) {
        fprintf(stderr, "BZTabletopTransport: fog buffer allocation failed, publishing without fog data\n");
        free(snap->fog_visible);
        free(snap->fog_explored);
        snap->fog_visible = NULL;
        snap->fog_explored = NULL;
        snap->fog_width = 0;
        snap->fog_height = 0;
        return;
    }
    memcpy(snap->fog_visible, cl.fow.visible, cells);
    memcpy(snap->fog_explored, cl.fow.explored, cells);
}

/* MSG_ReadDeltaUIFrame assumes a trusted packet; validate every variable field
 * before calling it because cached layout corruption must publish empty state,
 * not let its text walkers run beyond the bounded payload. */
static bool ActionFrameFieldsFit(const sizeBuf_t *msg, DWORD bits) {
    static BYTE const sizes[] = { 2, 2, 4, 4, 4, 4, 4, 4, 8, 4, 4, 1, 4, 0, 0, 0, 1, 4 };
    DWORD cursor = msg->readcount;
    if (bits & ~((1u << (sizeof(sizes) / sizeof(sizes[0]))) - 1))
        return false;
    FOR_LOOP(i, sizeof(sizes) / sizeof(sizes[0])) {
        if (!(bits & (1u << i)))
            continue;
        if (i >= 13 && i <= 15) {
            LPBYTE end;
            if (cursor >= msg->cursize ||
                !(end = memchr(msg->data + cursor, '\0', msg->cursize - cursor)))
                return false;
            cursor = (DWORD)(end - msg->data) + 1;
        } else {
            if (sizes[i] > msg->cursize - cursor)
                return false;
            cursor += sizes[i];
        }
    }
    return true;
}

/* Only a single safe button token crosses the typed ABI. All other authored
 * onclick forms remain visible as unsupported/disabled data and are never run. */
static bzTTActionSemantic_t BuildActionButton(bzTTActionButton_t *out, LPCUIFRAME frame, DWORD order) {
    LPCSTR onclick = frame->onclick ? frame->onclick : "";
    LPCSTR code = !strncmp(onclick, "button ", 7) ? onclick + 7 : NULL;
    size_t code_len = code ? strlen(code) : 0;
    memset(out, 0, sizeof(*out));
    out->image_index = frame->tex.index;
    snprintf(out->tooltip, sizeof(out->tooltip), "%s", frame->tooltip ? frame->tooltip : "");
    out->hotkey = (char)frame->hotkey;
    out->grid_x = order % 4; out->grid_y = order / 4;
    out->hidden = frame->flags.hidden;
    out->disabled = frame->flags.disabled;
    out->cooldown = frame->value;
    out->target = (bzTTActionTarget_t)frame->flags.target;
    if (code && IsSafeButtonCode(code, code_len)) {
        snprintf(out->action_code, sizeof(out->action_code), "%s", code);
        return !strcmp(code, "CmdCancel") ? BZ_TT_ACTION_CANCEL : BZ_TT_ACTION_BUTTON;
    }
    out->disabled = true;
    return BZ_TT_ACTION_UNSUPPORTED;
}

/* Decode the authoritative command-bar cache independently of desktop layout
 * globals so publication remains a pure, bounded client-state snapshot. */
static void BuildActionLayout(bzTTActionLayout_t *out) {
    LPBYTE cached = cl.layout[LAYER_COMMANDBAR];
    sizeBuf_t msg;
    DWORD payload_size;
    bool terminated = false;
    static bool warned_malformed, warned_unsupported, warned_overflow;

    memset(out, 0, sizeof(*out));
    out->current_target = (bzTTActionTarget_t)cl.playerstate.client_ui_target;
    out->visible = cl.playerstate.client_ui_state == CLIENT_UI_GAME &&
                   !(cl.playerstate.uiflags & (1u << LAYER_COMMANDBAR));
    if (!cached)
        return;
    out->present = true;
    memcpy(&payload_size, cached, sizeof(payload_size));
    if (!payload_size || payload_size > MAX_MSGLEN)
        goto malformed;
    msg = (sizeBuf_t){ .data = cached + sizeof(payload_size), .maxsize = payload_size, .cursize = payload_size };
    while (msg.readcount + sizeof(DWORD) + sizeof(WORD) <= msg.cursize) {
        uiFrame_t frame = { 0 };
        DWORD bits = 0;
        DWORD number = MSG_ReadEntityBits(&msg, &bits);
        if (!number && !bits) {
            terminated = msg.readcount == msg.cursize;
            break;
        }
        if (number >= MAX_LAYOUT_OBJECTS || !ActionFrameFieldsFit(&msg, bits))
            goto malformed;
        MSG_ReadDeltaUIFrame(&msg, &frame, number, bits);
        if (msg.readcount >= msg.cursize)
            goto malformed;
        frame.buffer.size = MSG_ReadByte(&msg);
        if (frame.buffer.size > msg.cursize - msg.readcount)
            goto malformed;
        msg.readcount += frame.buffer.size;
        if (frame.flags.type != FT_COMMANDBUTTON)
            continue;
        if (out->num_buttons >= BZ_TT_MAX_COMMAND_BUTTONS) {
            if (!warned_overflow) {
                fprintf(stderr, "BZTabletopTransport: command-bar button cap exceeded (logged once)\n");
                warned_overflow = true;
            }
            continue;
        }
        bzTTActionButton_t *button = &out->buttons[out->num_buttons];
        button->semantic = BuildActionButton(button, &frame, out->num_buttons);
        if (button->semantic == BZ_TT_ACTION_UNSUPPORTED && !warned_unsupported) {
            fprintf(stderr, "BZTabletopTransport: unsupported command-bar onclick \"%s\" (logged once)\n",
                    frame.onclick ? frame.onclick : "");
            warned_unsupported = true;
        }
        out->num_buttons++;
    }
    if (!terminated)
        goto malformed;
    out->valid = true;
    return;

malformed:
    out->num_buttons = 0;
    if (!warned_malformed) {
        fprintf(stderr, "BZTabletopTransport: malformed cached command-bar layout; publishing empty (logged once)\n");
        warned_malformed = true;
    }
}

/* Map bounds are only valid once the client has locally loaded the
 * collision model for the current map - cl.refresh_prepped tracks exactly
 * that (see client/cl_view.c's CL_PrepRefresh(), which sets it true only
 * after a successful CM_LoadMap()). Calling CM_GetWorldBounds() before then
 * would dereference an unloaded map. Raw terrain tile/height data has no
 * public engine accessor today; this snapshot intentionally leaves that gap
 * unpopulated rather than reaching into cmodel internals (see
 * docs/visionos-tabletop.md). */
static void BuildMap(bzTTSnapshot_t *snap) {
    snap->map_loaded = *cl.configstrings[CS_WORLD] != '\0';
    if (snap->map_loaded) {
        snprintf(snap->map_name, sizeof(snap->map_name), "%s", cl.configstrings[CS_WORLD]);
    }
    if (cl.refresh_prepped) {
        BOX2 const bounds = CM_GetWorldBounds();
        snap->map_bounds.min_x = bounds.min.x;
        snap->map_bounds.min_y = bounds.min.y;
        snap->map_bounds.max_x = bounds.max.x;
        snap->map_bounds.max_y = bounds.max.y;
        snap->map_bounds_valid = true;
    }
}

void BZ_TT_PublishSnapshotFromClient(void) {
    bzTTSnapshot_t *snap;
    bzTTSnapshot_t *old;
    bool ok;

    pthread_mutex_lock(&g_lock);
    ok = g_initialized && !g_terminal;
    pthread_mutex_unlock(&g_lock);
    if (!ok) {
        return;
    }

    snap = calloc(1, sizeof(*snap));
    if (!snap) {
        fprintf(stderr, "BZTabletopTransport: snapshot allocation failed, skipping this publish\n");
        return;
    }
    snap->refcount = 1;
    snap->abi_version = BZ_TABLETOP_ABI_VERSION;
    snap->conn_state = (bzTTConnState_t)cls.state;
    BuildMap(snap);
    BuildPlayer(&snap->player);
    BuildEntities(snap);
    BuildFog(snap);
    _Static_assert(sizeof(snap->configstrings) == sizeof(cl.configstrings), "configstring block shape must match cl.configstrings");
    memcpy(snap->configstrings, cl.configstrings, sizeof(snap->configstrings));
    snap->num_configstrings = MAX_CONFIGSTRINGS;
    /* Unit command-card layouts: populated only via BZTT_CopyCachedUnitUI()
     * (see platform/tabletop/client/ui_tabletop_null.c), which mirrors
     * whatever the last CL_ParseUnitUI() decode delivered to
     * ui.UpdateUnitUI(). Left at 0 (all-zero, versioned-empty) otherwise. */
    snap->num_unit_layouts = BZTT_CopyCachedUnitUI(snap->unit_layouts, BZ_TT_MAX_UNIT_LAYOUTS);
    BuildActionLayout(&snap->action_layout);

    pthread_mutex_lock(&g_lock);
    if (!g_initialized || g_terminal) {
        pthread_mutex_unlock(&g_lock);
        SnapshotFree(snap);
        return;
    }
    snap->generation = ++g_generation;
    old = g_latest;
    g_latest = snap;
    if (old) {
        SnapshotReleaseLocked(old);
    }
    pthread_mutex_unlock(&g_lock);
    BZ_GameTabletopPublish();
}
