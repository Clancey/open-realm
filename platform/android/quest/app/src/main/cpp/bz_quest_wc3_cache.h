/*
 * bz_quest_wc3_cache.h - layer 5A: platform-independent keyed cache
 * bookkeeping for Vulkan GPU resources (model vertex/index buffers, texture
 * images/views/samplers/descriptor sets - see bz_quest_vk_wc3.h).
 *
 * Deliberately holds no VkBuffer/VkImage/VkDevice or any other Vulkan type:
 * resource creation/destruction is injected as a pair of function pointers
 * (`create`/`destroy`) rather than called directly, so
 * platform/android/quest/tests/test_bz_quest_wc3_cache.c can exercise every
 * hit/miss/eviction/shutdown decision on the host with fake create/destroy
 * callbacks that just count calls - no NDK/Vulkan/Android headers required,
 * mirroring bz_quest_pure.h's and bz_quest_wc3_render.h's rationale. The
 * *real* create/destroy callbacks (bz_quest_vk_wc3.c) do the actual
 * vkCreateBuffer/staging-upload/vkDestroyBuffer work; this module only
 * decides *when* to call them.
 *
 * Keyed by a plain identity string (the bridge asset identity - see
 * platform/bridge/bz_tabletop_assets.h's BZ_TTAsset_Identity() and this
 * module's `bzQuestWc3CacheKey_t`), which this ABI already makes a stable,
 * fully-distinguishing key for both models and images: team-color/glow
 * texture identities already encode the team color in their path (e.g.
 * "ReplaceableTextures\TeamColor\TeamColor02.blp" -
 * games/warcraft-3/visionos/wc3_tabletop_assets.c's
 * wc3_resolve_team_texture_identity()), and model geometry does not vary by
 * entity metadata (only the *scale/tint* derived from metadata varies,
 * which is not GPU-cached content - see bz_quest_wc3_render.h). No
 * additional "variant" key component is needed for this slice; the key
 * struct still carries an explicit (currently always BZ_QUEST_WC3_VARIANT_NONE)
 * variant field so a future layer that *does* need one (e.g. a baked tint
 * atlas) has a place to add it without changing every call site.
 */
#ifndef BZ_QUEST_WC3_CACHE_H
#define BZ_QUEST_WC3_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "bz_quest_wc3_render.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BZ_QUEST_WC3_VARIANT_NONE = 0,
    BZ_QUEST_WC3_CACHE_CAPACITY = BZ_QUEST_WC3_MAX_UNIQUE_MODELS_PER_FRAME,
};

typedef struct {
    char identity[BZ_QUEST_WC3_MAX_IDENTITY];
    uint32_t variant; /* reserved - see this file's header comment; always 0 today */
} bzQuestWc3CacheKey_t;

/* Creates the real resource for `key` and returns an opaque handle (non-NULL
 * on success). Returning NULL is a hard miss-creation failure (e.g. Vulkan
 * allocation failure, malformed asset) - the cache does not retry or cache
 * the failure; the caller's next acquire() attempts creation again. */
typedef void *(*bzQuestWc3CacheCreateFn)(const bzQuestWc3CacheKey_t *key, void *userdata);
/* Destroys a handle previously returned by `create` - called on eviction and
 * on bz_quest_wc3_cache_shutdown(), exactly once per successfully created
 * handle, never more than once (see bz_quest_wc3_cache_shutdown()'s
 * contract). */
typedef void (*bzQuestWc3CacheDestroyFn)(void *handle, void *userdata);

typedef struct {
    bzQuestWc3CacheKey_t key;
    void *handle;
    bool occupied;
} bzQuestWc3CacheSlot_t;

typedef struct {
    bzQuestWc3CacheSlot_t slots[BZ_QUEST_WC3_CACHE_CAPACITY];
    uint32_t occupiedCount;
    uint32_t capacity; /* <= BZ_QUEST_WC3_CACHE_CAPACITY - see bz_quest_wc3_cache_init() */
    /* FIFO eviction order: the slot index inserted least-recently-of-those-
     * still-occupied. A plain monotonic insertion counter per slot (not a
     * linked list) keeps this struct POD/easy to fake in tests. */
    uint64_t insertionCounter;
    uint64_t insertionOrder[BZ_QUEST_WC3_CACHE_CAPACITY];
    bzQuestWc3CacheCreateFn create;
    bzQuestWc3CacheDestroyFn destroy;
    void *userdata;
    uint64_t hits, misses, evictions, createFailures;
} bzQuestWc3Cache_t;

/*
 * Initializes an empty cache with the given `capacity` (must be in
 * [1, BZ_QUEST_WC3_CACHE_CAPACITY]) and injected create/destroy callbacks.
 * Returns false (leaving *cache unmodified) if capacity is out of range or
 * either callback is NULL - a cache with no way to create/destroy resources
 * is a caller bug, not something to silently no-op.
 */
bool bz_quest_wc3_cache_init(bzQuestWc3Cache_t *cache, uint32_t capacity,
                             bzQuestWc3CacheCreateFn create, bzQuestWc3CacheDestroyFn destroy,
                             void *userdata);

/*
 * Looks up `key`. On a hit, increments cache->hits and returns the existing
 * handle via *outHandle. On a miss, calls cache->create(key, userdata):
 *   - if it returns non-NULL, increments cache->misses, evicts the oldest
 *     entry first (calling cache->destroy() on it) if the cache is already
 *     at capacity, inserts the new entry, and returns the new handle.
 *   - if it returns NULL, increments cache->createFailures and returns
 *     false without inserting anything (see bzQuestWc3CacheCreateFn's
 *     contract above).
 * Returns false (leaving *outHandle unmodified) only on a create failure.
 */
bool bz_quest_wc3_cache_acquire(bzQuestWc3Cache_t *cache, const bzQuestWc3CacheKey_t *key,
                                void **outHandle);

/*
 * Destroys every occupied slot's handle (via cache->destroy()) and resets
 * the cache to empty. Safe to call on an already-empty or zero-initialized
 * (memset to 0) cache - a zeroed cache has occupiedCount 0, so no destroy
 * calls happen and this is a no-op other than clearing counters.
 */
void bz_quest_wc3_cache_shutdown(bzQuestWc3Cache_t *cache);

/*
 * bzQuestWc3EpochTracker_t: shared "did the map actually reload" detector for
 * every per-map GPU resource that must reset exactly once on a real map
 * change and never on a mere snapshot-generation bump within the same map -
 * the particle pool (bz_quest_wc3_particles_pool_reset()'s "no stale effects
 * across resets" contract) and the model/texture GPU caches above
 * (bz_quest_vk_wc3.c's reset_model_texture_caches()) both compare the same
 * authoritative bzQuestWc3CaptureFrame_t::mapEpoch this way; this type
 * exists so that comparison - previously duplicated inline in more than one
 * place - has exactly one implementation to get right.
 */
typedef struct {
    uint64_t epoch;
    bool have;
} bzQuestWc3EpochTracker_t;

/*
 * Returns true exactly when `epoch` differs from the epoch recorded by a
 * previous call (a real transition - reset your resource now) and false
 * otherwise, including the very first call on a zero-initialized tracker
 * (bootstrap: there is nothing to reset yet, the caller's own resource was
 * already freshly created for this baseline epoch). Always records `epoch`
 * as the new baseline before returning, whether or not the caller's own
 * reset action actually succeeds - matching
 * bz_quest_wc3_terrain_capture.c's existing s_lastTerrainKey precedent
 * (the "a new generation was detected" bookkeeping advances unconditionally;
 * a rare downstream Vulkan failure, e.g. vkDeviceWaitIdle, is logged and
 * left as-is rather than retried forever, since it is not expected to be
 * transient - see bz_quest_vk_wc3.c's reset_model_texture_caches() call
 * site).
 */
bool bz_quest_wc3_epoch_changed(bzQuestWc3EpochTracker_t *tracker, uint64_t epoch);

#ifdef __cplusplus
}
#endif

#endif /* BZ_QUEST_WC3_CACHE_H */
