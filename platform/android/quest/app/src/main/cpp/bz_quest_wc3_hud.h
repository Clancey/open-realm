/*
 * bz_quest_wc3_hud.h - layer 5E: platform-independent Warcraft III tabletop
 * status/command-card HUD layout, deterministic quad/text/hit-region
 * generation, and a pure ray/plane hit-test contract for the later input
 * (controller) layer to consume.
 *
 * Like bz_quest_wc3_fog.h, every type/function here is plain C POD/math
 * only: no bzTTSnapshot_t, bzTTActionLayout_t, VkBuffer/VkImage, or Android/
 * OpenXR/Vulkan type appears anywhere in this file. The one impure HUD
 * capture unit (bz_quest_wc3_capture_hud(), in bz_quest_wc3_capture.c) does
 * its own independent BZ_TT_Latest()/retain/copy/release cycle to fill a
 * bzQuestHudInput_t; bz_quest_vk_wc3_hud.c then calls bz_quest_wc3_hud_build()
 * every frame and uploads the resulting bzQuestHudFrame_t's quads/text runs
 * as real Vulkan geometry. A later controller/input layer will call
 * bz_quest_wc3_hud_hit_test() against that SAME frame - it is never
 * recomputed from separate literals, so the rendered rectangle a player
 * sees and the rectangle their controller ray is tested against can never
 * drift apart (this file's whole reason for existing).
 *
 * -- Scope, decided from evidence, not assumption --
 *
 * The task that produced this layer asked for "selected unit info, health/
 * mana/portrait/icon, resources/status, target mode, enabled/disabled/
 * hidden states, hotkeys/tooltips, and loading/error state". Concretely
 * tracing every one of those against the real ABI and the visionOS
 * reference implementation:
 *
 * - health/mana: platform/bridge/bz_tabletop_transport.h's bzTTEntity_t has
 *   NO health/mana field of any kind (checked every field; only origin/
 *   angle/rotation/scale/radius/player/model/image/sound/frame/event/flags/
 *   renderfx/ability/splat/shadow/selected exist). Not present in
 *   bzTTPlayer_t either. There is no ABI accessor that could report it.
 *   NOT IMPLEMENTED - there is nothing to trace.
 * - portrait: only exists as a desktop-local 3D "portrait model" render in
 *   games/warcraft-3/ui (grep-confirmed); the tabletop transport/asset ABIs
 *   have no portrait accessor. NOT IMPLEMENTED.
 * - icon art (bzTTActionButton_t.image_index): traced via git blame to
 *   BuildActionButton() in bz_tabletop_transport.c, which sets it from
 *   `frame->tex.index` - a desktop-client-local UI texture REGISTRY HANDLE,
 *   not a configstring/catalog index or path string. platform/bridge/
 *   bz_tabletop_assets.h's BZ_TTA_Register* surface has no function that
 *   accepts a raw index like this or resolves it to pixel data. The
 *   visionOS reference (RealityTabletopView.swift's TabletopActionPanel,
 *   TabletopControls.swift's TabletopActionButtonSnapshot) itself NEVER
 *   renders `imageIndex` as an image - only tooltip text and cooldown.
 *   Matching that precedent (and layer 5D's "target mode has no location,
 *   so nothing is drawn" treatment of an equally unresolvable field): this
 *   layer renders each command-card slot as a flat, state-tinted quad
 *   (normal/disabled/cancel-armed color) with its tooltip/hotkey as TEXT,
 *   never a fabricated icon texture.
 * - resources/status (gold/lumber/food/hero tokens/name/selected count/game
 *   result): ALL present and real in bzTTPlayer_t + BZ_TTSnapshot_
 *   SelectedEntityIds(). visionOS's LiveTabletopTransport.swift decodes
 *   every one of these into its model but RealityTabletopView.swift never
 *   actually renders them on screen (grep-confirmed: no "gold"/"lumber"/
 *   "food" text anywhere in that file) - an apparent gap in the reference
 *   UI, not evidence the data is unrenderable. Because this data is real
 *   and authoritative, this layer DOES render it (a status bar), going
 *   slightly beyond visionOS's rendered surface while staying strictly
 *   inside the ABI's real fields - no fabrication involved.
 * - selected unit info: bzTTEntity_t carries no per-entity name/type/health
 *   to show; the only selection-derived fact available is the COUNT from
 *   BZ_TTSnapshot_SelectedEntityIds(). The status bar shows that count;
 *   nothing more is claimed.
 * - target mode, enabled/disabled/hidden, hotkeys/tooltips: all directly
 *   present on bzTTActionLayout_t/bzTTActionButton_t - fully implemented.
 * - loading/error state: no player snapshot this frame (bzQuestHudInput_t.
 *   player.present == false) renders a distinct "no player data" status
 *   line instead of a resource bar; a non-NONE game_result renders a
 *   distinct end-of-game line. Both are real ABI-derived states, not
 *   fabricated placeholders.
 *
 * -- Command-card grid: mirrors visionOS's TabletopActionPanel exactly --
 * RealityTabletopView.swift:837-878: shown only when present && visible &&
 * valid; hidden buttons are filtered out entirely (never shown disabled);
 * remaining buttons sorted by (gridY, gridX) ascending (row-major) into a
 * fixed BZ_QUEST_HUD_GRID_COLUMNS-column grid; a synthetic Cancel slot is
 * appended below the grid whenever currentTarget != NONE (visionOS renders
 * this as an unconditional "Cancel" button bound to the CMD_CANCEL command,
 * not tied to any specific buttons[] slot - mirrored here identically).
 */
#ifndef BZ_QUEST_WC3_HUD_H
#define BZ_QUEST_WC3_HUD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_HUD_MAX_BUTTONS = 12,      /* mirrors platform/bridge/bz_tabletop_transport.h's BZ_TT_MAX_COMMAND_BUTTONS */
    BZ_QUEST_HUD_MAX_ACTION_CODE = 32,  /* bounded local copy - real WC3 action codes are short alnum tokens */
    BZ_QUEST_HUD_MAX_TOOLTIP = 24,      /* bounded label text sized to fit one fixed-width button cell, no wrapping */
    BZ_QUEST_HUD_MAX_NAME = 32,
    BZ_QUEST_HUD_MAX_STATUS_TEXT = 40,
    BZ_QUEST_HUD_GRID_COLUMNS = 4,      /* RealityTabletopView.swift:843's fixed 4-column LazyVGrid */
    BZ_QUEST_HUD_MAX_QUADS = 2 + BZ_QUEST_HUD_MAX_BUTTONS + 1,       /* status bg + command bg + buttons + cancel */
    BZ_QUEST_HUD_MAX_TEXT_RUNS = 3 + BZ_QUEST_HUD_MAX_BUTTONS * 2 + 1, /* status lines + (label+cooldown)/button + cancel label */
    BZ_QUEST_HUD_MAX_HIT_REGIONS = BZ_QUEST_HUD_MAX_BUTTONS + 1,     /* buttons + synthetic cancel slot */
};

/* Mirrors platform/bridge/bz_tabletop_transport.h's bzTTActionTarget_t
 * values exactly (transport-independent copy, this module never includes
 * the bridge ABI header - see this file's header comment). */
typedef enum {
    BZ_QUEST_HUD_TARGET_NONE = 0,
    BZ_QUEST_HUD_TARGET_POINT,
    BZ_QUEST_HUD_TARGET_ENTITY,
    BZ_QUEST_HUD_TARGET_ENTITY_OR_POINT,
} bzQuestHudActionTarget_t;

/* Mirrors bzTTActionSemantic_t exactly. */
typedef enum {
    BZ_QUEST_HUD_SEMANTIC_UNSUPPORTED = 0,
    BZ_QUEST_HUD_SEMANTIC_BUTTON,
    BZ_QUEST_HUD_SEMANTIC_CANCEL,
} bzQuestHudActionSemantic_t;

/* Mirrors bzTTGameResult_t exactly. */
typedef enum {
    BZ_QUEST_HUD_GAME_RESULT_NONE = 0,
    BZ_QUEST_HUD_GAME_RESULT_VICTORY,
    BZ_QUEST_HUD_GAME_RESULT_DEFEAT,
    BZ_QUEST_HUD_GAME_RESULT_DRAW,
} bzQuestHudGameResult_t;

/* --- Input: a POD copy of exactly the ABI fields this layer uses -------- */

typedef struct {
    char actionCode[BZ_QUEST_HUD_MAX_ACTION_CODE]; /* truncated copy of bzTTActionButton_t.action_code */
    char tooltip[BZ_QUEST_HUD_MAX_TOOLTIP];        /* truncated copy of bzTTActionButton_t.tooltip */
    char hotkey;
    uint8_t gridX, gridY;
    bool hidden, disabled;
    float cooldown;
    bzQuestHudActionTarget_t target;
    bzQuestHudActionSemantic_t semantic;
} bzQuestHudButtonInput_t;

typedef struct {
    bool present, visible, valid;
    bzQuestHudActionTarget_t currentTarget;
    uint8_t numButtons; /* number of valid entries in buttons[]; clamped to BZ_QUEST_HUD_MAX_BUTTONS by the caller */
    bzQuestHudButtonInput_t buttons[BZ_QUEST_HUD_MAX_BUTTONS];
} bzQuestHudActionLayoutInput_t;

typedef struct {
    bool present; /* false => no player snapshot this frame (loading/disconnected) */
    char name[BZ_QUEST_HUD_MAX_NAME];
    uint32_t gold, lumber, foodUsed, foodCap, heroTokens;
    bzQuestHudGameResult_t gameResult;
} bzQuestHudPlayerInput_t;

typedef struct {
    bzQuestHudPlayerInput_t player;
    bzQuestHudActionLayoutInput_t actionLayout;
    uint32_t selectedCount; /* from BZ_TTSnapshot_SelectedEntityIds()'s returned count */
    uint64_t frameId;       /* BZ_TTSnapshot_Generation() - carried through unchanged for hit-test staleness checks */
} bzQuestHudInput_t;

/* --- Output: deterministic layout the Vulkan module renders and the later
 * input layer hit-tests, built ONCE per bz_quest_wc3_hud_build() call from
 * the SAME data (never two separate literal sets - see this file's header
 * comment) ------------------------------------------------------------- */

/* An oriented, axis-aligned-in-its-own-plane panel: local (x,y,0) maps to
 * world (originX,Y,Z) + x*(rightX,Y,Z) + y*(downX,Y,Z). right/down are
 * mutually orthogonal (not necessarily unit length - their length IS the
 * local->world scale for that axis); normal = normalize(right x down).
 * This representation lets bz_quest_wc3_hud_hit_test() project a world-
 * space ray into local panel coordinates with a plain dot-product (no
 * matrix inversion), and lets the Vulkan module build a standard
 * column-major 4x4 (columns right,down,normal,origin) for rendering - both
 * consumers read the exact same six vectors, so they can never disagree. */
typedef struct {
    float originX, originY, originZ;
    float rightX, rightY, rightZ;
    float downX, downY, downZ;
    float normalX, normalY, normalZ;
} bzQuestHudPanelTransform_t;

/* Panel-local flat-tint rectangle (a status/command background or one
 * button's flat placeholder-icon slot - see this file's header comment for
 * why no real icon texture is ever substituted here). */
typedef struct {
    float x, y, w, h;
    float r, g, b, a;
} bzQuestHudQuad_t;

/* Panel-local, left-baseline text run descriptor. `scale` is in the same
 * local units as the quads (NOT a font pixel size) - the Vulkan module
 * expands this into glyph quads via bz_quest_wc3_hud_font_layout_text() at
 * upload time (kept as a separate step so this module never needs to link
 * the font atlas module - single responsibility, see AGENTS.md's DRY/
 * module-boundary rules). */
typedef struct {
    float x, y, scale;
    char text[BZ_QUEST_HUD_MAX_STATUS_TEXT];
} bzQuestHudTextRun_t;

/* Stable action identity: what a later input layer echoes back when
 * posting a command. Two frames describe "the same slot" iff every field
 * here is equal - mirrors TabletopActionButtonSnapshot.id's (gridX, gridY,
 * semantic, actionCode) tuple (TabletopControls.swift:30) exactly. */
typedef struct {
    uint8_t gridX, gridY;
    bzQuestHudActionSemantic_t semantic;
    char actionCode[BZ_QUEST_HUD_MAX_ACTION_CODE];
} bzQuestHudActionId_t;

typedef struct {
    float x, y, w, h; /* panel-local rect, identical to the quad this slot rendered (never duplicated/re-derived) */
    bzQuestHudActionId_t action;
    bool enabled; /* !hidden && !disabled (hidden slots never produce a hit region at all - see below) */
} bzQuestHudHitRegion_t;

typedef struct {
    uint64_t frameId;
    bzQuestHudPanelTransform_t panel;
    uint32_t quadCount;
    bzQuestHudQuad_t quads[BZ_QUEST_HUD_MAX_QUADS];
    uint32_t textCount;
    bzQuestHudTextRun_t texts[BZ_QUEST_HUD_MAX_TEXT_RUNS];
    uint32_t hitRegionCount;
    bzQuestHudHitRegion_t hitRegions[BZ_QUEST_HUD_MAX_HIT_REGIONS];
} bzQuestHudFrame_t;

/*
 * Returns the fixed, deterministic, map-size-INDEPENDENT panel placement:
 * anchored just outside the diorama's conservative maximum half-extent
 * (BZ_QUEST_WC3_WORLD_TARGET_SPAN_F/2, bz_quest_wc3_render.h) on the +X
 * side, at a small height above the table, tilted back slightly (rotated
 * about the world X axis) for readability from above - the same
 * translate+rotate composition style bz_quest_wc3_build_world_matrix()
 * uses for entities, but with a FIXED scale (never map->diorama scaled)
 * per this layer's "stable scale/readability" requirement. Never depends on
 * a per-frame world transform succeeding, so the HUD panel still has a
 * well-defined position even with no map loaded (loading/error states).
 */
void bz_quest_wc3_hud_panel_transform(bzQuestHudPanelTransform_t *out);

/*
 * Builds the deterministic HUD frame from `input`. Never allocates, never
 * calls fprintf/logs (pure function - the impure capture layer is
 * responsible for any once-per-condition diagnostic logging). Safe to call
 * every frame with fresh POD input; holds no state between calls (a map
 * reload is just a new, independently-correct call - see this module's
 * tests for a same-shape-different-values regression check).
 */
void bz_quest_wc3_hud_build(const bzQuestHudInput_t *input, bzQuestHudFrame_t *out);

/*
 * Projects the world-space ray (rayOrigin + t*rayDir, t >= 0) onto
 * `frame->panel`'s plane, converts the hit point to panel-local (x,y), and
 * returns the FIRST hit region (in frame->hitRegions[] order - already
 * front-to-back irrelevant since this is one flat, non-overlapping-by-
 * construction plane) whose rect contains that point, regardless of its
 * `enabled` flag (the caller decides what to do with a disabled hit - this
 * function only answers "what did the ray hit", matching TabletopControls.
 * swift's own separation of hit-testing from action validation). Returns
 * false (leaving *outAction untouched) if: `currentFrameId` does not match
 * `frame->frameId` (stale frame - the caller must re-fetch before acting),
 * the ray is parallel to the panel plane, the plane intersection is behind
 * the ray origin (t < 0), or no hit region contains the local point.
 */
bool bz_quest_wc3_hud_hit_test(const bzQuestHudFrame_t *frame, uint64_t currentFrameId, float rayOriginX,
                               float rayOriginY, float rayOriginZ, float rayDirX, float rayDirY, float rayDirZ,
                               bzQuestHudActionId_t *outAction);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_HUD_H */
