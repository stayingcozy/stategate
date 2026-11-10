// @service imu_posture
// @node posture.Monitoring (invoke)
// @node posture.Slouching (invoke)
// @node posture.Nudging (invoke)
// @stub Step 0: report a slouch at t = 10 s

#include "gen/nodes.h"

#define SLOUCH_AT_MS 10000

static bool reported;

bool posture_imu_posture_poll(posture_safe_hal_t *hal, posture_event_t *event) {
    if (!reported && posture_hal_now_ms(hal) >= SLOUCH_AT_MS) {
        reported = true;
        *event = POSTURE_EV_SLOUCH;
        return true;
    }
    return false;
}
