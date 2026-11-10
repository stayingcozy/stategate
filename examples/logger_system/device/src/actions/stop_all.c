// @action stop_all
// @node device.Recording (exit)
// @stub send STOP to the sampler and the logger

#include "gen/nodes.h"

void device_stop_all_run(device_safe_hal_t *hal) {
    device_hal_log(hal, "stop_all (stub)");
}
