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
/* Returns true once the texture is fully consumed (uploaded, or already
 * cached) so the capture module can stop copying/re-offering it; returns
 * false to mean "not consumed yet" (deferred by a per-frame budget, or a
 * transient failure) so the capture module keeps it pending and retries on a
 * later call. */
typedef bool (*bzQuestWc3TerrainTextureReadyFn)(const char *identity, uint32_t width, uint32_t height,
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
 * ground/cliff/water textures are tracked with a per-texture "pending" flag
 * (all set on a NEW generation, alongside the onTerrainReady rebuild above).
 * On every call (including same-generation frames), only textures still
 * marked pending are re-registered/decoded/copied and fed to
 * callbacks->onTextureReady; a texture whose callback returns true (meaning
 * the Vulkan side actually consumed it - uploaded, or already cached) is
 * cleared and skipped - zero registration/copy work - on every later call
 * until the next generation reset. A texture whose callback returns false
 * (deferred by the Vulkan side's small per-frame upload budget, see
 * bz_quest_vk_wc3_terrain.c, or a transient registration/copy failure) stays
 * pending and is retried on the next call. This guarantees the bounded
 * per-frame texture budget eventually drains across frames without
 * permanently losing textures beyond the first frame's budget, while NOT
 * re-registering/re-copying pixels for textures that already finished
 * uploading - unlike re-offering every referenced texture unconditionally on
 * every frame, which would re-decode potentially many large images per frame
 * on mobile hardware even once nothing is left to do.
 * Safe with any callback field NULL: the corresponding work is simply omitted
 * (and a texture whose callback never runs, because onTextureReady is NULL,
 * simply never clears pending - harmless, since there is nothing to upload).
 */
bool bz_quest_wc3_terrain_capture(const bzQuestWc3TerrainCaptureCallbacks_t *callbacks);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_TERRAIN_CAPTURE_H */
