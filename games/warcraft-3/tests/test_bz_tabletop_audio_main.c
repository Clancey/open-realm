#include "test_framework.h"

int _tests_run;
int _tests_failed;

void run_bz_tabletop_audio_tests(void);

int main(void) {
    printf("=== OpenWarcraft3 Tabletop Audio Tests ===\n\n");
    run_bz_tabletop_audio_tests();
    TEST_RESULTS();
}
