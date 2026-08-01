#!/bin/sh
# platform/android/quest/scripts/test-wc3-descriptor-pool-headroom.sh
#
# Structurally guards the layer-5A/5B texture-descriptor-pool headroom
# contract documented in bz_quest_vk_wc3.h's
# BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY comment and mirrored in
# bz_quest_vk_wc3_terrain.h's
# BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY comment:
#
#   bz_quest_wc3_cache_acquire() (bz_quest_wc3_cache.c) calls create()
#   BEFORE evicting the oldest entry on a miss at capacity, to stay
#   transactional (a failed create() must never destroy a still-good
#   cached entry). That means texture_create()'s vkAllocateDescriptorSets()
#   call needs a descriptor pool sized to ONE MORE live set than
#   BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY: at the instant the
#   (capacity+1)-th distinct texture is created, `capacity` older sets are
#   still live (eviction hasn't run yet), so a pool sized to exactly
#   `capacity` sets always fails that allocation - permanently, since a
#   failed create() never reaches the eviction step that would free a set.
#   This was a real bug (fixed in the same change that added this test):
#   the pool was originally sized to the bare capacity constant with no
#   spare slot, so the 129th distinct texture in a session would
#   permanently fail to upload and every subsequent distinct texture would
#   repeat the identical failure - a one-way cache deadlock.
#
# This script does NOT run the real Vulkan code (no NDK/device available
# here - see test_bz_quest_wc3_cache.c's
# test_acquire_deadlocks_when_ceiling_matches_capacity_with_no_spare_slot/
# test_acquire_recovers_when_ceiling_has_one_spare_slot for the
# pure-cache-level regression coverage of the *logic* this headroom
# requirement depends on). Instead it greps the real source so a future
# edit that reverts either descriptor-pool call site back to the bare
# BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY constant (silently reintroducing
# the deadlock) fails loudly here instead of only on-device, many uniquely
# textured models into a session.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../../../.." && pwd)
cd "$ROOT"

VK_WC3_H=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3.h
VK_WC3_C=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3.c
VK_WC3_TERRAIN_H=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3_terrain.h
VK_WC3_TERRAIN_C=platform/android/quest/app/src/main/cpp/bz_quest_vk_wc3_terrain.c
FAIL=0

if [ ! -f "$VK_WC3_H" ] || [ ! -f "$VK_WC3_C" ] || [ ! -f "$VK_WC3_TERRAIN_H" ] || [ ! -f "$VK_WC3_TERRAIN_C" ]; then
    echo "test-wc3-descriptor-pool-headroom: expected source files missing: $VK_WC3_H / $VK_WC3_C / $VK_WC3_TERRAIN_H / $VK_WC3_TERRAIN_C" >&2
    exit 1
fi

# 1. The +1-headroom constant itself must exist and literally be defined as
#    "<cache capacity> + 1", not some other expression that could silently
#    collapse back to the bare capacity.
if ! grep -q 'BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY = BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY + 1' "$VK_WC3_H"; then
    echo "test-wc3-descriptor-pool-headroom: $VK_WC3_H no longer defines BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY as BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY + 1" >&2
    FAIL=1
fi
if ! grep -q 'BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY =.*BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_CACHE_CAPACITY + 1' "$VK_WC3_TERRAIN_H"; then
    echo "test-wc3-descriptor-pool-headroom: $VK_WC3_TERRAIN_H no longer defines BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY as BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_CACHE_CAPACITY + 1" >&2
    FAIL=1
fi

# 2. Both real Vulkan descriptor-pool call sites (pool size AND maxSets)
#    must use the +1'd constant, never the bare cache-capacity constant -
#    check each line individually so a partial revert (only one of the two
#    fields reverted) is still caught.
if ! grep -q 'VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,' "$VK_WC3_C"; then
    echo "test-wc3-descriptor-pool-headroom: $VK_WC3_C's expected VkDescriptorPoolSize initializer line was not found (file restructured? update this test)" >&2
    FAIL=1
fi
if ! grep -A1 'VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,' "$VK_WC3_C" | grep -q 'BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY'; then
    echo "test-wc3-descriptor-pool-headroom: $VK_WC3_C's VkDescriptorPoolSize.descriptorCount does not use BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY (reverted to the bare capacity constant?)" >&2
    FAIL=1
fi
if ! grep -q 'poolInfo.maxSets = BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY;' "$VK_WC3_C"; then
    echo "test-wc3-descriptor-pool-headroom: $VK_WC3_C's poolInfo.maxSets does not use BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY (reverted to the bare capacity constant?)" >&2
    FAIL=1
fi
if ! grep -q 'VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,' "$VK_WC3_TERRAIN_C"; then
    echo "test-wc3-descriptor-pool-headroom: $VK_WC3_TERRAIN_C's expected VkDescriptorPoolSize initializer line was not found (file restructured? update this test)" >&2
    FAIL=1
fi
if ! grep -A1 'VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,' "$VK_WC3_TERRAIN_C" | grep -q 'BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY'; then
    echo "test-wc3-descriptor-pool-headroom: $VK_WC3_TERRAIN_C's VkDescriptorPoolSize.descriptorCount does not use BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY" >&2
    FAIL=1
fi
if ! grep -q 'poolInfo.maxSets = BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY;' "$VK_WC3_TERRAIN_C"; then
    echo "test-wc3-descriptor-pool-headroom: $VK_WC3_TERRAIN_C's poolInfo.maxSets does not use BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY" >&2
    FAIL=1
fi

# 3. Inverse/negative check: the bare BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY
#    constant must NOT appear anywhere in the descriptor-pool creation block
#    (lines between the VkDescriptorPoolSize initializer and the
#    vkCreateDescriptorPool call) - catches a future edit that adds it back
#    alongside the +1'd constant (e.g. a bad merge) rather than in place of.
POOL_BLOCK=$(awk '/VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,/,/vkCreateDescriptorPool/' "$VK_WC3_C")
case "$POOL_BLOCK" in
    *BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY*)
        echo "test-wc3-descriptor-pool-headroom: $VK_WC3_C's descriptor-pool creation block references the bare BZ_QUEST_VK_WC3_TEXTURE_CACHE_CAPACITY constant - it must only use BZ_QUEST_VK_WC3_TEXTURE_DESCRIPTOR_POOL_CAPACITY" >&2
        FAIL=1
        ;;
esac
POOL_BLOCK=$(awk '/VkDescriptorPoolSize poolSize = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,/,/vkCreateDescriptorPool/' "$VK_WC3_TERRAIN_C")
case "$POOL_BLOCK" in
    *BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_CACHE_CAPACITY*)
        echo "test-wc3-descriptor-pool-headroom: $VK_WC3_TERRAIN_C's descriptor-pool creation block references the bare BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_CACHE_CAPACITY constant - it must only use BZ_QUEST_VK_WC3_TERRAIN_TEXTURE_DESCRIPTOR_POOL_CAPACITY" >&2
        FAIL=1
        ;;
esac

if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
echo "test-wc3-descriptor-pool-headroom: OK (model and terrain texture descriptor pools keep their +1 create-before-evict spare slot)"
