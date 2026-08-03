/*
 * test_bz_quest_wc3_cache.c - coverage for bz_quest_wc3_cache.c's pure
 * keyed cache bookkeeping (layer 5A), using fake create/destroy callbacks
 * (no Vulkan/ABI dependency - see bz_quest_wc3_cache.h's header comment).
 * Each case covers a normal path and its inverse (hit vs miss, eviction vs
 * no-eviction, shutdown-with-entries vs shutdown-of-empty-cache).
 *
 * Also covers bz_quest_wc3_epoch_changed() (PR #28's map-reload-detector
 * fix) and the exact shutdown()+init() map-reset PATTERN
 * bz_quest_vk_wc3.c's reset_model_texture_caches() uses in production -
 * proving the actual "stale GPU asset across map reload" defect this fixes
 * (a reused identity path silently keeping a previous map's content) and
 * its correction, against the real production cache module rather than a
 * reimplemented oracle - Vulkan is never touched, so this is exercised
 * entirely at the host level.
 */
#include <string.h>

#include "bz_quest_wc3_cache.h"
#include "test_framework.h"

/* Fake resource: just an incrementing id, so tests can tell distinct
 * created handles apart and verify destroy() is called on the right one. */
typedef struct {
    int id;
    bool destroyed;
} FakeResource_t;

typedef struct {
    FakeResource_t resources[32];
    int nextId;
    int createCalls;
    int destroyCalls;
    int lastDestroyedId;
    bool failNextCreate;
    /* Simultaneous-live-resource ceiling emulation - models a real bounded
     * Vulkan resource pool (e.g. bz_quest_vk_wc3.c's fixed-size texture
     * VkDescriptorPool). 0 means "unlimited" (the other tests in this file
     * don't care about a ceiling and leave this at its zeroed default).
     * When non-zero, fake_create() fails once `liveCount` would exceed
     * `ceiling` - this is exactly what reproduces bz_quest_vk_wc3.c's
     * original texture-descriptor-pool deadlock at the pure-cache level: a
     * create-before-evict cache (see bz_quest_wc3_cache_acquire()'s
     * documented order) whose real resource ceiling exactly equals its
     * logical `capacity` can never grow past that ceiling, because
     * create() for the (capacity+1)-th distinct key is always attempted -
     * and therefore always fails - before eviction ever gets a chance to
     * free a slot. See test_acquire_deadlocks_when_ceiling_matches_capacity_
     * with_no_spare_slot and test_acquire_recovers_when_ceiling_has_one_
     * spare_slot below. */
    int ceiling;
    int liveCount;
} FakeUserdata_t;

static void *fake_create(const bzQuestWc3CacheKey_t *key, void *userdata) {
    (void)key;
    FakeUserdata_t *u = (FakeUserdata_t *)userdata;
    u->createCalls++;
    if (u->failNextCreate) {
        u->failNextCreate = false;
        return NULL;
    }
    if (u->ceiling > 0 && u->liveCount >= u->ceiling) {
        /* Models an exhausted bounded resource pool (e.g. a fully-allocated
         * VkDescriptorPool) - see FakeUserdata_t's `ceiling` doc comment. */
        return NULL;
    }
    FakeResource_t *r = &u->resources[u->nextId];
    r->id = u->nextId;
    r->destroyed = false;
    u->nextId++;
    u->liveCount++;
    return r;
}

static void fake_destroy(void *handle, void *userdata) {
    FakeUserdata_t *u = (FakeUserdata_t *)userdata;
    FakeResource_t *r = (FakeResource_t *)handle;
    r->destroyed = true;
    u->destroyCalls++;
    u->lastDestroyedId = r->id;
    u->liveCount--;
}

static bzQuestWc3CacheKey_t make_key(const char *identity) {
    bzQuestWc3CacheKey_t key;
    memset(&key, 0, sizeof(key));
    strncpy(key.identity, identity, sizeof(key.identity) - 1);
    return key;
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_cache_init                                             */
/* ------------------------------------------------------------------ */

static void test_init_rejects_zero_capacity(void) {
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    ASSERT(!bz_quest_wc3_cache_init(&cache, 0, fake_create, fake_destroy, &u));
}

static void test_init_rejects_missing_callbacks(void) {
    bzQuestWc3Cache_t cache;
    ASSERT(!bz_quest_wc3_cache_init(&cache, 4, NULL, fake_destroy, NULL));
    ASSERT(!bz_quest_wc3_cache_init(&cache, 4, fake_create, NULL, NULL));
}

static void test_init_accepts_valid_capacity(void) {
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    ASSERT(bz_quest_wc3_cache_init(&cache, 4, fake_create, fake_destroy, &u));
    ASSERT_EQ_INT(cache.capacity, 4);
    ASSERT_EQ_INT(cache.occupiedCount, 0);
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_cache_acquire - miss then hit                          */
/* ------------------------------------------------------------------ */

static void test_acquire_first_call_is_a_miss(void) {
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    bz_quest_wc3_cache_init(&cache, 4, fake_create, fake_destroy, &u);

    bzQuestWc3CacheKey_t key = make_key("units/human/footman/footman.mdx");
    void *handle = NULL;
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &key, &handle));
    ASSERT_NOT_NULL(handle);
    ASSERT_EQ_INT(cache.misses, 1);
    ASSERT_EQ_INT(cache.hits, 0);
    ASSERT_EQ_INT(u.createCalls, 1);
}

static void test_acquire_second_call_same_key_is_a_hit(void) {
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    bz_quest_wc3_cache_init(&cache, 4, fake_create, fake_destroy, &u);

    bzQuestWc3CacheKey_t key = make_key("units/human/footman/footman.mdx");
    void *first = NULL, *second = NULL;
    bz_quest_wc3_cache_acquire(&cache, &key, &first);
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &key, &second));

    ASSERT(first == second);
    ASSERT_EQ_INT(cache.hits, 1);
    ASSERT_EQ_INT(cache.misses, 1);
    ASSERT_EQ_INT(u.createCalls, 1); /* create() must not be called twice for one key */
}

static void test_acquire_different_keys_are_independent_misses(void) {
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    bz_quest_wc3_cache_init(&cache, 4, fake_create, fake_destroy, &u);

    bzQuestWc3CacheKey_t keyA = make_key("units/human/footman/footman.mdx");
    bzQuestWc3CacheKey_t keyB = make_key("units/human/knight/knight.mdx");
    void *handleA = NULL, *handleB = NULL;
    bz_quest_wc3_cache_acquire(&cache, &keyA, &handleA);
    bz_quest_wc3_cache_acquire(&cache, &keyB, &handleB);

    ASSERT(handleA != handleB);
    ASSERT_EQ_INT(cache.misses, 2);
    ASSERT_EQ_INT(cache.occupiedCount, 2);
}

static void test_acquire_create_failure_reports_false_without_inserting(void) {
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    bz_quest_wc3_cache_init(&cache, 4, fake_create, fake_destroy, &u);
    u.failNextCreate = true;

    bzQuestWc3CacheKey_t key = make_key("units/human/footman/footman.mdx");
    void *handle = (void *)0x1; /* poison to prove it's left unmodified on failure path intent */
    ASSERT(!bz_quest_wc3_cache_acquire(&cache, &key, &handle));
    ASSERT_EQ_INT(cache.createFailures, 1);
    ASSERT_EQ_INT(cache.occupiedCount, 0);

    /* A retry with create succeeding this time must work normally - a
     * failed creation must not be cached as a permanent miss. */
    void *retry = NULL;
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &key, &retry));
    ASSERT_NOT_NULL(retry);
    ASSERT_EQ_INT(cache.occupiedCount, 1);
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_cache_acquire - eviction at capacity                   */
/* ------------------------------------------------------------------ */

static void test_acquire_evicts_oldest_entry_at_capacity(void) {
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    bz_quest_wc3_cache_init(&cache, 2, fake_create, fake_destroy, &u);

    bzQuestWc3CacheKey_t keyA = make_key("a.mdx");
    bzQuestWc3CacheKey_t keyB = make_key("b.mdx");
    bzQuestWc3CacheKey_t keyC = make_key("c.mdx");
    void *handleA = NULL, *handleB = NULL, *handleC = NULL;
    bz_quest_wc3_cache_acquire(&cache, &keyA, &handleA); /* fills slot 1/2 */
    bz_quest_wc3_cache_acquire(&cache, &keyB, &handleB); /* fills slot 2/2 */
    ASSERT_EQ_INT(cache.occupiedCount, 2);
    ASSERT_EQ_INT(u.destroyCalls, 0);

    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyC, &handleC)); /* forces eviction of A */
    ASSERT_EQ_INT(cache.occupiedCount, 2);
    ASSERT_EQ_INT(cache.evictions, 1);
    ASSERT_EQ_INT(u.destroyCalls, 1);
    ASSERT_EQ_INT(u.lastDestroyedId, ((FakeResource_t *)handleA)->id);
    ASSERT(((FakeResource_t *)handleA)->destroyed);
    ASSERT(!((FakeResource_t *)handleB)->destroyed);

    /* B (not evicted) must still be a hit; A (evicted) must be a fresh miss. */
    void *handleBAgain = NULL;
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyB, &handleBAgain));
    ASSERT(handleBAgain == handleB);
    ASSERT_EQ_INT(cache.hits, 1);
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_cache_acquire - simultaneous-live-resource ceiling      */
/* (models the real bounded-Vulkan-pool capacity mismatch that caused  */
/* bz_quest_vk_wc3.c's original texture-descriptor-pool deadlock)      */
/* ------------------------------------------------------------------ */

static void test_acquire_deadlocks_when_ceiling_matches_capacity_with_no_spare_slot(void) {
    /* Reproduces bz_quest_vk_wc3.c's original bug at the pure-cache level:
     * a real resource pool sized to exactly `capacity` live resources (no
     * spare slot - the ORIGINAL, broken descriptor-pool sizing) means the
     * (capacity+1)-th distinct key's create() call - which always happens
     * BEFORE eviction, per bz_quest_wc3_cache_acquire()'s documented order
     * - always fails, so eviction never runs and the cache is permanently
     * pinned at `capacity` entries. Retrying the same failing key forever
     * reproduces the identical failure - a one-way deadlock, not a
     * transient miss. */
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    u.ceiling = 2; /* == capacity: the unpatched, broken sizing */
    bz_quest_wc3_cache_init(&cache, 2, fake_create, fake_destroy, &u);

    bzQuestWc3CacheKey_t keyA = make_key("a.mdx");
    bzQuestWc3CacheKey_t keyB = make_key("b.mdx");
    bzQuestWc3CacheKey_t keyC = make_key("c.mdx");
    void *handleA = NULL, *handleB = NULL, *handleC = NULL;
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyA, &handleA));
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyB, &handleB));
    ASSERT_EQ_INT(cache.occupiedCount, 2);
    ASSERT_EQ_INT(u.liveCount, 2);

    /* The 3rd distinct key's create() attempt fails - the ceiling is
     * already saturated by A and B, and eviction has not run yet. */
    ASSERT(!bz_quest_wc3_cache_acquire(&cache, &keyC, &handleC));
    ASSERT_EQ_INT(cache.createFailures, 1);
    ASSERT_EQ_INT(cache.evictions, 0); /* eviction never even runs - this IS the deadlock */
    ASSERT_EQ_INT(cache.occupiedCount, 2); /* still pinned at the original 2 entries */
    ASSERT(!((FakeResource_t *)handleA)->destroyed); /* A/B survive untouched (transactional) */
    ASSERT(!((FakeResource_t *)handleB)->destroyed);

    /* Retrying the identical key reproduces the identical failure forever
     * - nothing ever recovers on its own without a sizing fix. */
    ASSERT(!bz_quest_wc3_cache_acquire(&cache, &keyC, &handleC));
    ASSERT_EQ_INT(cache.createFailures, 2);
    ASSERT_EQ_INT(cache.evictions, 0);
    ASSERT_EQ_INT(cache.occupiedCount, 2);
}

static void test_acquire_recovers_when_ceiling_has_one_spare_slot(void) {
    /* Proves the fix: a real resource pool with ONE spare slot beyond
     * `capacity` (bz_quest_vk_wc3.c's corrected
     * BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY = capacity + 1
     * sizing) lets create() for the (capacity+1)-th distinct key succeed,
     * since only `capacity` resources are simultaneously live at that
     * instant; eviction then runs immediately afterward and frees the
     * oldest, dropping liveCount back to `capacity` before the next
     * acquire(). A single spare slot is therefore sufficient no matter how
     * many further distinct keys are requested - liveCount never exceeds
     * capacity+1 even under sustained churn well beyond capacity. */
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    u.ceiling = 3; /* == capacity + 1: the corrected bz_quest_vk_wc3.c sizing */
    bz_quest_wc3_cache_init(&cache, 2, fake_create, fake_destroy, &u);

    int i;
    for (i = 0; i < 10; i++) {
        char identity[32];
        bzQuestWc3CacheKey_t key;
        void *handle = NULL;
        identity[0] = 'k';
        identity[1] = (char)('0' + (i / 10));
        identity[2] = (char)('0' + (i % 10));
        identity[3] = '\0';
        key = make_key(identity);
        ASSERT(bz_quest_wc3_cache_acquire(&cache, &key, &handle));
        ASSERT_NOT_NULL(handle);
        /* Never more than capacity+1 simultaneously live, matching the
         * real descriptor pool's hard ceiling. */
        ASSERT(u.liveCount <= 3);
    }
    ASSERT_EQ_INT(cache.createFailures, 0); /* never a single failure across 10 distinct keys */
    ASSERT_EQ_INT(cache.occupiedCount, 2);  /* still bounded at capacity */
    ASSERT(cache.evictions >= 8);           /* 10 inserts - 2 that fit without eviction */
}

static void test_acquire_recovers_after_eviction_then_create_failure_then_retry(void) {
    /* Proves no descriptor-set/resource leak and full recovery across a
     * eviction -> create-failure -> retry sequence: once an entry is
     * legitimately evicted (destroy() called), a subsequent create()
     * failure for a *different* new key must not corrupt the cache or
     * leak the evicted resource, and a later retry of that same key must
     * still succeed normally. */
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    bz_quest_wc3_cache_init(&cache, 2, fake_create, fake_destroy, &u);

    bzQuestWc3CacheKey_t keyA = make_key("a.mdx");
    bzQuestWc3CacheKey_t keyB = make_key("b.mdx");
    bzQuestWc3CacheKey_t keyC = make_key("c.mdx");
    void *handleA = NULL, *handleB = NULL, *handleC = NULL;
    bz_quest_wc3_cache_acquire(&cache, &keyA, &handleA);
    bz_quest_wc3_cache_acquire(&cache, &keyB, &handleB); /* cache full: {A, B} */

    /* C evicts A (oldest). */
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyC, &handleC));
    ASSERT_EQ_INT(cache.evictions, 1);
    ASSERT(((FakeResource_t *)handleA)->destroyed);
    ASSERT_EQ_INT(u.liveCount, 2); /* B, C - A's resource is gone, no leak */

    /* A new distinct key D's create() now fails outright (e.g. simulating
     * a malformed asset, unrelated to capacity) - must not evict/corrupt
     * B or C, and must not leak anything. */
    bzQuestWc3CacheKey_t keyD = make_key("d.mdx");
    void *handleD = (void *)0x1;
    u.failNextCreate = true;
    ASSERT(!bz_quest_wc3_cache_acquire(&cache, &keyD, &handleD));
    ASSERT_EQ_INT(cache.createFailures, 1);
    ASSERT_EQ_INT(cache.occupiedCount, 2); /* still exactly {B, C} */
    ASSERT_EQ_INT(u.liveCount, 2);         /* no leak from the failed attempt */
    ASSERT(!((FakeResource_t *)handleB)->destroyed);
    ASSERT(!((FakeResource_t *)handleC)->destroyed);

    /* Retrying D now (create succeeding) must work normally, evicting B
     * (the now-oldest of B/C). */
    void *handleDRetry = NULL;
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &keyD, &handleDRetry));
    ASSERT_NOT_NULL(handleDRetry);
    ASSERT_EQ_INT(cache.evictions, 2);
    ASSERT(((FakeResource_t *)handleB)->destroyed);
    ASSERT(!((FakeResource_t *)handleC)->destroyed);
    ASSERT_EQ_INT(u.liveCount, 2); /* C, D */
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_cache_shutdown                                         */
/* ------------------------------------------------------------------ */

static void test_shutdown_destroys_every_entry(void) {
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    bz_quest_wc3_cache_init(&cache, 4, fake_create, fake_destroy, &u);

    bzQuestWc3CacheKey_t keyA = make_key("a.mdx");
    bzQuestWc3CacheKey_t keyB = make_key("b.mdx");
    void *handleA = NULL, *handleB = NULL;
    bz_quest_wc3_cache_acquire(&cache, &keyA, &handleA);
    bz_quest_wc3_cache_acquire(&cache, &keyB, &handleB);

    bz_quest_wc3_cache_shutdown(&cache);

    ASSERT_EQ_INT(u.destroyCalls, 2);
    ASSERT(((FakeResource_t *)handleA)->destroyed);
    ASSERT(((FakeResource_t *)handleB)->destroyed);
    ASSERT_EQ_INT(cache.occupiedCount, 0);
}

static void test_shutdown_of_empty_cache_calls_nothing(void) {
    bzQuestWc3Cache_t cache;
    FakeUserdata_t u;
    memset(&u, 0, sizeof(u));
    bz_quest_wc3_cache_init(&cache, 4, fake_create, fake_destroy, &u);

    bz_quest_wc3_cache_shutdown(&cache);
    ASSERT_EQ_INT(u.destroyCalls, 0);
}

static void test_shutdown_of_zero_initialized_cache_is_safe(void) {
    /* Mirrors bz_quest_wc3_cache.h's documented contract for a plain
     * memset(&cache, 0, sizeof(cache)) cache that was never init()'d. */
    bzQuestWc3Cache_t cache;
    memset(&cache, 0, sizeof(cache));
    bz_quest_wc3_cache_shutdown(&cache); /* must not crash or call a NULL destroy() */
    ASSERT_EQ_INT(cache.occupiedCount, 0);
}

/* ------------------------------------------------------------------ */
/* bz_quest_wc3_epoch_changed - the shared map-reload detector used by   */
/* both the particle pool (bz_quest_vk_wc3.c's particlePoolEpoch) and    */
/* the model/texture GPU cache reset this fixes (PR #28,                */
/* modelTextureCacheEpoch) - see bz_quest_wc3_cache.h's doc comment.     */
/* ------------------------------------------------------------------ */

static void test_epoch_changed_first_call_never_reports_change(void) {
    /* "Zero/first epoch" case: a zero-initialized tracker's very first
     * check must never report a change, no matter what epoch value is
     * passed (including 0 itself - epoch value 0 is not a special
     * "unset" sentinel; `have` is the actual bootstrap flag) - the
     * caller's own resource was already freshly created for this
     * baseline, there is nothing to reset yet. */
    bzQuestWc3EpochTracker_t tracker;
    memset(&tracker, 0, sizeof(tracker));
    ASSERT(!bz_quest_wc3_epoch_changed(&tracker, 0));
    ASSERT(tracker.have);
    ASSERT_EQ_INT((int)tracker.epoch, 0);

    bzQuestWc3EpochTracker_t tracker2;
    memset(&tracker2, 0, sizeof(tracker2));
    ASSERT(!bz_quest_wc3_epoch_changed(&tracker2, 42));
    ASSERT(tracker2.have);
    ASSERT_EQ_INT((int)tracker2.epoch, 42);
}

static void test_epoch_changed_same_epoch_repeated_never_reports_change(void) {
    /* "Inverse/no unnecessary flush": once baselined, repeating the exact
     * same epoch any number of times must never report a change - a
     * caller resetting on every "true" must never do so merely because
     * capture_and_upload() runs every frame within the same map. */
    bzQuestWc3EpochTracker_t tracker;
    memset(&tracker, 0, sizeof(tracker));
    bz_quest_wc3_epoch_changed(&tracker, 7); /* bootstrap */
    for (int i = 0; i < 5; i++) ASSERT(!bz_quest_wc3_epoch_changed(&tracker, 7));
}

static void test_epoch_changed_reports_true_exactly_once_per_transition(void) {
    bzQuestWc3EpochTracker_t tracker;
    memset(&tracker, 0, sizeof(tracker));
    bz_quest_wc3_epoch_changed(&tracker, 100); /* bootstrap */
    ASSERT(bz_quest_wc3_epoch_changed(&tracker, 200));  /* real transition: true */
    ASSERT(!bz_quest_wc3_epoch_changed(&tracker, 200)); /* same epoch again: false */
    ASSERT(!bz_quest_wc3_epoch_changed(&tracker, 200)); /* still false */
}

static void test_epoch_changed_multiple_transitions_each_reported_once(void) {
    /* "Multiple changes": a sequence of distinct map reloads (A -> B -> C)
     * must each be reported exactly once, in order, never merged/skipped/
     * double-reported. */
    bzQuestWc3EpochTracker_t tracker;
    memset(&tracker, 0, sizeof(tracker));
    ASSERT(!bz_quest_wc3_epoch_changed(&tracker, 1)); /* bootstrap: A */
    ASSERT(bz_quest_wc3_epoch_changed(&tracker, 2));  /* A -> B */
    ASSERT(!bz_quest_wc3_epoch_changed(&tracker, 2));
    ASSERT(bz_quest_wc3_epoch_changed(&tracker, 3));  /* B -> C */
    ASSERT(!bz_quest_wc3_epoch_changed(&tracker, 3));
    ASSERT(bz_quest_wc3_epoch_changed(&tracker, 1));  /* C -> A again (a distinct 3rd map that
                                                        * happens to reuse epoch 1's numeric
                                                        * value is still a real transition). */
}

/* ------------------------------------------------------------------ */
/* Map-reload cache reset pattern - the exact shutdown()+init() sequence */
/* bz_quest_vk_wc3.c's reset_model_texture_caches() uses (PR #28), proven */
/* here against the pure cache module with fake create/destroy so the   */
/* "stale GPU asset across map reload" defect and its fix are provable  */
/* without Vulkan/NDK - see this file's own header comment.             */
/* ------------------------------------------------------------------ */

/* Simulates a map's own content for a reused identity string (e.g. two
 * different custom maps both importing "Textures\Custom.blp") - fake_create
 * bakes `contentTag` into the returned resource so a test can prove which
 * map's content a cache slot actually holds. */
typedef struct {
    FakeUserdata_t base;
    int contentTag;
} TaggedUserdata_t;

typedef struct {
    int id;
    int contentTag;
    bool destroyed;
} TaggedResource_t;

static TaggedResource_t s_taggedResources[32];

static void *tagged_create(const bzQuestWc3CacheKey_t *key, void *userdata) {
    (void)key;
    TaggedUserdata_t *u = (TaggedUserdata_t *)userdata;
    u->base.createCalls++;
    TaggedResource_t *r = &s_taggedResources[u->base.nextId];
    r->id = u->base.nextId;
    r->contentTag = u->contentTag;
    r->destroyed = false;
    u->base.nextId++;
    return r;
}

static void tagged_destroy(void *handle, void *userdata) {
    TaggedUserdata_t *u = (TaggedUserdata_t *)userdata;
    TaggedResource_t *r = (TaggedResource_t *)handle;
    r->destroyed = true;
    u->base.destroyCalls++;
}

static void test_map_reset_pattern_same_epoch_cache_hit_stays_resident(void) {
    /* "Same epoch cache hit stays resident": repeated acquire() calls for
     * the identical identity, with NO epoch transition in between, must
     * never re-create - the established, unaffected-by-this-fix hit path. */
    bzQuestWc3Cache_t cache;
    TaggedUserdata_t u;
    memset(&u, 0, sizeof(u));
    bzQuestWc3EpochTracker_t epochTracker;
    memset(&epochTracker, 0, sizeof(epochTracker));
    bz_quest_wc3_cache_init(&cache, 4, tagged_create, tagged_destroy, &u);
    bz_quest_wc3_epoch_changed(&epochTracker, 1); /* bootstrap */

    bzQuestWc3CacheKey_t key = make_key("Textures\\Custom.blp");
    void *first = NULL, *second = NULL, *third = NULL;
    bz_quest_wc3_cache_acquire(&cache, &key, &first);
    for (int frame = 0; frame < 10; frame++) {
        ASSERT(!bz_quest_wc3_epoch_changed(&epochTracker, 1)); /* same map every frame */
    }
    bz_quest_wc3_cache_acquire(&cache, &key, &second);
    bz_quest_wc3_cache_acquire(&cache, &key, &third);

    ASSERT(first == second && second == third);
    ASSERT_EQ_INT(u.base.createCalls, 1); /* never re-created */
}

static void test_map_reset_pattern_epoch_change_destroys_and_recreates_same_path_with_new_content(void) {
    /* The core PR #28 regression this fix closes: map A and map B both
     * import the identical path "Textures\Custom.blp" with DIFFERENT
     * content. Without a map-epoch-gated reset, map B's capture would hit
     * the stale cache entry and keep showing map A's GPU asset. */
    bzQuestWc3Cache_t cache;
    TaggedUserdata_t u;
    memset(&u, 0, sizeof(u));
    u.contentTag = 111; /* map A's content */
    bzQuestWc3EpochTracker_t epochTracker;
    memset(&epochTracker, 0, sizeof(epochTracker));
    bz_quest_wc3_cache_init(&cache, 4, tagged_create, tagged_destroy, &u);
    bz_quest_wc3_epoch_changed(&epochTracker, /*mapA*/ 1); /* bootstrap on map A */

    bzQuestWc3CacheKey_t key = make_key("Textures\\Custom.blp");
    void *onMapA = NULL;
    bz_quest_wc3_cache_acquire(&cache, &key, &onMapA);
    ASSERT_EQ_INT(((TaggedResource_t *)onMapA)->contentTag, 111);
    ASSERT_EQ_INT(u.base.createCalls, 1);

    /* Map B loads - a real epoch transition - reproducing
     * reset_model_texture_caches()'s exact shutdown()+init() sequence. */
    ASSERT(bz_quest_wc3_epoch_changed(&epochTracker, /*mapB*/ 2));
    bz_quest_wc3_cache_shutdown(&cache);
    ASSERT(((TaggedResource_t *)onMapA)->destroyed); /* map A's GPU asset is actually freed */
    u.contentTag = 222; /* map B's own, different content at the SAME path */
    bz_quest_wc3_cache_init(&cache, 4, tagged_create, tagged_destroy, &u);

    void *onMapB = NULL;
    ASSERT(bz_quest_wc3_cache_acquire(&cache, &key, &onMapB)); /* same identity string as map A */
    ASSERT_EQ_INT(((TaggedResource_t *)onMapB)->contentTag, 222); /* map B's content, not map A's */
    ASSERT_EQ_INT(u.base.createCalls, 2); /* a genuine fresh create, not a stale hit */
    ASSERT_EQ_INT(cache.hits, 0);         /* never counted as a hit against the old entry */
}

static void test_map_reset_pattern_multiple_epoch_changes_each_reset_exactly_once(void) {
    /* "Multiple changes": three maps in a row (A -> B -> C), each must
     * evict/recreate exactly once - never accumulate, never skip. */
    bzQuestWc3Cache_t cache;
    TaggedUserdata_t u;
    memset(&u, 0, sizeof(u));
    u.contentTag = 1;
    bzQuestWc3EpochTracker_t epochTracker;
    memset(&epochTracker, 0, sizeof(epochTracker));
    bz_quest_wc3_cache_init(&cache, 4, tagged_create, tagged_destroy, &u);
    bz_quest_wc3_epoch_changed(&epochTracker, 1); /* bootstrap: map A */

    bzQuestWc3CacheKey_t key = make_key("units\\human\\footman\\footman.mdx");
    void *handle = NULL;
    bz_quest_wc3_cache_acquire(&cache, &key, &handle);

    uint64_t epochs[3] = {2, 3, 4}; /* map B, C, D */
    for (int i = 0; i < 3; i++) {
        ASSERT(bz_quest_wc3_epoch_changed(&epochTracker, epochs[i]));
        bz_quest_wc3_cache_shutdown(&cache);
        u.contentTag = 100 + i;
        bz_quest_wc3_cache_init(&cache, 4, tagged_create, tagged_destroy, &u);
        bz_quest_wc3_cache_acquire(&cache, &key, &handle);
        ASSERT_EQ_INT(((TaggedResource_t *)handle)->contentTag, 100 + i);
    }
    ASSERT_EQ_INT(u.base.createCalls, 4); /* map A + 3 resets, never more */
}

static void test_map_reset_pattern_model_and_texture_caches_reset_symmetrically(void) {
    /* "Texture+model symmetry": bz_quest_vk_wc3.c's
     * reset_model_texture_caches() resets BOTH modelCache and textureCache
     * from the SAME epoch transition, in the same call - modeled here as
     * two independent bzQuestWc3Cache_t instances driven by one shared
     * epoch tracker, exactly like bzQuestVkWc3_t's real topology. */
    bzQuestWc3Cache_t modelCache, textureCache;
    TaggedUserdata_t modelUserdata, textureUserdata;
    memset(&modelUserdata, 0, sizeof(modelUserdata));
    memset(&textureUserdata, 0, sizeof(textureUserdata));
    modelUserdata.contentTag = 1;
    textureUserdata.contentTag = 1;
    bzQuestWc3EpochTracker_t epochTracker;
    memset(&epochTracker, 0, sizeof(epochTracker));
    bz_quest_wc3_cache_init(&modelCache, 4, tagged_create, tagged_destroy, &modelUserdata);
    bz_quest_wc3_cache_init(&textureCache, 4, tagged_create, tagged_destroy, &textureUserdata);
    bz_quest_wc3_epoch_changed(&epochTracker, 1); /* bootstrap */

    bzQuestWc3CacheKey_t modelKey = make_key("units\\human\\footman\\footman.mdx");
    bzQuestWc3CacheKey_t textureKey = make_key("units\\human\\footman\\footman.blp");
    void *modelHandle = NULL, *textureHandle = NULL;
    bz_quest_wc3_cache_acquire(&modelCache, &modelKey, &modelHandle);
    bz_quest_wc3_cache_acquire(&textureCache, &textureKey, &textureHandle);

    ASSERT(bz_quest_wc3_epoch_changed(&epochTracker, 2)); /* map reload */
    bz_quest_wc3_cache_shutdown(&modelCache);
    bz_quest_wc3_cache_shutdown(&textureCache);
    ASSERT(((TaggedResource_t *)modelHandle)->destroyed);   /* both destroyed ... */
    ASSERT(((TaggedResource_t *)textureHandle)->destroyed); /* ... not just one */
    modelUserdata.contentTag = 2;
    textureUserdata.contentTag = 2;
    bz_quest_wc3_cache_init(&modelCache, 4, tagged_create, tagged_destroy, &modelUserdata);
    bz_quest_wc3_cache_init(&textureCache, 4, tagged_create, tagged_destroy, &textureUserdata);

    void *modelHandle2 = NULL, *textureHandle2 = NULL;
    bz_quest_wc3_cache_acquire(&modelCache, &modelKey, &modelHandle2);
    bz_quest_wc3_cache_acquire(&textureCache, &textureKey, &textureHandle2);
    ASSERT_EQ_INT(((TaggedResource_t *)modelHandle2)->contentTag, 2);
    ASSERT_EQ_INT(((TaggedResource_t *)textureHandle2)->contentTag, 2);
}

void run_bz_quest_wc3_cache_tests(void) {
    RUN_TEST(test_init_rejects_zero_capacity);
    RUN_TEST(test_init_rejects_missing_callbacks);
    RUN_TEST(test_init_accepts_valid_capacity);
    RUN_TEST(test_acquire_first_call_is_a_miss);
    RUN_TEST(test_acquire_second_call_same_key_is_a_hit);
    RUN_TEST(test_acquire_different_keys_are_independent_misses);
    RUN_TEST(test_acquire_create_failure_reports_false_without_inserting);
    RUN_TEST(test_acquire_evicts_oldest_entry_at_capacity);
    RUN_TEST(test_acquire_deadlocks_when_ceiling_matches_capacity_with_no_spare_slot);
    RUN_TEST(test_acquire_recovers_when_ceiling_has_one_spare_slot);
    RUN_TEST(test_acquire_recovers_after_eviction_then_create_failure_then_retry);
    RUN_TEST(test_shutdown_destroys_every_entry);
    RUN_TEST(test_shutdown_of_empty_cache_calls_nothing);
    RUN_TEST(test_shutdown_of_zero_initialized_cache_is_safe);
    RUN_TEST(test_epoch_changed_first_call_never_reports_change);
    RUN_TEST(test_epoch_changed_same_epoch_repeated_never_reports_change);
    RUN_TEST(test_epoch_changed_reports_true_exactly_once_per_transition);
    RUN_TEST(test_epoch_changed_multiple_transitions_each_reported_once);
    RUN_TEST(test_map_reset_pattern_same_epoch_cache_hit_stays_resident);
    RUN_TEST(test_map_reset_pattern_epoch_change_destroys_and_recreates_same_path_with_new_content);
    RUN_TEST(test_map_reset_pattern_multiple_epoch_changes_each_reset_exactly_once);
    RUN_TEST(test_map_reset_pattern_model_and_texture_caches_reset_symmetrically);
}
