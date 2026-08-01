/*
 * bz_quest_wc3_terrain_capture.h - layer 5B's one impure terrain ABI caller.
 *
 * Mirrors bz_quest_wc3_capture.h's split exactly: all host-testable terrain
 * math/mesh logic lives in bz_quest_wc3_terrain.c, while this file is only the
 * thin retain/copy/release bridge walk over bzTTTerrain_t/bzTTAsset_t. Like
 * bz_quest_wc3_capture.c and bz_quest_snapshot.c, it has no direct unit test:
 * every decision that benefits from fake coverage already lives in the pure
 * terrain module, so what remains here is a mechanically-obvious ABI-call
 * sequence with bounded static scratch buffers, reviewed by inspection rather
 * than reproduced in a fake bridge harness.
 */
#ifndef BZ_QUEST_WC3_TERRAIN_CAPTURE_H
#define BZ_QUEST_WC3_TERRAIN_CAPTURE_H

#include <stdint.h>

#include "bz_quest_wc3_terrain.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bzQuestWc3TerrainReadyFn)(const bzQuestWc3TerrainInput_t *terrain, void *userdata);
typedef void (*bzQuestWc3TerrainTextureReadyFn)(const char *identity, uint32_t width, uint32_t height,
                                                uint32_t rowBytes, const uint8_t *pixels,
                                                uint32_t dataBytes, void *userdata);

typedef struct {
    bzQuestWc3TerrainReadyFn onTerrainReady;
    void *terrainUserdata;
    bzQuestWc3TerrainTextureReadyFn onTextureReady;
    void *textureUserdata;
} bzQuestWc3TerrainCaptureCallbacks_t;

/*
 * Retains BZ_TTA_LatestTerrain() once, copies the latest generation into the
 * one static bzQuestWc3TerrainInput_t scratch buffer, and invokes
 * callbacks->onTerrainReady exactly once per NEW terrain generation. Returns
 * true when a latest terrain handle exists this frame (even if the generation
 * is unchanged and no callback work ran), false when no terrain is currently
 * published or its metadata could not even be read. The
 * generation key mirrors LiveTabletopTransport.swift:575-610's terrainKey
 * precedent: a stable composite string derived from the retained terrain handle
 * plus its dimensions/bounds, so same-generation frames skip the expensive
 * corner walk/mesh-input rebuild and the onTerrainReady callback entirely.
 *
 * Textures are handled differently on purpose: this generation's referenced
 * ground/cliff/water textures are re-registered/copied and fed to
 * callbacks->onTextureReady on EVERY call, including same-generation frames,
 * not only when the generation first changes. The Vulkan-side GPU upload path
 * bounds uploads to a small per-frame budget (see bz_quest_vk_wc3_terrain.c),
 * so a generation with more referenced textures than one frame's budget would
 * otherwise get exactly one shot at each texture and any texture rejected by
 * that budget would never be retried. Re-offering every call is cheap (the
 * Vulkan cache's cache_find() dedups an already-uploaded identity before this
 * module's decode work even runs... note: this module still decodes pixels
 * into the scratch buffer before the cache dedup check happens downstream, so
 * "cheap" means "one ABI registration + memcpy", not "free" - acceptable given
 * terrain texture counts are bounded and this mirrors the chunk upload path's
 * existing every-frame re-scan in upload_missing_chunks()) and guarantees the
 * bounded per-frame texture budget eventually drains across frames instead of
 * silently, permanently losing textures beyond the first frame's budget.
 * Safe with any callback field NULL: the corresponding work is simply omitted.
 */
bool bz_quest_wc3_terrain_capture(const bzQuestWc3TerrainCaptureCallbacks_t *callbacks);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_TERRAIN_CAPTURE_H */
