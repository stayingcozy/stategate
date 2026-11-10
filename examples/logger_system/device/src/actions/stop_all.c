// @action stop_all
// @node device.Recording (exit)

#include "gen/nodes.h"

void device_stop_all_run(device_safe_hal_t *hal) {
    device_send_stop(hal, NULL, 0);
}
