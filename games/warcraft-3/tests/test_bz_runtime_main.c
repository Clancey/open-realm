/*
 * test_bz_runtime_main.c — standalone runner for the bz_runtime.c coverage
 * suite (see test_bz_runtime.c). Kept separate from test_openwarcraft3 so it
 * can link common/bz_runtime.c plus a minimal stubbed CL_/SV_ boundary
 * without pulling in the full game module or a unity build, mirroring
 * test_commands_main.c.
 */

#include <stdio.h>

#include "test_framework.h"

int _tests_run    = 0;
int _tests_failed = 0;

void run_bz_runtime_tests(void);

int main(void) {
    printf("=== OpenWarcraft3 Runtime Lifecycle Tests ===\n\n");

    printf("[bz_runtime init/frame/shutdown]\n");
    run_bz_runtime_tests();
    printf("\n");

    TEST_RESULTS();
}
