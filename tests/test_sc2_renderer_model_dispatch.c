#include <string.h>

#include "games/starcraft-2/renderer/r_game_utils.h"
#include "test_framework.h"

int _tests_run = 0, _tests_failed = 0;

static void test_short_buffers_and_valid_magic(void) {
    BYTE data[4] = { 0 };
    ASSERT(!r_sc2_model_is_m3(NULL, 0));
    FOR_LOOP(size, 4) ASSERT(!r_sc2_model_is_m3(data, size));
    memcpy(data, &(DWORD){ ID_43DM }, sizeof(DWORD));
    ASSERT(r_sc2_model_is_m3(data, sizeof(data)));
    memcpy(data, &(DWORD){ 0 }, sizeof(DWORD));
    ASSERT(!r_sc2_model_is_m3(data, sizeof(data)));
}

int main(void) {
    RUN_TEST(test_short_buffers_and_valid_magic);
    TEST_RESULTS();
}
