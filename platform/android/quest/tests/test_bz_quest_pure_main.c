/*
 * test_bz_quest_pure_main.c - standalone runner for the Quest pure
 * math/selection helper coverage suite (see test_bz_quest_pure.c). Mirrors
 * games/warcraft-3/tests/test_bz_tabletop_lifecycle_main.c's pattern: a
 * small, dependency-free binary built with the host compiler (no NDK/
 * Android SDK/OpenXR loader/Vulkan headers needed - see bz_quest_pure.h's
 * top comment for why that's possible for this particular module).
 */
#include <stdio.h>

#include "test_framework.h"

int _tests_run = 0;
int _tests_failed = 0;

void run_bz_quest_pure_tests(void);
void run_bz_quest_scene_tests(void);
void run_bz_quest_data_tests(void);
void run_bz_quest_frame_tests(void);

int main(void) {
    printf("=== OpenRealm Quest Pure Helper Tests ===\n\n");

    printf("[bz_quest_pure]\n");
    run_bz_quest_pure_tests();
    printf("\n");

    printf("[bz_quest_scene]\n");
    run_bz_quest_scene_tests();
    printf("\n");

    printf("[bz_quest_data]\n");
    run_bz_quest_data_tests();
    printf("\n");

    printf("[bz_quest_frame]\n");
    run_bz_quest_frame_tests();
    printf("\n");

    TEST_RESULTS();
}
