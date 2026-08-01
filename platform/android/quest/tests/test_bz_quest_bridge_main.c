/*
 * test_bz_quest_bridge_main.c - standalone runner for the bz_quest_bridge.c
 * coverage suite (see test_bz_quest_bridge.c). Mirrors games/warcraft-3/
 * tests/test_bz_tabletop_lifecycle_main.c's pattern: a small binary that
 * links the REAL platform/tabletop/bridge/bz_tabletop_lifecycle.c plus the
 * lightweight common/bz_runtime.c stub set - no NDK/Android SDK/OpenXR
 * loader/Vulkan headers needed (bz_quest_bridge.c has no such dependency,
 * see its header comment).
 */
#include <stdio.h>

#include "test_framework.h"

int _tests_run = 0;
int _tests_failed = 0;

void run_bz_quest_bridge_tests(void);

int main(void) {
    printf("=== OpenRealm Quest Tabletop Bridge Tests ===\n\n");

    printf("[bz_quest_bridge]\n");
    run_bz_quest_bridge_tests();
    printf("\n");

    TEST_RESULTS();
}
