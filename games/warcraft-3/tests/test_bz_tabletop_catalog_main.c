#include <stdio.h>

#include "test_framework.h"

int _tests_run;
int _tests_failed;

void run_bz_tabletop_catalog_tests(void);

int main(void) {
    printf("=== OpenWarcraft3 Tabletop Catalog Tests ===\n\n");
    run_bz_tabletop_catalog_tests();
    TEST_RESULTS();
}
