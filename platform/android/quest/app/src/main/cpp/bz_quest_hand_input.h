/*
 * bz_quest_hand_input.h - layer 8: PURE Meta Quest hand-tracking gesture
 * builder. Turns one hand's raw joint positions (XR_EXT_hand_tracking) or
 * Meta's standardized aim/pinch state (XR_FB_hand_tracking_aim, when
 * negotiated) into exactly the same bzQuestInputHandSample_t type the
 * Touch-controller path already produces (bz_quest_input_state.h) - see
 * that header's `bzQuestInputHandSample_t`. This is the whole point of the
 * split: bz_quest_input_state.c's interaction state machine, ray-hit
 * priority, and command-mapping table are reused byte-for-byte for hands,
 * with NO parallel command mapper/state machine and NO widened bridge/
 * transport ABI (see AGENTS.md's "keep every OpenXR type strictly inside
 * Quest host modules" rule and docs/quest-tabletop.md's "Layer 8").
 *
 * Like bz_quest_input_state.h/bz_quest_xr_bindings.h, every type/function
 * here is plain C POD/math only: no XrHandTrackerEXT/XrHandJointLocationEXT/
 * XrHandTrackingAimStateFB ever appears in this file. The impure
 * bz_quest_xr_hands.c owns the real OpenXR hand-tracking handles/calls,
 * unpacks its results into this header's plain float/bool types, and calls
 * bz_quest_hand_sample_build() below. platform/android/quest/tests/
 * test_bz_quest_hand_input.c exercises this exact gesture logic with a
 * plain host C compiler, no NDK/OpenXR/Vulkan/engine link.
 *
 * -- Frame-critical / real-time discipline --
 * bz_quest_hand_sample_build() runs once per hand, every frame, on the XR
 * render thread. It allocates nothing, locks nothing, does no file I/O,
 * calls no bridge/transport API, and calls no logging function - see
 * platform/android/quest/scripts/test-quest-hand-tracking-layout.sh, which
 * greps this exact function body for any of those, mirroring
 * bz_quest_audio.c's bz_quest_audio_data_callback() real-time contract.
 *
 * -- EXT-only ray-basis choice (why positions, not orientations) --
 * The OpenXR hand-joint orientation-axis convention (which local axis of a
 * XrHandJointLocationEXT's pose.orientation is "forward") lives only in the
 * spec's prose "Convention of Hand Joints" section, which could not be
 * fetched/verified from this environment (the bundled openxr.h header only
 * declares the struct layout, not the convention prose - see docs/quest-
 * tabletop.md's "Layer 8" research notes). Rather than guess a sign/axis
 * convention this project cannot verify (forbidden by AGENTS.md's "never
 * guess at a bug fix" rule, which applies equally to never guessing at a
 * NEW piece of geometry), the EXT-only fallback ray is instead grounded
 * entirely in tracked JOINT POSITIONS, which the header unambiguously
 * defines as plain world/reference-space coordinates: ray origin is the
 * index fingertip position, direction is the normalized vector from the
 * index metacarpal joint to the index fingertip (the finger's own,
 * long-baseline pointing direction) - "a stable ray basis grounded in
 * tracked joint poses" per this layer's task contract, without depending on
 * an unverified orientation convention.
 */
#ifndef BZ_QUEST_HAND_INPUT_H
#define BZ_QUEST_HAND_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_input_state.h" /* bzQuestInputHandSample_t - the shared per-hand sample type */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Which hand-tracking data source this session negotiated - decided ONCE,
 * at session-create time, from real OpenXR instance-extension/system-
 * property capability (see bz_quest_xr.h's bzQuestXrHandCapability_t), never
 * re-decided per frame or per hand. An enum (AGENTS.md), not two booleans,
 * since exactly one level is authoritative for the whole session.
 */
typedef enum {
    /* XR_EXT_hand_tracking unavailable/disabled this session (missing from
     * the runtime, or the system reports no hand-tracking support) - hand
     * tracking produces no samples at all; Touch controllers are the only
     * input source. Never a startup failure - see bz_quest_xr.h. */
    BZ_QUEST_HAND_CAPABILITY_NONE = 0,
    /* XR_EXT_hand_tracking only: joint positions, no Meta aim/pinch bits.
     * bz_quest_hand_sample_build() falls back to an explicit, hysteresis-
     * debounced pinch-distance threshold and the joint-position-only ray
     * basis described above. */
    BZ_QUEST_HAND_CAPABILITY_EXT_ONLY,
    /* XR_FB_hand_tracking_aim additionally negotiated: use the runtime's
     * standardized aim pose + index/middle pinch bits directly - no
     * distance thresholding of this module's own. */
    BZ_QUEST_HAND_CAPABILITY_FB_AIM,
} bzQuestHandCapability_t;

/*
 * POD copy of exactly the four XR_HAND_JOINT_COUNT_EXT joint positions this
 * module needs (unpacked by bz_quest_xr_hands.c from XrHandJointLocationEXT
 * - no Xr types here), each independently validated by the caller against
 * XR_SPACE_LOCATION_POSITION_VALID_BIT. Only positions are used - see this
 * file's header comment for why joint orientation is deliberately never
 * read.
 */
typedef struct {
    bool wristValid;
    float wristPos[3];           /* XR_HAND_JOINT_WRIST_EXT - board-pan grip-pose analog */
    bool indexMetacarpalValid;
    float indexMetacarpalPos[3]; /* XR_HAND_JOINT_INDEX_METACARPAL_EXT - ray-direction base */
    bool indexTipValid;
    float indexTipPos[3];        /* XR_HAND_JOINT_INDEX_TIP_EXT - ray origin + pinch distance */
    bool thumbTipValid;
    float thumbTipPos[3];        /* XR_HAND_JOINT_THUMB_TIP_EXT - pinch distance */
} bzQuestHandJoints_t;

/*
 * POD copy of exactly the XrHandTrackingAimStateFB fields this module uses
 * (unpacked by bz_quest_xr_hands.c). `valid` is
 * (status & (COMPUTED_BIT_FB|VALID_BIT_FB)) == both, per the OpenXR
 * registry's XR_FB_hand_tracking_aim definition. `indexPinching`/
 * `middlePinching` are the runtime's OWN status bits
 * (XR_HAND_TRACKING_AIM_INDEX_PINCHING_BIT_FB/_MIDDLE_PINCHING_BIT_FB) - per
 * Meta's hand-tracking guidance (developers.meta.com/horizon/documentation/
 * native/android/mobile-hand-tracking/, see docs/quest-tabletop.md), an app
 * should trust the runtime's own *Pinching status bit directly rather than
 * threshold the raw per-finger strength itself, so this module never
 * re-derives pinch from a distance/strength at this capability level - only
 * the EXT-only fallback below does that.
 */
typedef struct {
    bool valid;
    float aimOrigin[3], aimDir[3]; /* aimPose position + local -Z-forward direction, tracking space */
    bool indexPinching;
    bool middlePinching;
} bzQuestHandAimSample_t;

/*
 * Persisted per-hand hysteresis latch for the EXT-only pinch-distance
 * fallback (meaningful only at BZ_QUEST_HAND_CAPABILITY_EXT_ONLY - ignored
 * at the other two levels). Owned by the caller as Quest-local renderer
 * state (bz_quest_xr_hands.c's bzQuestXrHands_t), NOT bzQuestInputState_t -
 * this is a sensor-fusion detail of translating raw joints into one
 * bzQuestInputHandSample_t, not part of the shared interaction/command
 * state machine (see docs/quest-tabletop.md's "Layer 8" module map).
 */
typedef struct {
    bool pinching;
} bzQuestHandPinchState_t;

/* EXT-only pinch-distance hysteresis band (meters, thumb-tip to index-tip
 * gap). RELEASE is wider than ENGAGE so a fingertip gap sitting right at
 * one threshold can never chatter the pinch state every frame - the same
 * "distinct engage/release bands" hysteresis shape as e.g.
 * BZ_QUEST_HAND_SOURCE_SWITCH_DEBOUNCE_SEC in bz_quest_input_state.h.
 * Unvalidated on real hardware (see docs/quest-tabletop.md's hardware-only
 * acceptance gates) - a deliberately bounded, documented, trivially-tunable
 * estimate, exactly like this layer's existing haptic-pulse/board-rate
 * constants already are. */
#define BZ_QUEST_HAND_PINCH_ENGAGE_M 0.025f
#define BZ_QUEST_HAND_PINCH_RELEASE_M 0.035f

/* Everything bz_quest_hand_sample_build() needs for one hand, one frame. */
typedef struct {
    bzQuestHandCapability_t capability;
    bool trackerActive;        /* XrHandJointLocationsEXT.isActive this frame */
    bzQuestHandJoints_t joints; /* populated whenever trackerActive, at BOTH non-NONE capability levels */
    bzQuestHandAimSample_t aim; /* only meaningful when capability == FB_AIM */
} bzQuestHandSampleInput_t;

/*
 * Builds this hand's bzQuestInputHandSample_t - the SAME per-hand sample
 * type the Touch-controller path already produces - from either the FB aim
 * state (BZ_QUEST_HAND_CAPABILITY_FB_AIM) or the EXT-only joint fallback
 * (BZ_QUEST_HAND_CAPABILITY_EXT_ONLY), per `in->capability`. Pure and
 * frame-critical-safe - see this file's header comment.
 *
 * `out->active` is false whenever `in->trackerActive` is false or
 * `in->capability` is NONE - an inactive/unsupported hand is never treated
 * as "still pressed" downstream (bz_quest_edge_update() enforces this the
 * same way it already does for a disconnected controller - see
 * bz_quest_input_state.c). Every other field is computed per capability
 * level:
 *
 *   FB_AIM: aimValid = in->aim.valid; selectDown = valid && indexPinching;
 *     squeezeDown = valid && middlePinching (the evidence-backed second-
 *     pinch/grab this layer's task contract permits, since Meta's own
 *     extension exposes a dedicated middle-finger pinch bit for exactly
 *     this purpose).
 *   EXT_ONLY: aimValid requires the index metacarpal AND tip joints both
 *     valid; selectDown additionally requires the thumb tip valid and the
 *     index-tip/thumb-tip distance to cross the engage/release hysteresis
 *     band (BZ_QUEST_HAND_PINCH_ENGAGE_M/_RELEASE_M via `*pinchState`);
 *     squeezeDown is ALWAYS false - no second pinch/grab signal exists
 *     without FB aim, and this layer does not invent one (see docs/quest-
 *     tabletop.md's "Unsupported hand semantics").
 *   NONE: `out` is fully zeroed/inactive.
 *
 * `gripPos` (the board-pan anchor analog) is always the wrist joint
 * position when trackerActive+wristValid, at any non-NONE capability level
 * - board-pan only actually engages when squeezeDown is also true, which
 * per the above only ever happens at the FB_AIM level.
 *
 * `primaryDown`/`secondaryDown`/`thumbstick` have no reliable hand-tracking
 * analog (no Quest API evidence for either) and are always false/zero; the
 * HUD's own Cancel region (reachable via select/pinch, same priority as any
 * other HUD button - see bz_quest_input_state.c's kTargetTable) remains the
 * only hand-reachable cancel path, and board yaw/zoom/height (thumbstick-
 * driven) remains controller-only - both intentional, documented gaps, not
 * bugs (see docs/quest-tabletop.md).
 *
 * `*pinchState` is idempotently reset (pinching -> false) whenever this call
 * reports the hand inactive/unsupported, so a later reacquisition always
 * requires crossing the ENGAGE threshold fresh - it can never "resume" a
 * pinch that was merely under the wider RELEASE threshold at the moment
 * tracking was lost (see docs/quest-tabletop.md's "require a fresh pinch/
 * grab edge after reacquisition").
 */
void bz_quest_hand_sample_build(const bzQuestHandSampleInput_t *in, bzQuestHandPinchState_t *pinchState,
                                bzQuestInputHandSample_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_HAND_INPUT_H */
