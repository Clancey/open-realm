/*
 * bz_quest_snapshot.h - thin, Quest-only real snapshot reader.
 *
 * Deliberately kept out of bz_quest_frame.h/.c (which stays link-clean of
 * platform/bridge/bz_tabletop_transport.c's heavy engine dependencies - see
 * that file's header comment): this translation unit is the one place that
 * actually calls BZ_TT_Latest()/BZ_TTSnapshot_*()/BZ_TTSnapshot_Release(),
 * so only it (and the real bz_quest_native .so link, which already needs
 * openwarcraft3-engine for the renderer/scene) pulls that dependency in -
 * not the host-testable pure frame-descriptor tests.
 */
#ifndef BZ_QUEST_SNAPSHOT_H
#define BZ_QUEST_SNAPSHOT_H

#include "bz_quest_frame.h"
#include "platform/tabletop/bridge/bz_tabletop_lifecycle.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Acquires BZ_TT_Latest() (if any has been published), copies only the
 * small diagnostic values bz_quest_frame_from_values() needs into *out, and
 * releases the retained snapshot before returning - on every branch,
 * including "no snapshot published yet" and "ABI mismatch", so no engine
 * pointer/handle ever outlives this one call (see bz_tabletop_transport.h's
 * retain/release contract). Also reads the lifecycle's own state/last-error
 * via BZ_TabletopGetState()/BZ_TabletopLastError(); lc may be NULL (e.g.
 * before bz_quest_bridge_start() has ever been called), in which case the
 * lifecycle fields report BZ_TABLETOP_STATE_IDLE/no-error, matching
 * BZ_TabletopGetState(NULL)'s own documented behavior.
 *
 * Must only be called from the Quest XR/render thread - the same call site
 * as bz_quest_renderer_frame() (see bz_quest_host.c's android_main loop).
 * BZ_TT_Latest() itself is safe from any thread, but this project pins the
 * call here anyway: a future input/gameplay layer will need to read the
 * *same* generation's entity/selection data the renderer just drew, so both
 * must observe one consistent snapshot from a single call site rather than
 * two independently-timed BZ_TT_Latest() calls that could race a
 * generation change between them.
 */
void bz_quest_snapshot_capture(const bzTabletopLifecycle_t *lc, bzQuestFrame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_SNAPSHOT_H */
