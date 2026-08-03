/*
 * bz_quest_input_state.h - layer 6: platform-independent Meta Quest Touch-
 * controller interaction state machine, board (diorama) placement/pan/rotate/
 * zoom transform, deterministic ray-hit priority, and the typed-command
 * decision table the impure OpenXR driver (bz_quest_xr_actions.c) posts
 * through the existing BZ_TT_Post* transport ABI.
 *
 * Like bz_quest_wc3_render.h / bz_quest_wc3_hud.h / bz_quest_wc3_fog.h, every
 * type/function here is plain C POD/math only: no XrAction/XrSpace/XrPath,
 * VkBuffer, Android, or bzTTSnapshot_t/BZ_TT_* transport type appears
 * anywhere in this file. This is the whole point of the module split (see
 * AGENTS.md's "keep every OpenXR type strictly inside Quest host modules"
 * rule): platform/android/quest/tests/test_bz_quest_input_state.c builds and
 * exercises this exact state machine / hit-priority / command-mapping / board-
 * transform logic with a plain host C compiler, no NDK/OpenXR/Vulkan/engine
 * link. The impure bz_quest_xr_actions.c owns the real XrAction/XrSpace
 * handles, calls xrSyncActions / xrGetActionState* / xrLocateSpace /
 * xrApplyHapticFeedback, feeds plain float/bool/POD values into bz_quest_input_state_update()
 * below, and maps its POD output back onto BZ_TT_Post* + xrApplyHapticFeedback.
 *
 * -- Authority boundary (Quake 2 STAT_LAYOUTS / visionOS Layer 6 precedent) --
 * This module NEVER mutates player/entity/selection state to fake success.
 * It only decides which one typed command to post; the server is
 * authoritative and the effect appears only in a LATER snapshot (see
 * docs/visionos-tabletop-controls.md and platform/bridge/
 * bz_tabletop_transport.h's staleness contract). Target mode
 * (bzQuestHudActionTarget_t, mirrored in bz_quest_wc3_hud.h) is server-owned:
 * pressing a target-bearing button does not optimistically enter target mode
 * here - the state machine waits for the server-authored target enum in a
 * later snapshot, exactly as the reviewed visionOS controls do.
 */
#ifndef BZ_QUEST_INPUT_STATE_H
#define BZ_QUEST_INPUT_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_wc3_hud.h"    /* bzQuestHudFrame_t + bz_quest_wc3_hud_hit_test() (pure), bzQuestHudActionTarget_t */
#include "bz_quest_wc3_render.h" /* bzQuestWc3WorldTransform_t + world_transform_point_inverse() (pure) */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_INPUT_HAND_LEFT = 0,
    BZ_QUEST_INPUT_HAND_RIGHT = 1,
    BZ_QUEST_INPUT_HAND_COUNT = 2,
    /* Mirrors platform/bridge/bz_tabletop_transport.h's
     * BZ_TT_MAX_SELECT_IDS_PER_COMMAND - the additive-selection path merges
     * the copied selection set plus one new id, bounded by this cap. */
    BZ_QUEST_INPUT_MAX_SELECT_IDS = 64,
    /* Safe button token buffer: BZ_TT_MAX_BUTTON_CODE_LEN (255) + NUL. Kept
     * as a literal (not an #include of the bridge ABI header) so this file
     * stays transport-independent - see this file's header comment. */
    BZ_QUEST_INPUT_MAX_BUTTON_CODE = 256,
};

/* --- Board (diorama) transform ----------------------------------------- *
 * A Quest-user-owned rigid+uniform-scale placement that composes with (never
 * replaces) the bounds-derived bzQuestWc3WorldTransform_t: that transform
 * puts the map inside a fixed ~1.08-unit diorama box centered on the origin
 * (bz_quest_wc3_render.h), and THIS transform then rigidly places/orients/
 * scales that whole box into the OpenXR LOCAL tracking space in front of the
 * user. bz_quest_board_transform_matrix() is folded into every eye's
 * view*projection once per frame (so terrain, models, fog, selection markers,
 * and the HUD panel all move together), and bz_quest_board_transform_inverse_ray()
 * maps a tracking-space controller ray back into that same composed space for
 * hit-testing - one shared transform, applied consistently, never patched per
 * renderer component (mirrors bz_quest_wc3_fog.c's shared-transform pattern). */
typedef struct {
    float tx, ty, tz; /* translation into LOCAL tracking space (meters) */
    float yaw;        /* rotation about the tracking-space up (Y) axis, radians */
    float scale;      /* uniform scale, clamped to [BZ_QUEST_BOARD_SCALE_MIN, MAX] */
} bzQuestBoardTransform_t;

/* Default placement: 0.6 m in front of and 0.4 m below the LOCAL space origin
 * (the head pose at session start / recenter), unrotated, unit scale. A
 * seated user looking slightly down sees the ~1-unit diorama resting on a
 * table in front of them. These are deliberate first-playable defaults, not
 * scene-anchored placement (Quest scene/anchor understanding is a later,
 * out-of-scope layer - see docs/quest-tabletop.md); the user pans/rotates/
 * zooms from here and MENU resets to exactly this. */
#define BZ_QUEST_BOARD_DEFAULT_TX 0.0f
#define BZ_QUEST_BOARD_DEFAULT_TY (-0.40f)
#define BZ_QUEST_BOARD_DEFAULT_TZ (-0.60f)
/* Uniform scale bounds: the composed diorama is ~1.08 units, so 0.3x..3.0x
 * spans a ~0.32 m tabletop miniature up to a ~3.2 m room-scale board - a
 * bounded, physically sensible range that can never collapse to zero or run
 * away to infinity (AGENTS.md: "no unbounded zoom-to-infinity/zero"). */
#define BZ_QUEST_BOARD_SCALE_MIN 0.30f
#define BZ_QUEST_BOARD_SCALE_MAX 3.00f
/* Height (ty) bounds keep the board reachable/visible: never above +1 m or
 * below -2 m relative to the head pose at recenter. */
#define BZ_QUEST_BOARD_TY_MIN (-2.0f)
#define BZ_QUEST_BOARD_TY_MAX (1.0f)
/* Horizontal translation bound keeps a mis-drag from flinging the board out
 * of arm's reach entirely (a bounded 3 m radius from the recenter origin). */
#define BZ_QUEST_BOARD_TRANSLATE_LIMIT 3.0f
/* Thumbstick response rates (per second at full deflection) and deadzone. */
#define BZ_QUEST_BOARD_YAW_RATE 2.0f        /* rad/s at full thumbstick-X */
#define BZ_QUEST_BOARD_ZOOM_RATE 1.5f       /* scale-units/s at full thumbstick-Y */
#define BZ_QUEST_BOARD_HEIGHT_RATE 0.6f     /* m/s at full thumbstick-Y (left) */
#define BZ_QUEST_INPUT_THUMBSTICK_DEADZONE 0.2f

void bz_quest_board_transform_default(bzQuestBoardTransform_t *out);
/* Clamps scale/ty/tx/tz into their documented bounds in place. Always safe. */
void bz_quest_board_transform_clamp(bzQuestBoardTransform_t *bt);
/* Column-major model matrix T(tx,ty,tz) * Ryaw * S(scale) - the exact matrix
 * folded into view*projection for rendering. */
void bz_quest_board_transform_matrix(const bzQuestBoardTransform_t *bt, float out[16]);
/* Forward-maps a composed-space point into LOCAL tracking space. */
void bz_quest_board_transform_apply_point(const bzQuestBoardTransform_t *bt, const float in[3],
                                          float out[3]);
/* Inverse-maps a LOCAL tracking-space point back into composed space. Exact
 * inverse of apply_point within float rounding (round-trip tested). */
void bz_quest_board_transform_inverse_point(const bzQuestBoardTransform_t *bt, const float in[3],
                                            float out[3]);
/* Inverse-maps a tracking-space ray (origin+dir) into composed space for
 * hit-testing. `dir` is rotated/inverse-scaled but NOT re-normalized here
 * (the plane/sphere tests below are scale-invariant in t); callers that need
 * a unit direction for a metric distance should normalize the result. */
void bz_quest_board_transform_inverse_ray(const bzQuestBoardTransform_t *bt, const float origin[3],
                                          const float dir[3], float outOrigin[3], float outDir[3]);

/* --- Edge detection ---------------------------------------------------- *
 * Per-button previous-frame latch. bz_quest_edge_update() fires a rising
 * edge ONLY on a false->true transition while active, never re-fires while
 * held, and clears the latch (never leaving it "still pressed") the moment
 * the action goes inactive (controller/profile loss) - so a disconnect
 * during a hold can never strand a latched press. */
typedef struct {
    bool prev;
} bzQuestButtonEdge_t;

/* Returns true exactly once per press. `active` false forces prev=false and
 * returns false (an inactive action is never "pressed"). */
bool bz_quest_edge_update(bzQuestButtonEdge_t *edge, bool active, bool pressed);

/* --- Ray-hit priority -------------------------------------------------- */

typedef enum {
    BZ_QUEST_INPUT_HIT_NONE = 0,   /* ray hit nothing actionable */
    BZ_QUEST_INPUT_HIT_HUD_BUTTON, /* enabled command-card action button */
    BZ_QUEST_INPUT_HIT_HUD_CANCEL, /* synthetic cancel slot */
    BZ_QUEST_INPUT_HIT_HUD_DISABLED, /* a hit landed on a hidden/disabled/stale HUD slot -> consumed, no command */
    BZ_QUEST_INPUT_HIT_ENTITY,     /* nearest entity sphere along the ray */
    BZ_QUEST_INPUT_HIT_TERRAIN,    /* board base plane (composed-space y = planeY) */
} bzQuestInputHitKind_t;

/* One entity's rendered hit sphere in COMPOSED (post-world-transform, pre-
 * board) space - center is bz_quest_wc3_world_transform_point() of the
 * entity's swapped origin, radius is derived from the SAME
 * bz_quest_wc3_entity_footprint_scale() the rendered mesh/selection marker
 * uses (never bzTTEntity_t.radius - see bz_quest_wc3_render.h). Carries the
 * authoritative entity `number` (needed to post select/smart) and its engine-
 * space ground origin (needed for the target-entity-as-point ABI-gap path). */
typedef struct {
    uint32_t number;
    float centerX, centerY, centerZ; /* composed space */
    float radius;                    /* composed space */
    float engineX, engineNorth;      /* authoritative engine world x,y for target-as-point fallback */
} bzQuestInputEntity_t;

typedef struct {
    bzQuestInputHitKind_t kind;
    /* Valid iff kind == HUD_BUTTON/HUD_CANCEL/HUD_DISABLED: the action the
     * HUD hit-test returned (echoed back verbatim when posting). */
    bzQuestHudActionId_t hudAction;
    uint32_t entityNumber;      /* valid iff kind == ENTITY */
    float entityEngineX, entityEngineNorth; /* valid iff kind == ENTITY */
    float pointEngineX, pointEngineNorth;   /* valid iff kind == TERRAIN: engine world x,y of the ray/plane hit */
    float distance;             /* ray t at the hit (composed space), for nearest-wins ordering */
} bzQuestInputHit_t;

/* Immutable per-frame world the hit-test evaluates against. `hudFrame` may be
 * NULL (no HUD captured yet); entities may be NULL/0. `transform` NULL means
 * "no valid map bounds this frame" (raw passthrough inverse). `planeY` is the
 * composed-space height of the diorama base the terrain ray-plane test uses
 * (0 - terrain vertices sit at height*scale with no offset, see
 * bz_quest_wc3_render.h). `generation` is the snapshot generation carried
 * through for the HUD staleness check AND the post staleness check (same
 * value for both, never divergent). */
typedef struct {
    const bzQuestHudFrame_t *hudFrame;
    const bzQuestInputEntity_t *entities;
    uint32_t entityCount;
    const bzQuestWc3WorldTransform_t *transform;
    float planeY;
    uint64_t generation;
    bzQuestHudActionTarget_t targetMode;
} bzQuestInputWorld_t;

/*
 * Evaluates the deterministic ray-hit priority, IN THIS EXACT ORDER, for a
 * ray already expressed in COMPOSED space (the caller applies
 * bz_quest_board_transform_inverse_ray() first):
 *   1. HUD action/cancel hit region (bz_quest_wc3_hud_hit_test() against the
 *      SAME frame the renderer draws - a hidden slot yields no region at all;
 *      a disabled/stale-frame-id slot is reported as HUD_DISABLED and
 *      consumes the ray without producing a command, so the player still gets
 *      "you hit the panel" feedback rather than the ray passing through onto
 *      a unit behind it).
 *   2. nearest entity sphere (smallest positive ray t wins).
 *   3. board base plane (ray-plane at composed y = planeY), converted back to
 *      engine world coords via bz_quest_wc3_world_transform_point_inverse().
 *   4. no hit.
 * Returns the resolved bzQuestInputHit_t (kind NONE if nothing hit). `dir`
 * need not be unit length. Pure and stateless.
 */
void bz_quest_input_hit_test(const bzQuestInputWorld_t *world, const float rayOrigin[3],
                             const float rayDir[3], bzQuestInputHit_t *out);

/* --- Command decision -------------------------------------------------- */

/* Mirrors platform/bridge/bz_tabletop_transport.h's bzTTCommandType_t (the
 * transport-independent copy this module posts through - the impure driver
 * maps 1:1 onto BZ_TT_PostSelect/SmartEntity/SmartPoint/Button/Cancel/
 * TargetPoint). BZ_QUEST_INPUT_CMD_NONE means "no command this frame". */
typedef enum {
    BZ_QUEST_INPUT_CMD_NONE = 0,
    BZ_QUEST_INPUT_CMD_SELECT,
    BZ_QUEST_INPUT_CMD_SMART_ENTITY,
    BZ_QUEST_INPUT_CMD_SMART_POINT,
    BZ_QUEST_INPUT_CMD_BUTTON,
    BZ_QUEST_INPUT_CMD_CANCEL,
    BZ_QUEST_INPUT_CMD_TARGET_POINT,
} bzQuestInputCommandType_t;

typedef struct {
    bzQuestInputCommandType_t type;
    uint32_t selectIds[BZ_QUEST_INPUT_MAX_SELECT_IDS]; /* valid iff type == SELECT */
    uint32_t selectCount;
    uint32_t targetEntity;                             /* valid iff type == SMART_ENTITY */
    float x, y;                                        /* valid iff type == SMART_POINT/TARGET_POINT (engine world) */
    char code[BZ_QUEST_INPUT_MAX_BUTTON_CODE];         /* valid iff type == BUTTON (NUL-terminated) */
    uint8_t hand;                                      /* which controller triggered it (for haptics) */
} bzQuestInputCommand_t;

/* --- Interaction phase (one enum, never parallel booleans - AGENTS.md) --- */

typedef enum {
    BZ_QUEST_INPUT_PHASE_IDLE_RAY = 0,     /* ray pointing, no gesture owns input */
    BZ_QUEST_INPUT_PHASE_BOARD_MANIPULATE, /* a board pan/rotate/zoom gesture owns input this frame */
    BZ_QUEST_INPUT_PHASE_TARGET_POINT_MODE,/* server target != NONE owns input (gameplay targeting) */
} bzQuestInputPhase_t;

/* --- Layer 8: controller-vs-hand source arbitration --------------------- *
 * Deterministic, hysteresis-debounced arbitration between the two possible
 * per-hand input sources (Touch controller vs. hand tracking) - see
 * docs/quest-tabletop.md's "Deterministic source arbitration". An enum, not
 * two booleans (AGENTS.md: "do not use several booleans to represent
 * mutually exclusive state"), since exactly one source is ever authoritative
 * for a given hand slot in a given frame. CONTROLLER is the zero/default
 * value so a freshly bz_quest_input_state_init()'d state (all zeroed) starts
 * every hand favoring the controller, matching this layer's "Touch remains
 * preferred" contract even before the very first bz_quest_input_state_update()
 * call ever runs arbitration. */
typedef enum {
    BZ_QUEST_INPUT_SOURCE_CONTROLLER = 0,
    BZ_QUEST_INPUT_SOURCE_HAND,
} bzQuestInputSourceKind_t;

/* Debounce window (seconds) a Touch controller must be continuously inactive
 * before a hand sample is allowed to take over that hand's input slot - see
 * bz_quest_input_arbitrate_source(). Sized to comfortably absorb a single
 * dropped-frame tracking blip (even at a low 30 Hz XR frame rate, one frame
 * is ~33 ms) while still feeling prompt to a real user; unvalidated on real
 * hardware (see docs/quest-tabletop.md's hardware-only acceptance gates),
 * trivially tunable. */
#define BZ_QUEST_HAND_SOURCE_SWITCH_DEBOUNCE_SEC 0.3f

/*
 * Resolves ONE hand's authoritative input source for this frame, given the
 * PREVIOUS frame's winning source (`previous`) and this frame's raw
 * activity. Touch is the default/preferred source and reclaims it the
 * INSTANT the controller reports active again - no debounce on reclaim,
 * since the preferred device coming back is never something to
 * second-guess. A hand sample only becomes authoritative after the
 * controller has been continuously inactive for
 * BZ_QUEST_HAND_SOURCE_SWITCH_DEBOUNCE_SEC (so a momentary controller
 * tracking blip - one dropped frame - never bounces the source to hand and
 * back), AND the hand is itself active that same frame. Whenever a hand that
 * was previously authoritative stops being active, arbitration immediately
 * reports CONTROLLER again (even if the controller is ALSO currently
 * inactive - that still produces the exact same "no usable sample" outcome
 * downstream as continuing to report HAND-but-inactive would, and keeps the
 * enum's steady state identical to the "hand tracking was never enabled"
 * default every existing Touch-only test/build already relies on - see
 * bz_quest_input_state_update()'s call site for why this can never regress
 * controller-only behavior).
 *
 * `controllerLossSeconds` is the only persisted state (a debounce timer),
 * owned by the caller (bzQuestInputState_t) and reset to 0 every time the
 * controller is active. Pure and stateless beyond that one in/out float.
 */
bzQuestInputSourceKind_t bz_quest_input_arbitrate_source(bzQuestInputSourceKind_t previous,
                                                          bool controllerActive, bool handActive, float dt,
                                                          float *controllerLossSeconds);

/* Persistent interaction state (survives across frames). Zero-initialize once;
 * bz_quest_input_state_init() sets the board to its default. Everything else
 * is edge latches + idempotent-clear bookkeeping. */
typedef struct {
    bzQuestBoardTransform_t board;
    bzQuestInputPhase_t phase;
    /* Per-hand edge latches. */
    bzQuestButtonEdge_t selectEdge[BZ_QUEST_INPUT_HAND_COUNT];
    bzQuestButtonEdge_t squeezeEdge[BZ_QUEST_INPUT_HAND_COUNT];
    bzQuestButtonEdge_t secondaryEdge[BZ_QUEST_INPUT_HAND_COUNT];
    bzQuestButtonEdge_t menuEdge;
    /* Left grip-pan drag anchor (valid while dragging). */
    bool panDragging;
    float panAnchor[3];
    /* Idempotent-clear bookkeeping: the transient state is cleared exactly
     * once on each condition-entry (focus loss, controller loss, generation/
     * map change), never re-fired every frame while the condition persists.
     * `initialized` guards the first-ever update from spuriously treating
     * generation 0 as a "change". */
    bool initialized;
    bool wasFocused;
    bool leftWasActive, rightWasActive;
    uint64_t lastGeneration;
    uint64_t lastMapEpoch; /* bumps only on a real map-name change (see frame->mapEpoch) */
    /* Layer 8: per-hand controller-vs-hand source arbitration bookkeeping -
     * see bzQuestInputSourceKind_t/bz_quest_input_arbitrate_source() above.
     * `source` is the previous frame's winning source (so a CHANGE can be
     * detected and cleared exactly once, same idempotent-clear discipline as
     * the fields above); `controllerLossSeconds` is the debounce timer. */
    bzQuestInputSourceKind_t source[BZ_QUEST_INPUT_HAND_COUNT];
    float controllerLossSeconds[BZ_QUEST_INPUT_HAND_COUNT];
} bzQuestInputState_t;

/* Per-hand raw controller sample for one frame (all in LOCAL tracking space,
 * already unpacked from OpenXR by bz_quest_xr_actions.c). `active` false means
 * the aim/grip pose or the whole controller is unavailable this frame. */
typedef struct {
    bool active;
    bool aimValid;
    float aimOrigin[3], aimDir[3]; /* aim ray in tracking space */
    float gripPos[3];              /* grip pose position in tracking space */
    bool selectDown;               /* trigger (bool/threshold) */
    bool squeezeDown;              /* grip/grab */
    bool primaryDown;              /* A/X - additive-selection modifier */
    bool secondaryDown;            /* B/Y - cancel */
    float thumbstick[2];           /* [-1,1] x,y */
} bzQuestInputHandSample_t;

/* Everything bz_quest_input_state_update() needs for one frame. */
typedef struct {
    float dt;                                   /* seconds since last update (clamped internally) */
    bool focused;                               /* XR session focused this frame */
    bzQuestInputHandSample_t hands[BZ_QUEST_INPUT_HAND_COUNT]; /* Touch-controller sample per hand */
    /* Layer 8: hand-tracking sample per hand (bz_quest_hand_input.h's
     * bz_quest_hand_sample_build() output), alongside - never replacing -
     * `hands` above. bz_quest_input_state_update() arbitrates between the
     * two per hand (bz_quest_input_arbitrate_source()) before anything else
     * runs; every hit-test/board/command-mapping code path below is fed
     * only the single winning bzQuestInputHandSample_t and stays completely
     * unaware of which source produced it. Zeroed/inactive when hand
     * tracking is unsupported or this hand's tracker isn't active this
     * frame - never partially filled. */
    bzQuestInputHandSample_t handSample[BZ_QUEST_INPUT_HAND_COUNT];
    bool menuDown;                              /* left menu click */
    bzQuestInputWorld_t world;                  /* hit-test world (see above) */
    const uint32_t *selectedIds;                /* current server selection set (for additive select); may be NULL */
    uint32_t selectedCount;
    /* Monotonic map-identity token: the impure capture layer bumps this ONLY
     * when BZ_TTSnapshot_MapName() changes (never per snapshot generation,
     * which increments every frame) - a change triggers exactly one
     * transient-state clear + board reset (map reload). */
    uint64_t mapEpoch;
} bzQuestInputFrame_t;

/* Which reticle/pointer feedback the renderer should show this frame - pure
 * output, consumed by bz_quest_vk_wc3_pointer.c. `hit*` are in COMPOSED space
 * (the renderer applies the board transform when drawing). */
typedef struct {
    bool visible[BZ_QUEST_INPUT_HAND_COUNT]; /* draw this hand's ray at all */
    bool hasReticle[BZ_QUEST_INPUT_HAND_COUNT];
    float reticle[BZ_QUEST_INPUT_HAND_COUNT][3]; /* composed-space hit point */
    bzQuestInputHitKind_t hitKind[BZ_QUEST_INPUT_HAND_COUNT];
    /* Layer 8: the TRACKING-space aim ray this hand actually used this
     * frame - copied from the ARBITRATED bzQuestInputHandSample_t
     * (controller or hand, whichever bz_quest_input_arbitrate_source()
     * picked), never the raw pre-arbitration controller-only sample the
     * caller originally passed into bz_quest_input_state_update(). The
     * renderer needs this because it has no other way to see the
     * arbitration result: bz_quest_renderer_process_input() builds the
     * pointer beam/reticle geometry from this, not from re-reading its own
     * frame.hands[] (which arbitration never touches, by design - see
     * bz_quest_input_state_update()). Valid iff `visible[hand]`. */
    float aimOrigin[BZ_QUEST_INPUT_HAND_COUNT][3];
    float aimDir[BZ_QUEST_INPUT_HAND_COUNT][3];
} bzQuestInputFeedback_t;

typedef struct {
    bzQuestInputCommand_t command; /* type NONE if nothing to post */
    bool hasCommand;
    bzQuestInputFeedback_t feedback;
    bool clearedThisFrame;         /* transient state was idempotently cleared this frame (for tests/diagnostics) */
    /* Local haptic decision for the NO-command-posted cases the pure module
     * fully owns (a hit on a hidden/disabled/stale HUD slot, or a terrain tap
     * with no valid target in ENTITY-only target mode): a "rejected" pulse so
     * the player feels the input was received-but-refused. For cases that DO
     * post a command, the impure driver applies bz_quest_haptic_pulse(accepted)
     * from the bzTTResult_t of the post instead (accept on success, reject on
     * queue-full/stale/invalid) - see bz_quest_xr_actions.c. */
    bool wantHaptic;
    bool hapticAccepted;
    uint8_t hapticHand;
} bzQuestInputOutput_t;

/* Initializes persistent state: board -> default, all latches cleared. */
void bz_quest_input_state_init(bzQuestInputState_t *state);

/*
 * Idempotently clears TRANSIENT interaction state (edge latches, pan drag,
 * phase -> IDLE_RAY). `resetBoard` additionally restores the default board
 * placement (used on renderer teardown / engine stop / map reload, matching
 * visionOS's "lifecycle teardown restores default placement"). Safe to call
 * repeatedly; a second consecutive call is a no-op beyond re-zeroing already-
 * zero latches. Exposed so bz_quest_xr_actions.c / bz_quest_renderer.c can
 * force a clear on an explicit lifecycle event without a fake frame.
 */
void bz_quest_input_state_clear(bzQuestInputState_t *state, bool resetBoard);

/*
 * The one per-frame entry point: runs edge detection, idempotent lifecycle
 * clears, board pan/rotate/zoom, the deterministic ray-hit priority, and the
 * table-driven command mapping, updating `state` in place and writing the
 * single command (if any) + reticle feedback to `out`. Pure w.r.t. the engine/
 * OpenXR/Vulkan - the only side effects are on `state`/`out`. At most ONE
 * gameplay command is produced per call (right hand is the primary pointer;
 * the left hand is evaluated only when the right produced nothing and the
 * left is not board-grabbing), so a held button never duplicates a command.
 */
void bz_quest_input_state_update(bzQuestInputState_t *state, const bzQuestInputFrame_t *frame,
                                 bzQuestInputOutput_t *out);

/* --- Haptics (pure decision, applied by the impure driver) ------------- */

typedef struct {
    float amplitude;      /* [0,1] */
    int64_t durationNanos;
    float frequency;      /* Hz; 0 => XR_FREQUENCY_UNSPECIFIED at the driver */
} bzQuestHapticPulse_t;

/* An accepted command gets a short, crisp confirmation pulse; a rejected one
 * (queue full / stale generation / invalid / disabled-button hit) gets a
 * longer, softer "buzz" so the two are unmistakably distinct through the
 * controller. Pulled out as a pure, tested decision so the accept/reject
 * feel has one definition, not an inline literal at each call site. */
bzQuestHapticPulse_t bz_quest_haptic_pulse(bool accepted);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_INPUT_STATE_H */
