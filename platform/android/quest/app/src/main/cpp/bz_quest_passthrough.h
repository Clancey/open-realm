/*
 * bz_quest_passthrough.h - XR_FB_passthrough lifecycle (create/start/pause/
 * destroy layer + object) for the layer-3 renderer. Kept as its own module,
 * separate from bz_quest_xr.c's instance/session ownership, because
 * passthrough has its own create/start/pause/destroy state machine layered
 * on top of (not replacing) the OpenXR session state machine - see the
 * OpenXR FB_passthrough extension spec's "Lifecycle" section:
 * https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#XR_FB_passthrough
 *
 * bz_quest_xr_get_system() already hard-fails startup if the runtime lacks
 * XR_PASSTHROUGH_CAPABILITY_BIT_FB (see bz_quest_xr.c); this module performs
 * its own second capability check before creating the passthrough object,
 * per the task's explicit requirement that the passthrough object itself
 * checks capability rather than relying solely on the earlier blend-mode/
 * system check.
 */
#ifndef BZ_QUEST_PASSTHROUGH_H
#define BZ_QUEST_PASSTHROUGH_H

#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_xr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bzQuestPassthrough_s {
    XrPassthroughFB passthrough;
    XrPassthroughLayerFB layer;
    bool started; /* true between xrPassthroughStartFB and xrPassthroughPauseFB/destroy */
} bzQuestPassthrough_t;

/*
 * Re-checks `xr->passthroughCapabilities` against
 * XR_PASSTHROUGH_CAPABILITY_BIT_FB (hard failure, not a fallback - see this
 * header's top comment), then: xrCreatePassthroughFB (flags=0, i.e. not
 * started at creation - this module calls xrPassthroughStartFB explicitly
 * once via bz_quest_passthrough_start(), matching the spec's recommended
 * explicit-start pattern over XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB)
 * and xrCreatePassthroughLayerFB with
 * XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB (full-environment
 * reconstruction - the only purpose relevant to this prototype; no
 * per-surface/projected passthrough is implemented).
 */
bool bz_quest_passthrough_create(const bzQuestXr_t *xr, bzQuestPassthrough_t *pt);

/* xrPassthroughStartFB(pt->passthrough) - must run after
 * bz_quest_passthrough_create() and before the layer is referenced from any
 * composition layer list (an unstarted passthrough object composites
 * nothing, per the spec). */
bool bz_quest_passthrough_start(const bzQuestXr_t *xr, bzQuestPassthrough_t *pt);

/* xrPassthroughPauseFB(pt->passthrough) - the Android-lifecycle-driven
 * counterpart to bz_quest_passthrough_start(), called from
 * APP_CMD_PAUSE/APP_CMD_STOP handling (see bz_quest_host.c) so passthrough
 * camera access is released while the app isn't in the foreground. */
bool bz_quest_passthrough_pause(const bzQuestXr_t *xr, bzQuestPassthrough_t *pt);

/*
 * Fills `outLayer` with an XrCompositionLayerPassthroughFB referencing
 * pt->layer, using XR_REFERENCE_SPACE_TYPE_LOCAL semantics (full-screen
 * environment passthrough isn't anchored to a specific XrSpace the way a
 * projection layer is, but the struct still requires one - `space` is
 * accepted from the caller so it can pass the same appSpace used for the
 * projection layer). Returns false only if pt->layer is
 * XR_NULL_HANDLE (not yet created) - callers must call
 * bz_quest_passthrough_create() first, this never silently skips the
 * layer.
 */
bool bz_quest_passthrough_build_layer(const bzQuestPassthrough_t *pt, XrSpace space,
                                      XrCompositionLayerPassthroughFB *outLayer);

/* Reverse-order teardown: pauses if still started, destroys the layer, then
 * the passthrough object. Safe on a partially-initialized pt (every handle
 * checked against XR_NULL_HANDLE). */
void bz_quest_passthrough_destroy(const bzQuestXr_t *xr, bzQuestPassthrough_t *pt);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_PASSTHROUGH_H */
