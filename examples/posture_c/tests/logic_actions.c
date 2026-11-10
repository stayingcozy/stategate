#include "unity.h"

#include "gen/fake.h"
#include "gen/hal.h"
#include "gen/nodes.h"

static posture_probe_t probe;
static posture_safe_hal_t hal;

void setUp(void) {
    hal = posture_safe_hal(posture_fake_hal(&probe));
}

void tearDown(void) {}

static void stim_off_sets_intensity_to_zero(void) {
    int64_t last = -1;
    posture_stim_off_run(&hal);
    TEST_ASSERT_TRUE(posture_probe_last(&probe, "stim_set_intensity", &last));
    TEST_ASSERT_EQUAL_INT64(0, last);
}

static void imu_stub_reports_one_slouch_at_10_s(void) {
    posture_event_t e;
    probe.now_ms = 9990;
    TEST_ASSERT_FALSE(posture_imu_posture_poll(&hal, &e));
    probe.now_ms = 10000;
    TEST_ASSERT_TRUE(posture_imu_posture_poll(&hal, &e));
    TEST_ASSERT_EQUAL_INT(POSTURE_EV_SLOUCH, e);
    TEST_ASSERT_FALSE(posture_imu_posture_poll(&hal, &e));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(stim_off_sets_intensity_to_zero);
    RUN_TEST(imu_stub_reports_one_slouch_at_10_s);
    return UNITY_END();
}
