#include <stdio.h>

#include "test_framework.h"

int _tests_run;
int _tests_failed;

void run_world_override_tests(void);

int main(void) {
    printf("=== OpenWarcraft3 Object Override Tests ===\n\n");
    run_world_override_tests();
    TEST_RESULTS();
}
