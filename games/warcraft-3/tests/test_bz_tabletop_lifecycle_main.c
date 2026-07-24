/*
 * test_bz_tabletop_lifecycle_main.c — standalone runner for the
 * bz_tabletop_lifecycle.c coverage suite (see test_bz_tabletop_lifecycle.c).
 * Kept separate from test_openwarcraft3 so it can link
 * platform/apple/visionos/tabletop/bridge/bz_tabletop_lifecycle.c plus
 * common/bz_runtime.c and a minimal stubbed CL_/SV_ boundary without
 * pulling in the full game module or a unity build, mirroring
 * test_bz_runtime_main.c.
 */

#include <stdio.h>

#include "test_framework.h"

int _tests_run    = 0;
int _tests_failed = 0;

void run_bz_tabletop_lifecycle_tests(void);

int main(void) {
    printf("=== OpenWarcraft3 Tabletop Lifecycle Bridge Tests ===\n\n");

    printf("[bz_tabletop_lifecycle state machine]\n");
    run_bz_tabletop_lifecycle_tests();
    printf("\n");

    TEST_RESULTS();
}
