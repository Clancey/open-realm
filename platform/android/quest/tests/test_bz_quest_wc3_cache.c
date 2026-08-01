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
    FakeResource_t resources[16];
    int nextId;
    int createCalls;
    int destroyCalls;
    int lastDestroyedId;
    bool failNextCreate;
} FakeUserdata_t;

static void *fake_create(const bzQuestWc3CacheKey_t *key, void *userdata) {
    (void)key;
    FakeUserdata_t *u = (FakeUserdata_t *)userdata;
    u->createCalls++;
    if (u->failNextCreate) {
        u->failNextCreate = false;
        return NULL;
    }
    FakeResource_t *r = &u->resources[u->nextId];
    r->id = u->nextId;
    r->destroyed = false;
    u->nextId++;
    return r;
}

static void fake_destroy(void *handle, void *userdata) {
    FakeUserdata_t *u = (FakeUserdata_t *)userdata;
    FakeResource_t *r = (FakeResource_t *)handle;
    r->destroyed = true;
    u->destroyCalls++;
    u->lastDestroyedId = r->id;
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
    RUN_TEST(test_shutdown_destroys_every_entry);
    RUN_TEST(test_shutdown_of_empty_cache_calls_nothing);
    RUN_TEST(test_shutdown_of_zero_initialized_cache_is_safe);
}
