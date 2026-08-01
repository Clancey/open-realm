/*
 * test_bz_quest_wc3_cache.c - coverage for bz_quest_wc3_cache.c's pure
 * keyed cache bookkeeping (layer 5A), using fake create/destroy callbacks
 * (no Vulkan/ABI dependency - see bz_quest_wc3_cache.h's header comment).
 * Each case covers a normal path and its inverse (hit vs miss, eviction vs
 * no-eviction, shutdown-with-entries vs shutdown-of-empty-cache).
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
}
