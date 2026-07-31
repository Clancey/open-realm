#include <stdint.h>

#include "client/cl_model_lifecycle.h"
#include "test_framework.h"

int _tests_run = 0, _tests_failed = 0;
static LPMODEL g_released[16];
static uint32_t g_release_count;

static void release_model(LPMODEL model) {
    FOR_LOOP(i, g_release_count) ASSERT(g_released[i] != model);
    g_released[g_release_count++] = model;
}

static LPMODEL fake_model(uintptr_t value) { return (LPMODEL)value; }

static void test_map_transition_then_final_shutdown(void) {
    LPMODEL models[3] = { fake_model(1), NULL, fake_model(2) };
    LPMODEL portraits[3] = { NULL, fake_model(3), fake_model(4) };
    LPMODEL move_confirmation = fake_model(8);
    clModelHandles_t handles = {
        .models = models, .portraits = portraits, .move_confirmation = &move_confirmation,
        .count = 3, .release_model = release_model,
    };
    g_release_count = 0;
    CL_ReleaseModelHandles(&handles);
    ASSERT_EQ_INT(g_release_count, 5);
    FOR_LOOP(i, 3) { ASSERT_NULL(models[i]); ASSERT_NULL(portraits[i]); }
    ASSERT_NULL(move_confirmation);
    CL_ReleaseModelHandles(&handles);
    ASSERT_EQ_INT(g_release_count, 5);
}

static void test_final_shutdown_without_transition(void) {
    LPMODEL models[2] = { fake_model(5), fake_model(6) };
    LPMODEL portraits[2] = { fake_model(7), NULL };
    LPMODEL move_confirmation = fake_model(9);
    clModelHandles_t handles = {
        .models = models, .portraits = portraits, .move_confirmation = &move_confirmation,
        .count = 2, .release_model = release_model,
    };
    g_release_count = 0;
    CL_ReleaseModelHandles(&handles);
    ASSERT_EQ_INT(g_release_count, 4);
    FOR_LOOP(i, 2) { ASSERT_NULL(models[i]); ASSERT_NULL(portraits[i]); }
    ASSERT_NULL(move_confirmation);
}

static void test_duplicate_handles_release_once(void) {
    LPMODEL models[2] = { fake_model(10), fake_model(11) };
    LPMODEL portraits[2] = { fake_model(10), fake_model(12) };
    LPMODEL move_confirmation = fake_model(11);
    clModelHandles_t handles = {
        .models = models, .portraits = portraits, .move_confirmation = &move_confirmation,
        .count = 2, .release_model = release_model,
    };
    g_release_count = 0;
    CL_ReleaseModelHandles(&handles);
    ASSERT_EQ_INT(g_release_count, 3);
    FOR_LOOP(i, 2) { ASSERT_NULL(models[i]); ASSERT_NULL(portraits[i]); }
    ASSERT_NULL(move_confirmation);
}

int main(void) {
    RUN_TEST(test_map_transition_then_final_shutdown);
    RUN_TEST(test_final_shutdown_without_transition);
    RUN_TEST(test_duplicate_handles_release_once);
    TEST_RESULTS();
}
