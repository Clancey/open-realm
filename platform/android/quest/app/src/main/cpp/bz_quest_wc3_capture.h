/*
 * bz_quest_wc3_capture.h - layer 5A: the one impure translation unit that
 * calls BZ_TT_Latest()/BZ_TTSnapshot_EntityAt()/BZ_TTA_RegisterConfigString()/
 * BZ_TTA_ResolveEntityMetadata()/BZ_TTA_RegisterModelTexture()/
 * BZ_TTAsset_*() to turn the latest retained snapshot into the plain-POD
 * bzQuestWc3EntityInput_t list bz_quest_wc3_render.h's
 * bz_quest_wc3_build_render_list() consumes, plus freshly decoded model
 * geometry/texture pixel data for any model/texture touched this frame.
 *
 * Mirrors bz_quest_snapshot.c's discipline exactly (see that file's header
 * comment): retain/copy/release on every branch, no bridge pointer/handle
 * survives past this one call, and - like bz_quest_snapshot.c - this module
 * has no direct unit test. All the decision logic that *can* be tested
 * (coordinate/scale math, render-list construction, cache bookkeeping) is
 * already pushed into bz_quest_wc3_render.c/bz_quest_wc3_cache.c, which
 * *do* have full host test coverage; what remains here is a thin,
 * mechanically-obvious sequence of ABI calls plus two bounded static
 * scratch buffers, reviewed by inspection rather than by a fake-ABI test
 * harness - precisely the same trade-off bz_quest_snapshot.c already makes
 * for the transport ABI.
 *
 * -- Model geometry decode policy (deliberate, documented trade-off) --
 * This slice does not ask the Vulkan GPU cache "do you already have this
 * model's geometry" before deciding whether to decode it from the ABI: any
 * model touched this frame that is new *this frame* (deduplicated only
 * within the frame, by model config-string index) is decoded fully, and
 * bz_quest_vk_wc3.c's own identity-keyed cache (bz_quest_wc3_cache.h)
 * silently discards the redundant CPU-side copy on a cache hit rather than
 * re-uploading it. This means CPU decode cost recurs every frame for every
 * unique model touched even when its GPU buffer is already resident - a
 * real but bounded cost (a few hundred KB of struct-of-arrays -> AoS
 * interleave per unique model, not a disk/MPQ read - the underlying
 * bzTTAsset_t is itself already a decoded, retained, provider-owned MDX
 * structure, see games/warcraft-3/visionos/wc3_tabletop_assets.c). Task
 * priority is explicit ("favor correct ownership and draw ordering over
 * speculative optimization"), so this slice takes the simple, correct path;
 * a later layer can add a cross-module "already GPU-cached" query once
 * profiling on real hardware justifies the extra complexity. The one
 * exception: the per-model bzQuestWc3ModelAnim_t arena is only built when
 * callbacks->onModelReady is non-NULL, since that callback is the arena's
 * only free/ownership-transfer path (see model_ready_cb() in
 * bz_quest_vk_wc3.c) - building it with no consumer would leak it, not
 * just waste CPU.
 *
 * -- Texture decode policy (same trade-off, applied to replaceable_id 0/1/2) --
 * replaceable_id 0 (direct/non-team texture, see
 * platform/bridge/bz_tabletop_assets.h's BZ_TTA_ModelTextureInfo comment)
 * and 1/2 (team color/team glow, resolved via BZ_TTA_RegisterTeamTexture()
 * per LiveTabletopTransport.swift:281-295 - added in layer 5C so animated
 * team-colored units/buildings render correctly, see
 * bz_quest_wc3_capture_frame()'s per-entity team texture resolution) are
 * supported. Any per-
 * entity override image (replaceable_id outside {0,1,2}, resolved via
 * entity.metadata.image + BZ_TTA_ASSET_IMAGE at CS_IMAGES,
 * LiveTabletopTransport.swift:267-280) remains deliberately out of scope -
 * logged once per unique (model identity, layer index), the layer is
 * skipped (not drawn with a substituted texture), never silently demoted.
 * This is the same category of scoped exclusion as this project's
 * documented exclusion of billboarding/TXAN/KMTF/particles/HUD/etc. (see
 * docs/quest-tabletop.md's "Layer 5C" section), not a bug.
 */
#ifndef BZ_QUEST_WC3_CAPTURE_H
#define BZ_QUEST_WC3_CAPTURE_H

#include <stdint.h>

#include "bz_quest_wc3_fog.h"
#include "bz_quest_wc3_hud.h"
#include "bz_quest_wc3_render.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Invoked once per unique model (deduplicated by model config-string index
 * within one frame) that needs its geometry considered for the GPU cache.
 * `model` points at bz_quest_wc3_capture_frame()'s single static scratch
 * buffer (see this file's header comment) - valid only for the duration of
 * this call; the callee (bz_quest_vk_wc3.c in production, a counting fake
 * in tests of *that* module) must copy anything it needs out before
 * returning, never retain the pointer.
 */
typedef void (*bzQuestWc3ModelReadyFn)(const bzQuestWc3Model_t *model, void *userdata);

/*
 * Invoked once per unique, supported (replaceable_id == 0) texture
 * reference touched this frame - not deduplicated across different models'
 * use of the same texture identity within capture.c itself (see this file's
 * header comment); bz_quest_vk_wc3.c's identity-keyed image cache is what
 * actually deduplicates the GPU upload. `pixels` points at
 * bz_quest_wc3_capture_frame()'s single static pixel scratch buffer - valid
 * only for the duration of this call, must be copied out (e.g. into a
 * staging buffer) before returning, never retained.
 */
typedef void (*bzQuestWc3TextureReadyFn)(const char *identity, uint32_t width, uint32_t height,
                                         uint32_t rowBytes, const uint8_t *pixels,
                                         uint32_t dataBytes, void *userdata);

typedef struct {
    bool available;
    uint32_t width, height;
    uint32_t targetMode; /* raw bzTTActionTarget_t value; mode only, no transported point/entity payload exists */
    bzQuestWc3FogBounds_t bounds;
    /* Shared world/tabletop transform derived from the SAME map bounds
     * above (bz_quest_wc3_world_transform_measure()) - carried alongside
     * `bounds` so bz_quest_vk_wc3_fog.c's fog-quad vertex shader can place
     * the overlay on screen at exactly the same position as terrain and
     * entities, without a second bounds fetch or a second transform
     * derivation. See bz_quest_wc3_render.h's header comment. */
    bzQuestWc3WorldTransform_t transform;
    uint8_t visible[BZ_QUEST_WC3_FOG_MAX_CELLS];
    uint8_t explored[BZ_QUEST_WC3_FOG_MAX_CELLS];
} bzQuestWc3FogCapture_t;

typedef struct {
    bzQuestWc3ModelReadyFn onModelReady;
    void *modelUserdata;
    bzQuestWc3TextureReadyFn onTextureReady;
    void *textureUserdata;
} bzQuestWc3CaptureCallbacks_t;

/*
 * Acquires BZ_TT_Latest(), walks its entities, and for each entity with a
 * non-zero model config-string reference: resolves category/footprint via
 * BZ_TTA_ResolveEntityMetadata() (team_color override, matching
 * LiveTabletopTransport.swift:375-394's exact input construction), registers
 * its model config string (base 32, matching modelConfigStringBase -
 * LiveTabletopTransport.swift:212) to obtain a retained model asset and its
 * identity string, and appends one bzQuestWc3EntityInput_t to
 * `out->entities`. Every *newly*-seen model config-string index this frame
 * additionally gets a full geometry decode (see this file's header comment)
 * fed to `callbacks->onModelReady`, and every newly-seen, supported direct
 * texture reference on that model gets its pixels decoded and fed to
 * `callbacks->onTextureReady`. Every retained asset handle is released on
 * every branch (success, malformed, overflow) before returning; the
 * snapshot itself is released last. Safe to call with `callbacks` fields
 * NULL (no-ops, geometry/texture decode is simply skipped - never a crash),
 * though production wiring always supplies both.
 *
 * Must only be called from the Quest XR/render thread, immediately
 * alongside bz_quest_snapshot_capture() (see that header's own thread-
 * pinning rationale, which applies identically here): both read the *same*
 * kind of BZ_TT_Latest() generation's data, so this function performs its
 * own independent BZ_TT_Latest()/Release() pair rather than sharing one
 * snapshot handle with bz_quest_snapshot_capture() - retaining a snapshot
 * across two unrelated call sites would need extra lifetime bookkeeping
 * this slice does not need (a one-generation skew between the two calls'
 * snapshots is immaterial: both still observe a fully self-consistent,
 * single point-in-time snapshot each, never a torn one). Does not take a
 * bzTabletopLifecycle_t* - unlike bz_quest_snapshot_capture(), this
 * function has no diagnostic lifecycle-state/last-error fields to fill, and
 * BZ_TT_Latest() already returns NULL/an entity-less snapshot for every
 * pre-connect/idle/stopped state on its own, so no separate lifecycle gate
 * is needed here.
 */
void bz_quest_wc3_capture_frame(const bzQuestWc3CaptureCallbacks_t *callbacks,
                                bzQuestWc3RenderList_t *outRenderList);

/*
 * Returns a Quest-owned monotonic render-clock value in milliseconds, for
 * global-sequence sampling only (see bz_quest_wc3_anim.h's header comment
 * on why global sequences deliberately sample a render clock rather than
 * entity/sequence time - the exact desktop analog of `tr.viewDef.time` /
 * `SDL_GetTicks()`, r_mdx_anim.c:34-41). Backed by CLOCK_MONOTONIC (never
 * CLOCK_REALTIME, which can jump backward/forward on wall-clock changes and
 * would corrupt the modulo-duration wraparound this value feeds into). The
 * absolute epoch is irrelevant - only the msec delta across frames matters,
 * exactly as desktop's own free-running tick counter never resets mid-game.
 */

/*
 * Independent fog-snapshot retain/copy/release helper for layer 5D. Mirrors
 * bz_quest_wc3_capture_frame()'s "each call site retains its own immutable
 * snapshot" discipline exactly: this function does its own BZ_TT_Latest()/
 * Release() pair rather than sharing a retained handle with the model/terrain
 * capture paths, because a one-generation skew across those unrelated call
 * sites is immaterial while shared lifetime bookkeeping would add complexity.
 *
 * On success, copies one full fog generation (dimensions, map bounds, visible
 * plane, explored plane, and the player's current target MODE flag) into `out`
 * and returns true. Returns false - after fully clearing `out` and releasing
 * the snapshot on every branch - when no fog buffer is currently published, no
 * map bounds accompany the fog, the ABI version mismatches, or the fog grid
 * exceeds bz_quest_wc3_fog.h's real Warcraft III "Huge" map cap.
 *
 * `targetMode` is copied for completeness/documentation only: the transport ABI
 * exposes no matching target point or target entity id for POINT/ENTITY mode,
 * so layer 5D does NOT render a target marker from it.
 */
bool bz_quest_wc3_capture_fog(bzQuestWc3FogCapture_t *out);

/*
 * Layer 5E's independent HUD-snapshot retain/copy/release helper - same
 * "each call site retains its own immutable snapshot" discipline as
 * bz_quest_wc3_capture_fog() above (see that function's comment; the same
 * one-generation-skew-is-immaterial reasoning applies here identically).
 *
 * On success, copies the player's resource/status fields
 * (BZ_TTSnapshot_Player()), the authoritative command-card layout
 * (BZ_TTSnapshot_ActionLayout(), including every button's hidden/disabled/
 * cooldown/target/semantic/hotkey/grid position - truncating tooltip/
 * action-code strings into bz_quest_wc3_hud.h's bounded buffers, never
 * silently growing past the transport's own bounds), and the current
 * selection count (BZ_TTSnapshot_SelectedEntityIds()) into `out`, stamps
 * `out->frameId` with BZ_TTSnapshot_Generation() (the same generation the
 * later hit-test staleness check compares against), and returns true.
 *
 * Returns false (leaving `out` zeroed) only when no snapshot has ever been
 * published or its ABI version does not match this build - unlike
 * bz_quest_wc3_capture_fog(), there is no "degenerate bounds" failure mode
 * here: the HUD panel's placement is deliberately map-bounds-independent
 * (bz_quest_wc3_hud_panel_transform()), so a missing/degenerate map does
 * not prevent the HUD itself from being built (a distinct "no player data"
 * status line is what layer 5E shows for that state - see
 * bz_quest_wc3_hud.h's header comment).
 *
 * Deliberately does NOT attempt to resolve any button's `image_index` into
 * pixel data - logged once per process via LOG_ONCE, matching layer 5D's
 * "target mode has no location" precedent, since no ABI accessor exists
 * that could resolve it (see bz_quest_wc3_hud.h's header comment for the
 * full evidence trail; this is a scope decision, not a bug).
 */
bool bz_quest_wc3_capture_hud(bzQuestHudInput_t *out);

uint32_t bz_quest_wc3_render_clock_msec(void);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_CAPTURE_H */
