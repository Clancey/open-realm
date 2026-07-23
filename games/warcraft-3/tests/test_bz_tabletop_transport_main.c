/*
 * test_bz_tabletop_transport_main.c — standalone runner for the
 * bz_tabletop_transport.c coverage suite: the ABI-level suite
 * (test_bz_tabletop_transport.c) plus the real-client-integration suite
 * (test_bz_tabletop_transport_client.c, real client/cl_parse.c packet
 * parsing and real common/net.c loopback command delivery).
 */
#include <stdio.h>

#include "test_framework.h"

int _tests_run    = 0;
int _tests_failed = 0;

void run_bz_tabletop_transport_tests(void);
void run_bz_tabletop_transport_client_tests(void);

int main(void) {
    printf("=== OpenWarcraft3 Tabletop Transport (Layer 2) Tests ===\n\n");

    printf("[bz_tabletop_transport ABI]\n");
    run_bz_tabletop_transport_tests();
    printf("\n");

    printf("[bz_tabletop_transport real client integration]\n");
    run_bz_tabletop_transport_client_tests();
    printf("\n");

    TEST_RESULTS();
}
