/*
 * bz_quest_wc3_cache.c - see bz_quest_wc3_cache.h.
 */
#include "bz_quest_wc3_cache.h"

#include <string.h>

static bool bz_quest_wc3_cache_key_equal(const bzQuestWc3CacheKey_t *a, const bzQuestWc3CacheKey_t *b) {
    return a->variant == b->variant && bz_quest_wc3_identity_equal(a->identity, b->identity);
}

bool bz_quest_wc3_cache_init(bzQuestWc3Cache_t *cache, uint32_t capacity,
                             bzQuestWc3CacheCreateFn create, bzQuestWc3CacheDestroyFn destroy,
                             void *userdata) {
    if (capacity == 0 || capacity > BZ_QUEST_WC3_CACHE_CAPACITY || !create || !destroy) return false;
    memset(cache, 0, sizeof(*cache));
    cache->capacity = capacity;
    cache->create = create;
    cache->destroy = destroy;
    cache->userdata = userdata;
    return true;
}

/* Finds the occupied slot with the smallest insertionOrder[] value (i.e.
 * the least-recently-inserted entry still present) - see
 * bzQuestWc3Cache_t's FIFO-eviction comment in the header. */
static uint32_t bz_quest_wc3_cache_find_oldest(const bzQuestWc3Cache_t *cache) {
    uint32_t oldest = 0;
    uint64_t oldestOrder = UINT64_MAX;
    for (uint32_t i = 0; i < cache->capacity; i++) {
        if (!cache->slots[i].occupied) continue;
        if (cache->insertionOrder[i] < oldestOrder) {
            oldestOrder = cache->insertionOrder[i];
            oldest = i;
        }
    }
    return oldest;
}

static int32_t bz_quest_wc3_cache_find(const bzQuestWc3Cache_t *cache, const bzQuestWc3CacheKey_t *key) {
    for (uint32_t i = 0; i < cache->capacity; i++) {
        if (cache->slots[i].occupied && bz_quest_wc3_cache_key_equal(&cache->slots[i].key, key))
            return (int32_t)i;
    }
    return -1;
}

static int32_t bz_quest_wc3_cache_find_free(const bzQuestWc3Cache_t *cache) {
    for (uint32_t i = 0; i < cache->capacity; i++) {
        if (!cache->slots[i].occupied) return (int32_t)i;
    }
    return -1;
}

bool bz_quest_wc3_cache_acquire(bzQuestWc3Cache_t *cache, const bzQuestWc3CacheKey_t *key,
                                void **outHandle) {
    int32_t existing = bz_quest_wc3_cache_find(cache, key);
    if (existing >= 0) {
        cache->hits++;
        *outHandle = cache->slots[existing].handle;
        return true;
    }

    void *handle = cache->create(key, cache->userdata);
    if (!handle) {
        cache->createFailures++;
        return false;
    }
    cache->misses++;

    int32_t slot = bz_quest_wc3_cache_find_free(cache);
    if (slot < 0) {
        /* At capacity: evict the oldest entry to make room - see
         * bzQuestWc3CacheDestroyFn's "exactly once" contract. */
        slot = (int32_t)bz_quest_wc3_cache_find_oldest(cache);
        cache->destroy(cache->slots[slot].handle, cache->userdata);
        cache->slots[slot].occupied = false;
        cache->occupiedCount--;
        cache->evictions++;
    }

    cache->slots[slot].key = *key;
    cache->slots[slot].handle = handle;
    cache->slots[slot].occupied = true;
    cache->insertionOrder[slot] = cache->insertionCounter++;
    cache->occupiedCount++;
    *outHandle = handle;
    return true;
}

void bz_quest_wc3_cache_shutdown(bzQuestWc3Cache_t *cache) {
    for (uint32_t i = 0; i < cache->capacity; i++) {
        if (!cache->slots[i].occupied) continue;
        cache->destroy(cache->slots[i].handle, cache->userdata);
        cache->slots[i].occupied = false;
    }
    cache->occupiedCount = 0;
}
