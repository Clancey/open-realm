/*
 * bz_tabletop_transport.h - versioned C ABI for the visionOS tabletop
 * snapshot/command transport (Layer 2).
 *
 * This header is the ENTIRE public surface a later Swift/RealityKit host is
 * expected to import (directly, or through a bridging header) - it must
 * never gain an Objective-C, Swift, SDL, or RealityKit dependency, and must
 * stay includable with nothing but a C99 toolchain (<stdint.h>/<stdbool.h>/
 * <stddef.h>). It intentionally does NOT include any engine header
 * (common/shared.h, client/client.h, ...): every type here is a plain,
 * bounded POD value, not a live pointer into engine state. The .c
 * implementation is free to depend on the real engine internals; this file
 * must not.
 *
 * Design summary (see docs/visionos-tabletop.md "Layer 2" section for the
 * full write-up):
 *
 *   - Snapshots are opaque, immutable, reference-counted, deep-copied values
 *     with a monotonically increasing `generation`. BZ_TT_Latest() returns a
 *     retained reference; the caller must release it. Once retained, a
 *     snapshot's contents never change - callers may read it from any
 *     thread, any number of times, for as long as they hold a reference.
 *   - Commands are typed, not free-form strings. BZ_TT_Post*() may be called
 *     from any thread and enqueues into a bounded (BZ_TT_COMMAND_QUEUE_CAPACITY)
 *     ring buffer; BZ_TT_Drain() must only be called from the engine/client
 *     thread (it encodes queued commands through the existing clc_stringcmd
 *     network command path and is not safe to call concurrently with itself).
 *   - Every Init/Shutdown/Publish/Latest/Retain/Release/Post/Drain call is
 *     internally serialized through the same lock, so Shutdown can never
 *     race a concurrent reader/writer into observing half-torn-down state.
 *     Nothing is ever destroyed while another path could still reach it:
 *     the lock itself is a static POSIX object that is never destroyed.
 *   - A "stale generation" check lets a caller optionally tag a command with
 *     the generation of the snapshot it was decided against; if the
 *     transport has since published a strictly newer generation, the
 *     command is rejected (BZ_TT_ERR_STALE_GENERATION) instead of being
 *     applied against units/state the caller can no longer see. Pass
 *     generation 0 to skip this check.
 */
#ifndef BZ_TABLETOP_TRANSPORT_H
#define BZ_TABLETOP_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on every incompatible change to any struct/enum/function signature
 * below. Append-only: existing fields/values must never be renumbered or
 * removed, only added after the last member. */
#define BZ_TABLETOP_ABI_VERSION 1u

enum {
    BZ_TT_MAX_ENTITIES              = 1024, /* per-snapshot visible-entity cap; overflow is reported, never silently dropped - see BZ_TTSnapshot_EntitiesOverflowCount() */
    BZ_TT_MAX_SELECTED_ENTITIES     = 64,   /* mirrors engine MAX_SELECTED_ENTITIES */
    BZ_TT_MAX_COMMAND_BUTTONS       = 12,   /* mirrors engine MAX_COMMAND_BUTTONS */
    BZ_TT_MAX_INVENTORY_SLOTS       = 6,    /* mirrors engine MAX_INVENTORY_SLOTS */
    BZ_TT_MAX_BUILD_QUEUE_ITEMS     = 7,    /* mirrors engine MAX_BUILD_QUEUE_ITEMS */
    BZ_TT_MAX_UNIT_LAYOUTS          = 8,    /* bounded number of units with an active command-card layout */
    BZ_TT_COMMAND_QUEUE_CAPACITY    = 256,  /* bounded typed command queue */
    BZ_TT_MAX_SELECT_IDS_PER_COMMAND = 64,  /* mirrors BZ_TT_MAX_SELECTED_ENTITIES */
    BZ_TT_ENTITY_ID_LIMIT           = 16384, /* mirrors engine MAX_GAME_ENTITIES; bounds-check only, not an existence check */
    BZ_TT_MAX_NAME_LEN              = 80,   /* mirrors engine UINAME */
    BZ_TT_MAX_CONFIGSTRING_LEN      = 256,  /* mirrors engine MAX_PATHLEN/PATHSTR */
    BZ_TT_BUTTON_CODE_LEN           = 4,    /* WC3 4-character rawcode, e.g. "hpea" */
    BZ_TT_MAX_ART_LEN               = 256,
    BZ_TT_MAX_TOOLTIP_LEN           = 256,
    BZ_TT_MAX_UBERTIP_LEN           = 512,
};

/* Mirrors client/client.h's connstate_t ordering (never call-through: this
 * ABI must not include client.h). The .c implementation static_asserts this
 * stays in sync. */
typedef enum {
    BZ_TT_CONN_DISCONNECTED = 0,
    BZ_TT_CONN_CONNECTING   = 1,
    BZ_TT_CONN_CONNECTED    = 2,
    BZ_TT_CONN_ACTIVE       = 3,
} bzTTConnState_t;

typedef enum {
    BZ_TT_OK = 0,
    BZ_TT_ERR_NOT_INITIALIZED,   /* BZ_TT_Init() was never called, or has not completed */
    BZ_TT_ERR_TERMINAL,          /* BZ_TT_Shutdown() has run; transport is not accepting work */
    BZ_TT_ERR_ABI_VERSION,       /* caller's BZ_TABLETOP_ABI_VERSION does not match this build */
    BZ_TT_ERR_QUEUE_FULL,        /* command queue is at BZ_TT_COMMAND_QUEUE_CAPACITY */
    BZ_TT_ERR_INVALID_ARGUMENT,  /* bad entity id/count/coordinate/payload length */
    BZ_TT_ERR_STALE_GENERATION,  /* observed_generation is older than the latest published snapshot */
} bzTTResult_t;

typedef enum {
    BZ_TT_CMD_SELECT = 0,   /* select <id...>    - replace the local selection set */
    BZ_TT_CMD_SMART_ENTITY, /* smart <id>        - smart-command targeted at an entity */
    BZ_TT_CMD_SMART_POINT,  /* smartpoint <x> <y> - smart-command targeted at a world point */
    BZ_TT_CMD_BUTTON,       /* button <code>     - command-card/ability button press */
    BZ_TT_CMD_CANCEL,       /* cancel            - cancel the current command/menu */
} bzTTCommandType_t;

/* Opaque, immutable, reference-counted snapshot. Never dereference directly;
 * use the BZ_TTSnapshot_* accessors below. */
typedef struct bzTTSnapshot bzTTSnapshot_t;

typedef struct {
    float min_x, min_y;
    float max_x, max_y;
} bzTTBox2_t;

/* Deep-copied mirror of a subset of entityState_t (see common/shared.h).
 * Deliberately NOT the live engine struct: this is a transport-owned,
 * ABI-stable value type. */
typedef struct {
    uint32_t number;
    uint32_t class_id;
    float origin_x, origin_y, origin_z;
    float angle;
    float rotation_x, rotation_y, rotation_z;
    float scale;
    float radius;
    uint32_t player;
    uint32_t model;
    uint32_t model2;
    uint32_t image;
    uint32_t sound;
    uint32_t frame;
    uint32_t event;
    uint16_t flags;
    uint8_t renderfx;
    uint8_t ability;
    uint32_t splat;
    uint32_t shadow;
    uint32_t shadow_rect;
    bool selected; /* mirrors centity_t.selected (local client selection) */
} bzTTEntity_t;

/* Deep-copied mirror of a subset of playerState_t. */
typedef struct {
    uint32_t number;
    uint32_t team;
    uint32_t color;
    uint32_t race;
    uint32_t uiflags;          /* UILAYOUTLAYER bitmask - see common/shared.h */
    uint32_t client_ui_state;  /* CLIENT_UI_* - see common/shared.h */
    uint32_t selected_entity;
    int32_t  start_location;
    uint32_t resource_gold;
    uint32_t resource_lumber;
    uint32_t resource_food_used;
    uint32_t resource_food_cap;
    uint32_t resource_hero_tokens;
    char name[BZ_TT_MAX_NAME_LEN];
} bzTTPlayer_t;

typedef struct {
    char art[BZ_TT_MAX_ART_LEN];
    char tooltip[BZ_TT_MAX_TOOLTIP_LEN];
    char ubertip[BZ_TT_MAX_UBERTIP_LEN];
    char command[BZ_TT_MAX_ART_LEN]; /* console command string the button executes on click */
    char hotkey;
    uint8_t grid_x, grid_y;   /* always 0 today - the legacy svc_unit_ui wire format this is
                                 decoded from never populates a grid position; see
                                 docs/visionos-tabletop.md "Layer 2" for why */
    bool research;            /* always false today - see grid_x/grid_y note */
    bool active;               /* always false today - see grid_x/grid_y note */
} bzTTCommandButton_t;

typedef struct {
    char art[BZ_TT_MAX_ART_LEN];
    char tooltip[BZ_TT_MAX_TOOLTIP_LEN];
    char ubertip[BZ_TT_MAX_UBERTIP_LEN];
    uint8_t slot;
} bzTTInventoryItem_t;

typedef struct {
    char art[BZ_TT_MAX_ART_LEN];
    uint32_t entity;
} bzTTQueueItem_t;

/* Bounded, best-effort semantic command-card layout for one unit, decoded
 * from the server's legacy svc_unit_ui message (see the "Legacy unit UI
 * response parser" comment above CL_ParseUnitUI() in client/cl_scrn.c). The
 * current, primary command-card HUD is computed locally inside the
 * game-specific UI library (out of scope for this headless transport), so
 * in practice this legacy path - and therefore this struct - is frequently
 * all-zero (num_* == 0) even mid-game. That is reported honestly (an
 * explicitly empty, versioned value), never backfilled with fabricated
 * data. */
typedef struct {
    uint32_t entity_num;
    uint8_t num_buttons;
    bzTTCommandButton_t buttons[BZ_TT_MAX_COMMAND_BUTTONS];
    uint8_t num_inventory;
    bzTTInventoryItem_t inventory[BZ_TT_MAX_INVENTORY_SLOTS];
    uint8_t num_queue;
    bzTTQueueItem_t queue[BZ_TT_MAX_BUILD_QUEUE_ITEMS];
} bzTTUnitLayout_t;

/* --- Lifecycle -------------------------------------------------------- */

/* Idempotent-safe to call again after Shutdown (mirrors the tabletop
 * lifecycle's restart convention). Must be called before any other
 * BZ_TT_* function; call from any thread. */
void BZ_TT_Init(void);

/* Marks the transport terminal: subsequent Post/Drain/Publish/Latest calls
 * fail fast (BZ_TT_ERR_TERMINAL / NULL) instead of touching torn-down state.
 * Idempotent. Does not free snapshots still held by outstanding retains -
 * those remain valid until released. Safe to call concurrently with any
 * other BZ_TT_* call from any thread. */
void BZ_TT_Shutdown(void);

/* --- Snapshot consumption (any thread) --------------------------------- */

/* Returns a retained reference to the most recently published snapshot, or
 * NULL if none has been published yet (or the transport is terminal/not
 * initialized). Caller must call BZ_TTSnapshot_Release() exactly once for
 * every successful call to this function. */
const bzTTSnapshot_t *BZ_TT_Latest(void);

void BZ_TTSnapshot_Retain(const bzTTSnapshot_t *snap);
void BZ_TTSnapshot_Release(const bzTTSnapshot_t *snap);

uint32_t BZ_TTSnapshot_AbiVersion(const bzTTSnapshot_t *snap);
uint64_t BZ_TTSnapshot_Generation(const bzTTSnapshot_t *snap);
bzTTConnState_t BZ_TTSnapshot_ConnState(const bzTTSnapshot_t *snap);

/* true + *out_name populated iff CS_WORLD is non-empty (a map is loaded). */
bool BZ_TTSnapshot_MapName(const bzTTSnapshot_t *snap, char *out_name, size_t cap);
/* true + *out populated iff the collision model has actually loaded map
 * bounds (CM_GetWorldBounds()); false and *out left zeroed otherwise - never
 * fabricated. Raw terrain tile/height data has no public engine accessor
 * yet, so it is intentionally NOT exposed here (documented gap, see
 * docs/visionos-tabletop.md). */
bool BZ_TTSnapshot_MapBounds(const bzTTSnapshot_t *snap, bzTTBox2_t *out);

const bzTTPlayer_t *BZ_TTSnapshot_Player(const bzTTSnapshot_t *snap);

/* Copies up to `cap` selected entity ids into out_ids; returns the number
 * written (never more than min(cap, BZ_TT_MAX_SELECTED_ENTITIES)). */
uint32_t BZ_TTSnapshot_SelectedEntityIds(const bzTTSnapshot_t *snap, uint32_t *out_ids, uint32_t cap);

uint32_t BZ_TTSnapshot_EntityCount(const bzTTSnapshot_t *snap);
bool BZ_TTSnapshot_EntityAt(const bzTTSnapshot_t *snap, uint32_t index, bzTTEntity_t *out);
/* Number of live entities beyond BZ_TT_MAX_ENTITIES that did not fit in this
 * snapshot (0 in the common case). Surfaced as data, not a per-frame log
 * line, so a host can react (or simply observe truncation never occurs). */
uint32_t BZ_TTSnapshot_EntitiesOverflowCount(const bzTTSnapshot_t *snap);

/* false iff no fog-of-war buffer exists yet (map not loaded / not sized). */
bool BZ_TTSnapshot_FogDimensions(const bzTTSnapshot_t *snap, uint32_t *out_width, uint32_t *out_height);
uint32_t BZ_TTSnapshot_FogVisible(const bzTTSnapshot_t *snap, uint8_t *dst, uint32_t dst_cap);
uint32_t BZ_TTSnapshot_FogExplored(const bzTTSnapshot_t *snap, uint8_t *dst, uint32_t dst_cap);

/* Number of configstring slots captured by this snapshot (0 for a NULL
 * snapshot). Callers should iterate `cs_index` in `[0,
 * BZ_TTSnapshot_ConfigStringCount(snap))` rather than importing the
 * engine's private MAX_CONFIGSTRINGS constant, which this header
 * deliberately does not expose. Within that range, BZ_TTSnapshot_ConfigString()
 * returning false means the slot is validly empty, not out of range -
 * out-of-range indices (>= this count) also return false, so a caller that
 * only iterates the documented range never needs to distinguish the two
 * cases. Added after the initial ABI release as an append-only accessor;
 * does not change BZ_TT_ConfigString's existing behavior or signature. */
uint32_t BZ_TTSnapshot_ConfigStringCount(const bzTTSnapshot_t *snap);

/* Copies configstring `cs_index` (model/image/sound/... identity - see
 * common/shared.h's CS_* enum, mirrored numerically) into out; returns false
 * if cs_index is out of range (see BZ_TTSnapshot_ConfigStringCount() above)
 * or the string is empty. */
bool BZ_TTSnapshot_ConfigString(const bzTTSnapshot_t *snap, uint32_t cs_index, char *out, size_t cap);

uint32_t BZ_TTSnapshot_UnitLayoutCount(const bzTTSnapshot_t *snap);
bool BZ_TTSnapshot_UnitLayoutAt(const bzTTSnapshot_t *snap, uint32_t index, bzTTUnitLayout_t *out);

/* --- Command posting (any thread) --------------------------------------- */

/* observed_generation: pass 0 to skip the staleness check, or the
 * generation of the snapshot this decision was made against to reject the
 * command if a strictly newer snapshot has since been published. */
bzTTResult_t BZ_TT_PostSelect(uint32_t abi_version, uint64_t observed_generation,
                               const uint32_t *entity_ids, uint32_t count);
bzTTResult_t BZ_TT_PostSmartEntity(uint32_t abi_version, uint64_t observed_generation,
                                    uint32_t target_entity_id);
bzTTResult_t BZ_TT_PostSmartPoint(uint32_t abi_version, uint64_t observed_generation,
                                   float x, float y);
/* code must be exactly BZ_TT_BUTTON_CODE_LEN bytes (a WC3 4-char rawcode,
 * NOT NUL-terminated is fine as long as code_len == 4). */
bzTTResult_t BZ_TT_PostButton(uint32_t abi_version, uint64_t observed_generation,
                               const char *code, size_t code_len);
bzTTResult_t BZ_TT_PostCancel(uint32_t abi_version, uint64_t observed_generation);

/* --- Engine-thread-only integration -------------------------------------
 *
 * Both of these must only ever be called from the one dedicated engine
 * thread that also calls BZ_RuntimeFrame()/CL_Frame() (see
 * bz_tabletop_lifecycle.c) - neither is safe to call concurrently with
 * itself, though both may safely run concurrently with Post/Latest/Retain/
 * Release from other threads.
 */

/* Encodes every currently-queued command through the existing clc_stringcmd
 * network command path (select/smart/smartpoint/button/cancel) into the
 * client's outgoing netchan message. Call once per frame, before the
 * client's normal command-send step. No-op if the transport is terminal or
 * not initialized. */
void BZ_TT_Drain(void);

/* Builds and publishes a new snapshot from the current (already-parsed,
 * consistent) client state. Call once per frame, after the client has
 * finished reading/parsing all pending server packets for that frame.
 * No-op if the transport is terminal or not initialized. */
void BZ_TT_PublishSnapshotFromClient(void);

#ifdef __cplusplus
}
#endif

#endif /* BZ_TABLETOP_TRANSPORT_H */
